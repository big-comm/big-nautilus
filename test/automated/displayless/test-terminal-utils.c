#include <nautilus-terminal-widget.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
test_title_sanitization (void)
{
    g_autofree char *title = NULL;
    g_autofree char *invalid = NULL;
    g_autofree char *long_input = g_strnfill (100, 'a');
    g_autofree char *long_title = NULL;

    title = nautilus_terminal_sanitize_title ("  build\nproject\033[31m\u202etest  ");
    g_assert_cmpstr (title, ==, "build project[31mtest");

    invalid = nautilus_terminal_sanitize_title ("bad\xfftitle");
    g_assert_true (g_utf8_validate (invalid, -1, NULL));
    g_assert_nonnull (strstr (invalid, "bad"));
    g_assert_nonnull (strstr (invalid, "title"));

    long_title = nautilus_terminal_sanitize_title (long_input);
    g_assert_cmpuint (g_utf8_strlen (long_title, -1), ==, 80);

    g_assert_null (nautilus_terminal_sanitize_title (NULL));
    g_assert_null (nautilus_terminal_sanitize_title ("\n\t"));
    g_assert_null (nautilus_terminal_sanitize_title ("   \n   "));
}

static void
test_ssh_identity (void)
{
    g_assert_true (nautilus_terminal_ssh_identity_equal ("Server.EXAMPLE",
                                                         "alice",
                                                         "2222",
                                                         "server.example",
                                                         "alice",
                                                         "2222"));
    g_assert_false (nautilus_terminal_ssh_identity_equal ("server.example",
                                                          "alice",
                                                          "2222",
                                                          "server.example",
                                                          "bob",
                                                          "2222"));
    g_assert_false (nautilus_terminal_ssh_identity_equal ("server.example",
                                                          "alice",
                                                          "2222",
                                                          "server.example",
                                                          "alice",
                                                          "22"));
    g_assert_true (nautilus_terminal_ssh_identity_equal ("server.example",
                                                         NULL,
                                                         NULL,
                                                         "SERVER.EXAMPLE",
                                                         g_get_user_name (),
                                                         "22"));
}

static void
test_remote_shell_command (void)
{
    g_autofree char *tmp_dir = g_dir_make_tmp ("nautilus-terminal-test-XXXXXX",
                                               NULL);
    g_autofree char *remote_path =
        g_build_filename (tmp_dir, "path with spaces'; false", NULL);
    g_autofree char *command = NULL;
    g_auto (GStrv) environment = g_get_environ ();
    g_autoptr (GError) error = NULL;
    int wait_status = 0;

    g_assert_cmpint (g_mkdir (remote_path, 0700), ==, 0);
    environment = g_environ_setenv (environment, "SHELL", "/bin/true", TRUE);

    command = nautilus_terminal_build_remote_shell_command (remote_path, TRUE);
    {
        char *argv[] = { "/bin/sh", "-c", command, NULL };

        g_assert_true (g_spawn_sync (NULL,
                                     argv,
                                     environment,
                                     G_SPAWN_DEFAULT,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     &wait_status,
                                     &error));
    }
    g_assert_no_error (error);
    g_assert_true (g_spawn_check_wait_status (wait_status, &error));
    g_assert_no_error (error);

    g_clear_pointer (&command, g_free);
    command = nautilus_terminal_build_remote_shell_command (remote_path, FALSE);
    {
        char *argv[] = { "/bin/sh", "-c", command, NULL };

        g_assert_true (g_spawn_sync (NULL,
                                     argv,
                                     environment,
                                     G_SPAWN_DEFAULT,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     &wait_status,
                                     &error));
    }
    g_assert_no_error (error);
    g_assert_true (g_spawn_check_wait_status (wait_status, &error));
    g_assert_no_error (error);

    g_assert_cmpint (g_rmdir (remote_path), ==, 0);
    g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);
}

static void
test_settings_ranges (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema =
        g_settings_schema_source_lookup (source,
                                         "org.gnome.nautilus.terminal",
                                         TRUE);
    g_autoptr (GSettingsSchemaKey) pane_size =
        g_settings_schema_get_key (schema, "pane-size");
    g_autoptr (GSettingsSchemaKey) font_size =
        g_settings_schema_get_key (schema, "font-size");

    g_assert_true (g_settings_schema_key_range_check (pane_size,
                                                       g_variant_new_int32 (100)));
    g_assert_true (g_settings_schema_key_range_check (pane_size,
                                                       g_variant_new_int32 (4096)));
    g_assert_false (g_settings_schema_key_range_check (pane_size,
                                                        g_variant_new_int32 (99)));
    g_assert_false (g_settings_schema_key_range_check (pane_size,
                                                        g_variant_new_int32 (4097)));

    g_assert_true (g_settings_schema_key_range_check (font_size,
                                                       g_variant_new_int32 (6)));
    g_assert_true (g_settings_schema_key_range_check (font_size,
                                                       g_variant_new_int32 (72)));
    g_assert_false (g_settings_schema_key_range_check (font_size,
                                                        g_variant_new_int32 (5)));
    g_assert_false (g_settings_schema_key_range_check (font_size,
                                                        g_variant_new_int32 (73)));
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add_func ("/terminal/title-sanitization", test_title_sanitization);
    g_test_add_func ("/terminal/ssh-identity", test_ssh_identity);
    g_test_add_func ("/terminal/remote-shell-command", test_remote_shell_command);
    g_test_add_func ("/terminal/settings-ranges", test_settings_ranges);

    return g_test_run ();
}
