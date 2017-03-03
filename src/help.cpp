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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "string-utils.h"
#include "parser.h"
#include "qregisters.h"
#include "ring.h"
#include "interface.h"
#include "help.h"

namespace SciTECO {

HelpIndex help_index;

namespace States {
	StateGetHelp gethelp;
}

void
HelpIndex::load(void)
{
	gchar *lib_path;
	gchar *women_path;
	GDir *women_dir;
	const gchar *basename;

	if (G_LIKELY(min() != NULL))
		/* already loaded */
		return;

	lib_path = QRegisters::globals["$SCITECOPATH"]->get_string();
	women_path = g_build_filename(lib_path, "women", NIL);
	g_free(lib_path);

	women_dir = g_dir_open(women_path, 0, NULL);
	if (!women_dir) {
		g_free(women_path);
		return;
	}

	while ((basename = g_dir_read_name(women_dir))) {
		gchar *filename, *filename_tec;
		FILE *file;
		gchar buffer[1024];
		gchar *topic;

		if (!g_str_has_suffix(basename, ".woman"))
			continue;

		/*
		 * Open the corresponding SciTECO macro to read
		 * its first line.
		 */
		filename = g_build_filename(women_path, basename, NIL);
		filename_tec = g_strconcat(filename, ".tec", NIL);
		file = g_fopen(filename_tec, "r");
		g_free(filename_tec);
		if (!file) {
			/*
			 * There might simply be no support script for
			 * simple plain-text woman-pages.
			 * In this case we create a topic using the filename
			 * without an extension.
			 */
			topic = g_strndup(basename, strlen(basename)-6);
			set(topic, filename);
			g_free(topic);
			g_free(filename);
			continue;
		}

		/*
		 * Each womanpage script begins with a special comment
		 * header containing the position to topic index.
		 * Every topic will be on its own line and they are unlikely
		 * to be very long, so we can use fgets() here.
		 * NOTE: Since we haven't opened with the "b" flag,
		 * fgets() will translate linebreaks to LF even on
		 * MSVCRT (Windows).
		 */
		if (!fgets(buffer, sizeof(buffer), file) ||
		    !g_str_has_prefix(buffer, "!*")) {
			interface.msg(InterfaceCurrent::MSG_WARNING,
			              "Missing or invalid topic line in womanpage script \"%s\"",
			              filename);
			g_free(filename);
			continue;
		}
		/* skip opening comment */
		topic = buffer+2;

		do {
			gchar *endptr;
			tecoInt pos = strtoul(topic, &endptr, 10);
			gsize len;

			/*
			 * This also breaks at the last line of the
			 * header.
			 */
			if (*endptr != ':')
				break;

			/*
			 * Strip the likely LF at the end of the line.
			 */
			len = strlen(endptr)-1;
			if (G_LIKELY(endptr[len] == '\n'))
				endptr[len] = '\0';

			set(endptr+1, filename, pos);
		} while ((topic = fgets(buffer, sizeof(buffer), file)));

		fclose(file);
		g_free(filename);
	}

	g_dir_close(women_dir);
	g_free(women_path);
}

HelpIndex::Topic *
HelpIndex::find(const gchar *name)
{
	Topic *ret;

	/*
	 * The topic index contains printable characters
	 * only (to avoid having to perform string building
	 * on the topic terms to be able to define control
	 * characters).
	 * Therefore, we expand control characters in the
	 * look-up string to their printable forms.
	 */
	gchar *term = String::canonicalize_ctl(name);

	ret = (Topic *)RBTreeStringCase::find(term);

	g_free(term);
	return ret;
}

void
HelpIndex::set(const gchar *name, const gchar *filename, tecoInt pos)
{
	Topic *topic = new Topic(name, filename, pos);
	Topic *existing;

	existing = (Topic *)RBTree<RBEntryString>::find(topic);
	if (existing) {
		gchar *basename;

		if (!strcmp(existing->filename, filename)) {
			/*
			 * A topic with the same name already exists
			 * in the same file.
			 * For the time being, we simply overwrite the
			 * last topic.
			 * FIXME: Perhaps make it unique again!?
			 */
			existing->pos = pos;
			delete topic;
			return;
		}

		/* in another file -> make name unique */
		interface.msg(InterfaceCurrent::MSG_WARNING,
		              "Topic collision: \"%s\" defined in \"%s\" and \"%s\"",
		              name, existing->filename, filename);

		String::append(topic->name, ":");
		basename = g_path_get_basename(filename);
		String::append(topic->name, basename);
		g_free(basename);
	}

	RBTree::insert(topic);
}

/*
 * Command states
 */

/*$ "?" help
 * ?[topic]$ -- Get help for topic
 *
 * Look up <topic> in the help index, opening
 * the corresponding womanpage as a buffer and scrolling
 * to the topic's position.
 * The help index is built when this command is first
 * executed, so the help system does not consume resources
 * when not used (e.g. in a batch-mode script).
 *
 * \*(ST's help documents must be installed in the
 * directory \fB$SCITECOPATH/women\fP, i.e. as part of
 * the standard library.
 * Each document consist of at least one plain-text file with
 * the extension \(lq.woman\(rq.
 * Optionally, a \*(ST script with the extension
 * \(lq.woman.tec\(rq can be installed alongside the
 * main document to define topics covered by this document
 * and set up styling.
 *
 * The beginning of the script must be a header of the form:
 * .EX
 * !*\fIposition\fP:\fItopic1\fP
 * \fIposition2\fP:\fItopic2\fP
 * \fI...\fP
 * *!
 * .EE
 * In other words it must be a \*(ST comment followed
 * by an asterisk sign, followed by the first topic which
 * is a buffer position, followed by a colon and the topic
 * string.
 * The topic string is terminated by the end of the line.
 * The end of the header is marked by a single \(lq*!\(rq.
 * Topic terms should be specified with printable characters
 * only (e.g. use Caret+A instead of CTRL+A).
 * When looking up a help term, control characters are
 * canonicalized to their printable form, so the term
 * \(lq^A\(rq is found both by Caret+A and CTRL+A.
 * Also, while topic terms are not case folded, lookup
 * is case insensitive.
 *
 * The rest of the script is not read by \*(ST internally
 * but should contain styling for the main document.
 * It is usually read by the standard library's lexer
 * configuration system when showing a womanpage.
 * If the \(lq.woman.tec\(rq macro is missing,
 * \*(ST will define a single topic for the document based
 * on the \(lq.woman\(rq file's name.
 *
 * The combination of plain-text document and script
 * is called a \(lqwomanpage\(rq because these files
 * are usually generated using \fBgroff\fP(1) with the
 * \fIgrosciteco\fP formatter and the \fIsciteco.tmac\fP
 * GNU troff macros.
 * When using womanpages generated by \fIgrosciteco\fP,
 * help topics can be defined using the \fBSCITECO_TOPIC\fP
 * Troff macro.
 * This flexible system allows \*(ST to access internal
 * and third-party help files written in plain-text or
 * with an arbitrary GNU troff macro package.
 * As all GNU troff documents are processed at build-time,
 * GNU troff is not required at runtime.
 *
 * The \fB?\fP command does not have string building enabled.
 */
void
StateGetHelp::initial(void)
{
	/*
	 * The help-index is populated on demand,
	 * so we start up quicker and batch mode does
	 * not depend on the availability of the standard
	 * library.
	 */
	help_index.load();
}

State *
StateGetHelp::done(const gchar *str)
{
	HelpIndex::Topic *topic;

	BEGIN_EXEC(&States::start);

	topic = help_index.find(str);
	if (!topic)
		throw Error("Topic \"%s\" not found", str);

	ring.undo_edit();
	/*
	 * ED hooks with the default lexer framework
	 * will usually load the styling SciTECO script
	 * when editing the buffer for the first time.
	 */
	ring.edit(topic->filename);

	/*
	 * Make sure the topic is visible.
	 * We do need undo tokens for this (even though
	 * the buffer is removed on rubout if the woman
	 * page is viewed first) since we might browse
	 * multiple topics in the same buffer without
	 * closing it first.
	 */
	interface.undo_ssm(SCI_GOTOPOS,
	                   interface.ssm(SCI_GETCURRENTPOS));
	interface.ssm(SCI_GOTOPOS, topic->pos);

	return &States::start;
}

} /* namespace SciTECO */
