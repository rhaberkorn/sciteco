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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "interface.h"
#include "parser.h"
#include "ring.h"
#include "glob.h"

namespace SciTECO {

namespace States {
	StateGlob glob;
}

Globber::Globber(const gchar *_pattern)
{
	gchar *basename;

	dirname = g_path_get_dirname(_pattern);
	dir = g_dir_open(dirname, 0, NULL);

	basename = g_path_get_basename(_pattern);
	pattern = g_pattern_spec_new(basename);
	g_free(basename);
}

gchar *
Globber::next(void)
{
	const gchar *basename;

	while ((basename = g_dir_read_name(dir)))
		if (g_pattern_match_string(pattern, basename))
			return g_build_filename(dirname, basename, NIL);

	return NULL;
}

Globber::~Globber()
{
	if (pattern)
		g_pattern_spec_free(pattern);
	if (dir)
		g_dir_close(dir);
	g_free(dirname);
}

/*
 * Command States
 */

/*$
 * EN[pattern]$ -- Get list of files matching a glob pattern
 *
 * The EN command expands a glob \fIpattern\fP to a list of
 * matching file names. This is similar to globbing
 * on UNIX but not as powerful.
 * A \fIpattern\fP is a file name with \(lq*\(rq and
 * \(lq?\(rq wildcards:
 * \(lq*\(rq matches an arbitrary, possibly empty, string.
 * \(lq?\(rq matches an arbitrary character.
 *
 * EN will currently only match files in the file name component
 * of \fIpattern\fP, not on each component of the path name
 * separately.
 * In other words, EN only looks through the directory
 * of \fIpattern\fP \(em you cannot effectively match
 * multiple directories.
 *
 * The result of the globbing is inserted into the current
 * document, at the current position.
 * A linefeed is inserted after every file name, i.e.
 * every matching file will be on its own line.
 *
 * String-building characters are enabled for EN.
 */
/*
 * NOTE: This does not work like classic TECO's
 * EN command (iterative globbing), since the
 * position in the directory cannot be reasonably
 * reset on rubout with glib's API.
 * If we have to perform all the globbing on initialization
 * we can just as well return all the results at once.
 * And we can add them to the current document since
 * when they should be in a register, the user will
 * have to edit that register anyway.
 */
State *
StateGlob::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	Globber globber(str);

	gchar *filename;
	bool text_added = false;

	interface.ssm(SCI_BEGINUNDOACTION);

	while ((filename = globber.next())) {
		size_t len = strlen(filename);
		/* overwrite trailing null */
		filename[len] = '\n';

		/*
		 * FIXME: Once we're 8-bit clean, we should
		 * add the filenames null-terminated
		 * (there may be linebreaks in filename).
		 */
		interface.ssm(SCI_ADDTEXT, len+1,
		              (sptr_t)filename);

		g_free(filename);
		text_added = true;
	}

	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);

	if (text_added) {
		ring.dirtify();
		if (current_doc_must_undo())
			interface.undo_ssm(SCI_UNDO);
	}

	return &States::start;
}

} /* namespace SciTECO */
