#ifndef __SCITECO_H
#define __SCITECO_H

#include <stdarg.h>

#include <glib.h>
#include <gtk/gtk.h>
#include "gtk-info-popup.h"

#include <Scintilla.h>

extern gchar *cmdline;
extern bool quit_requested;

extern GtkInfoPopup *filename_popup;

void message_display(GtkMessageType type,
		     const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

void cmdline_keypress(gchar key);
void cmdline_display(const gchar *cmdline);

sptr_t editor_msg(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0);

#define IS_CTL(C)	((C) < ' ')
#define CTL_ECHO(C)	((C) | 0x40)
#define CTL_KEY(C)	((C) & ~0x40)

#define SUCCESS		(-1)
#define FAILURE		(0)

#define IS_SUCCESS(X)	((X) < 0)
#define IS_FAILURE(X)	(!IS_SUCCESS(X))

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

#endif