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

#include "sciteco.h"

const gchar *teco_eol_get_seq(gint eol_mode);

typedef struct teco_eol_reader_t teco_eol_reader_t;

struct teco_eol_reader_t {
	gsize read_len;
	guint offset;
	gsize block_len;
	gint last_char;

	gint eol_style;
	gboolean eol_style_inconsistent;

	GIOStatus (*read_cb)(teco_eol_reader_t *ctx, gsize *read_len, GError **error);

	/*
	 * NOTE: This wastes some bytes for "memory" readers,
	 * but avoids inheritance.
	 */
	union {
		struct {
			gchar buffer[1024];
			GIOChannel *channel;
		} gio;

		struct {
			gchar *buffer;
			gsize len;
		} mem;
	};
};

void teco_eol_reader_init_gio(teco_eol_reader_t *ctx, GIOChannel *channel);
void teco_eol_reader_init_mem(teco_eol_reader_t *ctx, gchar *buffer, gsize len);

/** @memberof teco_eol_reader_t */
static inline void
teco_eol_reader_set_channel(teco_eol_reader_t *ctx, GIOChannel *channel)
{
	if (ctx->gio.channel)
		g_io_channel_unref(ctx->gio.channel);
	ctx->gio.channel = channel;
	if (ctx->gio.channel)
		g_io_channel_ref(ctx->gio.channel);
}

GIOStatus teco_eol_reader_convert(teco_eol_reader_t *ctx, gchar **ret, gsize *data_len, GError **error);
GIOStatus teco_eol_reader_convert_all(teco_eol_reader_t *ctx, gchar **ret, gsize *out_len, GError **error);

void teco_eol_reader_clear(teco_eol_reader_t *ctx);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_eol_reader_t, teco_eol_reader_clear);

typedef struct teco_eol_writer_t teco_eol_writer_t;

struct teco_eol_writer_t {
	enum {
		TECO_EOL_STATE_START = 0,
		TECO_EOL_STATE_WRITE_LF
	} state;
	gchar last_c;
	const gchar *eol_seq;
	gsize eol_seq_len;

	gssize (*write_cb)(teco_eol_writer_t *ctx, const gchar *buffer, gsize buffer_len, GError **error);

	union {
		struct {
			GIOChannel *channel;
		} gio;

		struct {
			GString *str;
		} mem;
	};
};

void teco_eol_writer_init_gio(teco_eol_writer_t *ctx, gint eol_mode, GIOChannel *channel);
void teco_eol_writer_init_mem(teco_eol_writer_t *ctx, gint eol_mode, GString *str);

/** @memberof teco_eol_writer_t */
static inline void
teco_eol_writer_set_channel(teco_eol_writer_t *ctx, GIOChannel *channel)
{
	if (ctx->gio.channel)
		g_io_channel_unref(ctx->gio.channel);
	ctx->gio.channel = channel;
	if (ctx->gio.channel)
		g_io_channel_ref(ctx->gio.channel);
}

gssize teco_eol_writer_convert(teco_eol_writer_t *ctx, const gchar *buffer,
                               gsize buffer_len, GError **error);

void teco_eol_writer_clear(teco_eol_writer_t *ctx);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_eol_writer_t, teco_eol_writer_clear);
