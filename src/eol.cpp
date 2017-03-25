/*
 * Copyright (C) 2012-2017 Robin Haberkorn
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

#include <glib.h>

#include "sciteco.h"
#include "error.h"
#include "eol.h"

namespace SciTECO {

/**
 * Read data with automatic EOL translation.
 *
 * This gets the next data block from the converter
 * implementation, performs EOL translation (if enabled)
 * in a more or less efficient manner and returns
 * a chunk of EOL-normalized data.
 *
 * Since the underlying data source may have to be
 * queried repeatedly and because EOLReader avoids
 * reassembling the EOL-normalized data by returning
 * references into the modified data source, it is
 * necessary to call this function repeatedly until
 * it returns NULL.
 *
 * Errors reading the data source are propagated
 * (as exceptions).
 *
 * @param data_len The length of the data chunk returned
 *                 by this function. Set on return.
 * @return A pointer to a chunk of EOL-normalized
 *         data of length data_len.
 *         It is NOT null-terminated.
 *         NULL is returned when all data has been converted.
 */
const gchar *
EOLReader::convert(gsize &data_len)
{
	if (last_char < 0) {
		/* a CRLF was last translated */
		block_len++;
		last_char = '\n';
	}
	offset += block_len;

	if (offset == read_len) {
		offset = 0;

		/*
		 * NOTE: This throws in case of errors
		 */
		if (!this->read(buffer, read_len)) {
			/* EOF */
			if (last_char == '\r') {
				/*
				 * Very last character read is CR.
				 * If this is the only EOL so far, the
				 * EOL style is MAC.
				 * This is also executed if auto-eol is disabled
				 * but it doesn't hurt.
				 */
				if (eol_style < 0)
					eol_style = SC_EOL_CR;
				else if (eol_style != SC_EOL_CR)
					eol_style_inconsistent = TRUE;
			}

			return NULL;
		}

		if (!(Flags::ed & Flags::ED_AUTOEOL)) {
			/*
			 * No EOL translation - always return entire
			 * buffer
			 */
			data_len = block_len = read_len;
			return buffer;
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
	for (guint i = offset; i < read_len; i++) {
		switch (buffer[i]) {
		case '\n':
			if (last_char == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CRLF;
				else if (eol_style != SC_EOL_CRLF)
					eol_style_inconsistent = TRUE;

				/*
				 * Return block. CR has already
				 * been made LF in `buffer`.
				 */
				data_len = block_len = i-offset;
				/* next call will skip the CR */
				last_char = -1;
				return buffer + offset;
			}

			if (eol_style < 0)
				eol_style = SC_EOL_LF;
			else if (eol_style != SC_EOL_LF)
				eol_style_inconsistent = TRUE;
			/*
			 * No conversion necessary and no need to
			 * return block yet.
			 */
			last_char = '\n';
			break;

		case '\r':
			if (last_char == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CR;
				else if (eol_style != SC_EOL_CR)
					eol_style_inconsistent = TRUE;
			}

			/*
			 * Convert CR to LF in `buffer`.
			 * This way more than one line using
			 * Mac EOLs can be returned at once.
			 */
			buffer[i] = '\n';
			last_char = '\r';
			break;

		default:
			if (last_char == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CR;
				else if (eol_style != SC_EOL_CR)
					eol_style_inconsistent = TRUE;
			}
			last_char = buffer[i];
			break;
		}
	}

	/*
	 * Return remaining block.
	 * With UNIX/MAC EOLs, this will usually be the
	 * entire `buffer`
	 */
	data_len = block_len = read_len-offset;
	return buffer + offset;
}

bool
EOLReaderGIO::read(gchar *buffer, gsize &read_len)
{
	GError *error = NULL;

	switch (g_io_channel_read_chars(channel, buffer,
	                                sizeof(EOLReaderGIO::buffer),
	                                &read_len, &error)) {
	case G_IO_STATUS_ERROR:
		throw GlibError(error);
	case G_IO_STATUS_EOF:
		return false;
	case G_IO_STATUS_NORMAL:
	case G_IO_STATUS_AGAIN:
		break;
	}

	return true;
}

bool
EOLReaderMem::read(gchar *buffer, gsize &read_len)
{
	read_len = buffer_len;
	buffer_len = 0;
	/*
	 * On the first call, returns true,
	 * later false (no more data).
	 */
	return read_len != 0;
}

/*
 * This could be in EOLReader as well, but this way, we
 * make use of the buffer_len to avoid unnecessary allocations.
 */
gchar *
EOLReaderMem::convert_all(gsize *out_len)
{
	GString *str = g_string_sized_new(buffer_len);
	const gchar *data;
	gsize data_len;

	try {
		while ((data = convert(data_len)))
			g_string_append_len(str, data, data_len);
	} catch (...) {
		g_string_free(str, TRUE);
		throw; /* forward */
	}

	if (out_len)
		*out_len = str->len;
	return g_string_free(str, FALSE);
}

/**
 * Perform EOL-normalization on a buffer (if enabled) and
 * pass it to the underlying data sink.
 *
 * This can be called repeatedly to transform a larger
 * document - the buffer provided does not have to be
 * well-formed with regard to EOL sequences.
 *
 * @param buffer The buffer to convert.
 * @param buffer_len The length of the data in buffer.
 * @return The number of bytes consumed/converted from buffer.
 */
gsize
EOLWriter::convert(const gchar *buffer, gsize buffer_len)
{
	gsize bytes_written;
	guint i = 0;
	guint block_start;
	gsize block_written;

	if (!(Flags::ed & Flags::ED_AUTOEOL))
		/*
		 * Write without EOL-translation:
		 * `state` is not required
		 * NOTE: This throws in case of errors
		 */
		return this->write(buffer, buffer_len);

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
	bytes_written = 0;
	if (state == STATE_WRITE_LF) {
		/* complete writing a CRLF sequence */
		if (this->write("\n", 1) < 1)
			return 0;
		state = STATE_START;
		bytes_written++;
		i++;
	}

	block_start = i;
	while (i < buffer_len) {
		switch (buffer[i]) {
		case '\n':
			if (last_c == '\r') {
				/* EOL sequence already written */
				bytes_written++;
				block_start = i+1;
				break;
			}
			/* fall through */
		case '\r':
			block_written = this->write(buffer+block_start, i-block_start);
			bytes_written += block_written;
			if (block_written < i-block_start)
				return bytes_written;

			block_written = this->write(eol_seq, eol_seq_len);
			if (block_written == 0)
				return bytes_written;
			if (block_written < eol_seq_len) {
				/* incomplete EOL seq - we have written CR of CRLF */
				state = STATE_WRITE_LF;
				return bytes_written;
			}
			bytes_written++;

			block_start = i+1;
			break;
		}

		last_c = buffer[i++];
	}

	/*
	 * Write out remaining block (i.e. line)
	 */
	bytes_written += this->write(buffer+block_start, buffer_len-block_start);
	return bytes_written;
}

gsize
EOLWriterGIO::write(const gchar *buffer, gsize buffer_len)
{
	gsize bytes_written;
	GError *error = NULL;

	switch (g_io_channel_write_chars(channel, buffer, buffer_len,
	                                 &bytes_written, &error)) {
	case G_IO_STATUS_ERROR:
		throw GlibError(error);
	case G_IO_STATUS_EOF:
	case G_IO_STATUS_NORMAL:
	case G_IO_STATUS_AGAIN:
		break;
	}

	return bytes_written;
}

gsize
EOLWriterMem::write(const gchar *buffer, gsize buffer_len)
{
	g_string_append_len(str, buffer, buffer_len);
	return buffer_len;
}

} /* namespace SciTECO */
