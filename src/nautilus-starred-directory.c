/* nautilus-starred-directory.c
 *
 * Copyright (C) 2017 Alexandru Pandelea <alexandru.pandelea@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nautilus-starred-directory.h"

#include "nautilus-file.h"
#include "nautilus-file-utilities.h"
#include "nautilus-tag-manager.h"
#include "nautilus-scheme.h"

struct _NautilusFavoriteDirectory
{
    NautilusCollectionDirectory parent_instance;
};

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusFavoriteDirectory, nautilus_starred_directory, NAUTILUS_TYPE_COLLECTION_DIRECTORY,
                               nautilus_ensure_extension_points ();
                               g_io_extension_point_implement (NAUTILUS_DIRECTORY_PROVIDER_EXTENSION_POINT_NAME,
                                                               g_define_type_id,
                                                               NAUTILUS_STARRED_DIRECTORY_PROVIDER_NAME,
                                                               0))

static void
on_starred_files_changed (NautilusTagManager *tag_manager,
                          GList              *changed_files,
                          gpointer            user_data)
{
    nautilus_collection_directory_update_files (NAUTILUS_COLLECTION_DIRECTORY (user_data),
                                                changed_files);
}

static gboolean
real_contains_file (NautilusDirectory *directory,
                    NautilusFile      *file)
{
    g_autofree gchar *uri = NULL;

    uri = nautilus_file_get_uri (file);

    return nautilus_tag_manager_file_is_starred (nautilus_tag_manager_get (), uri);
}

static gboolean
real_handles_location (GFile *location)
{
    return g_file_has_uri_scheme (location, SCHEME_STARRED);
}

static GList *
real_list_members (NautilusCollectionDirectory *directory)
{
    return nautilus_tag_manager_get_starred_files (nautilus_tag_manager_get ());
}

static void
nautilus_starred_directory_finalize (GObject *object)
{
    g_signal_handlers_disconnect_by_func (nautilus_tag_manager_get (),
                                          on_starred_files_changed,
                                          object);

    G_OBJECT_CLASS (nautilus_starred_directory_parent_class)->finalize (object);
}

static void
nautilus_starred_directory_class_init (NautilusFavoriteDirectoryClass *klass)
{
    GObjectClass *oclass;
    NautilusDirectoryClass *directory_class;

    oclass = G_OBJECT_CLASS (klass);
    directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    oclass->finalize = nautilus_starred_directory_finalize;

    directory_class->handles_location = real_handles_location;
    directory_class->contains_file = real_contains_file;

    NAUTILUS_COLLECTION_DIRECTORY_CLASS (klass)->list_members = real_list_members;
}

NautilusFavoriteDirectory *
nautilus_starred_directory_new (void)
{
    NautilusFavoriteDirectory *self;

    self = g_object_new (NAUTILUS_TYPE_STARRED_DIRECTORY, NULL);

    return self;
}

static void
nautilus_starred_directory_init (NautilusFavoriteDirectory *self)
{
    g_signal_connect (nautilus_tag_manager_get (),
                      "starred-changed",
                      (GCallback) on_starred_files_changed,
                      self);

    nautilus_directory_force_reload (NAUTILUS_DIRECTORY (self));
}
