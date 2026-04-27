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

#include <config.h>

#include "nautilus-terminal-widget.h"

#include <glib/gi18n.h>
#include <adwaita.h>
#include <vte/vte.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Terminal-widget strings live in their own gettext domain so they can be
 * shipped independently of the main nautilus.mo (see po/big-nautilus-terminal-*.po
 * and po/generate_terminal_translations.py). */
#define GETTEXT_PACKAGE_TERMINAL "nautilus-terminal"
#define _T(s) dgettext (GETTEXT_PACKAGE_TERMINAL, (s))

/* UI constants */
#define MIN_TERMINAL_HEIGHT 100
#define MIN_FONT_SIZE 6
#define MAX_FONT_SIZE 72

/* GObject properties */
enum
{
  PROP_0,
  PROP_CURRENT_LOCATION,
  N_PROPS
};

/* GObject signals */
enum
{
  CHANGE_DIRECTORY,
  TOGGLE_VISIBILITY,
  LAST_SIGNAL
};

typedef enum
{
  NAUTILUS_TERMINAL_STATE_LOCAL,
  NAUTILUS_TERMINAL_STATE_IN_SSH,
} NautilusTerminalState;

struct _NautilusTerminalWidgetPrivate
{
  GtkWidget   *scrolled_window;
  VteTerminal *terminal;
  GtkWidget   *ssh_indicator;
  GtkWidget   *popup_menu;
  GActionGroup *action_group;

  NautilusTerminalState state;
  gboolean            is_visible;
  gboolean            needs_respawn;
  gboolean            ignore_next_terminal_cd_signal;
  gboolean            just_synced_directory;
  gboolean            refocus_after_paste;
  guint               focus_timeout_id;
  GPid                child_pid;
  GCancellable       *spawn_cancellable;
  GWeakRef            paned_weak_ref;

  GSettings            *settings;
  gchar                *color_scheme;
  NautilusTerminalSyncMode  local_sync_mode;
  NautilusTerminalSshAutoConnectMode ssh_auto_connect_mode;

  GFile *current_location;

  gchar                 *ssh_hostname;
  gchar                 *ssh_username;
  gchar                 *ssh_port;
  gchar                 *ssh_remote_path;
  NautilusTerminalSyncMode   ssh_sync_mode;

  gchar                 *pending_ssh_hostname;
  gchar                 *pending_ssh_username;
  gchar                 *pending_ssh_port;
  NautilusTerminalSyncMode   pending_ssh_sync_mode;
};

typedef struct _NautilusTerminalWidgetPrivate NautilusTerminalWidgetPrivate;

struct _NautilusTerminalWidget {
    GtkBox parent_instance;
    NautilusTerminalWidgetPrivate *priv;
};

static const gchar *const SHELL_CTRL_A = "\x01";
static const gchar *const SHELL_CTRL_K = "\x0B";
static const gchar *const SHELL_CTRL_Y = "\x19";
static const gchar *const SHELL_CTRL_E = "\x05";
static const gchar *const SHELL_DELETE = "\033[3~";

typedef struct
{
  GdkRGBA  foreground;
  GdkRGBA  background;
  GdkRGBA  palette[16];
  gboolean use_system_colors;
} NautilusTerminalColorPalette;

#define RGB(r, g, b) ((GdkRGBA) { .red = (r), .green = (g), .blue = (b), .alpha = 1.0 })

typedef struct
{
  const gchar                     *id;
  const gchar                     *label_pot;
  const NautilusTerminalColorPalette   palette;
} MenuSchemeEntry;

/* clang-format off */
static const MenuSchemeEntry COLOR_SCHEME_ENTRIES[] = {
  { "system", N_("System"), .palette = { .use_system_colors = TRUE } },
  { "dark", N_("Dark"), .palette = {
      .foreground = RGB(0.9, 0.9, 0.9), .background = RGB(0.12, 0.12, 0.12),
      .palette = {
        RGB(0.0, 0.0, 0.0), RGB(0.8, 0.0, 0.0), RGB(0.0, 0.8, 0.0), RGB(0.8, 0.8, 0.0),
        RGB(0.0, 0.0, 0.8), RGB(0.8, 0.0, 0.8), RGB(0.0, 0.8, 0.8), RGB(0.8, 0.8, 0.8),
        RGB(0.5, 0.5, 0.5), RGB(1.0, 0.4, 0.4), RGB(0.4, 1.0, 0.4), RGB(1.0, 1.0, 0.4),
        RGB(0.4, 0.4, 1.0), RGB(1.0, 0.4, 1.0), RGB(0.4, 1.0, 1.0), RGB(1.0, 1.0, 1.0)
      }
    }
  },
  { "light", N_("Light"), .palette = {
      .foreground = RGB(0.15, 0.15, 0.15), .background = RGB(0.98, 0.98, 0.98),
      .palette = {
        RGB(0.2, 0.2, 0.2), RGB(0.8, 0.2, 0.2), RGB(0.1, 0.6, 0.1), RGB(0.7, 0.6, 0.1),
        RGB(0.2, 0.4, 0.7), RGB(0.6, 0.3, 0.5), RGB(0.3, 0.6, 0.7), RGB(0.7, 0.7, 0.7),
        RGB(0.4, 0.4, 0.4), RGB(0.9, 0.3, 0.3), RGB(0.2, 0.7, 0.2), RGB(0.8, 0.7, 0.2),
        RGB(0.3, 0.5, 0.8), RGB(0.7, 0.4, 0.6), RGB(0.4, 0.7, 0.8), RGB(0.9, 0.9, 0.9)
      }
    }
  },
  { "solarized-dark", N_("Solarized Dark"), .palette = {
      .foreground = RGB(0.8235, 0.8588, 0.8706), .background = RGB(0.0000, 0.1686, 0.2118),
      .palette = {
        RGB(0.0275, 0.2118, 0.2588), RGB(0.8627, 0.1961, 0.1843), RGB(0.5216, 0.6000, 0.0000), RGB(0.7098, 0.5412, 0.0000),
        RGB(0.1490, 0.5451, 0.8235), RGB(0.8275, 0.2118, 0.5098), RGB(0.1647, 0.6314, 0.6000), RGB(0.9294, 0.9098, 0.8353),
        RGB(0.0000, 0.1686, 0.2118), RGB(0.8000, 0.2588, 0.2078), RGB(0.3725, 0.4235, 0.4314), RGB(0.4078, 0.4745, 0.4784),
        RGB(0.5137, 0.5804, 0.5843), RGB(0.4235, 0.4431, 0.6118), RGB(0.5804, 0.6078, 0.5373), RGB(0.9922, 0.9647, 0.8902)
      }
    }
  },
  { "solarized-light", N_("Solarized Light"), .palette = {
      .foreground = RGB(0.4000, 0.4784, 0.5098), .background = RGB(0.9922, 0.9647, 0.8902),
      .palette = {
        RGB(0.0275, 0.2118, 0.2588), RGB(0.8627, 0.1961, 0.1843), RGB(0.5216, 0.6000, 0.0000), RGB(0.7098, 0.5412, 0.0000),
        RGB(0.1490, 0.5451, 0.8235), RGB(0.8275, 0.2118, 0.5098), RGB(0.1647, 0.6314, 0.6000), RGB(0.9294, 0.9098, 0.8353),
        RGB(0.0000, 0.1686, 0.2118), RGB(0.8000, 0.2588, 0.2078), RGB(0.3725, 0.4235, 0.4314), RGB(0.4078, 0.4745, 0.4784),
        RGB(0.5137, 0.5804, 0.5843), RGB(0.4235, 0.4431, 0.6118), RGB(0.5804, 0.6078, 0.5373), RGB(0.8235, 0.8588, 0.8706)
      }
    }
  },
  { "matrix", N_("Matrix"), .palette = {
      .foreground = RGB(0.1, 0.9, 0.1), .background = RGB(0.0, 0.0, 0.0),
      .palette = {
        RGB(0.0, 0.0, 0.0), RGB(0.0, 0.5, 0.0), RGB(0.0, 0.8, 0.0), RGB(0.1, 0.6, 0.0),
        RGB(0.0, 0.4, 0.0), RGB(0.1, 0.5, 0.1), RGB(0.0, 0.7, 0.1), RGB(0.1, 0.9, 0.1),
        RGB(0.0, 0.3, 0.0), RGB(0.0, 0.6, 0.0), RGB(0.0, 1.0, 0.0), RGB(0.2, 0.7, 0.0),
        RGB(0.0, 0.5, 0.0), RGB(0.2, 0.6, 0.2), RGB(0.0, 0.8, 0.2), RGB(0.2, 1.0, 0.2)
      }
    }
  },
  { "one-half-dark", N_("One Half Dark"), .palette = {
      .foreground = RGB(0.870, 0.870, 0.870), .background = RGB(0.157, 0.168, 0.184),
      .palette = {
        RGB(0.157, 0.168, 0.184), RGB(0.882, 0.490, 0.470), RGB(0.560, 0.749, 0.450), RGB(0.941, 0.768, 0.470),
        RGB(0.400, 0.627, 0.850), RGB(0.768, 0.470, 0.800), RGB(0.341, 0.709, 0.729), RGB(0.870, 0.870, 0.870),
        RGB(0.400, 0.450, 0.500), RGB(0.882, 0.490, 0.470), RGB(0.560, 0.749, 0.450), RGB(0.941, 0.768, 0.470),
        RGB(0.400, 0.627, 0.850), RGB(0.768, 0.470, 0.800), RGB(0.341, 0.709, 0.729), RGB(0.970, 0.970, 0.970)
      }
    }
  },
  { "one-half-light", N_("One Half Light"), .palette = {
      .foreground = RGB(0.220, 0.240, 0.260), .background = RGB(0.980, 0.980, 0.980),
      .palette = {
        RGB(0.220, 0.240, 0.260), RGB(0.858, 0.200, 0.180), RGB(0.310, 0.600, 0.110), RGB(0.850, 0.588, 0.100),
        RGB(0.231, 0.490, 0.749), RGB(0.670, 0.270, 0.729), RGB(0.149, 0.639, 0.678), RGB(0.800, 0.800, 0.800),
        RGB(0.400, 0.400, 0.400), RGB(0.858, 0.200, 0.180), RGB(0.310, 0.600, 0.110), RGB(0.850, 0.588, 0.100),
        RGB(0.231, 0.490, 0.749), RGB(0.670, 0.270, 0.729), RGB(0.149, 0.639, 0.678), RGB(0.080, 0.080, 0.080)
      }
    }
  },
  { "monokai", N_("Monokai"), .palette = {
      .foreground = RGB(0.929, 0.925, 0.910), .background = RGB(0, 0, 0),
      .palette = {
        RGB(0.153, 0.157, 0.149), RGB(0.980, 0.149, 0.450), RGB(0.650, 0.890, 0.180), RGB(0.960, 0.780, 0.310),
        RGB(0.208, 0.580, 0.839), RGB(0.670, 0.380, 0.960), RGB(0.400, 0.950, 0.950), RGB(0.929, 0.925, 0.910),
        RGB(0.400, 0.400, 0.400), RGB(0.980, 0.149, 0.450), RGB(0.650, 0.890, 0.180), RGB(0.960, 0.780, 0.310),
        RGB(0.208, 0.580, 0.839), RGB(0.670, 0.380, 0.960), RGB(0.400, 0.950, 0.950), RGB(1.000, 1.000, 1.000)
      }
    }
  },
};
/* clang-format on */

typedef struct
{
  int size_pts;
} MenuFontSizeEntry;

static const MenuFontSizeEntry FONT_SIZE_ENTRIES[] = {
  { 9 }, { 10 }, { 11 }, { 12 }, { 13 }, { 14 }, { 15 }, { 16 },
  { 17 }, { 18 }, { 20 }, { 22 }, { 24 }, { 28 }, { 32 }, { 36 }, { 40 }, { 48 }
};

typedef struct
{
  NautilusTerminalSyncMode mode;
  const gchar         *label_pot;
} MenuSyncModeEntry;

static const MenuSyncModeEntry LOCAL_SYNC_MODE_ENTRIES[] = {
  { NAUTILUS_TERMINAL_SYNC_BOTH, N_("Sync Both Ways") },
  { NAUTILUS_TERMINAL_SYNC_FM_TO_TERM, N_("Sync File Manager → Terminal") },
  { NAUTILUS_TERMINAL_SYNC_TERM_TO_FM, N_("Sync Terminal → File Manager") },
  { NAUTILUS_TERMINAL_SYNC_NONE, N_("No Sync") }
};

typedef struct
{
  NautilusTerminalSshAutoConnectMode mode;
  const gchar                     *label_pot;
} MenuSshAutoConnectEntry;

static const MenuSshAutoConnectEntry SFTP_AUTO_CONNECT_ENTRIES[] = {
  { NAUTILUS_TERMINAL_SSH_AUTOCONNECT_OFF, N_("Do not connect automatically") },
  { NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_BOTH, N_("Automatically connect and sync both ways") },
  { NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_FM_TO_TERM, N_("Automatically connect and sync: File Manager → Terminal") },
  { NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_TERM_TO_FM, N_("Automatically connect and sync: Terminal → File Manager") },
  { NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_NONE, N_("Automatically connect without syncing") }
};

static const MenuSyncModeEntry MANUAL_SSH_SYNC_ENTRIES[] = {
  { NAUTILUS_TERMINAL_SYNC_BOTH, N_("Sync folder both ways") },
  { NAUTILUS_TERMINAL_SYNC_FM_TO_TERM, N_("Sync folder from File Manager → Terminal") },
  { NAUTILUS_TERMINAL_SYNC_TERM_TO_FM, N_("Sync folder from Terminal → File Manager") },
  { NAUTILUS_TERMINAL_SYNC_NONE, N_("No folder sync") }
};

static void spawn_terminal_async(NautilusTerminalWidget *self);
static void on_terminal_child_exited(VteTerminal *terminal, gint status, gpointer user_data);
static void on_terminal_button_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void on_terminal_primary_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static gboolean on_terminal_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean _ensure_focus_timeout(gpointer user_data);
static void on_terminal_directory_changed(VteTerminal *terminal, gpointer user_data);
static void on_terminal_preference_changed(GSettings *settings, const gchar *key, gpointer user_data);
static void setup_terminal_font(VteTerminal *terminal, GSettings *settings);
static void nautilus_terminal_widget_apply_color_scheme(NautilusTerminalWidget *self);
static void on_system_dark_changed(AdwStyleManager *style_manager, GParamSpec *pspec, gpointer user_data);
static void _initiate_ssh_connection(NautilusTerminalWidget *self, const gchar *hostname, const gchar *username, const gchar *port, NautilusTerminalSyncMode sync_mode);
static gboolean parse_gvfs_ssh_path(GFile *location, gchar **hostname, gchar **username, gchar **port);
static void change_directory_in_terminal(NautilusTerminalWidget *self, GFile *location);
static void _clear_ssh_connection_data(NautilusTerminalWidgetPrivate *priv);
static void _reset_to_local_state(NautilusTerminalWidget *self);
static const gchar * nautilus_terminal_widget_get_color_scheme(NautilusTerminalWidget *self);
static void nautilus_terminal_widget_set_color_scheme(NautilusTerminalWidget *self, const gchar *scheme);
static void update_popup_menu_model (NautilusTerminalWidget *self);

static GParamSpec *properties[N_PROPS];
static guint       signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(NautilusTerminalWidget, nautilus_terminal_widget, GTK_TYPE_BOX)

static void
nautilus_terminal_widget_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(object);

    switch (prop_id)
    {
        case PROP_CURRENT_LOCATION:
            nautilus_terminal_widget_set_current_location(self, g_value_get_object(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
nautilus_terminal_widget_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(object);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    switch (prop_id)
    {
        case PROP_CURRENT_LOCATION:
            g_value_set_object(value, priv->current_location);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
nautilus_terminal_widget_finalize(GObject *object)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(object);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr(GtkWidget) paned = g_weak_ref_get(&priv->paned_weak_ref);

    /* Disconnect all signal handlers to prevent callbacks on destroyed object */
    if (priv->terminal) {
        g_signal_handlers_disconnect_by_data(priv->terminal, self);
    }
    g_signal_handlers_disconnect_by_data(priv->settings, self);
    g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), self);
    g_clear_object(&priv->settings);
    g_clear_object(&priv->action_group);

    if (paned)
    {
        g_signal_handlers_disconnect_by_data(paned, self);
    }
    g_weak_ref_clear(&priv->paned_weak_ref);

    if (priv->focus_timeout_id > 0) {
        g_source_remove(priv->focus_timeout_id);
        priv->focus_timeout_id = 0;
    }

    g_cancellable_cancel(priv->spawn_cancellable);
    g_clear_object(&priv->spawn_cancellable);
    g_clear_object(&priv->current_location);
    g_clear_pointer(&priv->color_scheme, g_free);

    /* Clean up popup menu */
    if (priv->popup_menu) {
        gtk_widget_unparent(priv->popup_menu);
        priv->popup_menu = NULL;
    }

    _clear_ssh_connection_data(priv);

    G_OBJECT_CLASS(nautilus_terminal_widget_parent_class)->finalize(object);
}

static void
action_copy (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    vte_terminal_copy_clipboard_format (self->priv->terminal, VTE_FORMAT_TEXT);
}

static void
action_paste (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    priv->refocus_after_paste = TRUE;
    vte_terminal_paste_clipboard (priv->terminal);

    /* Directly schedule a focus grab after paste. The focus-leave handler
     * is unreliable because focus may not always leave during paste. */
    if (priv->focus_timeout_id > 0)
        g_source_remove (priv->focus_timeout_id);
    priv->focus_timeout_id = g_timeout_add (50, _ensure_focus_timeout, self);
}

static void
action_select_all (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    vte_terminal_select_all (self->priv->terminal);
}

static void
action_set_color_scheme_state (GSimpleAction *action, GVariant *state, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    const gchar *scheme_name = g_variant_get_string (state, NULL);
    nautilus_terminal_widget_set_color_scheme (self, scheme_name);
    g_simple_action_set_state (action, state);
}

static void
action_set_font_size_state (GSimpleAction *action, GVariant *state, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    int font_size_pts = g_variant_get_int32 (state);
    g_autoptr(PangoFontDescription) font_desc = pango_font_description_copy(vte_terminal_get_font(priv->terminal));
    pango_font_description_set_size(font_desc, font_size_pts * PANGO_SCALE);
    vte_terminal_set_font(priv->terminal, font_desc);
    g_settings_set_int(priv->settings, "font-size", font_size_pts);
    g_simple_action_set_state (action, state);
}

static void
action_set_local_sync_mode_state (GSimpleAction *action, GVariant *state, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    gint new_value = g_variant_get_int32 (state);
    g_settings_set_enum (priv->settings, "local-sync-mode", new_value);
    g_simple_action_set_state (action, state);
}

static void
action_set_ssh_auto_connect_mode_state (GSimpleAction *action, GVariant *state, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    gint new_value = g_variant_get_int32 (state);
    g_settings_set_enum (priv->settings, "ssh-auto-connect-mode", new_value);
    g_simple_action_set_state (action, state);
}

static void
action_ssh_disconnect (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH)
    {
        vte_terminal_feed_child (priv->terminal, " exit\n", -1);
    }
}

static void
action_manual_ssh_connect (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    g_auto(GStrv) parts = g_strsplit (g_variant_get_string (parameter, NULL), ";", 4);
    if (g_strv_length (parts) == 4)
    {
        const gchar *hostname = parts[0];
        const gchar *username = parts[1][0] ? parts[1] : NULL;
        const gchar *port = parts[2][0] ? parts[2] : NULL;
        NautilusTerminalSyncMode sync_mode = (NautilusTerminalSyncMode) g_ascii_strtoull(parts[3], NULL, 10);
        _initiate_ssh_connection(self, hostname, username, port, sync_mode);
    }
}


static const GActionEntry terminal_actions[] =
{
  { .name = "copy", .activate = action_copy },
  { .name = "paste", .activate = action_paste },
  { .name = "select-all", .activate = action_select_all },
  { .name = "ssh-disconnect", .activate = action_ssh_disconnect },
  { .name = "manual-ssh-connect", .parameter_type = "s", .activate = action_manual_ssh_connect },
  { .name = "set-color-scheme", .parameter_type = "s", .state = "\"system\"", .change_state = action_set_color_scheme_state },
  { .name = "set-font-size", .parameter_type = "i", .state = "12", .change_state = action_set_font_size_state },
  { .name = "set-local-sync-mode", .parameter_type = "i", .state = "3", .change_state = action_set_local_sync_mode_state },
  { .name = "set-ssh-auto-connect-mode", .parameter_type = "i", .state = "0", .change_state = action_set_ssh_auto_connect_mode_state },
};


static void
nautilus_terminal_widget_class_init(NautilusTerminalWidgetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = nautilus_terminal_widget_set_property;
    object_class->get_property = nautilus_terminal_widget_get_property;
    object_class->finalize = nautilus_terminal_widget_finalize;

    properties[PROP_CURRENT_LOCATION] =
        g_param_spec_object("current-location",
                            "Current Location",
                            "The GFile representing the current directory.",
                            G_TYPE_FILE,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    signals[CHANGE_DIRECTORY] =
        g_signal_new("change-directory",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL,
                     g_cclosure_marshal_VOID__OBJECT,
                     G_TYPE_NONE, 1, G_TYPE_FILE);

    signals[TOGGLE_VISIBILITY] =
        g_signal_new("toggle-visibility",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL,
                     g_cclosure_marshal_VOID__BOOLEAN,
                     G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

// Forward declaration for the new handler
static void on_terminal_focus_leave(GtkEventControllerFocus *controller, gpointer user_data);

static void
nautilus_terminal_widget_init(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = nautilus_terminal_widget_get_instance_private(self);
    g_autoptr(GtkCssProvider) provider = NULL;
    GtkGesture *click;
    GtkGesture *primary_click;
    GtkEventController *key_controller;
    GtkEventController *focus_controller;
    GSimpleAction *action;

    /* Bind our private gettext domain once. nautilus' main domain is bound by
     * nautilus-main.c; we layer our own on top so terminal strings are looked up
     * in nautilus-terminal.mo (installed alongside nautilus.mo in $LOCALEDIR). */
    static gsize textdomain_init = 0;
    if (g_once_init_enter (&textdomain_init))
    {
        bindtextdomain (GETTEXT_PACKAGE_TERMINAL, LOCALEDIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE_TERMINAL, "UTF-8");
        g_once_init_leave (&textdomain_init, 1);
    }

    self->priv = priv;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

    priv->state = NAUTILUS_TERMINAL_STATE_LOCAL;
    priv->needs_respawn = TRUE;
    priv->just_synced_directory = FALSE;
    priv->child_pid = -1;
    priv->spawn_cancellable = g_cancellable_new();
    g_weak_ref_init(&priv->paned_weak_ref, NULL);

    priv->scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(priv->scrolled_window, TRUE);
    gtk_widget_set_hexpand(priv->scrolled_window, TRUE);

    priv->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scroll_on_output(priv->terminal, FALSE);
    vte_terminal_set_scroll_on_keystroke(priv->terminal, TRUE);
    vte_terminal_set_scrollback_lines(priv->terminal, 10000);
    gtk_widget_set_can_focus(GTK_WIDGET(priv->terminal), TRUE);

    priv->ssh_indicator = gtk_label_new("SSH");
    gtk_widget_set_name(priv->ssh_indicator, "ssh-indicator");
    gtk_widget_set_visible(priv->ssh_indicator, FALSE);
    gtk_widget_set_vexpand(priv->ssh_indicator, FALSE);
    gtk_widget_set_hexpand(priv->ssh_indicator, TRUE);
    gtk_label_set_xalign(GTK_LABEL(priv->ssh_indicator), 0.5);

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, "label#ssh-indicator { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; padding: 2px 5px; margin: 0; font-weight: bold; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);

    gtk_box_append(GTK_BOX(self), priv->ssh_indicator);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(priv->scrolled_window), GTK_WIDGET(priv->terminal));
    gtk_box_append(GTK_BOX(self), priv->scrolled_window);

    priv->settings = g_settings_new("org.gnome.nautilus.terminal");
    priv->local_sync_mode = g_settings_get_enum(priv->settings, "local-sync-mode");
    priv->ssh_auto_connect_mode = g_settings_get_enum(priv->settings, "ssh-auto-connect-mode");

    setup_terminal_font(priv->terminal, priv->settings);
    nautilus_terminal_widget_get_color_scheme(self);
    nautilus_terminal_widget_apply_color_scheme(self);

    /* Listen for system dark/light theme changes */
    g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                     G_CALLBACK(on_system_dark_changed), self);

    priv->action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(priv->action_group), terminal_actions, G_N_ELEMENTS(terminal_actions), self);

    action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-local-sync-mode"));
    g_simple_action_set_state(action, g_variant_new_int32(priv->local_sync_mode));

    action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-ssh-auto-connect-mode"));
    g_simple_action_set_state(action, g_variant_new_int32(priv->ssh_auto_connect_mode));

    action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-color-scheme"));
    g_simple_action_set_state(action, g_variant_new_string(priv->color_scheme));

    action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-font-size"));
    g_simple_action_set_state(action, g_variant_new_int32(g_settings_get_int(priv->settings, "font-size")));

    gtk_widget_insert_action_group(GTK_WIDGET(self), "terminal", priv->action_group);

    g_signal_connect(priv->settings, "changed", G_CALLBACK(on_terminal_preference_changed), self);

    g_signal_connect(priv->terminal, "child-exited", G_CALLBACK(on_terminal_child_exited), self);
    g_signal_connect(priv->terminal, "current-directory-uri-changed", G_CALLBACK(on_terminal_directory_changed), self);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_terminal_button_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), GTK_EVENT_CONTROLLER(click));

    /* Primary click handler to ensure terminal gets focus when clicked */
    primary_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(primary_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(primary_click, "pressed", G_CALLBACK(on_terminal_primary_click), self);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), GTK_EVENT_CONTROLLER(primary_click));

    key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_terminal_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), key_controller);

    focus_controller = gtk_event_controller_focus_new();
    g_signal_connect(focus_controller, "leave", G_CALLBACK(on_terminal_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), focus_controller);

    /* Submenus open as nested popovers rather than sliding pages so the parent
     * width is dictated by its own top-level items, not by the longest label
     * inside any submenu. Height stays automatic from GTK. */
    priv->popup_menu = gtk_popover_menu_new_from_model_full (NULL, GTK_POPOVER_MENU_NESTED);
    gtk_popover_set_has_arrow (GTK_POPOVER (priv->popup_menu), FALSE);
    gtk_widget_add_css_class (priv->popup_menu, "menu");
    gtk_widget_set_parent(priv->popup_menu, GTK_WIDGET(self));

    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

// Helper to defer focus grab safely
static void
_deferred_focus_grab(gpointer user_data) {
    GtkWidget *widget = GTK_WIDGET(user_data);
    if (widget != NULL &&
        !gtk_widget_in_destruction(widget) &&
        gtk_widget_get_realized(widget) &&
        gtk_widget_get_visible(widget)) {
        gtk_widget_grab_focus(widget);
    }
}


static void
on_terminal_focus_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;

    if (priv->just_synced_directory || priv->refocus_after_paste)
    {
        priv->just_synced_directory = FALSE;
        priv->refocus_after_paste = FALSE;
        /*
         * UI/UX Rationale: Directly calling grab_focus can fail due to event loop race
         * conditions where Nautilus steals focus immediately after. By using a minimal,
         * one-shot timeout, we defer the focus grab until after the current event
         * processing cycle has completed, ensuring our action is the final one and
         * reliably returns focus to the terminal.
         */
        g_timeout_add_once(50, _deferred_focus_grab, priv->terminal);
    }
}

/* Primary click handler: ensure terminal grabs focus on any left click */
static void
on_terminal_primary_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (!gtk_widget_in_destruction(GTK_WIDGET(self)) && priv->terminal) {
        gtk_widget_grab_focus(GTK_WIDGET(priv->terminal));
    }
}

static void
spawn_async_callback(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
    {
        priv->child_pid = -1;
        return;
    }

    if (pid == -1)
    {
        g_warning("Failed to spawn terminal: %s", error ? error->message : "Unknown error");
        priv->needs_respawn = TRUE;
        priv->child_pid = -1;
    }
    else
    {
        priv->child_pid = pid;
        priv->needs_respawn = FALSE;
    }
}

static void
spawn_terminal_async (NautilusTerminalWidget *self)
{
  NautilusTerminalWidgetPrivate *priv = self->priv;
  g_autofree gchar            *working_directory = NULL;
  const gchar                 *shell_executable;
  gchar                      **argv = NULL;
  gchar                      **envp = NULL;

  g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

  if (priv->child_pid != -1)
    return;

  if (g_cancellable_is_cancelled (priv->spawn_cancellable))
    {
      g_clear_object (&priv->spawn_cancellable);
      priv->spawn_cancellable = g_cancellable_new ();
    }

  if (priv->pending_ssh_hostname != NULL)
    {
      _initiate_ssh_connection (self, priv->pending_ssh_hostname, priv->pending_ssh_username, priv->pending_ssh_port, priv->pending_ssh_sync_mode);
      g_clear_pointer (&priv->pending_ssh_hostname, g_free);
      g_clear_pointer (&priv->pending_ssh_username, g_free);
      g_clear_pointer (&priv->pending_ssh_port, g_free);
      return;
    }

  shell_executable = g_getenv ("SHELL");
  if (!shell_executable || *shell_executable == '\0')
    shell_executable = "/bin/sh";

  if (priv->current_location && g_file_query_exists (priv->current_location, NULL))
    {
      working_directory = g_file_get_path (priv->current_location);
    }

  if (g_str_has_suffix (shell_executable, "zsh"))
    {
      g_autofree gchar *config_dir = g_build_filename (g_get_user_config_dir (), "nautilus", "terminal", NULL);
      g_autofree gchar *zshrc_path = g_build_filename (config_dir, ".zshrc", NULL);
      g_autofree gchar *zshrc_content = NULL;
      g_autofree gchar *zdotdir_env = NULL;

      g_mkdir_with_parents (config_dir, 0700);

      zshrc_content = g_strdup_printf ("_nautilus_vte_update_cwd() { echo -en \"\\033]7;file://$PWD\\007\"; };\n"
                                       "typeset -a precmd_functions;\n"
                                       "precmd_functions+=(_nautilus_vte_update_cwd);\n"
                                       "[ -f \"$HOME/.zshrc\" ] && . \"$HOME/.zshrc\";\n");

      g_file_set_contents (zshrc_path, zshrc_content, -1, NULL);

      zdotdir_env = g_strdup_printf ("ZDOTDIR=%s", config_dir);
      gchar *zsh_envp[] = { "TERM=xterm-256color", "COLORTERM=truecolor", zdotdir_env, NULL };
      envp = g_strdupv (zsh_envp);
      argv = g_strsplit (shell_executable, " ", -1);
    }
  else /* Assume bash or other compatible shells */
    {
      argv = g_strsplit (shell_executable, " ", -1);
      gchar *bash_envp[] = {
        "TERM=xterm-256color",
        "PROMPT_COMMAND=echo -en \"\\033]7;file://$PWD\\007\"",
        "COLORTERM=truecolor",
        NULL
      };
      envp = g_strdupv (bash_envp);
    }

  vte_terminal_spawn_async (priv->terminal,
                            VTE_PTY_DEFAULT,
                            working_directory,
                            argv,
                            envp,
                            G_SPAWN_SEARCH_PATH,
                            NULL, /* child_setup */
                            NULL, /* child_setup_data */
                            NULL, /* child_setup_data_destroy */
                            -1,   /* timeout */
                            priv->spawn_cancellable,
                            (VteTerminalSpawnAsyncCallback) spawn_async_callback,
                            self);

  g_strfreev (argv);
  g_strfreev (envp);
}

static void
_clear_ssh_connection_data(NautilusTerminalWidgetPrivate *priv)
{
    g_clear_pointer(&priv->ssh_hostname, g_free);
    g_clear_pointer(&priv->ssh_username, g_free);
    g_clear_pointer(&priv->ssh_port, g_free);
    g_clear_pointer(&priv->ssh_remote_path, g_free);
    g_clear_pointer(&priv->pending_ssh_hostname, g_free);
    g_clear_pointer(&priv->pending_ssh_username, g_free);
    g_clear_pointer(&priv->pending_ssh_port, g_free);
}

static void
_reset_to_local_state(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    _clear_ssh_connection_data(priv);
    priv->ssh_sync_mode = NAUTILUS_TERMINAL_SYNC_NONE;

    priv->state = NAUTILUS_TERMINAL_STATE_LOCAL;
    gtk_widget_set_visible(priv->ssh_indicator, FALSE);
    priv->needs_respawn = TRUE;
}

static void
on_terminal_child_exited (VteTerminal *terminal,
                          gint         status,
                          gpointer     user_data)
{
  NautilusTerminalWidget        *self = NAUTILUS_TERMINAL_WIDGET (user_data);
  NautilusTerminalWidgetPrivate *priv = self->priv;

  if (gtk_widget_in_destruction (GTK_WIDGET (self)))
    {
      priv->child_pid = -1;
      return;
    }

  priv->child_pid = -1;

  /* If the terminal was in an SSH session, the end of the ssh process
   * triggers this signal. We reset the state back to local, which flags
   * the terminal for respawning a new local shell below. */
  if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH)
    {
      _reset_to_local_state (self); /* This sets needs_respawn = TRUE */
      if (priv->is_visible)
        {
          spawn_terminal_async (self);
        }
    }
  /*
   * If the local shell exits (e.g., user types 'exit'), we hide the
   * terminal widget and ensure it's marked for a full respawn on next show.
   */
  else if (priv->state == NAUTILUS_TERMINAL_STATE_LOCAL)
    {
      priv->needs_respawn = TRUE;

      /* If the user saw it exit, hide the pane. */
      if (priv->is_visible)
        {
          nautilus_terminal_widget_toggle_visible (self);
        }
    }
}

static void
on_terminal_preference_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GSimpleAction *action;

    if (g_strcmp0(key, "local-sync-mode") == 0)
    {
        priv->local_sync_mode = g_settings_get_enum(settings, "local-sync-mode");
        action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-local-sync-mode"));
        g_simple_action_set_state(action, g_variant_new_int32(priv->local_sync_mode));
    }
    else if (g_strcmp0(key, "ssh-auto-connect-mode") == 0)
    {
        priv->ssh_auto_connect_mode = g_settings_get_enum(settings, "ssh-auto-connect-mode");
        action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-ssh-auto-connect-mode"));
        g_simple_action_set_state(action, g_variant_new_int32(priv->ssh_auto_connect_mode));
    }
    else if (g_strcmp0(key, "font") == 0 || g_strcmp0(key, "font-size") == 0)
    {
        setup_terminal_font(priv->terminal, priv->settings);
        action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-font-size"));
        g_simple_action_set_state(action, g_variant_new_int32(g_settings_get_int(priv->settings, "font-size")));
    }
    else if (g_strcmp0(key, "color-scheme") == 0)
    {
        g_free(priv->color_scheme);
        priv->color_scheme = g_settings_get_string(settings, "color-scheme");
        nautilus_terminal_widget_apply_color_scheme(self);
        action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(priv->action_group), "set-color-scheme"));
        g_simple_action_set_state(action, g_variant_new_string(priv->color_scheme));
    }
}

static void
on_terminal_directory_changed(VteTerminal *terminal, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr(GFile) new_gfile_location = NULL;
    gboolean should_sync_to_fm = FALSE;
    const gchar *cwd_uri;

    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;

    cwd_uri = vte_terminal_get_current_directory_uri(terminal);
    if (!cwd_uri) return;

    if (priv->ignore_next_terminal_cd_signal)
    {
        priv->ignore_next_terminal_cd_signal = FALSE;
        return;
    }

    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH)
    {
        if (priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH || priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_TERM_TO_FM)
        {
            should_sync_to_fm = TRUE;
            if (g_str_has_prefix(cwd_uri, "file://"))
            {
                g_autofree gchar *local_path = g_filename_from_uri(cwd_uri, NULL, NULL);
                if (local_path && priv->ssh_hostname)
                {
                    g_autoptr(GString) sftp_uri = g_string_new("sftp://");
                    if (priv->ssh_username && *priv->ssh_username)
                        g_string_append_printf(sftp_uri, "%s@", priv->ssh_username);
                    g_string_append(sftp_uri, priv->ssh_hostname);
                    if (priv->ssh_port && *priv->ssh_port)
                        g_string_append_printf(sftp_uri, ":%s", priv->ssh_port);
                    g_string_append(sftp_uri, local_path);
                    new_gfile_location = g_file_new_for_uri(sftp_uri->str);
                }
            }
        }
    }
    else
    {
        if (priv->local_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH || priv->local_sync_mode == NAUTILUS_TERMINAL_SYNC_TERM_TO_FM)
        {
            should_sync_to_fm = TRUE;
            new_gfile_location = g_file_new_for_uri(cwd_uri);
        }
    }

    if (should_sync_to_fm && new_gfile_location && (priv->current_location == NULL || !g_file_equal(new_gfile_location, priv->current_location)))
    {
        g_set_object(&priv->current_location, new_gfile_location);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CURRENT_LOCATION]);

        /*
         * Prevent the terminal from stealing focus when a directory change
         * is initiated by the file manager, we only set the `just_synced_directory` flag
         * if the terminal had focus *before* the directory change was signaled. This ensures
         * that the focus is only reclaimed when the user was actively using the terminal.
         */
        priv->just_synced_directory = gtk_widget_has_focus(GTK_WIDGET(priv->terminal));

        g_signal_emit(self, signals[CHANGE_DIRECTORY], 0, new_gfile_location);
    }
}

static void
feed_cd_command(VteTerminal *terminal, const char *path)
{
    g_return_if_fail(VTE_IS_TERMINAL(terminal));
    g_return_if_fail(path != NULL);

    g_autofree gchar *quoted_path = g_shell_quote(path);
    g_autofree gchar *cd_command_str = g_strdup_printf(" cd %s\r", quoted_path);

    /*
     * Sequence of control characters provides a superior user experience
     * by changing the directory without destroying the command the user might be currently
     * typing. It works in shells with default Emacs-like keybindings (e.g., bash, zsh).
     *
     * The sequence is as follows:
     * 1. SHELL_CTRL_A: Move cursor to the beginning of the line.
     * 2. " ": Insert a space. This is a small hack to ensure the buffer is not empty
     *    and that CTRL+K has something to cut, just in case the line was empty.
     * 3. SHELL_CTRL_A: Move cursor to the beginning again.
     * 4. SHELL_CTRL_K: "Kill" (cut) the entire line, including the command the user was typing.
     *    This content is now in the shell's clipboard (kill ring).
     * 5. cd_command_str: Feed the "cd /path/to/new/dir\r" command and execute it.
     * 6. SHELL_CTRL_Y: "Yank" (paste) the killed text back into the prompt.
     * 7. SHELL_CTRL_A: Move to the beginning of the restored line.
     * 8. SHELL_DELETE: Delete the space inserted in step 2.
     * 9. SHELL_CTRL_E: Move cursor to the end of the line, fully restoring the user's state.
     */
    vte_terminal_feed_child(terminal, SHELL_CTRL_A, -1);
    vte_terminal_feed_child(terminal, " ", -1);
    vte_terminal_feed_child(terminal, SHELL_CTRL_A, -1);
    vte_terminal_feed_child(terminal, SHELL_CTRL_K, -1);
    vte_terminal_feed_child(terminal, cd_command_str, -1);
    vte_terminal_feed_child(terminal, SHELL_CTRL_Y, -1);
    vte_terminal_feed_child(terminal, SHELL_CTRL_A, -1);
    vte_terminal_feed_child(terminal, SHELL_DELETE, -1);
    vte_terminal_feed_child(terminal, SHELL_CTRL_E, -1);
}

static gboolean
on_terminal_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK))
    {
        switch (keyval)
        {
            case GDK_KEY_C:
            case GDK_KEY_c:
                vte_terminal_copy_clipboard_format(priv->terminal, VTE_FORMAT_TEXT);
                return TRUE;
            case GDK_KEY_V:
            case GDK_KEY_v:
                priv->refocus_after_paste = TRUE;
                vte_terminal_paste_clipboard(priv->terminal);
                /* Directly schedule focus grab after keyboard paste */
                if (priv->focus_timeout_id > 0)
                    g_source_remove (priv->focus_timeout_id);
                priv->focus_timeout_id = g_timeout_add (50, _ensure_focus_timeout, self);
                return TRUE;
            case GDK_KEY_A:
            case GDK_KEY_a:
                vte_terminal_select_all (priv->terminal);
                return TRUE;
        }
    }

    if (keyval == GDK_KEY_F4)
    {
        nautilus_terminal_widget_toggle_visible(self);
        return TRUE;
    }

    return FALSE;
}

static GMenuModel *
_build_color_scheme_submenu(NautilusTerminalWidget *self)
{
    GMenu *submenu = g_menu_new();
    for (gsize i = 0; i < G_N_ELEMENTS(COLOR_SCHEME_ENTRIES); ++i)
    {
        GMenuItem *item = g_menu_item_new (_T(COLOR_SCHEME_ENTRIES[i].label_pot), "terminal.set-color-scheme");
        g_menu_item_set_action_and_target_value(item, "terminal.set-color-scheme", g_variant_new_string(COLOR_SCHEME_ENTRIES[i].id));
        g_menu_append_item(submenu, item);
    }
    return G_MENU_MODEL(submenu);
}

static int
get_terminal_font_size(GSettings *settings)
{
    int saved_size_pts = g_settings_get_int(settings, "font-size");
    return CLAMP(saved_size_pts, MIN_FONT_SIZE, MAX_FONT_SIZE);
}

static GMenuModel *
_build_font_size_submenu(NautilusTerminalWidget *self)
{
    GMenu *submenu = g_menu_new();

    for (gsize i = 0; i < G_N_ELEMENTS(FONT_SIZE_ENTRIES); ++i)
    {
        g_autofree gchar *label = g_strdup_printf("%d", FONT_SIZE_ENTRIES[i].size_pts);
        GMenuItem *item = g_menu_item_new(label, "terminal.set-font-size");
        g_menu_item_set_action_and_target_value(item, "terminal.set-font-size", g_variant_new_int32(FONT_SIZE_ENTRIES[i].size_pts));
        g_menu_append_item(submenu, item);
    }
    return G_MENU_MODEL(submenu);
}

static GMenuModel *
_build_enum_pref_submenu(NautilusTerminalWidget *self, const void *entries_generic, gsize count, const gchar *action_name)
{
    GMenu *submenu = g_menu_new();
    const MenuSyncModeEntry *entries = entries_generic;

    for (gsize i = 0; i < count; ++i)
    {
        GMenuItem *item = g_menu_item_new(_T(entries[i].label_pot), action_name);
        g_menu_item_set_action_and_target_value(item, action_name, g_variant_new_int32(entries[i].mode));
        g_menu_append_item(submenu, item);
    }
    return G_MENU_MODEL(submenu);
}

static GMenuModel *
_build_local_sync_submenu(NautilusTerminalWidget *self)
{
    return _build_enum_pref_submenu(self, LOCAL_SYNC_MODE_ENTRIES, G_N_ELEMENTS(LOCAL_SYNC_MODE_ENTRIES), "terminal.set-local-sync-mode");
}

static GMenuModel *
_build_sftp_auto_connect_submenu(NautilusTerminalWidget *self)
{
    return _build_enum_pref_submenu(self, SFTP_AUTO_CONNECT_ENTRIES, G_N_ELEMENTS(SFTP_AUTO_CONNECT_ENTRIES), "terminal.set-ssh-auto-connect-mode");
}

static GMenuModel *
_build_manual_ssh_connect_submenu(NautilusTerminalWidget *self, const gchar *hostname, const gchar *username, const gchar *port)
{
    GMenu *submenu = g_menu_new();
    for (gsize i = 0; i < G_N_ELEMENTS(MANUAL_SSH_SYNC_ENTRIES); ++i)
    {
        GMenuItem *item = g_menu_item_new(_T(MANUAL_SSH_SYNC_ENTRIES[i].label_pot), "terminal.manual-ssh-connect");
        g_autofree gchar *target_str = g_strdup_printf("%s;%s;%s;%d", hostname, username ? username : "", port ? port : "", MANUAL_SSH_SYNC_ENTRIES[i].mode);
        g_menu_item_set_action_and_target_value(item, "terminal.manual-ssh-connect", g_variant_new_string(target_str));
        g_menu_append_item(submenu, item);
    }
    return G_MENU_MODEL(submenu);
}

static GMenuItem *
_menu_item_with_accel (const gchar *label, const gchar *action, const gchar *accel)
{
    GMenuItem *item = g_menu_item_new (label, action);
    if (accel != NULL)
        g_menu_item_set_attribute (item, "accel", "s", accel);
    return item;
}

static void
update_popup_menu_model (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr (GMenu) menu = g_menu_new ();
    g_autoptr (GMenu) clipboard_section = g_menu_new ();
    g_autoptr (GMenu) appearance_section = g_menu_new ();
    g_autoptr (GMenu) sync_section = g_menu_new ();
    gboolean is_sftp_location = FALSE;

    /* Section 1 — clipboard, with accelerator labels shown on the right. */
    {
        g_autoptr (GMenuItem) item = NULL;
        item = _menu_item_with_accel (_T("Copy"), "terminal.copy", "<Ctrl><Shift>c");
        g_menu_append_item (clipboard_section, item);
        g_clear_object (&item);
        item = _menu_item_with_accel (_T("Paste"), "terminal.paste", "<Ctrl><Shift>v");
        g_menu_append_item (clipboard_section, item);
        g_clear_object (&item);
        item = _menu_item_with_accel (_T("Select All"), "terminal.select-all", "<Ctrl><Shift>a");
        g_menu_append_item (clipboard_section, item);
    }
    g_menu_append_section (menu, NULL, G_MENU_MODEL (clipboard_section));

    /* Section 2 — appearance. */
    g_menu_append_submenu (appearance_section, _T("Color Scheme"), _build_color_scheme_submenu (self));
    g_menu_append_submenu (appearance_section, _T("Font Size"), _build_font_size_submenu (self));
    g_menu_append_section (menu, NULL, G_MENU_MODEL (appearance_section));

    /* Section 3 — synchronisation / SSH. */
    if (priv->current_location)
    {
        g_autofree gchar *scheme = g_file_get_uri_scheme (priv->current_location);
        if (scheme && g_strcmp0 (scheme, "sftp") == 0)
            is_sftp_location = TRUE;
    }

    g_menu_append_submenu (sync_section, _T("SSH Auto-Connect"), _build_sftp_auto_connect_submenu (self));
    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH)
    {
        g_menu_append (sync_section, _T("Disconnect from SSH"), "terminal.ssh-disconnect");
    }
    else
    {
        g_menu_append_submenu (sync_section, _T("Local Folder Sync"), _build_local_sync_submenu (self));
        if (is_sftp_location)
        {
            g_autofree gchar *hostname = NULL, *username = NULL, *port = NULL;
            if (parse_gvfs_ssh_path (priv->current_location, &hostname, &username, &port))
            {
                g_autofree gchar *label = g_strdup_printf (_T("SSH Connection to %s"), hostname);
                g_menu_append_submenu (sync_section, label, _build_manual_ssh_connect_submenu (self, hostname, username, port));
            }
        }
    }
    g_menu_append_section (menu, NULL, G_MENU_MODEL (sync_section));

    gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (priv->popup_menu), G_MENU_MODEL (menu));
}

static void
on_terminal_button_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    update_popup_menu_model(self);
    gtk_popover_set_pointing_to(GTK_POPOVER(priv->popup_menu), &(GdkRectangle){x, y, 1, 1});
    gtk_popover_popup(GTK_POPOVER(priv->popup_menu));
}

static gchar *
get_remote_path_from_sftp_gfile(GFile *location)
{
    g_autoptr(GUri) uri = NULL;
    g_autofree gchar *uri_str = g_file_get_uri(location);
    if (!uri_str)
        return NULL;

    uri = g_uri_parse(uri_str, G_URI_FLAGS_NONE, NULL);
    if (uri)
        return g_strdup(g_uri_get_path(uri));

    return NULL;
}

static void
change_directory_in_terminal(NautilusTerminalWidget *self, GFile *location)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autofree gchar *target_path = NULL;
    gboolean should_sync = FALSE;

    if (!priv->is_visible || priv->child_pid == -1 ||
        gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;

    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH)
    {
        if (priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH || priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_FM_TO_TERM)
        {
            should_sync = TRUE;
            target_path = get_remote_path_from_sftp_gfile(location);
        }
    }
    else
    {
        if (priv->local_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH || priv->local_sync_mode == NAUTILUS_TERMINAL_SYNC_FM_TO_TERM)
        {
            if (g_file_query_exists(location, NULL))
            {
                target_path = g_file_get_path(location);
                if (target_path != NULL)
                {
                    should_sync = TRUE;
                }
            }
        }
    }

    if (should_sync && target_path)
    {
        const gchar *term_uri = vte_terminal_get_current_directory_uri(priv->terminal);
        g_autoptr(GFile) term_gfile = term_uri ? g_file_new_for_uri(term_uri) : NULL;
        g_autofree gchar *term_path = term_gfile ? g_file_get_path(term_gfile) : NULL;

        if (term_path == NULL || g_strcmp0(term_path, target_path) != 0)
        {
            priv->ignore_next_terminal_cd_signal = TRUE;
            feed_cd_command(priv->terminal, target_path);
        }
    }
}

static gboolean
parse_gvfs_ssh_path(GFile *location, gchar **hostname, gchar **username, gchar **port)
{
    g_autoptr(GUri) uri = NULL;
    g_autofree gchar *uri_str = g_file_get_uri(location);

    *hostname = NULL;
    *username = NULL;
    *port = NULL;

    if (!uri_str)
        return FALSE;

    uri = g_uri_parse(uri_str, G_URI_FLAGS_NONE, NULL);
    if (uri && g_strcmp0(g_uri_get_scheme(uri), "sftp") == 0)
    {
        *hostname = g_strdup(g_uri_get_host(uri));
        if (g_uri_get_userinfo(uri))
            *username = g_strdup(g_uri_get_userinfo(uri));
        if (g_uri_get_port(uri) > 0)
            *port = g_strdup_printf("%d", g_uri_get_port(uri));
        return (*hostname != NULL);
    }

    return FALSE;
}

static void
_initiate_ssh_connection(NautilusTerminalWidget *self, const gchar *hostname, const gchar *username, const gchar *port, NautilusTerminalSyncMode sync_mode)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr(GPtrArray) argv_array = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *remote_cmd = NULL;
    GString *remote_cmd_builder;

    if (priv->child_pid != -1)
    {
        kill(priv->child_pid, SIGTERM);
        priv->child_pid = -1;
    }

    priv->state = NAUTILUS_TERMINAL_STATE_IN_SSH;
    priv->ssh_sync_mode = sync_mode;
    priv->ssh_hostname = g_strdup(hostname);
    priv->ssh_username = g_strdup(username);
    priv->ssh_port = g_strdup(port);

    if (priv->current_location)
        priv->ssh_remote_path = get_remote_path_from_sftp_gfile(priv->current_location);

    remote_cmd_builder = g_string_new("");
    if (priv->ssh_remote_path && *priv->ssh_remote_path)
    {
        g_autofree gchar *quoted_remote_path = g_shell_quote(priv->ssh_remote_path);
        g_string_append_printf(remote_cmd_builder, " cd %s; ", quoted_remote_path);
    }
    /*
     * To enable directory synchronization from terminal to file manager, we need the shell
     * to notify us of its current working directory. This is achieved by setting
     * PROMPT_COMMAND, which is a bash-specific feature. It executes the 'echo' command
     * before each prompt, sending a special OSC 7 escape sequence with the current directory URI.
     * Note: This will not work for other shells like zsh (which uses `precmd` hooks) or fish.
     */
    if (priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH || priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_TERM_TO_FM)
    {
        g_string_append(remote_cmd_builder, " export PROMPT_COMMAND='echo -en \"\\033]7;file://$PWD\\007\"'; ");
    }
    g_string_append(remote_cmd_builder, "$SHELL -l");
    remote_cmd = g_string_free(remote_cmd_builder, FALSE);

    g_ptr_array_add(argv_array, g_strdup("ssh"));
    g_ptr_array_add(argv_array, g_strdup("-t"));
    if (port && *port)
    {
        g_ptr_array_add(argv_array, g_strdup("-p"));
        g_ptr_array_add(argv_array, g_strdup(port));
    }
    if (username && *username)
        g_ptr_array_add(argv_array, g_strdup_printf("%s@%s", username, hostname));
    else
        g_ptr_array_add(argv_array, g_strdup(hostname));

    g_ptr_array_add(argv_array, g_strdup(remote_cmd));
    g_ptr_array_add(argv_array, NULL);

    gtk_widget_set_visible(priv->ssh_indicator, TRUE);

    priv->ignore_next_terminal_cd_signal = TRUE;

    vte_terminal_spawn_async(priv->terminal,
                            VTE_PTY_DEFAULT,
                            NULL,
                            (gchar **)argv_array->pdata,
                            NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL, NULL,
                            -1,
                            priv->spawn_cancellable,
                            (VteTerminalSpawnAsyncCallback)spawn_async_callback,
                            self);
}

static void
setup_terminal_font(VteTerminal *terminal, GSettings *settings)
{
    g_autoptr(PangoFontDescription) font_desc = NULL;
    g_autofree gchar *font_name = g_settings_get_string(settings, "font");
    int font_size = get_terminal_font_size(settings);

    if (font_name && *font_name) {
        font_desc = pango_font_description_from_string(font_name);
    } else {
        /* Read the system monospace font from GNOME settings */
        g_autoptr(GSettings) iface_settings = g_settings_new("org.gnome.desktop.interface");
        g_autofree gchar *system_font = g_settings_get_string(iface_settings, "monospace-font-name");
        if (system_font && *system_font) {
            font_desc = pango_font_description_from_string(system_font);
        } else {
            font_desc = pango_font_description_from_string("Monospace");
        }
    }

    pango_font_description_set_size(font_desc, font_size * PANGO_SCALE);
    vte_terminal_set_font(terminal, font_desc);
}

static const gchar *
nautilus_terminal_widget_get_color_scheme(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    if (priv->color_scheme == NULL) {
        priv->color_scheme = g_settings_get_string(priv->settings, "color-scheme");
    }
    return priv->color_scheme;
}

static void
nautilus_terminal_widget_set_color_scheme(NautilusTerminalWidget *self, const gchar *scheme)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_settings_set_string(priv->settings, "color-scheme", scheme);
    g_free(priv->color_scheme);
    priv->color_scheme = g_strdup(scheme);
    nautilus_terminal_widget_apply_color_scheme(self);
}

static void
nautilus_terminal_widget_apply_color_scheme(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    const gchar *scheme_id = nautilus_terminal_widget_get_color_scheme(self);
    const MenuSchemeEntry *scheme = NULL;
    GdkRGBA fg, bg;

    for (gsize i = 0; i < G_N_ELEMENTS(COLOR_SCHEME_ENTRIES); ++i) {
        if (g_strcmp0(COLOR_SCHEME_ENTRIES[i].id, scheme_id) == 0) {
            scheme = &COLOR_SCHEME_ENTRIES[i];
            break;
        }
    }

    if (scheme == NULL) { /* Fallback to system */
        scheme = &COLOR_SCHEME_ENTRIES[0];
    }

    if (scheme->palette.use_system_colors) {
        /* Use sensible defaults based on the system style preference */
        AdwStyleManager *style_manager = adw_style_manager_get_default();
        gboolean is_dark = adw_style_manager_get_dark(style_manager);

        /* Standard 16-color ANSI palette for proper terminal prompt rendering */
        GdkRGBA ansi_palette[16];
        if (is_dark) {
            fg = (GdkRGBA){ 0.9, 0.9, 0.9, 1.0 };
            bg = (GdkRGBA){ 0.12, 0.12, 0.12, 1.0 };
            ansi_palette[0]  = (GdkRGBA){ 0.20, 0.20, 0.20, 1.0 }; /* Black */
            ansi_palette[1]  = (GdkRGBA){ 0.80, 0.15, 0.15, 1.0 }; /* Red */
            ansi_palette[2]  = (GdkRGBA){ 0.20, 0.70, 0.20, 1.0 }; /* Green */
            ansi_palette[3]  = (GdkRGBA){ 0.75, 0.65, 0.10, 1.0 }; /* Yellow */
            ansi_palette[4]  = (GdkRGBA){ 0.30, 0.50, 0.80, 1.0 }; /* Blue */
            ansi_palette[5]  = (GdkRGBA){ 0.70, 0.30, 0.70, 1.0 }; /* Magenta */
            ansi_palette[6]  = (GdkRGBA){ 0.20, 0.65, 0.70, 1.0 }; /* Cyan */
            ansi_palette[7]  = (GdkRGBA){ 0.80, 0.80, 0.80, 1.0 }; /* White */
            ansi_palette[8]  = (GdkRGBA){ 0.45, 0.45, 0.45, 1.0 }; /* Bright Black */
            ansi_palette[9]  = (GdkRGBA){ 1.00, 0.35, 0.35, 1.0 }; /* Bright Red */
            ansi_palette[10] = (GdkRGBA){ 0.35, 0.90, 0.35, 1.0 }; /* Bright Green */
            ansi_palette[11] = (GdkRGBA){ 1.00, 0.90, 0.35, 1.0 }; /* Bright Yellow */
            ansi_palette[12] = (GdkRGBA){ 0.45, 0.65, 1.00, 1.0 }; /* Bright Blue */
            ansi_palette[13] = (GdkRGBA){ 0.90, 0.45, 0.90, 1.0 }; /* Bright Magenta */
            ansi_palette[14] = (GdkRGBA){ 0.35, 0.85, 0.90, 1.0 }; /* Bright Cyan */
            ansi_palette[15] = (GdkRGBA){ 1.00, 1.00, 1.00, 1.0 }; /* Bright White */
        } else {
            fg = (GdkRGBA){ 0.15, 0.15, 0.15, 1.0 };
            bg = (GdkRGBA){ 0.98, 0.98, 0.98, 1.0 };
            ansi_palette[0]  = (GdkRGBA){ 0.10, 0.10, 0.10, 1.0 }; /* Black */
            ansi_palette[1]  = (GdkRGBA){ 0.75, 0.10, 0.10, 1.0 }; /* Red */
            ansi_palette[2]  = (GdkRGBA){ 0.15, 0.55, 0.15, 1.0 }; /* Green */
            ansi_palette[3]  = (GdkRGBA){ 0.60, 0.50, 0.00, 1.0 }; /* Yellow */
            ansi_palette[4]  = (GdkRGBA){ 0.15, 0.35, 0.65, 1.0 }; /* Blue */
            ansi_palette[5]  = (GdkRGBA){ 0.55, 0.20, 0.55, 1.0 }; /* Magenta */
            ansi_palette[6]  = (GdkRGBA){ 0.10, 0.50, 0.55, 1.0 }; /* Cyan */
            ansi_palette[7]  = (GdkRGBA){ 0.70, 0.70, 0.70, 1.0 }; /* White */
            ansi_palette[8]  = (GdkRGBA){ 0.35, 0.35, 0.35, 1.0 }; /* Bright Black */
            ansi_palette[9]  = (GdkRGBA){ 0.85, 0.20, 0.20, 1.0 }; /* Bright Red */
            ansi_palette[10] = (GdkRGBA){ 0.25, 0.70, 0.25, 1.0 }; /* Bright Green */
            ansi_palette[11] = (GdkRGBA){ 0.80, 0.70, 0.15, 1.0 }; /* Bright Yellow */
            ansi_palette[12] = (GdkRGBA){ 0.25, 0.45, 0.80, 1.0 }; /* Bright Blue */
            ansi_palette[13] = (GdkRGBA){ 0.70, 0.30, 0.70, 1.0 }; /* Bright Magenta */
            ansi_palette[14] = (GdkRGBA){ 0.20, 0.65, 0.70, 1.0 }; /* Bright Cyan */
            ansi_palette[15] = (GdkRGBA){ 0.90, 0.90, 0.90, 1.0 }; /* Bright White */
        }
        vte_terminal_set_colors(priv->terminal, &fg, &bg, ansi_palette, 16);
    } else {
        vte_terminal_set_colors(priv->terminal,
                                &scheme->palette.foreground,
                                &scheme->palette.background,
                                (GdkRGBA *)scheme->palette.palette,
                                G_N_ELEMENTS(scheme->palette.palette));
    }
}

static void
on_system_dark_changed(AdwStyleManager *style_manager, GParamSpec *pspec, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;
    nautilus_terminal_widget_apply_color_scheme(self);
}

NautilusTerminalWidget *
nautilus_terminal_widget_new_with_location(GFile *location)
{
    return g_object_new(NAUTILUS_TYPE_TERMINAL_WIDGET, "current-location", location, NULL);
}

void
nautilus_terminal_widget_set_current_location(NautilusTerminalWidget *self, GFile *location)
{
    NautilusTerminalWidgetPrivate *priv;
    g_autofree gchar *hostname = NULL, *username = NULL, *port = NULL;
    g_autofree gchar *scheme = NULL;
    gboolean is_first_location_set;

    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;

    if (location == NULL) {
        return;
    }

    is_first_location_set = (priv->current_location == NULL);

    if (is_first_location_set || !g_file_equal(location, priv->current_location))
    {
        g_set_object(&priv->current_location, location);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CURRENT_LOCATION]);
    }
    else
    {
        /* Location is unchanged, no further action needed. */
        return;
    }

    /*
     * If this is the first time we're setting the location, it's part of the
     * initial setup. If the terminal should be visible but hasn't spawned, trigger the spawn now.
     * This ensures the terminal starts in the correct directory from the very beginning.
     */
    if (is_first_location_set && priv->is_visible && priv->needs_respawn) {
        spawn_terminal_async(self);
        return;
    }

    scheme = g_file_get_uri_scheme(location);
    if (scheme == NULL) {
        return;
    }

    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH) {
        if (g_strcmp0(scheme, "sftp") == 0 &&
            parse_gvfs_ssh_path(location, &hostname, &username, &port) &&
            priv->ssh_hostname && g_strcmp0(hostname, priv->ssh_hostname) == 0)
        {
            change_directory_in_terminal(self, location);
        }
    } else { /* Local state */
        if (g_strcmp0(scheme, "sftp") == 0 &&
            priv->ssh_auto_connect_mode != NAUTILUS_TERMINAL_SSH_AUTOCONNECT_OFF &&
            parse_gvfs_ssh_path(location, &hostname, &username, &port)) {

            NautilusTerminalSyncMode sync_mode;
            switch (priv->ssh_auto_connect_mode) {
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_BOTH: sync_mode = NAUTILUS_TERMINAL_SYNC_BOTH; break;
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_FM_TO_TERM: sync_mode = NAUTILUS_TERMINAL_SYNC_FM_TO_TERM; break;
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_TERM_TO_FM: sync_mode = NAUTILUS_TERMINAL_SYNC_TERM_TO_FM; break;
                default: sync_mode = NAUTILUS_TERMINAL_SYNC_NONE; break;
            }

            if (priv->child_pid != -1) {
                _initiate_ssh_connection(self, hostname, username, port, sync_mode);
            } else {
                priv->pending_ssh_hostname = g_strdup(hostname);
                priv->pending_ssh_username = g_strdup(username);
                priv->pending_ssh_port = g_strdup(port);
                priv->pending_ssh_sync_mode = sync_mode;
            }
        } else {
            change_directory_in_terminal(self, location);
        }
    }
}

void
nautilus_terminal_widget_set_container_paned(NautilusTerminalWidget *self, GtkWidget *paned)
{
    NautilusTerminalWidgetPrivate *priv;
    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;
    g_weak_ref_set(&priv->paned_weak_ref, paned);
}

void
nautilus_terminal_widget_toggle_visible(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv;
    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;

    priv->is_visible = !priv->is_visible;
    g_settings_set_boolean(priv->settings, "visible", priv->is_visible);

    if (!priv->is_visible) {
        g_autoptr(GtkWidget) paned = g_weak_ref_get(&priv->paned_weak_ref);
        if (paned && GTK_IS_PANED(paned)) {
            int position = gtk_paned_get_position(GTK_PANED(paned));
            int total_height = gtk_widget_get_height(paned);
            if (total_height > 0) {
                g_settings_set_int(priv->settings, "pane-size", total_height - position);
            }
        }
    }

    nautilus_terminal_widget_ensure_state(self);
    g_signal_emit(self, signals[TOGGLE_VISIBILITY], 0, priv->is_visible);
}

void
nautilus_terminal_widget_ensure_state(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv;
    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;

    priv->is_visible = g_settings_get_boolean(priv->settings, "visible");

    if (priv->is_visible) {
        /*
         * Defer spawning the terminal until we have a valid directory
         * from Nautilus. The initial spawn is now triggered by the first call to
         * `set_current_location`. This prevents the terminal from briefly appearing
         * in the wrong directory ($HOME) before quickly changing to the correct one,
         * resulting in a smoother, flicker-free startup.
         */
        if (priv->needs_respawn) {
            if (priv->current_location != NULL) {
                spawn_terminal_async(self);
            }
            /* If location is NULL, we wait for set_current_location to be called. */
        } else {
            /* Terminal is already running, just sync directory if needed when re-showing. */
            if (priv->current_location) {
                change_directory_in_terminal(self, priv->current_location);
            }
        }
        gtk_widget_set_visible(GTK_WIDGET(self), TRUE);
        nautilus_terminal_widget_apply_new_size(self);
        nautilus_terminal_widget_ensure_terminal_focus(self);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
    }
}

static gboolean
_ensure_focus_timeout(gpointer user_data)
{
    NautilusTerminalWidget *self = user_data;
    NautilusTerminalWidgetPrivate *priv = self->priv;
    priv->focus_timeout_id = 0;
    if (!gtk_widget_in_destruction(GTK_WIDGET(self)) && priv->is_visible && priv->terminal) {
        gtk_widget_grab_focus(GTK_WIDGET(priv->terminal));
    }
    return G_SOURCE_REMOVE;
}

void
nautilus_terminal_widget_ensure_terminal_focus(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv;
    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;

    if (priv->focus_timeout_id > 0) {
        g_source_remove(priv->focus_timeout_id);
    }
    priv->focus_timeout_id = g_timeout_add(100, _ensure_focus_timeout, self);
}

gboolean
nautilus_terminal_widget_get_visible(NautilusTerminalWidget *self)
{
    g_return_val_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self), FALSE);
    return self->priv->is_visible;
}

void
nautilus_terminal_widget_apply_new_size(NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv;
    g_autoptr(GtkWidget) paned = NULL;
    gint term_size;

    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));
    priv = self->priv;

    paned = g_weak_ref_get(&priv->paned_weak_ref);
    if (!paned || !GTK_IS_PANED(paned) || !gtk_widget_get_realized(paned)) {
        return;
    }

    if (priv->is_visible) {
        gint total_height = gtk_widget_get_height(paned);
        term_size = g_settings_get_int(priv->settings, "pane-size");

        if (term_size < MIN_TERMINAL_HEIGHT) {
            term_size = MIN_TERMINAL_HEIGHT;
        }
        gtk_paned_set_position(GTK_PANED(paned), total_height - term_size);
    }
}
