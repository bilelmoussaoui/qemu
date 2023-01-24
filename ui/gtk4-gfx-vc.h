/*
 * GTK 4 UI backend
 *
 * Virtual Console widget.
 *
 * Copyright Red Hat, Corp. 2023
 *
 * Authors:
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
#ifndef GTK_GFX_VC_H
#define GTK_GFX_VC_H

#include "qemu/osdep.h"
#include "qemu/main-loop.h"

#include "trace.h"
#include "ui/console.h"
#include "ui/gtk4.h"
#include "ui/egl-helpers.h"

#include "sysemu/sysemu.h"


#define VIRTUAL_CONSOLE_GFX_WIDGET_TYPE (virtual_console_gfx_widget_get_type())
#define VIRTUAL_CONSOLE_IS_GFX_WIDGET(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VIRTUAL_CONSOLE_GFX_WIDGET_TYPE))

G_DECLARE_FINAL_TYPE(VirtualConsoleGfxWidget, virtual_console_gfx_widget,
                     VIRTUAL_CONSOLE_GFX, WIDGET, GtkWidget)

GtkWidget *virtual_console_gfx_widget_new(VirtualConsole *vc, QemuConsole *con);
void virtual_console_gfx_widget_set_free_scale(VirtualConsoleGfxWidget *self,
                                               gboolean free_scale);

void virtual_console_gfx_widget_zoom_in(VirtualConsoleGfxWidget *self);
void virtual_console_gfx_widget_zoom_out(VirtualConsoleGfxWidget *self);
void virtual_console_gfx_widget_reset_zoom(VirtualConsoleGfxWidget *self);

void
virtual_console_gfx_widget_get_size(VirtualConsoleGfxWidget *self, int *width, int *height);

#endif
