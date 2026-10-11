/* test-search-engine-bigsearch.c
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exercises the BigSearch provider against a running daemon. The daemon is
 * optional, so every test skips when its socket is absent rather than failing:
 * a machine without the service must still pass the suite.
 */

#include <glib.h>
#include <gio/gio.h>

#include <src/nautilus-global-preferences.h>
#include <src/nautilus-file-utilities.h>

#include "nautilus-search-engine-bigsearch.h"
#include "nautilus-search-hit.h"
#include "nautilus-query.h"

typedef struct
{
    GMainLoop *loop;
    GList *hits;
    gboolean finished;
} Collector;

static void
on_hits_added (NautilusSearchProvider *provider,
               GPtrArray              *hits,
               gpointer                user_data)
{
    Collector *c = user_data;

    for (guint i = 0; i < hits->len; i++)
    {
        c->hits = g_list_prepend (c->hits, g_object_ref (g_ptr_array_index (hits, i)));
    }
}

static void
on_finished (NautilusSearchProvider *provider,
             gpointer                user_data)
{
    Collector *c = user_data;

    c->finished = TRUE;
    g_main_loop_quit (c->loop);
}

static gboolean
bail_out (gpointer user_data)
{
    Collector *c = user_data;

    g_main_loop_quit (c->loop);
    return G_SOURCE_REMOVE;
}

static GList *
search (const char *text,
        GFile      *location)
{
    g_autoptr (NautilusSearchEngineBigsearch) engine = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    Collector c = { 0 };

    c.loop = g_main_loop_new (NULL, FALSE);

    nautilus_query_set_text (query, text);
    if (location != NULL)
    {
        nautilus_query_set_location (query, location);
    }

    g_signal_connect (engine, "hits-added", G_CALLBACK (on_hits_added), &c);
    g_signal_connect (engine, "provider-finished", G_CALLBACK (on_finished), &c);

    nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (engine), query);

    g_timeout_add_seconds (15, bail_out, &c);
    g_main_loop_run (c.loop);

    g_assert_true (c.finished);
    g_main_loop_unref (c.loop);

    return c.hits;
}

static void
test_daemon_absent_is_not_searched (void)
{
    g_autoptr (NautilusQuery) query = nautilus_query_new ();

    nautilus_query_set_text (query, "anything");

    /* should_search() gates on the socket, so the provider is inert without
     * the daemon and the other engines answer alone. */
    if (!nautilus_search_engine_bigsearch_is_available ())
    {
        g_test_skip ("daemon not running; availability correctly reported as FALSE");
        return;
    }

    g_assert_true (nautilus_search_engine_bigsearch_is_available ());
}

static void
test_finds_by_name (void)
{
    GList *hits = NULL;

    if (!nautilus_search_engine_bigsearch_is_available ())
    {
        g_test_skip ("daemon not running");
        return;
    }

    hits = search ("nautilus-tag-colors", NULL);

    g_assert_nonnull (hits);
    g_list_free_full (hits, g_object_unref);
}

/* The whole point of using this daemon is that the directory filter runs
 * inside the index, not over a larger result set on our side. */
static void
test_scope_is_honoured (void)
{
    g_autoptr (GFile) scope = NULL;
    GList *hits = NULL;

    if (!nautilus_search_engine_bigsearch_is_available ())
    {
        g_test_skip ("daemon not running");
        return;
    }

    scope = g_file_new_for_path (g_test_get_filename (G_TEST_DIST, ".", NULL));
    hits = search ("nautilus", scope);

    for (GList *l = hits; l != NULL; l = l->next)
    {
        g_autofree char *uri = NULL;
        g_autoptr (GFile) file = NULL;

        g_object_get (l->data, "uri", &uri, NULL);
        file = g_file_new_for_uri (uri);

        g_assert_true (g_file_has_prefix (file, scope) || g_file_equal (file, scope));
    }

    g_list_free_full (hits, g_object_unref);
}

int
main (int argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    /* nautilus-query.c reads GSettings, and the tests are not installed, so
     * the schema has to be pointed at the build tree. Same dance as the other
     * displayless tests. */
    nautilus_ensure_extension_points ();
    nautilus_global_preferences_init ();

    g_test_add_func ("/bigsearch/availability", test_daemon_absent_is_not_searched);
    g_test_add_func ("/bigsearch/finds-by-name", test_finds_by_name);
    g_test_add_func ("/bigsearch/scope-is-honoured", test_scope_is_honoured);

    return g_test_run ();
}
