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

#ifndef __RBTREE_H
#define __RBTREE_H

#include <string.h>

#include <bsd/sys/tree.h>

#include <glib.h>
#include <glib/gprintf.h>

namespace SciTECO {

template <class RBEntryType>
class RBTree {
public:
	class RBEntry {
	public:
		RB_ENTRY(RBEntry) nodes;

		inline RBEntryType *
		next(void)
		{
			return (RBEntryType *)RBTree::Tree_RB_NEXT(this);
		}
		inline RBEntryType *
		prev(void)
		{
			return (RBEntryType *)RBTree::Tree_RB_PREV(this);
		}
	};

private:
	RB_HEAD(Tree, RBEntry) head;

	static inline int
	compare_entries(RBEntry *e1, RBEntry *e2)
	{
		return ((RBEntryType *)e1)->compare(*(RBEntryType *)e2);
	}

	/*
	 * All generated functions are plain-C, so they can be
	 * static methods.
	 */
	RB_GENERATE_INTERNAL(Tree, RBEntry, nodes, compare_entries, static);

public:
	RBTree()
	{
		RB_INIT(&head);
	}
	~RBTree()
	{
		/*
		 * Keeping the clean up out of this wrapper class
		 * means we can avoid declaring EBEntry implementations
		 * virtual.
		 */
		g_assert(min() == NULL);
	}

	inline RBEntryType *
	insert(RBEntryType *entry)
	{
		RB_INSERT(Tree, &head, entry);
		return entry;
	}
	inline RBEntryType *
	remove(RBEntryType *entry)
	{
		return (RBEntryType *)RB_REMOVE(Tree, &head, entry);
	}
	inline RBEntryType *
	find(RBEntryType *entry)
	{
		return (RBEntryType *)RB_FIND(Tree, &head, entry);
	}
	inline RBEntryType *
	operator [](RBEntryType *entry)
	{
		return find(entry);
	}
	inline RBEntryType *
	nfind(RBEntryType *entry)
	{
		return (RBEntryType *)RB_NFIND(Tree, &head, entry);
	}
	inline RBEntryType *
	min(void)
	{
		return (RBEntryType *)RB_MIN(Tree, &head);
	}
	inline RBEntryType *
	max(void)
	{
		return (RBEntryType *)RB_MAX(Tree, &head);
	}
};

typedef gint (*StringCmpFunc)(const gchar *str1, const gchar *str2);
typedef gint (*StringNCmpFunc)(const gchar *str1, const gchar *str2, gsize n);

template <StringCmpFunc StringCmp>
class RBEntryStringT : public RBTree<RBEntryStringT<StringCmp>>::RBEntry {
public:
	/*
	 * It is convenient to be able to access the string
	 * key with various attribute names.
	 */
	union {
		gchar *key;
		gchar *name;
	};

	RBEntryStringT(gchar *_key) : key(_key) {}

	inline gint
	compare(RBEntryStringT &other)
	{
		return StringCmp(key, other.key);
	}
};

template <StringCmpFunc StringCmp, StringNCmpFunc StringNCmp>
class RBTreeStringT : public RBTree<RBEntryStringT<StringCmp>> {
public:
	typedef RBEntryStringT<StringCmp> RBEntryString;

	class RBEntryOwnString : public RBEntryString {
	public:
		RBEntryOwnString(const gchar *key = NULL)
		                : RBEntryString(key ? g_strdup(key) : NULL) {}

		~RBEntryOwnString()
		{
			g_free(RBEntryString::key);
		}
	};

	inline RBEntryString *
	find(const gchar *str)
	{
		RBEntryString entry((gchar *)str);
		return RBTree<RBEntryString>::find(&entry);
	}
	inline RBEntryString *
	operator [](const gchar *name)
	{
		return find(name);
	}

	inline RBEntryString *
	nfind(const gchar *str)
	{
		RBEntryString entry((gchar *)str);
		return RBTree<RBEntryString>::nfind(&entry);
	}

	gchar *auto_complete(const gchar *key, gchar completed = '\0',
	                     gsize restrict_len = 0);
};

typedef RBTreeStringT<strcmp, strncmp> RBTreeString;
typedef RBTreeStringT<g_ascii_strcasecmp, g_ascii_strncasecmp> RBTreeStringCase;

/*
 * Only these two instantiations of RBTreeStringT are ever used,
 * so it is more efficient to explicitly instantiate them.
 * NOTE: The insane rules of C++ prevent using the typedefs here...
 */
extern template class RBTreeStringT<strcmp, strncmp>;
extern template class RBTreeStringT<g_ascii_strcasecmp, g_ascii_strncasecmp>;

} /* namespace SciTECO */

#endif
