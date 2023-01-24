/*
 * GTK 4 UI backend based on the GTK 3 backend implementation
 *
 * Copyright Red Hat, Corp. 2023
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori     <aliguori@us.ibm.com>
 *  Bilal Elmoussaoui   <belmous@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UI_GTK4_H
#define UI_GTK4_H

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/wayland/gdkwayland.h>
#include <gdk/x11/gdkx.h>


#include "ui/console.h"
#include "ui/kbd-state.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"

#define MAX_VCS 10

extern int nb_vcs;

typedef struct GtkDisplayState GtkDisplayState;

typedef struct VirtualGfxConsole {
    DisplayGLCtx dgc;
    DisplayChangeListener dcl;
    GdkGLContext *context;
    QKbdState *kbd;
    DisplaySurface *ds;
    QemuGLShader *gls;
    int glupdates;
    int x, y, w, h;
    egl_fb guest_fb;
    egl_fb win_fb;
    egl_fb cursor_fb;
    int cursor_x;
    int cursor_y;
    bool y0_top;
    bool scanout_mode;
    bool has_dmabuf;
} VirtualGfxConsole;

typedef struct VirtualConsole {
    GtkDisplayState *s;
    char *label;
    GtkWidget *window;
    GtkWidget *widget;
    /* Used to remember in which position the page was added to GtkNotebook */
    int index;
    VirtualGfxConsole gfx;
} VirtualConsole;

struct GtkDisplayState {
    GtkApplication *app;
    GSimpleActionGroup *actions;
    GtkWidget *menubar;
    GtkWidget *window;

    GMenu *vc_menu;

    int nb_vcs;
    VirtualConsole vc[MAX_VCS];

    GtkWidget *notebook;
    gboolean last_set;
    int last_x;
    int last_y;
    double grab_x_root;
    double grab_y_root;
    VirtualConsole *kbd_owner;
    VirtualConsole *ptr_owner;

    GdkCursor *null_cursor;
    Notifier mouse_mode_notifier;

    DisplayOptions *opts;
};

/* ui/gtk4.c */
void gd_update_windowsize(VirtualConsole *vc);
void gd_update_monitor_refresh_rate(VirtualConsole *vc);
int gd_map_keycode(int scancode);
void gd_set_ui_size(VirtualConsole *vc, int width, int height);

void gd_ungrab_pointer(GtkDisplayState *s);
void gd_grab_pointer(VirtualConsole *vc, const char *reason);

#endif /* UI_GTK4_H */
