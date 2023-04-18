/*
 * Copyright (C) 2012-2023 Robin Haberkorn
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
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "string-utils.h"
#include "error.h"
#include "parser.h"
#include "core-commands.h"
#include "qreg.h"
#include "ring.h"
#include "interface.h"
#include "rb3str.h"
#include "help.h"

static void teco_help_set(const gchar *topic_name, const gchar *filename, teco_int_t pos);

static GStringChunk *teco_help_chunk = NULL;

/** @extends teco_rb3str_head_t */
typedef struct {
	teco_rb3str_head_t head;

	teco_int_t pos;
	gchar filename[];
} teco_help_topic_t;

/** @static @memberof teco_help_topic_t */
static teco_help_topic_t *
teco_help_topic_new(const gchar *topic_name, const gchar *filename, teco_int_t pos)
{
	/*
	 * Topics are inserted only once into the RB tree, so we can store
	 * the strings in a GStringChunk.
	 *
	 * FIXME: The same should be true for teco_help_topic_t object itself.
	 * It could be allocated via a stack allocator.
	 */
	teco_help_topic_t *topic = g_malloc0(sizeof(teco_help_topic_t) + strlen(filename) + 1);
	teco_string_init_chunk(&topic->head.name, topic_name, strlen(topic_name), teco_help_chunk);
	topic->pos = pos;
	strcpy(topic->filename, filename);
	return topic;
}

/** @memberof teco_help_topic_t */
static inline void
teco_help_topic_free(teco_help_topic_t *ctx)
{
	/*
	 * NOTE: The topic name is allocated via GStringChunk and can only be
	 * be deallocated together.
	 */
	g_free(ctx);
}

static teco_rb3str_tree_t teco_help_tree;

static gboolean
teco_help_init(GError **error)
{
	if (G_LIKELY(teco_help_chunk != NULL))
		/* already loaded */
		return TRUE;

	teco_help_chunk = g_string_chunk_new(32);
	rb3_reset_tree(&teco_help_tree);

	teco_qreg_t *lib_reg = teco_qreg_table_find(&teco_qreg_table_globals, "$SCITECOPATH", 12);
	g_assert(lib_reg != NULL);
	g_auto(teco_string_t) lib_path = {NULL, 0};
	if (!lib_reg->vtable->get_string(lib_reg, &lib_path.data, &lib_path.len, error))
		return FALSE;
	/*
	 * FIXME: lib_path may contain null-bytes.
	 * It's not clear how to deal with this.
	 */
	g_autofree gchar *women_path = g_build_filename(lib_path.data, "women", NULL);

	/*
	 * FIXME: We might want to gracefully handle only the G_FILE_ERROR_NOENT
	 * error and propagate all other errors?
	 */
	g_autoptr(GDir) women_dir = g_dir_open(women_path, 0, NULL);
	if (!women_dir)
		return TRUE;

	const gchar *basename;
	while ((basename = g_dir_read_name(women_dir))) {
		if (!g_str_has_suffix(basename, ".woman"))
			continue;

		/*
		 * Open the corresponding SciTECO macro to read
		 * its first line.
		 */
		g_autofree gchar *filename = g_build_filename(women_path, basename, NULL);
		g_autofree gchar *filename_tec = g_strconcat(filename, ".tec", NULL);
		g_autoptr(FILE) file = g_fopen(filename_tec, "r");
		if (!file) {
			/*
			 * There might simply be no support script for
			 * simple plain-text woman-pages.
			 * In this case we create a topic using the filename
			 * without an extension.
			 */
			g_autofree gchar *topic = g_strndup(basename, strlen(basename)-6);
			teco_help_set(topic, filename, 0);
			continue;
		}

		/*
		 * Each womanpage script begins with a special comment
		 * header containing the position to topic index.
		 * Every topic will be on its own line and they are unlikely
		 * to be very long, so we can use fgets() here.
		 *
		 * NOTE: Since we haven't opened with the "b" flag,
		 * fgets() will translate linebreaks to LF even on
		 * MSVCRT (Windows).
		 */
		gchar buffer[1024];
		if (!fgets(buffer, sizeof(buffer), file) ||
		    !g_str_has_prefix(buffer, "!*")) {
			teco_interface_msg(TECO_MSG_WARNING,
			                   "Missing or invalid topic line in womanpage script \"%s\"",
			                   filename);
			continue;
		}
		/* skip opening comment */
		gchar *topic = buffer+2;

		do {
			gchar *endptr;
			teco_int_t pos = strtoul(topic, &endptr, 10);

			/*
			 * This also breaks at the last line of the
			 * header.
			 */
			if (*endptr != ':')
				break;

			/*
			 * Strip the likely LF at the end of the line.
			 */
			gsize len = strlen(endptr)-1;
			if (G_LIKELY(endptr[len] == '\n'))
				endptr[len] = '\0';

			teco_help_set(endptr+1, filename, pos);
		} while ((topic = fgets(buffer, sizeof(buffer), file)));
	}

	return TRUE;
}

static inline teco_help_topic_t *
teco_help_find(const gchar *topic_name)
{
	/*
	 * The topic index contains printable characters
	 * only (to avoid having to perform string building
	 * on the topic terms to be able to define control
	 * characters).
	 * Therefore, we expand control characters in the
	 * look-up string to their printable forms.
	 */
	g_autofree gchar *term = teco_string_echo(topic_name, strlen(topic_name));
	return (teco_help_topic_t *)teco_rb3str_find(&teco_help_tree, FALSE, term, strlen(term));
}

static void
teco_help_set(const gchar *topic_name, const gchar *filename, teco_int_t pos)
{
	teco_help_topic_t *topic;
	teco_help_topic_t *existing = teco_help_find(topic_name);
	if (existing) {
		if (!strcmp(existing->filename, filename)) {
			/*
			 * A topic with the same name already exists
			 * in the same file.
			 * For the time being, we simply overwrite the
			 * last topic.
			 * FIXME: Perhaps make it unique again!?
			 */
			existing->pos = pos;
			return;
		}

		/* in another file -> make name unique */
		teco_interface_msg(TECO_MSG_WARNING,
		                   "Topic collision: \"%s\" defined in \"%s\" and \"%s\"",
		                   topic_name, existing->filename, filename);

		g_autofree gchar *basename = g_path_get_basename(filename);
		g_autofree gchar *unique_name = g_strconcat(topic_name, ":", basename, NULL);
		topic = teco_help_topic_new(unique_name, filename, pos);
	} else {
		topic = teco_help_topic_new(topic_name, filename, pos);
	}

	teco_rb3str_insert(&teco_help_tree, FALSE, &topic->head);
}

gboolean
teco_help_auto_complete(const gchar *topic_name, teco_string_t *insert)
{
	return teco_rb3str_auto_complete(&teco_help_tree, FALSE, topic_name,
	                                 topic_name ? strlen(topic_name) : 0, 0, insert);
}

#ifndef NDEBUG
static void __attribute__((destructor))
teco_help_cleanup(void)
{
	if (!teco_help_chunk)
		/* not initialized */
		return;
	g_string_chunk_free(teco_help_chunk);

	struct rb3_head *cur;

	while ((cur = rb3_get_root(&teco_help_tree))) {
		rb3_unlink_and_rebalance(cur);
		teco_help_topic_free((teco_help_topic_t *)cur);
	}
}
#endif

/*
 * Command states
 */

static gboolean
teco_state_help_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	/*
	 * The help-index is populated on demand,
	 * so we start up quicker and batch mode does
	 * not depend on the availability of the standard
	 * library.
	 */
	return teco_help_init(error);
}

static teco_state_t *
teco_state_help_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (teco_string_contains(str, '\0')) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Help topic must not contain null-byte");
		return NULL;
	}
	const gchar *topic_name = str->data ? : "";
	teco_help_topic_t *topic = teco_help_find(topic_name);
	if (!topic) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Topic \"%s\" not found", topic_name);
		return NULL;
	}

	teco_ring_undo_edit();
	/*
	 * ED hooks with the default lexer framework
	 * will usually load the styling SciTECO script
	 * when editing the buffer for the first time.
	 */
	if (!teco_ring_edit(topic->filename, error))
		return NULL;

	/*
	 * Make sure the topic is visible.
	 * We do need undo tokens for this (even though
	 * the buffer is removed on rubout if the woman
	 * page is viewed first) since we might browse
	 * multiple topics in the same buffer without
	 * closing it first.
	 */
	undo__teco_interface_ssm(SCI_GOTOPOS,
	                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0), 0);
	teco_interface_ssm(SCI_GOTOPOS, topic->pos, 0);

	return &teco_state_start;
}

/* in cmdline.c */
gboolean teco_state_help_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar chr, GError **error);

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
 * help topics can be defined using the \fBTECO_TOPIC\fP
 * Troff macro.
 * This flexible system allows \*(ST to access internal
 * and third-party help files written in plain-text or
 * with an arbitrary GNU troff macro package.
 * As all GNU troff documents are processed at build-time,
 * GNU troff is not required at runtime.
 *
 * The \fB?\fP command does not have string building enabled.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_help,
	.initial_cb = (teco_state_initial_cb_t)teco_state_help_initial,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_help_process_edit_cmd,
	.expectstring.string_building = FALSE
);
