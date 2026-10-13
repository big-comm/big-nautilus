/* test-view-model.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the view model does when the contents of a view are replaced, which a
 * search does on every letter typed: items that are in both the old and the
 * new contents stay where they are, so that GTK keeps their rows.
 */

#include <nautilus-file.h>
#include <nautilus-file-utilities.h>
#include <nautilus-global-preferences.h>
#include <nautilus-resources.h>
#include <nautilus-tag-manager.h>
#include <nautilus-view-item.h>
#include <nautilus-view-model.h>

#include <gtk/gtk.h>

/* Files are sorted by name, unless this says to turn the order round. */
static gboolean reverse_order;
static guint n_changes;

static NautilusViewItem *
item_at (NautilusViewModel *model,
         guint              position)
{
    g_autoptr (GtkTreeListRow) row = g_list_model_get_item (G_LIST_MODEL (model), position);

    return gtk_tree_list_row_get_item (row);
}

/* The model hands its sorter the items, not the rows GTK wraps them in. */
static int
compare_items (gconstpointer a,
               gconstpointer b,
               gpointer      user_data)
{
    g_autofree char *uri_a = nautilus_file_get_uri (nautilus_view_item_get_file (NAUTILUS_VIEW_ITEM ((gpointer) a)));
    g_autofree char *uri_b = nautilus_file_get_uri (nautilus_view_item_get_file (NAUTILUS_VIEW_ITEM ((gpointer) b)));
    int result = g_strcmp0 (uri_a, uri_b);

    return reverse_order ? -result : result;
}

static void
on_items_changed (void)
{
    n_changes++;
}

static NautilusViewModel *
model_new (void)
{
    NautilusViewModel *model = nautilus_view_model_new (FALSE);
    g_autoptr (GtkCustomSorter) sorter = gtk_custom_sorter_new (compare_items, NULL, NULL);

    nautilus_view_model_set_sorter (model, GTK_SORTER (sorter));
    g_signal_connect (model, "items-changed", G_CALLBACK (on_items_changed), NULL);
    reverse_order = FALSE;

    return model;
}

static NautilusFile *
file_named (const char *name)
{
    g_autofree char *uri = g_strconcat ("file:///nautilus-test-view-model/", name, NULL);

    return nautilus_file_get_by_uri (uri);
}

/* Adds an item for each of @names, a NULL-terminated list, as the view does:
 * with items made for the occasion. */
static void
add_named (NautilusViewModel *model,
           ...)
{
    g_autolist (NautilusViewItem) items = NULL;
    const char *name;
    va_list names;

    va_start (names, model);
    while ((name = va_arg (names, const char *)) != NULL)
    {
        g_autoptr (NautilusFile) file = file_named (name);

        items = g_list_prepend (items, nautilus_view_item_new (file));
    }
    va_end (names);

    nautilus_view_model_add_items (model, items);
}

/* Checks that the model holds exactly the files of @names, in that order. */
static void
assert_contents (NautilusViewModel *model,
                 ...)
{
    const char *name;
    guint position = 0;
    va_list names;

    va_start (names, model);
    while ((name = va_arg (names, const char *)) != NULL)
    {
        g_autoptr (NautilusFile) file = file_named (name);
        g_autoptr (NautilusViewItem) item = NULL;

        g_assert_cmpuint (position, <, g_list_model_get_n_items (G_LIST_MODEL (model)));
        item = item_at (model, position);
        g_assert_true (nautilus_view_item_get_file (item) == file);
        /* And the model knows it by its file. */
        g_assert_true (nautilus_view_model_get_item_for_file (model, file) == item);
        position++;
    }
    va_end (names);

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, position);
}

static void
test_replacing_keeps_what_stays (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();
    g_autoptr (NautilusFile) gone = file_named ("a");
    g_autoptr (NautilusViewItem) item_b = NULL;
    g_autoptr (NautilusViewItem) item_d = NULL;
    g_autoptr (GtkTreeListRow) row_b = NULL;
    g_autoptr (NautilusViewItem) item_b_after = NULL;
    g_autoptr (NautilusViewItem) item_d_after = NULL;
    g_autoptr (GtkTreeListRow) row_b_after = NULL;

    add_named (model, "a", "b", "c", "d", NULL);
    assert_contents (model, "a", "b", "c", "d", NULL);

    item_b = item_at (model, 1);
    item_d = item_at (model, 3);
    row_b = g_list_model_get_item (G_LIST_MODEL (model), 1);

    nautilus_view_model_defer_removal (model);
    /* Still on display, but no longer what the model stands for. */
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 4);
    g_assert_null (nautilus_view_model_get_item_for_file (model, gone));

    add_named (model, "e", "d", "b", NULL);
    assert_contents (model, "b", "d", "e", NULL);
    g_assert_null (nautilus_view_model_get_item_for_file (model, gone));

    /* The same objects all the way to what GTK sees: that is what lets it
     * leave their rows alone. */
    item_b_after = item_at (model, 0);
    item_d_after = item_at (model, 1);
    row_b_after = g_list_model_get_item (G_LIST_MODEL (model), 0);
    g_assert_true (item_b_after == item_b);
    g_assert_true (item_d_after == item_d);
    g_assert_true (row_b_after == row_b);
}

static void
test_replacing_everything (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();

    add_named (model, "a", "b", NULL);
    nautilus_view_model_defer_removal (model);

    n_changes = 0;
    add_named (model, "y", "x", NULL);
    assert_contents (model, "x", "y", NULL);
    /* Out and in at once, not as an emptying followed by a filling. */
    g_assert_cmpuint (n_changes, ==, 1);
}

static void
test_replacing_with_the_same (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();

    add_named (model, "a", "b", "c", NULL);
    nautilus_view_model_defer_removal (model);

    n_changes = 0;
    add_named (model, "c", "b", "a", NULL);
    assert_contents (model, "a", "b", "c", NULL);
    /* Nothing went and nothing came. */
    g_assert_cmpuint (n_changes, ==, 0);
}

/* What a file is sorted by may change with the search: how well it matches
 * what was typed. Items that stay must end up where they now belong. */
static void
test_replacing_sorts_again (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();

    add_named (model, "a", "b", "c", NULL);
    nautilus_view_model_defer_removal (model);

    reverse_order = TRUE;
    add_named (model, "a", "b", "c", "d", NULL);
    assert_contents (model, "d", "c", "b", "a", NULL);
}

static void
test_removal_with_nothing_to_replace_it (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();
    g_autoptr (NautilusFile) file = file_named ("a");

    add_named (model, "a", "b", NULL);
    nautilus_view_model_defer_removal (model);
    nautilus_view_model_finish_removal (model);

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 0);
    g_assert_null (nautilus_view_model_get_item_for_file (model, file));

    /* Nothing left to finish. */
    add_named (model, "c", NULL);
    nautilus_view_model_finish_removal (model);
    assert_contents (model, "c", NULL);
}

static void
test_additions_after_the_replacement (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();

    add_named (model, "a", "b", NULL);
    nautilus_view_model_defer_removal (model);
    add_named (model, "b", NULL);
    /* More of the same results, arriving later: added, not swapped in. */
    add_named (model, "f", "e", NULL);
    assert_contents (model, "b", "e", "f", NULL);

    nautilus_view_model_remove_all_items (model);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 0);
}

/* The results of a search come from as many folders as there are results. */
static void
test_adding_from_many_folders_is_one_change (void)
{
    g_autoptr (NautilusViewModel) model = model_new ();
    g_autolist (NautilusViewItem) items = NULL;

    for (guint i = 0; i < 200; i++)
    {
        g_autofree char *name = g_strdup_printf ("folder-%03u/file", i);
        g_autoptr (NautilusFile) file = file_named (name);

        items = g_list_prepend (items, nautilus_view_item_new (file));
    }

    n_changes = 0;
    nautilus_view_model_add_items (model, items);

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 200);
    g_assert_cmpuint (n_changes, ==, 1);
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (NautilusTagManager) tag_manager = NULL;

    gtk_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    nautilus_register_resource ();
    nautilus_ensure_extension_points ();
    nautilus_global_preferences_init ();
    tag_manager = nautilus_tag_manager_new_dummy ();

    g_test_add_func ("/view-model/replacing-keeps-what-stays", test_replacing_keeps_what_stays);
    g_test_add_func ("/view-model/replacing-everything", test_replacing_everything);
    g_test_add_func ("/view-model/replacing-with-the-same", test_replacing_with_the_same);
    g_test_add_func ("/view-model/replacing-sorts-again", test_replacing_sorts_again);
    g_test_add_func ("/view-model/removal-with-nothing-to-replace-it",
                     test_removal_with_nothing_to_replace_it);
    g_test_add_func ("/view-model/additions-after-the-replacement", test_additions_after_the_replacement);
    g_test_add_func ("/view-model/adding-from-many-folders-is-one-change",
                     test_adding_from_many_folders_is_one_change);

    return g_test_run ();
}
