/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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
#include <glib/gstdio.h>

#include "sciteco.h"
#include "interface.h"
#include "parser.h"
#include "expressions.h"
#include "qregisters.h"
#include "ring.h"
#include "ioview.h"
#include "glob.h"

namespace SciTECO {

namespace States {
	StateGlob_pattern	glob_pattern;
	StateGlob_filename	glob_filename;
}

Globber::Globber(const gchar *pattern, GFileTest _test)
                : test(_test)
{
	gsize dirname_len;

	/*
	 * This finds the directory component including
	 * any trailing directory separator
	 * without making up a directory if it is missing
	 * (as g_path_get_dirname() does).
	 * Important since it allows us to construct
	 * file names with the exact same directory
	 * prefix as the input pattern.
	 */
	dirname_len = file_get_dirname_len(pattern);
	dirname = g_strndup(pattern, dirname_len);

	dir = g_dir_open(*dirname ? dirname : ".", 0, NULL);
	/* if dirname does not exist, dir may be NULL */

	Globber::pattern = g_pattern_spec_new(pattern + dirname_len);
}

gchar *
Globber::next(void)
{
	const gchar *basename;

	if (!dir)
		return NULL;

	while ((basename = g_dir_read_name(dir))) {
		gchar *filename;

		if (!g_pattern_match_string(pattern, basename))
			continue;

		/*
		 * As dirname includes the directory separator,
		 * we can simply concatenate dirname with basename.
		 */
		filename = g_strconcat(dirname, basename, NIL);

		/*
		 * No need to perform file test for EXISTS since
		 * g_dir_read_name() will only return existing entries
		 */
		if (test == G_FILE_TEST_EXISTS || g_file_test(filename, test))
			return filename;

		g_free(filename);
	}

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
 * [type]EN[pattern]$[filename]$ -- Glob files or match filename and check file type
 * [type]:EN[pattern]$[filename]$ -> Success|Failure
 *
 * EN is a powerful command for performing various tasks
 * given a glob \fIpattern\fP.
 * A \fIpattern\fP is a file name with \(lq*\(rq and
 * \(lq?\(rq wildcards:
 * \(lq*\(rq matches an arbitrary, possibly empty, string.
 * \(lq?\(rq matches an arbitrary character.
 *
 * \fIpattern\fP may be omitted, in which case it defaults
 * to the pattern saved in the search and glob register \(lq_\(rq.
 * If it is specified, it overwrites the contents of the register
 * \(lq_\(rq with \fIpattern\fP.
 * This behaviour is similar to the search and replace commands
 * and allows for repeated globbing/matching with the same
 * pattern.
 * Therefoe you should also save the \(lq_\(rq register on the
 * Q-Register stack when calling EN from portable macros.
 *
 * If \fIfilename\fP is omitted (empty), EN may be used to expand
 * a glob \fIpattern\fP to a list of matching file names.
 * This is similar to globbing
 * on UNIX but not as powerful and may be used e.g. for
 * iterating over directory contents.
 * E.g. \(lqEN*.c\fB$$\fP\(rq expands to all \(lq.c\(rq files
 * in the current directory.
 * The resulting file names have the exact same directory
 * component as \fIpattern\fP (if any).
 * Without \fIfilename\fP, EN will currently only match files
 * in the file name component
 * of \fIpattern\fP, not on each component of the path name
 * separately.
 * In other words, EN only looks through the directory
 * of \fIpattern\fP \(em you cannot effectively match
 * multiple directories.
 *
 * If \fIfilename\fP is specified, \fIpattern\fP will only
 * be matched against that single file name.
 * If it matches, \fIfilename\fP is used verbatim.
 * In this form, \fIpattern\fP is matched against the entire
 * file name, so it is possible to match directory components
 * as well.
 * \fIfilename\fP does not necessarily have to exist in the
 * file system for the match to succeed (unless a file type check
 * is also specified).
 * For instance, \(lqENf??/\[**].c\fB$\fPfoo/bar.c\fB$\fP\(rq will
 * always match and the string \(lqfoo/bar.c\(rq will be inserted
 * (see below).
 *
 * By default, if EN is not colon-modified, the result of
 * globbing or file name matching is inserted into the current
 * document, at the current position.
 * A linefeed is inserted after every file name, i.e.
 * every matching file will be on its own line.
 *
 * EN may be colon-modified to avoid any text insertion.
 * Instead, a boolean is returned that signals whether
 * any file matched \fIpattern\fP.
 * E.g. \(lq:EN*.c\fB$$\fP\(rq returns success (-1) if
 * there is at least one \(lq.c\(rq file in the current directory.
 *
 * The results of EN may be filtered by specifying a numeric file
 * \fItype\fP check argument.
 * This argument may be omitted (as in the examples above) and defaults
 * to 0, i.e. no additional checking.
 * The following file type check values are currently defined:
 * .IP 0 4
 * No file type checking is performed.
 * Note however, that when globbing only directory contents
 * (of any type) are used, so without the \fIfilename\fP
 * argument, the value 0 is equivalent to 5.
 * .IP 1
 * Only match \fIregular files\fP (no directories).
 * Will also match symlinks to regular files (on platforms
 * supporting symlinks).
 * .IP 2
 * Only match \fIsymlinks\fP.
 * On platforms without symlinks (non-UNIX), this will never
 * match anything.
 * .IP 3
 * Only match \fIdirectories\fP.
 * .IP 4
 * Only match \fIexecutables\fP.
 * On UNIX, the executable flag is evaluated, while on
 * Windows only the file name is checked.
 * .IP 5
 * Only match existing files or directories.
 * When globbing, this check makes no sense and is
 * equivalent to no check at all.
 * It may however be used to test that a filename refers
 * to an existing file.
 *
 * For instance, \(lq3EN*\fB$$\fP\(rq will expand to
 * all subdirectories in the current directory.
 * The following idiom may be used to check whether
 * a given filename refers to a regular file:
 * 1:EN*\fB$\fIfilename\fB$\fR
 *
 * Note that both without colon and colon modified
 * forms of EN save the success or failure of the
 * operation in the numeric part of the glob register
 * \(lq_\(rq (i.e. the same value that the colon modified
 * form would return).
 * The command itself never fails because of failure
 * in matching any files.
 * E.g. if \(lqEN*.c\fB$$\fP\(rq does not match any
 * files, the EN command is still successful but does
 * not insert anything. A failure boolean would be saved
 * in \(lq_\(rq, though.
 *
 * String-building characters are enabled for EN and
 * both string arguments are considered file names
 * with regard to auto-completions.
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
StateGlob_pattern::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::glob_filename);

	if (*filename) {
		QRegister *glob_reg = QRegisters::globals["_"];

		glob_reg->undo_set_string();
		glob_reg->set_string(filename);
	}

	return &States::glob_filename;
}

State *
StateGlob_filename::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);

	tecoInt teco_test_mode;
	GFileTest file_flags = G_FILE_TEST_EXISTS;

	bool matching = false;
	bool colon_modified = eval_colon();

	QRegister *glob_reg = QRegisters::globals["_"];
	gchar *pattern_str;

	expressions.eval();
	teco_test_mode = expressions.pop_num_calc(0, 0);
	switch (teco_test_mode) {
	/*
	 * 0 means, no file testing.
	 * file_flags will still be G_FILE_TEST_EXISTS which
	 * is equivalent to no testing when using the Globber class.
	 */
	case 0: break;
	case 1: file_flags = G_FILE_TEST_IS_REGULAR; break;
	case 2: file_flags = G_FILE_TEST_IS_SYMLINK; break;
	case 3: file_flags = G_FILE_TEST_IS_DIR; break;
	case 4: file_flags = G_FILE_TEST_IS_EXECUTABLE; break;
	case 5: file_flags = G_FILE_TEST_EXISTS; break;
	default:
		throw Error("Invalid file test %" TECO_INTEGER_FORMAT
		            " for <EN>", teco_test_mode);
	}

	pattern_str = glob_reg->get_string();

	if (*filename) {
		/*
		 * Match pattern against provided file name
		 */
		if (g_pattern_match_simple(pattern_str, filename) &&
		    (!teco_test_mode || g_file_test(filename, file_flags))) {
			if (!colon_modified) {
				interface.ssm(SCI_BEGINUNDOACTION);
				interface.ssm(SCI_ADDTEXT, strlen(filename),
				              (sptr_t)filename);
				interface.ssm(SCI_ADDTEXT, 1, (sptr_t)"\n");
				interface.ssm(SCI_SCROLLCARET);
				interface.ssm(SCI_ENDUNDOACTION);
			}

			matching = true;
		}
	} else if (colon_modified) {
		/*
		 * Match pattern against directory contents (globbing),
		 * returning SUCCESS if at least one file matches
		 */
		Globber globber(pattern_str, file_flags);
		gchar *globbed_filename = globber.next();

		matching = globbed_filename != NULL;

		g_free(globbed_filename);
	} else {
		/*
		 * Match pattern against directory contents (globbing),
		 * inserting all matching file names (linefeed-terminated)
		 */
		Globber globber(pattern_str, file_flags);

		gchar *globbed_filename;

		interface.ssm(SCI_BEGINUNDOACTION);

		while ((globbed_filename = globber.next())) {
			size_t len = strlen(globbed_filename);
			/* overwrite trailing null */
			globbed_filename[len] = '\n';

			/*
			 * FIXME: Once we're 8-bit clean, we should
			 * add the filenames null-terminated
			 * (there may be linebreaks in filename).
			 */
			interface.ssm(SCI_ADDTEXT, len+1,
			              (sptr_t)globbed_filename);

			g_free(globbed_filename);
			matching = true;
		}

		interface.ssm(SCI_SCROLLCARET);
		interface.ssm(SCI_ENDUNDOACTION);
	}

	g_free(pattern_str);

	if (colon_modified) {
		expressions.push(TECO_BOOL(matching));
	} else if (matching) {
		/* text has been inserted */
		ring.dirtify();
		if (current_doc_must_undo())
			interface.undo_ssm(SCI_UNDO);
	}

	glob_reg->undo_set_integer();
	glob_reg->set_integer(TECO_BOOL(matching));

	return &States::start;
}

} /* namespace SciTECO */
