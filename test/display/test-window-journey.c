/* test-window-journey.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One window, driven through the actions its keys and menus trigger, across
 * what the fork adds to it: the order of names, the views, the embedded
 * terminal following the folder both ways, colour tags leaving with a
 * trashed file, and search.
 */

#include <nautilus-application.h>
#include <nautilus-file.h>
#include <nautilus-file-changes-queue.h>
#include <nautilus-file-operations.h>
#include <nautilus-file-utilities.h>
#include <nautilus-files-view.h>
#include <nautilus-global-preferences.h>
#include <nautilus-query.h>
#include <nautilus-resources.h>
#include <nautilus-tag-colors.h>
#include <nautilus-tag-manager.h>
#include <nautilus-view-info.h>
#include <nautilus-view-item.h>
#include <nautilus-window.h>
#include <nautilus-window-slot.h>

#include <adwaita.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

/* The order `ls -1` gives for these in a pt_BR.UTF-8 locale. */
static const char * const episodes[] =
{
    "CSI.Crime.Scene.Investigation.S06E01.1080p.WEB-DL.h264.Dual2.0-Dinho.mkv",
    "Csi.Crime.Scene.Investigation.S06E01.1080p.WEB-DL.h264.Dual2.0-Dinho.srt",
    "CSI.Crime.Scene.Investigation.S06E02.1080p.WEB-DL.h264.Dual2.0-Dinho.mkv",
    "CSI.Crime.Scene.Investigation.S06E02.1080p.WEB-DL.h264.Dual2.0-Dinho.srt",
    "Csi.Crime.Scene.Investigation.S06E21.1080p.WEB-DL.h264.Dual2.0-Dinho.mkv",
    "Csi.Crime.Scene.Investigation.S06E21.1080p.WEB-DL.h264.Dual2.0-Dinho.srt",
};

static char *folder;
static char *subfolder;

static void
spin (guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time () + milliseconds * G_TIME_SPAN_MILLISECOND;

    while (g_get_monotonic_time () < deadline)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (2000);
    }
}

typedef gboolean (*Condition) (gpointer data);

/* Spins until @condition holds; fails the test after @seconds. */
static void
wait_for (Condition    condition,
          gpointer     data,
          guint        seconds,
          const char  *what)
{
    gint64 deadline = g_get_monotonic_time () + seconds * G_TIME_SPAN_SECOND;

    while (!condition (data))
    {
        if (g_get_monotonic_time () > deadline)
        {
            g_error ("Timed out waiting for %s", what);
        }

        g_main_context_iteration (NULL, FALSE);
        g_usleep (2000);
    }
}

static NautilusWindowSlot *
active_slot (NautilusWindow *window)
{
    NautilusWindowSlot *slot = NULL;

    g_object_get (window, "active-slot", &slot, NULL);
    g_object_unref (slot);

    return slot;
}

static NautilusFilesView *
active_view (NautilusWindow *window)
{
    return nautilus_window_slot_get_current_view (active_slot (window));
}

/* The names the view shows, in its order. */
static GStrv
shown_names (NautilusWindow *window)
{
    NautilusFilesView *view = active_view (window);
    GListModel *model = view != NULL ? G_LIST_MODEL (nautilus_files_view_get_private_model (view)) : NULL;
    g_autoptr (GStrvBuilder) names = g_strv_builder_new ();

    for (guint i = 0; model != NULL && i < g_list_model_get_n_items (model); i++)
    {
        g_autoptr (GtkTreeListRow) row = g_list_model_get_item (model, i);
        g_autoptr (NautilusViewItem) item = gtk_tree_list_row_get_item (row);

        g_strv_builder_add (names, nautilus_file_get_display_name (nautilus_view_item_get_file (item)));
    }

    return g_strv_builder_end (names);
}

static gboolean
shows_all_episodes (gpointer window)
{
    g_auto (GStrv) names = shown_names (window);

    return g_strv_length (names) == G_N_ELEMENTS (episodes) + 1;
}

static gboolean
is_loaded (gpointer window)
{
    NautilusFilesView *view = active_view (window);

    return view != NULL && !nautilus_files_view_get_loading (view);
}

static void
assert_episode_order (NautilusWindow *window,
                      gboolean        reversed)
{
    g_auto (GStrv) names = shown_names (window);
    g_autoptr (GPtrArray) files = g_ptr_array_new ();

    /* Folders come first; what is left are the episodes. */
    for (guint i = 0; names[i] != NULL; i++)
    {
        if (g_str_has_prefix (names[i], "CSI") || g_str_has_prefix (names[i], "Csi"))
        {
            g_ptr_array_add (files, names[i]);
        }
    }

    g_assert_cmpuint (files->len, ==, G_N_ELEMENTS (episodes));
    for (guint i = 0; i < files->len; i++)
    {
        guint expected = reversed ? G_N_ELEMENTS (episodes) - 1 - i : i;

        g_assert_cmpstr (g_ptr_array_index (files, i), ==, episodes[expected]);
    }
}

static void
activate (NautilusWindow *window,
          const char     *action,
          GVariant       *parameter)
{
    GtkWidget *target = GTK_WIDGET (active_view (window));

    g_assert_true (gtk_widget_activate_action_variant (target, action, parameter));
}

static VteTerminal *
find_vte (GtkWidget *widget)
{
    if (VTE_IS_TERMINAL (widget))
    {
        return VTE_TERMINAL (widget);
    }

    for (GtkWidget *child = gtk_widget_get_first_child (widget);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        VteTerminal *found = find_vte (child);

        if (found != NULL)
        {
            return found;
        }
    }

    return NULL;
}

typedef struct
{
    VteTerminal *vte;
    const char *path;
} TerminalAt;

/* Whether the shell said, the way it does at its prompt, that it is in @path. */
static gboolean
terminal_is_at (gpointer data)
{
    TerminalAt *at = data;
    g_autoptr (GUri) uri = vte_terminal_ref_termprop_uri (at->vte, VTE_TERMPROP_CURRENT_DIRECTORY_URI);
    g_autofree char *path = uri != NULL ? g_uri_unescape_string (g_uri_get_path (uri), NULL) : NULL;

    return g_strcmp0 (path, at->path) == 0;
}

typedef struct
{
    NautilusWindow *window;
    const char *path;
} SlotAt;

static gboolean
slot_is_at (gpointer data)
{
    SlotAt *at = data;
    GFile *location = nautilus_window_slot_get_location (active_slot (at->window));
    g_autofree char *path = location != NULL ? g_file_get_path (location) : NULL;

    return g_strcmp0 (path, at->path) == 0 && is_loaded (at->window);
}

static void
test_sort_and_views (NautilusWindow *window)
{
    g_autoptr (GFile) location = g_file_new_for_path (folder);
    guint view_id;

    nautilus_window_open_location_full (window, location, 0, NULL);
    wait_for (shows_all_episodes, window, 10, "the folder to load");
    assert_episode_order (window, FALSE);

    activate (window, "view.sort", g_variant_new ("(sb)", "name", TRUE));
    spin (300);
    assert_episode_order (window, TRUE);

    activate (window, "view.sort", g_variant_new ("(sb)", "name", FALSE));
    spin (300);
    assert_episode_order (window, FALSE);

    view_id = nautilus_files_view_get_view_id (active_view (window));
    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (active_slot (window)),
                                               "slot.files-view-mode-toggle", NULL));
    wait_for (shows_all_episodes, window, 10, "the other view to load");
    g_assert_cmpuint (nautilus_files_view_get_view_id (active_view (window)), !=, view_id);
    assert_episode_order (window, FALSE);

    activate (window, "view.zoom-in", NULL);
    activate (window, "view.zoom-out", NULL);
    activate (window, "view.zoom-out", NULL);
    activate (window, "view.zoom-standard", NULL);
    spin (200);
    assert_episode_order (window, FALSE);

    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (active_slot (window)),
                                               "slot.files-view-mode-toggle", NULL));
    wait_for (shows_all_episodes, window, 10, "the first view to come back");
    g_assert_cmpuint (nautilus_files_view_get_view_id (active_view (window)), ==, view_id);
}

static void
test_terminal_follows_both_ways (NautilusWindow *window)
{
    NautilusWindowSlot *slot = active_slot (window);
    g_autoptr (GFile) sub = g_file_new_for_path (subfolder);
    TerminalAt terminal_at;
    SlotAt slot_at = { window, folder };
    VteTerminal *vte;

    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (slot), "slot.toggle-terminal", NULL));
    spin (200);
    vte = find_vte (GTK_WIDGET (slot));
    g_assert_nonnull (vte);
    g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (vte)));

    /* The shell starts in the folder shown. */
    terminal_at = (TerminalAt) { vte, folder };
    wait_for (terminal_is_at, &terminal_at, 15, "the shell to report the folder");

    /* Browsing moves the shell along. */
    nautilus_window_slot_open_location_full (slot, sub, NULL);
    terminal_at.path = subfolder;
    wait_for (terminal_is_at, &terminal_at, 15, "the shell to follow the folder");

    /* And a cd in the shell moves the view. */
    vte_terminal_feed_child (vte, "cd ..\r", -1);
    wait_for (slot_is_at, &slot_at, 15, "the view to follow the shell");

    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (slot), "slot.toggle-terminal", NULL));
    spin (200);
    g_assert_false (gtk_widget_get_mapped (GTK_WIDGET (vte)));
}

static char *
episode_uri (guint index)
{
    g_autofree char *path = g_build_filename (folder, episodes[index], NULL);

    return g_filename_to_uri (path, NULL, NULL);
}

/* Undoing the trash needs a gvfsd-trash watching this test's own data
 * directory, which the session bus does not give it. */
static void
test_tag_dropped_with_trashed_file (NautilusWindow *window)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autofree char *uri = episode_uri (4);
    g_autoptr (NautilusFile) file = nautilus_file_get_by_uri (uri);
    g_autoptr (GFile) location = g_file_new_for_uri (uri);
    g_autoptr (GError) error = NULL;

    nautilus_files_view_set_selection (active_view (window),
                                       &(GList) { .data = file },
                                       NAUTILUS_SELECTION_SOURCE_MANUAL);
    spin (100);
    activate (window, "view.tag-color", g_variant_new_string ("red"));
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, uri), ==, NAUTILUS_TAG_COLOR_RED);

    /* Under GTest the trash job deletes outright, so this takes the steps it
     * takes for each file it trashes. */
    g_assert_true (g_file_trash (location, NULL, &error));
    g_assert_no_error (error);
    nautilus_file_changes_queue_file_removed (location);
    nautilus_file_changes_consume_changes ();
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, uri), ==, NAUTILUS_TAG_COLOR_NONE);
}

static gboolean
localsearch_activatable (void)
{
    g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    g_autoptr (GVariant) reply = NULL;
    g_autofree const char **names = NULL;

    reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "ListActivatableNames",
                                         NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE,
                                         -1, NULL, NULL);
    g_assert_nonnull (reply);
    g_variant_get (reply, "(^a&s)", &names);

    return g_strv_contains (names, "org.freedesktop.Tracker3.Miner.Files");
}

static gboolean
search_finished (gpointer window)
{
    return !nautilus_window_slot_get_allow_stop (active_slot (window)) && is_loaded (window);
}

static void
test_search_finds_episodes (NautilusWindow *window)
{
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    g_autoptr (GFile) location = g_file_new_for_path (folder);
    g_auto (GStrv) names = NULL;

    /* Without it the search falls back to walking the folder, after saying so. */
    if (!localsearch_activatable ())
    {
        g_test_expect_message ("nautilus-search", G_LOG_LEVEL_WARNING,
                               "Could not establish a connection to Localsearch*");
    }

    nautilus_query_set_text (query, "E21");
    nautilus_query_set_location (query, location);
    nautilus_window_slot_search (active_slot (window), query);
    spin (300);
    wait_for (search_finished, window, 30, "the search to finish");
    g_test_assert_expected_messages ();

    names = shown_names (window);
    /* Episode 4 went to the trash earlier on. */
    g_assert_cmpuint (g_strv_length (names), ==, 1);
    g_assert_cmpstr (names[0], ==, episodes[5]);
}

static void
test_window_journey (void)
{
    NautilusWindow *window = nautilus_window_new ();

    g_object_ref_sink (window);
    gtk_window_present (GTK_WINDOW (window));

    test_sort_and_views (window);
    test_terminal_follows_both_ways (window);
    test_tag_dropped_with_trashed_file (window);
    test_search_finds_episodes (window);

    gtk_window_destroy (GTK_WINDOW (window));
    g_object_unref (window);
    spin (300);
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (NautilusApplication) app = NULL;
    g_autoptr (NautilusTagManager) tag_manager = NULL;
    g_autoptr (GSettings) terminal_settings = NULL;
    g_autofree char *root = g_dir_make_tmp ("nautilus-journey-XXXXXX", NULL);
    const char * const dirs[] = { "config", "data", "cache", "home" };

    g_assert_nonnull (root);
    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
        g_autofree char *dir = g_build_filename (root, dirs[i], NULL);

        g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
    }

    /* Before anything reads them: GLib keeps the first answer. */
    g_setenv ("HOME", g_build_filename (root, "home", NULL), TRUE);
    g_setenv ("XDG_CONFIG_HOME", g_build_filename (root, "config", NULL), TRUE);
    g_setenv ("XDG_DATA_HOME", g_build_filename (root, "data", NULL), TRUE);
    g_setenv ("XDG_CACHE_HOME", g_build_filename (root, "cache", NULL), TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    /* The terminal's directory sync is written for these shells. */
    g_setenv ("SHELL", "/bin/bash", TRUE);

    folder = g_build_filename (root, "home", "CSI.S06", NULL);
    subfolder = g_build_filename (folder, "pasta com espaço", NULL);
    g_assert_cmpint (g_mkdir_with_parents (subfolder, 0700), ==, 0);
    for (guint i = 0; i < G_N_ELEMENTS (episodes); i++)
    {
        g_autofree char *path = g_build_filename (folder, episodes[i], NULL);

        g_assert_true (g_file_set_contents (path, "x", -1, NULL));
    }

    g_test_init (&argc, &argv, NULL);
    nautilus_register_resource ();
    nautilus_ensure_extension_points ();
    tag_manager = nautilus_tag_manager_new_dummy ();
    app = nautilus_application_new ();
    g_application_set_flags (G_APPLICATION (app),
                             g_application_get_flags (G_APPLICATION (app)) |
                             G_APPLICATION_NON_UNIQUE);
    {
        /* Starting up, the portal helper warns when the compositor is not
         * GNOME Shell. Fatal again for everything the test does. */
        GLogLevelFlags fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);

        g_assert_true (g_application_register (G_APPLICATION (app), NULL, NULL));
        g_log_set_always_fatal (fatal);
    }
    adw_init ();

    terminal_settings = g_settings_new ("org.gnome.nautilus.terminal");
    g_settings_set_boolean (terminal_settings, "visible", FALSE);
    g_settings_set_string (terminal_settings, "local-sync-mode", "both");

    g_test_add_func ("/window/journey", test_window_journey);

    return g_test_run ();
}
