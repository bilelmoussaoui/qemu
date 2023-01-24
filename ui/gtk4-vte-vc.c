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

#include <gtk/gtk.h>
#include <vte/vte.h>

#include "gtk4-vte-vc.h"

#include "qapi/error.h"
#include "qemu/fifo8.h"
#include "qemu/main-loop.h"
#include "qemu/osdep.h"

#include "sysemu/sysemu.h"

#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/gtk4.h"

#define VC_TERM_X_MIN     80
#define VC_TERM_Y_MIN     25

static int vcs_idx;
int nb_vcs;
static Chardev *vcs[MAX_VCS];

struct _VirtualConsoleVteWidget {
    GtkWidget parent_instance;
    GtkWidget *terminal;

    Chardev *chr;
    Fifo8 out_fifo;
    bool echo;
    VirtualConsole *vc;
};

enum {
  PROP_0,

  PROP_VC,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { 0 };

G_DEFINE_TYPE(VirtualConsoleVteWidget, virtual_console_vte_widget,
              GTK_TYPE_WIDGET);

/** Terminal callbacks **/

static void gd_vc_send_chars(VirtualConsoleVteWidget *widget)
{
    uint32_t len, avail;

    len = qemu_chr_be_can_write(widget->chr);
    avail = fifo8_num_used(&widget->out_fifo);
    while (len > 0 && avail > 0) {
        const uint8_t *buf;
        uint32_t size;

        buf = fifo8_pop_buf(&widget->out_fifo, MIN(len, avail), &size);
        qemu_chr_be_write(widget->chr, buf, size);
        len = qemu_chr_be_can_write(widget->chr);
        avail -= size;
    }
}

static int gd_vc_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsoleVteWidget *widget = vcd->widget;

    vte_terminal_feed(VTE_TERMINAL(widget->terminal), (const char *)buf, len);
    return len;
}

static void gd_vc_chr_accept_input(Chardev *chr)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsoleVteWidget *widget = vcd->widget;

    gd_vc_send_chars(widget);
}

static void gd_vc_chr_set_echo(Chardev *chr, bool echo)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsoleVteWidget *widget = vcd->widget;

    if (widget) {
        widget->echo = echo;
    } else {
        vcd->echo = echo;
    }
}

static void gd_vc_open(Chardev *chr,
                       ChardevBackend *backend,
                       bool *be_opened,
                       Error **errp)
{
    if (nb_vcs == MAX_VCS) {
        error_setg(errp, "Maximum number of consoles reached");
        return;
    }

    vcs[nb_vcs++] = chr;
    /*
     * console/chardev init sometimes completes elsewhere in a 2nd
     * stage, so defer OPENED events until they are fully initialized
     */
    *be_opened = false;
}

static gboolean gd_vc_in(VteTerminal *terminal, gchar *text, guint size,
                         VirtualConsoleVteWidget *widget)
{
    uint32_t free;

    if (widget->echo) {
        VteTerminal *term = VTE_TERMINAL(widget->terminal);
        int i;
        for (i = 0; i < size; i++) {
            uint8_t c = text[i];
            if (c >= 128 || isprint(c)) {
                /* 8-bit characters are considered printable.  */
                vte_terminal_feed(term, &text[i], 1);
            } else if (c == '\r' || c == '\n') {
                vte_terminal_feed(term, "\r\n", 2);
            } else {
                char ctrl[2] = { '^', 0};
                ctrl[1] = text[i] ^ 64;
                vte_terminal_feed(term, ctrl, 2);
            }
        }
    }

    free = fifo8_num_free(&widget->out_fifo);
    fifo8_push_all(&widget->out_fifo, (uint8_t *)text, MIN(free, size));
    gd_vc_send_chars(widget);

    return TRUE;
}

/** Event Controllers handlers **/

/**
 * Keydown event controller handler
 */
static gboolean gd_text_key_down(GtkEventControllerKey *controller,
                                 guint keyval, guint keycode,
                                 GdkModifierType state,
                                 VirtualConsole *vc)
{
    QemuConsole *con = vc->gfx.dcl.con;

    if (keyval == GDK_KEY_Delete) {
        kbd_put_qcode_console(con, Q_KEY_CODE_DELETE, false);
    } else {
        int qcode = gd_map_keycode(keycode);
        kbd_put_qcode_console(con, qcode, false);
    }
    return TRUE;
}


static void
virtual_console_vte_widget_init(VirtualConsoleVteWidget *widget)
{
}


static void
virtual_console_vte_widget_constructed(GObject *object)
{
    char buffer[32];
    GtkWidget *scrolled_window;

    GtkWidget *widget = GTK_WIDGET(object);
    VirtualConsoleVteWidget *self = VIRTUAL_CONSOLE_VTE_WIDGET(object);
    VirtualConsole *vc = self->vc;
    vc->widget = widget;

    Chardev *chr = vcs[vcs_idx];
    VCChardev *vcd = VC_CHARDEV(chr);

    self->terminal = vte_terminal_new();

    self->echo = vcd->echo;
    self->chr = chr;
    fifo8_create(&self->out_fifo, 4096);
    vcd->widget = self;

    snprintf(buffer, sizeof(buffer), "vc%d", vcs_idx);
    vc->label = g_strdup_printf("%s", chr->label ? chr->label : buffer);

    g_signal_connect(self->terminal, "commit", G_CALLBACK(gd_vc_in), self);

    vte_terminal_set_scrollback_lines(VTE_TERMINAL(self->terminal), -1);
    vte_terminal_set_size(VTE_TERMINAL(self->terminal),
                          VC_TERM_X_MIN, VC_TERM_Y_MIN);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);

    scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window),
                                  self->terminal);
    gtk_widget_set_parent(scrolled_window, widget);

    qemu_chr_be_event(chr, CHR_EVENT_OPENED);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(widget, key_controller);

    g_signal_connect(key_controller, "key-pressed",
                        G_CALLBACK(gd_text_key_down), vc);

    vcs_idx++;
    G_OBJECT_CLASS(virtual_console_vte_widget_parent_class)->constructed(object);
}

static void
virtual_console_vte_widget_get_property(GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
    VirtualConsoleVteWidget *self = VIRTUAL_CONSOLE_VTE_WIDGET(object);
    switch (prop_id) {
    case PROP_VC:
        g_value_set_pointer(value, &self->vc);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }

}

static void
virtual_console_vte_widget_set_property(GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
    VirtualConsoleVteWidget *self = VIRTUAL_CONSOLE_VTE_WIDGET(object);

    switch (prop_id) {
    case PROP_VC:
        self->vc =  (VirtualConsole *)g_value_get_pointer(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean
virtual_console_vte_widget_grab_focus(GtkWidget *widget)
{
    VirtualConsoleVteWidget *self = VIRTUAL_CONSOLE_VTE_WIDGET(widget);

    gtk_widget_grab_focus(self->terminal);
    return TRUE;
}

static void
virtual_console_vte_widget_class_init(VirtualConsoleVteWidgetClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

    object_class->constructed = virtual_console_vte_widget_constructed;
    object_class->get_property = virtual_console_vte_widget_get_property;
    object_class->set_property = virtual_console_vte_widget_set_property;

    widget_class->grab_focus = virtual_console_vte_widget_grab_focus;

    props[PROP_VC] =
    g_param_spec_pointer("vc",
                         "Virtual Console",
                         "Associated Virtual Console",
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, props);

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

GtkWidget *virtual_console_vte_widget_new(VirtualConsole *vc)
{
    return g_object_new(VIRTUAL_CONSOLE_VTE_WIDGET_TYPE,
                        "vc", vc,
                        NULL);
}

/**
 * Copy the terminal content to  clipboard.
 */
void
virtual_console_vte_widget_copy(VirtualConsoleVteWidget *self)
{
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(self->terminal),
                                       VTE_FORMAT_TEXT);
}

void
virtual_console_vte_widget_get_size(VirtualConsoleVteWidget *self, int *width, int *height)
{
    *width = vte_terminal_get_char_width(VTE_TERMINAL(self->terminal)) * VC_TERM_X_MIN;
    *height = vte_terminal_get_char_height(VTE_TERMINAL(self->terminal)) * VC_TERM_Y_MIN;
}


static void char_gd_vc_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse = qemu_chr_parse_vc;
    cc->open = gd_vc_open;
    cc->chr_write = gd_vc_chr_write;
    cc->chr_accept_input = gd_vc_chr_accept_input;
    cc->chr_set_echo = gd_vc_chr_set_echo;
}

static const TypeInfo char_gd_vc_type_info = {
    .name = TYPE_CHARDEV_VC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VCChardev),
    .class_init = char_gd_vc_class_init,
};

void vte_vc_type_register(void)
{
    type_register(&char_gd_vc_type_info);
}
