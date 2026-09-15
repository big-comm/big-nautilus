/* nautilus-tag-colors.c: Color tags for files and folders
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "nautilus-tag-colors.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>

/* Writes are coalesced: tagging a multi-file selection, or holding a shortcut,
 * must not turn into one disk write per file. */
#define SAVE_DELAY_MS 1500

struct _NautilusTagColors
{
    GObject parent_instance;

    /* uri (owned char *) -> NautilusTagColor, as GUINT_TO_POINTER.
     * Everything is resolved from here; the file is only read at startup and
     * written back on a timeout. Cell drawing must stay allocation-free. */
    GHashTable *by_uri;

    char *path;
    guint save_timeout_id;
};

G_DEFINE_FINAL_TYPE (NautilusTagColors, nautilus_tag_colors, G_TYPE_OBJECT)

enum
{
    CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

/* Stable on-disk names. Never reorder these against the enum. */
static const char *const color_names[NAUTILUS_TAG_COLOR_LAST] =
{
    [NAUTILUS_TAG_COLOR_NONE] = "none",
    [NAUTILUS_TAG_COLOR_RED] = "red",
    [NAUTILUS_TAG_COLOR_ORANGE] = "orange",
    [NAUTILUS_TAG_COLOR_YELLOW] = "yellow",
    [NAUTILUS_TAG_COLOR_GREEN] = "green",
    [NAUTILUS_TAG_COLOR_BLUE] = "blue",
    [NAUTILUS_TAG_COLOR_PURPLE] = "purple",
    [NAUTILUS_TAG_COLOR_BROWN] = "brown",
    [NAUTILUS_TAG_COLOR_GRAY] = "gray",
};

static const char *const css_classes[NAUTILUS_TAG_COLOR_LAST] =
{
    [NAUTILUS_TAG_COLOR_NONE] = NULL,
    [NAUTILUS_TAG_COLOR_RED] = "nautilus-tag-red",
    [NAUTILUS_TAG_COLOR_ORANGE] = "nautilus-tag-orange",
    [NAUTILUS_TAG_COLOR_YELLOW] = "nautilus-tag-yellow",
    [NAUTILUS_TAG_COLOR_GREEN] = "nautilus-tag-green",
    [NAUTILUS_TAG_COLOR_BLUE] = "nautilus-tag-blue",
    [NAUTILUS_TAG_COLOR_PURPLE] = "nautilus-tag-purple",
    [NAUTILUS_TAG_COLOR_BROWN] = "nautilus-tag-brown",
    [NAUTILUS_TAG_COLOR_GRAY] = "nautilus-tag-gray",
};

const char *
nautilus_tag_color_get_css_class (NautilusTagColor color)
{
    if (color <= NAUTILUS_TAG_COLOR_NONE || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return NULL;
    }

    return css_classes[color];
}

const char *
nautilus_tag_color_get_name (NautilusTagColor color)
{
    if (color < 0 || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return color_names[NAUTILUS_TAG_COLOR_NONE];
    }

    return color_names[color];
}

const char *
nautilus_tag_color_get_label (NautilusTagColor color)
{
    switch (color)
    {
        case NAUTILUS_TAG_COLOR_RED:
        {
            return _TAG ("Red");
        }

        case NAUTILUS_TAG_COLOR_ORANGE:
        {
            return _TAG ("Orange");
        }

        case NAUTILUS_TAG_COLOR_YELLOW:
        {
            return _TAG ("Yellow");
        }

        case NAUTILUS_TAG_COLOR_GREEN:
        {
            return _TAG ("Green");
        }

        case NAUTILUS_TAG_COLOR_BLUE:
        {
            return _TAG ("Blue");
        }

        case NAUTILUS_TAG_COLOR_PURPLE:
        {
            return _TAG ("Purple");
        }

        case NAUTILUS_TAG_COLOR_BROWN:
        {
            return _TAG ("Brown");
        }

        case NAUTILUS_TAG_COLOR_GRAY:
        {
            return _TAG ("Gray");
        }

        default:
        {
            return _TAG ("None");
        }
    }
}

static NautilusTagColor
color_from_name (const char *name)
{
    if (name == NULL)
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        if (g_strcmp0 (name, color_names[i]) == 0)
        {
            return (NautilusTagColor) i;
        }
    }

    return NAUTILUS_TAG_COLOR_NONE;
}

static void
load (NautilusTagColors *self)
{
    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    g_auto (GStrv) uris = NULL;

    if (!g_key_file_load_from_file (keyfile, self->path, G_KEY_FILE_NONE, &error))
    {
        /* A missing file is the normal first-run case, not a problem. */
        if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
            g_warning ("Could not read tag colors from %s: %s", self->path, error->message);
        }
        return;
    }

    uris = g_key_file_get_keys (keyfile, "tags", NULL, NULL);
    if (uris == NULL)
    {
        return;
    }

    for (guint i = 0; uris[i] != NULL; i++)
    {
        g_autofree char *name = g_key_file_get_string (keyfile, "tags", uris[i], NULL);
        NautilusTagColor color = color_from_name (name);

        if (color != NAUTILUS_TAG_COLOR_NONE)
        {
            g_hash_table_insert (self->by_uri, g_strdup (uris[i]),
                                 GUINT_TO_POINTER (color));
        }
    }
}

static gboolean
save_cb (gpointer user_data)
{
    NautilusTagColors *self = user_data;
    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    g_autofree char *dir = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    self->save_timeout_id = 0;

    g_hash_table_iter_init (&iter, self->by_uri);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        g_key_file_set_string (keyfile, "tags", key,
                               nautilus_tag_color_get_name (GPOINTER_TO_UINT (value)));
    }

    dir = g_path_get_dirname (self->path);
    if (g_mkdir_with_parents (dir, 0700) != 0)
    {
        g_warning ("Could not create %s for tag colors: %s", dir, g_strerror (errno));
        return G_SOURCE_REMOVE;
    }

    if (!g_key_file_save_to_file (keyfile, self->path, &error))
    {
        g_warning ("Could not write tag colors to %s: %s", self->path, error->message);
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_save (NautilusTagColors *self)
{
    g_clear_handle_id (&self->save_timeout_id, g_source_remove);
    self->save_timeout_id = g_timeout_add (SAVE_DELAY_MS, save_cb, self);
}

static void
nautilus_tag_colors_finalize (GObject *object)
{
    NautilusTagColors *self = NAUTILUS_TAG_COLORS (object);

    if (self->save_timeout_id != 0)
    {
        /* Do not lose the last edits on shutdown. */
        g_clear_handle_id (&self->save_timeout_id, g_source_remove);
        save_cb (self);
    }

    g_clear_pointer (&self->by_uri, g_hash_table_unref);
    g_clear_pointer (&self->path, g_free);

    G_OBJECT_CLASS (nautilus_tag_colors_parent_class)->finalize (object);
}

static void
nautilus_tag_colors_class_init (NautilusTagColorsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = nautilus_tag_colors_finalize;

    /**
     * NautilusTagColors::changed:
     * @uri: the file whose tag changed, or %NULL when many changed at once
     *
     * Cells listen to this to refresh their styling. Connect with
     * g_signal_connect_object() so the handler cannot outlive the widget.
     */
    signals[CHANGED] =
        g_signal_new ("changed",
                      NAUTILUS_TYPE_TAG_COLORS,
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
nautilus_tag_colors_init (NautilusTagColors *self)
{
    self->by_uri = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    self->path = g_build_filename (g_get_user_data_dir (), "nautilus", "tags.conf", NULL);

    load (self);
}

NautilusTagColors *
nautilus_tag_colors_get (void)
{
    static NautilusTagColors *instance = NULL;

    if (G_UNLIKELY (instance == NULL))
    {
        bindtextdomain (GETTEXT_PACKAGE_TAGS, LOCALEDIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE_TAGS, "UTF-8");

        instance = g_object_new (NAUTILUS_TYPE_TAG_COLORS, NULL);
    }

    return instance;
}

NautilusTagColor
nautilus_tag_colors_get_color (NautilusTagColors *self,
                               const char        *uri)
{
    gpointer value;

    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), NAUTILUS_TAG_COLOR_NONE);

    if (uri == NULL)
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }

    value = g_hash_table_lookup (self->by_uri, uri);

    return value != NULL ? GPOINTER_TO_UINT (value) : NAUTILUS_TAG_COLOR_NONE;
}

void
nautilus_tag_colors_set_color (NautilusTagColors *self,
                               const char        *uri,
                               NautilusTagColor   color)
{
    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));
    g_return_if_fail (uri != NULL);

    if (nautilus_tag_colors_get_color (self, uri) == color)
    {
        return;
    }

    if (color == NAUTILUS_TAG_COLOR_NONE)
    {
        g_hash_table_remove (self->by_uri, uri);
    }
    else
    {
        g_hash_table_insert (self->by_uri, g_strdup (uri), GUINT_TO_POINTER (color));
    }

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, uri);
}

GList *
nautilus_tag_colors_get_uris (NautilusTagColors *self,
                              NautilusTagColor   color)
{
    GList *result = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), NULL);

    g_hash_table_iter_init (&iter, self->by_uri);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        if (GPOINTER_TO_UINT (value) == (guint) color)
        {
            result = g_list_prepend (result, g_strdup (key));
        }
    }

    return result;
}

void
nautilus_tag_colors_move (NautilusTagColors *self,
                          const char        *old_uri,
                          const char        *new_uri)
{
    NautilusTagColor color;

    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));

    color = nautilus_tag_colors_get_color (self, old_uri);
    if (color == NAUTILUS_TAG_COLOR_NONE)
    {
        return;
    }

    g_hash_table_remove (self->by_uri, old_uri);
    g_hash_table_insert (self->by_uri, g_strdup (new_uri), GUINT_TO_POINTER (color));

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, NULL);
}
