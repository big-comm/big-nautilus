/*
 * Copyright (C) 2022 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-grid-view.h"

#include "nautilus-file.h"
#include "nautilus-global-preferences.h"
#include "nautilus-grid-cell.h"
#include "nautilus-list-base-private.h"
#include "nautilus-view-cell.h"
#include "nautilus-view-item.h"
#include "nautilus-view-model.h"

#include <glib/gi18n.h>

struct _NautilusGridView
{
    NautilusListBase parent_instance;

    GtkGridView *view_ui;

    gint zoom_level;

    gboolean directories_first;

    GQuark caption_attributes[NAUTILUS_GRID_CELL_N_CAPTIONS];

    GQuark sort_attribute;
    gboolean reversed;

    NautilusCellPool *cell_pool;
    guint cell_pool_fill_id;

    guint update_max_columns_id;
};

/* As many columns as upstream allows a very wide window to have. */
#define MAX_COLUMNS 20

/* GTK builds cells for some 640 items of a grid, whatever fits the window. */
#define CELL_POOL_SIZE 660
/* How many are built before they are asked for. The rest of the pool fills
 * as cells are needed. */
#define CELL_POOL_PREFILL 240
/* Built ahead of need a few at a time, in between everything else. */
#define CELL_POOL_FILL_STEP 6

G_DEFINE_TYPE (NautilusGridView, nautilus_grid_view, NAUTILUS_TYPE_LIST_BASE)

#define get_view_item(li) \
        (NAUTILUS_VIEW_ITEM (gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (gtk_list_item_get_item (li)))))

static const NautilusViewInfo grid_view_info =
{
    .view_id = NAUTILUS_VIEW_GRID_ID,
    .zoom_level_min = NAUTILUS_GRID_ZOOM_LEVEL_SMALL,
    .zoom_level_max = NAUTILUS_GRID_ZOOM_LEVEL_EXTRA_LARGE,
    .zoom_level_standard = NAUTILUS_GRID_ZOOM_LEVEL_MEDIUM,
};

static NautilusViewInfo
real_get_view_info (NautilusListBase *list_base)
{
    return grid_view_info;
}

static gint
nautilus_grid_view_sort (gconstpointer a,
                         gconstpointer b,
                         gpointer      user_data)
{
    NautilusGridView *self = user_data;
    NautilusFile *file_a;
    NautilusFile *file_b;

    file_a = nautilus_view_item_get_file ((NautilusViewItem *) a);
    file_b = nautilus_view_item_get_file ((NautilusViewItem *) b);

    return nautilus_file_compare_for_sort_by_attribute_q (file_a, file_b,
                                                          self->sort_attribute,
                                                          self->directories_first,
                                                          self->reversed);
}

static void
update_sort_directories_first (NautilusGridView *self)
{
    NautilusFile *directory_as_file = nautilus_list_base_get_directory_as_file (NAUTILUS_LIST_BASE (self));
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    /* Always treat directories as normal items in search and recent. Recent
     * can accidentally contain directories when they were picked via file chooser. */
    if (nautilus_file_is_in_search (directory_as_file) ||
        nautilus_file_is_in_recent (directory_as_file))
    {
        self->directories_first = FALSE;
    }
    else
    {
        self->directories_first = g_settings_get_boolean (gtk_filechooser_preferences,
                                                          NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);
    }

    if (model != NULL)
    {
        nautilus_view_model_sort (model);
    }
}

static void
nautilus_grid_view_setup_directory (NautilusListBase  *list_base,
                                    NautilusDirectory *new_directory)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);

    NAUTILUS_LIST_BASE_CLASS (nautilus_grid_view_parent_class)->setup_directory (list_base, new_directory);

    update_sort_directories_first (self);
}

static guint
get_icon_size_for_zoom_level (NautilusGridZoomLevel zoom_level)
{
    switch (zoom_level)
    {
        case NAUTILUS_GRID_ZOOM_LEVEL_SMALL:
        {
            return NAUTILUS_GRID_ICON_SIZE_SMALL;
        }
        break;

        case NAUTILUS_GRID_ZOOM_LEVEL_SMALL_PLUS:
        {
            return NAUTILUS_GRID_ICON_SIZE_SMALL_PLUS;
        }
        break;

        case NAUTILUS_GRID_ZOOM_LEVEL_MEDIUM:
        {
            return NAUTILUS_GRID_ICON_SIZE_MEDIUM;
        }
        break;

        case NAUTILUS_GRID_ZOOM_LEVEL_LARGE:
        {
            return NAUTILUS_GRID_ICON_SIZE_LARGE;
        }
        break;

        case NAUTILUS_GRID_ZOOM_LEVEL_EXTRA_LARGE:
        {
            return NAUTILUS_GRID_ICON_SIZE_EXTRA_LARGE;
        }
        break;
    }
    g_return_val_if_reached (NAUTILUS_GRID_ICON_SIZE_MEDIUM);
}

static gint
get_default_zoom_level (void)
{
    int default_zoom_level = g_settings_get_enum (nautilus_icon_view_preferences,
                                                  NAUTILUS_PREFERENCES_ICON_VIEW_DEFAULT_ZOOM_LEVEL);

    /* Sanitize preference value */
    return CLAMP (default_zoom_level,
                  grid_view_info.zoom_level_min,
                  grid_view_info.zoom_level_max);
}

static void
set_captions_from_preferences (NautilusGridView *self)
{
    g_auto (GStrv) value = NULL;
    gint n_captions_for_zoom_level;

    value = g_settings_get_strv (nautilus_icon_view_preferences,
                                 NAUTILUS_PREFERENCES_ICON_VIEW_CAPTIONS);

    /* Set a celling on the number of captions depending on the zoom level. */
    n_captions_for_zoom_level = MIN ((uint) self->zoom_level + 1,
                                     G_N_ELEMENTS (self->caption_attributes));

    /* Reset array to zeros beforehand, as we may not refill all elements. */
    memset (&self->caption_attributes, 0, sizeof (self->caption_attributes));
    for (gint i = 0, quark_i = 0;
         value[i] != NULL && quark_i < n_captions_for_zoom_level;
         i++)
    {
        if (g_strcmp0 (value[i], "none") == 0)
        {
            continue;
        }

        /* Convert to quarks in advance, otherwise each NautilusFile attribute
         * getter would call g_quark_from_string() once for each file. */
        self->caption_attributes[quark_i] = g_quark_from_string (value[i]);
        quark_i++;
    }
}

static void
real_set_zoom_level (NautilusListBase *list_base,
                     int               new_level)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);

    g_return_if_fail (new_level >= grid_view_info.zoom_level_min &&
                      new_level <= grid_view_info.zoom_level_max);

    self->zoom_level = new_level;

    if (g_settings_get_enum (nautilus_icon_view_preferences,
                             NAUTILUS_PREFERENCES_ICON_VIEW_DEFAULT_ZOOM_LEVEL) != new_level)
    {
        g_settings_set_enum (nautilus_icon_view_preferences,
                             NAUTILUS_PREFERENCES_ICON_VIEW_DEFAULT_ZOOM_LEVEL,
                             new_level);
    }

    /* The zoom level may change how many captions are allowed. Update it before
     * notifying the icon size change, under the assumption that NautilusGridCell
     * updates captions whenever the icon size is set*/
    set_captions_from_preferences (self);

    g_object_notify (G_OBJECT (self), "icon-size");
}

/* The generic implementation in src/nautilus-list-base.c doesn't allow the
 * 2-dimensional movements expected from a grid. Let's hack GTK here. */
static void
real_preview_selection_event (NautilusListBase *list_base,
                              GtkDirectionType  direction)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);
    guint direction_keyval;
    g_autoptr (GtkShortcutTrigger) direction_trigger = NULL;
    g_autoptr (GListModel) controllers = NULL;
    gboolean success = FALSE;

    /* We want the same behavior as when the user presses the arrow keys while
     * the focus is in the view. So, let's get the matching arrow key. */
    switch (direction)
    {
        case GTK_DIR_UP:
        {
            direction_keyval = GDK_KEY_Up;
        }
        break;

        case GTK_DIR_DOWN:
        {
            direction_keyval = GDK_KEY_Down;
        }
        break;

        case GTK_DIR_LEFT:
        {
            direction_keyval = GDK_KEY_Left;
        }
        break;

        case GTK_DIR_RIGHT:
        {
            direction_keyval = GDK_KEY_Right;
        }
        break;

        default:
        {
            g_return_if_reached ();
        }
    }

    /* We cannot simulate a click, but we can find the shortcut it triggers and
     * activate its action programatically.
     *
     * First, we create out would-be trigger.*/
    direction_trigger = gtk_keyval_trigger_new (direction_keyval, 0);

    /* Then we iterate over the shortcut installed in GtkGridView until we find
     * a matching trigger. There may be multiple shortcut controllers, and each
     * shortcut controller may hold multiple shortcuts each. Let's loop. */
    controllers = gtk_widget_observe_controllers (GTK_WIDGET (self->view_ui));
    for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
        g_autoptr (GtkEventController) controller = g_list_model_get_item (controllers, i);

        if (!GTK_IS_SHORTCUT_CONTROLLER (controller))
        {
            continue;
        }

        for (guint j = 0; j < g_list_model_get_n_items (G_LIST_MODEL (controller)); j++)
        {
            g_autoptr (GtkShortcut) shortcut = g_list_model_get_item (G_LIST_MODEL (controller), j);
            GtkShortcutTrigger *trigger = gtk_shortcut_get_trigger (shortcut);

            if (gtk_shortcut_trigger_equal (trigger, direction_trigger))
            {
                /* Match found. Activate the action to move cursor. */
                success = gtk_shortcut_action_activate (gtk_shortcut_get_action (shortcut),
                                                        0,
                                                        GTK_WIDGET (self->view_ui),
                                                        gtk_shortcut_get_arguments (shortcut));
                break;
            }
        }
    }

    /* If the hack fails (GTK may change it's internal behavior), fallback. */
    if (!success)
    {
        g_warning_once ("GTK shortcut behavior has changed, manual shortcut method failed");
        NAUTILUS_LIST_BASE_CLASS (nautilus_grid_view_parent_class)->preview_selection_event (list_base, direction);
    }
}

/* We only care about the keyboard activation part that GtkGridView provides,
 * but we don't need any special filtering here. Indeed, we ask GtkGridView
 * to not activate on single click, and we get to handle double clicks before
 * GtkGridView does (as one of widget subclassing's goal is to modify the parent
 * class's behavior), while claiming the click gestures, so it means GtkGridView
 * will never react to a click event to emit this signal. So we should be pretty
 * safe here with regards to our custom item click handling.
 */
static void
on_grid_view_item_activated (GtkGridView *grid_view,
                             guint        position,
                             gpointer     user_data)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (user_data);

    nautilus_list_base_activate_selection (NAUTILUS_LIST_BASE (self), FALSE);
}

static guint
real_get_icon_size (NautilusListBase *list_base_view)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base_view);

    return get_icon_size_for_zoom_level (self->zoom_level);
}

static int
real_get_zoom_level (NautilusListBase *list_base_view)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base_view);

    return self->zoom_level;
}

static void
real_scroll_to (NautilusListBase   *list_base_view,
                guint               position,
                GtkListScrollFlags  flags,
                GtkScrollInfo      *scroll)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base_view);

    gtk_grid_view_scroll_to (self->view_ui, position, flags, scroll);
}

static GVariant *
real_get_sort_state (NautilusListBase *list_base)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);

    return g_variant_take_ref (g_variant_new ("(sb)",
                                              g_quark_to_string (self->sort_attribute),
                                              self->reversed));
}

static void
real_set_enable_rubberband (NautilusListBase *list_base,
                            gboolean          enabled)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);

    gtk_grid_view_set_enable_rubberband (self->view_ui, enabled);
}

static void
real_set_sort_state (NautilusListBase *list_base,
                     GVariant         *value)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (list_base);
    const gchar *target_name;
    NautilusViewModel *model = nautilus_list_base_get_model (list_base);
    g_autoptr (GtkCustomSorter) sorter = NULL;

    /* Sort state binding should be set after the model is set.*/
    g_return_if_fail (model != NULL);

    g_variant_get (value, "(&sb)", &target_name, &self->reversed);
    self->sort_attribute = g_quark_from_string (target_name);

    sorter = gtk_custom_sorter_new (nautilus_grid_view_sort, self, NULL);
    nautilus_view_model_set_sorter (model, GTK_SORTER (sorter));
}

static void
on_captions_preferences_changed (NautilusGridView *self)
{
    set_captions_from_preferences (self);

    /* Hack: this relies on the assumption that NautilusGridCell updates
     * captions whenever the icon size is set (even if it's the same value). */
    g_object_notify (G_OBJECT (self), "icon-size");
}

static void
dispose (GObject *object)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (object);

    g_clear_handle_id (&self->cell_pool_fill_id, g_source_remove);
    g_clear_handle_id (&self->update_max_columns_id, g_source_remove);
    g_clear_pointer (&self->cell_pool, nautilus_cell_pool_free);

    G_OBJECT_CLASS (nautilus_grid_view_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    G_OBJECT_CLASS (nautilus_grid_view_parent_class)->finalize (object);
}

/* Everything about a cell that is its own, and not the list item's. */
static NautilusGridCell *
create_cell (NautilusGridView *self)
{
    NautilusGridCell *cell = nautilus_grid_cell_new (NAUTILUS_LIST_BASE (self));

    setup_cell_controllers (NAUTILUS_VIEW_CELL (cell));
    setup_cell_hover (NAUTILUS_VIEW_CELL (cell));

    g_object_bind_property (self, "icon-size",
                            cell, "icon-size",
                            G_BINDING_SYNC_CREATE);

    nautilus_grid_cell_set_caption_attributes (cell, self->caption_attributes);

    return cell;
}

static void
bind_cell (GtkSignalListItemFactory *factory,
           GtkListItem              *listitem,
           gpointer                  user_data)
{
    GtkWidget *cell;
    g_autoptr (NautilusViewItem) item = NULL;

    cell = gtk_list_item_get_child (listitem);
    item = get_view_item (listitem);
    g_return_if_fail (item != NULL);

    nautilus_view_item_set_item_ui (item, cell);

    {
        GtkWidget *parent;

        /* At the time of ::setup emission, the item ui has got no parent yet,
         * that's why we need to complete the widget setup process here, when
         * ::bind is emitted. Every time and not just the first: a cell
         * outlives its parent, see setup_cell(). The setters do nothing
         * when there is nothing to change. */
        parent = gtk_widget_get_parent (cell);
        gtk_widget_set_halign (parent, GTK_ALIGN_CENTER);
        gtk_widget_set_valign (parent, GTK_ALIGN_START);
    }
}

static void
unbind_cell (GtkSignalListItemFactory *factory,
             GtkListItem              *listitem,
             gpointer                  user_data)
{
    g_autoptr (NautilusViewItem) item = NULL;

    item = get_view_item (listitem);

    /* item may be NULL when row has just been destroyed. */
    if (item != NULL)
    {
        nautilus_view_item_set_item_ui (item, NULL);
    }
}

static void
setup_cell (GtkSignalListItemFactory *factory,
            GtkListItem              *listitem,
            gpointer                  user_data)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (user_data);
    NautilusGridCell *cell;
    GtkExpression *expression;

    /* One the view has and GTK let go of, if there is one: see
     * NautilusCellPool. Building it is some 0.6 ms, for the template alone. */
    cell = NAUTILUS_GRID_CELL (nautilus_cell_pool_take (self->cell_pool));
    if (cell == NULL)
    {
        cell = create_cell (self);
        gtk_list_item_set_child (listitem, GTK_WIDGET (cell));
        /* In use already, which the pool tells by its having a parent. */
        nautilus_cell_pool_add (self->cell_pool, NAUTILUS_VIEW_CELL (cell));
    }
    else
    {
        gtk_list_item_set_child (listitem, GTK_WIDGET (cell));
    }

    setup_cell_links (G_OBJECT (listitem), NAUTILUS_VIEW_CELL (cell));

    /* Use file display name as accessible label. Explaining in pseudo-code:
     * listitem:accessible-name :- listitem:item:item:file:a11y-name */
    expression = gtk_property_expression_new (GTK_TYPE_LIST_ITEM, NULL, "item");
    expression = gtk_property_expression_new (GTK_TYPE_TREE_LIST_ROW, expression, "item");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_VIEW_ITEM, expression, "file");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_FILE, expression, "a11y-name");
    gtk_expression_bind (expression, listitem, "accessible-label", listitem);
}

static void
teardown_cell (GtkSignalListItemFactory *factory,
               GtkListItem              *listitem,
               gpointer                  user_data)
{
    /* The cell has left the list item by now, and is back among the free
     * ones if it is one the view keeps. What is left of the two is what
     * tied them together. */
    teardown_cell_links (G_OBJECT (listitem));
}

static gboolean
fill_cell_pool (gpointer user_data)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (user_data);

    for (guint i = 0;
         i < CELL_POOL_FILL_STEP && nautilus_cell_pool_get_n_cells (self->cell_pool) < CELL_POOL_PREFILL;
         i++)
    {
        nautilus_cell_pool_add (self->cell_pool, NAUTILUS_VIEW_CELL (create_cell (self)));
    }

    if (nautilus_cell_pool_get_n_cells (self->cell_pool) < CELL_POOL_PREFILL)
    {
        return G_SOURCE_CONTINUE;
    }

    self->cell_pool_fill_id = 0;

    return G_SOURCE_REMOVE;
}

/* GTK builds cells for 32 rows of as many columns as the grid may have at
 * most, not as many as it has: 641 of them with the limit at 20, for a window
 * that shows six columns and thirty items. Each one is built when a list is
 * shown and thrown away when it is replaced, which is what a search does to
 * its results on every letter typed.
 *
 * So the limit follows the window: as many columns as fit, and one to spare,
 * which is never reached and so changes nothing about the layout. */
static gboolean
update_max_columns (gpointer user_data)
{
    NautilusGridView *self = NAUTILUS_GRID_VIEW (user_data);
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (scrolled_window));
    GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->view_ui));
    double width = gtk_adjustment_get_page_size (adjustment);
    int column_width = 0;
    guint columns;

    self->update_max_columns_id = 0;

    if (child == NULL || width <= 0)
    {
        /* Nothing to go by. */
        return G_SOURCE_REMOVE;
    }

    /* What GTK goes by when it decides how many fit: the least a column can
     * be, which is the same for every cell at a given icon size. */
    gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &column_width, NULL, NULL, NULL);
    if (column_width <= 0)
    {
        return G_SOURCE_REMOVE;
    }

    columns = CLAMP ((guint) (width / column_width) + 1, 2, MAX_COLUMNS);
    if (columns != gtk_grid_view_get_max_columns (self->view_ui))
    {
        gtk_grid_view_set_max_columns (self->view_ui, columns);
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_update_max_columns (NautilusGridView *self)
{
    if (self->update_max_columns_id == 0)
    {
        /* After the layout this was called from the middle of. */
        self->update_max_columns_id = g_idle_add (update_max_columns, self);
    }
}

static void
on_model_items_changed (NautilusGridView *self,
                        guint             position,
                        guint             removed,
                        guint             added,
                        GListModel       *model)
{
    /* The first items of a view that had none: something to measure. */
    if (added > 0 && g_list_model_get_n_items (model) == added)
    {
        schedule_update_max_columns (self);
    }
}

static void
on_model_changed (NautilusGridView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    if (model != NULL)
    {
        gtk_grid_view_set_enable_rubberband (GTK_GRID_VIEW (self->view_ui),
                                             !nautilus_view_model_get_single_selection (model));
    }

    gtk_grid_view_set_model (self->view_ui, GTK_SELECTION_MODEL (model));

    if (model != NULL)
    {
        g_signal_connect_object (model, "items-changed",
                                 G_CALLBACK (on_model_items_changed), self,
                                 G_CONNECT_SWAPPED);
    }
}
static GtkGridView *
create_view_ui (NautilusGridView *self)
{
    GtkListItemFactory *factory;
    GtkWidget *widget;

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup_cell), self);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_cell), self);
    g_signal_connect (factory, "unbind", G_CALLBACK (unbind_cell), self);
    g_signal_connect (factory, "teardown", G_CALLBACK (teardown_cell), self);

    widget = gtk_grid_view_new (NULL, factory);

    /* We don't use the built-in child activation feature for clicks because it
     * doesn't fill all our needs nor does it match our expected behavior.
     * Instead, we roll our own event handling and double/single click mode.
     * However, GtkGridView:single-click-activate has other effects besides
     * activation, as it affects the selection behavior as well (e.g. selects on
     * hover). Setting it to FALSE gives us the expected behavior. */
    gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (widget), FALSE);
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (widget), MAX_COLUMNS);
    gtk_grid_view_set_tab_behavior (GTK_GRID_VIEW (widget), GTK_LIST_TAB_ITEM);

    gtk_accessible_update_property (GTK_ACCESSIBLE (widget),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Content View"),
                                    GTK_ACCESSIBLE_PROPERTY_ROLE_DESCRIPTION,
                                    _("View of the current location"),
                                    -1);

    /* While we don't want to use GTK's click activation, we'll let it handle
     * the key activation part (with Enter).
     */
    g_signal_connect (widget, "activate", G_CALLBACK (on_grid_view_item_activated), self);

    return GTK_GRID_VIEW (widget);
}

static void
nautilus_grid_view_class_init (NautilusGridViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusListBaseClass *list_base_view_class = NAUTILUS_LIST_BASE_CLASS (klass);

    object_class->dispose = dispose;
    object_class->finalize = finalize;

    list_base_view_class->get_icon_size = real_get_icon_size;
    list_base_view_class->get_sort_state = real_get_sort_state;
    list_base_view_class->get_view_info = real_get_view_info;
    list_base_view_class->get_zoom_level = real_get_zoom_level;
    list_base_view_class->preview_selection_event = real_preview_selection_event;
    list_base_view_class->scroll_to = real_scroll_to;
    list_base_view_class->set_enable_rubberband = real_set_enable_rubberband;
    list_base_view_class->set_sort_state = real_set_sort_state;
    list_base_view_class->set_zoom_level = real_set_zoom_level;
    list_base_view_class->setup_directory = nautilus_grid_view_setup_directory;
}

static void
nautilus_grid_view_init (NautilusGridView *self)
{
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-grid-view");

    set_captions_from_preferences (self);
    g_signal_connect_object (nautilus_icon_view_preferences,
                             "changed::" NAUTILUS_PREFERENCES_ICON_VIEW_CAPTIONS,
                             G_CALLBACK (on_captions_preferences_changed),
                             self,
                             G_CONNECT_SWAPPED);


    self->cell_pool = nautilus_cell_pool_new (CELL_POOL_SIZE);
    self->view_ui = create_view_ui (self);
    nautilus_list_base_setup_gestures (NAUTILUS_LIST_BASE (self));

    g_signal_connect_object (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (scrolled_window)),
                             "notify::page-size",
                             G_CALLBACK (schedule_update_max_columns), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect (self, "notify::icon-size", G_CALLBACK (schedule_update_max_columns), NULL);

    /* When there is nothing else to do, so that the first long list finds
     * its cells built. */
    self->cell_pool_fill_id = g_idle_add_full (G_PRIORITY_LOW, fill_cell_pool, self, NULL);

    g_signal_connect_swapped (self, "notify::model", G_CALLBACK (on_model_changed), self);

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window),
                                   GTK_WIDGET (self->view_ui));

    g_signal_connect_object (gtk_filechooser_preferences,
                             "changed::" NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST,
                             G_CALLBACK (update_sort_directories_first),
                             self,
                             G_CONNECT_SWAPPED);

    nautilus_list_base_set_zoom_level (NAUTILUS_LIST_BASE (self), get_default_zoom_level ());
}

NautilusGridView *
nautilus_grid_view_new (void)
{
    return g_object_new (NAUTILUS_TYPE_GRID_VIEW, NULL);
}
