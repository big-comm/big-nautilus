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
#include <signal.h>

/* Terminal-widget strings use a separate gettext domain. Catalogs are built
 * from po/big-nautilus-terminal-*.po by Meson. */
#define GETTEXT_PACKAGE_TERMINAL "nautilus-terminal"
#define _T(s) dgettext (GETTEXT_PACKAGE_TERMINAL, (s))

/* UI constants */
#define MIN_TERMINAL_HEIGHT 100
#define MAX_TERMINAL_HEIGHT 4096
#define MIN_FONT_SIZE 6
#define MAX_FONT_SIZE 72
#define MAX_COMMAND_TITLE_CHARS 80

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
  RUNNING_COMMAND_CHANGED,
  LAST_SIGNAL
};

typedef enum
{
  NAUTILUS_TERMINAL_STATE_LOCAL,
  NAUTILUS_TERMINAL_STATE_SWITCHING_TO_SSH,
  NAUTILUS_TERMINAL_STATE_IN_SSH,
  NAUTILUS_TERMINAL_STATE_SWITCHING_TO_LOCAL,
} NautilusTerminalState;

typedef enum
{
  NAUTILUS_TERMINAL_SPAWN_LOCAL,
  NAUTILUS_TERMINAL_SPAWN_SSH,
  NAUTILUS_TERMINAL_SPAWN_SECONDARY,
} NautilusTerminalSpawnKind;

typedef struct
{
  GWeakRef widget_ref;
  NautilusTerminalSpawnKind kind;
  guint generation;
} NautilusTerminalSpawnData;

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
  gboolean            disposing;
  gboolean            primary_spawn_pending;
  gboolean            secondary_spawn_pending;
  guint               focus_timeout_id;
  guint               apply_size_retry_id;
  guint               apply_size_retries;
  guint               primary_spawn_generation;
  guint               secondary_spawn_generation;
  GPid                child_pid;
  GCancellable       *spawn_cancellable;
  GWeakRef            paned_weak_ref;
  GWeakRef            focus_target_weak_ref;

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

  /* Scrollback search bar (Ctrl+Shift+F). */
  GtkWidget   *search_revealer;
  GtkWidget   *search_entry;
  GtkWidget   *search_case_toggle;
  GtkWidget   *search_regex_toggle;

  /* Running-command indicator. Updated from VTE shell.preexec/postexec
   * termprops; the window-slot listens on the running-command-changed signal
   * to badge the tab title. */
  gboolean     command_running;
  gchar       *last_command_title;

  /* Split panes. The primary terminal lives in priv->terminal and owns all the
   * shared state (SSH, sync, settings, color scheme). A secondary pane, when
   * present, is always a local-shell terminal — no SSH, no FM sync — and shares
   * only display preferences. Maximum 2 panes; close-pane reverts to single.
   *
   *   split_paned == NULL                  → single-pane mode
   *   split_paned != NULL && secondary*    → two panes inside split_paned
   */
  GtkWidget   *primary_pane_box;     /* wraps scrolled_window + search bar */
  GtkWidget   *split_paned;          /* NULL when not split */
  VteTerminal *secondary_terminal;
  GtkWidget   *secondary_pane_box;
  GtkWidget   *secondary_scrolled;
  GPid         secondary_pid;
  GCancellable *secondary_cancellable;
  VteTerminal *active_terminal;      /* primary or secondary, follows focus */

  /* Set in on_terminal_button_pressed *before* gtk_popover_popup() and cleared
   * on the popover's "closed" signal. gtk_widget_get_visible() lags by one
   * event-loop tick after gtk_popover_popup(), and in SSH the remote prompt
   * re-emits OSC 7 fast enough to fire between popup() and the visibility flag
   * flipping — long enough to navigate the FM and dismiss the popover. */
  gboolean     popup_pending;
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
static void on_popup_menu_closed (GtkPopover *popover, gpointer user_data);
static void on_terminal_primary_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static gboolean on_terminal_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean _ensure_focus_timeout(gpointer user_data);
static void schedule_terminal_focus (NautilusTerminalWidget *self,
                                     VteTerminal           *terminal,
                                     guint                  delay_ms);
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
static void spawn_pending_ssh_connection (NautilusTerminalWidget *self);

/* A host or user taken from an sftp:// URI is attacker-influenced. Anything
 * starting with '-' would be parsed by ssh as an option, and ';' would break
 * the ";"-separated target of the manual-connect action apart. Whitespace and
 * control bytes have no business in either field. */
static gboolean
nautilus_terminal_ssh_field_is_safe (const char *value)
{
    if (value == NULL || *value == '\0')
    {
        return TRUE;
    }

    if (*value == '-')
    {
        return FALSE;
    }

    for (const char *p = value; *p != '\0'; p++)
    {
        if ((guchar) *p <= 0x20 || (guchar) *p == 0x7f || *p == ';')
        {
            return FALSE;
        }
    }

    return TRUE;
}
static const gchar * nautilus_terminal_widget_get_color_scheme(NautilusTerminalWidget *self);
static void nautilus_terminal_widget_set_color_scheme(NautilusTerminalWidget *self, const gchar *scheme);
static void update_popup_menu_model (NautilusTerminalWidget *self);
static GtkWidget *_build_search_bar (NautilusTerminalWidget *self);
static void _install_drop_target (NautilusTerminalWidget *self,
                                  VteTerminal           *terminal);
static void _on_termprop_changed (VteTerminal *terminal, const char *prop, gpointer user_data);
static void action_split_horizontal (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void action_split_vertical   (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void action_close_pane       (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void _attach_focus_tracker   (NautilusTerminalWidget *self, VteTerminal *term);
static void switch_to_local_terminal (NautilusTerminalWidget *self);

static GParamSpec *properties[N_PROPS];
static guint       signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(NautilusTerminalWidget, nautilus_terminal_widget, GTK_TYPE_BOX)

static NautilusTerminalSpawnData *
terminal_spawn_data_new (NautilusTerminalWidget    *self,
                         NautilusTerminalSpawnKind  kind,
                         guint                      generation)
{
    NautilusTerminalSpawnData *data = g_new0 (NautilusTerminalSpawnData, 1);

    g_weak_ref_init (&data->widget_ref, self);
    data->kind = kind;
    data->generation = generation;

    return data;
}

static void
terminal_spawn_data_free (NautilusTerminalSpawnData *data)
{
    g_weak_ref_clear (&data->widget_ref);
    g_free (data);
}

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
nautilus_terminal_widget_dispose(GObject *object)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(object);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr(GtkWidget) paned = g_weak_ref_get(&priv->paned_weak_ref);

    priv->disposing = TRUE;
    priv->primary_spawn_generation++;
    priv->secondary_spawn_generation++;

    /* Widget cleanup must happen in dispose, not finalize: by the time finalize
     * runs, the GtkBox parent dispose has already unparented our children and
     * priv->terminal is no longer a valid GObject. */
    if (priv->terminal && G_IS_OBJECT (priv->terminal)) {
        g_signal_handlers_disconnect_by_data(priv->terminal, self);
    }
    if (priv->secondary_terminal && G_IS_OBJECT (priv->secondary_terminal)) {
        g_signal_handlers_disconnect_by_data(priv->secondary_terminal, self);
    }
    if (priv->settings) {
        g_signal_handlers_disconnect_by_data(priv->settings, self);
    }
    g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), self);
    if (paned)
    {
        g_signal_handlers_disconnect_by_data(paned, self);
    }

    if (priv->focus_timeout_id > 0) {
        g_source_remove(priv->focus_timeout_id);
        priv->focus_timeout_id = 0;
    }
    g_clear_handle_id (&priv->apply_size_retry_id, g_source_remove);
    g_weak_ref_set (&priv->focus_target_weak_ref, NULL);

    if (priv->spawn_cancellable) {
        g_cancellable_cancel(priv->spawn_cancellable);
    }
    if (priv->secondary_cancellable != NULL) {
        g_cancellable_cancel (priv->secondary_cancellable);
    }
    if (priv->secondary_pid > 0) {
        kill (priv->secondary_pid, SIGHUP);
        priv->secondary_pid = -1;
    }
    if (priv->child_pid > 0) {
        kill (priv->child_pid, SIGHUP);
        priv->child_pid = -1;
    }

    /* popup_menu is parented to `self` via gtk_widget_set_parent (not added to
     * the box), so the GtkBox dispose chain won't auto-unparent it. */
    if (priv->popup_menu && GTK_IS_WIDGET (priv->popup_menu)) {
        gtk_widget_unparent(priv->popup_menu);
        priv->popup_menu = NULL;
    }

    priv->terminal = NULL;
    priv->secondary_terminal = NULL;
    priv->active_terminal = NULL;
    priv->primary_pane_box = NULL;
    priv->secondary_pane_box = NULL;
    priv->secondary_scrolled = NULL;
    priv->split_paned = NULL;

    G_OBJECT_CLASS(nautilus_terminal_widget_parent_class)->dispose(object);
}

static void
nautilus_terminal_widget_finalize(GObject *object)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(object);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    g_weak_ref_clear(&priv->paned_weak_ref);
    g_weak_ref_clear(&priv->focus_target_weak_ref);
    g_clear_object(&priv->settings);
    g_clear_object(&priv->action_group);
    g_clear_object(&priv->spawn_cancellable);
    g_clear_object(&priv->secondary_cancellable);
    g_clear_object(&priv->current_location);
    g_clear_pointer(&priv->color_scheme, g_free);
    g_clear_pointer(&priv->last_command_title, g_free);

    _clear_ssh_connection_data(priv);

    G_OBJECT_CLASS(nautilus_terminal_widget_parent_class)->finalize(object);
}

static VteTerminal *_active_terminal (NautilusTerminalWidget *self);

static void
action_copy (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    vte_terminal_copy_clipboard_format (_active_terminal (self), VTE_FORMAT_TEXT);
}

static void
action_paste (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    priv->refocus_after_paste = TRUE;
    vte_terminal_paste_clipboard (_active_terminal (self));

    /* Directly schedule a focus grab after paste. The focus-leave handler
     * is unreliable because focus may not always leave during paste. */
    schedule_terminal_focus (self, _active_terminal (self), 50);
}

static void
action_select_all (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    vte_terminal_select_all (_active_terminal (self));
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

    switch_to_local_terminal (self);
}

static void _terminal_search_update_regex (NautilusTerminalWidget *self);
static void _terminal_search_set_visible  (NautilusTerminalWidget *self, gboolean visible);

static void
action_search_toggle (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    gboolean now_visible = !gtk_revealer_get_reveal_child (GTK_REVEALER (priv->search_revealer));
    _terminal_search_set_visible (self, now_visible);
}

static void
action_search_next (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_update_regex (self);
    vte_terminal_search_find_next (_active_terminal (self));
}

static void
action_search_prev (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_update_regex (self);
    vte_terminal_search_find_previous (_active_terminal (self));
}

static void
action_manual_ssh_connect (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    const gchar *hostname = NULL;
    const gchar *username = NULL;
    const gchar *port = NULL;
    gint32 mode = 0;

    g_variant_get (parameter, "(&s&s&si)", &hostname, &username, &port, &mode);

    if (mode < 0 || mode > NAUTILUS_TERMINAL_SYNC_BOTH)
    {
        return;
    }

    _initiate_ssh_connection (self,
                              hostname,
                              (username != NULL && *username != '\0') ? username : NULL,
                              (port != NULL && *port != '\0') ? port : NULL,
                              (NautilusTerminalSyncMode) mode);
}


static const GActionEntry terminal_actions[] =
{
  { .name = "copy", .activate = action_copy },
  { .name = "paste", .activate = action_paste },
  { .name = "select-all", .activate = action_select_all },
  { .name = "ssh-disconnect", .activate = action_ssh_disconnect },
  { .name = "manual-ssh-connect", .parameter_type = "(sssi)", .activate = action_manual_ssh_connect },
  { .name = "set-color-scheme", .parameter_type = "s", .state = "\"system\"", .change_state = action_set_color_scheme_state },
  { .name = "set-font-size", .parameter_type = "i", .state = "12", .change_state = action_set_font_size_state },
  { .name = "set-local-sync-mode", .parameter_type = "i", .state = "3", .change_state = action_set_local_sync_mode_state },
  { .name = "set-ssh-auto-connect-mode", .parameter_type = "i", .state = "0", .change_state = action_set_ssh_auto_connect_mode_state },
  { .name = "search-toggle",   .activate = action_search_toggle    },
  { .name = "search-next",     .activate = action_search_next      },
  { .name = "search-prev",     .activate = action_search_prev      },
  { .name = "split-horizontal", .activate = action_split_horizontal },
  { .name = "split-vertical",  .activate = action_split_vertical   },
  { .name = "close-pane",      .activate = action_close_pane       },
};


static void
nautilus_terminal_widget_class_init(NautilusTerminalWidgetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = nautilus_terminal_widget_set_property;
    object_class->get_property = nautilus_terminal_widget_get_property;
    object_class->dispose = nautilus_terminal_widget_dispose;
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

    signals[RUNNING_COMMAND_CHANGED] =
        g_signal_new("running-command-changed",
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
    g_weak_ref_init(&priv->focus_target_weak_ref, NULL);

    priv->scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(priv->scrolled_window, TRUE);
    gtk_widget_set_hexpand(priv->scrolled_window, TRUE);

    priv->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scroll_on_output(priv->terminal, FALSE);
    vte_terminal_set_scroll_on_keystroke(priv->terminal, TRUE);
    vte_terminal_set_scrollback_lines(priv->terminal, 10000);
    gtk_widget_set_can_focus(GTK_WIDGET(priv->terminal), TRUE);

    priv->ssh_indicator = gtk_label_new("SSH");
    gtk_widget_add_css_class (priv->ssh_indicator, "nautilus-terminal-ssh-indicator");
    gtk_widget_set_visible(priv->ssh_indicator, FALSE);
    gtk_widget_set_vexpand(priv->ssh_indicator, FALSE);
    gtk_widget_set_hexpand(priv->ssh_indicator, TRUE);
    gtk_label_set_xalign(GTK_LABEL(priv->ssh_indicator), 0.5);

    gtk_box_append(GTK_BOX(self), priv->ssh_indicator);
    priv->search_revealer = _build_search_bar (self);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(priv->scrolled_window), GTK_WIDGET(priv->terminal));

    /* Wrap the primary pane's content (search bar + scrolled VTE) in its own
     * box so we can reparent it when entering/leaving split mode. */
    priv->primary_pane_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand (priv->primary_pane_box, TRUE);
    gtk_widget_set_hexpand (priv->primary_pane_box, TRUE);
    gtk_box_append (GTK_BOX (priv->primary_pane_box), priv->search_revealer);
    gtk_box_append (GTK_BOX (priv->primary_pane_box), priv->scrolled_window);
    gtk_box_append (GTK_BOX (self), priv->primary_pane_box);

    priv->active_terminal = priv->terminal;
    _attach_focus_tracker (self, priv->terminal);
    priv->secondary_pid = -1;

    priv->settings = g_settings_new("org.gnome.nautilus.terminal");
    priv->local_sync_mode = g_settings_get_enum(priv->settings, "local-sync-mode");
    priv->ssh_auto_connect_mode = g_settings_get_enum(priv->settings, "ssh-auto-connect-mode");

    setup_terminal_font(priv->terminal, priv->settings);
    nautilus_terminal_widget_get_color_scheme(self);
    nautilus_terminal_widget_apply_color_scheme(self);

    /* Listen for system dark/light theme changes */
    g_signal_connect_object (adw_style_manager_get_default (),
                             "notify::dark",
                             G_CALLBACK (on_system_dark_changed),
                             self,
                             0);

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

    g_signal_connect_object (priv->settings,
                             "changed",
                             G_CALLBACK (on_terminal_preference_changed),
                             self,
                             0);

    g_signal_connect_object (priv->terminal,
                             "child-exited",
                             G_CALLBACK (on_terminal_child_exited),
                             self,
                             0);
    g_signal_connect_object (priv->terminal,
                             "current-directory-uri-changed",
                             G_CALLBACK (on_terminal_directory_changed),
                             self,
                             0);
    g_signal_connect_object (priv->terminal,
                             "termprop-changed",
                             G_CALLBACK (_on_termprop_changed),
                             self,
                             0);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect_object (click,
                             "pressed",
                             G_CALLBACK (on_terminal_button_pressed),
                             self,
                             0);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), GTK_EVENT_CONTROLLER(click));

    /* Primary click handler to ensure terminal gets focus when clicked */
    primary_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(primary_click), GDK_BUTTON_PRIMARY);
    g_signal_connect_object (primary_click,
                             "pressed",
                             G_CALLBACK (on_terminal_primary_click),
                             self,
                             0);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), GTK_EVENT_CONTROLLER(primary_click));

    key_controller = gtk_event_controller_key_new();
    g_signal_connect_object (key_controller,
                             "key-pressed",
                             G_CALLBACK (on_terminal_key_pressed),
                             self,
                             0);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), key_controller);

    focus_controller = gtk_event_controller_focus_new();
    g_signal_connect_object (focus_controller,
                             "leave",
                             G_CALLBACK (on_terminal_focus_leave),
                             self,
                             0);
    gtk_widget_add_controller(GTK_WIDGET(priv->terminal), focus_controller);

    _install_drop_target (self, priv->terminal);

    /* Submenus open as nested popovers rather than sliding pages so the parent
     * width is dictated by its own top-level items, not by the longest label
     * inside any submenu. Height stays automatic from GTK. */
    priv->popup_menu = gtk_popover_menu_new_from_model_full (NULL, GTK_POPOVER_MENU_NESTED);
    gtk_popover_set_has_arrow (GTK_POPOVER (priv->popup_menu), FALSE);
    gtk_widget_add_css_class (priv->popup_menu, "menu");
    gtk_widget_set_parent(priv->popup_menu, GTK_WIDGET(self));
    g_signal_connect_object (priv->popup_menu,
                             "closed",
                             G_CALLBACK (on_popup_menu_closed),
                             self,
                             0);

    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

/* ---- Split panes -------------------------------------------------------- */

static VteTerminal *
_active_terminal (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    return priv->active_terminal != NULL ? priv->active_terminal : priv->terminal;
}

static void
_on_pane_focus_enter (GtkEventControllerFocus *controller, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    GtkWidget *w = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
    if (VTE_IS_TERMINAL (w))
        self->priv->active_terminal = VTE_TERMINAL (w);
}

static void
_attach_focus_tracker (NautilusTerminalWidget *self, VteTerminal *term)
{
    GtkEventController *fc = gtk_event_controller_focus_new ();
    g_signal_connect_object (fc,
                             "enter",
                             G_CALLBACK (_on_pane_focus_enter),
                             self,
                             0);
    gtk_widget_add_controller (GTK_WIDGET (term), fc);
}

static void
_secondary_spawn_cb (VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    NautilusTerminalSpawnData *data = user_data;
    NautilusTerminalWidget *self = g_weak_ref_get (&data->widget_ref);

    if (self == NULL)
    {
        if (pid > 0)
        {
            kill (pid, SIGHUP);
        }
        terminal_spawn_data_free (data);
        return;
    }

    NautilusTerminalWidgetPrivate *priv = self->priv;
    gboolean stale = priv->disposing ||
                     data->generation != priv->secondary_spawn_generation ||
                     terminal != priv->secondary_terminal;

    if (stale)
    {
        if (pid > 0)
        {
            kill (pid, SIGHUP);
        }
    }
    else
    {
        priv->secondary_spawn_pending = FALSE;
        if (pid == -1)
        {
            g_warning ("split pane: failed to spawn: %s",
                       error != NULL ? error->message : "unknown error");
        }
        else
        {
            priv->secondary_pid = pid;
        }
    }

    g_object_unref (self);
    terminal_spawn_data_free (data);
}

static void
_on_secondary_child_exited (VteTerminal *terminal, gint status, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);

    if (self->priv->disposing || terminal != self->priv->secondary_terminal ||
        self->priv->split_paned == NULL)
    {
        return;
    }

    /* Mirror primary behaviour: when the shell exits, collapse the split. */
    self->priv->secondary_pid = -1;
    g_action_group_activate_action (self->priv->action_group, "close-pane", NULL);
}

static gchar *
_get_local_cwd_for_split (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    if (priv->current_location != NULL && g_file_is_native (priv->current_location)) {
        gchar *p = g_file_get_path (priv->current_location);
        if (p != NULL) return p;
    }
    return g_strdup (g_get_home_dir ());
}

static GtkWidget *
_build_secondary_pane (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GtkWidget *box;
    g_autofree gchar *cwd = _get_local_cwd_for_split (self);
    const gchar *shell = g_getenv ("SHELL");
    if (shell == NULL || *shell == '\0') shell = "/bin/sh";
    gchar *argv[] = { (gchar *) shell, NULL };

    priv->secondary_terminal = VTE_TERMINAL (vte_terminal_new ());
    vte_terminal_set_scroll_on_output    (priv->secondary_terminal, FALSE);
    vte_terminal_set_scroll_on_keystroke (priv->secondary_terminal, TRUE);
    vte_terminal_set_scrollback_lines    (priv->secondary_terminal, 10000);
    /* Mirror primary appearance — font + colors *before* it is realized so the
     * pane doesn't flash with the default VTE black background. */
    vte_terminal_set_font (priv->secondary_terminal, vte_terminal_get_font (priv->terminal));
    gtk_widget_set_can_focus (GTK_WIDGET (priv->secondary_terminal), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (priv->secondary_terminal), TRUE);
    gtk_widget_set_hexpand (GTK_WIDGET (priv->secondary_terminal), TRUE);
    nautilus_terminal_widget_apply_color_scheme (self);

    priv->secondary_scrolled = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (priv->secondary_scrolled, TRUE);
    gtk_widget_set_hexpand (priv->secondary_scrolled, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (priv->secondary_scrolled), GTK_WIDGET (priv->secondary_terminal));

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand (box, TRUE);
    gtk_widget_set_hexpand (box, TRUE);
    gtk_box_append (GTK_BOX (box), priv->secondary_scrolled);
    priv->secondary_pane_box = box;

    /* Mirror primary's input handlers so the secondary pane behaves the same:
     * right-click menu, primary-click focus grab, key shortcuts. */
    {
        GtkGesture *click = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_SECONDARY);
        g_signal_connect_object (click,
                                 "pressed",
                                 G_CALLBACK (on_terminal_button_pressed),
                                 self,
                                 0);
        gtk_widget_add_controller (GTK_WIDGET (priv->secondary_terminal), GTK_EVENT_CONTROLLER (click));

        GtkGesture *primary_click = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (primary_click), GDK_BUTTON_PRIMARY);
        g_signal_connect_object (primary_click,
                                 "pressed",
                                 G_CALLBACK (on_terminal_primary_click),
                                 self,
                                 0);
        gtk_widget_add_controller (GTK_WIDGET (priv->secondary_terminal), GTK_EVENT_CONTROLLER (primary_click));

        GtkEventController *key_controller = gtk_event_controller_key_new ();
        g_signal_connect_object (key_controller,
                                 "key-pressed",
                                 G_CALLBACK (on_terminal_key_pressed),
                                 self,
                                 0);
        gtk_widget_add_controller (GTK_WIDGET (priv->secondary_terminal), key_controller);
    }

    g_clear_object (&priv->secondary_cancellable);
    priv->secondary_cancellable = g_cancellable_new ();
    priv->secondary_spawn_pending = TRUE;
    priv->secondary_spawn_generation++;
    vte_terminal_spawn_async (priv->secondary_terminal,
                              VTE_PTY_DEFAULT,
                              cwd,
                              argv,
                              NULL, /* envp */
                              G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL,
                              -1,
                              priv->secondary_cancellable,
                              _secondary_spawn_cb,
                              terminal_spawn_data_new (self,
                                                       NAUTILUS_TERMINAL_SPAWN_SECONDARY,
                                                       priv->secondary_spawn_generation));

    g_signal_connect_object (priv->secondary_terminal,
                             "child-exited",
                             G_CALLBACK (_on_secondary_child_exited),
                             self,
                             0);
    _attach_focus_tracker (self, priv->secondary_terminal);
    _install_drop_target (self, priv->secondary_terminal);

    return box;
}

static void
_split_pane (NautilusTerminalWidget *self, GtkOrientation orientation)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GtkWidget *new_pane;
    int total;

    if (priv->split_paned != NULL) return; /* max 2 panes */
    if (priv->primary_pane_box == NULL) return;
    if (gtk_widget_get_parent (priv->primary_pane_box) != GTK_WIDGET (self)) return;

    new_pane = _build_secondary_pane (self);

    priv->split_paned = gtk_paned_new (orientation);
    gtk_paned_set_resize_start_child  (GTK_PANED (priv->split_paned), TRUE);
    gtk_paned_set_resize_end_child    (GTK_PANED (priv->split_paned), TRUE);
    gtk_paned_set_shrink_start_child  (GTK_PANED (priv->split_paned), FALSE);
    gtk_paned_set_shrink_end_child    (GTK_PANED (priv->split_paned), FALSE);
    gtk_widget_set_vexpand (priv->split_paned, TRUE);
    gtk_widget_set_hexpand (priv->split_paned, TRUE);

    g_object_ref (priv->primary_pane_box);
    gtk_box_remove (GTK_BOX (self), priv->primary_pane_box);
    gtk_paned_set_start_child (GTK_PANED (priv->split_paned), priv->primary_pane_box);
    g_object_unref (priv->primary_pane_box);

    gtk_paned_set_end_child (GTK_PANED (priv->split_paned), new_pane);
    gtk_box_append (GTK_BOX (self), priv->split_paned);

    /* Place the divider in the middle. */
    total = orientation == GTK_ORIENTATION_HORIZONTAL
            ? gtk_widget_get_width (GTK_WIDGET (self))
            : gtk_widget_get_height (GTK_WIDGET (self));
    if (total > 0)
        gtk_paned_set_position (GTK_PANED (priv->split_paned), total / 2);

    gtk_widget_grab_focus (GTK_WIDGET (priv->secondary_terminal));
    priv->active_terminal = priv->secondary_terminal;
}

static void
_close_secondary_pane (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (priv->split_paned == NULL || priv->primary_pane_box == NULL) return;

    priv->secondary_spawn_generation++;
    priv->secondary_spawn_pending = FALSE;

    if (priv->secondary_terminal != NULL)
    {
        g_signal_handlers_disconnect_by_data (priv->secondary_terminal, self);
    }

    if (priv->secondary_pid > 0) {
        kill (priv->secondary_pid, SIGHUP);
        priv->secondary_pid = -1;
    }
    if (priv->secondary_cancellable != NULL) {
        g_cancellable_cancel (priv->secondary_cancellable);
        g_clear_object (&priv->secondary_cancellable);
    }

    g_object_ref (priv->primary_pane_box);
    if (gtk_widget_get_parent (priv->primary_pane_box) == priv->split_paned)
    {
        gtk_paned_set_start_child (GTK_PANED (priv->split_paned), NULL);
    }
    if (priv->secondary_pane_box != NULL &&
        gtk_widget_get_parent (priv->secondary_pane_box) == priv->split_paned)
    {
        gtk_paned_set_end_child (GTK_PANED (priv->split_paned), NULL);
    }
    if (gtk_widget_get_parent (priv->split_paned) == GTK_WIDGET (self))
    {
        gtk_box_remove (GTK_BOX (self), priv->split_paned);
    }
    priv->split_paned = NULL;
    priv->secondary_terminal = NULL;
    priv->secondary_scrolled = NULL;
    priv->secondary_pane_box = NULL;

    if (gtk_widget_get_parent (priv->primary_pane_box) == NULL)
    {
        gtk_box_append (GTK_BOX (self), priv->primary_pane_box);
    }
    g_object_unref (priv->primary_pane_box);

    priv->active_terminal = priv->terminal;
    gtk_widget_grab_focus (GTK_WIDGET (priv->terminal));
}

static void
action_split_horizontal (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    _split_pane (NAUTILUS_TERMINAL_WIDGET (user_data), GTK_ORIENTATION_HORIZONTAL);
}

static void
action_split_vertical (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    _split_pane (NAUTILUS_TERMINAL_WIDGET (user_data), GTK_ORIENTATION_VERTICAL);
}

static void
action_close_pane (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    _close_secondary_pane (NAUTILUS_TERMINAL_WIDGET (user_data));
}

/* ---- Running-command tracking (OSC 133 via VTE termprops) -------------- */

char *
nautilus_terminal_sanitize_title (const char *title)
{
    g_autofree char *valid_title = NULL;
    g_autoptr (GString) sanitized = NULL;
    const char *cursor;
    guint characters = 0;

    if (title == NULL)
    {
        return NULL;
    }

    valid_title = g_utf8_make_valid (title, -1);
    sanitized = g_string_new (NULL);

    for (cursor = valid_title;
         *cursor != '\0' && characters < MAX_COMMAND_TITLE_CHARS;
         cursor = g_utf8_next_char (cursor))
    {
        gunichar character = g_utf8_get_char (cursor);
        GUnicodeType character_type = g_unichar_type (character);

        if (g_unichar_isspace (character))
        {
            if (sanitized->len > 0 &&
                sanitized->str[sanitized->len - 1] != ' ')
            {
                g_string_append_c (sanitized, ' ');
                characters++;
            }
            continue;
        }

        if (g_unichar_iscntrl (character) ||
            character_type == G_UNICODE_FORMAT)
        {
            continue;
        }

        g_string_append_unichar (sanitized, character);
        characters++;
    }

    g_strstrip (sanitized->str);
    if (*sanitized->str == '\0')
    {
        return NULL;
    }

    g_string_truncate (sanitized, strlen (sanitized->str));
    return g_string_free (g_steal_pointer (&sanitized), FALSE);
}

static void
_emit_running_command_changed (NautilusTerminalWidget *self, gboolean running)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    if (priv->command_running == running)
        return;
    priv->command_running = running;
    g_signal_emit (self, signals[RUNNING_COMMAND_CHANGED], 0, running);
}

static void
_on_termprop_changed (VteTerminal *terminal, const char *prop, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (prop == NULL || gtk_widget_in_destruction (GTK_WIDGET (self)))
        return;

    if (g_strcmp0 (prop, VTE_TERMPROP_SHELL_PREEXEC) == 0)
    {
        _emit_running_command_changed (self, TRUE);
    }
    else if (g_strcmp0 (prop, VTE_TERMPROP_SHELL_POSTEXEC) == 0)
    {
        _emit_running_command_changed (self, FALSE);
    }
    else if (g_strcmp0 (prop, VTE_TERMPROP_XTERM_TITLE) == 0)
    {
        const char *title = vte_terminal_get_termprop_string (terminal, VTE_TERMPROP_XTERM_TITLE, NULL);
        g_clear_pointer (&priv->last_command_title, g_free);
        priv->last_command_title = nautilus_terminal_sanitize_title (title);
        /* Re-emit so listeners can refresh the badge text. */
        g_signal_emit (self, signals[RUNNING_COMMAND_CHANGED], 0, priv->command_running);
    }
}

const char *
nautilus_terminal_widget_get_last_command_title (NautilusTerminalWidget *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self), NULL);
    return self->priv->last_command_title;
}

gboolean
nautilus_terminal_widget_is_command_running (NautilusTerminalWidget *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self), FALSE);
    return self->priv->command_running;
}

/* ---- Drag & drop into terminal ------------------------------------------ */

gboolean
nautilus_terminal_ssh_identity_equal (const char *hostname_a,
                                      const char *username_a,
                                      const char *port_a,
                                      const char *hostname_b,
                                      const char *username_b,
                                      const char *port_b)
{
    const char *normalized_username_a =
        username_a != NULL && *username_a != '\0' ? username_a : g_get_user_name ();
    const char *normalized_username_b =
        username_b != NULL && *username_b != '\0' ? username_b : g_get_user_name ();
    const char *normalized_port_a =
        port_a != NULL && *port_a != '\0' ? port_a : "22";
    const char *normalized_port_b =
        port_b != NULL && *port_b != '\0' ? port_b : "22";

    return g_ascii_strcasecmp (hostname_a != NULL ? hostname_a : "",
                               hostname_b != NULL ? hostname_b : "") == 0 &&
           g_strcmp0 (normalized_username_a, normalized_username_b) == 0 &&
           g_strcmp0 (normalized_port_a, normalized_port_b) == 0;
}

static gchar *
_path_for_drop (NautilusTerminalWidget *self, GFile *file)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autofree gchar *scheme = g_file_get_uri_scheme (file);

    /* In SSH mode, accept sftp:// drops only if they point at the same host the
     * terminal is connected to; emit just the remote path. */
    if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH && scheme != NULL && g_strcmp0 (scheme, "sftp") == 0)
    {
        g_autofree gchar *hostname = NULL, *username = NULL, *port = NULL;
        if (parse_gvfs_ssh_path (file, &hostname, &username, &port) &&
            nautilus_terminal_ssh_identity_equal (hostname,
                                                  username,
                                                  port,
                                                  priv->ssh_hostname,
                                                  priv->ssh_username,
                                                  priv->ssh_port))
        {
            g_autofree gchar *uri = g_file_get_uri (file);
            g_autoptr (GUri) parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
            if (parsed != NULL)
                return g_strdup (g_uri_get_path (parsed));
        }
        return NULL;
    }

    /* Local file: only file:// makes sense in a local shell. */
    if (priv->state == NAUTILUS_TERMINAL_STATE_LOCAL)
    {
        gchar *path = g_file_get_path (file);
        return path; /* may be NULL for non-native; caller skips it. */
    }

    return NULL;
}

static gboolean
_on_terminal_drop (GtkDropTarget *target,
                   const GValue  *value,
                   double         x,
                   double         y,
                   gpointer       user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GtkWidget *drop_widget =
        gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
    VteTerminal *terminal = VTE_IS_TERMINAL (drop_widget)
                            ? VTE_TERMINAL (drop_widget)
                            : _active_terminal (self);
    g_autoptr (GString) feed = g_string_new (NULL);

    if (terminal == NULL ||
        (terminal == priv->terminal && priv->child_pid == -1) ||
        (terminal == priv->secondary_terminal && priv->secondary_pid == -1))
        return FALSE;

    if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    {
        GSList *files = g_value_get_boxed (value);
        for (GSList *l = files; l != NULL; l = l->next)
        {
            g_autofree gchar *path = _path_for_drop (self, l->data);
            if (path == NULL)
                continue;
            g_autofree gchar *quoted = g_shell_quote (path);
            if (feed->len > 0)
                g_string_append_c (feed, ' ');
            g_string_append (feed, quoted);
        }
    }
    else if (G_VALUE_HOLDS (value, G_TYPE_FILE))
    {
        g_autofree gchar *path = _path_for_drop (self, g_value_get_object (value));
        if (path != NULL) {
            g_autofree gchar *quoted = g_shell_quote (path);
            g_string_append (feed, quoted);
        }
    }
    else if (G_VALUE_HOLDS (value, G_TYPE_STRING))
    {
        /* Plain text drop: insert verbatim. */
        const char *s = g_value_get_string (value);
        if (s != NULL) g_string_append (feed, s);
    }

    if (feed->len == 0)
        return FALSE;

    /* Insert at the prompt without auto-executing. User reviews and presses Enter. */
    vte_terminal_feed_child (terminal, feed->str, feed->len);
    gtk_widget_grab_focus (GTK_WIDGET (terminal));
    return TRUE;
}

static void
_install_drop_target (NautilusTerminalWidget *self,
                      VteTerminal           *terminal)
{
    GtkDropTarget *target;
    GType types[] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE, G_TYPE_STRING };

    target = gtk_drop_target_new (G_TYPE_INVALID, GDK_ACTION_COPY);
    gtk_drop_target_set_gtypes (target, types, G_N_ELEMENTS (types));
    g_signal_connect_object (target,
                             "drop",
                             G_CALLBACK (_on_terminal_drop),
                             self,
                             0);
    gtk_widget_add_controller (GTK_WIDGET (terminal), GTK_EVENT_CONTROLLER (target));
}

/* ---- Scrollback search bar ---------------------------------------------- */

static void
_terminal_search_update_regex (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    const char *text = gtk_editable_get_text (GTK_EDITABLE (priv->search_entry));
    g_autoptr (VteRegex) regex = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *pattern = NULL;
    /* PCRE2 compile flags. VTE wraps PCRE2 but does not re-export its flag
     * constants; we OR these onto VTE_REGEX_FLAGS_DEFAULT. Values from pcre2.h. */
    enum { PCRE2_FLAG_CASELESS = 0x00000008u, PCRE2_FLAG_MULTILINE = 0x00000400u };
    guint32 flags = VTE_REGEX_FLAGS_DEFAULT | PCRE2_FLAG_MULTILINE;
    gboolean is_regex;
    gboolean case_sensitive;

    if (text == NULL || *text == '\0') {
        vte_terminal_search_set_regex (_active_terminal (self), NULL, 0);
        return;
    }

    is_regex = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->search_regex_toggle));
    case_sensitive = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->search_case_toggle));

    if (is_regex) {
        pattern = g_strdup (text);
    } else {
        /* Escape PCRE2 metacharacters so plain search is literal. */
        GString *s = g_string_new (NULL);
        for (const char *p = text; *p; p++) {
            if (strchr ("\\^$.|?*+()[]{}", *p))
                g_string_append_c (s, '\\');
            g_string_append_c (s, *p);
        }
        pattern = g_string_free (s, FALSE);
    }

    if (!case_sensitive)
        flags |= PCRE2_FLAG_CASELESS;

    regex = vte_regex_new_for_search (pattern, -1, flags, &error);
    if (regex == NULL) {
        g_debug ("terminal search regex error: %s", error ? error->message : "(unknown)");
        vte_terminal_search_set_regex (_active_terminal (self), NULL, 0);
        return;
    }
    vte_terminal_search_set_regex (_active_terminal (self), regex, 0);
    vte_terminal_search_set_wrap_around (_active_terminal (self), TRUE);
}

static void
_on_search_entry_changed (GtkSearchEntry *entry, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_update_regex (self);
    /* Live-jump to the next match while typing. */
    vte_terminal_search_find_next (_active_terminal (self));
}

static void
_on_search_entry_activate (GtkSearchEntry *entry, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_update_regex (self);
    vte_terminal_search_find_next (_active_terminal (self));
}

static void
_on_search_stop (GtkSearchEntry *entry, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_set_visible (self, FALSE);
}

static void
_on_search_toggle_clicked (GtkToggleButton *button, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET (user_data);
    _terminal_search_update_regex (self);
}

static void
_terminal_search_set_visible (NautilusTerminalWidget *self, gboolean visible)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    gtk_revealer_set_reveal_child (GTK_REVEALER (priv->search_revealer), visible);

    if (visible) {
        gtk_widget_grab_focus (priv->search_entry);
    } else {
        /* Clear highlight and return focus to the active pane's terminal. */
        VteTerminal *t = _active_terminal (self);
        if (t != NULL) {
            vte_terminal_search_set_regex (t, NULL, 0);
            vte_terminal_unselect_all (t);
        }
        if (t != NULL && !gtk_widget_in_destruction (GTK_WIDGET (self))) {
            gtk_widget_grab_focus (GTK_WIDGET (t));
        }
    }
}

static GtkWidget *
_build_search_bar (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GtkWidget *revealer;
    GtkWidget *bar;
    GtkWidget *prev_btn;
    GtkWidget *next_btn;
    GtkWidget *close_btn;

    revealer = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration (GTK_REVEALER (revealer), 120);
    gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);

    bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (bar, "toolbar");
    gtk_widget_set_margin_start  (bar, 4);
    gtk_widget_set_margin_end    (bar, 4);
    gtk_widget_set_margin_top    (bar, 2);
    gtk_widget_set_margin_bottom (bar, 2);

    priv->search_entry = gtk_search_entry_new ();
    gtk_widget_set_hexpand (priv->search_entry, TRUE);
    gtk_widget_set_tooltip_text (priv->search_entry, _T("Search terminal scrollback"));
    g_signal_connect_object (priv->search_entry,
                             "search-changed",
                             G_CALLBACK (_on_search_entry_changed),
                             self,
                             0);
    g_signal_connect_object (priv->search_entry,
                             "activate",
                             G_CALLBACK (_on_search_entry_activate),
                             self,
                             0);
    g_signal_connect_object (priv->search_entry,
                             "stop-search",
                             G_CALLBACK (_on_search_stop),
                             self,
                             0);
    gtk_box_append (GTK_BOX (bar), priv->search_entry);

    prev_btn = gtk_button_new_from_icon_name ("go-up-symbolic");
    gtk_widget_set_tooltip_text (prev_btn, _T("Previous match"));
    gtk_actionable_set_action_name (GTK_ACTIONABLE (prev_btn), "terminal.search-prev");
    gtk_box_append (GTK_BOX (bar), prev_btn);

    next_btn = gtk_button_new_from_icon_name ("go-down-symbolic");
    gtk_widget_set_tooltip_text (next_btn, _T("Next match"));
    gtk_actionable_set_action_name (GTK_ACTIONABLE (next_btn), "terminal.search-next");
    gtk_box_append (GTK_BOX (bar), next_btn);

    priv->search_case_toggle = gtk_toggle_button_new_with_label ("Aa");
    gtk_widget_set_tooltip_text (priv->search_case_toggle, _T("Match Case"));
    g_signal_connect_object (priv->search_case_toggle,
                             "toggled",
                             G_CALLBACK (_on_search_toggle_clicked),
                             self,
                             0);
    gtk_box_append (GTK_BOX (bar), priv->search_case_toggle);

    priv->search_regex_toggle = gtk_toggle_button_new_with_label (".*");
    gtk_widget_set_tooltip_text (priv->search_regex_toggle, _T("Regular Expression"));
    g_signal_connect_object (priv->search_regex_toggle,
                             "toggled",
                             G_CALLBACK (_on_search_toggle_clicked),
                             self,
                             0);
    gtk_box_append (GTK_BOX (bar), priv->search_regex_toggle);

    close_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_widget_set_tooltip_text (close_btn, _T("Close search"));
    gtk_actionable_set_action_name (GTK_ACTIONABLE (close_btn), "terminal.search-toggle");
    gtk_box_append (GTK_BOX (bar), close_btn);

    gtk_revealer_set_child (GTK_REVEALER (revealer), bar);
    return revealer;
}

static void
on_terminal_focus_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;

    /* In SSH, the remote prompt emits OSC 7 frequently, leaving
     * just_synced_directory=TRUE almost permanently. Without this guard, opening
     * the right-click popover triggers focus-leave, which would re-grab focus
     * 50ms later and dismiss the popover instantly. */
    if (priv->popup_pending)
    {
        priv->just_synced_directory = FALSE;
        priv->refocus_after_paste = FALSE;
        return;
    }

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
        GtkWidget *terminal = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

        if (VTE_IS_TERMINAL (terminal))
        {
            schedule_terminal_focus (self, VTE_TERMINAL (terminal), 50);
        }
    }
}

/* Primary click handler: focus whichever terminal was clicked (primary or
 * secondary), so the pane the user just clicked on receives keyboard input. */
static void
on_terminal_primary_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    GtkWidget *clicked = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

    if (!gtk_widget_in_destruction(GTK_WIDGET(self)) && clicked != NULL) {
        gtk_widget_grab_focus(clicked);
    }
}

static void
spawn_async_callback(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    NautilusTerminalSpawnData *data = user_data;
    NautilusTerminalWidget *self = g_weak_ref_get (&data->widget_ref);

    if (self == NULL)
    {
        if (pid > 0)
        {
            kill (pid, SIGHUP);
        }
        terminal_spawn_data_free (data);
        return;
    }

    NautilusTerminalWidgetPrivate *priv = self->priv;
    gboolean stale = priv->disposing ||
                     data->generation != priv->primary_spawn_generation ||
                     terminal != priv->terminal;

    if (stale)
    {
        if (pid > 0)
        {
            kill (pid, SIGHUP);
        }
    }
    else
    {
        priv->primary_spawn_pending = FALSE;
        if (pid == -1)
        {
            g_warning ("Failed to spawn terminal: %s",
                       error != NULL ? error->message : "unknown error");
            priv->needs_respawn = TRUE;
            priv->child_pid = -1;

            if (data->kind == NAUTILUS_TERMINAL_SPAWN_SSH)
            {
                _reset_to_local_state (self);
                if (priv->is_visible)
                {
                    spawn_terminal_async (self);
                }
            }
        }
        else
        {
            priv->child_pid = pid;
            priv->needs_respawn = FALSE;
            priv->state = data->kind == NAUTILUS_TERMINAL_SPAWN_SSH
                          ? NAUTILUS_TERMINAL_STATE_IN_SSH
                          : NAUTILUS_TERMINAL_STATE_LOCAL;

            if (data->kind == NAUTILUS_TERMINAL_SPAWN_SSH &&
                priv->current_location != NULL)
            {
                change_directory_in_terminal (self, priv->current_location);
            }
        }
    }

    g_object_unref (self);
    terminal_spawn_data_free (data);
}

static void
spawn_terminal_async (NautilusTerminalWidget *self)
{
  NautilusTerminalWidgetPrivate *priv = self->priv;
  g_autofree gchar            *working_directory = NULL;
  g_autofree gchar            *shell_basename = NULL;
  const gchar                 *shell_executable;
  gchar                      **argv = NULL;
  gchar                      **envp = NULL;

  g_return_if_fail (NAUTILUS_IS_TERMINAL_WIDGET (self));

  if (priv->child_pid != -1 || priv->primary_spawn_pending || priv->disposing)
    return;

  if (g_cancellable_is_cancelled (priv->spawn_cancellable))
    {
      g_clear_object (&priv->spawn_cancellable);
      priv->spawn_cancellable = g_cancellable_new ();
    }

  if (priv->pending_ssh_hostname != NULL)
    {
      spawn_pending_ssh_connection (self);
      return;
    }

  shell_executable = g_getenv ("SHELL");
  if (!shell_executable || *shell_executable == '\0')
    shell_executable = "/bin/sh";
  shell_basename = g_path_get_basename (shell_executable);
  envp = g_get_environ ();
  envp = g_environ_setenv (envp, "TERM", "xterm-256color", TRUE);
  envp = g_environ_setenv (envp, "COLORTERM", "truecolor", TRUE);

  if (priv->current_location != NULL && g_file_is_native (priv->current_location))
    {
      working_directory = g_file_get_path (priv->current_location);
    }

  if (g_strcmp0 (shell_basename, "zsh") == 0)
    {
      g_autofree gchar *config_dir = g_build_filename (g_get_user_config_dir (), "nautilus", "terminal", NULL);
      g_autofree gchar *zshrc_path = g_build_filename (config_dir, ".zshrc", NULL);
      g_autofree gchar *zshrc_content = NULL;
      g_autofree gchar *old_zshrc_content = NULL;
      g_autofree gchar *original_zdotdir_quoted = NULL;
      g_autofree gchar *zdotdir_env = NULL;
      g_autoptr (GError) error = NULL;
      const gchar *original_zdotdir = g_getenv ("ZDOTDIR");
      gboolean wrapper_ready = FALSE;

      if (original_zdotdir == NULL || *original_zdotdir == '\0' ||
          g_strcmp0 (original_zdotdir, config_dir) == 0)
        original_zdotdir = g_get_home_dir ();
      original_zdotdir_quoted = g_shell_quote (original_zdotdir);

      zshrc_content = g_strdup_printf ("typeset -g _NAUTILUS_ORIGINAL_ZDOTDIR=%s\n"
                                       "if [[ -r \"$_NAUTILUS_ORIGINAL_ZDOTDIR/.zshrc\" ]]; then\n"
                                       "  source \"$_NAUTILUS_ORIGINAL_ZDOTDIR/.zshrc\"\n"
                                       "fi\n"
                                       "autoload -Uz add-zsh-hook\n"
                                       "_nautilus_vte_update_cwd() { printf '\\033]7;file://%%s\\007' \"$PWD\"; }\n"
                                       "add-zsh-hook precmd _nautilus_vte_update_cwd\n",
                                       original_zdotdir_quoted);

      if (g_mkdir_with_parents (config_dir, 0700) == 0)
        {
          g_file_get_contents (zshrc_path, &old_zshrc_content, NULL, NULL);
          wrapper_ready = g_strcmp0 (old_zshrc_content, zshrc_content) == 0 ||
                          g_file_set_contents (zshrc_path, zshrc_content, -1, &error);
        }

      if (!wrapper_ready)
        {
          g_warning ("Failed to prepare zsh integration: %s",
                     error != NULL ? error->message : g_strerror (errno));
        }

      zdotdir_env = g_strdup (config_dir);
      if (wrapper_ready)
        {
          envp = g_environ_setenv (envp, "ZDOTDIR", zdotdir_env, TRUE);
        }
      argv = g_new0 (gchar *, 2);
      argv[0] = g_strdup (shell_executable);
    }
  else if (g_strcmp0 (shell_basename, "fish") == 0)
    {
      argv = g_new0 (gchar *, 4);
      argv[0] = g_strdup (shell_executable);
      argv[1] = g_strdup ("--init-command");
      argv[2] = g_strdup ("function _nautilus_vte_update_cwd --on-event fish_prompt; printf '\\033]7;file://%s\\007' $PWD; end");
    }
  else
    {
      g_autofree gchar *prompt_command_env = NULL;
      const gchar *existing_prompt_command = g_getenv ("PROMPT_COMMAND");

      argv = g_new0 (gchar *, 2);
      argv[0] = g_strdup (shell_executable);

      if (g_strcmp0 (shell_basename, "bash") == 0)
        {
          prompt_command_env = g_strdup_printf (
              "printf '\\033]7;file://%%s\\007' \"$PWD\"%s%s",
              existing_prompt_command != NULL && *existing_prompt_command != '\0' ? ";" : "",
              existing_prompt_command != NULL ? existing_prompt_command : "");
          envp = g_environ_setenv (envp,
                                   "PROMPT_COMMAND",
                                   prompt_command_env,
                                   TRUE);
        }
    }

  g_clear_object (&priv->spawn_cancellable);
  priv->spawn_cancellable = g_cancellable_new ();
  priv->primary_spawn_pending = TRUE;
  priv->primary_spawn_generation++;

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
                            spawn_async_callback,
                            terminal_spawn_data_new (self,
                                                     NAUTILUS_TERMINAL_SPAWN_LOCAL,
                                                     priv->primary_spawn_generation));

  g_strfreev (argv);
  g_strfreev (envp);
}

static void
clear_pending_ssh_connection_data (NautilusTerminalWidgetPrivate *priv)
{
    g_clear_pointer (&priv->pending_ssh_hostname, g_free);
    g_clear_pointer (&priv->pending_ssh_username, g_free);
    g_clear_pointer (&priv->pending_ssh_port, g_free);
}

static void
_clear_ssh_connection_data(NautilusTerminalWidgetPrivate *priv)
{
    g_clear_pointer(&priv->ssh_hostname, g_free);
    g_clear_pointer(&priv->ssh_username, g_free);
    g_clear_pointer(&priv->ssh_port, g_free);
    g_clear_pointer(&priv->ssh_remote_path, g_free);
    clear_pending_ssh_connection_data (priv);
}

static void
cancel_primary_spawn (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    priv->primary_spawn_generation++;
    priv->primary_spawn_pending = FALSE;
    if (priv->spawn_cancellable != NULL)
    {
        g_cancellable_cancel (priv->spawn_cancellable);
    }
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

  if (priv->disposing || terminal != priv->terminal)
    {
      priv->child_pid = -1;
      return;
    }

  priv->child_pid = -1;
  priv->needs_respawn = TRUE;
  _emit_running_command_changed (self, FALSE);

  if (priv->state == NAUTILUS_TERMINAL_STATE_SWITCHING_TO_SSH)
    {
      spawn_pending_ssh_connection (self);
    }
  else if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH ||
           priv->state == NAUTILUS_TERMINAL_STATE_SWITCHING_TO_LOCAL)
    {
      _reset_to_local_state (self);
      if (priv->is_visible)
        {
          spawn_terminal_async (self);
        }
    }
  else if (priv->state == NAUTILUS_TERMINAL_STATE_LOCAL)
    {
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
        if (priv->secondary_terminal != NULL)
        {
            setup_terminal_font (priv->secondary_terminal, priv->settings);
        }
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
    g_autoptr (GUri) cwd = NULL;
    g_autofree gchar *cwd_uri = NULL;
    gboolean should_sync_to_fm = FALSE;

    if (gtk_widget_in_destruction(GTK_WIDGET(self)))
        return;

    cwd = vte_terminal_ref_termprop_uri (terminal,
                                         VTE_TERMPROP_CURRENT_DIRECTORY_URI);
    if (cwd == NULL)
        return;
    cwd_uri = g_uri_to_string (cwd);

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
                    gint port = priv->ssh_port != NULL
                                ? (gint) g_ascii_strtoll (priv->ssh_port, NULL, 10)
                                : -1;
                    g_autoptr (GUri) sftp_uri =
                        g_uri_build_with_user (G_URI_FLAGS_NONE,
                                               "sftp",
                                               priv->ssh_username,
                                               NULL,
                                               NULL,
                                               priv->ssh_hostname,
                                               port,
                                               local_path,
                                               NULL,
                                               NULL);
                    g_autofree gchar *sftp_uri_string = g_uri_to_string (sftp_uri);

                    new_gfile_location = g_file_new_for_uri (sftp_uri_string);
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
        /*
         * If the right-click popover is open, suppress FM navigation: in SSH the
         * remote prompt re-emits OSC 7 on every redraw (oh-my-zsh, async prompts,
         * etc.). Each navigation rebuilds the FM view and grabs focus, which
         * dismisses the popover within milliseconds. The location will resync on
         * the next OSC 7 once the popover is closed.
         */
        if (priv->popup_pending)
            return;

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

    /* g_shell_quote() protects the shell parser, but this text is typed into
     * the pty, and the line discipline acts on control bytes before the shell
     * ever sees them. A directory named "x<Ctrl-C>rm -rf ~<LF>" would have the
     * Ctrl-C discard the quoted line and run the rest at a fresh prompt. Such
     * names are legal on Linux and can arrive from an archive or an SFTP
     * server, so refuse to sync rather than type them. */
    for (const char *p = path; *p != '\0'; p++)
    {
        if ((guchar) *p < 0x20 || (guchar) *p == 0x7f)
        {
            g_warning ("Refusing to sync a path containing control characters");
            return;
        }
    }

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
    GtkWidget *controller_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
    VteTerminal *terminal = VTE_IS_TERMINAL (controller_widget)
                            ? VTE_TERMINAL (controller_widget)
                            : _active_terminal (self);

    priv->active_terminal = terminal;

    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK))
    {
        switch (keyval)
        {
            case GDK_KEY_C:
            case GDK_KEY_c:
                vte_terminal_copy_clipboard_format (terminal, VTE_FORMAT_TEXT);
                return TRUE;
            case GDK_KEY_V:
            case GDK_KEY_v:
                priv->refocus_after_paste = TRUE;
                vte_terminal_paste_clipboard (terminal);
                /* Directly schedule focus grab after keyboard paste */
                schedule_terminal_focus (self, terminal, 50);
                return TRUE;
            case GDK_KEY_A:
            case GDK_KEY_a:
                vte_terminal_select_all (terminal);
                return TRUE;
            case GDK_KEY_F:
            case GDK_KEY_f:
                action_search_toggle (NULL, NULL, self);
                return TRUE;
            case GDK_KEY_O:
            case GDK_KEY_o:
                action_split_horizontal (NULL, NULL, self);
                return TRUE;
            case GDK_KEY_E:
            case GDK_KEY_e:
                action_split_vertical (NULL, NULL, self);
                return TRUE;
            case GDK_KEY_W:
            case GDK_KEY_w:
                action_close_pane (NULL, NULL, self);
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
        /* A tuple, not a ";"-joined string: a host or user containing ';'
         * used to shift the fields apart and let the caller control the
         * others. */
        GVariant *target = g_variant_new ("(sssi)",
                                          hostname,
                                          username ? username : "",
                                          port ? port : "",
                                          (gint32) MANUAL_SSH_SYNC_ENTRIES[i].mode);
        g_menu_item_set_action_and_target_value(item, "terminal.manual-ssh-connect", target);
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
        g_clear_object (&item);
        item = _menu_item_with_accel (_T("Find…"), "terminal.search-toggle", "<Ctrl><Shift>f");
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

    /* Section 4 — split panes. */
    {
        g_autoptr (GMenu) split_section = g_menu_new ();
        if (priv->split_paned == NULL) {
            g_autoptr (GMenuItem) it = NULL;
            it = _menu_item_with_accel (_T("Split Horizontally"), "terminal.split-horizontal", "<Ctrl><Shift>o");
            g_menu_append_item (split_section, it);
            g_clear_object (&it);
            it = _menu_item_with_accel (_T("Split Vertically"), "terminal.split-vertical", "<Ctrl><Shift>e");
            g_menu_append_item (split_section, it);
        } else {
            g_autoptr (GMenuItem) it = _menu_item_with_accel (_T("Close Pane"), "terminal.close-pane", "<Ctrl><Shift>w");
            g_menu_append_item (split_section, it);
        }
        g_menu_append_section (menu, NULL, G_MENU_MODEL (split_section));
    }

    gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (priv->popup_menu), G_MENU_MODEL (menu));
}

static void
on_popup_menu_closed (GtkPopover *popover, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    self->priv->popup_pending = FALSE;
}

static void
on_terminal_button_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    NautilusTerminalWidget *self = NAUTILUS_TERMINAL_WIDGET(user_data);
    NautilusTerminalWidgetPrivate *priv = self->priv;
    GtkWidget *clicked_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    /* Translate click coords to widget-self coordinates: the popover is
     * parented on `self`, not on the inner VteTerminal. */
    double sx = x, sy = y;
    if (clicked_widget != GTK_WIDGET (self))
    {
        graphene_point_t pt = GRAPHENE_POINT_INIT ((float) x, (float) y);
        graphene_point_t out;
        if (gtk_widget_compute_point (clicked_widget, GTK_WIDGET (self), &pt, &out))
        {
            sx = out.x;
            sy = out.y;
        }
    }

    priv->popup_pending = TRUE;
    update_popup_menu_model(self);
    gtk_popover_set_pointing_to(GTK_POPOVER(priv->popup_menu), &(GdkRectangle){(int) sx, (int) sy, 1, 1});
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
            if (g_file_is_native (location))
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
        g_autoptr (GUri) term_uri =
            vte_terminal_ref_termprop_uri (priv->terminal,
                                           VTE_TERMPROP_CURRENT_DIRECTORY_URI);
        g_autofree gchar *term_uri_string =
            term_uri != NULL ? g_uri_to_string (term_uri) : NULL;
        g_autoptr(GFile) term_gfile =
            term_uri_string != NULL ? g_file_new_for_uri (term_uri_string) : NULL;
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
        if (g_uri_get_user (uri) != NULL)
            *username = g_strdup (g_uri_get_user (uri));
        if (g_uri_get_port(uri) > 0)
            *port = g_strdup_printf("%d", g_uri_get_port(uri));
        return (*hostname != NULL);
    }

    return FALSE;
}

gchar *
nautilus_terminal_build_remote_shell_command (const gchar *remote_path,
                                              gboolean     sync_to_file_manager)
{
    g_autoptr (GString) script = g_string_new ("_nvshell=${SHELL:-/bin/sh}; ");
    g_autofree gchar *quoted_script = NULL;

    if (remote_path != NULL && *remote_path != '\0')
    {
        g_autofree gchar *quoted_remote_path = g_shell_quote (remote_path);

        g_string_append_printf (script, "cd %s; ", quoted_remote_path);
    }

    if (!sync_to_file_manager)
    {
        g_string_append (script, "exec \"$_nvshell\" -l");
    }
    else
    {
        g_string_append (
            script,
            "_nvbase=${_nvshell##*/}; case \"$_nvbase\" in "
            "bash) "
            "_nvpc='printf \"\\033]7;file://%s\\007\" \"$PWD\"'; "
            "if [ -n \"${PROMPT_COMMAND:-}\" ]; then "
            "PROMPT_COMMAND=\"$_nvpc;$PROMPT_COMMAND\"; "
            "else PROMPT_COMMAND=\"$_nvpc\"; fi; "
            "export PROMPT_COMMAND; exec \"$_nvshell\" -l ;; "
            "fish) exec \"$_nvshell\" -l --init-command "
            "'function _nautilus_vte_update_cwd --on-event fish_prompt; "
            "printf \"\\033]7;file://%s\\007\" $PWD; end' ;; "
            "zsh) "
            "_nvd=\"${TMPDIR:-/tmp}/nautilus-vte-${USER:-user}-$$\"; "
            "if mkdir -m 700 \"$_nvd\"; then "
            "_NAUTILUS_ORIGINAL_ZDOTDIR=${ZDOTDIR:-$HOME}; "
            "_NAUTILUS_VTE_ZDOTDIR=$_nvd; "
            "export _NAUTILUS_ORIGINAL_ZDOTDIR _NAUTILUS_VTE_ZDOTDIR; "
            "printf '%s\\n' "
            "'if [[ -r \"$_NAUTILUS_ORIGINAL_ZDOTDIR/.zshrc\" ]]; then' "
            "'  source \"$_NAUTILUS_ORIGINAL_ZDOTDIR/.zshrc\"' "
            "'fi' "
            "'autoload -Uz add-zsh-hook' "
            "'_nautilus_vte_update_cwd() { printf \"\\033]7;file://%s\\007\" \"$PWD\"; }' "
            "'_nautilus_vte_cleanup() { command rm -f -- \"$_NAUTILUS_VTE_ZDOTDIR/.zshrc\"; command rmdir -- \"$_NAUTILUS_VTE_ZDOTDIR\"; }' "
            "'add-zsh-hook precmd _nautilus_vte_update_cwd' "
            "'add-zsh-hook zshexit _nautilus_vte_cleanup' "
            "> \"$_nvd/.zshrc\"; "
            "ZDOTDIR=$_nvd; export ZDOTDIR; fi; "
            "exec \"$_nvshell\" -l ;; "
            "*) exec \"$_nvshell\" -l ;; esac");
    }

    quoted_script = g_shell_quote (script->str);
    return g_strdup_printf ("exec sh -c %s", quoted_script);
}

static void
spawn_pending_ssh_connection (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;
    g_autoptr(GPtrArray) argv_array = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *remote_cmd = NULL;
    const gchar *hostname;
    const gchar *username;
    const gchar *port;

    if (priv->disposing || priv->child_pid != -1 ||
        priv->primary_spawn_pending || priv->pending_ssh_hostname == NULL)
    {
        return;
    }

    g_clear_pointer (&priv->ssh_hostname, g_free);
    g_clear_pointer (&priv->ssh_username, g_free);
    g_clear_pointer (&priv->ssh_port, g_free);
    g_clear_pointer (&priv->ssh_remote_path, g_free);
    priv->ssh_hostname = g_steal_pointer (&priv->pending_ssh_hostname);
    priv->ssh_username = g_steal_pointer (&priv->pending_ssh_username);
    priv->ssh_port = g_steal_pointer (&priv->pending_ssh_port);
    priv->ssh_sync_mode = priv->pending_ssh_sync_mode;

    hostname = priv->ssh_hostname;
    username = priv->ssh_username;
    port = priv->ssh_port;

    if (priv->current_location)
        priv->ssh_remote_path = get_remote_path_from_sftp_gfile(priv->current_location);

    remote_cmd = nautilus_terminal_build_remote_shell_command (
        priv->ssh_remote_path,
        priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_BOTH ||
        priv->ssh_sync_mode == NAUTILUS_TERMINAL_SYNC_TERM_TO_FM);

    /* The host and user come percent-decoded from the URI. Without this check
     * a location like sftp://-oProxyCommand=.../ turns into an ssh option and
     * runs a command locally -- the CVE-2017-1000117 pattern. */
    if (!nautilus_terminal_ssh_field_is_safe (hostname) ||
        (username != NULL && !nautilus_terminal_ssh_field_is_safe (username)))
    {
        g_warning ("Refusing to connect: suspicious host or user in the SFTP location");
        g_ptr_array_free (argv_array, TRUE);
        return;
    }

    g_ptr_array_add(argv_array, g_strdup("ssh"));
    g_ptr_array_add(argv_array, g_strdup("-t"));
    if (port && *port)
    {
        g_ptr_array_add(argv_array, g_strdup("-p"));
        g_ptr_array_add(argv_array, g_strdup(port));
    }

    /* Stop option parsing, so a destination can never be read as a flag. */
    g_ptr_array_add(argv_array, g_strdup("--"));

    if (username && *username)
        g_ptr_array_add(argv_array, g_strdup_printf("%s@%s", username, hostname));
    else
        g_ptr_array_add(argv_array, g_strdup(hostname));

    g_ptr_array_add(argv_array, g_strdup(remote_cmd));
    g_ptr_array_add(argv_array, NULL);

    gtk_widget_set_visible(priv->ssh_indicator, TRUE);

    priv->ignore_next_terminal_cd_signal = TRUE;
    priv->state = NAUTILUS_TERMINAL_STATE_SWITCHING_TO_SSH;
    g_clear_object (&priv->spawn_cancellable);
    priv->spawn_cancellable = g_cancellable_new ();
    priv->primary_spawn_pending = TRUE;
    priv->primary_spawn_generation++;

    vte_terminal_spawn_async(priv->terminal,
                            VTE_PTY_DEFAULT,
                            NULL,
                            (gchar **)argv_array->pdata,
                            NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL, NULL,
                            -1,
                            priv->spawn_cancellable,
                            spawn_async_callback,
                            terminal_spawn_data_new (self,
                                                     NAUTILUS_TERMINAL_SPAWN_SSH,
                                                     priv->primary_spawn_generation));
}

static void
_initiate_ssh_connection (NautilusTerminalWidget *self,
                          const gchar            *hostname,
                          const gchar            *username,
                          const gchar            *port,
                          NautilusTerminalSyncMode sync_mode)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (hostname == NULL || *hostname == '\0' || priv->disposing)
    {
        return;
    }

    clear_pending_ssh_connection_data (priv);
    priv->pending_ssh_hostname = g_strdup (hostname);
    priv->pending_ssh_username = g_strdup (username);
    priv->pending_ssh_port = g_strdup (port);
    priv->pending_ssh_sync_mode = sync_mode;

    if (priv->primary_spawn_pending)
    {
        cancel_primary_spawn (self);
    }

    if (priv->child_pid > 0)
    {
        priv->state = NAUTILUS_TERMINAL_STATE_SWITCHING_TO_SSH;
        if (kill (priv->child_pid, SIGHUP) == -1 && errno == ESRCH)
        {
            priv->child_pid = -1;
            spawn_pending_ssh_connection (self);
        }
        return;
    }

    spawn_pending_ssh_connection (self);
}

static void
switch_to_local_terminal (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (priv->disposing ||
        (priv->state == NAUTILUS_TERMINAL_STATE_LOCAL &&
         !priv->primary_spawn_pending))
    {
        return;
    }

    clear_pending_ssh_connection_data (priv);
    if (priv->primary_spawn_pending)
    {
        cancel_primary_spawn (self);
    }

    if (priv->child_pid > 0)
    {
        priv->state = NAUTILUS_TERMINAL_STATE_SWITCHING_TO_LOCAL;
        if (kill (priv->child_pid, SIGHUP) == -1 && errno == ESRCH)
        {
            priv->child_pid = -1;
        }
        else
        {
            return;
        }
    }

    _reset_to_local_state (self);
    if (priv->is_visible)
    {
        spawn_terminal_async (self);
    }
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
    gboolean valid = FALSE;

    if (priv->color_scheme == NULL) {
        priv->color_scheme = g_settings_get_string(priv->settings, "color-scheme");
    }

    for (gsize i = 0; i < G_N_ELEMENTS (COLOR_SCHEME_ENTRIES); i++)
    {
        if (g_strcmp0 (priv->color_scheme, COLOR_SCHEME_ENTRIES[i].id) == 0)
        {
            valid = TRUE;
            break;
        }
    }

    if (!valid)
    {
        g_free (priv->color_scheme);
        priv->color_scheme = g_strdup ("system");
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
        if (priv->secondary_terminal != NULL)
            vte_terminal_set_colors(priv->secondary_terminal, &fg, &bg, ansi_palette, 16);
    } else {
        vte_terminal_set_colors(priv->terminal,
                                &scheme->palette.foreground,
                                &scheme->palette.background,
                                (GdkRGBA *)scheme->palette.palette,
                                G_N_ELEMENTS(scheme->palette.palette));
        if (priv->secondary_terminal != NULL)
            vte_terminal_set_colors(priv->secondary_terminal,
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

    scheme = g_file_get_uri_scheme(location);
    if (scheme == NULL) {
        return;
    }

    if (g_strcmp0 (scheme, "sftp") == 0 &&
        parse_gvfs_ssh_path (location, &hostname, &username, &port))
    {
        if (priv->state == NAUTILUS_TERMINAL_STATE_IN_SSH &&
            nautilus_terminal_ssh_identity_equal (hostname,
                                                  username,
                                                  port,
                                                  priv->ssh_hostname,
                                                  priv->ssh_username,
                                                  priv->ssh_port))
        {
            change_directory_in_terminal (self, location);
            return;
        }

        if (priv->state == NAUTILUS_TERMINAL_STATE_SWITCHING_TO_SSH)
        {
            const gchar *target_hostname = priv->pending_ssh_hostname != NULL
                                           ? priv->pending_ssh_hostname
                                           : priv->ssh_hostname;
            const gchar *target_username = priv->pending_ssh_hostname != NULL
                                           ? priv->pending_ssh_username
                                           : priv->ssh_username;
            const gchar *target_port = priv->pending_ssh_hostname != NULL
                                       ? priv->pending_ssh_port
                                       : priv->ssh_port;

            if (nautilus_terminal_ssh_identity_equal (hostname,
                                                       username,
                                                       port,
                                                       target_hostname,
                                                       target_username,
                                                       target_port))
            {
                return;
            }
        }

        if (priv->ssh_auto_connect_mode != NAUTILUS_TERMINAL_SSH_AUTOCONNECT_OFF)
        {
            NautilusTerminalSyncMode sync_mode;

            switch (priv->ssh_auto_connect_mode)
            {
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_BOTH: sync_mode = NAUTILUS_TERMINAL_SYNC_BOTH; break;
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_FM_TO_TERM: sync_mode = NAUTILUS_TERMINAL_SYNC_FM_TO_TERM; break;
                case NAUTILUS_TERMINAL_SSH_AUTOCONNECT_SYNC_TERM_TO_FM: sync_mode = NAUTILUS_TERMINAL_SYNC_TERM_TO_FM; break;
                default: sync_mode = NAUTILUS_TERMINAL_SYNC_NONE; break;
            }

            _initiate_ssh_connection (self, hostname, username, port, sync_mode);
        }
        else if (priv->state == NAUTILUS_TERMINAL_STATE_LOCAL &&
                 priv->is_visible && priv->needs_respawn)
        {
            spawn_terminal_async (self);
        }

        return;
    }

    if (priv->state != NAUTILUS_TERMINAL_STATE_LOCAL)
    {
        switch_to_local_terminal (self);
        return;
    }

    if (is_first_location_set && priv->is_visible && priv->needs_respawn)
    {
        spawn_terminal_async (self);
    }
    else
    {
        change_directory_in_terminal (self, location);
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
                int pane_size = CLAMP (total_height - position,
                                       MIN_TERMINAL_HEIGHT,
                                       MAX_TERMINAL_HEIGHT);

                g_settings_set_int (priv->settings, "pane-size", pane_size);
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
    VteTerminal *terminal = g_weak_ref_get (&priv->focus_target_weak_ref);

    priv->focus_timeout_id = 0;
    g_weak_ref_set (&priv->focus_target_weak_ref, NULL);

    if (!priv->disposing && priv->is_visible && terminal != NULL &&
        gtk_widget_get_realized (GTK_WIDGET (terminal)) &&
        gtk_widget_get_visible (GTK_WIDGET (terminal)))
    {
        gtk_widget_grab_focus (GTK_WIDGET (terminal));
    }

    g_clear_object (&terminal);
    return G_SOURCE_REMOVE;
}

static void
schedule_terminal_focus (NautilusTerminalWidget *self,
                         VteTerminal           *terminal,
                         guint                  delay_ms)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (priv->disposing || terminal == NULL)
    {
        return;
    }

    g_clear_handle_id (&priv->focus_timeout_id, g_source_remove);
    g_weak_ref_set (&priv->focus_target_weak_ref, terminal);
    priv->focus_timeout_id =
        g_timeout_add_full (G_PRIORITY_DEFAULT,
                            delay_ms,
                            _ensure_focus_timeout,
                            g_object_ref (self),
                            g_object_unref);
}

void
nautilus_terminal_widget_ensure_terminal_focus(NautilusTerminalWidget *self)
{
    g_return_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self));

    schedule_terminal_focus (self, _active_terminal (self), 100);
}

gboolean
nautilus_terminal_widget_get_visible(NautilusTerminalWidget *self)
{
    g_return_val_if_fail(NAUTILUS_IS_TERMINAL_WIDGET(self), FALSE);
    return self->priv->is_visible;
}

/* Number of idle turns to wait for the paned allocation before giving up. */
#define APPLY_SIZE_MAX_RETRIES 30

static gboolean
_apply_size_retry (gpointer user_data)
{
    NautilusTerminalWidget *self = user_data;

    self->priv->apply_size_retry_id = 0;
    if (!self->priv->disposing) {
        nautilus_terminal_widget_apply_new_size (self);
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_apply_size_retry (NautilusTerminalWidget *self)
{
    NautilusTerminalWidgetPrivate *priv = self->priv;

    if (priv->disposing || priv->apply_size_retry_id != 0) {
        return;
    }
    if (priv->apply_size_retries >= APPLY_SIZE_MAX_RETRIES) {
        return;
    }

    priv->apply_size_retries++;
    priv->apply_size_retry_id =
        g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                         _apply_size_retry,
                         g_object_ref (self),
                         g_object_unref);
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

        /* Realized does not imply allocated. With total_height == 0 the maths
         * below collapse to position 0, which hides the file view for good once
         * the real allocation arrives. Retry on idle until the paned has a real
         * height, giving up rather than looping if it never gets one. */
        if (total_height <= 0) {
            schedule_apply_size_retry (self);
            return;
        }

        priv->apply_size_retries = 0;
        term_size = g_settings_get_int(priv->settings, "pane-size");

        term_size = CLAMP (term_size, MIN_TERMINAL_HEIGHT, MAX_TERMINAL_HEIGHT);
        term_size = MIN (term_size, total_height);
        gtk_paned_set_position(GTK_PANED(paned), MAX (0, total_height - term_size));
    }
}
