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
#pragma once

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "interface.h"

#define TECO_TYPE_GTK_INFO_POPUP teco_gtk_info_popup_get_type()
G_DECLARE_FINAL_TYPE(TecoGtkInfoPopup, teco_gtk_info_popup, TECO, GTK_INFO_POPUP, GtkEventBox)

GtkWidget *teco_gtk_info_popup_new(void);

void teco_gtk_info_popup_add(TecoGtkInfoPopup *self,
                             teco_popup_entry_type_t type,
                             const gchar *name, gssize len,
                             gboolean highlight);
void teco_gtk_info_popup_scroll_page(TecoGtkInfoPopup *self);
void teco_gtk_info_popup_clear(TecoGtkInfoPopup *self);

gboolean teco_gtk_info_popup_get_position_in_overlay(GtkOverlay *overlay,
                                                     GtkWidget *widget,
                                                     GdkRectangle *allocation,
                                                     gpointer user_data);
GIcon *teco_gtk_info_popup_get_icon_for_path(const gchar *path,
                                             const gchar *fallback_name);
