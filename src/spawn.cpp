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

#include <glib.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "qregisters.h"
#include "ring.h"
#include "parser.h"
#include "spawn.h"

namespace States {
	StateExecuteCommand	executecommand;
}

extern "C" {
static void child_watch_cb(GPid pid, gint status, gpointer data);
static gboolean stdin_watch_cb(GIOChannel *chan,
                               GIOCondition condition, gpointer data);
static gboolean stdout_watch_cb(GIOChannel *chan,
                                GIOCondition condition, gpointer data);
}

gchar **
parse_shell_command_line(const gchar *cmdline, GError **error)
{
	gchar **argv;

#ifdef G_OS_WIN32
	if (!(Flags::ed & Flags::ED_SHELLEMU)) {
		const gchar *argv_win32[] = {
			"cmd.exe", "/q", "/c", cmdline, NULL
		};
		return g_strdupv((gchar **)argv_win32);
	}
#elif defined(G_OS_UNIX)
	if (!(Flags::ed & Flags::ED_SHELLEMU)) {
		const gchar *argv_unix[] = {
			"/bin/sh", "-c", cmdline, NULL
		};
		return g_strdupv((gchar **)argv_unix);
	}
#endif

	if (!g_shell_parse_argv(cmdline, NULL, &argv, error))
		return NULL;

	return argv;
}

StateExecuteCommand::StateExecuteCommand() : StateExpectString()
{
	/*
	 * Context and loop can be reused between EC invocations.
	 * However we should not use the default context, since it
	 * may be used by GTK
	 */
	ctx.mainctx = g_main_context_new();
	ctx.mainloop = g_main_loop_new(ctx.mainctx, FALSE);
}

StateExecuteCommand::~StateExecuteCommand()
{
	g_main_loop_unref(ctx.mainloop);
	g_main_context_unref(ctx.mainctx);
}

void
StateExecuteCommand::initial(void)
{
	tecoBool rc = SUCCESS;

	expressions.eval();

	/*
	 * By evaluating arguments here, the command may fail
	 * before the string argument is typed
	 */
	switch (expressions.args()) {
	case 0:
		/* pipe nothing, insert at dot */
		ctx.from = ctx.to = interface.ssm(SCI_GETCURRENTPOS);
		break;

	case 1: {
		/* pipe and replace line range */
		sptr_t line;

		ctx.from = interface.ssm(SCI_GETCURRENTPOS);
		line = interface.ssm(SCI_LINEFROMPOSITION, ctx.from) +
		       expressions.pop_num_calc();
		ctx.to = interface.ssm(SCI_POSITIONFROMLINE, line);
		rc = TECO_BOOL(Validate::line(line));

		break;
	}

	default:
		/* pipe and replace character range */
		ctx.to = expressions.pop_num_calc();
		ctx.from = expressions.pop_num_calc();
		rc = TECO_BOOL(ctx.from <= ctx.to &&
		               Validate::pos(ctx.from) &&
		               Validate::pos(ctx.to));
		break;
	}

	if (IS_FAILURE(rc)) {
		if (eval_colon()) {
			expressions.push(rc);
			ctx.from = ctx.to = -1;
			/* done() will still be called */
		} else {
			throw RangeError("EC");
		}
	}
}

/*
 * FIXME: `xclip -selection clipboard -in` hangs -- the
 * stdout watcher is never activated!
 * Workaround is to pipe to /dev/null
 */
State *
StateExecuteCommand::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (ctx.from < 0)
		/*
		 * initial() failed without throwing
		 * error (colon-modified)
		 */
		return &States::start;

	gchar **argv;
	static const gint flags = G_SPAWN_DO_NOT_REAP_CHILD |
	                          G_SPAWN_SEARCH_PATH |
	                          G_SPAWN_STDERR_TO_DEV_NULL;

	GPid pid;
	gint stdin_fd, stdout_fd;
	GIOChannel *stdin_chan, *stdout_chan;

	ctx.text_added = false;
	ctx.error = NULL;

	argv = parse_shell_command_line(str, &ctx.error);
	if (!argv)
		goto gerror;

	g_spawn_async_with_pipes(NULL, argv, NULL, (GSpawnFlags)flags,
	                         NULL, NULL, &pid,
	                         &stdin_fd, &stdout_fd, NULL,
	                         &ctx.error);

	g_strfreev(argv);

	if (ctx.error)
		goto gerror;

	ctx.child_src = g_child_watch_source_new(pid);
	g_source_set_callback(ctx.child_src, (GSourceFunc)child_watch_cb,
	                      &ctx, NULL);
	g_source_attach(ctx.child_src, ctx.mainctx);

#ifdef G_OS_WIN32
	stdin_chan = g_io_channel_win32_new_fd(stdin_fd);
	stdout_chan = g_io_channel_win32_new_fd(stdout_fd);
#else /* the UNIX constructors should work everywhere else */
	stdin_chan = g_io_channel_unix_new(stdin_fd);
	stdout_chan = g_io_channel_unix_new(stdout_fd);
#endif
	g_io_channel_set_flags(stdin_chan, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_encoding(stdin_chan, NULL, NULL);
	g_io_channel_set_buffered(stdin_chan, FALSE);
	g_io_channel_set_flags(stdout_chan, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_encoding(stdout_chan, NULL, NULL);
	g_io_channel_set_buffered(stdout_chan, FALSE);

	ctx.stdin_src = g_io_create_watch(stdin_chan,
	                                  (GIOCondition)(G_IO_OUT | G_IO_ERR | G_IO_HUP));
	g_source_set_callback(ctx.stdin_src, (GSourceFunc)stdin_watch_cb,
	                      &ctx, NULL);
	g_source_attach(ctx.stdin_src, ctx.mainctx);

	ctx.stdout_src = g_io_create_watch(stdout_chan,
	                                   (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP));
	g_source_set_callback(ctx.stdout_src, (GSourceFunc)stdout_watch_cb,
	                      &ctx, NULL);
	g_source_attach(ctx.stdout_src, ctx.mainctx);

	if (current_doc_must_undo())
		undo.push_msg(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));
	interface.ssm(SCI_GOTOPOS, ctx.to);

	interface.ssm(SCI_BEGINUNDOACTION);
	ctx.start = ctx.from;
	g_main_loop_run(ctx.mainloop);
	interface.ssm(SCI_DELETERANGE, ctx.from, ctx.to - ctx.from);
	interface.ssm(SCI_ENDUNDOACTION);

	if (ctx.start != ctx.from || ctx.text_added) {
		/* undo action is only effective if it changed anything */
		if (current_doc_must_undo())
			undo.push_msg(SCI_UNDO);
		interface.ssm(SCI_SCROLLCARET);
		ring.dirtify();
	}

	if (!g_source_is_destroyed(ctx.stdin_src))
		g_io_channel_shutdown(stdin_chan, TRUE, NULL);
	g_io_channel_unref(stdin_chan);
	g_source_unref(ctx.stdin_src);
	g_io_channel_shutdown(stdout_chan, TRUE, NULL);
	g_io_channel_unref(stdout_chan);
	g_source_unref(ctx.stdout_src);

	g_source_unref(ctx.child_src);
	g_spawn_close_pid(pid);

	if (ctx.error)
		goto gerror;

	if (interface.is_interrupted())
		throw State::Error("Interrupted");

	if (eval_colon())
		expressions.push(SUCCESS);

	return &States::start;

gerror:
	if (!eval_colon())
		throw GError(ctx.error);

	/*
	 * If possible, encode process exit code
	 * in return boolean. It's guaranteed to be
	 * a failure since it's non-negative.
	 */
	if (ctx.error->domain == G_SPAWN_EXIT_ERROR)
		expressions.push(ABS(ctx.error->code));
	else
		expressions.push(FAILURE);
	return &States::start;
}

/*
 * Glib callbacks
 */

static void
child_watch_cb(GPid pid, gint status, gpointer data)
{
	StateExecuteCommand::Context &ctx =
			*(StateExecuteCommand::Context *)data;

	/*
	 * Writing stdin or reading stdout might have already
	 * failed. We preserve the earliest GError.
	 */
	if (!ctx.error)
		g_spawn_check_exit_status(status, &ctx.error);

	if (g_source_is_destroyed(ctx.stdout_src))
		g_main_loop_quit(ctx.mainloop);
}

static gboolean
stdin_watch_cb(GIOChannel *chan, GIOCondition condition, gpointer data)
{
	StateExecuteCommand::Context &ctx =
			*(StateExecuteCommand::Context *)data;

	const gchar *buffer;
	gsize bytes_written;

	buffer = (const gchar *)interface.ssm(SCI_GETRANGEPOINTER,
	                                      ctx.from,
	                                      ctx.to - ctx.start);

	switch (g_io_channel_write_chars(chan, buffer, ctx.to - ctx.start,
	                                 &bytes_written,
	                                 ctx.error ? NULL : &ctx.error)) {
	case G_IO_STATUS_ERROR:
		/* do not yet quit -- we still have to reap the child */
		goto remove;
	case G_IO_STATUS_NORMAL:
		break;
	case G_IO_STATUS_EOF:
		/* process closed stdin preliminarily? */
		goto remove;
	case G_IO_STATUS_AGAIN:
		return G_SOURCE_CONTINUE;
	}

	ctx.start += bytes_written;

	if (ctx.start == ctx.to)
		/* this will signal EOF to the process */
		goto remove;

	return G_SOURCE_CONTINUE;

remove:
	/*
	 * Channel is always shut down here (fd is closed),
	 * so it's always shut down IF the GSource has been
	 * destroyed. It is not guaranteed to be destroyed
	 * during the main loop run however since it quits
	 * as soon as the child was reaped and stdout was read.
	 */
	g_io_channel_shutdown(chan, TRUE, NULL);
	return G_SOURCE_REMOVE;
}

static gboolean
stdout_watch_cb(GIOChannel *chan, GIOCondition condition, gpointer data)
{
	StateExecuteCommand::Context &ctx =
			*(StateExecuteCommand::Context *)data;

	gchar buffer[1024];
	gsize bytes_read;

	switch (g_io_channel_read_chars(chan, buffer, sizeof(buffer),
	                                &bytes_read,
	                                ctx.error ? NULL : &ctx.error)) {
	case G_IO_STATUS_NORMAL:
		break;
	case G_IO_STATUS_ERROR:
	case G_IO_STATUS_EOF:
		if (g_source_is_destroyed(ctx.child_src))
			g_main_loop_quit(ctx.mainloop);
		return G_SOURCE_REMOVE;
	case G_IO_STATUS_AGAIN:
		return G_SOURCE_CONTINUE;
	}

	interface.ssm(SCI_ADDTEXT, bytes_read, (sptr_t)buffer);
	ctx.text_added = true;

	return G_SOURCE_CONTINUE;
}
