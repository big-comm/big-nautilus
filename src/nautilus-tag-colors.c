/* nautilus-tag-colors.c: Color tags for files and folders
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include "nautilus-tag-colors.h"

#include "nautilus-file-undo-manager.h"
#include "nautilus-file-undo-operations.h"

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

    /* Per-colour user name, indexed by NautilusTagColor. NULL means the
     * translated colour name is used instead. */
    char *names[NAUTILUS_TAG_COLOR_LAST];

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
    const char *custom = nautilus_tag_colors_get_display_name (nautilus_tag_colors_get (), color);

    if (custom != NULL)
    {
        return custom;
    }

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

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        char *name = g_key_file_get_string (keyfile, "names",
                                            nautilus_tag_color_get_name (i), NULL);

        if (name != NULL && *name != '\0')
        {
            self->names[i] = name;
        }
        else
        {
            g_free (name);
        }
    }

    /* Current format: one key per colour, whose value is the list of URIs.
     * URIs live on the value side because GKeyFile rejects '=', '[' and ']'
     * in key names, and silently dropped any file whose URI contained one. */
    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        g_auto (GStrv) list = g_key_file_get_string_list (keyfile, "tags",
                                                          nautilus_tag_color_get_name (i),
                                                          NULL, NULL);

        for (guint j = 0; list != NULL && list[j] != NULL; j++)
        {
            if (*list[j] != '\0')
            {
                g_hash_table_insert (self->by_uri, g_strdup (list[j]),
                                     GUINT_TO_POINTER (i));
            }
        }
    }

    /* Legacy format, where the URI was the key. Still read so that existing
     * tags survive the upgrade; the next save rewrites them in the new shape.
     * Colour names are never valid URIs, so the two cannot be confused. */
    uris = g_key_file_get_keys (keyfile, "tags", NULL, NULL);
    if (uris == NULL)
    {
        return;
    }

    for (guint i = 0; uris[i] != NULL; i++)
    {
        g_autofree char *name = NULL;
        NautilusTagColor color;

        if (color_from_name (uris[i]) != NAUTILUS_TAG_COLOR_NONE)
        {
            /* A colour name: already handled by the loop above. */
            continue;
        }

        name = g_key_file_get_string (keyfile, "tags", uris[i], NULL);
        color = color_from_name (name);

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

    /* Keyed by colour, with the URIs on the value side: GKeyFile rejects '=',
     * '[' and ']' in key names, so the previous layout quietly dropped every
     * file whose URI contained one. */
    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        g_autoptr (GPtrArray) uris = g_ptr_array_new ();

        g_hash_table_iter_init (&iter, self->by_uri);
        while (g_hash_table_iter_next (&iter, &key, &value))
        {
            if (GPOINTER_TO_UINT (value) == (guint) i)
            {
                g_ptr_array_add (uris, key);
            }
        }

        if (uris->len > 0)
        {
            g_key_file_set_string_list (keyfile, "tags",
                                        nautilus_tag_color_get_name (i),
                                        (const char * const *) uris->pdata,
                                        uris->len);
        }
    }

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        if (self->names[i] != NULL)
        {
            g_key_file_set_string (keyfile, "names",
                                   nautilus_tag_color_get_name (i), self->names[i]);
        }
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

    for (int i = 0; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        g_clear_pointer (&self->names[i], g_free);
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

/* Table update only. Callers that change several files in one go emit a single
 * "changed" afterwards instead of one per file: every cell and the sidebar
 * listen to it, so emitting per URI turned tagging a selection into thousands
 * of redundant style invalidations and sidebar rebuilds. */
static gboolean
set_color_internal (NautilusTagColors *self,
                    const char        *uri,
                    NautilusTagColor   color)
{
    if (nautilus_tag_colors_get_color (self, uri) == color)
    {
        return FALSE;
    }

    if (color == NAUTILUS_TAG_COLOR_NONE)
    {
        g_hash_table_remove (self->by_uri, uri);
    }
    else
    {
        g_hash_table_insert (self->by_uri, g_strdup (uri), GUINT_TO_POINTER (color));
    }

    return TRUE;
}

void
nautilus_tag_colors_set_color (NautilusTagColors *self,
                               const char        *uri,
                               NautilusTagColor   color)
{
    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));
    g_return_if_fail (uri != NULL);

    if (!set_color_internal (self, uri, color))
    {
        return;
    }

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, uri);
}

/* Remembers which class is on the widget, so the common case -- nothing
 * changed -- touches no CSS at all. The previous code removed all eight
 * classes and added one back on every refresh of every visible cell. */
#define APPLIED_COLOR_KEY "nautilus-tag-applied-color"

void
nautilus_tag_colors_apply_to_widget (GtkWidget  *widget,
                                     const char *uri)
{
    NautilusTagColor color;
    NautilusTagColor applied;

    g_return_if_fail (GTK_IS_WIDGET (widget));

    color = nautilus_tag_colors_get_color (nautilus_tag_colors_get (), uri);
    applied = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget), APPLIED_COLOR_KEY));

    if (applied == color)
    {
        return;
    }

    if (applied != NAUTILUS_TAG_COLOR_NONE)
    {
        gtk_widget_remove_css_class (widget, nautilus_tag_color_get_css_class (applied));
    }

    if (color != NAUTILUS_TAG_COLOR_NONE)
    {
        gtk_widget_add_css_class (widget, nautilus_tag_color_get_css_class (color));
    }

    g_object_set_data (G_OBJECT (widget), APPLIED_COLOR_KEY, GUINT_TO_POINTER (color));
}

gboolean
nautilus_tag_colors_has_color (NautilusTagColors *self,
                               NautilusTagColor   color)
{
    GHashTableIter iter;
    gpointer value;

    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), FALSE);

    g_hash_table_iter_init (&iter, self->by_uri);
    while (g_hash_table_iter_next (&iter, NULL, &value))
    {
        if (GPOINTER_TO_UINT (value) == (guint) color)
        {
            return TRUE;
        }
    }

    return FALSE;
}

void
nautilus_tag_colors_flush (NautilusTagColors *self)
{
    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));

    if (self->save_timeout_id != 0)
    {
        g_clear_handle_id (&self->save_timeout_id, g_source_remove);
        save_cb (self);
    }
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

void
nautilus_tag_colors_apply_with_undo (GList            *uris,
                                     NautilusTagColor  color)
{
    NautilusTagColors *self = nautilus_tag_colors_get ();
    g_autoptr (GHashTable) previous = NULL;
    g_autoptr (NautilusFileUndoInfo) undo_info = NULL;

    if (uris == NULL)
    {
        return;
    }

    previous = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    for (GList *l = uris; l != NULL; l = l->next)
    {
        const char *uri = l->data;

        /* A repeated URI would otherwise record the colour we just applied as
         * its "previous" one, and undo would restore the wrong value. */
        if (!g_hash_table_contains (previous, uri))
        {
            g_hash_table_insert (previous, g_strdup (uri),
                                 GUINT_TO_POINTER (nautilus_tag_colors_get_color (self, uri)));
        }

        set_color_internal (self, uri, color);
    }

    /* One notification for the whole batch: uri == NULL means "many changed". */
    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, NULL);

    undo_info = nautilus_file_undo_info_tag_color_new (previous, color);
    nautilus_file_undo_manager_set_action (undo_info);
}

const char *
nautilus_tag_colors_get_display_name (NautilusTagColors *self,
                                      NautilusTagColor   color)
{
    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), NULL);

    if (color <= NAUTILUS_TAG_COLOR_NONE || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return NULL;
    }

    return self->names[color];
}

void
nautilus_tag_colors_set_display_name (NautilusTagColors *self,
                                      NautilusTagColor   color,
                                      const char        *name)
{
    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));

    if (color <= NAUTILUS_TAG_COLOR_NONE || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return;
    }

    g_clear_pointer (&self->names[color], g_free);

    /* An empty name clears the override rather than showing a blank row. */
    if (name != NULL && *name != '\0')
    {
        self->names[color] = g_strdup (name);
    }

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, NULL);
}
