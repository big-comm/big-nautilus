/* nautilus-tag-directory.h: Virtual directory listing files by color tag
 *
 * Copyright (C) 2026 BigCommunity
 *
 * Modelled on nautilus-starred-directory.c, which implements the same
 * protocol for starred files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-directory.h"
#include "nautilus-tag-colors.h"

G_BEGIN_DECLS

#define NAUTILUS_TAG_DIRECTORY_PROVIDER_NAME "tag-directory-provider"

#define NAUTILUS_TYPE_TAG_DIRECTORY (nautilus_tag_directory_get_type ())

G_DECLARE_FINAL_TYPE (NautilusTagDirectory, nautilus_tag_directory, NAUTILUS, TAG_DIRECTORY, NautilusDirectory)

/**
 * nautilus_tag_directory_get_uri_for_color:
 *
 * The location that lists every file carrying @color, for example
 * "tag:///red". Returns %NULL for %NAUTILUS_TAG_COLOR_NONE.
 *
 * Returns: (transfer full) (nullable)
 */
NautilusTagDirectory *nautilus_tag_directory_new (void);

char *nautilus_tag_directory_get_uri_for_color (NautilusTagColor color);

/**
 * nautilus_tag_directory_get_color_for_location:
 *
 * Inverse of the above: reads the color back out of a "tag://" location.
 */
NautilusTagColor nautilus_tag_directory_get_color_for_location (GFile *location);

G_END_DECLS
