/* test-search-engine-bigsearch.c
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exercises the BigSearch provider against a stand-in for the daemon that
 * lives in this process, on a socket inside a private runtime directory. The
 * real daemon is never involved: what it has indexed on the machine running
 * the suite must not decide whether the suite passes.
 *
 * To check against the real one, with this source tree inside what it indexes:
 *
 *   NAUTILUS_TEST_LIVE_BIGSEARCH=1 meson test -C builddir test-search-engine-bigsearch
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <src/nautilus-file.h>
#include <src/nautilus-global-preferences.h>
#include <src/nautilus-file-utilities.h>

#include "nautilus-search-engine.h"
#include "nautilus-search-engine-bigsearch.h"
#include "nautilus-search-hit.h"
#include "nautilus-search-provider.h"
#include "nautilus-query.h"

typedef enum
{
    /* Send the canned answer. */
    DAEMON_ANSWERS,
    /* Take the request and close without a word. */
    DAEMON_CLOSES,
    /* Take the request and never answer. */
    DAEMON_HANGS,
    /* Hand the answer out in pages of one, as a daemon that does not filter
     * by type itself would have to be read. */
    DAEMON_PAGES,
} DaemonMode;

static char *runtime_dir;
static char *scope_dir;
static NautilusFile *scope_file;
static GSocketService *daemon_service;
static DaemonMode daemon_mode;
static char *daemon_answer;
static char *last_request;
static guint n_requests;
static GStrv daemon_pages;
static GList *held_connections;

static char *
daemon_socket_path (void)
{
    return g_build_filename (runtime_dir, "biglinux", "indexd.sock", NULL);
}

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection,
             GObject           *source_object,
             gpointer           user_data)
{
    g_autoptr (GDataInputStream) input = NULL;
    GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

    input = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
    g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (input), FALSE);

    g_free (last_request);
    last_request = g_data_input_stream_read_line (input, NULL, NULL, NULL);
    n_requests++;

    switch (daemon_mode)
    {
        case DAEMON_ANSWERS:
        {
            g_output_stream_write_all (output, daemon_answer, strlen (daemon_answer),
                                       NULL, NULL, NULL);
            g_output_stream_write_all (output, "\n", 1, NULL, NULL, NULL);
        }
        break;

        case DAEMON_CLOSES:
        {
        }
        break;

        case DAEMON_HANGS:
        {
            held_connections = g_list_prepend (held_connections, g_object_ref (connection));
        }
        break;

        case DAEMON_PAGES:
        {
            guint n_pages = g_strv_length (daemon_pages);
            guint page = 0;
            const char *offset = (last_request != NULL) ? strstr (last_request, "\"offset\":") : NULL;
            g_autofree char *answer = NULL;

            if (offset != NULL)
            {
                page = (guint) g_ascii_strtoull (offset + strlen ("\"offset\":"), NULL, 10);
            }

            if (page < n_pages)
            {
                g_autofree char *next = (page + 1 < n_pages)
                                        ? g_strdup_printf ("%u", page + 1)
                                        : g_strdup ("null");

                answer = g_strdup_printf ("{\"kind\":\"query\",\"v\":2,\"hits\":[%s],"
                                          "\"total\":%u,\"next_offset\":%s}\n",
                                          daemon_pages[page], n_pages, next);
            }
            else
            {
                answer = g_strdup ("{\"kind\":\"query\",\"v\":2,\"hits\":[],\"next_offset\":null}\n");
            }

            g_output_stream_write_all (output, answer, strlen (answer), NULL, NULL, NULL);
        }
        break;
    }

    return TRUE;
}

static void
daemon_start (DaemonMode  mode,
              const char *answer)
{
    g_autofree char *path = daemon_socket_path ();
    g_autofree char *dir = g_path_get_dirname (path);
    g_autoptr (GSocketAddress) address = g_unix_socket_address_new (path);
    g_autoptr (GError) error = NULL;

    g_assert_null (daemon_service);

    g_mkdir_with_parents (dir, 0700);
    g_unlink (path);

    daemon_mode = mode;
    g_set_str (&daemon_answer, answer);
    g_clear_pointer (&last_request, g_free);
    n_requests = 0;

    daemon_service = g_socket_service_new ();
    g_socket_listener_add_address (G_SOCKET_LISTENER (daemon_service), address,
                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                   NULL, NULL, &error);
    g_assert_no_error (error);
    g_signal_connect (daemon_service, "incoming", G_CALLBACK (on_incoming), NULL);
}

/* @leave_socket: keep the file, as a daemon that crashed would. */
static void
daemon_stop (gboolean leave_socket)
{
    g_autofree char *path = daemon_socket_path ();

    g_list_free_full (g_steal_pointer (&held_connections), g_object_unref);

    if (daemon_service != NULL)
    {
        g_socket_service_stop (daemon_service);
        g_socket_listener_close (G_SOCKET_LISTENER (daemon_service));
        g_clear_object (&daemon_service);
    }

    if (!leave_socket)
    {
        g_unlink (path);
    }
}

static char *
answer_with_paths (const char * const *names)
{
    g_autoptr (GString) answer = g_string_new ("{\"kind\":\"hits\",\"hits\":[");

    for (guint i = 0; names[i] != NULL; i++)
    {
        g_autofree char *path = g_build_filename (scope_dir, names[i], NULL);

        g_string_append_printf (answer,
                                "%s{\"path\":\"%s\",\"mime\":\"text/plain\",\"score\":1.0,\"mtime\":1700000000}",
                                (i > 0) ? "," : "", path);
    }

    g_string_append (answer, "]}");

    return g_string_free (g_steal_pointer (&answer), FALSE);
}

typedef struct
{
    GMainLoop *loop;
    GList *uris;
    gboolean finished;
} Collector;

static void
on_hits_added (GObject   *source,
               GPtrArray *hits,
               gpointer   user_data)
{
    Collector *c = user_data;

    for (guint i = 0; i < hits->len; i++)
    {
        c->uris = g_list_prepend (c->uris,
                                  g_strdup (nautilus_search_hit_get_uri (g_ptr_array_index (hits, i))));
    }

    /* The provider hands the array over, the engine keeps its own. */
    if (NAUTILUS_IS_SEARCH_PROVIDER (source))
    {
        g_ptr_array_unref (hits);
    }
}

static void
on_finished (Collector *c)
{
    c->finished = TRUE;
    g_main_loop_quit (c->loop);
}

static void
on_bail_out (gpointer user_data)
{
    g_error ("The search did not finish in time");
}

static void
collector_run (Collector *c)
{
    guint bail_out_id = g_timeout_add_seconds_once (30, on_bail_out, NULL);

    if (!c->finished)
    {
        g_main_loop_run (c->loop);
    }

    g_source_remove (bail_out_id);
    g_assert_true (c->finished);
}

static void
collector_clear (Collector *c)
{
    g_list_free_full (c->uris, g_free);
    g_main_loop_unref (c->loop);
}

static gboolean
collector_has (Collector  *c,
               const char *name)
{
    g_autofree char *path = g_build_filename (scope_dir, name, NULL);
    g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);

    return g_list_find_custom (c->uris, uri, (GCompareFunc) g_strcmp0) != NULL;
}

static NautilusQuery *
query_new (const char *text)
{
    NautilusQuery *query = nautilus_query_new ();
    g_autoptr (GFile) location = g_file_new_for_path (scope_dir);

    nautilus_query_set_text (query, text);
    nautilus_query_set_location (query, location);
    nautilus_query_set_show_hidden_files (query, FALSE);

    return query;
}

/* Runs @query on a bare provider. Returns whether it asked for the fallback. */
static gboolean
provider_search (NautilusQuery *query,
                 Collector     *c)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();

    *c = (Collector) { .loop = g_main_loop_new (NULL, FALSE) };

    g_signal_connect (provider, "hits-added", G_CALLBACK (on_hits_added), c);
    g_signal_connect_swapped (provider, "provider-finished", G_CALLBACK (on_finished), c);

    g_assert_true (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));

    /* The round trip belongs to the search thread. Had it run here, the
     * provider would already be done. */
    g_assert_false (c->finished);

    collector_run (c);

    return nautilus_search_engine_bigsearch_needs_fallback (provider);
}

static void
test_absent_daemon_declines (void)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = query_new ("needle");

    g_assert_false (nautilus_search_engine_bigsearch_is_available ());
    g_assert_false (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));
}

static void
test_finds_and_scopes (void)
{
    const char * const names[] = { "needle.txt", "sub/needle-two.txt", NULL };
    g_autofree char *answer = answer_with_paths (names);
    g_autoptr (NautilusQuery) query = query_new ("needle");
    g_autofree char *under = g_strdup_printf ("\"under\":\"%s\"", scope_dir);
    Collector c;

    daemon_start (DAEMON_ANSWERS, answer);

    g_assert_false (provider_search (query, &c));
    g_assert_cmpuint (g_list_length (c.uris), ==, 2);
    g_assert_true (collector_has (&c, "needle.txt"));
    g_assert_true (collector_has (&c, "sub/needle-two.txt"));

    /* The directory filter runs inside the index, not over a larger result
     * set on our side. */
    g_assert_nonnull (last_request);
    g_assert_nonnull (strstr (last_request, under));
    g_assert_nonnull (strstr (last_request, "\"q\":\"needle\""));

    collector_clear (&c);
    daemon_stop (FALSE);
}

static void
test_hidden_files_are_left_out (void)
{
    const char * const names[] =
    {
        "needle.txt", ".needle-hidden", ".cache/needle.txt", "needle.txt~", NULL
    };
    g_autofree char *answer = answer_with_paths (names);
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c;

    daemon_start (DAEMON_ANSWERS, answer);

    g_assert_false (provider_search (query, &c));
    g_assert_cmpuint (g_list_length (c.uris), ==, 1);
    g_assert_true (collector_has (&c, "needle.txt"));
    collector_clear (&c);

    /* A search that wants them takes what the daemon has, and asks for the
     * walk as well: the index does not hold hidden files. */
    nautilus_query_set_show_hidden_files (query, TRUE);
    g_assert_true (provider_search (query, &c));
    g_assert_cmpuint (g_list_length (c.uris), ==, 4);
    collector_clear (&c);

    daemon_stop (FALSE);
}

static char *
hit_json (const char *name,
          const char *mime)
{
    g_autofree char *path = g_build_filename (scope_dir, name, NULL);

    return g_strdup_printf ("{\"path\":\"%s\",\"mime\":\"%s\",\"score\":1.0}", path, mime);
}

static NautilusQuery *
query_new_for_type (const char *text,
                    const char *mime_type)
{
    NautilusQuery *query = query_new (text);
    g_autoptr (GPtrArray) types = g_ptr_array_new_with_free_func (g_free);

    g_ptr_array_add (types, g_strdup (mime_type));
    nautilus_query_set_mime_types (query, types);

    return query;
}

/* The type wanted may be on a later page of what a daemon sends when it does
 * not filter by type itself. */
static void
test_follows_pages_for_a_type (void)
{
    g_autoptr (NautilusQuery) query = query_new_for_type ("needle", "application/pdf");
    char *pages[] =
    {
        hit_json ("needle.txt", "text/plain"),
        hit_json ("needle.c", "text/x-csrc"),
        hit_json ("needle.pdf", "application/pdf"),
        NULL
    };
    Collector c;

    daemon_pages = pages;
    daemon_start (DAEMON_PAGES, NULL);

    g_assert_false (provider_search (query, &c));
    g_assert_cmpuint (g_list_length (c.uris), ==, 1);
    g_assert_true (collector_has (&c, "needle.pdf"));
    g_assert_cmpuint (n_requests, ==, 3);

    /* And the daemon is told the type, so that one which can filter does. */
    g_assert_nonnull (strstr (last_request, "\"mime\":[\"application/pdf\"]"));

    collector_clear (&c);
    daemon_stop (FALSE);
    daemon_pages = NULL;

    for (guint i = 0; pages[i] != NULL; i++)
    {
        g_free (pages[i]);
    }
}

/* Without a type to look for, the first page is all the views can use. */
static void
test_one_page_without_a_type (void)
{
    g_autoptr (NautilusQuery) query = query_new ("needle");
    char *pages[] =
    {
        hit_json ("needle.txt", "text/plain"),
        hit_json ("needle.c", "text/x-csrc"),
        NULL
    };
    Collector c;

    daemon_pages = pages;
    daemon_start (DAEMON_PAGES, NULL);

    provider_search (query, &c);
    g_assert_cmpuint (g_list_length (c.uris), ==, 1);
    g_assert_cmpuint (n_requests, ==, 1);

    collector_clear (&c);
    daemon_stop (FALSE);
    daemon_pages = NULL;
    g_free (pages[0]);
    g_free (pages[1]);
}

static void
test_declines_what_the_index_lacks (void)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = query_new ("needle");
    g_autoptr (GFile) trash = g_file_new_for_uri ("trash:///");

    daemon_start (DAEMON_ANSWERS, "{\"kind\":\"hits\",\"hits\":[]}");

    nautilus_query_set_location (query, trash);
    g_assert_false (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));
    g_assert_null (last_request);

    daemon_stop (FALSE);
}

static void
assert_asks_for_fallback (DaemonMode  mode,
                          const char *answer)
{
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c;

    daemon_start (mode, answer);

    g_assert_true (provider_search (query, &c));
    g_assert_null (c.uris);

    collector_clear (&c);
    daemon_stop (FALSE);
}

static void
test_unusable_answers_ask_for_fallback (void)
{
    /* Each of these also warns, which is the point of them. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*");
    assert_asks_for_fallback (DAEMON_CLOSES, NULL);
    g_test_assert_expected_messages ();

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*");
    assert_asks_for_fallback (DAEMON_ANSWERS, "[]");
    g_test_assert_expected_messages ();

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*");
    assert_asks_for_fallback (DAEMON_ANSWERS, "{\"kind\":\"hits\",\"hits\":5}");
    g_test_assert_expected_messages ();

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*");
    assert_asks_for_fallback (DAEMON_ANSWERS, "not json at all");
    g_test_assert_expected_messages ();

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*version*");
    assert_asks_for_fallback (DAEMON_ANSWERS, "{\"kind\":\"error\",\"message\":\"unsupported version\"}");
    g_test_assert_expected_messages ();

    /* Malformed entries are skipped, not fatal; nothing found still asks. */
    assert_asks_for_fallback (DAEMON_ANSWERS, "{\"kind\":\"hits\",\"hits\":[7,{\"path\":\"relative\"},{}]}");
    assert_asks_for_fallback (DAEMON_ANSWERS, "{\"kind\":\"hits\",\"hits\":[]}");
}

static void
test_stale_socket_asks_for_fallback (void)
{
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c;

    daemon_start (DAEMON_ANSWERS, "{}");
    daemon_stop (TRUE);

    g_assert_true (nautilus_search_engine_bigsearch_is_available ());
    g_assert_true (provider_search (query, &c));
    g_assert_null (c.uris);

    collector_clear (&c);
    daemon_stop (FALSE);
}

static void
stop_provider (gpointer user_data)
{
    nautilus_search_provider_stop (NAUTILUS_SEARCH_PROVIDER (user_data));
}

static void
test_stop_interrupts_a_silent_daemon (void)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c = { .loop = g_main_loop_new (NULL, FALSE) };
    gint64 started = g_get_monotonic_time ();

    daemon_start (DAEMON_HANGS, NULL);

    g_signal_connect (provider, "hits-added", G_CALLBACK (on_hits_added), &c);
    g_signal_connect_swapped (provider, "provider-finished", G_CALLBACK (on_finished), &c);

    g_assert_true (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));
    g_timeout_add_once (200, stop_provider, provider);
    collector_run (&c);

    /* Well short of the socket timeout: the stop is what ended it. */
    g_assert_cmpint (g_get_monotonic_time () - started, <, 3 * G_USEC_PER_SEC);
    /* And a search the user gave up on is not handed to the walk. */
    g_assert_false (nautilus_search_engine_bigsearch_needs_fallback (provider));

    collector_clear (&c);
    daemon_stop (FALSE);
}

/* Runs @query on the whole engine, with the index and the walk enabled. */
static void
engine_search (NautilusQuery *query,
               Collector     *c)
{
    g_autoptr (NautilusSearchEngine) engine =
        nautilus_search_engine_new (NAUTILUS_SEARCH_TYPE_BIGSEARCH | NAUTILUS_SEARCH_TYPE_SIMPLE);

    *c = (Collector) { .loop = g_main_loop_new (NULL, FALSE) };

    g_signal_connect (engine, "hits-added", G_CALLBACK (on_hits_added), c);
    g_signal_connect_swapped (engine, "search-finished", G_CALLBACK (on_finished), c);

    nautilus_search_engine_start (engine, query);
    collector_run (c);
}

static void
test_engine_walks_when_the_index_fails (void)
{
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c;

    /* No daemon at all: the walk starts straight away. */
    engine_search (query, &c);
    g_assert_true (collector_has (&c, "needle.txt"));
    g_assert_true (collector_has (&c, "sub/needle-two.txt"));
    collector_clear (&c);

    /* A daemon that takes the search and drops it. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*BigSearch engine*");
    daemon_start (DAEMON_CLOSES, NULL);
    engine_search (query, &c);
    g_test_assert_expected_messages ();
    g_assert_true (collector_has (&c, "needle.txt"));
    g_assert_true (collector_has (&c, "sub/needle-two.txt"));
    collector_clear (&c);
    daemon_stop (FALSE);

    /* A daemon that knows nothing about this folder. */
    daemon_start (DAEMON_ANSWERS, "{\"kind\":\"hits\",\"hits\":[]}");
    engine_search (query, &c);
    g_assert_true (collector_has (&c, "needle.txt"));
    collector_clear (&c);
    daemon_stop (FALSE);
}

static void
test_engine_trusts_an_answering_index (void)
{
    /* Only one of the two files on disk: had the walk run as well, the other
     * would show up too. */
    const char * const names[] = { "needle.txt", NULL };
    g_autofree char *answer = answer_with_paths (names);
    g_autoptr (NautilusQuery) query = query_new ("needle");
    Collector c;

    daemon_start (DAEMON_ANSWERS, answer);
    engine_search (query, &c);

    g_assert_cmpuint (g_list_length (c.uris), ==, 1);
    g_assert_true (collector_has (&c, "needle.txt"));

    collector_clear (&c);
    daemon_stop (FALSE);
}

/* The tests below talk to the daemon of the session, see the top of the file. */

static void
test_live_finds_this_file (void)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    g_autoptr (GFile) scope = g_file_new_for_path (g_test_get_dir (G_TEST_DIST));
    g_autoptr (NautilusFile) scope_nautilus_file = nautilus_file_get (scope);
    g_autofree char *this_file = g_test_build_filename (G_TEST_DIST, "test-search-engine-bigsearch.c", NULL);
    g_autofree char *this_uri = g_filename_to_uri (this_file, NULL, NULL);
    Collector c = { .loop = g_main_loop_new (NULL, FALSE) };

    g_assert_true (nautilus_search_engine_bigsearch_is_available ());

    nautilus_query_set_text (query, "search engine bigsearch");
    nautilus_query_set_location (query, scope);
    nautilus_query_set_show_hidden_files (query, FALSE);

    g_signal_connect (provider, "hits-added", G_CALLBACK (on_hits_added), &c);
    g_signal_connect_swapped (provider, "provider-finished", G_CALLBACK (on_finished), &c);
    g_assert_true (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));
    collector_run (&c);

    g_assert_false (nautilus_search_engine_bigsearch_needs_fallback (provider));
    g_assert_nonnull (g_list_find_custom (c.uris, this_uri, (GCompareFunc) g_strcmp0));

    /* The folder asked for is a filter in the index, not a hint. */
    for (GList *l = c.uris; l != NULL; l = l->next)
    {
        g_autoptr (GFile) hit = g_file_new_for_uri (l->data);

        g_assert_true (g_file_has_prefix (hit, scope));
    }

    collector_clear (&c);
}

static void
test_live_filters_by_type (void)
{
    g_autoptr (NautilusSearchEngineBigsearch) provider = nautilus_search_engine_bigsearch_new ();
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    g_autoptr (GPtrArray) types = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GFile) scope = g_file_new_for_path (g_test_get_dir (G_TEST_DIST));
    g_autoptr (NautilusFile) scope_nautilus_file = nautilus_file_get (scope);
    Collector c = { .loop = g_main_loop_new (NULL, FALSE) };

    /* There is a meson.build next to the tests, and it is no C source. */
    g_ptr_array_add (types, g_strdup ("text/x-csrc"));
    nautilus_query_set_mime_types (query, types);
    nautilus_query_set_text (query, "test");
    nautilus_query_set_location (query, scope);

    g_signal_connect (provider, "hits-added", G_CALLBACK (on_hits_added), &c);
    g_signal_connect_swapped (provider, "provider-finished", G_CALLBACK (on_finished), &c);
    g_assert_true (nautilus_search_provider_start (NAUTILUS_SEARCH_PROVIDER (provider), query));
    collector_run (&c);

    g_assert_nonnull (c.uris);
    for (GList *l = c.uris; l != NULL; l = l->next)
    {
        g_assert_true (g_str_has_suffix (l->data, ".c"));
    }

    collector_clear (&c);
}

/* A folder the daemon does not index: the engine as a whole still finds the
 * file, by walking. */
static void
test_live_engine_covers_what_is_not_indexed (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *outside = g_dir_make_tmp ("nautilus-bigsearch-live-XXXXXX", &error);
    g_autofree char *file = g_build_filename (outside, "needle-outside-the-index.txt", NULL);
    g_autofree char *uri = g_filename_to_uri (file, NULL, NULL);
    g_autoptr (GFile) scope = g_file_new_for_path (outside);
    g_autoptr (NautilusFile) scope_nautilus_file = nautilus_file_get (scope);
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    Collector c;

    g_assert_no_error (error);
    g_assert_true (g_file_set_contents (file, "", 0, NULL));

    nautilus_query_set_text (query, "needle-outside");
    nautilus_query_set_location (query, scope);
    engine_search (query, &c);

    g_assert_nonnull (g_list_find_custom (c.uris, uri, (GCompareFunc) g_strcmp0));

    collector_clear (&c);
    g_unlink (file);
    g_rmdir (outside);
}

static void
make_file (const char *name)
{
    g_autofree char *path = g_build_filename (scope_dir, name, NULL);
    g_autofree char *dir = g_path_get_dirname (path);

    g_mkdir_with_parents (dir, 0700);
    g_assert_true (g_file_set_contents (path, "", 0, NULL));
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (GError) error = NULL;
    int result;

    if (g_getenv ("NAUTILUS_TEST_LIVE_BIGSEARCH") != NULL)
    {
        g_test_init (&argc, &argv, NULL);
        nautilus_ensure_extension_points ();
        nautilus_global_preferences_init ();

        g_test_add_func ("/bigsearch/live/finds-this-file", test_live_finds_this_file);
        g_test_add_func ("/bigsearch/live/filters-by-type", test_live_filters_by_type);
        g_test_add_func ("/bigsearch/live/engine-covers-what-is-not-indexed",
                         test_live_engine_covers_what_is_not_indexed);

        return g_test_run ();
    }

    /* Before anything asks GLib for the runtime directory, which it caches. */
    runtime_dir = g_dir_make_tmp ("nautilus-bigsearch-XXXXXX", &error);
    g_assert_no_error (error);
    g_setenv ("XDG_RUNTIME_DIR", runtime_dir, TRUE);

    g_test_init (&argc, &argv, NULL);

    /* nautilus-query.c reads GSettings, and the tests are not installed, so
     * the schema has to be pointed at the build tree. Same dance as the other
     * displayless tests. */
    nautilus_ensure_extension_points ();
    nautilus_global_preferences_init ();

    scope_dir = g_build_filename (runtime_dir, "scope", NULL);
    make_file ("needle.txt");
    make_file ("sub/needle-two.txt");

    /* A query only searches the subfolders of a place it knows to be local,
     * and it knows that from the NautilusFile, as long as there is one. */
    {
        g_autoptr (GFile) scope = g_file_new_for_path (scope_dir);

        scope_file = nautilus_file_get (scope);
    }

    g_test_add_func ("/bigsearch/absent-daemon-declines", test_absent_daemon_declines);
    g_test_add_func ("/bigsearch/finds-and-scopes", test_finds_and_scopes);
    g_test_add_func ("/bigsearch/hidden-files-are-left-out", test_hidden_files_are_left_out);
    g_test_add_func ("/bigsearch/follows-pages-for-a-type", test_follows_pages_for_a_type);
    g_test_add_func ("/bigsearch/one-page-without-a-type", test_one_page_without_a_type);
    g_test_add_func ("/bigsearch/declines-what-the-index-lacks", test_declines_what_the_index_lacks);
    g_test_add_func ("/bigsearch/unusable-answers-ask-for-fallback", test_unusable_answers_ask_for_fallback);
    g_test_add_func ("/bigsearch/stale-socket-asks-for-fallback", test_stale_socket_asks_for_fallback);
    g_test_add_func ("/bigsearch/stop-interrupts-a-silent-daemon", test_stop_interrupts_a_silent_daemon);
    g_test_add_func ("/bigsearch/engine-walks-when-the-index-fails", test_engine_walks_when_the_index_fails);
    g_test_add_func ("/bigsearch/engine-trusts-an-answering-index", test_engine_trusts_an_answering_index);

    result = g_test_run ();

    daemon_stop (FALSE);
    nautilus_file_unref (scope_file);

    return result;
}
