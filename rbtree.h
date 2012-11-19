#ifndef __RBTREE_H
#define __RBTREE_H

#include <bsd/sys/tree.h>

#include <glib.h>

#include "undo.h"

class RBTree {
public:
	class RBEntry;

private:
	RB_HEAD(Tree, RBEntry) head;

	RB_PROTOTYPE_INTERNAL(Tree, RBEntry, nodes, /* unused */, static);

public:
	class RBEntry {
	public:
		RB_ENTRY(RBEntry) nodes;

		virtual ~RBEntry() {}

		inline RBEntry *
		next(void)
		{
			return RBTree::Tree_RB_NEXT(this);
		}
		inline RBEntry *
		prev(void)
		{
			return RBTree::Tree_RB_PREV(this);
		}

		virtual int operator <(RBEntry &entry) = 0;
	};

private:
	static inline int
	compare_entries(RBEntry *e1, RBEntry *e2)
	{
		return *e1 < *e2;
	}

public:
	RBTree()
	{
		RB_INIT(&head);
	}
	virtual
	~RBTree()
	{
		clear();
	}

	inline RBEntry *
	insert(RBEntry *entry)
	{
		RB_INSERT(Tree, &head, entry);
		return entry;
	}
	inline RBEntry *
	remove(RBEntry *entry)
	{
		return RB_REMOVE(Tree, &head, entry);
	}
	inline RBEntry *
	find(RBEntry *entry)
	{
		return RB_FIND(Tree, &head, entry);
	}
	inline RBEntry *
	nfind(RBEntry *entry)
	{
		return RB_NFIND(Tree, &head, entry);
	}
	inline RBEntry *
	min(void)
	{
		return RB_MIN(Tree, &head);
	}
	inline RBEntry *
	max(void)
	{
		return RB_MAX(Tree, &head);
	}

	inline void
	clear(void)
	{
		RBEntry *cur;

		while ((cur = min())) {
			remove(cur);
			delete cur;
		}
	}
};

template <typename KeyType, typename ValueType>
class Table : public RBTree {
	class TableEntry : public RBEntry {
	public:
		KeyType		key;
		ValueType	value;

		TableEntry(KeyType &_key, ValueType &_value)
			  : key(_key), value(_value) {}

		int
		operator <(RBEntry &entry)
		{
			return key < ((TableEntry &)entry).key;
		}
	};

public:
	ValueType nil;

	Table(ValueType &_nil) : RBTree(), nil(_nil) {}

	inline bool
	hasEntry(KeyType &key)
	{
		return find(&TableEntry(key, nil)) != NULL;
	}

	ValueType &
	operator [](KeyType &key)
	{
		TableEntry *entry = new TableEntry(key, nil);
		TableEntry *existing = (TableEntry *)find(entry);

		if (existing)
			delete entry;
		else
			existing = (TableEntry *)insert(entry);

		return existing->value;
	}

	inline void
	remove(KeyType &key)
	{
		TableEntry entry(key, nil);
		TableEntry *existing = (TableEntry *)find(&entry);

		if (existing)
			RBTree::remove(existing);
	}

#if 0
	void
	dump(void)
	{
		RBEntry *cur;

		RB_FOREACH(cur, Tree, &head)
			g_printf("tree[\"%s\"] = %d\n", cur->name, cur->pc);
		g_printf("---END---\n");
	}
#endif

	void
	clear(void)
	{
		RBEntry *cur;

		while ((cur = min())) {
			RBTree::remove(cur);
			delete cur;
		}
	}
};

class CString {
public:
	gchar *str;

	CString(const gchar *_str) : str(g_strdup(_str)) {}
	~CString()
	{
		g_free(str);
	}

	inline int
	operator <(CString &obj)
	{
		return (int)g_strcmp0(str, obj.str);
	}
};

template <typename ValueType>
class StringTable : public Table<CString, ValueType> {
public:
	StringTable(ValueType &nil) : Table<CString, ValueType>(nil) {}

	inline bool
	hasEntry(const gchar *key)
	{
		CString str(key);
		return Table<CString, ValueType>::hasEntry(str);
	}

	inline ValueType &
	operator [](const gchar *key)
	{
		CString str(key);
		return (Table<CString, ValueType>::operator [])(str);
	}

	inline void
	remove(const gchar *key)
	{
		CString str(key);
		Table<CString, ValueType>::remove(str);
	}
};

template <typename ValueType>
class StringTableUndo : public StringTable<ValueType> {
	class UndoTokenSet : public UndoToken {
		StringTableUndo *table;

		CString		name;
		ValueType	value;

	public:
		UndoTokenSet(StringTableUndo *_table, CString &_name, ValueType &_value)
			    : table(_table), name(_name), value(_value) {}

		void
		run(void)
		{
			table->Table<CString, ValueType>::operator [](name) = value;
			name.str = NULL;
#if 0
			table->dump();
#endif
		}
	};

	class UndoTokenRemove : public UndoToken {
		StringTableUndo *table;

		CString name;

	public:
		UndoTokenRemove(StringTableUndo *_table, CString &_name)
			       : table(_table), name(_name) {}

		void
		run(void)
		{
			table->Table<CString, ValueType>::remove(name);
#if 0
			table->dump();
#endif
		}
	};

public:
	StringTableUndo(ValueType &nil) : StringTable<ValueType>(nil) {}

	void
	set(const gchar *key, ValueType &value)
	{
		ValueType &old = (StringTable<ValueType>::operator [])(key);
		CString str(key);

		if (old == StringTable<ValueType>::nil)
			undo.push(new UndoTokenRemove(this, str));
		else
			undo.push(new UndoTokenSet(this, str, old));

		old = value;
	}
};

#endif
