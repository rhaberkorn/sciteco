/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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

#include "sciteco.h"
#include "string-utils.h"
#include "interface.h"
#include "list.h"
#include "error.h"

guint teco_error_return_args = 0;

/*
 * FIXME: Does this have to be stored in teco_machine_main_t?
 * Probably becomes clear once we implement error handling by macros.
 */
guint teco_error_pos = 0, teco_error_line = 0, teco_error_column = 0;

void
teco_error_set_coord(const gchar *str, guint pos)
{
	teco_error_pos = pos;
	teco_string_get_coord(str, pos, &teco_error_line, &teco_error_column);
}

typedef enum {
	TECO_FRAME_QREG,
	TECO_FRAME_FILE,
	TECO_FRAME_EDHOOK,
	TECO_FRAME_TOPLEVEL
} teco_frame_type_t;

typedef struct teco_frame_t {
	teco_stailq_entry_t entry;

	teco_frame_type_t type;

	guint pos, line, column;

	/*
	 * NOTE: This is currently sufficient to describe all
	 * frame types. Otherwise, add an union.
	 */
	gchar name[];
} teco_frame_t;

/**
 * List of teco_frame_t describing the stack frames.
 *
 * Stack frames are collected deliberately unformatted
 * since there are future applications where displaying
 * a stack frame will not be necessary (e.g. error handled
 * by SciTECO macro).
 * Preformatting all stack frames would be very costly.
 */
static teco_stailq_head_t teco_frames = TECO_STAILQ_HEAD_INITIALIZER(&teco_frames);

void
teco_error_display_short(const GError *error)
{
	teco_interface_msg(TECO_MSG_ERROR, "%s (at %d)",
	                   error->message, teco_error_pos);
}

void
teco_error_display_full(const GError *error)
{
	teco_interface_msg(TECO_MSG_ERROR, "%s", error->message);

	guint nr = 0;

	for (teco_stailq_entry_t *cur = teco_frames.first; cur != NULL; cur = cur->next) {
		teco_frame_t *frame = (teco_frame_t *)cur;

		switch (frame->type) {
		case TECO_FRAME_QREG:
			teco_interface_msg(TECO_MSG_INFO,
			                   "#%d in Q-Register \"%s\" at %d (%d:%d)",
			                   nr, frame->name, frame->pos, frame->line, frame->column);
			break;
		case TECO_FRAME_FILE:
			teco_interface_msg(TECO_MSG_INFO,
			                   "#%d in file \"%s\" at %d (%d:%d)",
			                   nr, frame->name, frame->pos, frame->line, frame->column);
			break;
		case TECO_FRAME_EDHOOK:
			teco_interface_msg(TECO_MSG_INFO,
			                   "#%d in \"%s\" hook execution",
			                   nr, frame->name);
			break;
		case TECO_FRAME_TOPLEVEL:
			teco_interface_msg(TECO_MSG_INFO,
			                   "#%d in toplevel macro at %d (%d:%d)",
			                   nr, frame->pos, frame->line, frame->column);
			break;
		}

		nr++;
	}
}

static teco_frame_t *
teco_error_add_frame(teco_frame_type_t type, gsize size)
{
	teco_frame_t *frame = g_malloc(sizeof(teco_frame_t) + size);
	frame->type	= type;
	frame->pos	= teco_error_pos;
	frame->line	= teco_error_line;
	frame->column	= teco_error_column;
	teco_stailq_insert_tail(&teco_frames, &frame->entry);

	return frame;
}

void
teco_error_add_frame_qreg(const gchar *name, gsize len)
{
	g_autofree gchar *name_printable = teco_string_echo(name, len);
	teco_frame_t *frame = teco_error_add_frame(TECO_FRAME_QREG, strlen(name_printable) + 1);
	strcpy(frame->name, name_printable);
}

void
teco_error_add_frame_file(const gchar *name)
{
	teco_frame_t *frame = teco_error_add_frame(TECO_FRAME_FILE, strlen(name) + 1);
	strcpy(frame->name, name);
}

void
teco_error_add_frame_edhook(const gchar *type)
{
	teco_frame_t *frame = teco_error_add_frame(TECO_FRAME_EDHOOK, strlen(type) + 1);
	strcpy(frame->name, type);
}

void
teco_error_add_frame_toplevel(void)
{
	teco_error_add_frame(TECO_FRAME_TOPLEVEL, 0);
}

#ifndef NDEBUG
__attribute__((destructor))
#endif
void
teco_error_clear_frames(void)
{
	teco_stailq_entry_t *entry;
	while ((entry = teco_stailq_remove_head(&teco_frames)))
		g_free(entry);
}
