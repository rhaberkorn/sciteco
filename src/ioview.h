/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

#ifndef __IOVIEW_H
#define __IOVIEW_H

#include <glib.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"

namespace SciTECO {

/*
 * Auxiliary functions
 */

/**
 * Get absolute/full version of a possibly relative path.
 * Works with existing and non-existing paths (in the latter case,
 * heuristics may be applied.)
 *
 * @param path Possibly relative path name.
 * @return Newly-allocated absolute path name.
 */
gchar *get_absolute_path(const gchar *path);

bool file_is_visible(const gchar *path);

class IOView : public ViewCurrent {
	class UndoTokenRemoveFile : public UndoToken {
		gchar *filename;

	public:
		UndoTokenRemoveFile(const gchar *_filename)
				   : filename(g_strdup(_filename)) {}
		~UndoTokenRemoveFile()
		{
			g_free(filename);
		}

		void
		run(void)
		{
			g_unlink(filename);
		}
	};

public:
	void load(const gchar *filename);
	void save(const gchar *filename);
};

} /* namespace SciTECO */

#endif
