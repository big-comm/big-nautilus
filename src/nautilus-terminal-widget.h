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
 *  Porter: Elite Autonomous Agent for Linux
 */

#pragma once

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <vte/vte.h>

G_BEGIN_DECLS

/**
 * NautilusTerminalSyncMode:
 * @NAUTILUS_TERMINAL_SYNC_NONE: No synchronization between file manager and terminal.
 * @NAUTILUS_TERMINAL_SYNC_FM_TO_TERM: File manager navigation changes the terminal's directory.
 * @NAUTILUS_TERMINAL_SYNC_TERM_TO_FM: Terminal `cd` commands change the file manager's location.
 * @NAUTILUS_TERMINAL_SYNC_BOTH: Synchronization is bidirectional.
 *
 * Defines the synchronization behavior for the terminal's current directory.
 */
typedef enum
{
    NAUTILUS_TERMINAL_SYNC_NONE,
    NAUTILUS_TERMINAL_SYNC_FM_TO_TERM,
    NAUTILUS_TERMINAL_SYNC_TERM_TO_FM,
    NAUTILUS_TERMINAL_SYNC_BOTH
} NautilusTerminalSyncMode;

/**
 * NautilusTerminalSshAutoConnectMode:
 * @NAUTILUS_TERMINAL_SSH_AUTOCONNECT_OFF: Do not automatically connect to SSH when navigating to an SFTP location.
 * @NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_BOTH: Automatically connect and sync both ways.
 * @NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_FM_TO_TERM: Automatically connect and sync from file manager to terminal.
 * @NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_TERM_TO_FM: Automatically connect and sync from terminal to file manager.
 * @NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_NONE: Automatically connect but do not sync directories.
 *
 * Defines the auto-connection behavior when the file manager navigates to an SFTP location.
 */
typedef enum
{
    NAUTILUS_TERMINAL_SSH_AUTOCONNECT_OFF,
    NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_BOTH,
    NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_FM_TO_TERM,
    NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_TERM_TO_FM,
    NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_NONE
} NautilusTerminalSshAutoConnectMode;


#define NAUTILUS_TYPE_TERMINAL_WIDGET (nautilus_terminal_widget_get_type())
G_DECLARE_FINAL_TYPE(NautilusTerminalWidget, nautilus_terminal_widget, NAUTILUS, TERMINAL_WIDGET, GtkBox)

NautilusTerminalWidget *nautilus_terminal_widget_new_with_location (GFile *location);

void nautilus_terminal_widget_set_current_location (NautilusTerminalWidget *self,
                                                    GFile                  *location);
void nautilus_terminal_widget_set_container_paned (NautilusTerminalWidget *self,
                                                   GtkWidget              *paned);

void nautilus_terminal_widget_toggle_visible (NautilusTerminalWidget *self);
void nautilus_terminal_widget_ensure_state (NautilusTerminalWidget *self);
void nautilus_terminal_widget_ensure_terminal_focus (NautilusTerminalWidget *self);

gboolean nautilus_terminal_widget_get_visible (NautilusTerminalWidget *self);

void nautilus_terminal_widget_apply_new_size (NautilusTerminalWidget *self);

const char *nautilus_terminal_widget_get_last_command_title (NautilusTerminalWidget *self);
gboolean    nautilus_terminal_widget_is_command_running     (NautilusTerminalWidget *self);

G_END_DECLS
