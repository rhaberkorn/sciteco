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

#include <signal.h>

#include <glib.h>

#include <gmodule.h>

#ifdef HAVE_WINDOWS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef HAVE_SYS_CAPSICUM_H
#include <sys/capsicum.h>
#endif

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "qreg.h"
#include "eol.h"
#include "ring.h"
#include "parser.h"
#include "memory.h"
#include "core-commands.h"
#include "qreg-commands.h"
#include "error.h"
#include "spawn.h"

/*
 * Glib v2.70 deprecates g_spawn_check_exit_status(),
 * renaming it to g_spawn_check_wait_status().
 * This leaves no way to work on both new and old versions without warnings.
 */
#if GLIB_CHECK_VERSION(2,70,0)
#define teco_spawn_check_wait_status g_spawn_check_wait_status
#else
#define teco_spawn_check_wait_status g_spawn_check_exit_status
#endif

static void teco_spawn_child_watch_cb(GPid pid, gint status, gpointer data);
static gboolean teco_spawn_stdin_watch_cb(GIOChannel *chan,
                                          GIOCondition condition, gpointer data);
static gboolean teco_spawn_stdout_watch_cb(GIOChannel *chan,
                                           GIOCondition condition, gpointer data);
static gboolean teco_spawn_idle_cb(gpointer user_data);

/*
 * FIXME: Global state should be part of teco_machine_main_t
 */
static struct {
	GMainContext *mainctx;
	GMainLoop *mainloop;
	GSource *idle_src;
	/** Process ID or Job Object handle on Windows */
	GPid pid;
	GSource *child_src;
	GSource *stdin_src, *stdout_src;
	gboolean interrupted;

	gssize from, to;
	gsize start;
	gboolean text_added;

	teco_eol_writer_t stdin_writer;
	teco_eol_reader_t stdout_reader;

	GError *error;
	teco_bool_t rc;

	teco_qreg_t *register_argument;
} teco_spawn_ctx;

static void __attribute__((constructor))
teco_spawn_init(void)
{
	memset(&teco_spawn_ctx, 0, sizeof(teco_spawn_ctx));
	/* FIXME: Cannot share these objects between calls */
#if 0
	/*
	 * Context and loop can be reused between EC invocations.
	 * However we should not use the default context, since it
	 * may be used by GTK.
	 */
	teco_spawn_ctx.mainctx = g_main_context_new();
	teco_spawn_ctx.mainloop = g_main_loop_new(teco_spawn_ctx.mainctx, FALSE);

	/*
	 * This is required on platforms that require polling (both Gtk and Curses).
	 */
	teco_spawn_ctx.idle_src = g_idle_source_new();
	g_source_set_priority(teco_spawn_ctx.idle_src, G_PRIORITY_LOW);
	g_source_set_callback(teco_spawn_ctx.idle_src, (GSourceFunc)teco_spawn_idle_cb,
	                      NULL, NULL);
	g_source_attach(teco_spawn_ctx.idle_src, teco_spawn_ctx.mainctx);
#endif
}

static gchar **
teco_parse_shell_command_line(const gchar *cmdline, GError **error)
{
	gchar **argv;

#ifdef G_OS_WIN32
	if (!(teco_ed & TECO_ED_SHELLEMU)) {
		teco_qreg_t *reg = teco_qreg_table_find(&teco_qreg_table_globals, "$COMSPEC", 8);
		g_assert(reg != NULL);
		teco_string_t comspec;
		if (!reg->vtable->get_string(reg, &comspec.data, &comspec.len, NULL, error))
			return NULL;
		if (teco_string_contains(&comspec, '\0')) {
			teco_string_clear(&comspec);
			teco_error_qregcontainsnull_set(error, "$COMSPEC", 8, FALSE);
			return NULL;
		}

		argv = g_new(gchar *, 5);
		argv[0] = comspec.data;
		argv[1] = g_strdup("/q");
		argv[2] = g_strdup("/c");
		argv[3] = g_strdup(cmdline);
		argv[4] = NULL;
		return argv;
	}
#elif defined(G_OS_UNIX)
	if (!(teco_ed & TECO_ED_SHELLEMU)) {
		teco_qreg_t *reg = teco_qreg_table_find(&teco_qreg_table_globals, "$SHELL", 6);
		g_assert(reg != NULL);
		teco_string_t shell;
		if (!reg->vtable->get_string(reg, &shell.data, &shell.len, NULL, error))
			return NULL;
		if (teco_string_contains(&shell, '\0')) {
			teco_string_clear(&shell);
			teco_error_qregcontainsnull_set(error, "$SHELL", 6, FALSE);
			return NULL;
		}

		argv = g_new(gchar *, 4);
		argv[0] = shell.data;
		argv[1] = g_strdup("-c");
		argv[2] = g_strdup(cmdline);
		argv[3] = NULL;
		return argv;
	}
#endif

	return g_shell_parse_argv(cmdline, NULL, &argv, error) ? argv : NULL;
}

static gboolean
teco_state_execute_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;

	/*
	 * Command-lines and file names are always assumed to be UTF-8,
	 * unless we set TECO_ED_DEFAULT_ANSI.
	 */
	teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
	                                         teco_default_codepage());

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;

	teco_bool_t rc = TECO_SUCCESS;

	/*
	 * By evaluating arguments here, the command may fail
	 * before the string argument is typed
	 */
	switch (teco_expressions_args()) {
	case 0:
		if (teco_num_sign > 0) {
			/* pipe nothing, insert at dot */
			teco_spawn_ctx.from = teco_spawn_ctx.to = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
			break;
		}
		/* fall through if prefix sign is "-" */

	case 1: {
		/* pipe and replace line range */
		teco_int_t line;

		teco_spawn_ctx.from = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		if (!teco_expressions_pop_num_calc(&line, 0, error))
			return FALSE;
		line += teco_interface_ssm(SCI_LINEFROMPOSITION, teco_spawn_ctx.from, 0);
		teco_spawn_ctx.to = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0);
		rc = teco_bool(teco_validate_line(line));

		if (teco_spawn_ctx.to < teco_spawn_ctx.from) {
			teco_int_t temp = teco_spawn_ctx.from;
			teco_spawn_ctx.from = teco_spawn_ctx.to;
			teco_spawn_ctx.to = temp;
		}

		break;
	}

	default: {
		/* pipe and replace character range */
		teco_int_t from, to;
		if (!teco_expressions_pop_num_calc(&to, 0, error) ||
		    !teco_expressions_pop_num_calc(&from, 0, error))
			return FALSE;
		teco_spawn_ctx.from = teco_interface_glyphs2bytes(from);
		teco_spawn_ctx.to = teco_interface_glyphs2bytes(to);
		rc = teco_bool(teco_spawn_ctx.from <= teco_spawn_ctx.to &&
		               teco_spawn_ctx.from >= 0 && teco_spawn_ctx.to >= 0);
	}
	}

	if (teco_is_failure(rc)) {
		if (!teco_machine_main_eval_colon(ctx)) {
			teco_error_range_set(error, "EC");
			return FALSE;
		}

		teco_expressions_push(rc);
		teco_spawn_ctx.from = teco_spawn_ctx.to = -1;
		/* done() will still be called */
	}

	return TRUE;
}

static teco_state_t *
teco_state_execute_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	/*
	 * NOTE: With G_SPAWN_LEAVE_DESCRIPTORS_OPEN and without G_SPAWN_SEARCH_PATH_FROM_ENVP,
	 * Glib offers an "optimized codepath" on UNIX.
	 * G_SPAWN_SEARCH_PATH_FROM_ENVP does not appear to work on Windows, anyway.
	 */
	static const GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH |
#ifdef G_OS_UNIX
	                                 G_SPAWN_LEAVE_DESCRIPTORS_OPEN |
#endif
	                                 G_SPAWN_STDERR_TO_DEV_NULL;

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (teco_spawn_ctx.from < 0)
		/*
		 * teco_state_execute_initial() failed without throwing
		 * error (colon-modified)
		 */
		return &teco_state_start;

	teco_spawn_ctx.text_added = FALSE;
	teco_spawn_ctx.rc = TECO_FAILURE;

	g_autoptr(GIOChannel) stdin_chan = NULL, stdout_chan = NULL;
	g_auto(GStrv) argv = NULL, envp = NULL;

#ifdef HAVE_CAP_GETMODE
	/*
	 * If we don't explicitly check for sandboxing, glib could assert
	 * internally and we want to detect all unexpected assertions
	 * in "infinite monkey"-style tests.
	 */
	u_int sandbox_mode;
	if (G_UNLIKELY(cap_getmode(&sandbox_mode) || sandbox_mode)) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Forbidden in Capsicum sandbox");
		goto gerror;
	}
#endif

	if (!str->len || teco_string_contains(str, '\0')) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Command line must not be empty or contain null-bytes");
		goto gerror;
	}

	argv = teco_parse_shell_command_line(str->data, error);
	if (!argv)
		goto gerror;

	envp = teco_qreg_table_get_environ(&teco_qreg_table_globals, error);
	if (!envp)
		goto gerror;

	gint stdin_fd, stdout_fd;

	GPid pid;
	if (!g_spawn_async_with_pipes(NULL, argv, envp, flags, NULL, NULL, &pid,
	                              &stdin_fd, &stdout_fd, NULL, error))
		goto gerror;

	/*
	 * FIXME: At least on Win32, we cannot resume a main loop
	 * after it has been quit once, which is obviously a bug.
	 * Therefore, we cannot cache the context, loop and idle_src.
	 */
	teco_spawn_ctx.mainctx = g_main_context_new();
	teco_spawn_ctx.mainloop = g_main_loop_new(teco_spawn_ctx.mainctx, FALSE);

	teco_spawn_ctx.idle_src = g_idle_source_new();
	g_source_set_priority(teco_spawn_ctx.idle_src, G_PRIORITY_LOW);
	g_source_set_callback(teco_spawn_ctx.idle_src, (GSourceFunc)teco_spawn_idle_cb,
	                      NULL, NULL);
	g_source_attach(teco_spawn_ctx.idle_src, teco_spawn_ctx.mainctx);

	teco_spawn_ctx.child_src = g_child_watch_source_new(pid);
	g_source_set_callback(teco_spawn_ctx.child_src, (GSourceFunc)teco_spawn_child_watch_cb,
	                      NULL, NULL);
	g_source_attach(teco_spawn_ctx.child_src, teco_spawn_ctx.mainctx);
	teco_spawn_ctx.interrupted = FALSE;

#ifdef G_OS_WIN32
	/*
	 * FIXME: In case of errors, we will leak memory.
	 */
	teco_spawn_ctx.pid = CreateJobObject(NULL, NULL);
	if (!teco_spawn_ctx.pid) {
		teco_error_win32_set(error, "Cannot create job object", GetLastError());
		goto gerror;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {
		.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	};
	if (!SetInformationJobObject(teco_spawn_ctx.pid, JobObjectExtendedLimitInformation,
	                             &job_info, sizeof(job_info))) {
		CloseHandle(teco_spawn_ctx.pid);
		teco_error_win32_set(error, "Cannot configure job object", GetLastError());
		goto gerror;
	}
	/*
	 * Assigning the process to a job object will allow us to
	 * kill the entire process tree relatively easily and without
	 * race conditions.
	 * There can however be a race condition while assigning the
	 * job object since the process could already be dead.
	 */
	DWORD exit_code;
	if (!AssignProcessToJobObject(teco_spawn_ctx.pid, pid) &&
	    (GetLastError() != ERROR_ACCESS_DENIED ||
	     !GetExitCodeProcess(teco_spawn_ctx.pid, &exit_code) ||
	     exit_code == STILL_ACTIVE)) {
		CloseHandle(teco_spawn_ctx.pid);
		teco_error_win32_set(error, "Cannot assign process to job object",
		                     GetLastError());
		goto gerror;
	}

	stdin_chan = g_io_channel_win32_new_fd(stdin_fd);
	stdout_chan = g_io_channel_win32_new_fd(stdout_fd);
#else
	teco_spawn_ctx.pid = pid;

	/* the UNIX constructors should work everywhere else */
	stdin_chan = g_io_channel_unix_new(stdin_fd);
	stdout_chan = g_io_channel_unix_new(stdout_fd);
#endif
	g_io_channel_set_flags(stdin_chan, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_encoding(stdin_chan, NULL, NULL);
	/*
	 * The EOL writer expects the channel to be buffered
	 * for performance reasons
	 */
	g_io_channel_set_buffered(stdin_chan, TRUE);
	g_io_channel_set_flags(stdout_chan, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_encoding(stdout_chan, NULL, NULL);
	g_io_channel_set_buffered(stdout_chan, FALSE);

	/*
	 * We always read from the current view,
	 * so we use its EOL mode.
	 */
	teco_eol_writer_init_gio(&teco_spawn_ctx.stdin_writer, teco_interface_ssm(SCI_GETEOLMODE, 0, 0), stdin_chan);
	teco_eol_reader_init_gio(&teco_spawn_ctx.stdout_reader, stdout_chan);

	teco_spawn_ctx.stdin_src = g_io_create_watch(stdin_chan,
	                                             G_IO_OUT | G_IO_ERR | G_IO_HUP);
	g_source_set_callback(teco_spawn_ctx.stdin_src, (GSourceFunc)teco_spawn_stdin_watch_cb,
	                      NULL, NULL);
	g_source_attach(teco_spawn_ctx.stdin_src, teco_spawn_ctx.mainctx);

	teco_spawn_ctx.stdout_src = g_io_create_watch(stdout_chan,
	                                              G_IO_IN | G_IO_ERR | G_IO_HUP);
	g_source_set_callback(teco_spawn_ctx.stdout_src, (GSourceFunc)teco_spawn_stdout_watch_cb,
	                      GINT_TO_POINTER(FALSE), NULL);
	g_source_attach(teco_spawn_ctx.stdout_src, teco_spawn_ctx.mainctx);

	if (!teco_spawn_ctx.register_argument) {
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_GOTOPOS,
			                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0), 0);
		teco_interface_ssm(SCI_GOTOPOS, teco_spawn_ctx.to, 0);
	}

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_spawn_ctx.start = teco_spawn_ctx.from;
	g_main_loop_run(teco_spawn_ctx.mainloop);
	if (!teco_spawn_ctx.register_argument) {
		teco_interface_ssm(SCI_DELETERANGE, teco_spawn_ctx.from,
		                   teco_spawn_ctx.to - teco_spawn_ctx.from);

		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(teco_spawn_ctx.from);
		teco_undo_int(teco_ranges[0].to) = teco_interface_bytes2glyphs(pos);
		teco_undo_guint(teco_ranges_count) = 1;
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);

	if (teco_spawn_ctx.register_argument) {
		if (teco_spawn_ctx.stdout_reader.eol_style >= 0) {
			teco_qreg_undo_set_eol_mode(teco_spawn_ctx.register_argument);
			teco_qreg_set_eol_mode(teco_spawn_ctx.register_argument,
			                       teco_spawn_ctx.stdout_reader.eol_style);
		}
	} else if (teco_spawn_ctx.from != teco_spawn_ctx.to || teco_spawn_ctx.text_added) {
		/* undo action has only been created if it changed anything */
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
		teco_ring_dirtify();
	}

	if (!g_source_is_destroyed(teco_spawn_ctx.stdin_src))
		g_io_channel_shutdown(stdin_chan, TRUE, NULL);
	teco_eol_reader_clear(&teco_spawn_ctx.stdout_reader);
	teco_eol_writer_clear(&teco_spawn_ctx.stdin_writer);
	g_source_unref(teco_spawn_ctx.stdin_src);
	g_io_channel_shutdown(stdout_chan, TRUE, NULL);
	g_source_unref(teco_spawn_ctx.stdout_src);

	g_source_unref(teco_spawn_ctx.child_src);
	g_spawn_close_pid(pid);
#ifdef G_OS_WIN32
	CloseHandle(teco_spawn_ctx.pid);
#endif

	g_source_unref(teco_spawn_ctx.idle_src);
	g_main_loop_unref(teco_spawn_ctx.mainloop);
	g_main_context_unref(teco_spawn_ctx.mainctx);

	/*
	 * NOTE: This includes interruptions following CTRL+C.
	 * But they are reported as G_SPAWN_ERROR_FAILED and hard to filter out.
	 */
	if (teco_spawn_ctx.error) {
		g_propagate_error(error, teco_spawn_ctx.error);
		teco_spawn_ctx.error = NULL;
		goto gerror;
	}

	if (teco_machine_main_eval_colon(ctx) > 0)
		teco_expressions_push(TECO_SUCCESS);

	goto cleanup;

gerror:
	/* `error` has been set */
	if (!teco_machine_main_eval_colon(ctx))
		return NULL;
	g_clear_error(error);

	/* May contain the exit status encoded as a teco_bool_t. */
	teco_expressions_push(teco_spawn_ctx.rc);
	/* fall through */

cleanup:
	teco_undo_ptr(teco_spawn_ctx.register_argument) = NULL;
	return &teco_state_start;
}

/* in cmdline.c */
gboolean teco_state_execute_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error);

/*$ "EC" :EC" pipe filter
 * ECcommand$ -- Execute operating system command and filter buffer contents
 * linesECcommand$
 * -ECcommand$
 * from,toECcommand$
 * :ECcommand$ -> Success|Failure
 * lines:ECcommand$ -> Success|Failure
 * -:ECcommand$ -> Success|Failure
 * from,to:ECcommand$ -> Success|Failure
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
 * On DOS-like systems like Windows, <command> is passed to
 * the command interpreter specified in the \fB$COMSPEC\fP
 * environment variable with the \(lq/q\(rq and \(lq/c\(rq
 * command-line arguments.
 * On UNIX-like systems, <command> is passed to the interpreter
 * specified by the \fB$SHELL\fP environment variable
 * with the \(lq-c\(rq command-line argument.
 * Therefore the default shell can be configured using
 * the corresponding environment registers.
 * The operating system restrictions on the maximum
 * length of command-line arguments apply to <command> and
 * quoting of parameters within <command> is somewhat platform
 * dependent.
 * On all other platforms, \*(ST will uniformly parse
 * <command> just as an UNIX98 \(lq/bin/sh\(rq would, but without
 * performing any expansions.
 * The program specified in <command> is searched for in
 * standard locations (according to the \fB$PATH\fP environment
 * variable).
 * This mode of operation can also be enforced on all platforms
 * by enabling bit 7 in the ED flag, e.g. by executing
 * \(lq0,128ED\(rq, and is recommended when writing cross-platform
 * macros using the EC command.
 *
 * When using an UNIX-compatible shell or the UNIX98 shell emulation,
 * you might want to use the \fB^E@\fP string-building character
 * to pass Q-Register contents reliably as single arguments to
 * the spawned process.
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
 * The first time, this will try to kill the spawned process
 * gracefully.
 * The second time you press CTRL+C, it will hard kill the process.
 *
 * In interactive mode, \*(ST performs TAB-completion
 * of filenames in the <command> string parameter but
 * does not attempt any escaping of shell-relevant
 * characters like whitespaces.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_execute,
	.initial_cb = (teco_state_initial_cb_t)teco_state_execute_initial,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_execute_process_edit_cmd
);

static teco_state_t *
teco_state_egcommand_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                  teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode <= TECO_MODE_NORMAL)
		teco_undo_ptr(teco_spawn_ctx.register_argument) = qreg;
	return &teco_state_execute;
}

/*$ "EG" "EGq" ":EGq"
 * EGq command$ -- Set Q-Register to output of operating system command
 * linesEGq command$
 * -EGq command$
 * from,toEGq command$
 * :EGq command$ -> Success|Failure
 * lines:EGq command$ -> Success|Failure
 * -:EGq command$ -> Success|Failure
 * from,to:EGq command$ -> Success|Failure
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
TECO_DEFINE_STATE_EXPECTQREG(teco_state_egcommand,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

/*
 * Glib callbacks
 */

static void
teco_spawn_child_watch_cb(GPid pid, gint status, gpointer data)
{
	if (teco_spawn_ctx.error)
		/* source has already been dispatched */
		return;

	/*
	 * There might still be data to read from stdout,
	 * but we cannot count on the stdout watcher to be ever called again.
	 */
#ifdef G_OS_WIN32
	/*
	 * FIXME: This is not done after interruptions since it will hang at least on Windows.
	 * This shouldn't happen at least if the IO channel is non-blocking.
	 * Moreover, reads may return 0 even if the socket is blocking.
	 */
	if (!teco_spawn_ctx.interrupted) {
		g_io_channel_set_flags(teco_spawn_ctx.stdout_reader.gio.channel, 0, NULL);
		teco_spawn_stdout_watch_cb(teco_spawn_ctx.stdout_reader.gio.channel,
		                           G_IO_IN, GINT_TO_POINTER(TRUE));
	}
#else /* !G_OS_WIN32 */
	/*
	 * On UNIX on the other hand, we apparently never receive G_IO_STATUS_EOF
	 * and MUST react to a read length of 0.
	 */
	teco_spawn_stdout_watch_cb(teco_spawn_ctx.stdout_reader.gio.channel,
	                           G_IO_IN, GINT_TO_POINTER(FALSE));
#endif

	/*
	 * teco_spawn_stdout_watch_cb() might have set the error.
	 */
	if (!teco_spawn_ctx.error && !teco_spawn_check_wait_status(status, &teco_spawn_ctx.error))
		teco_spawn_ctx.rc = teco_spawn_ctx.error->domain == G_SPAWN_EXIT_ERROR
					? ABS(teco_spawn_ctx.error->code) : TECO_FAILURE;

	g_main_loop_quit(teco_spawn_ctx.mainloop);
}

static gboolean
teco_spawn_stdin_watch_cb(GIOChannel *chan, GIOCondition condition, gpointer data)
{
	if (teco_spawn_ctx.error)
		/* source has already been dispatched */
		return G_SOURCE_REMOVE;

	if (!(condition & G_IO_OUT))
		/* stdin might be closed prematurely */
		goto remove;

	/* we always read from the current view */
	sptr_t gap = teco_interface_ssm(SCI_GETGAPPOSITION, 0, 0);
	gsize convert_len = teco_spawn_ctx.start < gap && gap < teco_spawn_ctx.to
				? gap - teco_spawn_ctx.start : teco_spawn_ctx.to - teco_spawn_ctx.start;
	const gchar *buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER,
	                                                        teco_spawn_ctx.start, convert_len);

	/*
	 * This cares about automatic EOL conversion and
	 * returns the number of consumed bytes.
	 * If it can only write a part of the EOL sequence (i.e. CR of CRLF)
	 * it may return a short byte count (possibly 0) which ensures that
	 * we do not yet remove the source.
	 */
	gssize bytes_written = teco_eol_writer_convert(&teco_spawn_ctx.stdin_writer, buffer,
	                                               convert_len, &teco_spawn_ctx.error);
	if (bytes_written < 0) {
		/* GError occurred */
		g_main_loop_quit(teco_spawn_ctx.mainloop);
		return G_SOURCE_REMOVE;
	}

	teco_spawn_ctx.start += bytes_written;

	if (teco_spawn_ctx.start == teco_spawn_ctx.to)
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
teco_spawn_stdout_watch_cb(GIOChannel *chan, GIOCondition condition, gpointer data)
{
	gboolean read_to_eof = GPOINTER_TO_INT(data);

	if (teco_spawn_ctx.error)
		/* source has already been dispatched */
		return G_SOURCE_REMOVE;

	teco_qreg_t *qreg = teco_spawn_ctx.register_argument;

	for (;;) {
		teco_string_t buffer;

		switch (teco_eol_reader_convert(&teco_spawn_ctx.stdout_reader,
		                                &buffer.data, &buffer.len, &teco_spawn_ctx.error)) {
		case G_IO_STATUS_ERROR:
			goto error;

		case G_IO_STATUS_EOF:
			/*
			 * NOTE: At least on Windows, the callback can still be invoked
			 * afterwards, probably because the source is already queued.
			 * This is not easy to prevent.
			 */
			return G_SOURCE_REMOVE;

		default:
			break;
		}

		if (!read_to_eof && !buffer.len)
			return G_SOURCE_CONTINUE;

		if (qreg) {
			if (teco_spawn_ctx.text_added) {
				if (!qreg->vtable->append_string(qreg, buffer.data, buffer.len,
				                                 &teco_spawn_ctx.error))
					goto error;
			} else {
				if (!qreg->vtable->undo_set_string(qreg, &teco_spawn_ctx.error) ||
				    !qreg->vtable->set_string(qreg, buffer.data, buffer.len,
				                              teco_default_codepage(), &teco_spawn_ctx.error))
					goto error;
			}
		} else {
			teco_interface_ssm(SCI_ADDTEXT, buffer.len, (sptr_t)buffer.data);
		}
		teco_spawn_ctx.text_added = TRUE;

		/*
		 * NOTE: Since this reads from an external process and regular memory
		 * limiting in teco_machine_main_step() is not performed, we could insert
		 * indefinitely (eg. cat /dev/zero).
		 * We could also check in teco_spawn_idle_cb(), but there is no guarantee we
		 * actually return to the main loop.
		 */
		if (!teco_memory_check(0, &teco_spawn_ctx.error))
			goto error;
	}

	g_assert_not_reached();

error:
	g_main_loop_quit(teco_spawn_ctx.mainloop);
	return G_SOURCE_REMOVE;
}

#ifdef G_OS_WIN32

static inline void
teco_spawn_terminate_hard(GPid job)
{
	TerminateJobObject(job, 1);
}

/*
 * FIXME: We could actually try to gracefully terminate the process first
 * using GenerateConsoleCtrlEvent(CTRL_C_EVENT).
 * However, it's hard to find the correct process group id.
 * We can pass 0, but this will fire our own control handler again,
 * resulting in a hard kill via TerminateProcess() anyway.
 * Masking the control handler is also not viable because it's a race
 * condition. All the workarounds would be very hacky.
 */
#define teco_spawn_terminate_soft teco_spawn_terminate_hard

#elif defined(G_OS_UNIX)

static inline void
teco_spawn_terminate_soft(GPid pid)
{
	kill(pid, SIGINT);
}

static inline void
teco_spawn_terminate_hard(GPid pid)
{
	kill(pid, SIGKILL);
}

#else /* !G_OS_WIN32 && !G_OS_UNIX */

static inline void
teco_spawn_terminate_soft(GPid pid)
{
	/* This may signal unrelated processes as well. */
	raise(SIGINT);
}

#define teco_spawn_terminate_hard teco_spawn_terminate_soft

#endif

static gboolean
teco_spawn_idle_cb(gpointer user_data)
{
	if (G_LIKELY(!teco_interface_is_interrupted()))
		return G_SOURCE_CONTINUE;
	teco_interrupted = FALSE;

	/*
	 * The first CTRL+C will try to gracefully terminate the process.
	 */
	if (!teco_spawn_ctx.interrupted)
		teco_spawn_terminate_soft(teco_spawn_ctx.pid);
	else
		teco_spawn_terminate_hard(teco_spawn_ctx.pid);
	teco_spawn_ctx.interrupted = TRUE;

	return G_SOURCE_CONTINUE;
}

static void TECO_DEBUG_CLEANUP
teco_spawn_cleanup(void)
{
	/* FIXME: Cannot share these objects between calls */
#if 0
	g_source_unref(teco_spawn_ctx.idle_src);

	g_main_loop_unref(teco_spawn_ctx.mainloop);
	g_main_context_unref(teco_spawn_ctx.mainctx);
#endif

	if (teco_spawn_ctx.error)
		g_error_free(teco_spawn_ctx.error);
}
