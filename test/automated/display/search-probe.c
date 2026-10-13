/* search-probe.c: types a search into a real window and measures what the user
 * feels -- how long the main loop goes without turning -- against the real
 * folder, the real settings that matter and the search daemon of the session.
 *
 * Not a test: it has no opinion on the outcome, and what it measures depends
 * on the machine. Run by hand:
 *
 *   builddir/test/automated/display/search-probe [--hidden] [--no-daemon]
 *       [--dir DIR] [--interval MS] [--settle MS] TEXT...
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
#include <nautilus-file-utilities.h>
#include <nautilus-files-view.h>
#include <nautilus-global-preferences.h>
#include <nautilus-query.h>
#include <nautilus-resources.h>
#include <nautilus-tag-manager.h>
#include <nautilus-view-model.h>
#include <nautilus-window-slot.h>

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <unistd.h>

#define TICK_MS 2
#define STALL_MS 100

static gint64 last_tick;
static gint64 worst_stall;
static gint64 step_worst;
static gint64 stalled_total;
static guint n_stalls;
static gint64 started;
static gint watchdog_fired;
static gboolean want_backtrace;

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
        g_print ("  [%7.3f] main loop stalled for %" G_GINT64_FORMAT " ms\n", now_s (), gap);
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

    g_print ("[%7.3f] %u cells built for the view\n", now_s (), n_cells (GTK_WIDGET (slot)));
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
        g_printerr ("Usage: %s [--hidden] [--no-daemon] [--backtrace] [--dir DIR] "
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

    chooser_settings = g_settings_new ("org.gtk.gtk4.Settings.FileChooser");
    g_settings_set_boolean (chooser_settings, "show-hidden", show_hidden);

    location = g_file_new_for_path (dir);
    window = GTK_WINDOW (gtk_window_new ());
    gtk_window_set_default_size (window, 900, 600);
    slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);
    gtk_window_set_child (window, GTK_WIDGET (slot));
    gtk_window_present (window);
    nautilus_window_slot_open_location_full (slot, location, NULL);

    started = g_get_monotonic_time ();
    last_tick = started;
    spin (1500);

    g_print ("searching in %s, hidden files %s, daemon %s\n", dir,
             show_hidden ? "shown" : "not shown",
             g_str_equal (g_get_user_runtime_dir (), scratch) ? "out of reach" : "of the session");

    last_tick = g_get_monotonic_time ();
    worst_stall = 0;
    g_timeout_add (TICK_MS, on_tick, NULL);
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

        for (glong n = length - 1; n >= 1; n--)
        {
            g_autofree char *prefix = g_utf8_substring (text, 0, n);

            type_text (slot, location, prefix, show_hidden, interval);
        }

        {
            g_autofree char *first = g_utf8_substring (text, 0, 1);

            settle (slot, first, settle_ms);
        }
    }

    g_print ("\nworst stall %" G_GINT64_FORMAT " ms; %u stalls of %d ms or more, %" G_GINT64_FORMAT " ms in all\n",
             worst_stall, n_stalls, STALL_MS, stalled_total);

    /* Leaving through the front door would wait for whatever is still
     * walking the disk. */
    _exit (0);
}
