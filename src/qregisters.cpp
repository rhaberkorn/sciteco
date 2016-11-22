/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "string-utils.h"
#include "interface.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "document.h"
#include "ring.h"
#include "ioview.h"
#include "eol.h"
#include "error.h"
#include "qregisters.h"

namespace SciTECO {

namespace States {
	StatePushQReg		pushqreg;
	StatePopQReg		popqreg;
	StateEQCommand		eqcommand;
	StateLoadQReg		loadqreg;
	StateEPctCommand	epctcommand;
	StateSaveQReg		saveqreg;
	StateQueryQReg		queryqreg;
	StateCtlUCommand	ctlucommand;
	StateEUCommand		eucommand;
	StateSetQRegString	setqregstring_nobuilding(false);
	StateSetQRegString	setqregstring_building(true);
	StateGetQRegString	getqregstring;
	StateSetQRegInteger	setqreginteger;
	StateIncreaseQReg	increaseqreg;
	StateMacro		macro;
	StateMacroFile		macro_file;
	StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	QRegisterTable		*locals = NULL;
	QRegister		*current = NULL;

	static QRegisterStack	stack;
}

static QRegister *register_argument = NULL;

void
QRegisterData::set_string(const gchar *str, gsize len)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.reset();
	string.edit(QRegisters::view);

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_CLEARALL);
	QRegisters::view.ssm(SCI_APPENDTEXT,
	                     len, (sptr_t)(str ? : ""));
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

void
QRegisterData::undo_set_string(void)
{
	if (!must_undo)
		return;

	/*
	 * Necessary, so that upon rubout the
	 * string's parameters are restored.
	 */
	string.update(QRegisters::view);

	if (QRegisters::current && QRegisters::current->must_undo)
		QRegisters::current->string.undo_edit(QRegisters::view);

	string.undo_reset();
	QRegisters::view.undo_ssm(SCI_UNDO);

	string.undo_edit(QRegisters::view);
}

void
QRegisterData::append_string(const gchar *str, gsize len)
{
	/*
	 * NOTE: Will not create undo action
	 * if string is empty.
	 * Also, appending preserves the string's
	 * parameters.
	 */
	if (!len)
		return;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_APPENDTEXT,
	                     len, (sptr_t)str);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

gchar *
QRegisterData::get_string(void)
{
	gint size;
	gchar *str;

	if (!string.is_initialized())
		return g_strdup("");

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	size = QRegisters::view.ssm(SCI_GETLENGTH) + 1;
	str = (gchar *)g_malloc(size);
	QRegisters::view.ssm(SCI_GETTEXT, size, (sptr_t)str);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return str;
}

gsize
QRegisterData::get_string_size(void)
{
	gsize size;

	if (!string.is_initialized())
		return 0;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	size = QRegisters::view.ssm(SCI_GETLENGTH);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return size;
}

gint
QRegisterData::get_character(gint position)
{
	gint ret = -1;

	if (position < 0)
		return -1;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	if (position < QRegisters::view.ssm(SCI_GETLENGTH))
		ret = QRegisters::view.ssm(SCI_GETCHARAT, position);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return ret;
}

void
QRegisterData::undo_exchange_string(QRegisterData &reg)
{
	if (must_undo)
		string.undo_exchange();
	if (reg.must_undo)
		reg.string.undo_exchange();
}

void
QRegister::edit(void)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);
	interface.show_view(&QRegisters::view);
	interface.info_update(this);
}

void
QRegister::undo_edit(void)
{
	/*
	 * We might be switching the current document
	 * to a buffer.
	 */
	string.update(QRegisters::view);

	if (!must_undo)
		return;

	interface.undo_info_update(this);
	string.undo_edit(QRegisters::view);
	interface.undo_show_view(&QRegisters::view);
}

void
QRegister::execute(bool locals)
{
	gchar *str = get_string();

	try {
		Execute::macro(str, locals);
	} catch (Error &error) {
		error.add_frame(new Error::QRegFrame(name));

		g_free(str);
		throw; /* forward */
	} catch (...) {
		g_free(str);
		throw; /* forward */
	}

	g_free(str);
}

void
QRegister::undo_set_eol_mode(void)
{
	if (!must_undo)
		return;

	/*
	 * Necessary, so that upon rubout the
	 * string's parameters are restored.
	 */
	string.update(QRegisters::view);

	if (QRegisters::current && QRegisters::current->must_undo)
		QRegisters::current->string.undo_edit(QRegisters::view);

	QRegisters::view.undo_ssm(SCI_SETEOLMODE,
	                          QRegisters::view.ssm(SCI_GETEOLMODE));

	string.undo_edit(QRegisters::view);
}

void
QRegister::set_eol_mode(gint mode)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);
	QRegisters::view.ssm(SCI_SETEOLMODE, mode);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

void
QRegister::load(const gchar *filename)
{
	undo_set_string();

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);
	string.reset();

	/*
	 * IOView::load() might change the EOL style.
	 */
	undo_set_eol_mode();

	/*
	 * undo_set_string() pushes undo tokens that restore
	 * the previous document in the view.
	 * So if loading fails, QRegisters::current will be
	 * made the current document again.
	 */
	QRegisters::view.load(filename);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

void
QRegister::save(const gchar *filename)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	try {
		QRegisters::view.save(filename);
	} catch (...) {
		if (QRegisters::current)
			QRegisters::current->string.edit(QRegisters::view);
		throw; /* forward */
	}

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

tecoInt
QRegisterBufferInfo::set_integer(tecoInt v)
{
	if (!ring.edit(v))
		throw Error("Invalid buffer id %" TECO_INTEGER_FORMAT, v);

	return v;
}

void
QRegisterBufferInfo::undo_set_integer(void)
{
	current_doc_undo_edit();
}

tecoInt
QRegisterBufferInfo::get_integer(void)
{
	return ring.get_id();
}

gchar *
QRegisterBufferInfo::get_string(void)
{
	gchar *str = g_strdup(ring.current->filename ? : "");

	/*
	 * On platforms with a default non-forward-slash directory
	 * separator (i.e. Windows), Buffer::filename will have
	 * the wrong separator.
	 * To make the life of macros that evaluate "*" easier,
	 * the directory separators are normalized to "/" here.
	 * This does not change the size of the string, so
	 * get_string_size() still works.
	 */
	return normalize_path(str);
}

gsize
QRegisterBufferInfo::get_string_size(void)
{
	return ring.current->filename ? strlen(ring.current->filename) : 0;
}

gint
QRegisterBufferInfo::get_character(gint position)
{
	if (position < 0 ||
	    position >= (gint)QRegisterBufferInfo::get_string_size())
		return -1;

	return ring.current->filename[position];
}

void
QRegisterBufferInfo::edit(void)
{
	gchar *str;

	QRegister::edit();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	str = QRegisterBufferInfo::get_string();
	QRegisters::view.ssm(SCI_SETTEXT, 0, (sptr_t)str);
	g_free(str);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	QRegisters::view.undo_ssm(SCI_UNDO);
}

void
QRegisterWorkingDir::set_string(const gchar *str, gsize len)
{
	/* str is not null-terminated */
	gchar *dir = g_strndup(str, len);
	int ret = g_chdir(dir);

	g_free(dir);

	if (ret)
		/* FIXME: Is errno usable on Windows here? */
		throw Error("Cannot change working directory "
		            "to \"%.*s\"", (int)len, str);
}

void
QRegisterWorkingDir::undo_set_string(void)
{
	/* pass ownership of current dir string */
	undo.push_own<UndoTokenChangeDir>(g_get_current_dir());
}

gchar *
QRegisterWorkingDir::get_string(void)
{
	/*
	 * On platforms with a default non-forward-slash directory
	 * separator (i.e. Windows), Buffer::filename will have
	 * the wrong separator.
	 * To make the life of macros that evaluate "$" easier,
	 * the directory separators are normalized to "/" here.
	 * This does not change the size of the string, so
	 * get_string_size() still works.
	 */
	return normalize_path(g_get_current_dir());
}

gsize
QRegisterWorkingDir::get_string_size(void)
{
	gchar *str = g_get_current_dir();
	gsize len = strlen(str);

	g_free(str);
	return len;
}

gint
QRegisterWorkingDir::get_character(gint position)
{
	gchar *str = QRegisterWorkingDir::get_string();
	gint ret = -1;

	if (position >= 0 &&
	    position < (gint)strlen(str))
		ret = str[position];

	g_free(str);
	return ret;
}

void
QRegisterWorkingDir::edit(void)
{
	gchar *str;

	QRegister::edit();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	str = QRegisterWorkingDir::get_string();
	QRegisters::view.ssm(SCI_SETTEXT, 0, (sptr_t)str);
	g_free(str);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	QRegisters::view.undo_ssm(SCI_UNDO);
}

void
QRegisterWorkingDir::exchange_string(QRegisterData &reg)
{
	gchar *own_str = QRegisterWorkingDir::get_string();
	gchar *other_str = reg.get_string();

	QRegisterData::set_string(other_str);
	g_free(other_str);
	reg.set_string(own_str);
	g_free(own_str);
}

void
QRegisterWorkingDir::undo_exchange_string(QRegisterData &reg)
{
	QRegisterWorkingDir::undo_set_string();
	reg.undo_set_string();
}

void
QRegisterClipboard::UndoTokenSetClipboard::run(void)
{
	interface.set_clipboard(name, str, str_len);
}

void
QRegisterClipboard::set_string(const gchar *str, gsize len)
{
	if (Flags::ed & Flags::ED_AUTOEOL) {
		GString *str_converted = g_string_sized_new(len);
		/*
		 * This will convert to the Q-Register view's EOL mode.
		 */
		EOLWriterMem writer(str_converted,
		                    QRegisters::view.ssm(SCI_GETEOLMODE));
		gsize bytes_written;

		/*
		 * NOTE: Shouldn't throw any error, ever.
		 */
		bytes_written = writer.convert(str, len);
		g_assert(bytes_written == len);

		interface.set_clipboard(get_clipboard_name(),
		                        str_converted->str, str_converted->len);

		g_string_free(str_converted, TRUE);
	} else {
		/*
		 * No EOL conversion necessary. The EOLWriter can handle
		 * this as well, but will result in unnecessary allocations.
		 */
		interface.set_clipboard(get_clipboard_name(), str, len);
	}
}

void
QRegisterClipboard::undo_set_string(void)
{
	gchar *str;
	gsize str_len;

	/*
	 * Upon rubout, the current contents of the clipboard are
	 * restored.
	 * We are checking for undo.enabled instead of relying on
	 * undo.push_own(), since getting the clipboard
	 * is an expensive operation that we want to avoid.
	 */
	if (!undo.enabled)
		return;

	/*
	 * Ownership of str (may be NULL) is passed to
	 * the undo token. We do not need undo.push_own() since
	 * we checked for undo.enabled before.
	 * This avoids any EOL translation as that would be cumbersome
	 * and could also modify the clipboard in unexpected ways.
	 */
	str = interface.get_clipboard(get_clipboard_name(), &str_len);
	undo.push<UndoTokenSetClipboard>(get_clipboard_name(), str, str_len);
}

gchar *
QRegisterClipboard::get_string(gsize *out_len)
{
	if (!(Flags::ed & Flags::ED_AUTOEOL)) {
		/*
		 * No auto-eol conversion - avoid unnecessary copying
		 * and allocations.
		 * NOTE: get_clipboard() already returns a null-terminated string.
		 */
		return interface.get_clipboard(get_clipboard_name(), out_len);
	}

	gsize str_len;
	gchar *str = interface.get_clipboard(get_clipboard_name(), &str_len);
	EOLReaderMem reader(str, str_len);
	gchar *str_converted;

	try {
		str_converted = reader.convert_all(out_len);
	} catch (...) {
		g_free(str);
		throw; /* forward */
	}
	g_free(str);

	return str_converted;
}

gchar *
QRegisterClipboard::get_string(void)
{
	return QRegisterClipboard::get_string(NULL);
}

gsize
QRegisterClipboard::get_string_size(void)
{
	gsize str_len;
	gchar *str = interface.get_clipboard(get_clipboard_name(), &str_len);
	/*
	 * Using the EOLReader does not hurt much if AutoEOL is disabled
	 * since we use it only for counting the bytes.
	 */
	EOLReaderMem reader(str, str_len);
	gsize data_len;
	gsize converted_len = 0;

	try {
		while (reader.convert(data_len))
			converted_len += data_len;
	} catch (...) {
		g_free(str);
		throw; /* forward */
	}
	g_free(str);

	return converted_len;
}

gint
QRegisterClipboard::get_character(gint position)
{
	gsize str_len;
	gchar *str = QRegisterClipboard::get_string(&str_len);
	gint ret = -1;

	/*
	 * `str` may be NULL, but only if str_len == 0 as well.
	 */
	if (position >= 0 &&
	    position < (gint)str_len)
		ret = str[position];

	g_free(str);
	return ret;
}

void
QRegisterClipboard::edit(void)
{
	gchar *str;
	gsize str_len;

	QRegister::edit();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_CLEARALL);
	str = QRegisterClipboard::get_string(&str_len);
	QRegisters::view.ssm(SCI_APPENDTEXT, str_len, (sptr_t)str);
	g_free(str);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	QRegisters::view.undo_ssm(SCI_UNDO);
}

void
QRegisterClipboard::exchange_string(QRegisterData &reg)
{
	gchar *own_str;
	gsize own_str_len;
	gchar *other_str = reg.get_string();
	gsize other_str_len = reg.get_string_size();

	/*
	 * FIXME: What if `reg` is a clipboard and it changes
	 * between the two calls?
	 * QRegister::get_string() should always return the length as well.
	 */
	QRegisterData::set_string(other_str, other_str_len);
	g_free(other_str);
	own_str = QRegisterClipboard::get_string(&own_str_len);
	reg.set_string(own_str, own_str_len);
	g_free(own_str);
}

void
QRegisterClipboard::undo_exchange_string(QRegisterData &reg)
{
	QRegisterClipboard::undo_set_string();
	reg.undo_set_string();
}

void
QRegisterTable::UndoTokenRemoveGlobal::run(void)
{
	delete (QRegister *)QRegisters::globals.remove(reg);
}

void
QRegisterTable::UndoTokenRemoveLocal::run(void)
{
	/*
	 * NOTE: QRegisters::locals should point
	 * to the correct table when the token is
	 * executed.
	 */
	delete (QRegister *)QRegisters::locals->remove(reg);
}

void
QRegisterTable::undo_remove(QRegister *reg)
{
	if (!must_undo)
		return;

	/*
	 * NOTE: Could also be solved using a virtual
	 * method and subclasses...
	 */
	if (this == &QRegisters::globals)
		undo.push<UndoTokenRemoveGlobal>(reg);
	else
		undo.push<UndoTokenRemoveLocal>(reg);
}

void
QRegisterTable::insert_defaults(void)
{
	/* general purpose registers */
	for (gchar q = 'A'; q <= 'Z'; q++)
		insert(q);
	for (gchar q = '0'; q <= '9'; q++)
		insert(q);
}

/*
 * NOTE: by not making this inline,
 * we can access QRegisters::current
 */
void
QRegisterTable::edit(QRegister *reg)
{
	reg->edit();
	QRegisters::current = reg;
}

/**
 * Import process environment into table
 * by setting environment registers for every
 * environment variable.
 * It is assumed that the table does not yet
 * contain any environment register.
 *
 * In general this method is only safe to call
 * at startup.
 */
void
QRegisterTable::set_environ(void)
{
	/*
	 * NOTE: Using g_get_environ() would be more efficient,
	 * but it appears to be broken, at least on Wine
	 * and Windows 2000.
	 */
	gchar **env = g_listenv();

	for (gchar **key = env; *key; key++) {
		gchar name[1 + strlen(*key) + 1];
		QRegister *reg;

		name[0] = '$';
		strcpy(name + 1, *key);

		reg = insert(name);
		reg->set_string(g_getenv(*key));
	}

	g_strfreev(env);
}

/**
 * Export environment registers as a list of environment
 * variables compatible with `g_get_environ()`.
 *
 * @return Zero-terminated list of strings in the form
 *         `NAME=VALUE`. Should be freed with `g_strfreev()`.
 */
gchar **
QRegisterTable::get_environ(void)
{
	QRegister *first = nfind("$");

	gint envp_len = 1;
	gchar **envp, **p;

	/*
	 * Iterate over all registers beginning with "$" to
	 * guess the size required for the environment array.
	 * This may waste a few bytes because not __every__
	 * register beginning with "$" is an environment
	 * register.
	 */
	for (QRegister *cur = first;
	     cur && cur->name[0] == '$';
	     cur = (QRegister *)cur->next())
		envp_len++;

	p = envp = (gchar **)g_malloc(sizeof(gchar *)*envp_len);

	for (QRegister *cur = first;
	     cur && cur->name[0] == '$';
	     cur = (QRegister *)cur->next()) {
		gchar *value;

		/*
		 * Ignore the "$" register (not an environment
		 * variable register) and registers whose
		 * name contains "=" (not allowed in environment
		 * variable names).
		 */
		if (!cur->name[1] || strchr(cur->name+1, '='))
			continue;

		value = cur->get_string();
		/* more efficient than g_environ_setenv() */
		*p++ = g_strconcat(cur->name+1, "=", value, NIL);
		g_free(value);
	}

	*p = NULL;

	return envp;
}

/**
 * Update process environment with environment registers
 * using `g_setenv()`.
 * It does not try to unset environment variables that
 * are no longer in the Q-Register table.
 *
 * This method may be dangerous in a multi-threaded environment
 * but may be necessary for libraries that access important
 * environment variables internally without providing alternative
 * APIs.
 */
void
QRegisterTable::update_environ(void)
{
	for (QRegister *cur = nfind("$");
	     cur && cur->name[0] == '$';
	     cur = (QRegister *)cur->next()) {
		gchar *value;

		/*
		 * Ignore the "$" register (not an environment
		 * variable register) and registers whose
		 * name contains "=" (not allowed in environment
		 * variable names).
		 */
		if (!cur->name[1] || strchr(cur->name+1, '='))
			continue;

		value = cur->get_string();
		g_setenv(cur->name+1, value, TRUE);
		g_free(value);
	}
}

/**
 * Free resources associated with table.
 *
 * This is similar to the destructor but
 * has the advantage that we can check whether some
 * register is currently edited.
 * Since this is not a destructor, we can throw
 * errors.
 * Therefore this method should be called before
 * a (local) QRegisterTable is deleted.
 */
void
QRegisterTable::clear(void)
{
	QRegister *cur;

	while ((cur = (QRegister *)root())) {
		if (cur == QRegisters::current)
			throw Error("Currently edited Q-Register \"%s\" "
			            "cannot be discarded", cur->name);

		delete (QRegister *)remove(cur);
	}
}

void
QRegisterStack::UndoTokenPush::run(void)
{
	SLIST_INSERT_HEAD(&stack->head, entry, entries);
	entry = NULL;
}

void
QRegisterStack::UndoTokenPop::run(void)
{
	Entry *entry = SLIST_FIRST(&stack->head);

	SLIST_REMOVE_HEAD(&stack->head, entries);
	delete entry;
}

void
QRegisterStack::push(QRegister &reg)
{
	Entry *entry = new Entry();

	gchar *str = reg.get_string();
	if (*str)
		entry->set_string(str);
	g_free(str);
	entry->string.update(reg.string);
	entry->set_integer(reg.get_integer());

	SLIST_INSERT_HEAD(&head, entry, entries);
	undo.push<UndoTokenPop>(this);
}

bool
QRegisterStack::pop(QRegister &reg)
{
	Entry *entry = SLIST_FIRST(&head);

	if (!entry)
		return false;

	reg.undo_set_integer();
	reg.set_integer(entry->get_integer());

	/* exchange document ownership between Stack entry and Q-Register */
	reg.undo_exchange_string(*entry);
	reg.exchange_string(*entry);

	SLIST_REMOVE_HEAD(&head, entries);
	/* Pass entry ownership to undo stack. */
	undo.push_own<UndoTokenPush>(this, entry);

	return true;
}

QRegisterStack::~QRegisterStack()
{
	Entry *entry, *next;

	SLIST_FOREACH_SAFE(entry, &head, entries, next)
		delete entry;
}

void
QRegisters::hook(Hook type)
{
	static const gchar *type2name[] = {
		/* [HOOK_ADD-1] = */	"ADD",
		/* [HOOK_EDIT-1] = */	"EDIT",
		/* [HOOK_CLOSE-1] = */	"CLOSE",
		/* [HOOK_QUIT-1] = */	"QUIT",
	};

	QRegister *reg;

	if (!(Flags::ed & Flags::ED_HOOKS))
		return;

	try {
		reg = globals["ED"];
		if (!reg)
			throw Error("Undefined ED-hook register (\"ED\")");

		/*
		 * ED-hook execution should not see any
		 * integer parameters but the hook type.
		 * Such parameters could confuse the ED macro
		 * and macro authors do not expect side effects
		 * of ED macros on the expression stack.
		 * Also make sure it does not leave behind
		 * additional arguments on the stack.
		 *
		 * So this effectively executes:
		 * (typeM[ED]^[)
		 */
		expressions.brace_open();
		expressions.push(type);
		reg->execute();
		expressions.discard_args();
		expressions.brace_close();
	} catch (Error &error) {
		const gchar *type_str = type2name[type-1];

		error.add_frame(new Error::EDHookFrame(type_str));
		throw; /* forward */
	}
}

void
QRegSpecMachine::reset(void)
{
	MicroStateMachine<QRegister *>::reset();
	string_machine.reset();
	undo.push_var(is_local) = false;
	undo.push_var(nesting) = 0;
	undo.push_str(name);
	g_free(name);
	name = NULL;
}

bool
QRegSpecMachine::input(gchar chr, QRegister *&result)
{
	gchar *insert;

MICROSTATE_START;
	switch (chr) {
	case '#':
		set(&&StateFirstChar);
		break;
	case '[':
		set(&&StateString);
		undo.push_var(nesting)++;
		break;
	case '.':
		if (!is_local) {
			undo.push_var(is_local) = true;
			break;
		}
		/* fall through */
	default:
		undo.push_str(name) = String::chrdup(String::toupper(chr));
		goto done;
	}

	return false;

StateFirstChar:
	undo.push_str(name) = (gchar *)g_malloc(3);
	name[0] = String::toupper(chr);
	name[1] = '\0';
	set(&&StateSecondChar);
	return false;

StateSecondChar:
	name[1] = String::toupper(chr);
	name[2] = '\0';
	goto done;

StateString:
	switch (chr) {
	case '[':
		undo.push_var(nesting)++;
		break;
	case ']':
		undo.push_var(nesting)--;
		if (!nesting)
			goto done;
		break;
	}

	if (mode > MODE_NORMAL)
		return false;

	if (!string_machine.input(chr, insert))
		return false;

	undo.push_str(name);
	String::append(name, insert);
	g_free(insert);
	return false;

done:
	if (mode > MODE_NORMAL) {
		/*
		 * StateExpectQRegs with type != OPTIONAL
		 * will never see this NULL pointer beyond
		 * BEGIN_EXEC()
		 */
		result = NULL;
		return true;
	}

	QRegisterTable &table = is_local ? *QRegisters::locals
	                                 : QRegisters::globals;

	switch (type) {
	case QREG_REQUIRED:
		result = table[name];
		if (!result)
			fail();
		break;

	case QREG_OPTIONAL:
		result = table[name];
		break;

	case QREG_OPTIONAL_INIT:
		result = table[name];
		if (!result) {
			result = table.insert(name);
			table.undo_remove(result);
		}
		break;
	}

	return true;
}

gchar *
QRegSpecMachine::auto_complete(void)
{
	gsize restrict_len = 0;

	if (string_machine.qregspec_machine)
		/* nested Q-Reg definition */
		return string_machine.qregspec_machine->auto_complete();

	if (state == StateStart)
		/* single-letter Q-Reg */
		restrict_len = 1;
	else if (!nesting)
		/* two-letter Q-Reg */
		restrict_len = 2;

	QRegisterTable &table = is_local ? *QRegisters::locals
	                                 : QRegisters::globals;
	return table.auto_complete(name, nesting == 1 ? ']' : '\0',
	                           restrict_len);
}

/*
 * Command states
 */

StateExpectQReg::StateExpectQReg(QRegSpecType type) : machine(type)
{
	transitions['\0'] = this;
}

State *
StateExpectQReg::custom(gchar chr)
{
	QRegister *reg;

	if (!machine.input(chr, reg))
		return this;

	/*
	 * NOTE: We must reset the Q-Reg machine
	 * now, since we have commands like <M>
	 * that indirectly call their state recursively.
	 */
	machine.reset();
	return got_register(reg);
}

/*$ "[" "[q" push
 * [q -- Save Q-Register
 *
 * Save Q-Register <q> contents on the global Q-Register push-down
 * stack.
 */
State *
StatePushQReg::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	QRegisters::stack.push(*reg);

	return &States::start;
}

/*$ "]" "]q" pop
 * ]q -- Restore Q-Register
 *
 * Restore Q-Register <q> by replacing its contents
 * with the contents of the register saved on top of
 * the Q-Register push-down stack.
 * The stack entry is popped.
 *
 * In interactive mode, the original contents of <q>
 * are not immediately reclaimed but are kept in memory
 * to support rubbing out the command.
 * Memory is reclaimed on command-line termination.
 */
State *
StatePopQReg::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	if (!QRegisters::stack.pop(*reg))
		throw Error("Q-Register stack is empty");

	return &States::start;
}

/*$ EQ EQq
 * EQq$ -- Edit or load Q-Register
 * EQq[file]$
 *
 * When specified with an empty <file> string argument,
 * EQ makes <q> the currently edited Q-Register.
 * Otherwise, when <file> is specified, it is the
 * name of a file to read into Q-Register <q>.
 * When loading a file, the currently edited
 * buffer/register is not changed and the edit position
 * of register <q> is reset to 0.
 *
 * Undefined Q-Registers will be defined.
 * The command fails if <file> could not be read.
 */
State *
StateEQCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::loadqreg);
	undo.push_var(register_argument) = reg;
	return &States::loadqreg;
}

State *
StateLoadQReg::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);

	if (*filename) {
		/* Load file into Q-Register */
		register_argument->load(filename);
	} else {
		/* Edit Q-Register */
		current_doc_undo_edit();
		QRegisters::globals.edit(register_argument);
	}

	return &States::start;
}

/*$ E% E%q
 * E%q<file>$ -- Save Q-Register string to file
 *
 * Saves the string contents of Q-Register <q> to
 * <file>.
 * The <file> must always be specified, as Q-Registers
 * have no notion of associated file names.
 *
 * In interactive mode, the E% command may be rubbed out,
 * restoring the previous state of <file>.
 * This follows the same rules as with the \fBEW\fP command.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 */
State *
StateEPctCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::saveqreg);
	undo.push_var(register_argument) = reg;
	return &States::saveqreg;
}

State *
StateSaveQReg::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);
	register_argument->save(filename);
	return &States::start;
}

/*$ Q Qq query
 * Qq -> n -- Query Q-Register existence, its integer or string characters
 * <position>Qq -> character
 * :Qq -> -1 | size
 *
 * Without any arguments, get and return the integer-part of
 * Q-Register <q>.
 *
 * With one argument, return the <character> code at <position>
 * from the string-part of Q-Register <q>.
 * Positions are handled like buffer positions \(em they
 * begin at 0 up to the length of the string minus 1.
 * An error is thrown for invalid positions.
 * Both non-colon-modified forms of Q require register <q>
 * to be defined and fail otherwise.
 *
 * When colon-modified, Q does not pop any arguments from
 * the expression stack and returns the <size> of the string
 * in Q-Register <q> if register <q> exists (i.e. is defined).
 * Naturally, for empty strings, 0 is returned.
 * When colon-modified and Q-Register <q> is undefined,
 * -1 is returned instead.
 * Therefore checking the return value \fB:Q\fP for values smaller
 * 0 allows checking the existence of a register.
 * Note that if <q> exists, its string part is not initialized,
 * so \fB:Q\fP may be used to handle purely numeric data structures
 * without creating Scintilla documents by accident.
 * These semantics allow the useful idiom \(lq:Q\fIq\fP">\(rq for
 * checking whether a Q-Register exists and has a non-empty string.
 * Note also that the return value of \fB:Q\fP may be interpreted
 * as a condition boolean that represents the non-existence of <q>.
 * If <q> is undefined, it returns \fIsuccess\fP, else a \fIfailure\fP
 * boolean.
 */
StateQueryQReg::StateQueryQReg() : machine(QREG_OPTIONAL)
{
	transitions['\0'] = this;
}

State *
StateQueryQReg::custom(gchar chr)
{
	QRegister *reg;

	if (!machine.input(chr, reg))
		return this;

	/* like BEGIN_EXEC(&States::start), but resets machine */
	if (mode > MODE_NORMAL)
		goto reset;

	expressions.eval();

	if (eval_colon()) {
		/* Query Q-Register's existence or string size */
		expressions.push(reg ? reg->get_string_size()
		                     : (tecoInt)-1);
		goto reset;
	}

	/*
	 * NOTE: This command is special since the QRegister is required
	 * without colon and otherwise optional.
	 * While it may be clearer to model this as two States,
	 * we cannot currently let parsing depend on the colon-modifier.
	 * That's why we have to declare the Q-Reg machine as QREG_OPTIONAL
	 * and care about exception throwing on our own.
	 * Since we need the machine's state to throw a reasonable error
	 * we cannot derive from StateExpectQReg since it has to reset the
	 * machine before calling got_register().
	 */
	if (!reg)
		machine.fail();

	if (expressions.args() > 0) {
		/* Query character from Q-Register string */
		gint c = reg->get_character(expressions.pop_num_calc());
		if (c < 0)
			throw RangeError('Q');
		expressions.push(c);
	} else {
		/* Query integer */
		expressions.push(reg->get_integer());
	}

reset:
	machine.reset();
	return &States::start;
}

/*$ ^Uq
 * [c1,c2,...]^Uq[string]$ -- Set or append to Q-Register string without string building
 * [c1,c2,...]:^Uq[string]$
 *
 * If not colon-modified, it first fills the Q-Register <q>
 * with all the values on the expression stack (interpreted as
 * codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> will be the first character in <q>, <c2> the second, etc.
 * Eventually the <string> argument is appended to the
 * register.
 * Any existing string value in <q> is overwritten by this operation.
 *
 * In the colon-modified form ^U does not overwrite existing
 * contents of <q> but only appends to it.
 *
 * If <q> is undefined, it will be defined.
 *
 * String-building characters are \fBdisabled\fP for ^U
 * commands.
 * Therefore they are especially well-suited for defining
 * \*(ST macros, since string building characters in the
 * desired Q-Register contents do not have to be escaped.
 * The \fBEU\fP command may be used where string building
 * is desired.
 */
State *
StateCtlUCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::setqregstring_nobuilding);
	undo.push_var(register_argument) = reg;
	return &States::setqregstring_nobuilding;
}

/*$ EU EUq
 * [c1,c2,...]EUq[string]$ -- Set or append to Q-Register string with string building characters
 * [c1,c2,...]:EUq[string]$
 *
 * This command sets or appends to the contents of
 * Q-Register \fIq\fP.
 * It is identical to the \fB^U\fP command, except
 * that this form of the command has string building
 * characters \fBenabled\fP.
 */
State *
StateEUCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::setqregstring_building);
	undo.push_var(register_argument) = reg;
	return &States::setqregstring_building;
}

void
StateSetQRegString::initial(void)
{
	int args;

	expressions.eval();
	args = expressions.args();
	text_added = args > 0;
	if (!args)
		return;

	gchar buffer[args+1];

	buffer[args] = '\0';
	while (args--)
		buffer[args] = (gchar)expressions.pop_num_calc();

	if (eval_colon()) {
		/* append to register */
		register_argument->undo_append_string();
		register_argument->append_string(buffer);
	} else {
		/* set register */
		register_argument->undo_set_string();
		register_argument->set_string(buffer);
	}
}

State *
StateSetQRegString::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (text_added || eval_colon()) {
		/*
		 * Append to register:
		 * Note that append_string() does not create an UNDOACTION
		 * if str == NULL
		 */
		if (str) {
			register_argument->undo_append_string();
			register_argument->append_string(str);
		}
	} else {
		/* set register */
		register_argument->undo_set_string();
		register_argument->set_string(str);
	}

	return &States::start;
}

/*$ G Gq get
 * Gq -- Insert Q-Register string
 *
 * Inserts the string of Q-Register <q> into the buffer
 * at its current position.
 * Specifying an undefined <q> yields an error.
 */
State *
StateGetQRegString::got_register(QRegister *reg)
{
	gchar *str;

	BEGIN_EXEC(&States::start);

	str = reg->get_string();
	if (*str) {
		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_ADDTEXT, strlen(str), (sptr_t)str);
		interface.ssm(SCI_SCROLLCARET);
		interface.ssm(SCI_ENDUNDOACTION);
		ring.dirtify();

		interface.undo_ssm(SCI_UNDO);
	}
	g_free(str);

	return &States::start;
}

/*$ U Uq
 * nUq -- Set Q-Register integer
 * -Uq
 * [n]:Uq -> Success|Failure
 *
 * Sets the integer-part of Q-Register <q> to <n>.
 * \(lq-U\(rq is equivalent to \(lq-1U\(rq, otherwise
 * the command fails if <n> is missing.
 *
 * If the command is colon-modified, it returns a success
 * boolean if <n> or \(lq-\(rq is given.
 * Otherwise it returns a failure boolean and does not
 * modify <q>.
 *
 * The register is defined if it does not exist.
 */
State *
StateSetQRegInteger::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	if (expressions.args() || expressions.num_sign < 0) {
		reg->undo_set_integer();
		reg->set_integer(expressions.pop_num_calc());

		if (eval_colon())
			expressions.push(SUCCESS);
	} else if (eval_colon()) {
		expressions.push(FAILURE);
	} else {
		throw ArgExpectedError('U');
	}

	return &States::start;
}

/*$ % %q increment
 * [n]%q -> q+n -- Increase Q-Register integer
 *
 * Add <n> to the integer part of register <q>, returning
 * its new value.
 * <q> will be defined if it does not exist.
 */
State *
StateIncreaseQReg::got_register(QRegister *reg)
{
	tecoInt res;

	BEGIN_EXEC(&States::start);

	reg->undo_set_integer();
	res = reg->get_integer() + expressions.pop_num_calc();
	expressions.push(reg->set_integer(res));

	return &States::start;
}

/*$ M Mq eval
 * Mq -- Execute macro
 * :Mq
 *
 * Execute macro stored in string of Q-Register <q>.
 * The command itself does not push or pop and arguments from the stack
 * but the macro executed might well do so.
 * The new macro invocation level will contain its own go-to label table
 * and local Q-Register table.
 * Except when the command is colon-modified - in this case, local
 * Q-Registers referenced in the macro refer to the parent macro-level's
 * local Q-Register table (or whatever level defined one last).
 *
 * Errors during the macro execution will propagate to the M command.
 * In other words if a command in the macro fails, the M command will fail
 * and this failure propagates until the top-level macro (e.g.
 * the command-line macro).
 *
 * Note that the string of <q> will be copied upon macro execution,
 * so subsequent changes to Q-Register <q> from inside the macro do
 * not modify the executed code.
 */
State *
StateMacro::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	reg->execute(!eval_colon());

	return &States::start;
}

/*$ EM
 * EMfile$ -- Execute macro from file
 * :EMfile$
 *
 * Read the file with name <file> into memory and execute its contents
 * as a macro.
 * It is otherwise similar to the \(lqM\(rq command.
 *
 * If <file> could not be read, the command yields an error.
 */
State *
StateMacroFile::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	Execute::file(filename, !eval_colon());

	return &States::start;
}

/*$ X Xq
 * [lines]Xq -- Copy into or append to Q-Register
 * -Xq
 * from,toXq
 * [lines]:Xq
 * -:Xq
 * from,to:Xq
 *
 * Copy the next or previous number of <lines> from the buffer
 * into the Q-Register <q> string.
 * If <lines> is omitted, the sign prefix is implied.
 * If two arguments are specified, the characters beginning
 * at position <from> up to the character at position <to>
 * are copied.
 * The semantics of the arguments is analogous to the K
 * command's arguments.
 * If the command is colon-modified, the characters will be
 * appended to the end of register <q> instead.
 *
 * Register <q> will be created if it is undefined.
 */
State *
StateCopyToQReg::got_register(QRegister *reg)
{
	tecoInt from, len;
	Sci_TextRange tr;

	BEGIN_EXEC(&States::start);
	expressions.eval();

	if (expressions.args() <= 1) {
		from = interface.ssm(SCI_GETCURRENTPOS);
		sptr_t line = interface.ssm(SCI_LINEFROMPOSITION, from) +
			      expressions.pop_num_calc();

		if (!Validate::line(line))
			throw RangeError("X");

		len = interface.ssm(SCI_POSITIONFROMLINE, line) - from;

		if (len < 0) {
			from += len;
			len *= -1;
		}
	} else {
		tecoInt to = expressions.pop_num();
		from = expressions.pop_num();

		len = to - from;

		if (len < 0 || !Validate::pos(from) || !Validate::pos(to))
			throw RangeError("X");
	}

	tr.chrg.cpMin = from;
	tr.chrg.cpMax = from + len;
	tr.lpstrText = (char *)g_malloc(len + 1);
	interface.ssm(SCI_GETTEXTRANGE, 0, (sptr_t)&tr);

	if (eval_colon()) {
		reg->undo_append_string();
		reg->append_string(tr.lpstrText);
	} else {
		reg->undo_set_string();
		reg->set_string(tr.lpstrText);
	}
	g_free(tr.lpstrText);

	return &States::start;
}

} /* namespace SciTECO */
