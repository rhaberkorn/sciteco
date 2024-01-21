/*
 * Copyright (C) 2012-2024 Robin Haberkorn
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

typedef struct teco_stailq_entry_t {
	struct teco_stailq_entry_t *next;
} teco_stailq_entry_t;

typedef struct {
	/** Pointer to the first element or NULL */
	teco_stailq_entry_t *first;
	/** Pointer to the last element's `next` field or the head's `first` field */
	teco_stailq_entry_t **last;
} teco_stailq_head_t;

#define TECO_STAILQ_HEAD_INITIALIZER(HEAD) ((teco_stailq_head_t){NULL, &(HEAD)->first})

static inline void
teco_stailq_insert_tail(teco_stailq_head_t *head, teco_stailq_entry_t *entry)
{
	entry->next = NULL;
	*head->last = entry;
	head->last = &entry->next;
}

static inline teco_stailq_entry_t *
teco_stailq_remove_head(teco_stailq_head_t *head)
{
	teco_stailq_entry_t *first = head->first;
	if (first && !(head->first = first->next))
		head->last = &head->first;
	return first;
}

/** Can be both a tail queue head or an entry (tail queue element). */
typedef union teco_tailq_entry_t {
	struct {
		/** Pointer to the next entry or NULL */
		union teco_tailq_entry_t *next;
		/** Pointer to the previous entry or to the queue head */
		union teco_tailq_entry_t *prev;
	};

	struct {
		/** Pointer to the first entry or NULL */
		union teco_tailq_entry_t *first;
		/** Pointer to the last entry or to the queue head */
		union teco_tailq_entry_t *last;
	};
} teco_tailq_entry_t;

#define TECO_TAILQ_HEAD_INITIALIZER(HEAD) ((teco_tailq_entry_t){.first = NULL, .last = (HEAD)})

static inline void
teco_tailq_insert_before(teco_tailq_entry_t *entry_a, teco_tailq_entry_t *entry_b)
{
	entry_b->prev = entry_a->prev;
	entry_b->next = entry_a;
	entry_a->prev->next = entry_b;
	entry_a->prev = entry_b;
}

static inline void
teco_tailq_insert_tail(teco_tailq_entry_t *head, teco_tailq_entry_t *entry)
{
	entry->next = NULL;
	entry->prev = head->last;
	head->last->next = entry;
	head->last = entry;
}

static inline void
teco_tailq_remove(teco_tailq_entry_t *head, teco_tailq_entry_t *entry)
{
	if (entry->next)
		entry->next->prev = entry->prev;
	else
		head->last = entry->prev;
	entry->prev->next = entry->next;
}
