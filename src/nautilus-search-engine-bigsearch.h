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
 * Whether the daemon is listening. Checked per search rather than cached: the
 * service can be started or stopped while Nautilus is running, and the only
 * cost is a stat() on the socket.
 */
gboolean nautilus_search_engine_bigsearch_is_available (void);

G_END_DECLS
