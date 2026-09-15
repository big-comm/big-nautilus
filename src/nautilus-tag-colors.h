/* nautilus-tag-colors.h: Color tags for files and folders
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <glib/gi18n.h>

G_BEGIN_DECLS

/* Fork-local translation domain, mirroring the embedded terminal. Keeping
 * these strings out of the upstream catalog means an upstream merge, which
 * brings GNOME's own .po files, cannot wipe them. */
#define GETTEXT_PACKAGE_TAGS "nautilus-tags"
#define _TAG(s) dgettext (GETTEXT_PACKAGE_TAGS, (s))

/**
 * NautilusTagColor:
 *
 * Fixed palette. Colors are named rather than free-form so that they can map
 * onto libadwaita's palette variables and stay legible in both light and dark
 * themes. Stored by name, so the numeric values may be reordered freely.
 */
typedef enum
{
    NAUTILUS_TAG_COLOR_NONE = 0,
    NAUTILUS_TAG_COLOR_RED,
    NAUTILUS_TAG_COLOR_ORANGE,
    NAUTILUS_TAG_COLOR_YELLOW,
    NAUTILUS_TAG_COLOR_GREEN,
    NAUTILUS_TAG_COLOR_BLUE,
    NAUTILUS_TAG_COLOR_PURPLE,
    NAUTILUS_TAG_COLOR_BROWN,
    NAUTILUS_TAG_COLOR_GRAY,
    NAUTILUS_TAG_COLOR_LAST
} NautilusTagColor;

#define NAUTILUS_TYPE_TAG_COLORS (nautilus_tag_colors_get_type ())

G_DECLARE_FINAL_TYPE (NautilusTagColors, nautilus_tag_colors, NAUTILUS, TAG_COLORS, GObject)

/**
 * nautilus_tag_colors_get:
 *
 * Returns the process-wide store. The backing file is read once, on first use.
 */
NautilusTagColors *nautilus_tag_colors_get (void);

/**
 * nautilus_tag_colors_get_color:
 *
 * Looks up @uri in the in-memory table. This is called once per visible cell on
 * every redraw, so it must never touch the disk.
 */
NautilusTagColor nautilus_tag_colors_get_color (NautilusTagColors *self,
                                                const char        *uri);

/**
 * nautilus_tag_colors_set_color:
 *
 * Assigns @color to @uri, or clears it when @color is %NAUTILUS_TAG_COLOR_NONE.
 * Emits #NautilusTagColors::changed and schedules a write.
 */
void nautilus_tag_colors_set_color (NautilusTagColors *self,
                                    const char        *uri,
                                    NautilusTagColor   color);

/**
 * nautilus_tag_colors_get_uris:
 *
 * Every URI carrying @color. This is the reverse lookup that a plain metadata
 * store cannot do, and what makes tag locations possible.
 *
 * Returns: (transfer full) (element-type utf8)
 */
GList *nautilus_tag_colors_get_uris (NautilusTagColors *self,
                                     NautilusTagColor   color);

/**
 * nautilus_tag_colors_move:
 *
 * Follows a rename or move performed inside Nautilus, so the tag is not lost.
 */
void nautilus_tag_colors_move (NautilusTagColors *self,
                               const char        *old_uri,
                               const char        *new_uri);

/* Presentation helpers. Both return static strings. */
const char *nautilus_tag_color_get_css_class (NautilusTagColor color);
const char *nautilus_tag_color_get_name      (NautilusTagColor color);
const char *nautilus_tag_color_get_label     (NautilusTagColor color);

G_END_DECLS
