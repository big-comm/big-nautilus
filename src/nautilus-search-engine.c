/*
 * Copyright (C) 2005 Novell, Inc.
 * Copyright © 2025 The Files contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Author: Anders Carlsson <andersca@imendio.com>
 * Author: Peter Eisenmann <p3732@getgoogleoff.me>
 */
#define G_LOG_DOMAIN "nautilus-search"

#include <config.h>
#include "nautilus-search-engine.h"

#include "nautilus-file-utilities.h"
#include "nautilus-query.h"
#include "nautilus-search-engine-model.h"
#include "nautilus-search-engine-bigsearch.h"
#include "nautilus-search-engine-localsearch.h"
#include "nautilus-search-engine-recent.h"
#include "nautilus-search-engine-simple.h"
#include "nautilus-search-hit.h"
#include "nautilus-search-provider.h"

#include <glib/gi18n.h>

struct _NautilusSearchEngine
{
    GObject parent_instance;

    NautilusSearchType search_type;

    NautilusSearchProvider *localsearch;
    NautilusSearchProvider *bigsearch;
    NautilusSearchProvider *model;
    NautilusSearchProvider *recent;
    NautilusSearchProvider *simple;

    GHashTable *uris;
    guint providers_started;
    guint providers_finished;

    NautilusQuery *query;
    gboolean running;
    gboolean starting;
    gboolean restart;
    gboolean stopped;

    guint n_hits;
    guint limit_idle_id;

    /* Hits past the first ones, waiting to see whether the search is still
     * the one wanted a moment from now. */
    GPtrArray *held;
    guint n_handed_over;
    guint held_timeout_id;
    gboolean paced;
};

enum
{
    PROP_0,
    PROP_RUNNING,
    PROP_SEARCH_TYPE,
    N_PROPERTIES
};
static GParamSpec *properties[N_PROPERTIES];

enum
{
    HITS_ADDED,
    SEARCH_FINISHED,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE (NautilusSearchEngine, nautilus_search_engine, G_TYPE_OBJECT)

/* What a search hands to the view at most. Every hit becomes a file object, a
 * row in the model and a pass through the sort, all in the main loop: a
 * single letter typed in a home folder matched 66926 entries of the directory
 * walk and kept the window from answering for 6.8 seconds, long enough for
 * the session to offer to kill it. Nobody reads past the first screens of a
 * search; they type another letter. */
#define SEARCH_ENGINE_MAX_HITS 1000

/* What a search hands over at once, and how long it sits on the rest.
 *
 * While someone is typing, each letter is a search of its own that replaces
 * the one before, and the view pays for every result it is handed twice:
 * once to build a cell for it, once to tear that down a letter later. GTK
 * builds cells for some 640 items of a grid whatever the size of the window,
 * which made a thousand results cost 200 ms of frozen window per letter
 * typed -- on results nobody had the time to look at.
 *
 * So the first ones go out as they come, enough to fill what can be seen, and
 * the rest follow once the search has stood for a moment, which is to say
 * once the typing has paused. Only for an engine that was asked to, see
 * nautilus_search_engine_set_paced(). */
#define SEARCH_ENGINE_FIRST_HITS 60
#define SEARCH_ENGINE_HOLD_MS 500

static void
check_providers_status (NautilusSearchEngine *self);
static void
search_engine_drop_held (NautilusSearchEngine *self);
static gboolean
search_engine_hand_over_held (gpointer user_data);

/* Whether every provider is done and the search is only still open for the
 * hits it is sitting on. */
static gboolean
search_engine_waits_for_held_only (NautilusSearchEngine *self)
{
    return self->running && !self->starting &&
           self->providers_finished >= self->providers_started &&
           self->held_timeout_id != 0;
}

static void
search_engine_start_provider (NautilusSearchProvider *provider,
                              NautilusSearchEngine   *self)
{
    if (provider == NULL)
    {
        return;
    }
    else if (nautilus_search_provider_start (provider, self->query))
    {
        self->providers_started++;
    }
}

/* The ones that go to the disk or to another process, and take their time. */
static void
search_engine_start_slow_providers (NautilusSearchEngine *self)
{
    search_engine_start_provider (self->localsearch, self);
    search_engine_start_provider (self->simple, self);
}

/* Starts @provider and reports whether it took the search on. A provider
 * declines when it has nothing to offer for this query -- the BigSearch one
 * does so when its daemon is not there, or for what its index does not cover. */
static gboolean
search_engine_start_provider_if (NautilusSearchProvider *provider,
                                 NautilusSearchEngine   *self)
{
    guint before = self->providers_started;

    search_engine_start_provider (provider, self);

    return self->providers_started > before;
}

static void
search_engine_start_real (NautilusSearchEngine *self)
{
    g_return_if_fail (self->running);

    self->providers_started = 0;
    self->providers_finished = 0;
    self->stopped = FALSE;

    self->n_hits = 0;
    self->n_handed_over = 0;
    g_clear_handle_id (&self->limit_idle_id, g_source_remove);
    search_engine_drop_held (self);

    self->starting = TRUE;
    /* These two answer from memory. */
    search_engine_start_provider (self->model, self);
    search_engine_start_provider (self->recent, self);

    /* A search is as slow as its slowest provider: it is not finished, and
     * the view keeps taking in results, until the last one is done. Measured
     * on one home folder, for a single letter typed: BigSearch answers in 3
     * to 80 ms, LocalSearch in 11 seconds, and the directory walk visits
     * over a million entries and hands back tens of thousands of them.
     *
     * So when BigSearch takes the search, it has it to itself. It covers
     * the names the walk would find and, when the search is to look inside
     * the files, the contents LocalSearch would find, from one index that
     * follows the filesystem with inotify.
     *
     * Decided here rather than inside the other engines because only this
     * one knows which providers the search asked for: a search set up for
     * the simple provider alone must still use it. And decided per search,
     * so that stopping the daemon hands the others their job back.
     *
     * Taking the search is not answering it: should the daemon then fail,
     * or know nothing of the folder, search_provider_bigsearch_finished()
     * starts the others after all. */
    if (!search_engine_start_provider_if (self->bigsearch, self))
    {
        search_engine_start_slow_providers (self);
    }

    self->starting = FALSE;

    /* Providers could already be finished */
    check_providers_status (self);
}

void
nautilus_search_engine_start (NautilusSearchEngine *self,
                              NautilusQuery        *query)
{
    g_return_if_fail (query != NULL);

    g_autoptr (NautilusQuery) query_to_copy = g_object_ref (query);

    g_clear_object (&self->query);
    self->query = nautilus_query_copy (query_to_copy);

    if (self->running)
    {
        self->restart = TRUE;

        /* Normally the providers still at work bring the engine round to
         * restarting. With all of them done and only held hits keeping the
         * search open, nothing would until the hold ran out. */
        if (search_engine_waits_for_held_only (self))
        {
            check_providers_status (self);
        }

        return;
    }

    /* Keep reference on self while running */
    g_object_ref (self);
    self->running = TRUE;
    g_object_notify (G_OBJECT (self), "running");

    g_debug ("Search engine start");
    search_engine_start_real (self);
}

static void
search_engine_stop_providers (NautilusSearchEngine *self)
{
    if (self->localsearch != NULL)
    {
        nautilus_search_provider_stop (self->localsearch);
    }
    if (self->model != NULL)
    {
        nautilus_search_provider_stop (self->model);
    }
    if (self->recent != NULL)
    {
        nautilus_search_provider_stop (self->recent);
    }
    if (self->simple != NULL)
    {
        nautilus_search_provider_stop (self->simple);
    }
    if (self->bigsearch != NULL)
    {
        nautilus_search_provider_stop (self->bigsearch);
    }
}

void
nautilus_search_engine_stop (NautilusSearchEngine *self)
{
    g_debug ("Search engine stop");

    gboolean waited_for_held_only = search_engine_waits_for_held_only (self);

    g_clear_handle_id (&self->limit_idle_id, g_source_remove);
    search_engine_drop_held (self);
    search_engine_stop_providers (self);

    self->restart = FALSE;
    self->stopped = TRUE;

    if (waited_for_held_only)
    {
        /* No provider is left to report, and the hold that would have
         * closed the search is gone: close it, from the main loop, as a
         * provider stopping would have. */
        self->held_timeout_id = g_idle_add (search_engine_hand_over_held, self);
    }
}

static void
search_engine_drop_held (NautilusSearchEngine *self)
{
    g_clear_handle_id (&self->held_timeout_id, g_source_remove);
    g_clear_pointer (&self->held, g_ptr_array_unref);
}

/* The search is still standing: out with the rest. */
static gboolean
search_engine_hand_over_held (gpointer user_data)
{
    NautilusSearchEngine *self = user_data;
    g_autoptr (GPtrArray) held = g_steal_pointer (&self->held);

    self->held_timeout_id = 0;

    if (!self->running)
    {
        return G_SOURCE_REMOVE;
    }

    if (held != NULL && held->len > 0 && !self->restart)
    {
        self->n_handed_over += held->len;
        g_signal_emit (self, signals[HITS_ADDED], 0, held);
    }

    /* The providers may all be done, with only this left to wait for. Last:
     * it can be what lets go of the engine. */
    check_providers_status (self);

    return G_SOURCE_REMOVE;
}

static gboolean
search_engine_limit_reached (gpointer user_data)
{
    NautilusSearchEngine *self = user_data;

    self->limit_idle_id = 0;

    if (self->running && !self->restart)
    {
        search_engine_stop_providers (self);
        /* Nothing more is wanted, from a fallback either. */
        self->stopped = TRUE;
    }

    return G_SOURCE_REMOVE;
}

static void
search_provider_hits_added (NautilusSearchProvider *provider,
                            GPtrArray              *transferred_hits,
                            NautilusSearchEngine   *self)
{
    g_autoptr (GPtrArray) hits = transferred_hits;

    if (!self->running || self->restart)
    {
        g_debug ("Ignoring hits-added, since engine is %s",
                 !self->running ? "not running" : "waiting to restart");
        return;
    }

    g_autoptr (GPtrArray) added = g_ptr_array_new_with_free_func (g_object_unref);
    for (guint i = 0; i < hits->len && self->n_hits < SEARCH_ENGINE_MAX_HITS; i++)
    {
        NautilusSearchHit *hit = hits->pdata[i];
        const char *uri = nautilus_search_hit_get_uri (hit);

        if (g_hash_table_contains (self->uris, uri))
        {
            continue;
        }

        g_hash_table_add (self->uris, g_strdup (uri));
        self->n_hits++;

        if (!self->paced || self->n_handed_over < SEARCH_ENGINE_FIRST_HITS)
        {
            g_ptr_array_add (added, g_object_ref (hit));
            self->n_handed_over++;
        }
        else
        {
            if (self->held == NULL)
            {
                self->held = g_ptr_array_new_with_free_func (g_object_unref);
            }

            g_ptr_array_add (self->held, g_object_ref (hit));
        }
    }

    if (added->len > 0)
    {
        g_signal_emit (self, signals[HITS_ADDED], 0, added);
    }

    if (self->held != NULL && self->held->len > 0 && self->held_timeout_id == 0)
    {
        self->held_timeout_id = g_timeout_add (SEARCH_ENGINE_HOLD_MS,
                                               search_engine_hand_over_held, self);
    }

    if (self->n_hits >= SEARCH_ENGINE_MAX_HITS && self->limit_idle_id == 0 && self->running)
    {
        /* Not from in here: this runs inside a provider handing over its
         * hits, and stopping it now could have it report itself finished
         * while it is in the middle of doing just that. */
        g_debug ("Search engine has %u hits, stopping the providers", self->n_hits);
        self->limit_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                               search_engine_limit_reached,
                                               g_object_ref (self), g_object_unref);
    }
}

static void
check_providers_status (NautilusSearchEngine *self)
{
    g_assert (self->running);

    if (self->starting || self->providers_finished < self->providers_started)
    {
        return;
    }

    if (self->restart)
    {
        /* For a search nobody wants any more. */
        search_engine_drop_held (self);
    }
    else if (self->held_timeout_id != 0)
    {
        /* Not finished until what is held went out;
         * search_engine_hand_over_held() comes back here. */
        return;
    }

    if (self->restart)
    {
        g_debug ("Search engine finished and restarting");
    }
    else
    {
        g_debug ("Search engine finished");

        g_signal_emit (self, signals[SEARCH_FINISHED], 0);
    }

    g_hash_table_remove_all (self->uris);

    if (self->restart)
    {
        self->restart = FALSE;
        g_debug ("Search engine restarting");
        search_engine_start_real (self);
    }
    else
    {
        self->running = FALSE;
        g_object_notify (G_OBJECT (self), "running");

        g_object_unref (self);
    }
}

static void
search_provider_finished (NautilusSearchEngine *self)
{
    g_debug ("Search provider finished");

    self->providers_finished++;

    check_providers_status (self);
}

/* The walk was held back on the promise of an answer from the index. When
 * that answer did not come, or came empty, the walk is the only thing left
 * that can find the files, so it runs late rather than not at all. */
static void
search_provider_bigsearch_finished (NautilusSearchEngine *self)
{
    if (self->running && !self->restart && !self->stopped &&
        nautilus_search_engine_bigsearch_needs_fallback (NAUTILUS_SEARCH_ENGINE_BIGSEARCH (self->bigsearch)))
    {
        g_debug ("BigSearch had no answer, falling back to the other providers");
        search_engine_start_slow_providers (self);
    }

    search_provider_finished (self);
}

typedef NautilusSearchProvider *(* CreateFunc) (void);

static void
setup_provider (NautilusSearchEngine    *self,
                NautilusSearchProvider **provider_pointer,
                NautilusSearchType       provider_flag,
                CreateFunc               create_func)
{
    if (self->search_type & provider_flag)
    {
        if (*provider_pointer == NULL)
        {
            *provider_pointer = create_func ();

            g_signal_connect (*provider_pointer, "hits-added",
                              G_CALLBACK (search_provider_hits_added),
                              self);
            g_signal_connect_swapped (*provider_pointer, "provider-finished",
                                      (provider_flag == NAUTILUS_SEARCH_TYPE_BIGSEARCH)
                                      ? G_CALLBACK (search_provider_bigsearch_finished)
                                      : G_CALLBACK (search_provider_finished),
                                      self);
        }
    }
    else
    {
        g_clear_object (provider_pointer);
    }
}

void
nautilus_search_engine_set_paced (NautilusSearchEngine *self,
                                  gboolean              paced)
{
    g_return_if_fail (NAUTILUS_IS_SEARCH_ENGINE (self));

    self->paced = paced;
}

void
nautilus_search_engine_set_search_type (NautilusSearchEngine *self,
                                        NautilusSearchType    search_type)
{
    if (self->search_type == search_type)
    {
        return;
    }

    self->search_type = search_type;

    setup_provider (self, &self->localsearch, NAUTILUS_SEARCH_TYPE_LOCALSEARCH,
                    (CreateFunc) nautilus_search_engine_localsearch_new);
    setup_provider (self, &self->bigsearch, NAUTILUS_SEARCH_TYPE_BIGSEARCH,
                    (CreateFunc) nautilus_search_engine_bigsearch_new);
    setup_provider (self, &self->model, NAUTILUS_SEARCH_TYPE_MODEL,
                    (CreateFunc) nautilus_search_engine_model_new);
    setup_provider (self, &self->recent, NAUTILUS_SEARCH_TYPE_RECENT,
                    (CreateFunc) nautilus_search_engine_recent_new);
    setup_provider (self, &self->simple, NAUTILUS_SEARCH_TYPE_SIMPLE,
                    (CreateFunc) nautilus_search_engine_simple_new);
}

static void
nautilus_search_engine_finalize (GObject *object)
{
    NautilusSearchEngine *self = NAUTILUS_SEARCH_ENGINE (object);

    g_hash_table_destroy (self->uris);
    search_engine_drop_held (self);

    g_clear_object (&self->localsearch);
    g_clear_object (&self->bigsearch);
    g_clear_object (&self->recent);
    g_clear_object (&self->model);
    g_clear_object (&self->simple);
    g_clear_object (&self->query);

    G_OBJECT_CLASS (nautilus_search_engine_parent_class)->finalize (object);
}

static void
nautilus_search_engine_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
    NautilusSearchEngine *self = NAUTILUS_SEARCH_ENGINE (object);

    switch (prop_id)
    {
        case PROP_RUNNING:
        {
            g_value_set_boolean (value, self->running);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
search_engine_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
    NautilusSearchEngine *self = NAUTILUS_SEARCH_ENGINE (object);

    switch (prop_id)
    {
        case PROP_SEARCH_TYPE:
        {
            NautilusSearchType search_type = g_value_get_int (value);
            nautilus_search_engine_set_search_type (self, search_type);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_search_engine_class_init (NautilusSearchEngineClass *class)
{
    GObjectClass *object_class;

    object_class = (GObjectClass *) class;

    object_class->finalize = nautilus_search_engine_finalize;
    object_class->get_property = nautilus_search_engine_get_property;
    object_class->set_property = search_engine_set_property;

    /**
     * NautilusSearchEngine::running:
     *
     * Whether the search engine is running a search.
     */
    properties[PROP_RUNNING] =
        g_param_spec_boolean ("running",
                              "search running",
                              "Whether the engine is running a search",
                              FALSE,
                              G_PARAM_READABLE);
    properties[PROP_SEARCH_TYPE] =
        g_param_spec_int ("search-type",
                          "search type",
                          "a #NautilusSearchType",
                          0, G_MAXINT, 0,
                          G_PARAM_WRITABLE);

    g_object_class_install_properties (object_class, N_PROPERTIES, properties);

    /**
     * NautilusSearchEngine::hits-added:
     *
     * @engine: The engine which emitted the signal.
     * @hits: (transfer none): #GPtrArray of #NautilusSearchHit
     *
     * Emitted when search hits are found.
     */
    signals[HITS_ADDED] = g_signal_new (
        "hits-added", G_TYPE_FROM_CLASS (object_class),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__POINTER,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    /**
     * NautilusSearchEngine::search-finished:
     *
     * @engine: The engine which emitted the signal.
     *
     * Emitted when the search finishes. Not emitted in-between restarts.
     */
    signals[SEARCH_FINISHED] = g_signal_new (
        "search-finished", G_TYPE_FROM_CLASS (object_class),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);
}

static void
nautilus_search_engine_init (NautilusSearchEngine *self)
{
    self->uris = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

NautilusSearchEngine *
nautilus_search_engine_new (NautilusSearchType search_type)
{
    return g_object_new (NAUTILUS_TYPE_SEARCH_ENGINE,
                         "search-type", search_type,
                         NULL);
}
