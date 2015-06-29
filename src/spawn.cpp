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

#include <glib.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "qregisters.h"
#include "ioview.h"
#include "ring.h"
#include "parser.h"
#include "error.h"
#include "spawn.h"

/*
 * Debian 7 is still at libglib v2.33, so
 * for the time being we support this UNIX-only
 * implementation of g_spawn_check_exit_status()
 * partially emulating libglib v2.34
 */
#ifndef G_SPAWN_EXIT_ERROR
#ifdef G_OS_UNIX
#warning "libglib v2.34 or later recommended."
#else
#error "libglib v2.34 or later required."
#endif

#include <sys/types.h>
#include <sys/wait.h>

#define G_SPAWN_EXIT_ERROR \
	g_quark_from_static_string("g-spawn-exit-error-quark")

static gboolean
g_spawn_check_exit_status(gint exit_status, GError **error)
{
	if (!WIFEXITED(exit_status)) {
		g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
		            "Abnormal process termination (%d)",
		            exit_status);
		return FALSE;
	}

	if (WEXITSTATUS(exit_status) != 0) {
		g_set_error(error, G_SPAWN_EXIT_ERROR, WEXITSTATUS(exit_status),
		            "Unsuccessful exit status %d",
		            WEXITSTATUS(exit_status));
		return FALSE;
	}

	return TRUE;
}

#endif

namespace SciTECO {

namespace States {
	StateExecuteCommand	executecommand;
	StateEGCommand		egcommand;
}

extern "C" {
static void child_watch_cb(GPid pid, gint status, gpointer data);
static gboolean stdin_watch_cb(GIOChannel *chan,
                               GIOCondition condition, gpointer data);
static gboolean stdout_watch_cb(GIOChannel *chan,
                                GIOCondition condition, gpointer data);
}

static QRegister *register_argument = NULL;

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

/*$
 * EC[command]$ -- Execute operating system command and filter buffer contents
 * linesEC[command]$
 * -EC[command]$
 * from,toEC[command]$
 * :EC[command]$ -> Success|Failure
 * lines:EC[command]$ -> Success|Failure
 * -:EC[command]$ -> Success|Failure
 * from,to:EC[command]$ -> Success|Failure
 *
 * The EC command allows you to interface with the operating
 * system shell and external programs.
 * The external program is spawned as a background process
 * and its standard input stream is fed with data from the
 * current document, i.e. text is piped into the external
 * program.
 * When automatic EOL translation is enabled, this will
 * translate all end of line sequences according to the
 * source document's EOL mode (see \fBEL\fP command).
 * For instance when piping from a document with DOS
 * line breaks, the receiving program will only be sent
 * DOS line breaks.
 * The process' standard output stream is also redirected
 * and inserted into the current document.
 * End of line sequences are normalized accordingly
 * but the EOL mode guessed from the program's output is
 * \fBnot\fP set on the document.
 * The process' standard error stream is discarded.
 * If data is piped into the external program, its output
 * replaces that data in the buffer.
 * Dot is always left at the end of the insertion.
 *
 * If invoked without parameters, no data is piped into
 * the process (and no characters are removed) and its
 * output is inserted at the current buffer position.
 * This is equivalent to invoking \(lq.,.EC\(rq.
 * If invoked with one parameter, the next or previous number
 * of <lines> are piped from the buffer into the program and
 * its output replaces these <lines>.
 * This effectively runs <command> as a filter over <lines>.
 * \(lq-EC\(rq may be written as a short-cut for \(lq-1EC\(rq.
 * When invoked with two parameters, the characters beginning
 * at position <from> up to the character at position <to>
 * are piped into the program and replaced with its output.
 * This effectively runs <command> as a filter over a buffer
 * range.
 *
 * Errors are thrown not only for invalid buffer ranges
 * but also for errors during process execution.
 * If the external <command> has an unsuccessful exit code,
 * the EC command will also fail.
 * If the EC command is colon-modified, it will instead return
 * a TECO boolean signifying success or failure.
 * In case of an unsuccessful exit code, a colon-modified EC
 * will return the absolute value of the process exit
 * code (which is also a TECO failure boolean) and 0 for all
 * other failures.
 * This feature may be used to take action depending on a
 * specific process exit code.
 *
 * <command> execution is by default platform-dependent.
 * On Windows, <command> is passed to the default command
 * interpreter \(lqcmd.exe\(rq with the \(lq/c\(rq
 * command-line argument.
 * On Unix/Linux, <command> is passed to the \(lq/bin/sh\(rq
 * shell with the \(lq-c\(rq command-line argument.
 * Therefore operating system restrictions on the maximum
 * length of command-line arguments apply to <command> and
 * quoting of parameters within <command> is somewhat platform
 * dependent.
 * On all other platforms, \*(ST will uniformly parse
 * <command> just as an UNIX98 \(lq/bin/sh\(rq would, but without
 * performing any expansions.
 * The program specified in <command> is searched for in
 * standard locations (according to the \(lqPATH\(rq environment
 * variable).
 * This mode of operation can also be enforced on Windows and
 * Unix by enabling bit 7 in the ED flag, e.g. by executing
 * \(lq0,128ED\(rq, and is recommended when writing cross-platform
 * macros using the EC command.
 *
 * The spawned process inherits both \*(ST's current working
 * directory and its environment variables.
 * More precisely, \*(ST uses its environment registers
 * to construct the spawned process' environment.
 * Therefore it is also straight forward to change the working
 * directory or some environment variable temporarily
 * for a spawned process.
 *
 * Note that when run interactively and subsequently rubbed
 * out, \*(ST can easily undo all changes to the editor
 * state.
 * It \fBcannot\fP however undo any other side-effects that the
 * execution of <command> might have had on your system.
 *
 * Note also that the EC command blocks indefinitely until
 * the <command> completes, which may result in editor hangs.
 * You may however interrupt the spawned process by sending
 * the \fBSIGINT\fP signal to \*(ST, e.g. by pressing CTRL+C.
 *
 * In interactive mode, \*(ST performs TAB-completion
 * of filenames in the <command> string parameter but
 * does not attempt any escaping of shell-relevant
 * characters like whitespaces.
 */
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
		if (expressions.num_sign > 0) {
			/* pipe nothing, insert at dot */
			ctx.from = ctx.to = interface.ssm(SCI_GETCURRENTPOS);
			break;
		}
		/* fall through if prefix sign is "-" */

	case 1: {
		/* pipe and replace line range */
		sptr_t line;

		ctx.from = interface.ssm(SCI_GETCURRENTPOS);
		line = interface.ssm(SCI_LINEFROMPOSITION, ctx.from) +
		       expressions.pop_num_calc();
		ctx.to = interface.ssm(SCI_POSITIONFROMLINE, line);
		rc = TECO_BOOL(Validate::line(line));

		if (ctx.to < ctx.from) {
			tecoInt temp = ctx.from;
			ctx.from = ctx.to;
			ctx.to = temp;
		}

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

	gchar **argv, **envp;
	static const gint flags = G_SPAWN_DO_NOT_REAP_CHILD |
	                          G_SPAWN_SEARCH_PATH |
	                          G_SPAWN_STDERR_TO_DEV_NULL;

	GPid pid;
	gint stdin_fd, stdout_fd;
	GIOChannel *stdin_chan, *stdout_chan;

	ctx.text_added = false;
	/* opaque state for IOView::save() */
	ctx.stdin_state = 0;
	/* opaque state for IOView::channel_read_with_eol() */
	ctx.stdout_state = 0;
	/* eol style guessed from the stdout stream */
	ctx.eol_style = -1;
	ctx.error = NULL;

	argv = parse_shell_command_line(str, &ctx.error);
	if (!argv)
		goto gerror;

	envp = QRegisters::globals.get_environ();

	g_spawn_async_with_pipes(NULL, argv, envp, (GSpawnFlags)flags,
	                         NULL, NULL, &pid,
	                         &stdin_fd, &stdout_fd, NULL,
	                         &ctx.error);

	g_strfreev(envp);
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
	/*
	 * IOView::save() expects the channel to be buffered
	 * for performance reasons
	 */
	g_io_channel_set_buffered(stdout_chan, TRUE);

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

	if (!register_argument) {
		if (current_doc_must_undo())
			interface.undo_ssm(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));
		interface.ssm(SCI_GOTOPOS, ctx.to);
	}

	interface.ssm(SCI_BEGINUNDOACTION);
	ctx.start = ctx.from;
	g_main_loop_run(ctx.mainloop);
	if (!register_argument)
		interface.ssm(SCI_DELETERANGE, ctx.from, ctx.to - ctx.from);
	interface.ssm(SCI_ENDUNDOACTION);

	if (register_argument) {
		if (ctx.eol_style >= 0) {
			register_argument->undo_set_eol_mode();
			register_argument->set_eol_mode(ctx.eol_style);
		}
	} else if (ctx.from != ctx.to || ctx.text_added) {
		/* undo action is only effective if it changed anything */
		if (current_doc_must_undo())
			interface.undo_ssm(SCI_UNDO);
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
		throw Error("Interrupted");

	if (eval_colon())
		expressions.push(SUCCESS);

	undo.push_var(register_argument) = NULL;
	return &States::start;

gerror:
	if (!eval_colon())
		throw GlibError(ctx.error);

	/*
	 * If possible, encode process exit code
	 * in return boolean. It's guaranteed to be
	 * a failure since it's non-negative.
	 */
	if (ctx.error->domain == G_SPAWN_EXIT_ERROR)
		expressions.push(ABS(ctx.error->code));
	else
		expressions.push(FAILURE);
	undo.push_var(register_argument) = NULL;

	g_error_free(ctx.error);
	return &States::start;
}

/*$
 * EGq[command]$ -- Set Q-Register to output of operating system command
 * linesEGq[command]$
 * -EGq[command]$
 * from,toEGq[command]$
 * :EGq[command]$ -> Success|Failure
 * lines:EGq[command]$ -> Success|Failure
 * -:EGq[command]$ -> Success|Failure
 * from,to:EGq[command]$ -> Success|Failure
 *
 * Runs an operating system <command> and set Q-Register
 * <q> to the data read from its standard output stream.
 * Data may be fed to <command> from the current buffer/document.
 * The interpretation of the parameters and <command> as well
 * as the colon-modification is analoguous to the EC command.
 *
 * The EG command only differs from EC in not deleting any
 * characters from the current buffer, not changing
 * the current buffer position and writing process output
 * to the Q-Register <q>.
 * In other words, the current buffer is not modified by EG.
 * Also since EG replaces the string value of <q>, the register's
 * EOL mode is set to the mode guessed from the external program's
 * output.
 *
 * The register <q> is defined if it does not already exist.
 */
State *
StateEGCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::executecommand);
	undo.push_var(register_argument) = reg;
	return &States::executecommand;
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

	/* we always read from the current view */
	IOView *view = (IOView *)interface.get_current_view();

	gsize bytes_written;

	/*
	 * IOView::save() cares about automatic EOL conversion
	 */
	switch (view->save(chan, ctx.from, ctx.to - ctx.start,
	                   &bytes_written, ctx.stdin_state,
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

	GIOStatus status;

	gchar buffer[1024];
	gsize read_len = 0;
	guint offset = 0;
	gsize block_len = 0;
	/* we're not really interested in that: */
	gboolean eol_style_inconsistent = FALSE;

	for (;;) {
		status = IOView::channel_read_with_eol(
			chan, buffer, sizeof(buffer),
			read_len, offset, block_len,
			ctx.stdout_state, ctx.eol_style,
			eol_style_inconsistent,
			ctx.error ? NULL : &ctx.error
		);

		switch (status) {
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

		if (!block_len)
			continue;

		if (register_argument) {
			if (ctx.text_added) {
				register_argument->undo_append_string();
				register_argument->append_string(buffer+offset, block_len);
			} else {
				register_argument->undo_set_string();
				register_argument->set_string(buffer+offset, block_len);
			}
		} else {
			interface.ssm(SCI_ADDTEXT, block_len, (sptr_t)(buffer+offset));
		}
		ctx.text_added = true;
	}

	/* not reached */
	return G_SOURCE_CONTINUE;
}

} /* namespace SciTECO */
