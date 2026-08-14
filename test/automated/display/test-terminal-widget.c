#include <nautilus-application.h>
#include <nautilus-file-utilities.h>
#include <nautilus-global-preferences.h>
#include <nautilus-resources.h>
#include <nautilus-tag-manager.h>
#include <nautilus-terminal-widget.h>
#include <nautilus-window.h>
#include <nautilus-window-slot.h>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib/gstdio.h>

static void
drain_main_context (void)
{
    while (g_main_context_iteration (NULL, FALSE))
    {
    }
}

static void
spin_main_context (guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time () + milliseconds * G_TIME_SPAN_MILLISECOND;

    do
    {
        drain_main_context ();
        g_usleep (1000);
    }
    while (g_get_monotonic_time () < deadline);
}

static void
test_terminal_dispose_during_spawn (void)
{
    g_autoptr (GFile) location = g_file_new_for_path (g_get_tmp_dir ());

    for (guint iteration = 0; iteration < 25; iteration++)
    {
        NautilusTerminalWidget *terminal =
            nautilus_terminal_widget_new_with_location (location);

        g_object_ref_sink (terminal);
        nautilus_terminal_widget_toggle_visible (terminal);
        gtk_widget_activate_action (GTK_WIDGET (terminal),
                                    "terminal.split-horizontal",
                                    NULL);
        gtk_widget_activate_action (GTK_WIDGET (terminal),
                                    "terminal.close-pane",
                                    NULL);
        g_object_unref (terminal);
        drain_main_context ();
    }

    drain_main_context ();
}

static void
test_slot_dispose_before_idle (void)
{
    for (guint iteration = 0; iteration < 25; iteration++)
    {
        NautilusWindowSlot *slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);

        g_object_ref_sink (slot);
        g_object_unref (slot);
    }

    drain_main_context ();
}

static GtkWindow *
create_live_slot_window (GFile *location)
{
    GtkWindow *window = GTK_WINDOW (gtk_window_new ());
    NautilusWindowSlot *slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);

    g_object_ref_sink (window);
    gtk_window_set_child (window, GTK_WIDGET (slot));
    gtk_window_present (window);
    nautilus_window_slot_open_location_full (slot, location, NULL);
    nautilus_window_slot_toggle_terminal (slot);

    return window;
}

static void
test_live_slots_dispose_with_vte_children (void)
{
    g_autofree gchar *first_path = g_build_filename (g_get_tmp_dir (),
                                                      "nautilus-terminal-live-a",
                                                      NULL);
    g_autofree gchar *second_path = g_build_filename (g_get_tmp_dir (),
                                                       "nautilus-terminal-live-b",
                                                       NULL);
    g_autoptr (GFile) first_location = NULL;
    g_autoptr (GFile) second_location = NULL;

    g_assert_cmpint (g_mkdir_with_parents (first_path, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (second_path, 0700), ==, 0);
    first_location = g_file_new_for_path (first_path);
    second_location = g_file_new_for_path (second_path);

    for (guint iteration = 0; iteration < 25; iteration++)
    {
        GtkWindow *first_window = create_live_slot_window (first_location);
        GtkWindow *second_window = create_live_slot_window (second_location);
        NautilusWindowSlot *first_slot = NAUTILUS_WINDOW_SLOT (
            gtk_window_get_child (first_window));
        NautilusWindowSlot *second_slot = NAUTILUS_WINDOW_SLOT (
            gtk_window_get_child (second_window));

        spin_main_context (100);
        nautilus_window_slot_open_location_full (first_slot, second_location, NULL);
        nautilus_window_slot_open_location_full (second_slot, first_location, NULL);
        spin_main_context (50);

        nautilus_window_slot_toggle_terminal (first_slot);
        nautilus_window_slot_toggle_terminal (second_slot);
        gtk_window_destroy (first_window);
        gtk_window_destroy (second_window);
        g_object_unref (first_window);
        g_object_unref (second_window);
        spin_main_context (50);
    }

    drain_main_context ();
    g_assert_cmpint (g_rmdir (first_path), ==, 0);
    g_assert_cmpint (g_rmdir (second_path), ==, 0);
}

static NautilusWindowSlot *
get_active_slot (NautilusWindow *window)
{
    NautilusWindowSlot *slot = NULL;

    g_object_get (window, "active-slot", &slot, NULL);

    return slot;
}

static NautilusTerminalWidget *
find_terminal_widget (GtkWidget *root)
{
    if (NAUTILUS_IS_TERMINAL_WIDGET (root))
    {
        return NAUTILUS_TERMINAL_WIDGET (root);
    }

    for (GtkWidget *child = gtk_widget_get_first_child (root);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusTerminalWidget *terminal = find_terminal_widget (child);

        if (terminal != NULL)
        {
            return terminal;
        }
    }

    return NULL;
}

static void
test_split_pane_sync_navigation (void)
{
    const gchar *stress_first = g_getenv ("NAUTILUS_TERMINAL_STRESS_LOCATION_A");
    const gchar *stress_second = g_getenv ("NAUTILUS_TERMINAL_STRESS_LOCATION_B");
    g_autofree gchar *temporary_first = NULL;
    g_autofree gchar *temporary_second = NULL;
    g_autoptr (GFile) first_location = NULL;
    g_autoptr (GFile) second_location = NULL;
    gboolean remove_temporary_locations = stress_first == NULL || stress_second == NULL;

    if (remove_temporary_locations)
    {
        temporary_first = g_build_filename (g_get_tmp_dir (),
                                             "nautilus-terminal-split-a",
                                             NULL);
        temporary_second = g_build_filename (g_get_tmp_dir (),
                                              "nautilus-terminal-split-b",
                                              NULL);
        g_assert_cmpint (g_mkdir_with_parents (temporary_first, 0700), ==, 0);
        g_assert_cmpint (g_mkdir_with_parents (temporary_second, 0700), ==, 0);
        stress_first = temporary_first;
        stress_second = temporary_second;
    }

    first_location = g_file_new_for_path (stress_first);
    second_location = g_file_new_for_path (stress_second);

    for (guint iteration = 0; iteration < 25; iteration++)
    {
        NautilusWindow *window = nautilus_window_new ();
        g_autoptr (NautilusWindowSlot) slot = NULL;
        NautilusTerminalWidget *terminal;

        g_object_ref_sink (window);
        nautilus_window_open_location_full (window, first_location, 0, NULL);
        spin_main_context (100);
        slot = get_active_slot (window);
        g_assert_nonnull (slot);
        nautilus_window_slot_toggle_terminal (slot);
        spin_main_context (100);

        terminal = find_terminal_widget (GTK_WIDGET (slot));
        g_assert_nonnull (terminal);
        g_assert_true (gtk_widget_activate_action (GTK_WIDGET (terminal),
                                                   "terminal.split-horizontal",
                                                   NULL));
        spin_main_context (100);

        for (guint navigation = 0; navigation < 3; navigation++)
        {
            nautilus_window_slot_open_location_full (slot, second_location, NULL);
            spin_main_context (50);
            nautilus_window_slot_open_location_full (slot, first_location, NULL);
            spin_main_context (50);
        }

        gtk_window_destroy (GTK_WINDOW (window));
        g_object_unref (window);
        spin_main_context (100);
    }

    drain_main_context ();
    if (remove_temporary_locations)
    {
        g_assert_cmpint (g_rmdir (temporary_first), ==, 0);
        g_assert_cmpint (g_rmdir (temporary_second), ==, 0);
    }
}

static void
test_window_tabs_dispose_with_vte_children (void)
{
    g_autofree gchar *first_path = g_build_filename (g_get_tmp_dir (),
                                                      "nautilus-terminal-tabs-a",
                                                      NULL);
    g_autofree gchar *second_path = g_build_filename (g_get_tmp_dir (),
                                                       "nautilus-terminal-tabs-b",
                                                       NULL);
    g_autoptr (GFile) first_location = NULL;
    g_autoptr (GFile) second_location = NULL;

    g_assert_cmpint (g_mkdir_with_parents (first_path, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (second_path, 0700), ==, 0);
    first_location = g_file_new_for_path (first_path);
    second_location = g_file_new_for_path (second_path);

    for (guint iteration = 0; iteration < 25; iteration++)
    {
        NautilusWindow *window = nautilus_window_new ();
        g_autoptr (NautilusWindowSlot) first_slot = NULL;
        g_autoptr (NautilusWindowSlot) second_slot = NULL;

        g_object_ref_sink (window);
        nautilus_window_open_location_full (window, first_location, 0, NULL);
        spin_main_context (50);
        first_slot = get_active_slot (window);
        g_assert_nonnull (first_slot);
        nautilus_window_slot_toggle_terminal (first_slot);

        nautilus_window_open_location_full (window,
                                            second_location,
                                            NAUTILUS_OPEN_FLAG_NEW_TAB,
                                            NULL);
        spin_main_context (50);
        second_slot = get_active_slot (window);
        g_assert_nonnull (second_slot);
        g_assert_true (first_slot != second_slot);
        nautilus_window_slot_toggle_terminal (second_slot);

        g_object_set (window, "active-slot", first_slot, NULL);
        nautilus_window_slot_open_location_full (first_slot, second_location, NULL);
        g_object_set (window, "active-slot", second_slot, NULL);
        nautilus_window_slot_open_location_full (second_slot, first_location, NULL);
        spin_main_context (50);

        gtk_window_destroy (GTK_WINDOW (window));
        g_object_unref (window);
        spin_main_context (50);
    }

    drain_main_context ();
    g_assert_cmpint (g_rmdir (first_path), ==, 0);
    g_assert_cmpint (g_rmdir (second_path), ==, 0);
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (NautilusApplication) app = NULL;
    g_autoptr (NautilusTagManager) tag_manager = NULL;
    g_autoptr (GSettings) settings = NULL;
    g_autofree gchar *test_root = NULL;
    g_autofree gchar *config_dir = NULL;
    g_autofree gchar *data_dir = NULL;
    g_autofree gchar *cache_dir = NULL;

    test_root = g_dir_make_tmp ("nautilus-terminal-test-XXXXXX", NULL);
    g_assert_nonnull (test_root);
    config_dir = g_build_filename (test_root, "config", NULL);
    data_dir = g_build_filename (test_root, "data", NULL);
    cache_dir = g_build_filename (test_root, "cache", NULL);
    g_assert_cmpint (g_mkdir_with_parents (config_dir, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (data_dir, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (cache_dir, 0700), ==, 0);
    g_setenv ("HOME", test_root, TRUE);
    g_setenv ("XDG_CONFIG_HOME", config_dir, TRUE);
    g_setenv ("XDG_DATA_HOME", data_dir, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache_dir, TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

    g_test_init (&argc, &argv, NULL);
    nautilus_register_resource ();
    nautilus_ensure_extension_points ();
    tag_manager = nautilus_tag_manager_new_dummy ();
    app = nautilus_application_new ();
    g_application_set_flags (G_APPLICATION (app),
                             g_application_get_flags (G_APPLICATION (app)) |
                             G_APPLICATION_NON_UNIQUE);
    g_assert_true (g_application_register (G_APPLICATION (app), NULL, NULL));
    adw_init ();

    settings = g_settings_new ("org.gnome.nautilus.terminal");
    g_settings_set_boolean (settings, "visible", FALSE);
    g_settings_set_string (settings, "local-sync-mode", "both");

    g_test_add_func ("/terminal/dispose-during-spawn",
                     test_terminal_dispose_during_spawn);
    g_test_add_func ("/terminal/slot-dispose-before-idle",
                     test_slot_dispose_before_idle);
    g_test_add_func ("/terminal/live-slots-dispose-with-vte-children",
                     test_live_slots_dispose_with_vte_children);
    g_test_add_func ("/terminal/window-tabs-dispose-with-vte-children",
                     test_window_tabs_dispose_with_vte_children);
    g_test_add_func ("/terminal/split-pane-sync-navigation",
                     test_split_pane_sync_navigation);

    return g_test_run ();
}
