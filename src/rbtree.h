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

#endif
