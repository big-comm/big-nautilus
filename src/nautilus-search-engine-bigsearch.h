/* nautilus-search-engine-bigsearch.h: search through the BigSearch daemon
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "nautilus-search-provider.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_SEARCH_ENGINE_BIGSEARCH (nautilus_search_engine_bigsearch_get_type ())

G_DECLARE_FINAL_TYPE (NautilusSearchEngineBigsearch, nautilus_search_engine_bigsearch,
                      NAUTILUS, SEARCH_ENGINE_BIGSEARCH, NautilusSearchProvider)

NautilusSearchEngineBigsearch *nautilus_search_engine_bigsearch_new (void);

/**
 * nautilus_search_engine_bigsearch_is_available:
 *
 * Whether the daemon's socket is there. Checked per search rather than cached:
 * the service can be started or stopped while Nautilus is running, and the
 * only cost is a stat(). A socket left behind by a daemon that died still
 * counts here; the search itself finds that out, see
 * nautilus_search_engine_bigsearch_needs_fallback().
 */
gboolean nautilus_search_engine_bigsearch_is_available (void);

/**
 * nautilus_search_engine_bigsearch_needs_fallback:
 *
 * Whether the last search should be handed to the directory walk after all:
 * the daemon gave no usable answer -- it refused the connection, timed out or
 * sent garbage -- or answered with nothing, which is also what a place outside
 * its index looks like. A cancelled search needs none. Meaningful once the
 * provider reported itself finished.
 */
gboolean nautilus_search_engine_bigsearch_needs_fallback (NautilusSearchEngineBigsearch *self);

G_END_DECLS
