/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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

#include <glib.h>

#include <gtk/gtk.h>

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "view.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ScintillaObject, g_object_unref)

#define TECO_TYPE_VIEW teco_view_get_type()
G_DECLARE_FINAL_TYPE(TecoView, teco_view, TECO, VIEW, ScintillaObject)

struct _TecoView {
	ScintillaObject parent_instance;
	/** current size allocation */
	GdkRectangle allocation;
};

G_DEFINE_TYPE(TecoView, teco_view, SCINTILLA_TYPE_OBJECT)

static void
teco_view_scintilla_notify_cb(ScintillaObject *sci, gint iMessage, SCNotification *notify)
{
	teco_view_process_notify((teco_view_t *)TECO_VIEW(sci), notify);
}

/**
 * Called when the view is size allocated.
 *
 * This especially ensures that the caret is visible after startup and when
 * opening files on specific lines.
 * It's important to scroll the caret only when the size actually changes,
 * so we do not interfere with mouse scrolling.
 * That callback is invoked even if the size does not change, so that's why
 * we have to store the current allocation in teco_view_t.
 * Calling it once is unfortunately not sufficient since the window size
 * can change during startup.
 */
static void
teco_view_size_allocate_cb(GtkWidget *widget, GdkRectangle *allocation)
{
	/* chain to parent class */
	GTK_WIDGET_CLASS(teco_view_parent_class)->size_allocate(widget, allocation);

	TecoView *view = TECO_VIEW(widget);

	if (allocation->width == view->allocation.width && allocation->height == view->allocation.height)
		return;
	teco_view_ssm((teco_view_t *)view, SCI_SCROLLCARET, 0, 0);
	memcpy(&view->allocation, allocation, sizeof(view->allocation));
}

teco_view_t *
teco_view_new(void)
{
	TecoView *ctx = TECO_VIEW(g_object_new(TECO_TYPE_VIEW, NULL));
	/*
	 * We don't want the object to be destroyed
	 * when it is removed from the vbox.
	 */
	g_object_ref_sink(ctx);

	scintilla_set_id(SCINTILLA(ctx), 0);

	gtk_widget_set_size_request(GTK_WIDGET(ctx), 500, 300);

	/*
	 * This disables mouse and key events on this view.
	 *
	 * FIXME: For some strange reason, masking events on
	 * the event box does NOT work.
	 * This might have been a bug in GdkWindow stacking
	 * when swapping out the GtkEventBox's child.
	 * Still, better be on the safe side.
	 */
	gtk_widget_set_can_focus(GTK_WIDGET(ctx), FALSE);
	gint events = gtk_widget_get_events(GTK_WIDGET(ctx));
	events &= ~(GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
	            GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_TOUCH_MASK |
	            GDK_TOUCHPAD_GESTURE_MASK |
	            GDK_TABLET_PAD_MASK |
	            GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
	gtk_widget_set_events(GTK_WIDGET(ctx), events);

	/*
	 * Disables drag and drop interaction.
	 */
	gtk_drag_dest_unset(GTK_WIDGET(ctx));

	return (teco_view_t *)ctx;
}

static void
teco_view_class_init(TecoViewClass *klass)
{
	SCINTILLA_CLASS(klass)->notify = teco_view_scintilla_notify_cb;
	GTK_WIDGET_CLASS(klass)->size_allocate = teco_view_size_allocate_cb;
}

static void teco_view_init(TecoView *self) {}

sptr_t
teco_view_ssm(teco_view_t *ctx, unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	return scintilla_send_message(SCINTILLA(ctx), iMessage, wParam, lParam);
}

void
teco_view_free(teco_view_t *ctx)
{
	g_object_unref(ctx);
}
