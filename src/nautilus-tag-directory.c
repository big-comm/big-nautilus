/* nautilus-tag-directory.c: Virtual directory listing files by color tag
 *
 * Copyright (C) 2026 BigCommunity
 *
 * Derived from nautilus-starred-directory.c (C) 2017 Alexandru Pandelea, which
 * implements this same directory protocol for starred files. The difference is
 * the data source: instead of querying TinySPARQL, this reads the in-memory
 * table in NautilusTagColors, so tag locations keep working on systems where
 * the indexer is not running.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-tag-directory.h"

#include "nautilus-directory-private.h"
#include "nautilus-file-private.h"
#include "nautilus-file-utilities.h"
#include "nautilus-internal-place-file.h"
#include "nautilus-tag-colors.h"
#include "nautilus-scheme.h"
#include <glib/gi18n.h>

struct _NautilusTagDirectory
{
    NautilusDirectory parent_slot;

    /* Which color this location lists. Resolved from the URI ("tag:///red")
     * on first use, because the location is only set after instance init. */
    NautilusTagColor color;
    gboolean color_resolved;

    GList *files;

    GList *monitor_list;
    GList *callback_list;
    GList *pending_callback_list;
};

typedef struct
{
    gboolean monitor_hidden_files;
    NautilusFileAttributes monitor_attributes;

    gconstpointer client;
} TagMonitor;

typedef struct
{
    NautilusTagDirectory *tag_directory;

    NautilusDirectoryCallback callback;
    gpointer callback_data;

    NautilusFileAttributes wait_for_attributes;
    gboolean wait_for_file_list;
    GList *file_list;
} TagCallback;

G_DEFINE_TYPE_WITH_CODE (NautilusTagDirectory, nautilus_tag_directory, NAUTILUS_TYPE_DIRECTORY,
                         nautilus_ensure_extension_points ();
                         /* It looks like you’re implementing an extension point.
                          * Did you modify nautilus_ensure_extension_builtins() accordingly?
                          *
                          * • Yes
                          * • Doing it right now
                          */
                         g_io_extension_point_implement (NAUTILUS_DIRECTORY_PROVIDER_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         NAUTILUS_TAG_DIRECTORY_PROVIDER_NAME,
                                                         0));

static void
file_changed (NautilusFile              *file,
              NautilusTagDirectory *tag_dir)
{
    nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (tag_dir),
                                           &(NautilusFileList) { .data = file });
}

static void
disconnect_and_unmonitor_file (NautilusFile              *file,
                               NautilusTagDirectory *self)
{
    /* Disconnect change handler */
    g_signal_handlers_disconnect_by_func (file, file_changed, self);

    /* Remove monitors */
    for (GList *m = self->monitor_list; m != NULL; m = m->next)
    {
        nautilus_file_monitor_remove (file, m->data);
    }
}

static NautilusTagColor
get_color (NautilusTagDirectory *self)
{
    g_autoptr (GFile) location = NULL;

    if (self->color_resolved)
    {
        return self->color;
    }

    location = nautilus_directory_get_location (NAUTILUS_DIRECTORY (self));
    self->color = nautilus_tag_directory_get_color_for_location (location);
    self->color_resolved = TRUE;

    return self->color;
}

static gboolean
file_has_our_color (NautilusTagDirectory *self,
                    const char           *uri)
{
    return nautilus_tag_colors_get_color (nautilus_tag_colors_get (), uri) == get_color (self);
}

static void
nautilus_tag_directory_update_files (NautilusTagDirectory *self,
                                         GList                     *changed_files)
{
    GList *monitor_list;
    TagMonitor *monitor;
    g_autoptr (GHashTable) uri_table = NULL;
    GList *next;
    g_autolist (NautilusFile) files_added = NULL;
    g_autolist (NautilusFile) files_removed = NULL;

    uri_table = g_hash_table_new_full (g_str_hash,
                                       g_str_equal,
                                       (GDestroyNotify) g_free,
                                       NULL);

    for (GList *l = self->files; l != NULL; l = l->next)
    {
        g_hash_table_add (uri_table, nautilus_file_get_uri (NAUTILUS_FILE (l->data)));
    }

    for (GList *l = changed_files; l != NULL; l = next)
    {
        NautilusFile *file = l->data;
        g_autofree char *uri = nautilus_file_get_uri (file);

        next = l->next;
        if (g_hash_table_contains (uri_table, uri) &&
            !file_has_our_color (self, uri))
        {
            disconnect_and_unmonitor_file (file, self);
            self->files = g_list_remove (self->files, file);
            files_removed = g_list_prepend (files_removed, g_steal_pointer (&file));
        }
        else if (!g_hash_table_contains (uri_table, uri) &&
                 file_has_our_color (self, uri))
        {
            for (monitor_list = self->monitor_list; monitor_list; monitor_list = monitor_list->next)
            {
                monitor = monitor_list->data;

                /* Add monitors */
                nautilus_file_monitor_add (file, monitor, monitor->monitor_attributes);
            }

            g_signal_connect (file, "changed", G_CALLBACK (file_changed), self);

            files_added = g_list_prepend (files_added, nautilus_file_ref (file));
            self->files = g_list_prepend (self->files, nautilus_file_ref (file));
        }
        else
        {
            /* This may happen if we have moved the file and updated the tag_dir
             * database for the new URI. In that case, the NautilusFile instance
             * has already updated its own URI. There's no need to re-add it. */
        }
    }

    if (files_added)
    {
        nautilus_directory_emit_files_added (NAUTILUS_DIRECTORY (self), files_added);
    }

    if (files_removed)
    {
        nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (self), files_removed);
    }
}

static void
on_tag_colors_changed (NautilusTagColors *colors,
                       const char        *uri,
                       gpointer           user_data)
{
    NautilusTagDirectory *self = NAUTILUS_TAG_DIRECTORY (user_data);
    g_autolist (NautilusFile) changed_files = NULL;

    if (uri != NULL)
    {
        NautilusFile *file = nautilus_file_get_by_uri (uri);

        if (file != NULL)
        {
            changed_files = g_list_prepend (changed_files, file);
        }
    }
    else
    {
        /* A bulk change, such as a move: re-evaluate everything we hold. */
        for (GList *l = self->files; l != NULL; l = l->next)
        {
            changed_files = g_list_prepend (changed_files,
                                            nautilus_file_ref (NAUTILUS_FILE (l->data)));
        }
    }

    nautilus_tag_directory_update_files (self, changed_files);
}

static NautilusFile *
real_new_as_file (NautilusDirectory *directory)
{
    return g_object_new (NAUTILUS_TYPE_INTERNAL_PLACE_FILE, "directory", directory, NULL);
}

static gboolean
real_contains_file (NautilusDirectory *directory,
                    NautilusFile      *file)
{
    g_autofree gchar *uri = NULL;

    uri = nautilus_file_get_uri (file);

    return file_has_our_color (NAUTILUS_TAG_DIRECTORY (directory), uri);
}

static gboolean
real_is_editable (NautilusDirectory *directory)
{
    return FALSE;
}

static void
real_call_when_ready (NautilusDirectory         *directory,
                      NautilusFileAttributes     file_attributes,
                      gboolean                   wait_for_file_list,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    GList *file_list;
    NautilusTagDirectory *tag_dir;

    tag_dir = NAUTILUS_TAG_DIRECTORY (directory);

    file_list = nautilus_file_list_copy (tag_dir->files);

    callback (NAUTILUS_DIRECTORY (directory),
              file_list,
              callback_data);

    nautilus_file_list_free (file_list);
}

static gboolean
real_are_all_files_seen (NautilusDirectory *directory)
{
    return TRUE;
}

static void
real_file_monitor_add (NautilusDirectory         *directory,
                       gconstpointer              client,
                       gboolean                   monitor_hidden_files,
                       NautilusFileAttributes     file_attributes,
                       NautilusDirectoryCallback  callback,
                       gpointer                   callback_data)
{
    GList *list;
    TagMonitor *monitor;
    NautilusTagDirectory *tag_dir;
    NautilusFile *file;

    tag_dir = NAUTILUS_TAG_DIRECTORY (directory);

    monitor = g_new0 (TagMonitor, 1);
    monitor->monitor_hidden_files = monitor_hidden_files;
    monitor->monitor_attributes = file_attributes;
    monitor->client = client;

    tag_dir->monitor_list = g_list_prepend (tag_dir->monitor_list, monitor);

    if (callback != NULL)
    {
        (*callback)(directory, tag_dir->files, callback_data);
    }

    for (list = tag_dir->files; list != NULL; list = list->next)
    {
        file = list->data;

        /* Add monitors */
        nautilus_file_monitor_add (file, monitor, file_attributes);
    }
}

static void
tag_monitor_destroy (TagMonitor           *monitor,
                         NautilusTagDirectory *tag_dir)
{
    GList *l;
    NautilusFile *file;

    for (l = tag_dir->files; l != NULL; l = l->next)
    {
        file = l->data;

        nautilus_file_monitor_remove (file, monitor);
    }

    g_free (monitor);
}

static void
real_monitor_remove (NautilusDirectory *directory,
                     gconstpointer      client)
{
    NautilusTagDirectory *tag_dir;
    TagMonitor *monitor;
    GList *list;

    tag_dir = NAUTILUS_TAG_DIRECTORY (directory);

    for (list = tag_dir->monitor_list; list != NULL; list = list->next)
    {
        monitor = list->data;

        if (monitor->client != client)
        {
            continue;
        }

        tag_dir->monitor_list = g_list_delete_link (tag_dir->monitor_list, list);

        tag_monitor_destroy (monitor, tag_dir);

        break;
    }
}

static gboolean
real_handles_location (GFile *location)
{
    return g_file_has_uri_scheme (location, SCHEME_TAG);
}

static TagCallback *
tag_callback_find_pending (NautilusTagDirectory *tag_dir,
                               NautilusDirectoryCallback  callback,
                               gpointer                   callback_data)
{
    TagCallback *tag_callback;
    GList *list;

    for (list = tag_dir->pending_callback_list; list != NULL; list = list->next)
    {
        tag_callback = list->data;

        if (tag_callback->callback == callback &&
            tag_callback->callback_data == callback_data)
        {
            return tag_callback;
        }
    }

    return NULL;
}

static TagCallback *
tag_callback_find (NautilusTagDirectory *tag_dir,
                       NautilusDirectoryCallback  callback,
                       gpointer                   callback_data)
{
    TagCallback *tag_callback;
    GList *list;

    for (list = tag_dir->callback_list; list != NULL; list = list->next)
    {
        tag_callback = list->data;

        if (tag_callback->callback == callback &&
            tag_callback->callback_data == callback_data)
        {
            return tag_callback;
        }
    }

    return NULL;
}

static void
tag_callback_destroy (TagCallback *tag_callback)
{
    nautilus_file_list_free (tag_callback->file_list);

    g_free (tag_callback);
}

static void
real_cancel_callback (NautilusDirectory         *directory,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    NautilusTagDirectory *tag_dir;
    TagCallback *tag_callback;

    tag_dir = NAUTILUS_TAG_DIRECTORY (directory);
    tag_callback = tag_callback_find (tag_dir, callback, callback_data);

    if (tag_callback)
    {
        tag_dir->callback_list = g_list_remove (tag_dir->callback_list, tag_callback);

        tag_callback_destroy (tag_callback);

        return;
    }

    /* Check for a pending callback */
    tag_callback = tag_callback_find_pending (tag_dir, callback, callback_data);

    if (tag_callback)
    {
        tag_dir->pending_callback_list = g_list_remove (tag_dir->pending_callback_list, tag_callback);

        tag_callback_destroy (tag_callback);
    }
}

static GList *
real_get_file_list (NautilusDirectory *directory)
{
    NautilusTagDirectory *tag_dir;

    tag_dir = NAUTILUS_TAG_DIRECTORY (directory);

    return nautilus_file_list_copy (tag_dir->files);
}

static void
nautilus_tag_directory_set_files (NautilusTagDirectory *self)
{
    g_autolist (NautilusFile) tagged_files = NULL;
    GList *uris = NULL;
    GList *l;
    GList *file_list;
    TagMonitor *monitor;
    GList *monitor_list;

    file_list = NULL;

    uris = nautilus_tag_colors_get_uris (nautilus_tag_colors_get (), get_color (self));
    for (l = uris; l != NULL; l = l->next)
    {
        NautilusFile *file = nautilus_file_get_by_uri (l->data);

        /* A tag may outlive the file it points at: the entry stays in the
         * store until the user clears it, so a missing file is expected. */
        if (file != NULL)
        {
            tagged_files = g_list_prepend (tagged_files, file);
        }
    }

    g_list_free_full (g_steal_pointer (&uris), g_free);

    for (l = tagged_files; l != NULL; l = l->next)
    {
        NautilusFile *file = l->data;

        g_signal_connect (file, "changed", G_CALLBACK (file_changed), self);

        for (monitor_list = self->monitor_list; monitor_list; monitor_list = monitor_list->next)
        {
            monitor = monitor_list->data;

            /* Add monitors */
            nautilus_file_monitor_add (file, monitor, monitor->monitor_attributes);
        }

        file_list = g_list_prepend (file_list, nautilus_file_ref (file));
    }

    self->files = file_list;
}

static void
real_force_reload (NautilusDirectory *directory)
{
    NautilusTagDirectory *self = NAUTILUS_TAG_DIRECTORY (directory);

    /* Unset current file list */
    g_list_foreach (self->files, (GFunc) disconnect_and_unmonitor_file, self);
    g_clear_list (&self->files, g_object_unref);

    /* Set a fresh file list  */
    nautilus_tag_directory_set_files (self);
}

static void
nautilus_tag_directory_finalize (GObject *object)
{
    NautilusTagDirectory *self;

    self = NAUTILUS_TAG_DIRECTORY (object);

    g_signal_handlers_disconnect_by_func (nautilus_tag_colors_get (),
                                          on_tag_colors_changed,
                                          self);

    nautilus_file_list_free (self->files);

    G_OBJECT_CLASS (nautilus_tag_directory_parent_class)->finalize (object);
}

static void
nautilus_tag_directory_dispose (GObject *object)
{
    NautilusTagDirectory *tag_dir;
    GList *l;

    tag_dir = NAUTILUS_TAG_DIRECTORY (object);

    /* Remove file connections */
    g_list_foreach (tag_dir->files, (GFunc) disconnect_and_unmonitor_file, tag_dir);

    /* Remove search monitors */
    if (tag_dir->monitor_list)
    {
        for (l = tag_dir->monitor_list; l != NULL; l = l->next)
        {
            tag_monitor_destroy ((TagMonitor *) l->data, tag_dir);
        }

        g_list_free (tag_dir->monitor_list);
        tag_dir->monitor_list = NULL;
    }

    G_OBJECT_CLASS (nautilus_tag_directory_parent_class)->dispose (object);
}

static void
nautilus_tag_directory_constructed (GObject *object)
{
    G_OBJECT_CLASS (nautilus_tag_directory_parent_class)->constructed (object);

    /* Only now is the "location" property set, and the color is read from it.
     * Doing this in init() dereferences a NULL location. */
    nautilus_tag_directory_set_files (NAUTILUS_TAG_DIRECTORY (object));
}

static void
nautilus_tag_directory_class_init (NautilusTagDirectoryClass *klass)
{
    GObjectClass *oclass;
    NautilusDirectoryClass *directory_class;

    oclass = G_OBJECT_CLASS (klass);
    directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    oclass->finalize = nautilus_tag_directory_finalize;
    oclass->dispose = nautilus_tag_directory_dispose;

    oclass->constructed = nautilus_tag_directory_constructed;

    directory_class->new_as_file = real_new_as_file;
    directory_class->handles_location = real_handles_location;
    directory_class->contains_file = real_contains_file;
    directory_class->is_editable = real_is_editable;
    directory_class->force_reload = real_force_reload;
    directory_class->call_when_ready = real_call_when_ready;
    directory_class->are_all_files_seen = real_are_all_files_seen;
    directory_class->file_monitor_add = real_file_monitor_add;
    directory_class->file_monitor_remove = real_monitor_remove;
    directory_class->cancel_callback = real_cancel_callback;
    directory_class->get_file_list = real_get_file_list;
}

NautilusTagDirectory *
nautilus_tag_directory_new (void)
{
    NautilusTagDirectory *self;

    self = g_object_new (NAUTILUS_TYPE_TAG_DIRECTORY, NULL);

    return self;
}

static void
nautilus_tag_directory_init (NautilusTagDirectory *self)
{
    g_signal_connect (nautilus_tag_colors_get (),
                      "changed",
                      (GCallback) on_tag_colors_changed,
                      self);
}

char *
nautilus_tag_directory_get_uri_for_color (NautilusTagColor color)
{
    if (color <= NAUTILUS_TAG_COLOR_NONE || color >= NAUTILUS_TAG_COLOR_LAST)
    {
        return NULL;
    }

    return g_strconcat (SCHEME_TAG ":///", nautilus_tag_color_get_name (color), NULL);
}

NautilusTagColor
nautilus_tag_directory_get_color_for_location (GFile *location)
{
    g_autofree char *path = NULL;
    const char *name;

    if (location == NULL || !g_file_has_uri_scheme (location, SCHEME_TAG))
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }

    path = g_file_get_uri (location);
    name = strrchr (path, '/');
    if (name == NULL)
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }
    name++;

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        if (g_strcmp0 (name, nautilus_tag_color_get_name (i)) == 0)
        {
            return (NautilusTagColor) i;
        }
    }

    return NAUTILUS_TAG_COLOR_NONE;
}
