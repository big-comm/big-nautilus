/*
 *  Copyright (C) 2025
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Author: Bruno Goncalves <biglinux.com.br>
 */

#pragma once

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define NAUTILUS_TYPE_TERMINAL_WIDGET (nautilus_terminal_widget_get_type ())
G_DECLARE_FINAL_TYPE (NautilusTerminalWidget, nautilus_terminal_widget, NAUTILUS, TERMINAL_WIDGET, GtkWidget)

NautilusTerminalWidget *nautilus_terminal_widget_new (void);
NautilusTerminalWidget *nautilus_terminal_widget_new_with_location (GFile *location);
void nautilus_terminal_widget_set_current_location (NautilusTerminalWidget *self,
                                                    GFile                  *location);
void spawn_terminal_in_widget (NautilusTerminalWidget *self);
void nautilus_terminal_widget_release_focus (NautilusTerminalWidget *self);
void nautilus_terminal_widget_ensure_terminal_focus (NautilusTerminalWidget *self);
void nautilus_terminal_widget_set_maintain_focus (NautilusTerminalWidget *self,
                                                  gboolean                maintain_focus);
gboolean nautilus_terminal_widget_get_has_focus (NautilusTerminalWidget *self);

/* Function to set terminal widget visibility and handle respawning */
void nautilus_terminal_widget_set_visible (NautilusTerminalWidget *self,
                                           gboolean                visible);

G_END_DECLS
