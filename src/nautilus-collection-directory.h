/* nautilus-collection-directory.h: a folder of files gathered from anywhere
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "nautilus-directory.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_COLLECTION_DIRECTORY (nautilus_collection_directory_get_type ())

G_DECLARE_DERIVABLE_TYPE (NautilusCollectionDirectory, nautilus_collection_directory,
                          NAUTILUS, COLLECTION_DIRECTORY, NautilusDirectory)

/**
 * NautilusCollectionDirectoryClass:
 * @list_members: (transfer full): every file that belongs to the collection
 *   now, as a list of #NautilusFile.
 *
 * A read-only directory whose files live elsewhere and belong to it by a
 * property of their own, such as a star or a colour tag. A subclass says
 * which files those are, through @list_members and
 * #NautilusDirectoryClass.contains_file, and reports the ones whose
 * membership may have changed.
 */
struct _NautilusCollectionDirectoryClass
{
    NautilusDirectoryClass parent_class;

    GList * (*list_members) (NautilusCollectionDirectory *self);
};

/**
 * nautilus_collection_directory_update_files:
 * @changed_files: (element-type NautilusFile): files whose membership may
 *   have changed
 *
 * Takes in the ones that became members and lets go of the ones that no
 * longer are.
 */
void nautilus_collection_directory_update_files (NautilusCollectionDirectory *self,
                                                 GList                       *changed_files);

G_END_DECLS
