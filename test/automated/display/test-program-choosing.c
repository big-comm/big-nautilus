/* test-program-choosing.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What Nautilus launches must not end up as part of Nautilus. In the cgroup
 * of Nautilus' own unit, a browser opened from a file -- forty processes,
 * four gigabytes -- is counted as Nautilus by the system monitor, outlives it
 * inside that unit, and is killed with it.
 */

#include <nautilus-program-choosing.h>

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <signal.h>

static char *
read_cgroup (int pid)
{
    g_autofree char *path = g_strdup_printf ("/proc/%d/cgroup", pid);
    char *contents = NULL;

    if (!g_file_get_contents (path, &contents, NULL, NULL))
    {
        return NULL;
    }

    return g_strstrip (contents);
}

/* The process running @marker among the children of this one, or 0. */
static int
find_child (const char *marker)
{
    g_autoptr (GDir) proc = g_dir_open ("/proc", 0, NULL);
    const char *name;

    while ((name = g_dir_read_name (proc)) != NULL)
    {
        g_autofree char *path = g_build_filename ("/proc", name, "cmdline", NULL);
        g_autofree char *cmdline = NULL;
        gsize length;

        if (!g_ascii_isdigit (*name) ||
            !g_file_get_contents (path, &cmdline, &length, NULL))
        {
            continue;
        }

        for (gsize i = 0; i + 1 < length; i++)
        {
            if (cmdline[i] == '\0')
            {
                cmdline[i] = ' ';
            }
        }

        if (strstr (cmdline, marker) != NULL)
        {
            return atoi (name);
        }
    }

    return 0;
}

static void
test_launched_application_gets_its_own_scope (void)
{
    g_autofree char *marker = g_strdup_printf ("nautilus-scope-test-%d", getpid ());
    g_autofree char *command = g_strdup_printf ("sh -c 'sleep 30; true %s'", marker);
    g_autofree char *own = read_cgroup (getpid ());
    g_autofree char *theirs = NULL;
    int pid = 0;

    if (own == NULL || !g_str_has_prefix (own, "0::/user.slice/"))
    {
        g_test_skip ("not running under a systemd user session");
        return;
    }

    nautilus_launch_application_from_command (gdk_display_get_default (), command, FALSE, NULL);

    /* The scope is asked for asynchronously. */
    for (guint i = 0; i < 300; i++)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (10 * 1000);

        if (pid == 0)
        {
            pid = find_child (marker);
        }

        if (pid != 0)
        {
            g_free (theirs);
            theirs = read_cgroup (pid);
            if (theirs != NULL && strstr (theirs, ".scope") != NULL)
            {
                break;
            }
        }
    }

    g_assert_cmpint (pid, >, 0);
    if (pid > 0)
    {
        kill (pid, SIGTERM);
    }

    g_assert_nonnull (theirs);
    g_assert_cmpstr (theirs, !=, own);
    g_assert_nonnull (strstr (theirs, "/app-gnome-"));
    g_assert_nonnull (strstr (theirs, ".scope"));
}

int
main (int   argc,
      char *argv[])
{
    gtk_test_init (&argc, &argv, NULL);

    g_test_add_func ("/program-choosing/launched-application-gets-its-own-scope",
                     test_launched_application_gets_its_own_scope);

    return g_test_run ();
}
