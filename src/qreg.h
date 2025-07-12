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
#pragma once

#include <glib.h>

#include "sciteco.h"
#include "view.h"
#include "doc.h"
#include "undo.h"
#include "string-utils.h"
#include "rb3str.h"

/*
 * Forward declarations.
 */
typedef struct teco_qreg_t teco_qreg_t;
/* Could be avoided by moving teco_qreg_execute() to the end of the file instead... */
typedef struct teco_qreg_table_t teco_qreg_table_t;

extern teco_view_t *teco_qreg_view;

/*
 * NOTE: This is not "hidden" in qreg.c, so that we won't need wrapper
 * functions for every vtable method.
 *
 * FIXME: Use TECO_DECLARE_VTABLE_METHOD(gboolean, teco_qreg, set_integer, teco_qreg_t *, teco_int_t, GError **);
 * ...
 * teco_qreg_set_integer_t set_integer;
 * ...
 * teco_qreg_set_integer(qreg, 23, error);
 */
typedef const struct {
	gboolean (*set_integer)(teco_qreg_t *qreg, teco_int_t value, GError **error);
	gboolean (*undo_set_integer)(teco_qreg_t *qreg, GError **error);
	gboolean (*get_integer)(teco_qreg_t *qreg, teco_int_t *ret, GError **error);

	gboolean (*set_string)(teco_qreg_t *qreg, const gchar *str, gsize len,
	                       guint codepage, GError **error);
	gboolean (*undo_set_string)(teco_qreg_t *qreg, GError **error);

	/* does not need an explicit undo-call */
	gboolean (*append_string)(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error);

	gboolean (*get_string)(teco_qreg_t *qreg, gchar **str, gsize *len,
	                       guint *codepage, GError **error);
	gboolean (*get_character)(teco_qreg_t *qreg, teco_int_t position,
	                          teco_int_t *chr, GError **error);
	/* always returns length in glyphs in contrast to get_string() */
	teco_int_t (*get_length)(teco_qreg_t *qreg, GError **error);

	/*
	 * These callbacks exist only to optimize teco_qreg_stack_push|pop()
	 * for plain Q-Registers making [q and ]q quite efficient operations even on rubout.
	 * On the other hand, this unnecessarily complicates teco_qreg_t derivations.
	 */
	gboolean (*exchange_string)(teco_qreg_t *qreg, teco_doc_t *src, GError **error);
	gboolean (*undo_exchange_string)(teco_qreg_t *qreg, teco_doc_t *src, GError **error);

	gboolean (*edit)(teco_qreg_t *qreg, GError **error);
	gboolean (*undo_edit)(teco_qreg_t *qreg, GError **error);

	/*
	 * Load and save already care about undo token
	 * creation.
	 */
	gboolean (*load)(teco_qreg_t *qreg, const gchar *filename, GError **error);
	gboolean (*save)(teco_qreg_t *qreg, const gchar *filename, GError **error);
} teco_qreg_vtable_t;

/** @extends teco_rb3str_head_t */
struct teco_qreg_t {
	/*
	 * NOTE: Must be the first member since we "upcast" to teco_qreg_t
	 */
	teco_rb3str_head_t head;

	teco_qreg_vtable_t *vtable;

	teco_int_t integer;
	teco_doc_t string;

	/**
	 * Whether to generate undo tokens (unnecessary for registers
	 * in local qreg tables in macro invocations).
	 *
	 * @fixme Every QRegister has this field, but it only differs
	 * between local and global QRegisters. This wastes space.
	 * Or by deferring any decision about undo token creation to a layer
	 * that knows which table it is accessing.
	 * On the other hand, we will need another flag like
	 * teco_qreg_current_must_undo.
	 *
	 * Otherwise, it might be possible to use a least significant bit
	 * in one of the pointers...
	 */
	gboolean must_undo;
};

teco_qreg_t *teco_qreg_plain_new(const gchar *name, gsize len);
teco_qreg_t *teco_qreg_dot_new(void);
teco_qreg_t *teco_qreg_bufferinfo_new(void);
teco_qreg_t *teco_qreg_workingdir_new(void);
teco_qreg_t *teco_qreg_clipboard_new(const gchar *name);

gboolean teco_qreg_execute(teco_qreg_t *qreg, teco_qreg_table_t *qreg_table_locals, GError **error);

void teco_qreg_undo_set_eol_mode(teco_qreg_t *qreg);
void teco_qreg_set_eol_mode(teco_qreg_t *qreg, gint mode);

/** @memberof teco_qreg_t */
static inline void
teco_qreg_free(teco_qreg_t *qreg)
{
	teco_doc_clear(&qreg->string);
	teco_string_clear(&qreg->head.name);
	g_free(qreg);
}

extern const teco_qreg_table_t *teco_qreg_table_current;
extern teco_qreg_t *teco_qreg_current;

/** @extends teco_rb3str_tree_t */
struct teco_qreg_table_t {
	teco_rb3str_tree_t tree;

	/*
	 * FIXME: Probably even this property can be eliminated.
	 * The only two tables with undo in the system are
	 * a) The global register table
	 * b) The top-level local register table.
	 */
	gboolean must_undo;

	/**
	 * The radix register in this local Q-Register table or NULL.
	 * This is an optimization to avoid frequent table lookups.
	 */
	teco_qreg_t *radix;
};

void teco_qreg_table_init(teco_qreg_table_t *table, gboolean must_undo);
void teco_qreg_table_init_locals(teco_qreg_table_t *table, gboolean must_undo);

/**
 * Insert Q-Register into table.
 *
 * @return If non-NULL a register with the same name as qreg already
 *   existed in table. In this case qreg is __not__ automatically freed.
 * @memberof teco_qreg_table_t
 */
static inline teco_qreg_t *
teco_qreg_table_insert(teco_qreg_table_t *table, teco_qreg_t *qreg)
{
	qreg->must_undo = table->must_undo; // FIXME
	return (teco_qreg_t *)teco_rb3str_insert(&table->tree, TRUE, &qreg->head);
}

/** @memberof teco_qreg_table_t */
static inline void
teco_qreg_table_insert_unique(teco_qreg_table_t *table, teco_qreg_t *qreg)
{
	G_GNUC_UNUSED teco_qreg_t *found = teco_qreg_table_insert(table, qreg);
	g_assert(found == NULL);
}

gboolean teco_qreg_table_replace(teco_qreg_table_t *table, teco_qreg_t *qreg,
                                 gboolean inherit_int, GError **error);

/** @memberof teco_qreg_table_t */
static inline teco_qreg_t *
teco_qreg_table_find(teco_qreg_table_t *table, const gchar *name, gsize len)
{
	return (teco_qreg_t *)teco_rb3str_find(&table->tree, TRUE, name, len);
}

teco_qreg_t *teco_qreg_table_edit_name(teco_qreg_table_t *table, const gchar *name,
                                       gsize len, GError **error);

/** @memberof teco_qreg_table_t */
static inline gboolean
teco_qreg_table_edit(teco_qreg_table_t *table, teco_qreg_t *qreg, GError **error)
{
	if (!qreg->vtable->edit(qreg, error))
		return FALSE;
	teco_qreg_table_current = table;
	teco_qreg_current = qreg;
	return TRUE;
}

gboolean teco_qreg_table_set_environ(teco_qreg_table_t *table, GError **error);
gchar **teco_qreg_table_get_environ(teco_qreg_table_t *table, GError **error);

gboolean teco_qreg_table_empty(teco_qreg_table_t *table, GError **error);
void teco_qreg_table_clear(teco_qreg_table_t *table);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_qreg_table_t, teco_qreg_table_clear);

extern teco_qreg_table_t teco_qreg_table_globals;

gboolean teco_qreg_stack_push(teco_qreg_t *qreg, GError **error);
gboolean teco_qreg_stack_pop(teco_qreg_t *qreg, GError **error);
void teco_qreg_stack_clear(void);

typedef enum {
	TECO_ED_HOOK_ADD = 1,
	TECO_ED_HOOK_EDIT,
	TECO_ED_HOOK_CLOSE,
	TECO_ED_HOOK_QUIT
} teco_ed_hook_t;

gboolean teco_ed_hook(teco_ed_hook_t type, GError **error);

typedef enum {
	TECO_MACHINE_QREGSPEC_ERROR = 0,
	TECO_MACHINE_QREGSPEC_MORE,
	TECO_MACHINE_QREGSPEC_DONE
} teco_machine_qregspec_status_t;

typedef enum {
	/** Register must exist, else fail */
	TECO_QREG_REQUIRED,
	/**
	 * Return NULL if register does not exist.
	 * You can still call QRegSpecMachine::fail() to require it.
	 */
	TECO_QREG_OPTIONAL,
	/** Initialize register if it does not already exist */
	TECO_QREG_OPTIONAL_INIT
} teco_qreg_type_t;

typedef struct teco_machine_qregspec_t teco_machine_qregspec_t;

teco_machine_qregspec_t *teco_machine_qregspec_new(teco_qreg_type_t type,
                                                   teco_qreg_table_t *locals, gboolean must_undo);

void teco_machine_qregspec_reset(teco_machine_qregspec_t *ctx);

/*
 * FIXME: This uses a forward declaration since we must not include parser.h
 */
struct teco_machine_stringbuilding_t *teco_machine_qregspec_get_stringbuilding(teco_machine_qregspec_t *ctx);

teco_machine_qregspec_status_t teco_machine_qregspec_input(teco_machine_qregspec_t *ctx, gunichar chr,
                                                           teco_qreg_t **result,
                                                           teco_qreg_table_t **result_table, GError **error);

void teco_machine_qregspec_get_results(teco_machine_qregspec_t *ctx,
                                       teco_qreg_t **result, teco_qreg_table_t **result_table);

gboolean teco_machine_qregspec_auto_complete(teco_machine_qregspec_t *ctx, teco_string_t *insert);

void teco_machine_qregspec_free(teco_machine_qregspec_t *ctx);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(teco_machine_qregspec_t, teco_machine_qregspec_free);

/** @memberof teco_machine_qregspec_t */
void undo__teco_machine_qregspec_clear(teco_machine_qregspec_t **);
TECO_DECLARE_UNDO_OBJECT(qregspec, teco_machine_qregspec_t *);

#define teco_undo_qregspec_own(VAR) \
	(*teco_undo_object_qregspec_push(&(VAR)))
