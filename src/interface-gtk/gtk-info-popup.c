/*
 * Copyright (C) 2012-2024 Robin Haberkorn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <math.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "list.h"
#include "string-utils.h"
#include "gtk-label.h"
#include "gtk-info-popup.h"

/*
 * FIXME: This is redundant with curses-info-popup.c.
 */
typedef struct {
	teco_stailq_entry_t entry;

	teco_popup_entry_type_t type;
	teco_string_t name;
	gboolean highlight;
} teco_popup_entry_t;

struct _TecoGtkInfoPopup {
	GtkEventBox parent_instance;

	GtkAdjustment *hadjustment, *vadjustment;

	GtkWidget *flow_box;
	GStringChunk *chunk;
	teco_stailq_head_t list;
	guint idle_id;
	gboolean frozen;
};

static gboolean teco_gtk_info_popup_scroll_event(GtkWidget *widget, GdkEventScroll *event);
static void teco_gtk_info_popup_show(GtkWidget *widget);
static void teco_gtk_info_popup_vadjustment_changed(GtkAdjustment *vadjustment, GtkWidget *scrollbar);

G_DEFINE_TYPE(TecoGtkInfoPopup, teco_gtk_info_popup, GTK_TYPE_EVENT_BOX)

/** Overrides GObject::finalize() (object destructor) */
static void
teco_gtk_info_popup_finalize(GObject *obj_self)
{
	TecoGtkInfoPopup *self = TECO_GTK_INFO_POPUP(obj_self);

	if (self->chunk)
		g_string_chunk_free(self->chunk);

	teco_stailq_entry_t *entry;
	while ((entry = teco_stailq_remove_head(&self->list)))
		g_free(entry);

	/* chain up to parent class */
	G_OBJECT_CLASS(teco_gtk_info_popup_parent_class)->finalize(obj_self);
}

static void
teco_gtk_info_popup_class_init(TecoGtkInfoPopupClass *klass)
{
	GTK_WIDGET_CLASS(klass)->scroll_event = teco_gtk_info_popup_scroll_event;
	GTK_WIDGET_CLASS(klass)->show = teco_gtk_info_popup_show;
	G_OBJECT_CLASS(klass)->finalize = teco_gtk_info_popup_finalize;
}

static void
teco_gtk_info_popup_init(TecoGtkInfoPopup *self)
{
	self->hadjustment = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
	self->vadjustment = gtk_adjustment_new(0, 0, 0, 0, 0, 0);

	/*
	 * A box containing a viewport and scrollbar will
	 * "emulate" a scrolled window.
	 * We cannot use a scrolled window since it ignores
	 * the preferred height of its viewport which breaks
	 * height-for-width management.
	 */
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	GtkWidget *scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
	                                         self->vadjustment);
	/* show/hide the scrollbar dynamically */
	g_signal_connect(self->vadjustment, "changed",
	                 G_CALLBACK(teco_gtk_info_popup_vadjustment_changed), scrollbar);

	self->flow_box = gtk_flow_box_new();
	/* take as little height as necessary */
	gtk_orientable_set_orientation(GTK_ORIENTABLE(self->flow_box),
	                               GTK_ORIENTATION_HORIZONTAL);
	//gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->flow_box), TRUE);
	/* this for focus handling only, not for scrolling */
	gtk_flow_box_set_hadjustment(GTK_FLOW_BOX(self->flow_box),
	                             self->hadjustment);
	gtk_flow_box_set_vadjustment(GTK_FLOW_BOX(self->flow_box),
	                             self->vadjustment);

	GtkWidget *viewport = gtk_viewport_new(self->hadjustment, self->vadjustment);
	gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
	gtk_container_add(GTK_CONTAINER(viewport), self->flow_box);

	gtk_box_pack_start(GTK_BOX(box), viewport, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), scrollbar, FALSE, FALSE, 0);
	gtk_widget_show_all(box);

	/*
	 * NOTE: Everything shown except the top-level container.
	 * Therefore a gtk_widget_show() is enough to show our popup.
	 */
	gtk_container_add(GTK_CONTAINER(self), box);

	self->chunk = g_string_chunk_new(32);
	self->list = TECO_STAILQ_HEAD_INITIALIZER(&self->list);
	self->idle_id = 0;
	self->frozen = FALSE;
}

gboolean
teco_gtk_info_popup_get_position_in_overlay(GtkOverlay *overlay, GtkWidget *widget,
                                            GdkRectangle *allocation, gpointer user_data)
{
	GtkWidget *main_child = gtk_bin_get_child(GTK_BIN(overlay));
	GtkAllocation main_child_alloc;
	gint natural_height;

	gtk_widget_get_allocation(main_child, &main_child_alloc);
	gtk_widget_get_preferred_height_for_width(widget,
	                                          main_child_alloc.width,
	                                          NULL, &natural_height);

	/*
	 * FIXME: Probably due to some bug in the height-for-width
	 * calculation of Gtk (at least in 3.10 or in the GtkFlowBox
	 * fallback included with SciTECO), the natural height
	 * is a bit too small to accommodate the entire GtkFlowBox,
	 * resulting in the GtkViewport always scrolling.
	 * This hack fixes it up in a NONPORTABLE manner.
	 */
	natural_height += 5;

	allocation->width = main_child_alloc.width;
	allocation->height = MIN(natural_height, main_child_alloc.height);
	allocation->x = 0;
	allocation->y = main_child_alloc.height - allocation->height;

	return TRUE;
}

/** Overrides GtkWidget::scroll_event() */
static gboolean
teco_gtk_info_popup_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	TecoGtkInfoPopup *self = TECO_GTK_INFO_POPUP(widget);
	gdouble delta_x, delta_y;

	if (!gdk_event_get_scroll_deltas((GdkEvent *)event,
	                                 &delta_x, &delta_y))
		return FALSE;

	GtkAdjustment *adj = self->vadjustment;
	gdouble page_size = gtk_adjustment_get_page_size(adj);
	gdouble scroll_unit = pow(page_size, 2.0 / 3.0);
	gdouble new_value;

	new_value = CLAMP(gtk_adjustment_get_value(adj) + delta_y * scroll_unit,
	                  gtk_adjustment_get_lower(adj),
	                  gtk_adjustment_get_upper(adj) -
	                  gtk_adjustment_get_page_size(adj));

	gtk_adjustment_set_value(adj, new_value);

	return TRUE;
}

static void
teco_gtk_info_popup_vadjustment_changed(GtkAdjustment *vadjustment, GtkWidget *scrollbar)
{
	/*
	 * This shows/hides the widget using opacity instead of using
	 * gtk_widget_set_visibility() since the latter would influence
	 * size allocations. A widget with opacity 0 keeps its size.
	 */
	gtk_widget_set_opacity(scrollbar,
	                       gtk_adjustment_get_upper(vadjustment) -
	                       gtk_adjustment_get_lower(vadjustment) >
	                       gtk_adjustment_get_page_size(vadjustment) ? 1 : 0);
}

GtkWidget *
teco_gtk_info_popup_new(void)
{
	return GTK_WIDGET(g_object_new(TECO_TYPE_GTK_INFO_POPUP, NULL));
}

GIcon *
teco_gtk_info_popup_get_icon_for_path(const gchar *path, const gchar *fallback_name)
{
	GIcon *icon = NULL;

	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::icon", 0, NULL, NULL);
	if (info) {
		icon = g_file_info_get_icon(info);
		g_object_ref(icon);
	} else {
		/* fall back to standard icon, but this can still return NULL! */
		icon = g_icon_new_for_string(fallback_name, NULL);
	}

	return icon;
}

void
teco_gtk_info_popup_add(TecoGtkInfoPopup *self, teco_popup_entry_type_t type,
                        const gchar *name, gssize len, gboolean highlight)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (TECO_IS_GTK_INFO_POPUP (self));

	teco_popup_entry_t *entry = g_new(teco_popup_entry_t, 1);
	entry->type = type;
	/*
	 * Popup entries aren't removed individually, so we can
	 * more efficiently store them via GStringChunk.
	 */
	teco_string_init_chunk(&entry->name, name, len < 0 ? strlen(name) : len,
	                       self->chunk);
	entry->highlight = highlight;

	/*
	 * NOTE: We don't immediately create the Gtk+ widget and add it
	 * to the GtkFlowBox since it would be too slow for very large
	 * numbers of popup entries.
	 * Instead, we queue and process them in idle time only once the widget
	 * is shown. This ensures a good reactivity, even though the popup may
	 * not yet be complete when first shown.
	 *
	 * While it would be possible to show the widget before the first
	 * add() call to achieve the same effect, this would prevent keyboard
	 * interaction unless we add support for interruptions or drive
	 * the event loop manually.
	 */
	teco_stailq_insert_tail(&self->list, &entry->entry);
}

static void
teco_gtk_info_popup_idle_add(TecoGtkInfoPopup *self, teco_popup_entry_type_t type,
                             const gchar *name, gssize len, gboolean highlight)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(TECO_IS_GTK_INFO_POPUP(self));

	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	if (highlight)
		gtk_style_context_add_class(gtk_widget_get_style_context(hbox),
		                            "highlight");

	/*
	 * FIXME: The icon fetching takes about 1/3 of the time required to
	 * add all widgets.
	 * Perhaps it's possible to optimize this.
	 */
	if (type == TECO_POPUP_FILE || type == TECO_POPUP_DIRECTORY) {
		const gchar *fallback = type == TECO_POPUP_FILE ? "text-x-generic"
		                                                : "folder";

		/*
		 * `name` is not guaranteed to be null-terminated.
		 */
		g_autofree gchar *path = len < 0 ? g_strdup(name) : g_strndup(name, len);

		g_autoptr(GIcon) icon = teco_gtk_info_popup_get_icon_for_path(path, fallback);
		if (icon) {
			gint width, height;
			gtk_icon_size_lookup(GTK_ICON_SIZE_MENU, &width, &height);

			GtkWidget *image = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_MENU);
			/* This is necessary so that oversized icons get scaled down. */
			gtk_image_set_pixel_size(GTK_IMAGE(image), height);
			gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
		}
	}

	GtkWidget *label = teco_gtk_label_new(name, len);
	/*
	 * Gtk v3.20 changed the CSS element names.
	 * Adding a style class eases writing a portable fallback.css.
	 */
	gtk_style_context_add_class(gtk_widget_get_style_context(label), "label");
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_widget_set_valign(label, GTK_ALIGN_CENTER);

	/*
	 * FIXME: This makes little sense once we've got mouse support.
	 * But for the time being, it's a useful setting.
	 */
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);

	switch (type) {
	case TECO_POPUP_PLAIN:
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
		break;
	case TECO_POPUP_FILE:
	case TECO_POPUP_DIRECTORY:
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
		break;
	}

	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

	gtk_widget_show_all(hbox);
	gtk_container_add(GTK_CONTAINER(self->flow_box), hbox);
}

static gboolean
teco_gtk_info_popup_idle_cb(TecoGtkInfoPopup *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(TECO_IS_GTK_INFO_POPUP(self), FALSE);

	/*
	 * The more often this is repeated, the faster we will add all popup entries,
	 * but at the same time, the UI will be less responsive.
	 */
	for (gint i = 0; i < 5; i++) {
		teco_popup_entry_t *head = (teco_popup_entry_t *)teco_stailq_remove_head(&self->list);
		if (G_UNLIKELY(!head)) {
			if (self->frozen)
				gdk_window_thaw_updates(gtk_widget_get_window(GTK_WIDGET(self)));
			self->frozen = FALSE;
			self->idle_id = 0;
			return G_SOURCE_REMOVE;
		}

		teco_gtk_info_popup_idle_add(self, head->type, head->name.data, head->name.len, head->highlight);

		/* All teco_popup_entry_t::names are freed via GStringChunk */
		g_free(head);
	}

	if (self->frozen &&
	    gtk_adjustment_get_upper(self->vadjustment) -
	    gtk_adjustment_get_lower(self->vadjustment) > gtk_adjustment_get_page_size(self->vadjustment)) {
		/* the GtkFlowBox needs scrolling - time to thaw */
		gdk_window_thaw_updates(gtk_widget_get_window(GTK_WIDGET(self)));
		self->frozen = FALSE;
	}

	return G_SOURCE_CONTINUE;
}

/** Overrides GtkWidget::show() */
static void
teco_gtk_info_popup_show(GtkWidget *widget)
{
	TecoGtkInfoPopup *self = TECO_GTK_INFO_POPUP(widget);

	if (!self->idle_id) {
		self->idle_id = gdk_threads_add_idle((GSourceFunc)teco_gtk_info_popup_idle_cb, self);

		/*
		 * To prevent a visible popup build-up for small popups,
		 * the display is frozen until the popup is large enough for
		 * scrolling or until all entries have been added.
		 */
		GdkWindow *window = gtk_widget_get_window(widget);
		if (window) {
			gdk_window_freeze_updates(window);
			self->frozen = TRUE;
		}
	}

	/* chain to parent class */
	GTK_WIDGET_CLASS(teco_gtk_info_popup_parent_class)->show(widget);
}

void
teco_gtk_info_popup_scroll_page(TecoGtkInfoPopup *self)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(TECO_IS_GTK_INFO_POPUP(self));

	GtkAdjustment *adj = self->vadjustment;
	gdouble new_value;

	if (gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) ==
	    gtk_adjustment_get_upper(adj)) {
		/* wrap and scroll back to the top */
		new_value = gtk_adjustment_get_lower(adj);
	} else {
		/* scroll one page */
		new_value = gtk_adjustment_get_value(adj) +
		            gtk_adjustment_get_page_size(adj);

		/*
		 * Adjust this so only complete entries are shown.
		 * Effectively, this rounds down to the line height.
		 */
		GList *child_list = gtk_container_get_children(GTK_CONTAINER(self->flow_box));
		if (child_list) {
			new_value -= (gint)new_value %
			             gtk_widget_get_allocated_height(GTK_WIDGET(child_list->data));
			g_list_free(child_list);
		}

		/* clip to the maximum possible value */
		new_value = MIN(new_value, gtk_adjustment_get_upper(adj));
	}

	gtk_adjustment_set_value(adj, new_value);
}

static void
teco_gtk_info_popup_destroy_cb(GtkWidget *widget, gpointer user_data)
{
	gtk_widget_destroy(widget);
}

void
teco_gtk_info_popup_clear(TecoGtkInfoPopup *self)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(TECO_IS_GTK_INFO_POPUP(self));

	gtk_container_foreach(GTK_CONTAINER(self->flow_box), teco_gtk_info_popup_destroy_cb, NULL);

	/*
	 * If there are still queued popoup entries, the next teco_gtk_info_popup_idle_cb()
	 * invocation will also stop the GSource.
	 */
	teco_stailq_entry_t *entry;
	while ((entry = teco_stailq_remove_head(&self->list)))
		g_free(entry);

	g_string_chunk_clear(self->chunk);
}
