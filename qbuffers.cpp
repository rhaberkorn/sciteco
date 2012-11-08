#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"
#include "parser.h"
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
	current = LIST_NEXT(buffer, buffers) ? : LIST_FIRST(&head);
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

State *
StateFile::done(const gchar *str)
{
	bool new_in_ring;

	BEGIN_EXEC(&states.start);

	ring.undo_edit();
	new_in_ring = ring.edit(*str ? str : NULL);
	if (new_in_ring)
		ring.undo_close();

	return &states.start;
}
