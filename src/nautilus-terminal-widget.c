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

#include "nautilus-terminal-widget.h"

#include <glib/gi18n.h>
#include <vte/vte.h>

struct _NautilusTerminalWidget
{
    GtkWidget parent_instance;

    GtkWidget *terminal;
    GtkWidget *box;
    GFile *current_location;
    gboolean maintain_focus;
    guint focus_timeout_id;
    gulong location_changed_id;
    gboolean needs_respawn;
};


static gboolean on_terminal_button_press (GtkGestureClick *gesture,
                                          int              n_press,
                                          double           x,
                                          double           y,
                                          gpointer         user_data);
static void on_terminal_directory_changed (VteTerminal *terminal,
                                           gpointer     user_data);
static gboolean focus_once_and_remove (gpointer user_data);

enum
{
    PROP_0,
    PROP_CURRENT_LOCATION,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
    CHANGE_DIRECTORY,
    TOGGLE_VISIBILITY,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (NautilusTerminalWidget, nautilus_terminal_widget, GTK_TYPE_WIDGET)

static void
nautilus_terminal_widget_finalize (GObject *object)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (object);

    g_clear_object (&self->current_location);

    G_OBJECT_CLASS (nautilus_terminal_widget_parent_class)->finalize (object);
}

static void
change_directory_in_terminal (NautilusTerminalWidget *self,
                              GFile                  *location)
{
    char *path;
    char *quoted_path;
    char *change_dir_command;

    path = g_file_get_path (location);
    if (path == NULL)
    {
        return;
    }

    quoted_path = g_shell_quote (path);
    change_dir_command = g_strdup_printf ("cd %s\n", quoted_path);

    vte_terminal_feed_child (VTE_TERMINAL (self->terminal), change_dir_command, -1);

    g_free (path);
    g_free (quoted_path);
    g_free (change_dir_command);
}

static void
on_terminal_child_exited (VteTerminal *terminal,
                          gint         status,
                          gpointer     user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);

    /* Signal visibility change */
    g_signal_emit (self, signals[TOGGLE_VISIBILITY], 0, FALSE);

    /* Instead of just setting a flag, schedule an immediate respawn */
    g_timeout_add_once (100, (GSourceOnceFunc) spawn_terminal_in_widget, self);
}

static void
nautilus_terminal_widget_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (object);

    switch (prop_id)
    {
        case PROP_CURRENT_LOCATION:
        {
            nautilus_terminal_widget_set_current_location (self, g_value_get_object (value));
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_terminal_widget_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (object);

    switch (prop_id)
    {
        case PROP_CURRENT_LOCATION:
        {
            g_value_set_object (value, self->current_location);
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_terminal_widget_class_init (NautilusTerminalWidgetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize = nautilus_terminal_widget_finalize;
    object_class->set_property = nautilus_terminal_widget_set_property;
    object_class->get_property = nautilus_terminal_widget_get_property;

    properties[PROP_CURRENT_LOCATION] =
        g_param_spec_object ("current-location",
                             "Current location",
                             "The current location displayed in the terminal",
                             G_TYPE_FILE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[CHANGE_DIRECTORY] =
        g_signal_new ("change-directory",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, G_TYPE_FILE);

    signals[TOGGLE_VISIBILITY] =
        g_signal_new ("toggle-visibility",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL,
                      g_cclosure_marshal_VOID__BOOLEAN,
                      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static gboolean
terminal_key_press_cb (GtkEventControllerKey *controller,
                       guint                  keyval,
                       guint                  keycode,
                       GdkModifierType        state,
                       gpointer               user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);

    /* Copy: Ctrl+Shift+C */
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) && keyval == GDK_KEY_c)
    {
        vte_terminal_copy_clipboard_format (VTE_TERMINAL (self->terminal), VTE_FORMAT_TEXT);
        return TRUE;
    }    /* Toggle terminal visibility with F4 */
    if (keyval == GDK_KEY_F4)
    {
        /* Toggle visibility of parent terminal pane */
        GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
        gboolean visible = gtk_widget_get_visible (parent);
        gboolean new_visible = !visible;

        /* First, emit signal to notify the slot of the visibility change */
        g_signal_emit (self, signals[TOGGLE_VISIBILITY], 0, new_visible);

        /* Use our custom function to handle visibility and respawning */
        nautilus_terminal_widget_set_visible (self, new_visible);

        /* If making the terminal visible, ensure it gets focus */
        if (new_visible)
        {
            nautilus_terminal_widget_ensure_terminal_focus (self);
        }

        return TRUE;
    }

    /* Paste: Ctrl+Shift+V */
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) && keyval == GDK_KEY_v)
    {
        vte_terminal_paste_clipboard (VTE_TERMINAL (self->terminal));
        return TRUE;
    }

    /* Select all: Ctrl+Shift+A */
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) && keyval == GDK_KEY_a)
    {
        vte_terminal_select_all (VTE_TERMINAL (self->terminal));
        return TRUE;
    }

    /* Increase font size: Ctrl++ */
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_plus)
    {
        double scale = vte_terminal_get_font_scale (VTE_TERMINAL (self->terminal));
        vte_terminal_set_font_scale (VTE_TERMINAL (self->terminal), scale + 0.1);
        return TRUE;
    }

    /* Decrease font size: Ctrl+- */
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_minus)
    {
        double scale = vte_terminal_get_font_scale (VTE_TERMINAL (self->terminal));
        if (scale > 0.2) /* Prevent font from becoming too small */
        {
            vte_terminal_set_font_scale (VTE_TERMINAL (self->terminal), scale - 0.1);
        }
        return TRUE;
    }

    return FALSE;
}

static gboolean
focus_once_and_remove (gpointer user_data)
{
    GtkWidget *widget = GTK_WIDGET (user_data);
    NautilusTerminalWidget *self;

    /* Grab focus once */
    gtk_widget_grab_focus (widget);

    /* Find the parent NautilusTerminalWidget to clear timeout ID */
    self = NAUTILUS_TERMINAL_WIDGET (gtk_widget_get_ancestor (widget, NAUTILUS_TYPE_TERMINAL_WIDGET));
    if (self != NULL)
    {
        self->focus_timeout_id = 0;
    }

    /* Return FALSE to remove this timeout source */
    return G_SOURCE_REMOVE;
}

static void
on_terminal_directory_changed (VteTerminal *terminal,
                               gpointer     user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    const char *cwd = vte_terminal_get_current_directory_uri (terminal);
    GFile *directory = NULL;
    GtkWidget *term_widget;

    if (cwd != NULL)
    {
        directory = g_file_new_for_uri (cwd);
        if (directory != NULL &&
            (self->current_location == NULL || !g_file_equal (directory, self->current_location)))
        {
            /* Remember the terminal widget for potential focus management */
            term_widget = GTK_WIDGET (self->terminal);

            /* Update our internal tracking */
            g_set_object (&self->current_location, directory);

            /* Signal the directory change to Nautilus */
            g_signal_emit (self, signals[CHANGE_DIRECTORY], 0, directory);

            /* Only grab focus if terminal had focus before the directory change */
            if (self->maintain_focus && gtk_widget_has_focus (term_widget))
            {
                /* Cancel any previous focus timeout to prevent focus stealing loops */
                if (self->focus_timeout_id > 0)
                {
                    g_source_remove (self->focus_timeout_id);
                    self->focus_timeout_id = 0;
                }

                /* Set up a one-time focus grab with a short delay */
                self->focus_timeout_id = g_timeout_add (50, focus_once_and_remove, term_widget);
            }
        }
        if (directory)
        {
            g_object_unref (directory);
        }
    }
}

static void
nautilus_terminal_widget_init (NautilusTerminalWidget *self)
{
    GtkEventController *key_controller;
    GtkGesture *click_gesture;

    /* Initialize focus maintenance to enabled by default
     * This ensures terminal maintains focus when directory changes are initiated from the terminal */
    self->maintain_focus = TRUE;

    self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_parent (self->box, GTK_WIDGET (self));

    /* Create terminal widget */
    self->terminal = vte_terminal_new ();

    /* Set terminal properties */
    vte_terminal_set_bold_is_bright (VTE_TERMINAL (self->terminal), TRUE);
    vte_terminal_set_enable_sixel (VTE_TERMINAL (self->terminal), TRUE);
    vte_terminal_set_scrollback_lines (VTE_TERMINAL (self->terminal), 10000);
    vte_terminal_set_allow_hyperlink (VTE_TERMINAL (self->terminal), TRUE);

    /* Handle terminal exit */
    g_signal_connect (self->terminal, "child-exited",
                      G_CALLBACK (on_terminal_child_exited), self);

    /* Add key press controller for keyboard shortcuts */
    key_controller = gtk_event_controller_key_new ();
    g_signal_connect (key_controller, "key-pressed",
                      G_CALLBACK (terminal_key_press_cb), self);
    gtk_widget_add_controller (self->terminal, key_controller);

    /* Add click gesture for right-click menu */
    click_gesture = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click_gesture), 0);
    g_signal_connect (click_gesture, "pressed",
                      G_CALLBACK (on_terminal_button_press), self);
    gtk_widget_add_controller (self->terminal, GTK_EVENT_CONTROLLER (click_gesture));

    /* Add terminal to the box */
    gtk_box_append (GTK_BOX (self->box), self->terminal);
    gtk_widget_set_hexpand (self->terminal, TRUE);
    gtk_widget_set_vexpand (self->terminal, TRUE);

    /* Listen for OSC 7 directory changes */
    g_signal_connect (self->terminal, "current-directory-uri-changed",
                      G_CALLBACK (on_terminal_directory_changed), self);
}

void
nautilus_terminal_widget_set_visible (NautilusTerminalWidget *self,
                                      gboolean                visible)
{
    g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

    GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
    if (parent != NULL)
    {
        gtk_widget_set_visible (parent, visible);
    }
}


NautilusTerminalWidget *
nautilus_terminal_widget_new_with_location (GFile *location)
{
    NautilusTerminalWidget *self = g_object_new (NAUTILUS_TYPE_TERMINAL_WIDGET, NULL);

    /* Set the location before spawning the terminal */
    if (location != NULL)
    {
        g_set_object (&self->current_location, location);
    }

    /* Start the terminal AFTER setting the location */
    spawn_terminal_in_widget (self);

    return self;
}

void
nautilus_terminal_widget_set_current_location (NautilusTerminalWidget *self,
                                               GFile                  *location)
{
    g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

    if (g_set_object (&self->current_location, location))
    {
        change_directory_in_terminal (self, location);
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CURRENT_LOCATION]);
    }
}

/**
 * nautilus_terminal_widget_set_maintain_focus:
 * @self: The #NautilusTerminalWidget
 * @maintain_focus: Whether to maintain focus in terminal after directory changes
 *
 * Controls whether the terminal should maintain focus when directory changes occur.
 * When set to TRUE, terminal will keep focus after directory changes (good for terminal use).
 * When set to FALSE, focus will not be affected (good for file manager use).
 */
void
nautilus_terminal_widget_set_maintain_focus (NautilusTerminalWidget *self,
                                             gboolean                maintain_focus)
{
    g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));
    self->maintain_focus = maintain_focus;
}

/**
 * nautilus_terminal_widget_release_focus:
 * @self: a #NautilusTerminalWidget
 *
 * Releases focus from the terminal and cancels any pending focus operations.
 * This is useful to call when the user interacts with other parts of Nautilus
 * to ensure the terminal doesn't grab focus back.
 */
void
nautilus_terminal_widget_release_focus (NautilusTerminalWidget *self)
{
    g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

    /* Cancel any pending focus grab */
    if (self->focus_timeout_id > 0)
    {
        g_source_remove (self->focus_timeout_id);
        self->focus_timeout_id = 0;
    }

    /* Set maintain_focus to FALSE */
    self->maintain_focus = FALSE;
}

static void
copy_to_clipboard (GtkWidget   *widget,
                   VteTerminal *terminal)
{
    vte_terminal_copy_clipboard_format (terminal, VTE_FORMAT_TEXT);
}

static void
on_copy_activate (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
    VteTerminal *terminal = VTE_TERMINAL (user_data);
    copy_to_clipboard (NULL, terminal);
}

static void
on_paste_activate (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
    VteTerminal *terminal = VTE_TERMINAL (user_data);
    vte_terminal_paste_clipboard (terminal);
}

static void
on_select_all_activate (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
    VteTerminal *terminal = VTE_TERMINAL (user_data);
    vte_terminal_select_all (terminal);
}

static GtkWidget *
create_terminal_popup_menu (NautilusTerminalWidget *self)
{
    GtkWidget *menu;
    GMenu *menu_model;
    GSimpleActionGroup *action_group;

    /* Create action group for menu actions */
    action_group = g_simple_action_group_new ();

    /* Create actions for menu items */
    GSimpleAction *copy_action = g_simple_action_new ("copy", NULL);
    g_signal_connect (copy_action, "activate", G_CALLBACK (on_copy_activate), self->terminal);
    g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (copy_action));
    g_object_unref (copy_action);

    GSimpleAction *paste_action = g_simple_action_new ("paste", NULL);
    g_signal_connect (paste_action, "activate", G_CALLBACK (on_paste_activate), self->terminal);
    g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (paste_action));
    g_object_unref (paste_action);

    GSimpleAction *select_all_action = g_simple_action_new ("select-all", NULL);
    g_signal_connect (select_all_action, "activate", G_CALLBACK (on_select_all_activate), self->terminal);
    g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (select_all_action));
    g_object_unref (select_all_action);

    /* Create menu model */
    menu_model = g_menu_new ();

    /* Add menu items to the model */
    g_menu_append (menu_model, _("Copy"), "popup.copy");
    g_menu_append (menu_model, _("Paste"), "popup.paste");
    g_menu_append (menu_model, _("Select All"), "popup.select-all");

    /* Create popover menu with the model */
    menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu_model));
    gtk_widget_set_parent (menu, GTK_WIDGET (self->terminal));

    /* Insert action group */
    gtk_widget_insert_action_group (menu, "popup", G_ACTION_GROUP (action_group));
    g_object_unref (action_group);

    g_object_unref (menu_model);

    return menu;
}

static gboolean
on_terminal_button_press (GtkGestureClick *gesture,
                          int              n_press,
                          double           x,
                          double           y,
                          gpointer         user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    guint button;

    button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

    /* Show context menu on right click */
    if (button == GDK_BUTTON_SECONDARY && n_press == 1)
    {
        GtkWidget *popup = create_terminal_popup_menu (self);
        gtk_popover_set_position (GTK_POPOVER (popup), GTK_POS_BOTTOM);
        gtk_popover_set_pointing_to (GTK_POPOVER (popup), &(GdkRectangle){ x, y, 1, 1 });
        gtk_popover_popup (GTK_POPOVER (popup));
        return TRUE;
    }

    return FALSE;
}

/* Implement the spawn_terminal_in_widget function to fix linker errors */
void
spawn_terminal_in_widget (NautilusTerminalWidget *self)
{
    VteTerminal *terminal = VTE_TERMINAL (self->terminal);
    g_auto (GStrv) env = NULL;
    g_autoptr (GError) error = NULL;
    const char *shell;
    char *working_directory = NULL;

    /* Clear the needs_respawn flag */
    self->needs_respawn = FALSE;

    /* Get the default shell */
    shell = g_getenv ("SHELL");
    if (shell == NULL)
    {
        shell = "/bin/bash";
    }

    /* Use current location as working directory if available */
    if (self->current_location != NULL)
    {
        working_directory = g_file_get_path (self->current_location);
    }

    /* Environment setup */
    env = g_get_environ ();

    /* OSC7 command to enable directory tracking */
    const char *osc7_command = "echo -en \"\\033]7;file://$PWD\\007\"";

    /* Check if existing PROMPT_COMMAND already has OSC7 support */
    const char *existing_prompt_cmd = g_environ_getenv (env, "PROMPT_COMMAND");
    if (existing_prompt_cmd != NULL)
    {
        /* Only add OSC7 support if not already present */
        if (strstr (existing_prompt_cmd, "\\033]7;file://") == NULL &&
            strstr (existing_prompt_cmd, "7;file://") == NULL)
        {
            /* Append OSC7 support to existing PROMPT_COMMAND */
            char *new_prompt_cmd = g_strdup_printf ("%s; %s",
                                                    existing_prompt_cmd,
                                                    osc7_command);
            env = g_environ_setenv (env, "PROMPT_COMMAND", new_prompt_cmd, TRUE);
            g_free (new_prompt_cmd);
        }
    }
    else
    {
        /* No existing PROMPT_COMMAND, create it with OSC7 support */
        env = g_environ_setenv (env, "PROMPT_COMMAND", osc7_command, TRUE);
    }


    /* Start the shell */
    vte_terminal_spawn_async (terminal,
                              VTE_PTY_DEFAULT,
                              working_directory, /* Start in Nautilus's current directory */
                              (char *[]) { (char *) shell, NULL },
                              env,          /* environment */
                              G_SPAWN_SEARCH_PATH,
                              NULL, NULL,   /* child setup */
                              NULL,         /* child pid */
                              -1,           /* timeout */
                              NULL,         /* cancellable */
                              NULL,
                              self);

    g_free (working_directory);
    /*  print working_directory on terminal to debug */
    g_message ("Working directory: %s", working_directory ? working_directory : "NULL");
}

/**
 * nautilus_terminal_widget_ensure_terminal_focus:
 * @self: a #NautilusTerminalWidget
 *
 * Ensures that the terminal gets focus after location change.
 * This is useful when a directory change was initiated from the terminal,
 * to guarantee it retains focus after the Nautilus view changes.
 */
void
nautilus_terminal_widget_ensure_terminal_focus (NautilusTerminalWidget *self)
{
    g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

    /* Cancel any pending focus grab first */
    if (self->focus_timeout_id > 0)
    {
        g_source_remove (self->focus_timeout_id);
        self->focus_timeout_id = 0;
    }

    /* Set maintain_focus flag to ensure focus is kept */
    self->maintain_focus = TRUE;

    /* Grab focus with a slightly longer delay to ensure it happens after
     * any Nautilus view operations are complete */
    self->focus_timeout_id = g_timeout_add (100, focus_once_and_remove, self->terminal);
}

/**
 * nautilus_terminal_widget_get_has_focus:
 * @self: a #NautilusTerminalWidget
 *
 * Checks whether the terminal inside this widget has focus.
 *
 * Returns: %TRUE if the terminal has focus, %FALSE otherwise
 */
gboolean
nautilus_terminal_widget_get_has_focus (NautilusTerminalWidget *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self), FALSE);

    if (self->terminal)
    {
        return gtk_widget_has_focus (GTK_WIDGET (self->terminal));
    }

    return FALSE;
}
