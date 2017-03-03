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

#ifndef __EOL_H
#define __EOL_H

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "memory.h"

namespace SciTECO {

class EOLReader : public Object {
	gchar *buffer;
	gsize read_len;
	guint offset;
	gsize block_len;
	gint last_char;

public:
	gint eol_style;
	gboolean eol_style_inconsistent;

	EOLReader(gchar *_buffer)
	         : buffer(_buffer),
	           read_len(0), offset(0), block_len(0),
	           last_char(0), eol_style(-1),
	           eol_style_inconsistent(FALSE) {}
	virtual ~EOLReader() {}

	const gchar *convert(gsize &data_len);

protected:
	virtual bool read(gchar *buffer, gsize &read_len) = 0;
};

class EOLReaderGIO : public EOLReader {
	gchar buffer[1024];
	GIOChannel *channel;

	bool read(gchar *buffer, gsize &read_len);

public:
	EOLReaderGIO(GIOChannel *_channel = NULL)
	            : EOLReader(buffer), channel(NULL)
	{
		set_channel(_channel);
	}

	inline void
	set_channel(GIOChannel *_channel = NULL)
	{
		if (channel)
			g_io_channel_unref(channel);
		channel = _channel;
		if (channel)
			g_io_channel_ref(channel);
	}

	~EOLReaderGIO()
	{
		set_channel();
	}
};

class EOLReaderMem : public EOLReader {
	gsize buffer_len;

	bool read(gchar *buffer, gsize &read_len);

public:
	EOLReaderMem(gchar *buffer, gsize _buffer_len)
	            : EOLReader(buffer), buffer_len(_buffer_len) {}

	gchar *convert_all(gsize *out_len = NULL);
};

class EOLWriter : public Object {
	enum {
		STATE_START = 0,
		STATE_WRITE_LF
	} state;
	gchar last_c;
	const gchar *eol_seq;
	gsize eol_seq_len;

public:
	EOLWriter(gint eol_mode) : state(STATE_START), last_c('\0')
	{
		eol_seq = get_eol_seq(eol_mode);
		eol_seq_len = strlen(eol_seq);
	}
	virtual ~EOLWriter() {}

	gsize convert(const gchar *buffer, gsize buffer_len);

protected:
	virtual gsize write(const gchar *buffer, gsize buffer_len) = 0;
};

class EOLWriterGIO : public EOLWriter {
	GIOChannel *channel;

	gsize write(const gchar *buffer, gsize buffer_len);

public:
	EOLWriterGIO(gint eol_mode)
	            : EOLWriter(eol_mode), channel(NULL) {}

	EOLWriterGIO(GIOChannel *_channel, gint eol_mode)
	            : EOLWriter(eol_mode), channel(NULL)
	{
		set_channel(_channel);
	}

	inline void
	set_channel(GIOChannel *_channel = NULL)
	{
		if (channel)
			g_io_channel_unref(channel);
		channel = _channel;
		if (channel)
			g_io_channel_ref(channel);
	}

	~EOLWriterGIO()
	{
		set_channel();
	}
};

class EOLWriterMem : public EOLWriter {
	GString *str;

	gsize write(const gchar *buffer, gsize buffer_len);

public:
	EOLWriterMem(GString *_str, gint eol_mode)
	            : EOLWriter(eol_mode), str(_str) {}
};

} /* namespace SciTECO */

#endif
