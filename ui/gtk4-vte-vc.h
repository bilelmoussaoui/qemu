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
#ifndef GTK_VTE_VC_H
#define GTK_VTE_VC_H

#include "qemu/osdep.h"
#include "qemu/main-loop.h"

#include "trace.h"
#include "ui/console.h"
#include "ui/gtk4.h"
#include "ui/egl-helpers.h"
#include "chardev/char.h"

#include "sysemu/sysemu.h"


#define VIRTUAL_CONSOLE_VTE_WIDGET_TYPE (virtual_console_vte_widget_get_type())
#define VIRTUAL_CONSOLE_IS_VTE_WIDGET(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VIRTUAL_CONSOLE_VTE_WIDGET_TYPE))

G_DECLARE_FINAL_TYPE(VirtualConsoleVteWidget, virtual_console_vte_widget,
                     VIRTUAL_CONSOLE_VTE, WIDGET, GtkWidget)

GtkWidget *virtual_console_vte_widget_new(VirtualConsole *vc);
void virtual_console_vte_widget_copy(VirtualConsoleVteWidget *self);

void
virtual_console_vte_widget_get_size(VirtualConsoleVteWidget *self, int *width, int *height);


void vte_vc_type_register(void);

struct VCChardev {
    Chardev parent;
    VirtualConsoleVteWidget *widget;
    bool echo;
};
typedef struct VCChardev VCChardev;

#define TYPE_CHARDEV_VC "chardev-vc"
DECLARE_INSTANCE_CHECKER(VCChardev, VC_CHARDEV,
                         TYPE_CHARDEV_VC)
#endif
