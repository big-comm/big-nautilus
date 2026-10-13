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

/* Only walked when the mime filter is on. A daemon with the "mime-filter"
 * capability applies it in the index and the first page is all there is to
 * want. One without it ignores the filter, which leaves it to this side, and
 * then the first page alone could hold nothing of the wanted type. */
#define BIGSEARCH_MAX_PAGES 10

/* A sane answer is a few hundred kilobytes. */
#define BIGSEARCH_MAX_RESPONSE_BYTES (64 * 1024 * 1024)

struct _NautilusSearchEngineBigsearch
{
    NautilusSearchProvider parent_instance;

    /* Written by the search thread before it hands over to the main context,
     * read from the main context afterwards. */
    gboolean needs_fallback;
    guint n_hits;
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

gboolean
nautilus_search_engine_bigsearch_needs_fallback (NautilusSearchEngineBigsearch *self)
{
    g_return_val_if_fail (NAUTILUS_IS_SEARCH_ENGINE_BIGSEARCH (self), FALSE);

    return self->needs_fallback;
}

static const char *
get_name (NautilusSearchProvider *provider)
{
    return "bigsearch";
}

static gboolean
run_in_thread (NautilusSearchProvider *provider)
{
    return TRUE;
}

/* Declining hands the search to the directory-walking engine, so decline
 * whenever the index cannot give the answer the query asks for. */
static gboolean
should_search (NautilusSearchProvider *provider,
               NautilusQuery          *query)
{
    g_autofree char *text = nautilus_query_get_text (query);
    g_autoptr (GFile) location = nautilus_query_get_location (query);
    g_autoptr (GPtrArray) date_range = nautilus_query_get_date_range (query);

    if (text == NULL || *text == '\0')
    {
        return FALSE;
    }

    /* The index is scoped by local path: the trash, recent files, tags and
     * remote mounts are not in it. */
    if (location != NULL)
    {
        g_autofree char *path = g_file_get_path (location);

        if (path == NULL || !g_file_is_native (location))
        {
            return FALSE;
        }
    }

    /* "under" always includes the subfolders. A search with no location has
     * no subfolders to speak of, and takes the whole index. */
    if (location != NULL && !nautilus_query_recursive (query))
    {
        return FALSE;
    }

    /* Only the modification time is indexed. */
    if (date_range != NULL &&
        nautilus_query_get_search_type (query) != NAUTILUS_SEARCH_TIME_TYPE_LAST_MODIFIED)
    {
        return FALSE;
    }

    return nautilus_search_engine_bigsearch_is_available ();
}

/* Builds the request. Everything the query carries that the daemon understands
 * is pushed down into it, so the filtering happens in the index instead of
 * over a larger result set here. */
static char *
build_request (NautilusQuery *query,
               const char    *under,
               gint64         offset)
{
    g_autoptr (JsonBuilder) builder = json_builder_new ();
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autoptr (JsonNode) root = NULL;
    g_autofree char *text = nautilus_query_get_text (query);
    g_autofree char *mime_types = nautilus_query_get_mime_type_str (query);
    g_autoptr (GPtrArray) date_range = nautilus_query_get_date_range (query);

    json_builder_begin_object (builder);

    json_builder_set_member_name (builder, "v");
    json_builder_add_int_value (builder, BIGSEARCH_PROTOCOL_VERSION);

    json_builder_set_member_name (builder, "cmd");
    json_builder_add_string_value (builder, "query");

    json_builder_set_member_name (builder, "q");
    json_builder_add_string_value (builder, text);

    /* When the search is to look inside the files as well, the daemon does
     * that too, from the same index: this provider then stands in for the
     * LocalSearch one, which the engine leaves out. */
    json_builder_set_member_name (builder, "mode");
    json_builder_add_string_value (builder,
                                   nautilus_query_get_search_content (query) ? "both" : "name");

    json_builder_set_member_name (builder, "filter");
    json_builder_begin_object (builder);

    if (under != NULL)
    {
        json_builder_set_member_name (builder, "under");
        json_builder_add_string_value (builder, under);
    }

    if (mime_types != NULL)
    {
        g_auto (GStrv) types = g_strsplit (mime_types, ",", -1);

        json_builder_set_member_name (builder, "mime");
        json_builder_begin_array (builder);
        for (guint i = 0; types[i] != NULL; i++)
        {
            json_builder_add_string_value (builder, types[i]);
        }
        json_builder_end_array (builder);
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
    json_builder_add_int_value (builder, offset);
    json_builder_set_member_name (builder, "limit");
    json_builder_add_int_value (builder, BIGSEARCH_RESULT_LIMIT);
    json_builder_end_object (builder);

    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    json_generator_set_root (generator, root);

    return json_generator_to_data (generator, NULL);
}

/* The daemon answers with one JSON object on one line. Runs in the search
 * thread: every step here can block for as long as the socket timeout. */
static char *
round_trip (const char    *request,
            GCancellable  *cancellable,
            GError       **error)
{
    g_autofree char *path = socket_path ();
    g_autoptr (GSocketClient) client = NULL;
    g_autoptr (GSocketAddress) address = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    g_autoptr (GString) line = g_string_new (NULL);
    GOutputStream *output;
    GInputStream *input;

    if (path == NULL)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "No runtime directory");
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

    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));

    while (TRUE)
    {
        char buffer[16384];
        gssize n_read = g_input_stream_read (input, buffer, sizeof (buffer),
                                             cancellable, error);
        const char *newline;

        if (n_read < 0)
        {
            return NULL;
        }

        if (n_read == 0)
        {
            /* Closed before a full line came through. */
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                 "Connection closed before the answer was complete");
            return NULL;
        }

        newline = memchr (buffer, '\n', n_read);
        if (newline != NULL)
        {
            g_string_append_len (line, buffer, newline - buffer);
            break;
        }

        g_string_append_len (line, buffer, n_read);

        if (line->len > BIGSEARCH_MAX_RESPONSE_BYTES)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                                 "Answer too large");
            return NULL;
        }
    }

    return g_string_free (g_steal_pointer (&line), FALSE);
}

/* Mirrors what the directory walk hides: dot files, backup files, and
 * everything inside a hidden folder below the place searched. */
static gboolean
path_is_hidden (const char *path,
                const char *under)
{
    const char *relative = path;
    gsize length;

    if (under != NULL && g_str_has_prefix (path, under))
    {
        relative = path + strlen (under);
    }

    for (const char *p = relative; *p != '\0'; p++)
    {
        if (*p == '.' && p > path && p[-1] == '/')
        {
            return TRUE;
        }
    }

    length = strlen (path);

    return length > 0 && path[length - 1] == '~';
}

/* Returns: where the next page starts, 0 when this was the last one, or -1
 * with @error set. */
static gint64
add_hits_from_response (NautilusSearchEngineBigsearch *self,
                        const char                    *response,
                        const char                    *under,
                        GError                       **error)
{
    g_autoptr (JsonParser) parser = json_parser_new ();
    JsonNode *root_node;
    JsonNode *hits_node;
    JsonObject *root;
    JsonArray *hits;
    const char *kind;
    NautilusQuery *query = nautilus_search_provider_get_query (self);
    gboolean show_hidden = nautilus_query_get_show_hidden_files (query);
    gboolean filter_mime = nautilus_query_has_mime_types (query);

    if (!json_parser_load_from_data (parser, response, -1, error))
    {
        return -1;
    }

    root_node = json_parser_get_root (parser);
    if (root_node == NULL || !JSON_NODE_HOLDS_OBJECT (root_node))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Answer is not a JSON object");
        return -1;
    }

    root = json_node_get_object (root_node);
    kind = json_object_get_string_member_with_default (root, "kind", "");

    if (g_strcmp0 (kind, "error") == 0)
    {
        /* Most likely a protocol version mismatch, which the daemon spells
         * out. Report it rather than silently returning nothing. */
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                     json_object_get_string_member_with_default (root, "message",
                                                                 "unknown error"));
        return -1;
    }

    hits_node = json_object_get_member (root, "hits");
    if (hits_node == NULL)
    {
        return 0;
    }

    if (JSON_NODE_HOLDS_NULL (hits_node))
    {
        return 0;
    }

    if (!JSON_NODE_HOLDS_ARRAY (hits_node))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "\"hits\" is not an array");
        return -1;
    }

    hits = json_node_get_array (hits_node);

    for (guint i = 0; i < json_array_get_length (hits); i++)
    {
        JsonNode *entry_node = json_array_get_element (hits, i);
        JsonObject *entry;
        const char *path;
        const char *mime;
        g_autoptr (GFile) file = NULL;
        g_autofree char *uri = NULL;
        g_autoptr (NautilusSearchHit) hit = NULL;
        gint64 mtime;

        if (!JSON_NODE_HOLDS_OBJECT (entry_node))
        {
            continue;
        }

        entry = json_node_get_object (entry_node);

        /* A hit whose file has gone away, or lives on something unmounted,
         * would only turn into a broken row. */
        if (!json_object_get_boolean_member_with_default (entry, "available", TRUE))
        {
            continue;
        }

        path = json_object_get_string_member_with_default (entry, "path", NULL);
        if (path == NULL || !g_path_is_absolute (path))
        {
            continue;
        }

        if (!show_hidden && path_is_hidden (path, under))
        {
            continue;
        }

        /* Asked of the daemon too, but an older one does not listen. It
         * reports the type, which saves querying the file. */
        mime = json_object_get_string_member_with_default (entry, "mime", NULL);
        if (filter_mime &&
            (mime == NULL || !nautilus_query_matches_mime_type (query, mime)))
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
        self->n_hits++;
    }

    /* null on the last page. */
    return MAX (json_object_get_int_member_with_default (root, "next_offset", 0), 0);
}

/* Whether the daemon has an index to speak of, and @under in it. Asked only
 * after a search that came back empty. Any doubt counts as no: the price is a
 * directory walk on a search that had nothing to show. */
static gboolean
index_covers (const char   *under,
              GCancellable *cancellable)
{
    g_autofree char *status = round_trip ("{\"v\":" G_STRINGIFY (BIGSEARCH_PROTOCOL_VERSION) ",\"cmd\":\"status\"}",
                                          cancellable, NULL);
    g_autofree char *sources = NULL;
    g_autoptr (JsonParser) status_parser = json_parser_new ();
    g_autoptr (JsonParser) sources_parser = json_parser_new ();
    JsonNode *node;
    JsonNode *roots_node;
    JsonArray *roots;

    if (status == NULL || !json_parser_load_from_data (status_parser, status, -1, NULL))
    {
        return FALSE;
    }

    /* Nothing indexed yet: the first run after the daemon was installed. */
    node = json_parser_get_root (status_parser);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node) ||
        json_object_get_int_member_with_default (json_node_get_object (node), "indexed", 0) <= 0)
    {
        return FALSE;
    }

    if (under == NULL)
    {
        /* A search of everything, which is whatever the index holds. */
        return TRUE;
    }

    sources = round_trip ("{\"v\":" G_STRINGIFY (BIGSEARCH_PROTOCOL_VERSION) ",\"cmd\":\"sources\"}",
                          cancellable, NULL);
    if (sources == NULL || !json_parser_load_from_data (sources_parser, sources, -1, NULL))
    {
        return FALSE;
    }

    node = json_parser_get_root (sources_parser);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
        return FALSE;
    }

    roots_node = json_object_get_member (json_node_get_object (node), "roots");
    if (roots_node == NULL || !JSON_NODE_HOLDS_ARRAY (roots_node))
    {
        return FALSE;
    }

    roots = json_node_get_array (roots_node);
    for (guint i = 0; i < json_array_get_length (roots); i++)
    {
        JsonNode *root_node = json_array_get_element (roots, i);
        JsonObject *root;
        const char *path;
        gsize length;

        if (!JSON_NODE_HOLDS_OBJECT (root_node))
        {
            continue;
        }

        root = json_node_get_object (root_node);
        path = json_object_get_string_member_with_default (root, "path", NULL);
        if (path == NULL || *path != '/' ||
            !json_object_get_boolean_member_with_default (root, "available", TRUE))
        {
            continue;
        }

        length = strlen (path);
        while (length > 1 && path[length - 1] == '/')
        {
            length--;
        }

        if (strncmp (under, path, length) == 0 &&
            (length == 1 || under[length] == '\0' || under[length] == '/'))
        {
            return TRUE;
        }
    }

    return FALSE;
}

static gpointer
search_thread_func (NautilusSearchEngineBigsearch *self)
{
    NautilusQuery *query = nautilus_search_provider_get_query (self);
    GCancellable *cancellable = nautilus_search_provider_get_cancellable (self);
    g_autoptr (GFile) location = nautilus_query_get_location (query);
    g_autofree char *under = (location != NULL) ? g_file_get_path (location) : NULL;
    g_autoptr (GError) error = NULL;
    /* Without the mime filter the first page is the whole answer the views
     * can use. */
    guint max_pages = nautilus_query_has_mime_types (query) ? BIGSEARCH_MAX_PAGES : 1;
    gint64 offset = 0;

    for (guint page = 0; page < max_pages; page++)
    {
        g_autofree char *request = build_request (query, under, offset);
        g_autofree char *response = round_trip (request, cancellable, &error);
        gint64 next_offset;

        if (response == NULL)
        {
            break;
        }

        next_offset = add_hits_from_response (self, response, under, &error);
        if (next_offset <= offset)
        {
            /* The last page, an error, or a cursor that does not advance. */
            break;
        }

        offset = next_offset;
    }

    if (error == NULL)
    {
        /* An answer is the answer, also when it lists little: the other
         * engines are slower by orders of magnitude, and running them next
         * to this one makes the search exactly as slow as they are. The one
         * thing an empty answer cannot tell apart is "no such file" from "I
         * do not know that folder" -- so that is asked. */
        self->needs_fallback = (self->n_hits == 0 &&
                                !index_covers (under, cancellable));
    }
    else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        /* Whatever went wrong, the search was not answered: say so, and the
         * engine puts the directory walk on it instead. */
        self->needs_fallback = TRUE;

        /* A socket file left behind by a daemon that is no longer running is
         * an ordinary state, not a fault. Anything else -- a timeout, a
         * protocol mismatch, malformed JSON -- is worth saying out loud. */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
            g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
            g_debug ("BigSearch daemon not answering: %s", error->message);
        }
        else
        {
            g_warning ("BigSearch engine: %s", error->message);
        }
    }

    nautilus_search_provider_flush_hits (self);
    g_idle_add_once ((GSourceOnceFunc) nautilus_search_provider_finished, self);

    return NULL;
}

static void
start_search (NautilusSearchProvider *provider)
{
    NautilusSearchEngineBigsearch *self = NAUTILUS_SEARCH_ENGINE_BIGSEARCH (provider);
    g_autoptr (GThread) thread = NULL;

    self->needs_fallback = FALSE;
    self->n_hits = 0;

    /* The provider holds a reference on itself until it reports finished. */
    thread = g_thread_new ("nautilus-search-bigsearch",
                           (GThreadFunc) search_thread_func, self);
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
