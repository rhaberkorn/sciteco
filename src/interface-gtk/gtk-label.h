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
#pragma once

#include <glib.h>
#include <glib-object.h>

#include <gtk/gtk.h>

#define TECO_TYPE_GTK_LABEL teco_gtk_label_get_type()
G_DECLARE_FINAL_TYPE(TecoGtkLabel, teco_gtk_label, TECO, GTK_LABEL, GtkLabel)

GtkWidget *teco_gtk_label_new(const gchar *str, gssize len);

void teco_gtk_label_set_text(TecoGtkLabel *self, const gchar *str, gssize len);
const teco_string_t *teco_gtk_label_get_text(TecoGtkLabel *self);

void teco_gtk_label_parse_string(const gchar *str, gssize len,
                                 PangoColor *fg, guint16 fg_alpha,
                                 PangoColor *bg, guint16 bg_alpha,
                                 PangoAttrList **attribs, gchar **text);
