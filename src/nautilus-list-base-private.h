/*
 * Copyright (C) 2022 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-types.h"

#include <gtk/gtk.h>

/*
 * Private header to be included only by subclasses.
 */

G_BEGIN_DECLS

/* Methods */
void               nautilus_list_base_activate_selection (NautilusListBase *self,
                                                          gboolean          open_in_new_tab);
NautilusFile      *nautilus_list_base_get_directory_as_file (NautilusListBase *self);
NautilusViewModel *nautilus_list_base_get_model     (NautilusListBase *self);
GtkWidget         *nautilus_list_base_get_scrolled_window (NautilusListBase *self);
void               nautilus_list_base_setup_gestures (NautilusListBase *self);

/* The cells of a view, which it keeps.
 *
 * Building a cell is most of what it costs to show an item, and GTK does not
 * reuse them when the contents of a list are replaced: it lets go of the ones
 * it has and asks for new ones. A search replaces the contents on every
 * letter typed. A view therefore holds on to the cells it built and hands
 * back the ones GTK let go of; what is left to do for each is to tie it to
 * the list item that shows it, see setup_cell_links(). */
typedef struct _NautilusCellPool NautilusCellPool;

NautilusCellPool *nautilus_cell_pool_new  (guint             max_cells);
void              nautilus_cell_pool_free (NautilusCellPool *pool);
guint             nautilus_cell_pool_get_n_cells (NautilusCellPool *pool);
/* A cell that is not in use, or NULL. (transfer none) */
NautilusViewCell *nautilus_cell_pool_take (NautilusCellPool *pool);
/* Has the pool keep @cell, floating or not, if it has room for it. */
void              nautilus_cell_pool_add  (NautilusCellPool *pool,
                                           NautilusViewCell *cell);

/* Shareable helpers */
void                          setup_cell_common                 (GObject          *listitem,
                                                                 NautilusViewCell *cell);
void                          setup_cell_links                  (GObject          *listitem,
                                                                 NautilusViewCell *cell);
void                          teardown_cell_links               (GObject          *listitem);
void                          setup_cell_controllers            (NautilusViewCell *cell);
void                          setup_cell_hover                  (NautilusViewCell *cell);
void                          setup_cell_hover_inner_target     (NautilusViewCell *cell,
                                                                 GtkWidget        *target);

G_END_DECLS
