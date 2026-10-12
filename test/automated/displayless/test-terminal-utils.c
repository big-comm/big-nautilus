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
    /* Says hello before the shell starts, which is how the widget learns
     * that ssh is done asking questions; each connection in its own words. */
    g_assert_nonnull (strstr (command, "]7;file:///#"));
    {
        g_autofree char *again = nautilus_terminal_build_remote_shell_command (remote_path, TRUE);

        g_assert_cmpstr (command, !=, again);
    }
    /* A '%' in a folder name must reach the widget as "%25". */
    g_assert_nonnull (strstr (command, "%/%25}"));
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

/* Has @shell echo each quoted string back and checks it comes out unchanged
 * and alone: nothing else may run. */
static void
assert_quoting_survives (const char *shell)
{
    const char * const paths[] =
    {
        "/plain",
        "/with space/and\ttab",
        "/it's",
        "/trailing backslash\\",
        "/a\\'; touch PWNED #",
        "/'\\''",
        "/$HOME/`id`/$(id)/!!/*?[x]",
        "/acentuação/日本語",
        "",
        NULL
    };

    for (guint i = 0; paths[i] != NULL; i++)
    {
        g_autofree char *quoted = nautilus_terminal_quote_path (paths[i]);
        g_autofree char *script = g_strdup_printf ("printf '%%s' %s", quoted);
        g_autofree char *output = NULL;
        g_autoptr (GError) error = NULL;
        const char *argv[] = { shell, "-c", script, NULL };
        int wait_status;

        g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, &output, NULL, &wait_status, &error));
        g_assert_no_error (error);
        g_assert_true (g_spawn_check_wait_status (wait_status, NULL));
        g_assert_cmpstr (output, ==, paths[i]);
    }

    g_assert_false (g_file_test ("PWNED", G_FILE_TEST_EXISTS));
}

static void
test_path_quoting (void)
{
    /* Whatever is installed. fish is the one that reads single quotes
     * differently, see nautilus_terminal_quote_path(). */
    const char * const shells[] = { "sh", "bash", "dash", "zsh", "fish", NULL };

    for (guint i = 0; shells[i] != NULL; i++)
    {
        g_autofree char *program = g_find_program_in_path (shells[i]);

        if (program != NULL)
        {
            g_test_message ("quoting checked with %s", program);
            assert_quoting_survives (program);
        }
    }
}

static void
test_typable_paths (void)
{
    g_assert_true (nautilus_terminal_path_is_typable ("/home/user/Área de Trabalho"));
    g_assert_true (nautilus_terminal_path_is_typable ("/it's; rm -rf ~"));
    /* Ctrl-C, newline, carriage return, escape, delete. */
    g_assert_false (nautilus_terminal_path_is_typable ("/x\003rm -rf ~"));
    g_assert_false (nautilus_terminal_path_is_typable ("/x\ny"));
    g_assert_false (nautilus_terminal_path_is_typable ("/x\ry"));
    g_assert_false (nautilus_terminal_path_is_typable ("/x\033[2Jy"));
    g_assert_false (nautilus_terminal_path_is_typable ("/x\177y"));
}

static void
test_ssh_fields (void)
{
    g_assert_true (nautilus_terminal_ssh_field_is_safe (NULL));
    g_assert_true (nautilus_terminal_ssh_field_is_safe (""));
    g_assert_true (nautilus_terminal_ssh_field_is_safe ("example.org"));
    g_assert_true (nautilus_terminal_ssh_field_is_safe ("fe80::1%eth0"));
    g_assert_true (nautilus_terminal_ssh_field_is_safe ("first.last"));
    /* Would be read by ssh as an option. */
    g_assert_false (nautilus_terminal_ssh_field_is_safe ("-oProxyCommand=id"));
    g_assert_false (nautilus_terminal_ssh_field_is_safe ("host name"));
    g_assert_false (nautilus_terminal_ssh_field_is_safe ("host;id"));
    g_assert_false (nautilus_terminal_ssh_field_is_safe ("host\nid"));
}

static void
assert_cwd_uri (const char *uri_string,
                const char *expected_path,
                const char *expected_host)
{
    /* The flags VTE parses OSC 7 with. */
    g_autoptr (GUri) uri = g_uri_parse (uri_string, G_URI_FLAGS_ENCODED, NULL);
    g_autofree char *host = NULL;
    g_autofree char *path = NULL;

    g_assert_nonnull (uri);
    path = nautilus_terminal_path_from_cwd_uri (uri, &host);

    g_assert_cmpstr (path, ==, expected_path);
    g_assert_cmpstr ((host != NULL && *host != '\0') ? host : NULL, ==, expected_host);
}

static void
test_cwd_uri (void)
{
    assert_cwd_uri ("file:///home/user", "/home/user", NULL);
    assert_cwd_uri ("file:///tmp/a%20b", "/tmp/a b", NULL);
    assert_cwd_uri ("file:///tmp/%C3%A1", "/tmp/á", NULL);
    assert_cwd_uri ("file://server.example/srv", "/srv", "server.example");
    /* A shell that encodes its path. */
    assert_cwd_uri ("file:///tmp/C%23/a%3Fb", "/tmp/C#/a?b", NULL);
    /* The hooks installed by the widget print the path as it is, and the
     * parser takes the name apart. */
    assert_cwd_uri ("file:///tmp/C#/src", "/tmp/C#/src", NULL);
    assert_cwd_uri ("file:///tmp/a?b", "/tmp/a?b", NULL);
    assert_cwd_uri ("file:///tmp/a?b#c%20d", "/tmp/a?b#c d", NULL);
    /* A folder named "nv 100%/a%20b#c", as the hooks of the widget report
     * it -- only the '%' encoded -- and as fish reports it on its own. */
    assert_cwd_uri ("file:///tmp/nv%20100%25/a%2520b#c", "/tmp/nv 100%/a%20b#c", NULL);
    assert_cwd_uri ("file://host.example/tmp/nv%20100%25/a%2520b%23c", "/tmp/nv 100%/a%20b#c", "host.example");
    /* Not a folder of anyone's. */
    assert_cwd_uri ("sftp://server.example/srv", NULL, NULL);
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
    g_test_add_func ("/terminal/path-quoting", test_path_quoting);
    g_test_add_func ("/terminal/typable-paths", test_typable_paths);
    g_test_add_func ("/terminal/ssh-fields", test_ssh_fields);
    g_test_add_func ("/terminal/cwd-uri", test_cwd_uri);
    g_test_add_func ("/terminal/settings-ranges", test_settings_ranges);

    return g_test_run ();
}
