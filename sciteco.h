#ifndef __SCITECO_H
#define __SCITECO_H

#include <stdarg.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <Scintilla.h>

extern gchar *cmdline;

void message_display(GtkMessageType type,
		     const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

void cmdline_keypress(gchar key);
void cmdline_display(const gchar *cmdline);

sptr_t editor_msg(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0);

#define IS_CTL(C)	((C) < ' ')
#define CTL_ECHO(C)	((C) | 0x40)

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

#endif