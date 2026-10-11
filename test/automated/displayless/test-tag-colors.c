/* test-tag-colors.c: colour tag storage
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "nautilus-tag-colors.h"

/* The store resolves its path from g_get_user_data_dir() once, on first use,
 * so the sandbox has to be in place before any test touches it. */
static char *
tags_conf_path (void)
{
    return g_build_filename (g_get_user_data_dir (), "nautilus", "tags.conf", NULL);
}

static void
write_tags_conf (const char *contents)
{
    g_autofree char *path = tags_conf_path ();
    g_autofree char *dir = g_path_get_dirname (path);
    g_autoptr (GError) error = NULL;

    g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
    g_file_set_contents (path, contents, -1, &error);
    g_assert_no_error (error);
}

/* Tags used to be stored with the URI as the GKeyFile key. Those files are
 * still out there, so they must keep loading. */
static void
test_reads_legacy_format (void)
{
    NautilusTagColors *colors;

    write_tags_conf ("[tags]\n"
                     "file:///home/u/one=blue\n"
                     "file:///home/u/two=orange\n"
                     "\n"
                     "[names]\n"
                     "orange=Projects\n");

    colors = nautilus_tag_colors_get ();

    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/one"),
                     ==, NAUTILUS_TAG_COLOR_BLUE);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/two"),
                     ==, NAUTILUS_TAG_COLOR_ORANGE);
    g_assert_cmpstr (nautilus_tag_colors_get_display_name (colors, NAUTILUS_TAG_COLOR_ORANGE),
                     ==, "Projects");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/untagged"),
                     ==, NAUTILUS_TAG_COLOR_NONE);
}

/* GKeyFile rejects '=', '[' and ']' in key names, so storing the URI as a key
 * silently dropped those files on save. They belong on the value side. */
static void
test_persists_awkward_uris (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autofree char *path = tags_conf_path ();
    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    g_auto (GStrv) stored = NULL;
    gboolean found = FALSE;
    const char *awkward = "file:///home/u/report%5B1%5D=final.pdf";

    nautilus_tag_colors_set_color (colors, awkward, NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_flush (colors);

    g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, &error);
    g_assert_no_error (error);

    stored = g_key_file_get_string_list (keyfile, "tags", "red", NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (stored);

    for (guint i = 0; stored[i] != NULL; i++)
    {
        if (g_strcmp0 (stored[i], awkward) == 0)
        {
            found = TRUE;
        }
    }

    g_assert_true (found);
}

/* Saves are coalesced on a timeout. The store is a never-released singleton,
 * so finalize() never runs and flush() is what keeps a tag applied just before
 * quitting from being lost. */
static void
test_flush_writes_pending_changes (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autofree char *path = tags_conf_path ();
    g_autofree char *contents = NULL;

    nautilus_tag_colors_set_color (colors, "file:///home/u/fresh", NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_flush (colors);

    g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
    g_assert_nonnull (strstr (contents, "file:///home/u/fresh"));
}

static void
test_has_color (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    nautilus_tag_colors_set_color (colors, "file:///home/u/purple-one", NAUTILUS_TAG_COLOR_PURPLE);

    g_assert_true (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_PURPLE));
    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));
}

/* A rename must carry the tag over, instead of leaving it on a URI that no
 * longer exists. */
static void
test_move_follows_rename (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    nautilus_tag_colors_set_color (colors, "file:///home/u/before", NAUTILUS_TAG_COLOR_YELLOW);
    nautilus_tag_colors_move (colors, "file:///home/u/before", "file:///home/u/after");

    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/after"),
                     ==, NAUTILUS_TAG_COLOR_YELLOW);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/before"),
                     ==, NAUTILUS_TAG_COLOR_NONE);
}

int
main (int argc, char *argv[])
{
    g_autofree char *sandbox = g_dir_make_tmp ("nautilus-tag-colors-XXXXXX", NULL);

    g_assert_nonnull (sandbox);
    g_setenv ("XDG_DATA_HOME", sandbox, TRUE);

    g_test_init (&argc, &argv, NULL);

    /* Seeded before the singleton is first touched, so the legacy file is what
     * the store reads at startup. */
    write_tags_conf ("[tags]\n"
                     "file:///home/u/one=blue\n"
                     "file:///home/u/two=orange\n"
                     "\n"
                     "[names]\n"
                     "orange=Projects\n");

    g_test_add_func ("/tag-colors/reads-legacy-format", test_reads_legacy_format);
    g_test_add_func ("/tag-colors/persists-awkward-uris", test_persists_awkward_uris);
    g_test_add_func ("/tag-colors/flush-writes-pending-changes", test_flush_writes_pending_changes);
    g_test_add_func ("/tag-colors/has-color", test_has_color);
    g_test_add_func ("/tag-colors/move-follows-rename", test_move_follows_rename);

    return g_test_run ();
}
