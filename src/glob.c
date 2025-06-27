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
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "string-utils.h"
#include "file-utils.h"
#include "interface.h"
#include "parser.h"
#include "core-commands.h"
#include "expressions.h"
#include "qreg.h"
#include "ring.h"
#include "error.h"
#include "undo.h"
#include "glob.h"

/*
 * FIXME: This state could be static.
 */
TECO_DECLARE_STATE(teco_state_glob_filename);

/** @memberof teco_globber_t */
void
teco_globber_init(teco_globber_t *ctx, const gchar *pattern, GFileTest test)
{
	if (!pattern)
		pattern = "";

	memset(ctx, 0, sizeof(*ctx));
	ctx->test = test;

	/*
	 * This finds the directory component including
	 * any trailing directory separator
	 * without making up a directory if it is missing
	 * (as g_path_get_dirname() does).
	 * Important since it allows us to construct
	 * file names with the exact same directory
	 * prefix as the input pattern.
	 */
	gsize dirname_len = teco_file_get_dirname_len(pattern);
	ctx->dirname = g_strndup(pattern, dirname_len);

	ctx->dir = g_dir_open(*ctx->dirname ? ctx->dirname : ".", 0, NULL);
	/* if dirname does not exist, the result may be NULL */

	ctx->pattern = teco_globber_compile_pattern(pattern + dirname_len);
}

/** @memberof teco_globber_t */
gchar *
teco_globber_next(teco_globber_t *ctx)
{
	const gchar *basename;

	if (!ctx->dir)
		return NULL;

	while ((basename = g_dir_read_name(ctx->dir))) {
		if (!g_regex_match(ctx->pattern, basename, 0, NULL))
			continue;

		/*
		 * As dirname includes the directory separator,
		 * we can simply concatenate dirname with basename.
		 */
		gchar *filename = g_strconcat(ctx->dirname, basename, NULL);

		/*
		 * No need to perform file test for EXISTS since
		 * g_dir_read_name() will only return existing entries
		 */
		if (ctx->test == G_FILE_TEST_EXISTS || g_file_test(filename, ctx->test))
			return filename;

		g_free(filename);
	}

	return NULL;
}

/** @memberof teco_globber_t */
void
teco_globber_clear(teco_globber_t *ctx)
{
	if (ctx->pattern)
		g_regex_unref(ctx->pattern);
	if (ctx->dir)
		g_dir_close(ctx->dir);
	g_free(ctx->dirname);
}

/** @static @memberof teco_globber_t */
gchar *
teco_globber_escape_pattern(const gchar *pattern)
{
	if (!pattern)
		return g_strdup("");

	gsize escaped_len = 1;
	gchar *escaped, *pout;

	/*
	 * NOTE: The exact size of the escaped string is easy to calculate
	 * in O(n) just like strlen(pattern), so we can just as well
	 * do that.
	 */
	for (const gchar *pin = pattern; *pin; pin++) {
		switch (*pin) {
		case '*':
		case '?':
		case '[':
			escaped_len += 3;
			break;
		default:
			escaped_len++;
			break;
		}
	}
	pout = escaped = g_malloc(escaped_len);

	while (*pattern) {
		switch (*pattern) {
		case '*':
		case '?':
		case '[':
			*pout++ = '[';
			*pout++ = *pattern;
			*pout++ = ']';
			break;
		default:
			*pout++ = *pattern;
			break;
		}

		pattern++;
	}
	*pout = '\0';

	return escaped;
}

/**
 * Compile a fnmatch(3)-compatible glob pattern to
 * a PCRE regular expression.
 *
 * There is GPattern, but it only supports the
 * "*" and "?" wildcards which most importantly
 * do not allow escaping.
 *
 * @param pattern The pattern to compile.
 * @return A new compiled regular expression object.
 *         Always non-NULL. Unref after use.
 *
 * @static @memberof teco_globber_t
 */
GRegex *
teco_globber_compile_pattern(const gchar *pattern)
{
	enum {
		STATE_WILDCARD,
		STATE_CLASS_START,
		STATE_CLASS_NEGATE,
		STATE_CLASS
	} state = STATE_WILDCARD;

	/*
	 * NOTE: The conversion to regex needs at most two
	 * characters per input character and the regex pattern
	 * is required only temporarily, so we use a fixed size
	 * buffer avoiding reallocations but wasting a few bytes
	 * (determining the exact required space would be tricky).
	 * It is not allocated on the stack though since pattern
	 * might be arbitrary user input and we must avoid
	 * stack overflows at all costs.
	 */
	g_autofree gchar *pattern_regex = g_malloc(strlen(pattern)*2 + 1 + 1);
	gchar *pout = pattern_regex;

	while (*pattern) {
		if (state == STATE_WILDCARD) {
			/*
			 * Outside a character class/set.
			 */
			switch (*pattern) {
			case '*':
				*pout++ = '.';
				*pout++ = '*';
				break;
			case '?':
				*pout++ = '.';
				break;
			case '[':
				/*
				 * The special case of an unclosed character
				 * class is allowed in fnmatch(3) but invalid
				 * in PCRE, so we must check for it explicitly.
				 * FIXME: This is sort of inefficient...
				 */
				if (strchr(pattern, ']')) {
					state = STATE_CLASS_START;
					*pout++ = '[';
					break;
				}
				/* fall through: escape PCRE metacharacters */
			case '\\':
			case '^':
			case '$':
			case '.':
			case '|':
			case '(':
			case ')':
			case '+':
			case '{':
				*pout++ = '\\';
				/* fall through */
			default:
				*pout++ = *pattern;
				break;
			}
		} else {
			/*
			 * Within a character class/set.
			 */
			switch (*pattern) {
			case '!':
				/*
				 * fnmatch(3) allows ! instead of ^ immediately
				 * after the opening bracket.
				 */
				if (state > STATE_CLASS_START) {
					state = STATE_CLASS;
					*pout++ = '!';
					break;
				}
				/* fall through */
			case '^':
				state = state == STATE_CLASS_START
					? STATE_CLASS_NEGATE : STATE_CLASS;
				*pout++ = '^';
				break;
			case ']':
				/*
				 * fnmatch(3) allows the closing bracket as the
				 * first character to include it in the set, while
				 * PCRE requires it to be escaped.
				 */
				if (state == STATE_CLASS) {
					state = STATE_WILDCARD;
					*pout++ = ']';
					break;
				}
				/* fall through: escape PCRE metacharacters */
			case '\\':
			case '[':
				*pout++ = '\\';
				/* fall through */
			case '-':
			default:
				state = STATE_CLASS;
				*pout++ = *pattern;
				break;
			}
		}

		pattern++;
	}
	*pout++ = '$';
	*pout = '\0';

	GRegex *pattern_compiled = g_regex_new(pattern_regex,
	                                       G_REGEX_DOTALL | G_REGEX_ANCHORED, 0, NULL);
	/*
	 * Since the regex is generated from patterns that are
	 * always valid, there must be no syntactic error.
	 */
	g_assert(pattern_compiled != NULL);

	return pattern_compiled;
}

/*
 * Command States
 */

static teco_state_t *
teco_state_glob_pattern_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_glob_filename;

	if (str->len > 0) {
		g_autofree gchar *filename = teco_file_expand_path(str->data);

		teco_qreg_t *glob_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
		g_assert(glob_reg != NULL);
		if (!glob_reg->vtable->undo_set_string(glob_reg, error) ||
		    !glob_reg->vtable->set_string(glob_reg, filename, strlen(filename),
		                                  teco_default_codepage(), error))
			return NULL;
	}

	return &teco_state_glob_filename;
}

/*$ EN glob
 * [type]EN[pattern]$[filename]$ -- Glob files or match filename and check file type
 * [type]:EN[pattern]$[filename]$ -> Success|Failure
 *
 * EN is a powerful command for performing various tasks
 * given a glob \fIpattern\fP.
 * For a description of the glob pattern syntax, refer to the section
 * .B Glob Patterns
 * for details.
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
 * For instance, \(lqENf??\[sl]*.c\fB$\fPfoo/bar.c\fB$\fP\(rq will
 * always match and the string \(lqfoo/bar.c\(rq will be inserted
 * (see below).
 *
 * By default, if EN is not colon-modified, the result of
 * globbing or file name matching is inserted into the current
 * document, at the current position.
 * The file names will be separated by line feeds, i.e.
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
TECO_DEFINE_STATE_EXPECTGLOB(teco_state_glob_pattern,
	.expectstring.last = FALSE
);

static teco_state_t *
teco_state_glob_filename_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	GFileTest file_flags = G_FILE_TEST_EXISTS;

	gboolean matching = FALSE;
	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	teco_int_t teco_test_mode;

	if (!teco_expressions_eval(FALSE, error) ||
	    !teco_expressions_pop_num_calc(&teco_test_mode, 0, error))
		return NULL;
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
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid file test %" TECO_INT_FORMAT " for <EN>",
		            teco_test_mode);
		return NULL;
	}

	teco_qreg_t *glob_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(glob_reg != NULL);
	g_auto(teco_string_t) pattern_str = {NULL, 0};
	if (!glob_reg->vtable->get_string(glob_reg, &pattern_str.data, &pattern_str.len,
	                                  NULL, error))
		return NULL;
	if (teco_string_contains(&pattern_str, '\0')) {
		teco_error_qregcontainsnull_set(error, "_", 1, FALSE);
		return NULL;
	}

	if (str->len > 0) {
		/*
		 * Match pattern against provided file name
		 */
		g_autofree gchar *filename = teco_file_expand_path(str->data);
		g_autoptr(GRegex) pattern = teco_globber_compile_pattern(pattern_str.data);

		if (g_regex_match(pattern, filename, 0, NULL) &&
		    (teco_test_mode == 0 || g_file_test(filename, file_flags))) {
			if (!colon_modified) {
				gsize len = strlen(filename);

				sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
				teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(pos);
				teco_undo_int(teco_ranges[0].to) = teco_interface_bytes2glyphs(pos + len + 1);
				teco_undo_guint(teco_ranges_count) = 1;

				/*
				 * FIXME: Filenames may contain linefeeds.
				 * But if we add them null-terminated, they will be relatively hard to parse.
				 */
				filename[len] = '\n';
				teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
				teco_interface_ssm(SCI_ADDTEXT, len+1, (sptr_t)filename);
				teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
			}

			matching = TRUE;
		}
	} else if (colon_modified) {
		/*
		 * Match pattern against directory contents (globbing),
		 * returning TECO_SUCCESS if at least one file matches
		 */
		g_auto(teco_globber_t) globber;

		teco_globber_init(&globber, pattern_str.data, file_flags);
		g_autofree gchar *globbed_filename = teco_globber_next(&globber);

		matching = globbed_filename != NULL;
	} else {
		/*
		 * Match pattern against directory contents (globbing),
		 * inserting all matching file names (null-byte-terminated)
		 */
		g_auto(teco_globber_t) globber;
		teco_globber_init(&globber, pattern_str.data, file_flags);

		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(pos);

		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);

		gchar *globbed_filename;
		while ((globbed_filename = teco_globber_next(&globber))) {
			gsize len = strlen(globbed_filename);
			pos += len+1;

			/*
			 * FIXME: Filenames may contain linefeeds.
			 * But if we add them null-terminated, they will be relatively hard to parse.
			 */
			globbed_filename[len] = '\n';
			teco_interface_ssm(SCI_ADDTEXT, len+1, (sptr_t)globbed_filename);

			g_free(globbed_filename);
			matching = TRUE;
		}

		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);

		teco_undo_int(teco_ranges[0].to) = teco_interface_bytes2glyphs(pos);
		teco_undo_guint(teco_ranges_count) = 1;
	}

	if (colon_modified) {
		teco_expressions_push(teco_bool(matching));
	} else if (matching) {
		/* text has been inserted */
		teco_ring_dirtify();
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	if (!glob_reg->vtable->undo_set_integer(glob_reg, error) ||
	    !glob_reg->vtable->set_integer(glob_reg, teco_bool(matching), error))
		return NULL;

	return &teco_state_start;
}

TECO_DEFINE_STATE_EXPECTFILE(teco_state_glob_filename);
