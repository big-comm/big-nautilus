/* nautilus-collection-directory.c: a folder of files gathered from anywhere
 *
 * Copyright (C) 2017 Alexandru Pandelea <alexandru.pandelea@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nautilus-collection-directory.h"

#include "nautilus-directory-private.h"
#include "nautilus-file-private.h"
#include "nautilus-internal-place-file.h"

typedef struct
{
    GList *files;
    GList *monitor_list;
} NautilusCollectionDirectoryPrivate;

typedef struct
{
    NautilusAttributes attributes;
    gconstpointer client;
} Monitor;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (NautilusCollectionDirectory, nautilus_collection_directory,
                                     NAUTILUS_TYPE_DIRECTORY)

static void
file_changed (NautilusFile                *file,
              NautilusCollectionDirectory *self)
{
    nautilus_directory_emit_files_changed (NAUTILUS_DIRECTORY (self),
                                           &(NautilusFileList) { .data = file });
}

static void
connect_and_monitor_file (NautilusFile                *file,
                          NautilusCollectionDirectory *self)
{
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    g_signal_connect (file, "changed", G_CALLBACK (file_changed), self);

    for (GList *m = priv->monitor_list; m != NULL; m = m->next)
    {
        Monitor *monitor = m->data;

        nautilus_file_monitor_add (file, monitor, monitor->attributes);
    }
}

static void
disconnect_and_unmonitor_file (NautilusFile                *file,
                               NautilusCollectionDirectory *self)
{
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    g_signal_handlers_disconnect_by_func (file, file_changed, self);

    for (GList *m = priv->monitor_list; m != NULL; m = m->next)
    {
        nautilus_file_monitor_remove (file, m->data);
    }
}

void
nautilus_collection_directory_update_files (NautilusCollectionDirectory *self,
                                            GList                       *changed_files)
{
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);
    NautilusDirectory *directory = NAUTILUS_DIRECTORY (self);
    NautilusDirectoryClass *directory_class = NAUTILUS_DIRECTORY_GET_CLASS (self);
    g_autoptr (GHashTable) held = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autolist (NautilusFile) files_added = NULL;
    g_autolist (NautilusFile) files_removed = NULL;

    for (GList *l = priv->files; l != NULL; l = l->next)
    {
        g_hash_table_add (held, nautilus_file_get_uri (NAUTILUS_FILE (l->data)));
    }

    for (GList *l = changed_files; l != NULL; l = l->next)
    {
        NautilusFile *file = l->data;
        g_autofree char *uri = nautilus_file_get_uri (file);
        gboolean is_held = g_hash_table_contains (held, uri);
        gboolean is_member = directory_class->contains_file (directory, file);

        /* Both the same is no change. It also happens after a move: the
         * subclass already knows the new URI, and so does the file. */
        if (is_held && !is_member)
        {
            disconnect_and_unmonitor_file (file, self);
            /* The reference the list held goes to files_removed. */
            priv->files = g_list_remove (priv->files, file);
            files_removed = g_list_prepend (files_removed, file);
        }
        else if (!is_held && is_member)
        {
            connect_and_monitor_file (file, self);
            files_added = g_list_prepend (files_added, nautilus_file_ref (file));
            priv->files = g_list_prepend (priv->files, nautilus_file_ref (file));
        }
    }

    if (files_added != NULL)
    {
        nautilus_directory_emit_files_added (directory, files_added);
    }

    if (files_removed != NULL)
    {
        nautilus_directory_emit_files_changed (directory, files_removed);
    }
}

static NautilusFile *
real_new_as_file (NautilusDirectory *directory)
{
    return g_object_new (NAUTILUS_TYPE_INTERNAL_PLACE_FILE, "directory", directory, NULL);
}

static gboolean
real_is_editable (NautilusDirectory *directory)
{
    return FALSE;
}

static gboolean
real_are_all_files_seen (NautilusDirectory *directory)
{
    return TRUE;
}

static GList *
real_get_file_list (NautilusDirectory *directory)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (directory);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    return nautilus_file_list_copy (priv->files);
}

static void
real_call_when_ready (NautilusDirectory         *directory,
                      NautilusAttributes         attributes,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    g_autolist (NautilusFile) files = real_get_file_list (directory);

    callback (directory, files, callback_data);
}

/* The files are always at hand, so a callback has run by the time
 * call_when_ready returns and none is left to cancel. */
static void
real_cancel_callback (NautilusDirectory         *directory,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
}

static void
real_file_monitor_add (NautilusDirectory         *directory,
                       gconstpointer              client,
                       gboolean                   monitor_hidden_files,
                       NautilusAttributes         attributes,
                       NautilusDirectoryCallback  callback,
                       gpointer                   callback_data)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (directory);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);
    Monitor *monitor = g_new0 (Monitor, 1);

    monitor->attributes = attributes;
    monitor->client = client;
    priv->monitor_list = g_list_prepend (priv->monitor_list, monitor);

    if (callback != NULL)
    {
        callback (directory, priv->files, callback_data);
    }

    for (GList *l = priv->files; l != NULL; l = l->next)
    {
        nautilus_file_monitor_add (l->data, monitor, attributes);
    }
}

static void
monitor_destroy (Monitor                     *monitor,
                 NautilusCollectionDirectory *self)
{
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    for (GList *l = priv->files; l != NULL; l = l->next)
    {
        nautilus_file_monitor_remove (l->data, monitor);
    }

    g_free (monitor);
}

static void
real_file_monitor_remove (NautilusDirectory *directory,
                          gconstpointer      client)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (directory);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    for (GList *l = priv->monitor_list; l != NULL; l = l->next)
    {
        Monitor *monitor = l->data;

        if (monitor->client == client)
        {
            priv->monitor_list = g_list_delete_link (priv->monitor_list, l);
            monitor_destroy (monitor, self);

            return;
        }
    }
}

static void
real_force_reload (NautilusDirectory *directory)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (directory);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    g_list_foreach (priv->files, (GFunc) disconnect_and_unmonitor_file, self);
    g_clear_list (&priv->files, g_object_unref);

    priv->files = NAUTILUS_COLLECTION_DIRECTORY_GET_CLASS (self)->list_members (self);
    g_list_foreach (priv->files, (GFunc) connect_and_monitor_file, self);
}

static void
nautilus_collection_directory_dispose (GObject *object)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (object);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    g_list_foreach (priv->files, (GFunc) disconnect_and_unmonitor_file, self);
    g_list_foreach (priv->monitor_list, (GFunc) monitor_destroy, self);
    g_clear_list (&priv->monitor_list, NULL);

    G_OBJECT_CLASS (nautilus_collection_directory_parent_class)->dispose (object);
}

static void
nautilus_collection_directory_finalize (GObject *object)
{
    NautilusCollectionDirectory *self = NAUTILUS_COLLECTION_DIRECTORY (object);
    NautilusCollectionDirectoryPrivate *priv = nautilus_collection_directory_get_instance_private (self);

    nautilus_file_list_free (priv->files);

    G_OBJECT_CLASS (nautilus_collection_directory_parent_class)->finalize (object);
}

static void
nautilus_collection_directory_class_init (NautilusCollectionDirectoryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusDirectoryClass *directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    object_class->dispose = nautilus_collection_directory_dispose;
    object_class->finalize = nautilus_collection_directory_finalize;

    directory_class->new_as_file = real_new_as_file;
    directory_class->is_editable = real_is_editable;
    directory_class->force_reload = real_force_reload;
    directory_class->call_when_ready = real_call_when_ready;
    directory_class->are_all_files_seen = real_are_all_files_seen;
    directory_class->file_monitor_add = real_file_monitor_add;
    directory_class->file_monitor_remove = real_file_monitor_remove;
    directory_class->cancel_callback = real_cancel_callback;
    directory_class->get_file_list = real_get_file_list;
}

static void
nautilus_collection_directory_init (NautilusCollectionDirectory *self)
{
}
