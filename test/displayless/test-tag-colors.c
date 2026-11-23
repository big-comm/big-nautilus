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

/* A folder takes its contents along, and they are tagged under their own
 * URIs: those have to follow, without touching a sibling that merely starts
 * with the same letters. */
static void
test_move_takes_folder_contents (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    nautilus_tag_colors_set_color (colors, "file:///home/u/dir", NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_set_color (colors, "file:///home/u/dir/a.txt", NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_set_color (colors, "file:///home/u/dir/sub/b.txt", NAUTILUS_TAG_COLOR_BLUE);
    nautilus_tag_colors_set_color (colors, "file:///home/u/dir-other/c.txt", NAUTILUS_TAG_COLOR_PURPLE);

    nautilus_tag_colors_move (colors, "file:///home/u/dir", "file:///home/u/moved");

    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/moved"),
                     ==, NAUTILUS_TAG_COLOR_RED);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/moved/a.txt"),
                     ==, NAUTILUS_TAG_COLOR_GREEN);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/moved/sub/b.txt"),
                     ==, NAUTILUS_TAG_COLOR_BLUE);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/dir/a.txt"),
                     ==, NAUTILUS_TAG_COLOR_NONE);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/dir-other/c.txt"),
                     ==, NAUTILUS_TAG_COLOR_PURPLE);

    /* The same move reported a second time, as a rename is -- once by the
     * operation, once by the file monitor. */
    nautilus_tag_colors_move (colors, "file:///home/u/dir", "file:///home/u/moved");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/moved/a.txt"),
                     ==, NAUTILUS_TAG_COLOR_GREEN);

    /* An untagged folder with tagged contents. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/plain/d.txt", NAUTILUS_TAG_COLOR_GRAY);
    nautilus_tag_colors_move (colors, "file:///home/u/plain", "file:///home/u/renamed");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/renamed/d.txt"),
                     ==, NAUTILUS_TAG_COLOR_GRAY);
}

/* Going to the trash removes the file, and with it the tag. Coming back --
 * by undo, or restored from the trash -- must bring the tag back. */
static void
test_restored_file_gets_its_tag_back (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    nautilus_tag_colors_set_color (colors, "file:///home/u/undone", NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_file_removed (colors, "file:///home/u/undone");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/undone"),
                     ==, NAUTILUS_TAG_COLOR_NONE);
    nautilus_tag_colors_file_restored (colors, "file:///home/u/undone");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/undone"),
                     ==, NAUTILUS_TAG_COLOR_RED);

    /* Restoring from the trash is a move out of it. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/trashed", NAUTILUS_TAG_COLOR_BLUE);
    nautilus_tag_colors_file_removed (colors, "file:///home/u/trashed");
    nautilus_tag_colors_move (colors, "trash:///trashed", "file:///home/u/trashed");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/trashed"),
                     ==, NAUTILUS_TAG_COLOR_BLUE);

    /* Only once: another file moved to that name later is another file. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/trashed", NAUTILUS_TAG_COLOR_NONE);
    nautilus_tag_colors_move (colors, "trash:///trashed", "file:///home/u/trashed");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/trashed"),
                     ==, NAUTILUS_TAG_COLOR_NONE);

    /* A file never seen going. */
    nautilus_tag_colors_file_restored (colors, "file:///home/u/stranger");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/stranger"),
                     ==, NAUTILUS_TAG_COLOR_NONE);

    /* A plain move must not resurrect a removed tag. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/deleted", NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_file_removed (colors, "file:///home/u/deleted");
    nautilus_tag_colors_move (colors, "file:///home/u/elsewhere", "file:///home/u/deleted");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/deleted"),
                     ==, NAUTILUS_TAG_COLOR_NONE);
}

/* An editor that keeps a backup renames the document to "name~" and writes
 * the new contents under the old name. The file monitor reports the rename,
 * and the tag must stay with the document. */
static void
test_backup_rename_keeps_tag (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    nautilus_tag_colors_set_color (colors, "file:///home/u/report.odt", NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_move (colors, "file:///home/u/report.odt", "file:///home/u/report.odt~");
    nautilus_tag_colors_move (colors, "file:///home/u/.goutputstream-X1", "file:///home/u/report.odt");

    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/report.odt"),
                     ==, NAUTILUS_TAG_COLOR_RED);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/report.odt~"),
                     ==, NAUTILUS_TAG_COLOR_NONE);

    /* Any other new name is a rename like the rest. */
    nautilus_tag_colors_move (colors, "file:///home/u/report.odt", "file:///home/u/report-final.odt");
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/report-final.odt"),
                     ==, NAUTILUS_TAG_COLOR_RED);
}

/* The count behind has_color() has to survive every way a tag can change. */
static void
test_has_color_follows_every_change (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();

    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));
    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_NONE));

    nautilus_tag_colors_set_color (colors, "file:///home/u/count/a", NAUTILUS_TAG_COLOR_BROWN);
    nautilus_tag_colors_set_color (colors, "file:///home/u/count/b", NAUTILUS_TAG_COLOR_BROWN);
    nautilus_tag_colors_set_color (colors, "file:///home/u/count/b", NAUTILUS_TAG_COLOR_BROWN);
    g_assert_true (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));

    /* Moved, with the folder: still two. */
    nautilus_tag_colors_move (colors, "file:///home/u/count", "file:///home/u/counted");
    nautilus_tag_colors_set_color (colors, "file:///home/u/counted/a", NAUTILUS_TAG_COLOR_NONE);
    g_assert_true (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));

    /* Recoloured: brown is free again. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/counted/b", NAUTILUS_TAG_COLOR_GRAY);
    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));

    /* Removed and restored. */
    nautilus_tag_colors_set_color (colors, "file:///home/u/counted/c", NAUTILUS_TAG_COLOR_BROWN);
    nautilus_tag_colors_file_removed (colors, "file:///home/u/counted/c");
    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));
    nautilus_tag_colors_file_restored (colors, "file:///home/u/counted/c");
    g_assert_true (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));
    nautilus_tag_colors_set_color (colors, "file:///home/u/counted/c", NAUTILUS_TAG_COLOR_NONE);
    g_assert_false (nautilus_tag_colors_has_color (colors, NAUTILUS_TAG_COLOR_BROWN));
}

static char *
uri_in (const char *dir,
        const char *name)
{
    g_autofree char *path = g_build_filename (dir, name, NULL);

    return g_filename_to_uri (path, NULL, NULL);
}

/* Tags of deleted files have to go at some point, but never the tag of a
 * file that is only out of reach: on a disk that is unplugged, the file is
 * missing just the same. */
static void
test_prune_drops_only_what_is_certainly_gone (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autofree char *root = g_dir_make_tmp ("nautilus-tag-prune-XXXXXX", NULL);
    g_autofree char *busy_dir = g_build_filename (root, "busy", NULL);
    g_autofree char *empty_dir = g_build_filename (root, "unmounted", NULL);
    g_autofree char *kept_path = g_build_filename (busy_dir, "kept.txt", NULL);
    g_autofree char *kept = uri_in (busy_dir, "kept.txt");
    g_autofree char *deleted = uri_in (busy_dir, "deleted.txt");
    g_autofree char *on_unmounted_disk = uri_in (empty_dir, "photo.jpg");
    g_autofree char *in_missing_folder = uri_in (root, "unplugged/dir/file.txt");
    const char *remote = "sftp://server.example/home/u/remote.txt";

    g_assert_cmpint (g_mkdir_with_parents (busy_dir, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (empty_dir, 0700), ==, 0);
    g_assert_true (g_file_set_contents (kept_path, "", 0, NULL));

    nautilus_tag_colors_set_color (colors, kept, NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_set_color (colors, deleted, NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_set_color (colors, on_unmounted_disk, NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_set_color (colors, in_missing_folder, NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_set_color (colors, remote, NAUTILUS_TAG_COLOR_GREEN);

    /* Everything else in the store at this point lives under /home/u, which
     * is not there: none of it may go either. */
    g_assert_cmpuint (nautilus_tag_colors_prune (colors), ==, 1);

    g_assert_cmpint (nautilus_tag_colors_get_color (colors, deleted), ==, NAUTILUS_TAG_COLOR_NONE);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, kept), ==, NAUTILUS_TAG_COLOR_GREEN);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, on_unmounted_disk), ==, NAUTILUS_TAG_COLOR_GREEN);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, in_missing_folder), ==, NAUTILUS_TAG_COLOR_GREEN);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, remote), ==, NAUTILUS_TAG_COLOR_GREEN);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, "file:///home/u/one"), ==, NAUTILUS_TAG_COLOR_BLUE);

    /* Nothing left to drop. */
    g_assert_cmpuint (nautilus_tag_colors_prune (colors), ==, 0);

    /* And a pruned file that comes back still gets its tag. */
    nautilus_tag_colors_file_restored (colors, deleted);
    g_assert_cmpint (nautilus_tag_colors_get_color (colors, deleted), ==, NAUTILUS_TAG_COLOR_GREEN);

    g_unlink (kept_path);
    g_rmdir (busy_dir);
    g_rmdir (empty_dir);
    g_rmdir (root);
}

/* One line GKeyFile cannot parse makes it reject the whole file. The store
 * then starts empty, and its next save must not be what destroys the tags
 * that were in there. Runs last: it replaces the file under the singleton. */
static void
test_unreadable_file_is_kept (void)
{
    g_autofree char *path = tags_conf_path ();
    g_autofree char *dir = g_path_get_dirname (path);
    g_autoptr (NautilusTagColors) fresh = NULL;
    g_autoptr (GDir) listing = NULL;
    g_autofree char *kept = NULL;
    g_autofree char *kept_contents = NULL;
    const char *damaged = "[tags]\nred=file:///home/u/precious;\nthis line has no equals sign\n";
    const char *name;

    nautilus_tag_colors_flush (nautilus_tag_colors_get ());
    write_tags_conf (damaged);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*Could not read tag colors*Kept as*");
    fresh = g_object_new (NAUTILUS_TYPE_TAG_COLORS, NULL);
    g_test_assert_expected_messages ();

    g_assert_false (nautilus_tag_colors_has_color (fresh, NAUTILUS_TAG_COLOR_RED));
    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

    listing = g_dir_open (dir, 0, NULL);
    while ((name = g_dir_read_name (listing)) != NULL)
    {
        if (g_str_has_prefix (name, "tags.conf.unreadable-"))
        {
            kept = g_build_filename (dir, name, NULL);
        }
    }

    g_assert_nonnull (kept);
    g_assert_true (g_file_get_contents (kept, &kept_contents, NULL, NULL));
    g_assert_cmpstr (kept_contents, ==, damaged);

    /* And saving from the empty state leaves it alone. */
    nautilus_tag_colors_set_color (fresh, "file:///home/u/new", NAUTILUS_TAG_COLOR_GREEN);
    nautilus_tag_colors_flush (fresh);
    g_clear_pointer (&kept_contents, g_free);
    g_assert_true (g_file_get_contents (kept, &kept_contents, NULL, NULL));
    g_assert_cmpstr (kept_contents, ==, damaged);
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
    g_test_add_func ("/tag-colors/move-takes-folder-contents", test_move_takes_folder_contents);
    g_test_add_func ("/tag-colors/restored-file-gets-its-tag-back", test_restored_file_gets_its_tag_back);
    g_test_add_func ("/tag-colors/backup-rename-keeps-tag", test_backup_rename_keeps_tag);
    g_test_add_func ("/tag-colors/has-color-follows-every-change", test_has_color_follows_every_change);
    g_test_add_func ("/tag-colors/prune-drops-only-what-is-certainly-gone",
                     test_prune_drops_only_what_is_certainly_gone);
    g_test_add_func ("/tag-colors/unreadable-file-is-kept", test_unreadable_file_is_kept);

    return g_test_run ();
}
