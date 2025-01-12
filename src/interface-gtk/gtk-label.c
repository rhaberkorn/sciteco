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
#include <glib/gprintf.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "sciteco.h"
#include "string-utils.h"

#include "gtk-label.h"

#define GDK_TO_PANGO_COLOR(X) ((guint16)((X) * G_MAXUINT16))

struct _TecoGtkLabel {
	GtkLabel parent_instance;

	PangoColor fg, bg;
	guint16 fg_alpha, bg_alpha;

	teco_string_t string;
};

G_DEFINE_TYPE(TecoGtkLabel, teco_gtk_label, GTK_TYPE_LABEL)

/** Overrides GObject::finalize() (object destructor) */
static void
teco_gtk_label_finalize(GObject *obj_self)
{
	TecoGtkLabel *self = TECO_GTK_LABEL(obj_self);

	teco_string_clear(&self->string);

	/* chain up to parent class */
	G_OBJECT_CLASS(teco_gtk_label_parent_class)->finalize(obj_self);
}

/** Overrides GtkWidget::style_updated() */
static void
teco_gtk_label_style_updated(GtkWidget *widget)
{
	TecoGtkLabel *self = TECO_GTK_LABEL(widget);

	/* chain to parent class */
	GTK_WIDGET_CLASS(teco_gtk_label_parent_class)->style_updated(widget);

	GtkStyleContext *style = gtk_widget_get_style_context(widget);

	GdkRGBA normal_color;
	gtk_style_context_get_color(style, GTK_STATE_FLAG_NORMAL, &normal_color);
	self->bg.red   = GDK_TO_PANGO_COLOR(normal_color.red);
	self->bg.green = GDK_TO_PANGO_COLOR(normal_color.green);
	self->bg.blue  = GDK_TO_PANGO_COLOR(normal_color.blue);
	self->bg_alpha = GDK_TO_PANGO_COLOR(normal_color.alpha);

	/*
	 * If Pango does not support transparent foregrounds,
	 * it will at least use a high-contrast foreground.
	 *
	 * NOTE: It would be very hard to get an appropriate background
	 * color even if Gtk supports it since the label itself may
	 * not have one but one of its parents.
	 *
	 * FIXME: We may want to honour the background color,
	 * so we can at least get decent reverse text when setting
	 * the background color in the CSS.
	 */
	self->fg.red   = normal_color.red > 0.5 ? 0 : G_MAXUINT16;
	self->fg.green = normal_color.green > 0.5 ? 0 : G_MAXUINT16;
	self->fg.blue  = normal_color.blue > 0.5 ? 0 : G_MAXUINT16;
	/* try hard to get a transparent foreground anyway */
	self->fg_alpha = 0;

	/*
	 * The widget might be styled after the text has been set on it,
	 * we must recreate the Pango attributes.
	 */
	if (self->string.len > 0) {
		PangoAttrList *attribs = NULL;
		g_autofree gchar *plaintext = NULL;

		teco_gtk_label_parse_string(self->string.data, self->string.len,
		                            &self->fg, self->fg_alpha,
		                            &self->bg, self->bg_alpha,
		                            &attribs, &plaintext);

		gtk_label_set_attributes(GTK_LABEL(self), attribs);
		pango_attr_list_unref(attribs);

		g_assert(!g_strcmp0(plaintext, gtk_label_get_text(GTK_LABEL(self))));
	}
}

static void
teco_gtk_label_class_init(TecoGtkLabelClass *klass)
{
	GTK_WIDGET_CLASS(klass)->style_updated = teco_gtk_label_style_updated;
	G_OBJECT_CLASS(klass)->finalize = teco_gtk_label_finalize;
}

static void teco_gtk_label_init(TecoGtkLabel *self) {}

GtkWidget *
teco_gtk_label_new(const gchar *str, gssize len)
{
	TecoGtkLabel *widget = TECO_GTK_LABEL(g_object_new(TECO_TYPE_GTK_LABEL, NULL));

	teco_gtk_label_set_text(widget, str, len);

	return GTK_WIDGET(widget);
}

static void
teco_gtk_label_add_highlight_attribs(PangoAttrList *attribs, PangoColor *fg, guint16 fg_alpha,
                                     PangoColor *bg, guint16 bg_alpha, guint index, gsize len)
{
	PangoAttribute *attr;

	/*
	 * NOTE: Transparent foreground do not seem to work,
	 * even in Pango v1.38.
	 * Perhaps, this has been fixed in later versions.
	 */
#if PANGO_VERSION_CHECK(1,38,0)
	attr = pango_attr_foreground_alpha_new(fg_alpha);
	attr->start_index = index;
	attr->end_index = index + len;
	pango_attr_list_insert(attribs, attr);

	attr = pango_attr_background_alpha_new(bg_alpha);
	attr->start_index = index;
	attr->end_index = index + len;
	pango_attr_list_insert(attribs, attr);
#endif

	attr = pango_attr_foreground_new(fg->red, fg->green, fg->blue);
	attr->start_index = index;
	attr->end_index = index + len;
	pango_attr_list_insert(attribs, attr);

	attr = pango_attr_background_new(bg->red, bg->green, bg->blue);
	attr->start_index = index;
	attr->end_index = index + len;
	pango_attr_list_insert(attribs, attr);
}

void
teco_gtk_label_parse_string(const gchar *str, gssize len, PangoColor *fg, guint16 fg_alpha,
                            PangoColor *bg, guint16 bg_alpha, PangoAttrList **attribs, gchar **text)
{
	if (len < 0)
		len = strlen(str);

	/*
	 * Approximate size of unformatted text.
	 */
	gsize text_len = 1; /* for trailing 0 */
	for (gint i = 0; i < len; i++)
		text_len += TECO_IS_CTL(str[i]) ? 3 : 1;

	*attribs = pango_attr_list_new();
	*text = g_malloc(text_len);

	gint index = 0;
	while (len > 0) {
		/*
		 * NOTE: This mapping is similar to
		 * teco_view_set_presentations()
		 */
		switch (*str) {
		case '\e':
			teco_gtk_label_add_highlight_attribs(*attribs,
			                                     fg, fg_alpha,
			                                     bg, bg_alpha,
			                                     index, 1);
			(*text)[index++] = '$';
			break;
		case '\r':
			teco_gtk_label_add_highlight_attribs(*attribs,
			                                     fg, fg_alpha,
			                                     bg, bg_alpha,
			                                     index, 2);
			(*text)[index++] = 'C';
			(*text)[index++] = 'R';
			break;
		case '\n':
			teco_gtk_label_add_highlight_attribs(*attribs,
			                                     fg, fg_alpha,
			                                     bg, bg_alpha,
			                                     index, 2);
			(*text)[index++] = 'L';
			(*text)[index++] = 'F';
			break;
		case '\t':
			teco_gtk_label_add_highlight_attribs(*attribs,
			                                     fg, fg_alpha,
			                                     bg, bg_alpha,
			                                     index, 3);
			(*text)[index++] = 'T';
			(*text)[index++] = 'A';
			(*text)[index++] = 'B';
			break;
		default:
			if (TECO_IS_CTL(*str)) {
				teco_gtk_label_add_highlight_attribs(*attribs,
				                                     fg, fg_alpha,
				                                     bg, bg_alpha,
				                                     index, 2);
				(*text)[index++] = '^';
				(*text)[index++] = TECO_CTL_ECHO(*str);
			} else {
				(*text)[index++] = *str;
			}
			break;
		}

		str++;
		len--;
	}

	/* null-terminate generated text */
	(*text)[index] = '\0';
}

void
teco_gtk_label_set_text(TecoGtkLabel *self, const gchar *str, gssize len)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(TECO_IS_GTK_LABEL(self));

	teco_string_clear(&self->string);
	teco_string_init(&self->string, str, len < 0 ? strlen(str) : len);

	g_autofree gchar *plaintext = NULL;

	if (self->string.len > 0) {
		PangoAttrList *attribs = NULL;

		teco_gtk_label_parse_string(self->string.data, self->string.len,
		                            &self->fg, self->fg_alpha,
		                            &self->bg, self->bg_alpha,
		                            &attribs, &plaintext);

		gtk_label_set_attributes(GTK_LABEL(self), attribs);
		pango_attr_list_unref(attribs);
	}

	gtk_label_set_text(GTK_LABEL(self), plaintext);
}
