/* nautilus-tag-directory.c: Virtual directory listing files by color tag
 *
 * Copyright (C) 2026 BigCommunity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-tag-directory.h"

#include "nautilus-file.h"
#include "nautilus-file-utilities.h"
#include "nautilus-tag-colors.h"
#include "nautilus-scheme.h"

struct _NautilusTagDirectory
{
    NautilusCollectionDirectory parent_instance;

    /* Which color this location lists. Resolved from the URI ("tag://red/")
     * on first use, because the location is only set after instance init. */
    NautilusTagColor color;
    gboolean color_resolved;
};

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusTagDirectory, nautilus_tag_directory, NAUTILUS_TYPE_COLLECTION_DIRECTORY,
                               nautilus_ensure_extension_points ();
                               g_io_extension_point_implement (NAUTILUS_DIRECTORY_PROVIDER_EXTENSION_POINT_NAME,
                                                               g_define_type_id,
                                                               NAUTILUS_TAG_DIRECTORY_PROVIDER_NAME,
                                                               0))

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
    NautilusTagColor color = get_color (self);

    /* tag://bogus/ names no colour. Matching "none" there would make this
     * the folder of every file without a tag. */
    return color != NAUTILUS_TAG_COLOR_NONE &&
           nautilus_tag_colors_get_color (nautilus_tag_colors_get (), uri) == color;
}

/* A tag may outlive the file it points at: the entry stays in the store
 * until the user clears it, so a URI without a file is expected. */
static GList *
real_list_members (NautilusCollectionDirectory *directory)
{
    NautilusTagDirectory *self = NAUTILUS_TAG_DIRECTORY (directory);
    GList *uris = NULL;
    GList *files = NULL;

    if (get_color (self) == NAUTILUS_TAG_COLOR_NONE)
    {
        return NULL;
    }

    uris = nautilus_tag_colors_get_uris (nautilus_tag_colors_get (), get_color (self));
    for (GList *l = uris; l != NULL; l = l->next)
    {
        NautilusFile *file = nautilus_file_get_by_uri (l->data);

        if (file != NULL)
        {
            files = g_list_prepend (files, file);
        }
    }

    g_list_free_full (uris, g_free);

    return files;
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
        /* Most changes are about some other colour. A file that is not ours
         * to take in can only matter if we hold it, and then it exists as an
         * object already: no need to make one up to find that out. */
        NautilusFile *file = file_has_our_color (self, uri)
                             ? nautilus_file_get_by_uri (uri)
                             : nautilus_file_get_existing_by_uri (uri);

        if (file != NULL)
        {
            changed_files = g_list_prepend (changed_files, file);
        }
    }
    else
    {
        g_autoptr (GHashTable) held = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             g_free, NULL);
        g_autolist (NautilusFile) tagged = real_list_members (NAUTILUS_COLLECTION_DIRECTORY (self));

        /* A bulk change, such as a move: re-evaluate everything we hold... */
        changed_files = nautilus_directory_get_file_list (NAUTILUS_DIRECTORY (self));
        for (GList *l = changed_files; l != NULL; l = l->next)
        {
            g_hash_table_add (held, nautilus_file_get_uri (NAUTILUS_FILE (l->data)));
        }

        /* ...and take in what was given our colour, which tagging a
         * selection reports the same way. */
        for (GList *l = tagged; l != NULL; l = l->next)
        {
            g_autofree char *tagged_uri = nautilus_file_get_uri (l->data);

            if (!g_hash_table_contains (held, tagged_uri))
            {
                changed_files = g_list_prepend (changed_files, nautilus_file_ref (l->data));
            }
        }
    }

    nautilus_collection_directory_update_files (NAUTILUS_COLLECTION_DIRECTORY (self),
                                                changed_files);
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
real_handles_location (GFile *location)
{
    return g_file_has_uri_scheme (location, SCHEME_TAG);
}

static void
nautilus_tag_directory_finalize (GObject *object)
{
    g_signal_handlers_disconnect_by_func (nautilus_tag_colors_get (),
                                          on_tag_colors_changed,
                                          object);

    G_OBJECT_CLASS (nautilus_tag_directory_parent_class)->finalize (object);
}

static void
nautilus_tag_directory_constructed (GObject *object)
{
    G_OBJECT_CLASS (nautilus_tag_directory_parent_class)->constructed (object);

    /* Only now is the "location" property set, and the color is read from it.
     * Doing this in init() dereferences a NULL location. */
    nautilus_directory_force_reload (NAUTILUS_DIRECTORY (object));
}

static void
nautilus_tag_directory_class_init (NautilusTagDirectoryClass *klass)
{
    GObjectClass *oclass;
    NautilusDirectoryClass *directory_class;

    oclass = G_OBJECT_CLASS (klass);
    directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    oclass->finalize = nautilus_tag_directory_finalize;
    oclass->constructed = nautilus_tag_directory_constructed;

    directory_class->handles_location = real_handles_location;
    directory_class->contains_file = real_contains_file;

    NAUTILUS_COLLECTION_DIRECTORY_CLASS (klass)->list_members = real_list_members;
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

    /* The colour goes in the host, not the path, so that each colour is its
     * own root. A location with a path component is treated as a file inside
     * a directory, and opening it makes Nautilus try to mount the enclosing
     * volume, which fails with G_IO_ERROR_NOT_SUPPORTED because no GVfs
     * backend owns this scheme. starred:/// works for the same reason. */
    return g_strconcat (SCHEME_TAG "://", nautilus_tag_color_get_name (color), "/", NULL);
}

NautilusTagColor
nautilus_tag_directory_get_color_for_location (GFile *location)
{
    g_autofree char *path = NULL;
    char *name;

    if (location == NULL || !g_file_has_uri_scheme (location, SCHEME_TAG))
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }

    path = g_file_get_uri (location);

    /* "tag://red/" -> "red" */
    name = strstr (path, "://");
    if (name == NULL)
    {
        return NAUTILUS_TAG_COLOR_NONE;
    }
    name += 3;

    {
        char *end = strchr (name, '/');

        if (end != NULL)
        {
            *end = '\0';
        }
    }

    for (int i = NAUTILUS_TAG_COLOR_NONE + 1; i < NAUTILUS_TAG_COLOR_LAST; i++)
    {
        if (g_strcmp0 (name, nautilus_tag_color_get_name (i)) == 0)
        {
            return (NautilusTagColor) i;
        }
    }

    return NAUTILUS_TAG_COLOR_NONE;
}
