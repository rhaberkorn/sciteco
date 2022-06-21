/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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
#pragma once

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "string-utils.h"
#include "goto.h"
#include "qreg.h"

/*
 * Forward Declarations
 */
typedef const struct teco_state_t teco_state_t;
typedef struct teco_machine_t teco_machine_t;
typedef struct teco_machine_main_t teco_machine_main_t;

typedef struct {
	/** how many iterations are left */
	teco_int_t counter;
	/** Program counter of loop start command */
	guint pc : sizeof(guint)*8 - 1;
	/**
	 * Whether the loop represents an argument
	 * barrier or not (it "passes through"
	 * stack arguments).
	 *
	 * Since the program counter is usually
	 * a signed integer, it's ok steal one
	 * bit for the pass_through flag.
	 */
	gboolean pass_through : 1;
} teco_loop_context_t;

extern GArray *teco_loop_stack;

void undo__insert_val__teco_loop_stack(guint, teco_loop_context_t);
void undo__remove_index__teco_loop_stack(guint);

/**
 * @defgroup states Parser states
 *
 * Parser states are defined as global constants using the TECO_DEFINE_STATE()
 * macro, allowing individual fields and callbacks to be overwritten.
 * Derived macros are defined to factor out common fields and settings.
 * States therefore form a hierarchy, which is documented using
 * \@interface and \@implements tags.
 *
 * @{
 */

/*
 * FIXME: Remove _cb from all callback names. See qreg.h.
 * FIXME: Maybe use TECO_DECLARE_VTABLE_METHOD()?
 */
typedef const struct {
	gboolean string_building : 1;
	gboolean last : 1;

	/**
	 * Called repeatedly to process chunks of input and give interactive feedback.
	 *
	 * Can be NULL if no interactive feedback is required.
	 */
	gboolean (*process_cb)(teco_machine_main_t *ctx, const teco_string_t *str,
	                       gsize new_chars, GError **error);

	/**
	 * Called at the end of the string argument to determine the next state.
	 * Commands that don't give interactive feedback can use this callback
	 * to perform their main processing.
	 */
	teco_state_t *(*done_cb)(teco_machine_main_t *ctx, const teco_string_t *str, GError **error);
} teco_state_expectstring_t;

typedef const struct {
	teco_qreg_type_t type;

	/** Called when a register specification has been successfully parsed. */
	teco_state_t *(*got_register_cb)(teco_machine_main_t *ctx, teco_qreg_t *qreg,
	                                 teco_qreg_table_t *table, GError **error);
} teco_state_expectqreg_t;

typedef gboolean (*teco_state_initial_cb_t)(teco_machine_t *ctx, GError **error);
typedef teco_state_t *(*teco_state_input_cb_t)(teco_machine_t *ctx, gchar chr, GError **error);
typedef gboolean (*teco_state_refresh_cb_t)(teco_machine_t *ctx, GError **error);
typedef gboolean (*teco_state_end_of_macro_cb_t)(teco_machine_t *ctx, GError **error);
typedef gboolean (*teco_state_process_edit_cmd_cb_t)(teco_machine_t *ctx, teco_machine_t *parent_ctx,
                                                     gchar key, GError **error);

typedef enum {
	TECO_FNMACRO_MASK_START		= (1 << 0),
	TECO_FNMACRO_MASK_STRING	= (1 << 1),
	TECO_FNMACRO_MASK_DEFAULT	= ~((1 << 2)-1)
} teco_fnmacro_mask_t;

/**
 * A teco_machine_t state.
 * These are declared as constants using TECO_DEFINE_STATE() and friends.
 *
 * @note Unless you don't want to manually "upcast" the teco_machine_t* in
 * callback implementations, you will have to cast your callback types when initializing
 * the teco_state_t vtables.
 * Casting to functions of different signature is theoretically undefined behavior,
 * but works on all major platforms including Emscripten, as long as they differ only
 * in pointer types.
 */
struct teco_state_t {
	/**
	 * Called the first time this state is entered.
	 * Theoretically, you can use teco_machine_main_transition_t instead,
	 * but this callback improves reusability.
	 *
	 * It can be NULL if not required.
	 */
	teco_state_initial_cb_t initial_cb;

	/**
	 * Get next state given an input character.
	 *
	 * This is a mandatory field.
	 */
	teco_state_input_cb_t input_cb;

	/**
	 * Provide interactive feedback.
	 *
	 * This gets called whenever a state with
	 * immediate interactive feedback should provide that
	 * feedback; allowing them to optimize batch mode,
	 * macro and many other cases.
	 *
	 * It can be NULL if not required.
	 */
	teco_state_refresh_cb_t refresh_cb;

	/**
	 * Called at the end of a macro.
	 * Most states/commands are not allowed to end unterminated
	 * at the end of a macro.
	 *
	 * It can be NULL if not required.
	 */
	teco_state_end_of_macro_cb_t end_of_macro_cb;

	/**
	 * Process editing command (or key press).
	 *
	 * This is part of command line handling in interactive
	 * mode and allows the definition of state-specific
	 * editing commands (behaviour on key press).
	 *
	 * By implementing this method, sub-states can either
	 * handle a key and return, chain to the
	 * parent's process_edit_cmd() implementation or even
	 * to the parent state machine's handler.
	 *
	 * All implementations of this method are defined in
	 * cmdline.c.
	 *
	 * This is a mandatory field.
	 */
	teco_state_process_edit_cmd_cb_t process_edit_cmd_cb;

	/**
	 * Whether this state is a start state (ie. not within any
	 * escape sequence etc.).
	 * This is separate of TECO_FNMACRO_MASK_START which is set
	 * only in the main machine's start states.
	 */
	gboolean is_start : 1;
	/**
	 * Function key macro mask.
	 * This is not a bitmask since it is compared with values set
	 * from TECO, so the bitorder needs to be defined.
	 *
	 * @fixme If we intend to "forward" masks from other state machines like
	 * teco_machine_stringbuilding_t, this should probably be a callback.
	 */
	teco_fnmacro_mask_t fnmacro_mask : 8;

	/**
	 * Additional state-dependent callbacks and settings.
	 * This wastes some bytes compared to other techniques for extending teco_state_t
	 * but this is acceptable since there is only a limited number of constant instances.
	 * The main advantage of this approach is that we can use a single
	 * TECO_DEFINE_STATE() for defining and deriving all defaults.
	 */
	union {
		teco_state_expectstring_t	expectstring;
		teco_state_expectqreg_t		expectqreg;
	};
};

/** @} */

gboolean teco_state_end_of_macro(teco_machine_t *ctx, GError **error);

/* in cmdline.c */
gboolean teco_state_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gchar chr, GError **error);

/**
 * @interface TECO_DEFINE_STATE
 * @implements teco_state_t
 * @ingroup states
 *
 * @todo Should we eliminate required callbacks, this could be turned into a
 * struct initializer TECO_INIT_STATE() and TECO_DECLARE_STATE() would become pointless.
 * This would also ease declaring static states.
 */
#define TECO_DEFINE_STATE(NAME, ...) \
	/** @ingroup states */ \
	teco_state_t NAME = { \
		.initial_cb = NULL,	/* do nothing */ \
		.input_cb = (teco_state_input_cb_t)NAME##_input, /* always required */ \
		.refresh_cb = NULL,	/* do nothing */ \
		.end_of_macro_cb = teco_state_end_of_macro, \
		.process_edit_cmd_cb = teco_state_process_edit_cmd, \
		.is_start = FALSE, \
		.fnmacro_mask = TECO_FNMACRO_MASK_DEFAULT, \
		##__VA_ARGS__ \
	}

/** @ingroup states */
#define TECO_DECLARE_STATE(NAME) \
	extern teco_state_t NAME

/* in cmdline.c */
gboolean teco_state_caseinsensitive_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gchar chr, GError **error);

/**
 * @interface TECO_DEFINE_STATE_CASEINSENSITIVE
 * @implements TECO_DEFINE_STATE
 * @ingroup states
 *
 * Base class of states with case-insenstive input.
 *
 * This is meant for states accepting command characters
 * that can possibly be case-folded.
 */
#define TECO_DEFINE_STATE_CASEINSENSITIVE(NAME, ...) \
	TECO_DEFINE_STATE(NAME, \
		.process_edit_cmd_cb = teco_state_caseinsensitive_process_edit_cmd, \
		##__VA_ARGS__ \
	)

/**
 * Base class of state machine.
 *
 * @note On extending teco_machine_t:
 * There is `-fplan9-extensions`, but Clang doesn't support it.
 * There is `-fms-extensions`, but that would require type-unsafe
 * casting to teco_machine_t*.
 * It's possible to portably implement typesafe inheritance by using
 * an anonymous union of an anonymous struct and a named struct, but it's
 * not really worth the trouble in our flat "class" hierachy.
 */
struct teco_machine_t {
	teco_state_t *current;
	/**
	 * Whether side effects must be reverted on rubout.
	 * State machines created within macro calls don't have to
	 * even in interactive mode.
	 */
	gboolean must_undo;
};

static inline void
teco_machine_init(teco_machine_t *ctx, teco_state_t *initial, gboolean must_undo)
{
	ctx->current = initial;
	ctx->must_undo = must_undo;
}

static inline void
teco_machine_reset(teco_machine_t *ctx, teco_state_t *initial)
{
	if (ctx->current != initial)
		teco_undo_ptr(ctx->current) = initial;
}

gboolean teco_machine_input(teco_machine_t *ctx, gchar chr, GError **error);

typedef enum {
	TECO_STRINGBUILDING_MODE_NORMAL = 0,
	TECO_STRINGBUILDING_MODE_UPPER,
	TECO_STRINGBUILDING_MODE_LOWER
} teco_stringbuilding_mode_t;

/**
 * A stringbuilding state machine.
 *
 * @fixme Should contain the escape char (currently in teco_machine_expectstring_t),
 * so that we can escape it via ^Q.
 *
 * @extends teco_machine_t
 */
typedef struct teco_machine_stringbuilding_t {
	teco_machine_t parent;

	/**
	 * A teco_stringbuilding_mode_t.
	 * This is still a guint, so you can call teco_undo_guint().
	 */
	guint mode;

	/**
	 * The escape/termination character.
	 *
	 * If this is `[` or `{`, it is assumed that `]` and `}` must
	 * be escaped as well by teco_machine_stringbuilding_escape().
	 */
	gchar escape_char;

	/**
	 * Q-Register table for local registers.
	 * This is stored here only to be passed to the Q-Reg spec machine.
	 */
	teco_qreg_table_t *qreg_table_locals;

	/**
	 * A QRegister specification parser.
	 * It is allocated since it in turn contains a string building machine.
	 */
	teco_machine_qregspec_t *machine_qregspec;

	/**
	 * A string to append characters to or NULL in parse-only mode.
	 *
	 * @bug As a side-effect, rubbing out in parse-only mode is severely limited
	 * (see teco_state_stringbuilding_start_process_edit_cmd()).
	 */
	teco_string_t *result;
} teco_machine_stringbuilding_t;

void teco_machine_stringbuilding_init(teco_machine_stringbuilding_t *ctx, gchar escape_char,
                                      teco_qreg_table_t *locals, gboolean must_undo);

void teco_machine_stringbuilding_reset(teco_machine_stringbuilding_t *ctx);

/**
 * Parse a string building character.
 *
 * @param ctx The string building machine.
 * @param chr The character to parse.
 * @param result String to append characters to or NULL in parse-only mode.
 * @param error GError.
 * @return FALSE in case of error.
 */
static inline gboolean
teco_machine_stringbuilding_input(teco_machine_stringbuilding_t *ctx, gchar chr,
                                  teco_string_t *result, GError **error)
{
	ctx->result = result;
	return teco_machine_input(&ctx->parent, chr, error);
}

void teco_machine_stringbuilding_escape(teco_machine_stringbuilding_t *ctx, const gchar *str, gsize len,
                                        teco_string_t *target);

void teco_machine_stringbuilding_clear(teco_machine_stringbuilding_t *ctx);

/**
 * Peristent state for teco_state_expectstring_input().
 *
 * This is part of the main machine instead of being a global variable,
 * so that parsers can be run in parallel.
 *
 * Since it will also be part of a macro invocation frame, it will allow
 * for tricks like macro-hooks while in "expectstring" states or calling
 * macros as part of string building characters or macro string arguments.
 */
typedef struct {
	teco_string_t string;
	gsize insert_len;
	gint nesting;

	teco_machine_stringbuilding_t machine;
} teco_machine_expectstring_t;

/**
 * Scintilla message for collection by ES commands.
 *
 * @fixme This is a "forward" declaration, so that we don't introduce cyclic
 * header dependencies.
 * Could presumably be avoided by splitting parser.h in two.
 */
typedef struct {
	unsigned int iMessage;
	uptr_t wParam;
} teco_machine_scintilla_t;

typedef enum {
	/** Normal parsing - ie. execute while parsing */
	TECO_MODE_NORMAL = 0,
	/** Parse, but don't execute until reaching not-yet-defined Goto-label */
	TECO_MODE_PARSE_ONLY_GOTO,
	/** Parse, but don't execute until reaching end of loop */
	TECO_MODE_PARSE_ONLY_LOOP,
	/** Parse, but don't execute until reaching end of conditional or its else-clause */
	TECO_MODE_PARSE_ONLY_COND,
	/** Parse, but don't execute until reaching the very end of conditional */
	TECO_MODE_PARSE_ONLY_COND_FORCE
} teco_mode_t;

/** @extends teco_machine_t */
struct teco_machine_main_t {
	teco_machine_t parent;

	gint macro_pc;

	/**
	 * Aliases bitfield with an integer.
	 * This allows teco_undo_guint(__flags),
	 * while still supporting easy-to-access flags.
	 */
	union {
		struct {
			teco_mode_t mode : 8;

			gboolean modifier_colon : 1;
			gboolean modifier_at : 1;
		};
		guint __flags;
	};

	/** The nesting level of braces */
	guint brace_level;
	/** The nesting level of loops and control structures */
	gint nest_level;
	/**
	 * Loop frame pointer: The number of elements on
	 * the loop stack when a macro invocation frame is
	 * created.
	 * This is used to perform checks for flow control
	 * commands to avoid jumping with invalid PCs while
	 * not creating a new stack per macro frame.
	 */
	guint loop_stack_fp;

	teco_goto_table_t goto_table;
	teco_qreg_table_t *qreg_table_locals;

	/*
	 * teco_state_t-dependent state.
	 *
	 * Some of these cannot be used concurrently and are therefore
	 * grouped into unions.
	 * We could further optimize memory usage by dynamically allocating
	 * some of these structures on demand.
	 */
	teco_machine_expectstring_t		expectstring;
	union {
		teco_string_t			goto_label;
		teco_machine_qregspec_t		*expectqreg;
		teco_machine_scintilla_t	scintilla;
	};
};

void teco_machine_main_init(teco_machine_main_t *ctx,
                            teco_qreg_table_t *qreg_table_locals,
                            gboolean must_undo);

gboolean teco_machine_main_eval_colon(teco_machine_main_t *ctx);

gboolean teco_machine_main_step(teco_machine_main_t *ctx,
                                const gchar *macro, gint stop_pos, GError **error);

gboolean teco_execute_macro(const gchar *macro, gsize macro_len,
                            teco_qreg_table_t *qreg_table_locals, GError **error);
gboolean teco_execute_file(const gchar *filename, teco_qreg_table_t *qreg_table_locals, GError **error);

typedef const struct {
	teco_state_t *next;
	void (*transition_cb)(teco_machine_main_t *ctx, GError **error);
} teco_machine_main_transition_t;

/*
 * FIXME: There should probably be a teco_state_plain with
 * the transitions and their length being stored in
 * teco_state_t::transitions.
 * This does not exclude the possibility of overwriting input_cb.
 */
teco_state_t *teco_machine_main_transition_input(teco_machine_main_t *ctx,
                                                 teco_machine_main_transition_t *transitions,
                                                 guint len, gchar chr, GError **error);

void teco_machine_main_clear(teco_machine_main_t *ctx);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_machine_main_t, teco_machine_main_clear);

teco_state_t *teco_state_expectstring_input(teco_machine_main_t *ctx, gchar chr, GError **error);
gboolean teco_state_expectstring_refresh(teco_machine_main_t *ctx, GError **error);

/* in cmdline.c */
gboolean teco_state_expectstring_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error);

/**
 * @interface TECO_DEFINE_STATE_EXPECTSTRING
 * @implements TECO_DEFINE_STATE
 * @ingroup states
 *
 * Super-class for states accepting string arguments
 * Opaquely cares about alternative-escape characters,
 * string building commands and accumulation into a string
 *
 * @note Generating the input_cb could be avoided if there were a default
 * implementation.
 */
#define TECO_DEFINE_STATE_EXPECTSTRING(NAME, ...) \
	static teco_state_t * \
	NAME##_input(teco_machine_main_t *ctx, gchar chr, GError **error) \
	{ \
		return teco_state_expectstring_input(ctx, chr, error); \
	} \
	TECO_DEFINE_STATE(NAME, \
		.refresh_cb = (teco_state_refresh_cb_t)teco_state_expectstring_refresh, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_expectstring_process_edit_cmd, \
		.fnmacro_mask = TECO_FNMACRO_MASK_STRING, \
		.expectstring.string_building = TRUE, \
		.expectstring.last = TRUE, \
		.expectstring.process_cb = NULL,	/* do nothing */ \
		.expectstring.done_cb = NAME##_done,	/* always required */ \
		##__VA_ARGS__ \
	)

gboolean teco_state_expectfile_process(teco_machine_main_t *ctx, const teco_string_t *str,
                                       gsize new_chars, GError **error);

/* in cmdline.c */
gboolean teco_state_expectfile_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error);

/**
 * @interface TECO_DEFINE_STATE_EXPECTFILE
 * @implements TECO_DEFINE_STATE_EXPECTSTRING
 * @ingroup states
 */
#define TECO_DEFINE_STATE_EXPECTFILE(NAME, ...) \
	TECO_DEFINE_STATE_EXPECTSTRING(NAME, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_expectfile_process_edit_cmd, \
		.expectstring.process_cb = teco_state_expectfile_process, \
		##__VA_ARGS__ \
	)

/* in cmdline.c */
gboolean teco_state_expectdir_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error);

/**
 * @interface TECO_DEFINE_STATE_EXPECTDIR
 * @implements TECO_DEFINE_STATE_EXPECTFILE
 * @ingroup states
 */
#define TECO_DEFINE_STATE_EXPECTDIR(NAME, ...) \
	TECO_DEFINE_STATE_EXPECTFILE(NAME, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_expectdir_process_edit_cmd, \
		##__VA_ARGS__ \
	)
