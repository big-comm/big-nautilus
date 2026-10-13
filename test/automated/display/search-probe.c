/* search-probe.c: types a search into a real window and measures what the user
 * feels -- how long the main loop goes without turning -- against the real
 * folder, the real settings that matter and the search daemon of the session.
 *
 * Not a test: it has no opinion on the outcome, and what it measures depends
 * on the machine. Run by hand:
 *
 *   builddir/test/automated/display/search-probe [--hidden] [--no-daemon]
 *       [--workspace N] [--dir DIR] [--interval MS] [--settle MS] TEXT...
 *
 * Each TEXT is typed one character at a time, then erased the same way.
 *
 * To keep its window off the desktop, run it on a compositor with no screen:
 *
 *   dbus-run-session -- sh -c 'mutter --headless --wayland --no-x11 \
 *       --virtual-monitor 1600x900 --wayland-display wayland-probe & sleep 1;
 *       WAYLAND_DISPLAY=wayland-probe GSETTINGS_SCHEMA_DIR=builddir/data \
 *       builddir/test/automated/display/search-probe --hidden nautilus'
 */

#include <nautilus-application.h>
#include <nautilus-file.h>
#include <nautilus-file-utilities.h>
#include <nautilus-files-view.h>
#include <nautilus-global-preferences.h>
#include <nautilus-query.h>
#include <nautilus-resources.h>
#include <nautilus-tag-manager.h>
#include <nautilus-view-item.h>
#include <nautilus-view-model.h>
#include <nautilus-window-slot.h>

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#define TICK_MS 2
/* Two frames at 60 Hz: what a person starts to see as a hitch. */
#define STALL_MS 33

static gint64 last_tick;
static gint64 worst_stall;
static gint64 step_worst;
static gint64 stalled_total;
static guint n_stalls;
static gint64 started;
static gint watchdog_fired;
static GArray *key_freezes;
static gboolean want_backtrace;

/* A sampling profiler for the main thread, for a machine with none installed:
 * a timer on the CPU time of this thread alone raises a signal every
 * millisecond the thread is busy, and the handler records where it was. */
#define PROFILE_DEPTH 56
#define PROFILE_MAX_SAMPLES 60000

static void *(*profile_samples)[PROFILE_DEPTH];
static int *profile_depths;
static volatile int n_profile_samples;
static gboolean want_profile;
/* Which part of the run is profiled: all of it, or only the erasing. */
static gboolean profile_erasing_only;
static volatile gboolean profile_recording = TRUE;

static void
on_profile_signal (int signal_number)
{
    int n = n_profile_samples;

    if (profile_recording && n < PROFILE_MAX_SAMPLES)
    {
        profile_depths[n] = backtrace (profile_samples[n], PROFILE_DEPTH);
        n_profile_samples = n + 1;
    }
}

static void
profile_start (void)
{
    struct sigaction action = { .sa_handler = on_profile_signal, .sa_flags = SA_RESTART };
    struct sigevent event = { 0 };
    struct itimerspec interval = { { 0, 1000000 }, { 0, 1000000 } };
    clockid_t clock;
    timer_t timer;
    void *warm_up[4];

    profile_samples = g_malloc0 (sizeof (*profile_samples) * PROFILE_MAX_SAMPLES);
    profile_depths = g_new0 (int, PROFILE_MAX_SAMPLES);
    /* Loads what backtrace() needs, outside of the handler. */
    backtrace (warm_up, G_N_ELEMENTS (warm_up));

    sigaction (SIGPROF, &action, NULL);
    pthread_getcpuclockid (pthread_self (), &clock);
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGPROF;
    event._sigev_un._tid = (pid_t) syscall (SYS_gettid);
    timer_create (clock, &event, &timer);
    timer_settime (timer, 0, &interval, NULL);
}

static char *
profile_name (void       *address,
              GHashTable *names)
{
    char *name = g_hash_table_lookup (names, address);
    Dl_info info;

    if (name != NULL)
    {
        return name;
    }

    if (dladdr (address, &info) && info.dli_fname != NULL)
    {
        if (strstr (info.dli_fname, "search-probe") != NULL)
        {
            /* Our own functions are mostly static: ask the debug info. */
            g_autofree char *command = g_strdup_printf ("addr2line -f -e %s %p", info.dli_fname,
                                                        (void *) ((char *) address - (char *) info.dli_fbase));
            g_autofree char *output = NULL;

            if (g_spawn_command_line_sync (command, &output, NULL, NULL, NULL) && output != NULL)
            {
                name = g_strdup (g_strstrip (g_strsplit (output, "\n", 2)[0]));
            }
        }
        else if (info.dli_sname != NULL)
        {
            name = g_strdup (info.dli_sname);
        }
        else
        {
            g_autofree char *base = g_path_get_basename (info.dli_fname);

            name = g_strdup_printf ("(%s)", base);
        }
    }

    if (name == NULL)
    {
        name = g_strdup ("?");
    }

    g_hash_table_insert (names, address, name);

    return name;
}

static int
compare_counts (gconstpointer a,
                gconstpointer b,
                gpointer      counts)
{
    return GPOINTER_TO_INT (g_hash_table_lookup (counts, *(char * const *) b)) -
           GPOINTER_TO_INT (g_hash_table_lookup (counts, *(char * const *) a));
}

static void
profile_report (void)
{
    g_autoptr (GHashTable) names = g_hash_table_new (NULL, NULL);
    g_autoptr (GHashTable) inclusive = g_hash_table_new (g_str_hash, g_str_equal);
    g_autoptr (GPtrArray) order = NULL;
    int n = n_profile_samples;

    for (int i = 0; i < n; i++)
    {
        g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);

        /* The first frames are the handler and the kernel's way into it. */
        for (int j = 3; j < profile_depths[i]; j++)
        {
            char *name = profile_name (profile_samples[i][j], names);

            if (g_hash_table_add (seen, name))
            {
                g_hash_table_insert (inclusive, name,
                                     GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (inclusive, name)) + 1));
            }
        }
    }

    order = g_hash_table_get_keys_as_ptr_array (inclusive);
    g_ptr_array_sort_with_data (order, compare_counts, inclusive);

    g_print ("\nmain thread busy for %d ms; share of that spent in or below:\n", n);
    for (guint i = 0; i < order->len && i < 70; i++)
    {
        const char *name = g_ptr_array_index (order, i);
        int count = GPOINTER_TO_INT (g_hash_table_lookup (inclusive, name));

        g_print ("  %5.1f%%  %s\n", 100.0 * count / MAX (n, 1), name);
    }
}

static int
compare_freezes (gconstpointer a,
                 gconstpointer b)
{
    gint64 x = *(const gint64 *) a;
    gint64 y = *(const gint64 *) b;

    return (x > y) - (x < y);
}

static double
now_s (void)
{
    return (g_get_monotonic_time () - started) / 1e6;
}

static gboolean
on_tick (gpointer user_data)
{
    gint64 now = g_get_monotonic_time ();
    gint64 gap = (now - g_atomic_int_get (&watchdog_fired) * 0 - last_tick) / 1000;

    if (gap >= STALL_MS)
    {
        n_stalls++;
        stalled_total += gap;
    }

    worst_stall = MAX (worst_stall, gap);
    step_worst = MAX (step_worst, gap);
    last_tick = now;

    return G_SOURCE_CONTINUE;
}

/* Looks at the main loop from outside it. The first time it has been stuck for
 * a while, asks gdb where. */
static gpointer
watchdog (gpointer user_data)
{
    while (TRUE)
    {
        gint64 stuck = (g_get_monotonic_time () - last_tick) / 1000;

        if (want_backtrace && stuck > 700 &&
            g_atomic_int_compare_and_exchange (&watchdog_fired, 0, 1))
        {
            g_autofree char *command = g_strdup_printf (
                "gdb -q -batch -iex 'set debuginfod enabled off' -p %d "
                "-ex 'thread 1' -ex 'bt 45' 2>/dev/null | grep -E '^#' | cut -c1-170 >&2",
                (int) getpid ());

            g_printerr ("  --- stuck for %" G_GINT64_FORMAT " ms, main thread is at:\n", stuck);
            if (system (command) != 0)
            {
                g_printerr ("  (no backtrace)\n");
            }
        }

        g_usleep (50 * 1000);
    }

    return NULL;
}

static void
spin (guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time () + milliseconds * G_TIME_SPAN_MILLISECOND;

    while (g_get_monotonic_time () < deadline)
    {
        g_main_context_iteration (NULL, TRUE);
    }
}

static guint
n_items (NautilusWindowSlot *slot)
{
    NautilusFilesView *view = nautilus_window_slot_get_current_view (slot);

    if (view == NULL)
    {
        return 0;
    }

    return g_list_model_get_n_items (G_LIST_MODEL (nautilus_files_view_get_private_model (view)));
}

/* How many cells the view built for what it shows: the part of a search that
 * costs, widget by widget. */
static guint
n_cells (GtkWidget *widget)
{
    guint n = g_str_has_suffix (G_OBJECT_TYPE_NAME (widget), "Cell") &&
              g_str_has_prefix (G_OBJECT_TYPE_NAME (widget), "Nautilus") ? 1 : 0;

    for (GtkWidget *child = gtk_widget_get_first_child (widget);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        n += n_cells (child);
    }

    return n;
}

/* How many cells sit in the first row: the columns of a grid. */
static void
count_columns (GtkWidget *widget,
               GtkWidget *reference,
               double    *first_y,
               guint     *n_columns)
{
    if (g_str_equal (G_OBJECT_TYPE_NAME (widget), "NautilusGridCell") &&
        gtk_widget_get_mapped (widget) && gtk_widget_get_height (widget) > 0)
    {
        graphene_point_t origin;

        if (gtk_widget_compute_point (widget, reference, &GRAPHENE_POINT_INIT (0, 0), &origin))
        {
            if (*n_columns == 0 || origin.y < *first_y - 1)
            {
                *first_y = origin.y;
                *n_columns = 1;
            }
            else if (ABS (origin.y - *first_y) <= 1)
            {
                (*n_columns)++;
            }
        }
    }

    for (GtkWidget *child = gtk_widget_get_first_child (widget);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        count_columns (child, reference, first_y, n_columns);
    }
}

/* Whether the window is being drawn at all, and what a frame costs: a window
 * the compositor does not show is not laid out or painted, and typing into it
 * measures less than the user would feel. */
static guint n_frames;
static gint64 frame_started;
static gint64 frames_total;
static gint64 frame_worst;

static void
on_before_paint (GdkFrameClock *clock)
{
    frame_started = g_get_monotonic_time ();
}

static void
on_after_paint (GdkFrameClock *clock)
{
    gint64 took = (g_get_monotonic_time () - frame_started) / 1000;

    if (started == 0)
    {
        /* Still opening the window. */
        return;
    }

    if (took >= STALL_MS)
    {
        g_print ("  [%7.3f] a frame took %" G_GINT64_FORMAT " ms\n", now_s (), took);
    }

    n_frames++;
    frames_total += took;
    frame_worst = MAX (frame_worst, took);
}

static void
type_text (NautilusWindowSlot *slot,
           GFile              *location,
           const char         *text,
           gboolean            show_hidden,
           guint               interval)
{
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    gint64 before = g_get_monotonic_time ();
    gint64 after;

    nautilus_query_set_text (query, text);
    nautilus_query_set_location (query, location);
    nautilus_query_set_show_hidden_files (query, show_hidden);
    nautilus_window_slot_search (slot, query);
    after = g_get_monotonic_time ();

    step_worst = 0;
    spin (interval);
    g_array_append_val (key_freezes, step_worst);
    g_print ("[%7.3f] typed \"%s\": the call took %" G_GINT64_FORMAT " ms, longest freeze until the next key %" G_GINT64_FORMAT " ms\n",
             now_s (), text, (after - before) / 1000, step_worst);
}

/* Waits for the search to say it is done, or for @timeout. */
static void
settle (NautilusWindowSlot *slot,
        const char         *text,
        guint               timeout)
{
    gint64 begin = g_get_monotonic_time ();
    gint64 deadline = begin + timeout * G_TIME_SPAN_MILLISECOND;
    gint64 first_items = 0;

    while (g_get_monotonic_time () < deadline)
    {
        g_main_context_iteration (NULL, TRUE);

        if (first_items == 0 && n_items (slot) > 0)
        {
            first_items = g_get_monotonic_time ();
        }

        if (!nautilus_window_slot_get_allow_stop (slot) &&
            g_get_monotonic_time () - begin > 300 * G_TIME_SPAN_MILLISECOND)
        {
            break;
        }
    }

    {
        double first_y = 0;
        guint n_columns = 0;

        count_columns (GTK_WIDGET (slot), GTK_WIDGET (slot), &first_y, &n_columns);
        g_print ("[%7.3f] %u cells built for the view, %u columns\n", now_s (),
                 n_cells (GTK_WIDGET (slot)), n_columns);
    }
    g_print ("[%7.3f] \"%s\": %u items, first after %" G_GINT64_FORMAT " ms, %s after %" G_GINT64_FORMAT " ms\n",
             now_s (), text, n_items (slot),
             first_items != 0 ? (first_items - begin) / 1000 : -1,
             nautilus_window_slot_get_allow_stop (slot) ? "STILL SEARCHING" : "done",
             (g_get_monotonic_time () - begin) / 1000);
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (NautilusApplication) app = NULL;
    g_autoptr (NautilusTagManager) tag_manager = NULL;
    g_autoptr (GSettings) chooser_settings = NULL;
    g_autoptr (GFile) location = NULL;
    g_autofree char *scratch = g_dir_make_tmp ("nautilus-search-probe-XXXXXX", NULL);
    g_autofree char *config_dir = g_build_filename (scratch, "config", NULL);
    g_autofree char *data_dir = g_build_filename (scratch, "data", NULL);
    g_autofree char *cache_dir = g_build_filename (scratch, "cache", NULL);
    g_autofree char *dir = g_strdup (g_get_home_dir ());
    gboolean show_hidden = FALSE;
    guint interval = 150;
    int workspace = -1;
    int width = 900;
    gboolean list_view = FALSE;
    gboolean exercise = FALSE;
    const char *screenshot = NULL;
    const char *dump = NULL;
    guint settle_ms = 20000;
    NautilusWindowSlot *slot;
    GtkWindow *window;
    int first_text = argc;

    for (int i = 1; i < argc; i++)
    {
        if (g_str_equal (argv[i], "--hidden"))
        {
            show_hidden = TRUE;
        }
        else if (g_str_equal (argv[i], "--no-daemon"))
        {
            const char *display = g_getenv ("WAYLAND_DISPLAY");

            /* The display is looked up in the same directory as the daemon. */
            if (display != NULL && !g_path_is_absolute (display))
            {
                g_autofree char *path = g_build_filename (g_getenv ("XDG_RUNTIME_DIR"), display, NULL);

                g_setenv ("WAYLAND_DISPLAY", path, TRUE);
            }

            /* Nothing listens in here. */
            g_setenv ("XDG_RUNTIME_DIR", scratch, TRUE);
        }
        else if (g_str_equal (argv[i], "--workspace") && i + 1 < argc)
        {
            /* Counted from 1, as people do. Only X11 lets a client say where
             * its window goes, so that is what the probe then runs on.
             *
             * Good for watching, not for measuring: a compositor does not
             * draw the windows of a workspace nobody is looking at, so no
             * layout and no painting happen there, and the freezes come out
             * shorter than they are. The "frames drawn" line tells. */
            workspace = atoi (argv[++i]) - 1;
            g_setenv ("GDK_BACKEND", "x11", TRUE);
        }
        else if (g_str_equal (argv[i], "--dump") && i + 1 < argc)
        {
            dump = argv[++i];
        }
        else if (g_str_equal (argv[i], "--screenshot") && i + 1 < argc)
        {
            screenshot = argv[++i];
        }
        else if (g_str_equal (argv[i], "--exercise"))
        {
            exercise = TRUE;
        }
        else if (g_str_equal (argv[i], "--list"))
        {
            list_view = TRUE;
        }
        else if (g_str_equal (argv[i], "--width") && i + 1 < argc)
        {
            width = atoi (argv[++i]);
        }
        else if (g_str_equal (argv[i], "--profile"))
        {
            want_profile = TRUE;
        }
        else if (g_str_equal (argv[i], "--profile-erasing"))
        {
            want_profile = TRUE;
            profile_erasing_only = TRUE;
            profile_recording = FALSE;
        }
        else if (g_str_equal (argv[i], "--backtrace"))
        {
            want_backtrace = TRUE;
        }
        else if (g_str_equal (argv[i], "--dir") && i + 1 < argc)
        {
            g_set_str (&dir, argv[++i]);
        }
        else if (g_str_equal (argv[i], "--interval") && i + 1 < argc)
        {
            interval = atoi (argv[++i]);
        }
        else if (g_str_equal (argv[i], "--settle") && i + 1 < argc)
        {
            settle_ms = atoi (argv[++i]);
        }
        else
        {
            first_text = i;
            break;
        }
    }

    if (first_text >= argc)
    {
        g_printerr ("Usage: %s [--hidden] [--list] [--exercise] [--screenshot FILE] [--dump FILE] [--no-daemon] [--backtrace] [--profile] [--workspace N] [--width W] [--dir DIR] "
                    "[--interval MS] [--settle MS] TEXT...\n", argv[0]);
        return 2;
    }

    /* The folder searched is the real one. What the probe writes -- settings,
     * tags, thumbnails -- stays out of the user's own. */
    g_mkdir_with_parents (config_dir, 0700);
    g_mkdir_with_parents (data_dir, 0700);
    g_mkdir_with_parents (cache_dir, 0700);
    g_setenv ("XDG_CONFIG_HOME", config_dir, TRUE);
    g_setenv ("XDG_DATA_HOME", data_dir, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache_dir, TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

    nautilus_register_resource ();
    nautilus_ensure_extension_points ();
    tag_manager = nautilus_tag_manager_new_dummy ();
    app = nautilus_application_new ();
    g_application_set_flags (G_APPLICATION (app),
                             g_application_get_flags (G_APPLICATION (app)) |
                             G_APPLICATION_NON_UNIQUE);
    g_assert_true (g_application_register (G_APPLICATION (app), NULL, NULL));
    adw_init ();
    nautilus_global_preferences_init ();

    g_settings_set_string (nautilus_preferences, NAUTILUS_PREFERENCES_DEFAULT_FOLDER_VIEWER,
                           list_view ? "list-view" : "icon-view");

    chooser_settings = g_settings_new ("org.gtk.gtk4.Settings.FileChooser");
    g_settings_set_boolean (chooser_settings, "show-hidden", show_hidden);

    location = g_file_new_for_path (dir);
    window = GTK_WINDOW (gtk_window_new ());
    gtk_window_set_default_size (window, width, 600);
    slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);
    gtk_window_set_child (window, GTK_WIDGET (slot));

#ifdef GDK_WINDOWING_X11
    if (workspace >= 0)
    {
        GdkSurface *surface;

        /* Before it is mapped, so that it never shows where the user is. */
        gtk_widget_realize (GTK_WIDGET (window));
        surface = gtk_native_get_surface (GTK_NATIVE (window));
        if (GDK_IS_X11_SURFACE (surface))
        {
            gdk_x11_surface_move_to_desktop (surface, workspace);
        }
        else
        {
            g_printerr ("Not on X11: the window opens on the current workspace\n");
        }
    }
#endif

    gtk_window_present (window);

#ifdef GDK_WINDOWING_X11
    if (workspace >= 0)
    {
        GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));

        /* Again once it is on its way to the screen: the window manager goes
         * by a request at this point, not by what was set on the window. */
        if (GDK_IS_X11_SURFACE (surface))
        {
            gdk_x11_surface_move_to_desktop (surface, workspace);
            gdk_display_flush (gdk_display_get_default ());
        }
    }
#endif

    nautilus_window_slot_open_location_full (slot, location, NULL);

    {
        GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (window));

        g_signal_connect (clock, "before-paint", G_CALLBACK (on_before_paint), NULL);
        g_signal_connect_after (clock, "after-paint", G_CALLBACK (on_after_paint), NULL);
    }

    key_freezes = g_array_new (FALSE, FALSE, sizeof (gint64));
    last_tick = g_get_monotonic_time ();
    /* Long enough for the folder to load and the view to settle, as it has
     * by the time anyone starts typing. */
    spin (3000);
    started = g_get_monotonic_time ();

    g_print ("searching in %s, hidden files %s, daemon %s\n", dir,
             show_hidden ? "shown" : "not shown",
             g_str_equal (g_get_user_runtime_dir (), scratch) ? "out of reach" : "of the session");

    last_tick = g_get_monotonic_time ();
    worst_stall = 0;
    g_timeout_add (TICK_MS, on_tick, NULL);
    if (want_profile)
    {
        profile_start ();
    }
    g_thread_unref (g_thread_new ("watchdog", watchdog, NULL));

    for (int i = first_text; i < argc; i++)
    {
        const char *text = argv[i];
        glong length = g_utf8_strlen (text, -1);

        for (glong n = 1; n <= length; n++)
        {
            g_autofree char *prefix = g_utf8_substring (text, 0, n);

            type_text (slot, location, prefix, show_hidden, interval);
        }

        settle (slot, text, settle_ms);

        profile_recording = TRUE;
        for (glong n = length - 1; n >= 1; n--)
        {
            g_autofree char *prefix = g_utf8_substring (text, 0, n);

            type_text (slot, location, prefix, show_hidden, interval);
        }

        profile_recording = !profile_erasing_only;
        {
            g_autofree char *first = g_utf8_substring (text, 0, 1);

            settle (slot, first, settle_ms);
        }
    }

    if (exercise)
    {
        /* Everything else that happens to the cells of a view which are kept
         * across changes of its contents: other sizes, the other kind of
         * view, another folder, and back into a search. For a run under
         * sanitizers rather than for its timings. */
        const char * const actions[] =
        {
            "view.zoom-in", "view.zoom-in", "view.zoom-out", "view.zoom-out", "view.zoom-out",
            "slot.files-view-mode-toggle", "view.zoom-in", "view.zoom-out",
            "slot.files-view-mode-toggle", "view.zoom-standard", "slot.reload",
        };
        g_autoptr (GFile) elsewhere = g_file_new_for_path ("/usr/share");
        GtkWidget *target = GTK_WIDGET (nautilus_window_slot_get_current_view (slot));

        for (guint i = 0; i < G_N_ELEMENTS (actions); i++)
        {
            gboolean done = gtk_widget_activate_action (target, actions[i], NULL);

            g_print ("[%7.3f] %s%s\n", now_s (), actions[i], done ? "" : " (not available)");
            spin (350);
            target = GTK_WIDGET (nautilus_window_slot_get_current_view (slot));
        }

        nautilus_window_slot_open_location_full (slot, elsewhere, NULL);
        spin (1200);
        g_print ("[%7.3f] in /usr/share: %u items\n", now_s (), n_items (slot));
        type_text (slot, elsewhere, "icon", show_hidden, 300);
        settle (slot, "icon", 3000);
        nautilus_window_slot_open_location_full (slot, location, NULL);
        spin (1200);
        g_print ("[%7.3f] back: %u items\n", now_s (), n_items (slot));
    }

    if (dump != NULL)
    {
        /* What the view holds at the end, in the order it shows it: to
         * compare one way of getting to a search with another. */
        GListModel *model = G_LIST_MODEL (nautilus_files_view_get_private_model (nautilus_window_slot_get_current_view (slot)));
        g_autoptr (GString) listing = g_string_new (NULL);

        spin (800);
        for (guint i = 0; i < g_list_model_get_n_items (model); i++)
        {
            g_autoptr (GtkTreeListRow) row = g_list_model_get_item (model, i);
            g_autoptr (NautilusViewItem) item = gtk_tree_list_row_get_item (row);
            g_autofree char *uri = nautilus_file_get_uri (nautilus_view_item_get_file (item));

            g_string_append_printf (listing, "%s\n", uri);
        }

        g_file_set_contents (dump, listing->str, listing->len, NULL);
    }

    if (screenshot != NULL)
    {
        /* What the window shows at the end, to be looked at: cells that are
         * reused must not show what they showed before. */
        GtkSnapshot *snapshot = gtk_snapshot_new ();
        g_autoptr (GskRenderNode) node = NULL;

        spin (800);
        {
            /* The window as a whole, background included: light text on no
             * background at all is not much to look at. */
            g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (window));

            gdk_paintable_snapshot (paintable, snapshot,
                                    gtk_widget_get_width (GTK_WIDGET (window)),
                                    gtk_widget_get_height (GTK_WIDGET (window)));
        }
        node = gtk_snapshot_free_to_node (snapshot);
        if (node != NULL)
        {
            GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (window));
            g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, NULL);

            gdk_texture_save_to_png (texture, screenshot);
            g_print ("screenshot in %s\n", screenshot);
        }
    }

    g_array_sort (key_freezes, compare_freezes);
    if (key_freezes->len > 0)
    {
        g_print ("\nper key typed (%u keys): median %" G_GINT64_FORMAT " ms, 90%% %" G_GINT64_FORMAT " ms, worst %" G_GINT64_FORMAT " ms\n",
                 key_freezes->len,
                 g_array_index (key_freezes, gint64, key_freezes->len / 2),
                 g_array_index (key_freezes, gint64, key_freezes->len * 9 / 10),
                 g_array_index (key_freezes, gint64, key_freezes->len - 1));
    }
    g_print ("%u frames drawn, %" G_GINT64_FORMAT " ms in them, the longest %" G_GINT64_FORMAT " ms\n",
             n_frames, frames_total, frame_worst);
    g_print ("worst stall %" G_GINT64_FORMAT " ms; %u stalls of %d ms or more, %" G_GINT64_FORMAT " ms in all\n",
             worst_stall, n_stalls, STALL_MS, stalled_total);

    if (want_profile)
    {
        signal (SIGPROF, SIG_IGN);
        profile_report ();
    }

    /* Leaving through the front door would wait for whatever is still
     * walking the disk. */
    _exit (0);
}
