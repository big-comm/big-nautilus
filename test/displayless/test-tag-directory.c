/* test-tag-directory.c: the folder that lists the files of one colour tag
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>

#include "nautilus-directory.h"
#include "nautilus-file.h"
#include "nautilus-file-utilities.h"
#include "nautilus-tag-colors.h"
#include "nautilus-tag-directory.h"
#include "nautilus-tag-manager.h"

#define URI_ONE "file:///home/u/one"
#define URI_TWO "file:///home/u/two"
#define URI_THREE "file:///home/u/three"
#define URI_BLUE "file:///home/u/blue"

static NautilusDirectory *
directory_for (NautilusTagColor color)
{
    g_autofree char *uri = nautilus_tag_directory_get_uri_for_color (color);

    return nautilus_directory_get_by_uri (uri);
}

/* The URIs the directory lists, sorted. */
static GStrv
listed_uris (NautilusDirectory *directory)
{
    g_autolist (NautilusFile) files = nautilus_directory_get_file_list (directory);
    g_autoptr (GPtrArray) uris = g_ptr_array_new ();

    for (GList *l = files; l != NULL; l = l->next)
    {
        g_ptr_array_add (uris, nautilus_file_get_uri (l->data));
    }

    g_ptr_array_sort_values (uris, (GCompareFunc) g_strcmp0);
    g_ptr_array_add (uris, NULL);

    return (GStrv) g_ptr_array_free (g_steal_pointer (&uris), FALSE);
}

static void
assert_lists (NautilusDirectory  *directory,
              const char * const *expected)
{
    g_auto (GStrv) uris = listed_uris (directory);

    g_assert_cmpstrv (uris, expected);
}

static void
untag_everything (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    const char * const uris[] = { URI_ONE, URI_TWO, URI_THREE, URI_BLUE };

    for (guint i = 0; i < G_N_ELEMENTS (uris); i++)
    {
        nautilus_tag_colors_set_color (colors, uris[i], NAUTILUS_TAG_COLOR_NONE);
    }
}

static void
test_lists_its_colour_only (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autoptr (NautilusDirectory) red = NULL;
    g_autoptr (NautilusFile) one = nautilus_file_get_by_uri (URI_ONE);
    g_autoptr (NautilusFile) blue = nautilus_file_get_by_uri (URI_BLUE);

    nautilus_tag_colors_set_color (colors, URI_ONE, NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_set_color (colors, URI_TWO, NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_set_color (colors, URI_BLUE, NAUTILUS_TAG_COLOR_BLUE);

    red = directory_for (NAUTILUS_TAG_COLOR_RED);
    g_assert_true (NAUTILUS_IS_TAG_DIRECTORY (red));
    assert_lists (red, (const char * const []) { URI_ONE, URI_TWO, NULL });
    g_assert_true (nautilus_directory_contains_file (red, one));
    g_assert_false (nautilus_directory_contains_file (red, blue));
    g_assert_true (nautilus_directory_are_all_files_seen (red));
    g_assert_false (nautilus_directory_is_editable (red));

    nautilus_directory_force_reload (red);
    assert_lists (red, (const char * const []) { URI_ONE, URI_TWO, NULL });

    untag_everything ();
}

typedef struct
{
    guint added;
    guint changed;
} Emissions;

static void
on_files_added (NautilusDirectory *directory,
                GList             *files,
                Emissions         *emissions)
{
    emissions->added += g_list_length (files);
}

static void
on_files_changed (NautilusDirectory *directory,
                  GList             *files,
                  Emissions         *emissions)
{
    emissions->changed += g_list_length (files);
}

static void
test_follows_tagging (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autoptr (NautilusDirectory) red = NULL;
    Emissions emissions = { 0, 0 };

    nautilus_tag_colors_set_color (colors, URI_ONE, NAUTILUS_TAG_COLOR_RED);
    red = directory_for (NAUTILUS_TAG_COLOR_RED);
    nautilus_directory_force_reload (red);
    assert_lists (red, (const char * const []) { URI_ONE, NULL });

    g_signal_connect (red, "files-added", G_CALLBACK (on_files_added), &emissions);
    g_signal_connect (red, "files-changed", G_CALLBACK (on_files_changed), &emissions);

    nautilus_tag_colors_set_color (colors, URI_THREE, NAUTILUS_TAG_COLOR_RED);
    assert_lists (red, (const char * const []) { URI_ONE, URI_THREE, NULL });
    g_assert_cmpuint (emissions.added, ==, 1);

    /* Another colour's change is not this folder's business. */
    nautilus_tag_colors_set_color (colors, URI_BLUE, NAUTILUS_TAG_COLOR_BLUE);
    assert_lists (red, (const char * const []) { URI_ONE, URI_THREE, NULL });
    g_assert_cmpuint (emissions.added, ==, 1);

    /* Changing colour takes the file out, as removing the tag does. */
    nautilus_tag_colors_set_color (colors, URI_ONE, NAUTILUS_TAG_COLOR_BLUE);
    assert_lists (red, (const char * const []) { URI_THREE, NULL });
    nautilus_tag_colors_set_color (colors, URI_THREE, NAUTILUS_TAG_COLOR_NONE);
    assert_lists (red, (const char * const []) { NULL });
    g_assert_cmpuint (emissions.changed, >=, 2);

    g_signal_handlers_disconnect_by_data (red, &emissions);
    untag_everything ();
}

static void
on_monitor_ready (NautilusDirectory *directory,
                  GList             *files,
                  gpointer           user_data)
{
    guint *seen = user_data;

    *seen = g_list_length (files);
}

static void
test_monitor_reports_current_files (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autoptr (NautilusDirectory) red = NULL;
    guint seen = G_MAXUINT;
    guint ready = G_MAXUINT;

    nautilus_tag_colors_set_color (colors, URI_ONE, NAUTILUS_TAG_COLOR_RED);
    nautilus_tag_colors_set_color (colors, URI_TWO, NAUTILUS_TAG_COLOR_RED);
    red = directory_for (NAUTILUS_TAG_COLOR_RED);
    nautilus_directory_force_reload (red);

    nautilus_directory_file_monitor_add (red, &seen, TRUE, NAUTILUS_ATTRIBUTE_INFO,
                                         on_monitor_ready, &seen);
    g_assert_cmpuint (seen, ==, 2);

    nautilus_directory_call_when_ready (red, NAUTILUS_ATTRIBUTE_INFO,
                                        on_monitor_ready, &ready);
    g_assert_cmpuint (ready, ==, 2);
    nautilus_directory_cancel_callback (red, on_monitor_ready, &ready);

    /* A file taken in while monitored, then the monitor going away. */
    nautilus_tag_colors_set_color (colors, URI_THREE, NAUTILUS_TAG_COLOR_RED);
    nautilus_directory_file_monitor_remove (red, &seen);
    assert_lists (red, (const char * const []) { URI_ONE, URI_THREE, URI_TWO, NULL });

    untag_everything ();
}

static void
test_unknown_colour_is_empty (void)
{
    NautilusTagColors *colors = nautilus_tag_colors_get ();
    g_autoptr (NautilusDirectory) bogus = nautilus_directory_get_by_uri ("tag://bogus/");
    g_autoptr (NautilusFile) untagged = nautilus_file_get_by_uri (URI_TWO);

    nautilus_tag_colors_set_color (colors, URI_ONE, NAUTILUS_TAG_COLOR_RED);

    g_assert_true (NAUTILUS_IS_TAG_DIRECTORY (bogus));
    assert_lists (bogus, (const char * const []) { NULL });
    g_assert_false (nautilus_directory_contains_file (bogus, untagged));

    /* A bulk change is reported without a URI. */
    nautilus_tag_colors_move (colors, URI_ONE, URI_THREE);
    assert_lists (bogus, (const char * const []) { NULL });

    untag_everything ();
}

int
main (int   argc,
      char *argv[])
{
    g_autofree char *sandbox = g_dir_make_tmp ("nautilus-tag-directory-XXXXXX", NULL);
    g_autoptr (NautilusTagManager) tag_manager = NULL;

    g_assert_nonnull (sandbox);
    /* The store resolves its path from the data directory on first use. */
    g_setenv ("XDG_DATA_HOME", sandbox, TRUE);

    g_test_init (&argc, &argv, NULL);
    nautilus_ensure_extension_points ();
    nautilus_ensure_extension_builtins ();
    tag_manager = nautilus_tag_manager_new_dummy ();

    g_test_add_func ("/tag-directory/lists-its-colour-only", test_lists_its_colour_only);
    g_test_add_func ("/tag-directory/follows-tagging", test_follows_tagging);
    g_test_add_func ("/tag-directory/monitor-reports-current-files",
                     test_monitor_reports_current_files);
    g_test_add_func ("/tag-directory/unknown-colour-is-empty", test_unknown_colour_is_empty);

    return g_test_run ();
}
