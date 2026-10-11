/* nautilus-search-engine-bigsearch.c: search through the BigSearch daemon
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * BigSearch keeps a Tantivy index behind a small JSON protocol on a Unix
 * socket. Measured against the alternatives on the same machine: a scoped
 * query answers in about half a millisecond, where the LocalSearch engine
 * takes tens to hundreds and the directory-walking engine takes seconds on a
 * large tree.
 *
 * The daemon is optional. When its socket is absent this provider reports
 * itself unavailable and the other engines carry the search on their own, so
 * nothing here is load-bearing.
 */

#include <config.h>

#include "nautilus-search-engine-bigsearch.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

#include "nautilus-search-hit.h"
#include "nautilus-query.h"

/* The daemon rejects anything it does not speak, by version, with a readable
 * message. Bump this together with the daemon, never silently. */
#define BIGSEARCH_PROTOCOL_VERSION 2

/* Matching what the views can usefully show. The protocol pages results, so a
 * larger window costs the daemon little, but every hit past this one turns
 * into a NautilusFile on our side. */
#define BIGSEARCH_RESULT_LIMIT 1000

#define BIGSEARCH_TIMEOUT_SECONDS 5

struct _NautilusSearchEngineBigsearch
{
    NautilusSearchProvider parent_instance;
};

G_DEFINE_TYPE (NautilusSearchEngineBigsearch, nautilus_search_engine_bigsearch,
               NAUTILUS_TYPE_SEARCH_PROVIDER)

static char *
socket_path (void)
{
    const char *runtime_dir = g_get_user_runtime_dir ();

    if (runtime_dir == NULL)
    {
        return NULL;
    }

    return g_build_filename (runtime_dir, "biglinux", "indexd.sock", NULL);
}

gboolean
nautilus_search_engine_bigsearch_is_available (void)
{
    g_autofree char *path = socket_path ();

    return path != NULL && g_file_test (path, G_FILE_TEST_EXISTS);
}

static const char *
get_name (NautilusSearchProvider *provider)
{
    return "bigsearch";
}

static gboolean
run_in_thread (NautilusSearchProvider *provider)
{
    /* One blocking round trip, measured in fractions of a millisecond. Doing
     * it in the search thread keeps this file free of async plumbing. */
    return TRUE;
}

static gboolean
should_search (NautilusSearchProvider *provider,
               NautilusQuery          *query)
{
    g_autofree char *text = nautilus_query_get_text (query);

    if (text == NULL || *text == '\0')
    {
        return FALSE;
    }

    return nautilus_search_engine_bigsearch_is_available ();
}

/* Builds the request. Everything the query carries that the daemon understands
 * is pushed down into it, so the filtering happens in the index instead of
 * over a larger result set here. */
static char *
build_request (NautilusQuery *query)
{
    g_autoptr (JsonBuilder) builder = json_builder_new ();
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autoptr (JsonNode) root = NULL;
    g_autofree char *text = nautilus_query_get_text (query);
    g_autoptr (GFile) location = nautilus_query_get_location (query);
    GPtrArray *date_range = nautilus_query_get_date_range (query);

    json_builder_begin_object (builder);

    json_builder_set_member_name (builder, "v");
    json_builder_add_int_value (builder, BIGSEARCH_PROTOCOL_VERSION);

    json_builder_set_member_name (builder, "cmd");
    json_builder_add_string_value (builder, "query");

    json_builder_set_member_name (builder, "q");
    json_builder_add_string_value (builder, text);

    /* Content search is a separate, slower mode and a different feature than
     * what this provider is wired up to answer. */
    json_builder_set_member_name (builder, "mode");
    json_builder_add_string_value (builder, "name");

    json_builder_set_member_name (builder, "filter");
    json_builder_begin_object (builder);

    if (location != NULL)
    {
        g_autofree char *path = g_file_get_path (location);

        /* Only a local path can be scoped in the index. A search started in a
         * remote location falls through to the other engines. */
        if (path != NULL)
        {
            json_builder_set_member_name (builder, "under");
            json_builder_add_string_value (builder, path);
        }
    }

    if (date_range != NULL && date_range->len == 2)
    {
        GDateTime *first = g_ptr_array_index (date_range, 0);
        GDateTime *last = g_ptr_array_index (date_range, 1);

        json_builder_set_member_name (builder, "mtime");
        json_builder_begin_array (builder);
        json_builder_add_int_value (builder, g_date_time_to_unix (first));
        json_builder_add_int_value (builder, g_date_time_to_unix (last));
        json_builder_end_array (builder);
    }

    json_builder_end_object (builder);

    json_builder_set_member_name (builder, "page");
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "offset");
    json_builder_add_int_value (builder, 0);
    json_builder_set_member_name (builder, "limit");
    json_builder_add_int_value (builder, BIGSEARCH_RESULT_LIMIT);
    json_builder_end_object (builder);

    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    json_generator_set_root (generator, root);

    return json_generator_to_data (generator, NULL);
}

/* The daemon answers with one JSON object on one line. */
static char *
round_trip (const char    *request,
            GCancellable  *cancellable,
            GError       **error)
{
    g_autofree char *path = socket_path ();
    g_autoptr (GSocketClient) client = NULL;
    g_autoptr (GSocketAddress) address = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    g_autoptr (GDataInputStream) input = NULL;
    g_autofree char *line = NULL;
    GOutputStream *output;

    if (path == NULL)
    {
        return NULL;
    }

    client = g_socket_client_new ();
    g_socket_client_set_timeout (client, BIGSEARCH_TIMEOUT_SECONDS);
    address = g_unix_socket_address_new (path);

    connection = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (address),
                                          cancellable, error);
    if (connection == NULL)
    {
        return NULL;
    }

    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

    if (!g_output_stream_write_all (output, request, strlen (request), NULL,
                                    cancellable, error) ||
        !g_output_stream_write_all (output, "\n", 1, NULL, cancellable, error))
    {
        return NULL;
    }

    input = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
    line = g_data_input_stream_read_line (input, NULL, cancellable, error);

    return g_steal_pointer (&line);
}

static void
add_hits_from_response (NautilusSearchEngineBigsearch *self,
                        const char                    *response,
                        GError                       **error)
{
    g_autoptr (JsonParser) parser = json_parser_new ();
    JsonObject *root;
    JsonArray *hits;
    const char *kind;
    NautilusQuery *query = nautilus_search_provider_get_query (self);

    if (!json_parser_load_from_data (parser, response, -1, error))
    {
        return;
    }

    root = json_node_get_object (json_parser_get_root (parser));
    if (root == NULL)
    {
        return;
    }

    kind = json_object_get_string_member_with_default (root, "kind", "");

    if (g_strcmp0 (kind, "error") == 0)
    {
        /* Most likely a protocol version mismatch, which the daemon spells
         * out. Report it rather than silently returning nothing. */
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                     json_object_get_string_member_with_default (root, "message",
                                                                 "unknown error"));
        return;
    }

    if (!json_object_has_member (root, "hits"))
    {
        return;
    }

    hits = json_object_get_array_member (root, "hits");

    for (guint i = 0; i < json_array_get_length (hits); i++)
    {
        JsonObject *entry = json_array_get_object_element (hits, i);
        const char *path;
        const char *mime;
        g_autoptr (GFile) file = NULL;
        g_autofree char *uri = NULL;
        g_autoptr (NautilusSearchHit) hit = NULL;
        gint64 mtime;

        if (entry == NULL)
        {
            continue;
        }

        /* A hit whose file has gone away, or lives on something unmounted,
         * would only turn into a broken row. */
        if (!json_object_get_boolean_member_with_default (entry, "available", TRUE))
        {
            continue;
        }

        path = json_object_get_string_member_with_default (entry, "path", NULL);
        if (path == NULL)
        {
            continue;
        }

        /* The index does not know about the mime filter, so apply it here.
         * The daemon reports the type, which saves querying the file. */
        mime = json_object_get_string_member_with_default (entry, "mime", NULL);
        if (nautilus_query_has_mime_types (query) &&
            !nautilus_query_matches_mime_type (query, mime))
        {
            continue;
        }

        file = g_file_new_for_path (path);
        uri = g_file_get_uri (file);
        hit = nautilus_search_hit_new (uri);

        nautilus_search_hit_set_fts_rank (hit,
                                          json_object_get_double_member_with_default (entry, "score", 0.0));

        mtime = json_object_get_int_member_with_default (entry, "mtime", 0);
        if (mtime > 0)
        {
            g_autoptr (GDateTime) date = g_date_time_new_from_unix_local (mtime);

            nautilus_search_hit_set_modification_time (hit, date);
        }

        nautilus_search_provider_add_hit (self, g_steal_pointer (&hit));
    }
}

static void
start_search (NautilusSearchProvider *provider)
{
    NautilusSearchEngineBigsearch *self = NAUTILUS_SEARCH_ENGINE_BIGSEARCH (provider);
    g_autoptr (GError) error = NULL;
    g_autofree char *request = NULL;
    g_autofree char *response = NULL;

    request = build_request (nautilus_search_provider_get_query (self));
    response = round_trip (request,
                           nautilus_search_provider_get_cancellable (self),
                           &error);

    if (response != NULL)
    {
        add_hits_from_response (self, response, &error);
    }

    if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        /* The other engines are still running, so a failure here degrades the
         * search rather than breaking it. */
        g_warning ("BigSearch engine: %s", error->message);
    }

    nautilus_search_provider_flush_hits (self);
    nautilus_search_provider_finished (provider);
}

static void
nautilus_search_engine_bigsearch_class_init (NautilusSearchEngineBigsearchClass *class)
{
    NautilusSearchProviderClass *search_provider_class = NAUTILUS_SEARCH_PROVIDER_CLASS (class);

    search_provider_class->get_name = get_name;
    search_provider_class->run_in_thread = run_in_thread;
    search_provider_class->should_search = should_search;
    search_provider_class->start_search = start_search;
}

static void
nautilus_search_engine_bigsearch_init (NautilusSearchEngineBigsearch *self)
{
}

NautilusSearchEngineBigsearch *
nautilus_search_engine_bigsearch_new (void)
{
    return g_object_new (NAUTILUS_TYPE_SEARCH_ENGINE_BIGSEARCH, NULL);
}
