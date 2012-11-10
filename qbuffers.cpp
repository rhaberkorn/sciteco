#include <bsd/sys/queue.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "gtk-info-popup.h"

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "qbuffers.h"

Ring ring;

bool
Buffer::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	edit();
	editor_msg(SCI_CLEARALL);

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;
	editor_msg(SCI_APPENDTEXT, size, (sptr_t)contents);
	g_free(contents);

	editor_msg(SCI_GOTOPOS, 0);
	editor_msg(SCI_SETSAVEPOINT);

	set_filename(filename);

	return true;
}

void
Buffer::close(void)
{
	LIST_REMOVE(this, buffers);

	if (filename)
		message_display(GTK_MESSAGE_INFO,
				"Removed file \"%s\" from the ring",
				filename);
	else
		message_display(GTK_MESSAGE_INFO,
				"Removed unnamed file from the ring.");
}

Buffer *
Ring::find(const gchar *filename)
{
	Buffer *cur;

	LIST_FOREACH(cur, &head, buffers)
		if (!g_strcmp0(cur->filename, filename))
			return cur;

	return NULL;
}

bool
Ring::edit(const gchar *filename)
{
	bool new_in_ring = false;
	Buffer *buffer = find(filename);

	if (current)
		current->dot = editor_msg(SCI_GETCURRENTPOS);

	if (buffer) {
		buffer->edit();
	} else {
		new_in_ring = true;

		buffer = new Buffer();
		LIST_INSERT_HEAD(&head, buffer, buffers);

		if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
			buffer->load(filename);

			message_display(GTK_MESSAGE_INFO,
					"Added file \"%s\" to ring", filename);
		} else {
			buffer->edit();
			buffer->set_filename(filename);

			if (filename)
				message_display(GTK_MESSAGE_INFO,
						"Added new file \"%s\" to ring",
						filename);
			else
				message_display(GTK_MESSAGE_INFO,
						"Added new unnamed file to ring.");
		}
	}

	current = buffer;

	return new_in_ring;
}

void
Ring::close(void)
{
	Buffer *buffer = current;

	buffer->close();
	current = buffer->next() ? : first();
	if (!current)
		edit(NULL);

	delete buffer;
}

Ring::~Ring()
{
	Buffer *buffer, *next;

	LIST_FOREACH_SAFE(buffer, &head, buffers, next)
		delete buffer;
}

/*
 * Command states
 */

void
StateFile::do_edit(const gchar *filename)
{
	ring.undo_edit();
	if (ring.edit(filename))
		ring.undo_close();
}

void
StateFile::initial(void)
{
	gint64 id = expressions.pop_num_calc(1, -1);

	if (id == 0) {
		for (Buffer *cur = ring.first(); cur; cur = cur->next())
			gtk_info_popup_add_filename(filename_popup,
						    GTK_INFO_POPUP_FILE,
						    cur->filename ? : "(Unnamed)",
						    cur == ring.current);

		gtk_widget_show(GTK_WIDGET(filename_popup));
	}
}

State *
StateFile::done(const gchar *str)
{
	BEGIN_EXEC(&states.start);

	if (is_glob_pattern(str)) {
		gchar *dirname;
		GDir *dir;

		dirname = g_path_get_dirname(str);
		dir = g_dir_open(dirname, 0, NULL);

		if (dir) {
			const gchar *basename;
			GPatternSpec *pattern;

			basename = g_path_get_basename(str);
			pattern = g_pattern_spec_new(basename);
			g_free((gchar *)basename);

			while ((basename = g_dir_read_name(dir))) {
				if (g_pattern_match_string(pattern, basename)) {
					gchar *filename;

					filename = g_build_filename(dirname,
								    basename,
								    NULL);
					do_edit(filename);
					g_free(filename);
				}
			}

			g_pattern_spec_free(pattern);
			g_dir_close(dir);
		}

		g_free(dirname);
	} else {
		do_edit(*str ? str : NULL);
	}

	return &states.start;
}
