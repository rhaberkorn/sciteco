/*
 * Copyright (C) 2012-2021 Robin Haberkorn
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

/*
 * NOTE: Must be included only once.
 */
//#include <rb3ptr.h>

#include "sciteco.h"
#include "interface.h"
#include "string-utils.h"
#include "rb3str.h"

static gint
teco_rb3str_cmp(const teco_rb3str_head_t *head, const teco_string_t *data)
{
	return teco_string_cmp(&head->key, data->data, data->len);
}

static gint
teco_rb3str_casecmp(const teco_rb3str_head_t *head, const teco_string_t *data)
{
	return teco_string_casecmp(&head->key, data->data, data->len);
}

/** @memberof teco_rb3str_tree_t */
teco_rb3str_head_t *
teco_rb3str_insert(teco_rb3str_tree_t *tree, gboolean case_sensitive, teco_rb3str_head_t *head)
{
	rb3_cmp *cmp = case_sensitive ? (rb3_cmp *)teco_rb3str_cmp : (rb3_cmp *)teco_rb3str_casecmp;
	return (teco_rb3str_head_t *)rb3_insert(tree, &head->head, cmp, &head->key);
}

/** @memberof teco_rb3str_tree_t */
teco_rb3str_head_t *
teco_rb3str_find(teco_rb3str_tree_t *tree, gboolean case_sensitive, const gchar *str, gsize len)
{
	rb3_cmp *cmp = case_sensitive ? (rb3_cmp *)teco_rb3str_cmp : (rb3_cmp *)teco_rb3str_casecmp;
	teco_string_t data = {(gchar *)str, len};
	return (teco_rb3str_head_t *)rb3_find(tree, cmp, &data);
}

/** @memberof teco_rb3str_tree_t */
teco_rb3str_head_t *
teco_rb3str_nfind(teco_rb3str_tree_t *tree, gboolean case_sensitive, const gchar *str, gsize len)
{
	rb3_cmp *cmp = case_sensitive ? (rb3_cmp *)teco_rb3str_cmp : (rb3_cmp *)teco_rb3str_casecmp;
	teco_string_t data = {(gchar *)str, len};

	/*
	 * This is based on rb3_INLINE_find() in rb3ptr.h.
	 * Alternatively, we might adapt/wrap teco_rb3str_cmp() in order to store
	 * the last element > data.
	 */
	struct rb3_head *parent = rb3_get_base(tree);
	struct rb3_head *res = NULL;
	int dir = RB3_LEFT;
        while (rb3_has_child(parent, dir)) {
                parent = rb3_get_child(parent, dir);
                int r = cmp(parent, &data);
                if (r == 0)
                        return (teco_rb3str_head_t *)parent;
                dir = (r < 0) ? RB3_RIGHT : RB3_LEFT;
		if (dir == RB3_LEFT)
			res = parent;
        }

	return (teco_rb3str_head_t *)res;
}

/**
 * Auto-complete string given the entries of a RB tree.
 *
 * @param tree The RB tree (root).
 * @param case_sensitive Whether to match case-sensitive.
 * @param str String to complete (not necessarily null-terminated).
 * @param str_len Length of characters in `str`.
 * @param restrict_len Limit completions to this size.
 * @param insert String to set with characters that can be autocompleted.
 * @return TRUE if the completion was unambiguous, else FALSE.
 *
 * @memberof teco_rb3str_tree_t
 */
gboolean
teco_rb3str_auto_complete(teco_rb3str_tree_t *tree, gboolean case_sensitive,
                          const gchar *str, gsize str_len, gsize restrict_len, teco_string_t *insert)
{
	memset(insert, 0, sizeof(*insert));

	teco_string_diff_t diff = case_sensitive ? teco_string_diff : teco_string_casediff;
	teco_rb3str_head_t *first = NULL;
	gsize prefix_len = 0;
	guint prefixed_entries = 0;

	for (teco_rb3str_head_t *cur = teco_rb3str_nfind(tree, case_sensitive, str, str_len);
	     cur && cur->key.len >= str_len && diff(&cur->key, str, str_len) == str_len;
	     cur = teco_rb3str_get_next(cur)) {
		if (restrict_len && cur->key.len != restrict_len)
			continue;

		if (G_UNLIKELY(!first)) {
			first = cur;
			prefix_len = cur->key.len - str_len;
		} else {
			gsize len = diff(&cur->key, first->key.data, first->key.len) - str_len;
			if (len < prefix_len)
				prefix_len = len;
		}

		prefixed_entries++;
	}

	if (prefix_len > 0) {
		teco_string_init(insert, first->key.data + str_len, prefix_len);
	} else if (prefixed_entries > 1) {
		for (teco_rb3str_head_t *cur = first;
		     cur && cur->key.len >= str_len && diff(&cur->key, str, str_len) == str_len;
		     cur = teco_rb3str_get_next(cur)) {
			if (restrict_len && cur->key.len != restrict_len)
				continue;

			teco_interface_popup_add(TECO_POPUP_PLAIN,
			                         cur->key.data, cur->key.len, FALSE);
		}

		teco_interface_popup_show();
	}

	return prefixed_entries == 1;
}
