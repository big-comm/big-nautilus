#include <nautilus-application.h>
#include <nautilus-file-utilities.h>
#include <nautilus-global-preferences.h>
#include <nautilus-resources.h>
#include <nautilus-tag-manager.h>
#include <nautilus-terminal-widget.h>
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

    gtk_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme",
                  FALSE,
                  NULL);
    adw_init ();
    nautilus_register_resource ();
    nautilus_ensure_extension_points ();
    nautilus_global_preferences_init ();
    tag_manager = nautilus_tag_manager_new_dummy ();
    app = nautilus_application_new ();

    settings = g_settings_new ("org.gnome.nautilus.terminal");
    g_settings_set_boolean (settings, "visible", FALSE);
    g_settings_set_string (settings, "local-sync-mode", "none");

    g_test_add_func ("/terminal/dispose-during-spawn",
                     test_terminal_dispose_during_spawn);
    g_test_add_func ("/terminal/slot-dispose-before-idle",
                     test_slot_dispose_before_idle);

    return g_test_run ();
}
