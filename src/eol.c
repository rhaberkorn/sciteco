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

#include <Scintilla.h>

#include "sciteco.h"
#include "eol.h"

const gchar *
teco_eol_get_seq(gint eol_mode)
{
	switch (eol_mode) {
	case SC_EOL_CRLF:
		return "\r\n";
	case SC_EOL_CR:
		return "\r";
	case SC_EOL_LF:
	default:
		return "\n";
	}
}

static inline void
teco_eol_reader_init(teco_eol_reader_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->eol_style = -1;
}

static GIOStatus
teco_eol_reader_read_gio(teco_eol_reader_t *ctx, gsize *read_len, GError **error)
{
	return g_io_channel_read_chars(ctx->gio.channel, ctx->gio.buffer,
	                               sizeof(ctx->gio.buffer),
	                               read_len, error);
}

/** @memberof teco_eol_reader_t */
void
teco_eol_reader_init_gio(teco_eol_reader_t *ctx, GIOChannel *channel)
{
	teco_eol_reader_init(ctx);
	ctx->read_cb = teco_eol_reader_read_gio;

	teco_eol_reader_set_channel(ctx, channel);
}

static GIOStatus
teco_eol_reader_read_mem(teco_eol_reader_t *ctx, gsize *read_len, GError **error)
{
	*read_len = ctx->mem.len;
	ctx->mem.len = 0;
	/*
	 * On the first call, returns G_IO_STATUS_NORMAL,
	 * later G_IO_STATUS_EOF.
	 */
	return *read_len != 0 ? G_IO_STATUS_NORMAL : G_IO_STATUS_EOF;
}

/** @memberof teco_eol_reader_t */
void
teco_eol_reader_init_mem(teco_eol_reader_t *ctx, gchar *buffer, gsize len)
{
	teco_eol_reader_init(ctx);
	ctx->read_cb = teco_eol_reader_read_mem;

	ctx->mem.buffer = buffer;
	ctx->mem.len = len;
}

/**
 * Read data with automatic EOL translation.
 *
 * This gets the next data block from the converter
 * implementation, performs EOL translation (if enabled)
 * in a more or less efficient manner and returns
 * a chunk of EOL-normalized data.
 *
 * Since the underlying data source may have to be
 * queried repeatedly and because the EOL Reader avoids
 * reassembling the EOL-normalized data by returning
 * references into the modified data source, it is
 * necessary to call this function repeatedly until
 * it returns G_IO_STATUS_EOF.
 *
 * @param ctx The EOL Reader object.
 * @param ret Location to store a pointer to the converted chunk.
 *            The EOL-converted data is NOT null-terminated.
 * @param data_len A pointer to the length of the converted chunk.
 * @param error A GError.
 * @return The status of the conversion.
 *
 * @memberof teco_eol_reader_t
 */
GIOStatus
teco_eol_reader_convert(teco_eol_reader_t *ctx, gchar **ret, gsize *data_len, GError **error)
{
	gchar *buffer = ctx->read_cb == teco_eol_reader_read_gio ? ctx->gio.buffer : ctx->mem.buffer;

	if (ctx->last_char < 0) {
		/* a CRLF was last translated */
		ctx->block_len++;
		ctx->last_char = '\n';
	}
	ctx->offset += ctx->block_len;

	if (ctx->offset >= ctx->read_len) {
		ctx->offset = 0;

		switch (ctx->read_cb(ctx, &ctx->read_len, error)) {
		case G_IO_STATUS_ERROR:
			return G_IO_STATUS_ERROR;

		case G_IO_STATUS_EOF:
			if (ctx->last_char == '\r') {
				/*
				 * Very last character read is CR.
				 * If this is the only EOL so far, the
				 * EOL style is MAC.
				 * This is also executed if auto-eol is disabled
				 * but it doesn't hurt.
				 */
				if (ctx->eol_style < 0)
					ctx->eol_style = SC_EOL_CR;
				else if (ctx->eol_style != SC_EOL_CR)
					ctx->eol_style_inconsistent = TRUE;
			}

			return G_IO_STATUS_EOF;

		case G_IO_STATUS_NORMAL:
		case G_IO_STATUS_AGAIN:
			break;
		}

		if (!(teco_ed & TECO_ED_AUTOEOL)) {
			/*
			 * No EOL translation - always return entire
			 * buffer
			 */
			*data_len = ctx->block_len = ctx->read_len;
			*ret = buffer;
			return G_IO_STATUS_NORMAL;
		}
	}

	/*
	 * Return data with automatic EOL translation.
	 * Every EOL sequence is normalized to LF and
	 * the first sequence determines the documents
	 * EOL style.
	 * This loop is executed for every byte of the
	 * file/stream, so it was important to optimize
	 * it. Specifically, the number of returns
	 * is minimized by keeping a pointer to
	 * the beginning of a block of data in the buffer
	 * which already has LFs (offset).
	 * Mac EOLs can be converted to UNIX EOLs directly
	 * in the buffer.
	 * So if their EOLs are consistent, the function
	 * will return one block for the entire buffer.
	 * When reading a file with DOS EOLs, there will
	 * be one call per line which is significantly slower.
	 */
	for (guint i = ctx->offset; i < ctx->read_len; i++) {
		switch (buffer[i]) {
		case '\n':
			if (ctx->last_char == '\r') {
				if (ctx->eol_style < 0)
					ctx->eol_style = SC_EOL_CRLF;
				else if (ctx->eol_style != SC_EOL_CRLF)
					ctx->eol_style_inconsistent = TRUE;

				/*
				 * Return block. CR has already
				 * been made LF in `buffer`.
				 */
				*data_len = ctx->block_len = i-ctx->offset;
				/* next call will skip the CR */
				ctx->last_char = -1;
				*ret = buffer + ctx->offset;
				return G_IO_STATUS_NORMAL;
			}

			if (ctx->eol_style < 0)
				ctx->eol_style = SC_EOL_LF;
			else if (ctx->eol_style != SC_EOL_LF)
				ctx->eol_style_inconsistent = TRUE;
			/*
			 * No conversion necessary and no need to
			 * return block yet.
			 */
			ctx->last_char = '\n';
			break;

		case '\r':
			if (ctx->last_char == '\r') {
				if (ctx->eol_style < 0)
					ctx->eol_style = SC_EOL_CR;
				else if (ctx->eol_style != SC_EOL_CR)
					ctx->eol_style_inconsistent = TRUE;
			}

			/*
			 * Convert CR to LF in `buffer`.
			 * This way more than one line using
			 * Mac EOLs can be returned at once.
			 */
			buffer[i] = '\n';
			ctx->last_char = '\r';
			break;

		default:
			if (ctx->last_char == '\r') {
				if (ctx->eol_style < 0)
					ctx->eol_style = SC_EOL_CR;
				else if (ctx->eol_style != SC_EOL_CR)
					ctx->eol_style_inconsistent = TRUE;
			}
			ctx->last_char = (guchar)buffer[i];
			break;
		}
	}

	/*
	 * Return remaining block.
	 * With UNIX/MAC EOLs, this will usually be the
	 * entire `buffer`
	 */
	*data_len = ctx->block_len = ctx->read_len-ctx->offset;
	*ret = buffer + ctx->offset;
	return G_IO_STATUS_NORMAL;
}

/** @memberof teco_eol_reader_t */
GIOStatus
teco_eol_reader_convert_all(teco_eol_reader_t *ctx, gchar **ret, gsize *out_len, GError **error)
{
	gsize buffer_len = ctx->read_cb == teco_eol_reader_read_gio
				? sizeof(ctx->gio.buffer) : ctx->mem.len;

	/*
	 * NOTE: Doesn't use teco_string_t to make use of GString's
	 * preallocation feature.
	 */
	GString *str = g_string_sized_new(buffer_len);

	for (;;) {
		gchar *data;
		gsize data_len;

		GIOStatus rc = teco_eol_reader_convert(ctx, &data, &data_len, error);
		if (rc == G_IO_STATUS_ERROR) {
			g_string_free(str, TRUE);
			return G_IO_STATUS_ERROR;
		}
		if (rc == G_IO_STATUS_EOF)
			break;

		g_string_append_len(str, data, data_len);
	}

	if (out_len)
		*out_len = str->len;
	*ret = g_string_free(str, FALSE);
	return G_IO_STATUS_NORMAL;
}

/** @memberof teco_eol_reader_t */
void
teco_eol_reader_clear(teco_eol_reader_t *ctx)
{
	if (ctx->read_cb == teco_eol_reader_read_gio && ctx->gio.channel)
		g_io_channel_unref(ctx->gio.channel);
}

static inline void
teco_eol_writer_init(teco_eol_writer_t *ctx, gint eol_mode)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->eol_seq = teco_eol_get_seq(eol_mode);
	ctx->eol_seq_len = strlen(ctx->eol_seq);
}

static gssize
teco_eol_writer_write_gio(teco_eol_writer_t *ctx, const gchar *buffer, gsize buffer_len, GError **error)
{
	gsize bytes_written;

	switch (g_io_channel_write_chars(ctx->gio.channel, buffer, buffer_len,
	                                 &bytes_written, error)) {
	case G_IO_STATUS_ERROR:
		return -1;
	case G_IO_STATUS_EOF:
	case G_IO_STATUS_NORMAL:
	case G_IO_STATUS_AGAIN:
		break;
	}

	return bytes_written;
}

/** @memberof teco_eol_writer_t */
void
teco_eol_writer_init_gio(teco_eol_writer_t *ctx, gint eol_mode, GIOChannel *channel)
{
	teco_eol_writer_init(ctx, eol_mode);
	ctx->write_cb = teco_eol_writer_write_gio;
	teco_eol_writer_set_channel(ctx, channel);
}

static gssize
teco_eol_writer_write_mem(teco_eol_writer_t *ctx, const gchar *buffer, gsize buffer_len, GError **error)
{
	g_string_append_len(ctx->mem.str, buffer, buffer_len);
	return buffer_len;
}

/**
 * @note Currently uses GString instead of teco_string_t to allow making use
 * of preallocation.
 * On the other hand GString has a higher overhead.
 *
 * @memberof teco_eol_writer_t
 */
void
teco_eol_writer_init_mem(teco_eol_writer_t *ctx, gint eol_mode, GString *str)
{
	teco_eol_writer_init(ctx, eol_mode);
	ctx->write_cb = teco_eol_writer_write_mem;
	ctx->mem.str = str;
}

/**
 * Perform EOL-normalization on a buffer (if enabled) and
 * pass it to the underlying data sink.
 *
 * This can be called repeatedly to transform a larger
 * document - the buffer provided does not have to be
 * well-formed with regard to EOL sequences.
 *
 * @param ctx The EOL Reader object.
 * @param buffer The buffer to convert.
 * @param buffer_len The length of the data in buffer.
 * @param error A GError.
 * @return The number of bytes consumed/converted from buffer.
 *         A value smaller than 0 is returned in case of errors.
 *
 * @memberof teco_eol_writer_t
 */
gssize
teco_eol_writer_convert(teco_eol_writer_t *ctx, const gchar *buffer, gsize buffer_len, GError **error)
{
	if (!(teco_ed & TECO_ED_AUTOEOL))
		/*
		 * Write without EOL-translation:
		 * `state` is not required
		 * NOTE: This throws in case of errors
		 */
		return ctx->write_cb(ctx, buffer, buffer_len, error);

	/*
	 * Write to stream with EOL-translation.
	 * The document's EOL mode tells us what was guessed
	 * when its content was read in (presumably from a file)
	 * but might have been changed manually by the user.
	 * NOTE: This code assumes that the output stream is
	 * buffered, since otherwise it would be slower
	 * (has been benchmarked).
	 * NOTE: The loop is executed for every character
	 * in `buffer` and has been optimized for minimal
	 * function (i.e. GIOChannel) calls.
	 */
	guint i = 0;
	gsize bytes_written = 0;
	if (ctx->state == TECO_EOL_STATE_WRITE_LF) {
		/* complete writing a CRLF sequence */
		gssize rc = ctx->write_cb(ctx, "\n", 1, error);
		if (rc < 1)
			/* nothing written or error */
			return rc;
		ctx->state = TECO_EOL_STATE_START;
		bytes_written++;
		i++;
	}

	guint block_start = i;
	gssize block_written;
	while (i < buffer_len) {
		switch (buffer[i]) {
		case '\n':
			if (ctx->last_c == '\r') {
				/* EOL sequence already written */
				bytes_written++;
				block_start = i+1;
				break;
			}
			/* fall through */
		case '\r':
			block_written = ctx->write_cb(ctx, buffer+block_start, i-block_start, error);
			if (block_written < 0)
				return -1;
			bytes_written += block_written;
			if (block_written < i-block_start)
				return bytes_written;

			block_written = ctx->write_cb(ctx, ctx->eol_seq, ctx->eol_seq_len, error);
			if (block_written < 0)
				return -1;
			if (block_written == 0)
				return bytes_written;
			if (block_written < ctx->eol_seq_len) {
				/* incomplete EOL seq - we have written CR of CRLF */
				ctx->state = TECO_EOL_STATE_WRITE_LF;
				return bytes_written;
			}
			bytes_written++;

			block_start = i+1;
			break;
		}

		ctx->last_c = buffer[i++];
	}

	/*
	 * Write out remaining block (i.e. line)
	 */
	gssize rc = ctx->write_cb(ctx, buffer+block_start, buffer_len-block_start, error);
	return rc < 0 ? -1 : bytes_written + rc;
}

/** @memberof teco_eol_writer_t */
void
teco_eol_writer_clear(teco_eol_writer_t *ctx)
{
	if (ctx->write_cb == teco_eol_writer_write_gio && ctx->gio.channel)
		g_io_channel_unref(ctx->gio.channel);
}
