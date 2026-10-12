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
#include <vte/vte.h>

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

static VteTerminal *
find_vte_terminal (GtkWidget *widget)
{
    if (VTE_IS_TERMINAL (widget))
    {
        return VTE_TERMINAL (widget);
    }

    for (GtkWidget *child = gtk_widget_get_first_child (widget);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        VteTerminal *found = find_vte_terminal (child);

        if (found != NULL)
        {
            return found;
        }
    }

    return NULL;
}

typedef struct
{
    guint n_changes;
    GFile *location;
} DirectoryChanges;

static void
on_change_directory (NautilusTerminalWidget *terminal,
                     GFile                  *location,
                     gpointer                user_data)
{
    DirectoryChanges *changes = user_data;

    changes->n_changes++;
    g_set_object (&changes->location, location);
}

/* What a shell prints at its prompt to say where it is. */
static void
report_directory (VteTerminal *vte,
                  const char  *uri)
{
    g_autofree char *sequence = g_strdup_printf ("\033]7;%s\007", uri);

    vte_terminal_feed (vte, sequence, -1);
}

static void
assert_reported_as (VteTerminal      *vte,
                    DirectoryChanges *changes,
                    const char       *uri,
                    const char       *expected_path)
{
    guint before = changes->n_changes;

    report_directory (vte, uri);

    if (expected_path == NULL)
    {
        /* VTE takes what it is fed in on its own time. */
        spin_main_context (400);
        g_assert_cmpuint (changes->n_changes, ==, before);
    }
    else
    {
        g_autofree char *path = NULL;

        for (guint i = 0; i < 100 && changes->n_changes == before; i++)
        {
            spin_main_context (50);
        }

        g_assert_cmpuint (changes->n_changes, ==, before + 1);
        path = g_file_get_path (changes->location);
        g_assert_cmpstr (path, ==, expected_path);
    }
}

/* The folder a shell reports moves the file manager, whatever characters are
 * in its name, and only when it is a folder of this machine. */
static void
test_reported_directory_reaches_file_manager (void)
{
    g_autoptr (GFile) location = g_file_new_for_path (g_get_tmp_dir ());
    NautilusTerminalWidget *terminal = nautilus_terminal_widget_new_with_location (location);
    DirectoryChanges changes = { 0 };
    VteTerminal *vte;

    g_object_ref_sink (terminal);
    vte = find_vte_terminal (GTK_WIDGET (terminal));
    g_assert_nonnull (vte);
    g_signal_connect (terminal, "change-directory", G_CALLBACK (on_change_directory), &changes);

    assert_reported_as (vte, &changes, "file:///usr", "/usr");
    /* Where the file manager already is: nothing to do. */
    assert_reported_as (vte, &changes, "file:///usr/", NULL);
    assert_reported_as (vte, &changes, "file://localhost/opt", "/opt");

    /* As the hooks of the widget print it: only the '%' encoded. */
    assert_reported_as (vte, &changes, "file:///var/C#/src", "/var/C#/src");
    assert_reported_as (vte, &changes, "file:///var/what?/now", "/var/what?/now");
    assert_reported_as (vte, &changes, "file:///var/nv 100%25/a%2520b", "/var/nv 100%/a%20b");
    /* As fish and vte.sh print it: all of it encoded. */
    assert_reported_as (vte, &changes, "file:///var/C%23/src%20x", "/var/C#/src x");

    /* An ssh typed by hand, with a remote shell that reports its folder:
     * /etc over there is not /etc here. */
    assert_reported_as (vte, &changes, "file://some-other-machine.example/etc", NULL);
    assert_reported_as (vte, &changes, "sftp://some-other-machine.example/etc", NULL);

    g_clear_object (&changes.location);
    g_object_unref (terminal);
    drain_main_context ();
}

/* Opening the terminal while looking at the trash starts a shell somewhere
 * else, since the trash is no folder to be in. That must not move the file
 * manager out of the trash. */
static void
test_shell_start_keeps_virtual_location (void)
{
    g_autoptr (GFile) trash = g_file_new_for_uri ("trash:///");
    NautilusTerminalWidget *terminal = nautilus_terminal_widget_new_with_location (trash);
    GtkWindow *window = GTK_WINDOW (gtk_window_new ());
    DirectoryChanges changes = { 0 };
    VteTerminal *vte;

    g_object_ref_sink (window);
    gtk_window_set_child (window, GTK_WIDGET (terminal));
    gtk_window_present (window);
    g_signal_connect (terminal, "change-directory", G_CALLBACK (on_change_directory), &changes);

    nautilus_terminal_widget_toggle_visible (terminal);
    /* Long enough for the shell to draw its first prompt. */
    spin_main_context (1500);
    g_assert_cmpuint (changes.n_changes, ==, 0);

    /* A "cd" typed by the user afterwards still counts. */
    vte = find_vte_terminal (GTK_WIDGET (terminal));
    g_assert_nonnull (vte);
    assert_reported_as (vte, &changes, "file:///usr/share", "/usr/share");

    g_clear_object (&changes.location);
    gtk_window_destroy (window);
    g_object_unref (window);
    spin_main_context (100);
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
    g_test_add_func ("/terminal/reported-directory-reaches-file-manager",
                     test_reported_directory_reaches_file_manager);
    g_test_add_func ("/terminal/shell-start-keeps-virtual-location",
                     test_shell_start_keeps_virtual_location);

    return g_test_run ();
}
