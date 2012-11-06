#ifndef __SCITECO_H
#define __SCITECO_H

#include <stdarg.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <Scintilla.h>

#include "undo.h"

extern gchar *cmdline;
extern gint macro_pc;

void message_display(GtkMessageType type, const gchar *fmt, ...);

void cmdline_keypress(gchar key);
void cmdline_display(const gchar *cmdline);

void editor_msg(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0);

gboolean macro_execute(const gchar *macro);

#define IS_CTL(C)	((C) < ' ')
#define CTL_ECHO(C)	((C) | 0x40)

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

#endif