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
#include <errno.h>
#include <gio/gunixmounts.h>
#include <glib/gstdio.h>
#include <string.h>

/* Writes are coalesced: tagging a multi-file selection, or holding a shortcut,
 * must not turn into one disk write per file. */
#define SAVE_DELAY_MS 1500

/* Well after startup, when nothing waits on the disk any more. */
#define PRUNE_DELAY_SECONDS 60

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

    /* URI -> colour of files removed during this session. Trashing a file
     * removes it too, and it may come back: the tag is then put back with it.
     * Not saved. */
    GHashTable *removed;

    /* How many URIs carry each colour, so that asking whether a colour is in
     * use -- the sidebar does, for all of them, on every change -- does not
     * mean going through the table. */
    guint counts[NAUTILUS_TAG_COLOR_LAST];

    guint prune_timeout_id;
    GCancellable *prune_cancellable;

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

/* The one place that writes the table, and keeps the counts true. */
static void
table_set (NautilusTagColors *self,
           const char        *uri,
           NautilusTagColor   color)
{
    NautilusTagColor old = GPOINTER_TO_UINT (g_hash_table_lookup (self->by_uri, uri));

    if (old != NAUTILUS_TAG_COLOR_NONE)
    {
        self->counts[old]--;
    }

    if (color == NAUTILUS_TAG_COLOR_NONE)
    {
        g_hash_table_remove (self->by_uri, uri);
    }
    else
    {
        g_hash_table_insert (self->by_uri, g_strdup (uri), GUINT_TO_POINTER (color));
        self->counts[color]++;
    }
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
            g_autoptr (GDateTime) now = g_date_time_new_now_local ();
            g_autofree char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
            g_autofree char *aside = g_strdup_printf ("%s.unreadable-%s", self->path, stamp);

            /* One bad line makes GKeyFile reject the whole file. The next
             * save would then write this empty state over every tag in it,
             * so put it out of the way first: what can be recovered by hand
             * stays recoverable. */
            if (g_rename (self->path, aside) == 0)
            {
                g_warning ("Could not read tag colors from %s: %s. Kept as %s.",
                           self->path, error->message, aside);
            }
            else
            {
                g_warning ("Could not read tag colors from %s: %s",
                           self->path, error->message);
            }
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
                table_set (self, list[j], i);
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
            table_set (self, uris[i], color);
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
    GPtrArray *by_color[NAUTILUS_TAG_COLOR_LAST] = { NULL };
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    self->save_timeout_id = 0;

    /* Keyed by colour, with the URIs on the value side: GKeyFile rejects '=',
     * '[' and ']' in key names, so the previous layout quietly dropped every
     * file whose URI contained one. */
    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        by_color[i] = g_ptr_array_new ();
    }

    g_hash_table_iter_init (&iter, self->by_uri);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        g_ptr_array_add (by_color[GPOINTER_TO_UINT (value)], key);
    }

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        if (by_color[i]->len > 0)
        {
            g_key_file_set_string_list (keyfile, "tags",
                                        nautilus_tag_color_get_name (i),
                                        (const char * const *) by_color[i]->pdata,
                                        by_color[i]->len);
        }

        g_ptr_array_unref (by_color[i]);
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

    g_clear_handle_id (&self->prune_timeout_id, g_source_remove);
    g_cancellable_cancel (self->prune_cancellable);
    g_clear_object (&self->prune_cancellable);
    g_clear_pointer (&self->by_uri, g_hash_table_unref);
    g_clear_pointer (&self->removed, g_hash_table_unref);
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
    self->removed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
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

    table_set (self, uri, color);

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
    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), FALSE);

    if (color <= NAUTILUS_TAG_COLOR_NONE || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return FALSE;
    }

    return self->counts[color] > 0;
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
    g_autoptr (GPtrArray) inside = NULL;
    g_autofree char *old_prefix = NULL;
    NautilusTagColor color;
    GHashTableIter iter;
    gpointer key;
    gboolean changed = FALSE;

    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));
    g_return_if_fail (old_uri != NULL);
    g_return_if_fail (new_uri != NULL);

    if (g_strcmp0 (old_uri, new_uri) == 0)
    {
        return;
    }

    /* Out of the trash and back where it was. */
    if (g_str_has_prefix (old_uri, "trash:"))
    {
        nautilus_tag_colors_file_restored (self, new_uri);
    }

    /* "report.odt" renamed to "report.odt~" is an editor putting the old
     * contents aside before it writes the new ones under the old name; the
     * file monitor reports that like any rename. The tag belongs to the
     * document, which keeps its name, not to the backup. */
    if (g_str_has_suffix (new_uri, "~") &&
        strlen (new_uri) == strlen (old_uri) + 1 &&
        g_str_has_prefix (new_uri, old_uri))
    {
        return;
    }

    color = nautilus_tag_colors_get_color (self, old_uri);
    if (color != NAUTILUS_TAG_COLOR_NONE)
    {
        table_set (self, old_uri, NAUTILUS_TAG_COLOR_NONE);
        table_set (self, new_uri, color);
        changed = TRUE;
    }

    /* A folder takes what is inside it along, and those are keyed by their
     * own URIs. */
    old_prefix = g_str_has_suffix (old_uri, "/")
                 ? g_strdup (old_uri)
                 : g_strconcat (old_uri, "/", NULL);
    inside = g_ptr_array_new_with_free_func (g_free);

    g_hash_table_iter_init (&iter, self->by_uri);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        if (g_str_has_prefix (key, old_prefix))
        {
            g_ptr_array_add (inside, g_strdup (key));
        }
    }

    for (guint i = 0; i < inside->len; i++)
    {
        const char *inside_uri = g_ptr_array_index (inside, i);
        NautilusTagColor inside_color = nautilus_tag_colors_get_color (self, inside_uri);
        g_autofree char *moved_uri = g_strconcat (new_uri,
                                                  g_str_has_suffix (new_uri, "/") ? "" : "/",
                                                  inside_uri + strlen (old_prefix),
                                                  NULL);

        table_set (self, inside_uri, NAUTILUS_TAG_COLOR_NONE);
        table_set (self, moved_uri, inside_color);
        changed = TRUE;
    }

    if (!changed)
    {
        return;
    }

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, NULL);
}

void
nautilus_tag_colors_file_removed (NautilusTagColors *self,
                                  const char        *uri)
{
    NautilusTagColor color;

    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));
    g_return_if_fail (uri != NULL);

    color = nautilus_tag_colors_get_color (self, uri);
    if (color == NAUTILUS_TAG_COLOR_NONE)
    {
        return;
    }

    g_hash_table_insert (self->removed, g_strdup (uri), GUINT_TO_POINTER (color));
    nautilus_tag_colors_set_color (self, uri, NAUTILUS_TAG_COLOR_NONE);
}

void
nautilus_tag_colors_file_restored (NautilusTagColors *self,
                                   const char        *uri)
{
    gpointer value;

    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));
    g_return_if_fail (uri != NULL);

    if (!g_hash_table_lookup_extended (self->removed, uri, NULL, &value))
    {
        return;
    }

    /* Only into an empty seat: a tag given to whatever took the place since
     * is the newer decision. */
    if (nautilus_tag_colors_get_color (self, uri) == NAUTILUS_TAG_COLOR_NONE)
    {
        nautilus_tag_colors_set_color (self, uri, GPOINTER_TO_UINT (value));
    }

    g_hash_table_remove (self->removed, uri);
}

/* Whether the file behind @uri is known to be gone for good, as opposed to
 * being out of reach for now. Only what is certain counts: a tag dropped by
 * mistake is lost, one kept by mistake costs a line in a file.
 *
 * So the file must be local and must not exist, while the folder it was in
 * does, and has something in it. A disk that is not plugged in or not mounted
 * leaves no folder behind, or the empty one it gets mounted on -- which is
 * also left alone when fstab names it, whatever it holds.
 *
 * Blocks on the filesystem. */
static gboolean
uri_is_gone_for_good (const char *uri)
{
    g_autoptr (GFile) file = g_file_new_for_uri (uri);
    g_autoptr (GFile) parent = NULL;
    g_autofree char *path = NULL;
    g_autofree char *parent_path = NULL;
    g_autoptr (GUnixMountPoint) mount_point = NULL;
    g_autoptr (GDir) dir = NULL;
    GStatBuf buffer;

    if (!g_file_is_native (file))
    {
        return FALSE;
    }

    path = g_file_get_path (file);
    parent = g_file_get_parent (file);
    if (path == NULL || parent == NULL)
    {
        return FALSE;
    }

    /* Anything but "no such file" is not knowing: no permission, a stale
     * network mount, an I/O error. */
    if (g_lstat (path, &buffer) == 0 || errno != ENOENT)
    {
        return FALSE;
    }

    parent_path = g_file_get_path (parent);
    if (parent_path == NULL || !g_file_test (parent_path, G_FILE_TEST_IS_DIR))
    {
        return FALSE;
    }

    mount_point = g_unix_mount_point_at (parent_path, NULL);
    if (mount_point != NULL)
    {
        return FALSE;
    }

    dir = g_dir_open (parent_path, 0, NULL);

    return dir != NULL && g_dir_read_name (dir) != NULL;
}

/* Returns: the URIs among @uris, a NULL-terminated array, that can be let go
 * of. */
static GPtrArray *
find_gone (char         **uris,
           GCancellable  *cancellable)
{
    GPtrArray *gone = g_ptr_array_new_with_free_func (g_free);

    for (guint i = 0; uris[i] != NULL && !g_cancellable_is_cancelled (cancellable); i++)
    {
        if (uri_is_gone_for_good (uris[i]))
        {
            g_ptr_array_add (gone, g_strdup (uris[i]));
        }
    }

    return gone;
}

/* Returns: (transfer container): NULL-terminated; the strings stay the
 * table's. */
static char **
all_uris (NautilusTagColors *self)
{
    return (char **) g_hash_table_get_keys_as_array (self->by_uri, NULL);
}

static guint
drop_gone (NautilusTagColors *self,
           GPtrArray         *gone)
{
    guint n_dropped = 0;

    for (guint i = 0; i < gone->len; i++)
    {
        const char *uri = g_ptr_array_index (gone, i);
        NautilusTagColor color = nautilus_tag_colors_get_color (self, uri);

        /* Tagged again, or renamed away, while the disk was being asked. */
        if (color == NAUTILUS_TAG_COLOR_NONE)
        {
            continue;
        }

        /* Kept at hand like any removal, should the file be restored. */
        g_hash_table_insert (self->removed, g_strdup (uri), GUINT_TO_POINTER (color));
        table_set (self, uri, NAUTILUS_TAG_COLOR_NONE);
        n_dropped++;
    }

    if (n_dropped > 0)
    {
        g_debug ("Dropped %u tag(s) of files that no longer exist", n_dropped);
        schedule_save (self);
        g_signal_emit (self, signals[CHANGED], 0, NULL);
    }

    return n_dropped;
}

guint
nautilus_tag_colors_prune (NautilusTagColors *self)
{
    g_autofree char **uris = NULL;
    g_autoptr (GPtrArray) gone = NULL;

    g_return_val_if_fail (NAUTILUS_IS_TAG_COLORS (self), 0);

    uris = all_uris (self);
    gone = find_gone (uris, NULL);

    return drop_gone (self, gone);
}

static void
prune_thread (GTask        *task,
              gpointer      source_object,
              gpointer      task_data,
              GCancellable *cancellable)
{
    g_task_return_pointer (task, find_gone (task_data, cancellable),
                           (GDestroyNotify) g_ptr_array_unref);
}

static void
prune_done (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data)
{
    g_autoptr (GPtrArray) gone = g_task_propagate_pointer (G_TASK (result), NULL);

    if (gone != NULL)
    {
        drop_gone (NAUTILUS_TAG_COLORS (source_object), gone);
    }
}

static gboolean
prune_timeout_cb (gpointer user_data)
{
    NautilusTagColors *self = user_data;
    g_autoptr (GTask) task = NULL;

    self->prune_timeout_id = 0;

    g_clear_object (&self->prune_cancellable);
    self->prune_cancellable = g_cancellable_new ();

    /* The disk is asked from a thread, about copies of the URIs: one of them
     * may sit on a mount that takes its time to answer. */
    task = g_task_new (self, self->prune_cancellable, prune_done, NULL);
    g_task_set_check_cancellable (task, TRUE);
    {
        g_autofree char **uris = all_uris (self);

        g_task_set_task_data (task, g_strdupv (uris), (GDestroyNotify) g_strfreev);
    }
    g_task_run_in_thread (task, prune_thread);

    return G_SOURCE_REMOVE;
}

void
nautilus_tag_colors_schedule_prune (NautilusTagColors *self)
{
    g_return_if_fail (NAUTILUS_IS_TAG_COLORS (self));

    if (self->prune_timeout_id == 0)
    {
        self->prune_timeout_id = g_timeout_add_seconds (PRUNE_DELAY_SECONDS,
                                                        prune_timeout_cb, self);
    }
}

void
nautilus_tag_colors_apply_with_undo (GList            *uris,
                                     NautilusTagColor  color)
{
    NautilusTagColors *self = nautilus_tag_colors_get ();
    g_autoptr (GHashTable) previous = NULL;
    g_autoptr (NautilusFileUndoInfo) undo_info = NULL;
    gboolean changed = FALSE;

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

        changed |= set_color_internal (self, uri, color);
    }

    if (!changed)
    {
        /* Nothing to undo, and recording it would push out the action that
         * can still be undone -- sending a file to the trash, say. */
        return;
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

    /* An empty name clears the override rather than showing a blank row. */
    if (name != NULL && *name == '\0')
    {
        name = NULL;
    }

    if (g_strcmp0 (self->names[color], name) == 0)
    {
        return;
    }

    g_set_str (&self->names[color], name);

    schedule_save (self);
    g_signal_emit (self, signals[CHANGED], 0, NULL);
}
