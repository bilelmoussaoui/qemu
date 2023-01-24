/*
 * GTK 4 UI backend based on the GTK 3 backend implementation
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"

#include "ui/console.h"
#include "ui/gtk4.h"
#include "gtk4-gfx-vc.h"

#include <glib/gi18n.h>
#include <locale.h>
#if defined(CONFIG_VTE4)
#include "gtk4-vte-vc.h"
#endif
#include <math.h>

#include "trace.h"
#include "qemu/cutils.h"
#include "ui/input.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "keymaps.h"
#include "qom/object.h"

#define VC_WINDOW_X_MIN  320
#define VC_WINDOW_Y_MIN  240

static const guint16 *keycode_map;
static size_t keycode_maplen;

static gboolean gtkinit;

static void setup_actions(GtkDisplayState *ds);


/** Utility Functions **/

void gd_set_ui_size(VirtualConsole *vc, int width, int height)
{
    QemuUIInfo info;

    info = *dpy_get_ui_info(vc->gfx.dcl.con);
    info.width = width;
    info.height = height;
    dpy_set_ui_info(vc->gfx.dcl.con, &info, true);
}

/**
 * Find the virtual console by its identifier.
 *
 * Return: (nullable): The virtual console.
 */
static VirtualConsole *gd_vc_find_by_menu(GtkDisplayState *s, const char* idx)
{
    VirtualConsole *vc;
    gint i;

    for (i = 0; i < s->nb_vcs; i++) {
        vc = &s->vc[i];
        if (g_str_equal(vc->label, idx)) {
            return vc;
        }
    }
    return NULL;
}

/**
 * Find the virtual console associated to a specific page.
 *
 * Return: (nullable): The virtual console.
 */
static VirtualConsole *gd_vc_find_by_page(GtkDisplayState *s, gint page)
{
    VirtualConsole *vc;
    gint i, p;

    for (i = 0; i < s->nb_vcs; i++) {
        vc = &s->vc[i];
        p = gtk_notebook_page_num(GTK_NOTEBOOK(s->notebook), vc->widget);
        if (p == page) {
            return vc;
        }
    }
    return NULL;
}

/**
 * Find the current virtual console of the selected page.
 *
 * Return: (nullable): The virtual console.
 */
static VirtualConsole *gd_vc_find_current(GtkDisplayState *s)
{
    gint page;

    page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    if (page == -1) {
        return NULL;
    }
    return gd_vc_find_by_page(s, page);
}

/**
 * Retrieve the boolean state of the action.
 */
static bool s_action_get_state(GSimpleAction *action)
{
    return g_variant_get_boolean(g_action_get_state(G_ACTION(action)));
}

/**
 * Set a boolean state of an action
 */
static void s_action_set_state(GSimpleAction *action, gboolean state)
{
    g_simple_action_set_state(action, g_variant_new_boolean(state));
}

/**
 * Update the cursor of a virtual console.
 */
static void gd_update_cursor(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    if (!VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget) ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }

    if (!gtk_widget_get_realized(vc->widget)) {
        return;
    }
    gboolean is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(s->window));

    if (is_fullscreen || qemu_input_is_absolute() || s->ptr_owner == vc) {
        gtk_widget_set_cursor(GTK_WIDGET(vc->widget), s->null_cursor);
    } else {
        gtk_widget_set_cursor(GTK_WIDGET(vc->widget), NULL);
    }
}

/**
 * Update the window title based on whether the machine is paused / events are grabbed.
 */
static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *prefix;
    gchar *title;
    GAction *pause_action;
    const char *grab = "";
    bool is_paused = !runstate_is_running();
    int i;

    if (qemu_name) {
        prefix = g_strdup_printf("QEMU (%s)", qemu_name);
    } else {
        prefix = g_strdup_printf("QEMU");
    }

    if (s->ptr_owner != NULL &&
        s->ptr_owner->window == NULL) {
        grab = _(" - Press Ctrl+Alt+G to release grab");
    }

    if (is_paused) {
        status = _(" [Paused]");
    }
    pause_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "pause");
    s_action_set_state(G_SIMPLE_ACTION(pause_action), is_paused);

    title = g_strdup_printf("%s%s%s", prefix, status, grab);
    gtk_window_set_title(GTK_WINDOW(s->window), title);
    g_free(title);

    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];

        if (!vc->window) {
            continue;
        }
        title = g_strdup_printf("%s: %s%s%s", prefix, vc->label,
                                vc == s->kbd_owner ? " +kbd" : "",
                                vc == s->ptr_owner ? " +ptr" : "");
        gtk_window_set_title(GTK_WINDOW(vc->window), title);
        g_free(title);
    }

    g_free(prefix);
}

void gd_update_windowsize(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    int width = 0;
    int height = 0;
    GtkWindow *geo_window = GTK_WINDOW(vc->window ? vc->window : s->window);

    if (VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget)) {
        virtual_console_gfx_widget_get_size(VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget), 
                                            &width, &height);
    } else {
        virtual_console_vte_widget_get_size(VIRTUAL_CONSOLE_VTE_WIDGET(vc->widget),
                                            &width, &height);
    }
    if (width != 0 && height != 0) {
        gtk_widget_set_size_request(vc->widget, width, height);
    }
    gtk_window_set_default_size(geo_window, VC_WINDOW_X_MIN, VC_WINDOW_Y_MIN);
}

/** QEMU Events **/

static void
on_runstate_change_cb(void *opaque, bool running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
}

static void gd_mouse_mode_change(Notifier *notify, void *data)
{
    GtkDisplayState *s;
    GAction *action;
    int i;

    s = container_of(notify, GtkDisplayState, mouse_mode_notifier);
    /* release the grab at switching to absolute mode */
    if (qemu_input_is_absolute() && s->ptr_owner) {
        if (!s->ptr_owner->window) {
            action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab-input");
            s_action_set_state(G_SIMPLE_ACTION(action), FALSE);
        } else {
            gd_ungrab_pointer(s);
        }
    }
    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];
        gd_update_cursor(vc);
    }
}


static void gd_grab_update(VirtualConsole *vc, bool kbd, bool ptr)
{
    /*
     * GdkDisplay *display = gtk_widget_get_display(vc->widget);
     * GdkSeatCapabilities caps = 0;
     * GdkCursor *cursor = NULL;
     * if (kbd) {
     *     caps |= GDK_SEAT_CAPABILITY_KEYBOARD;
     * }
     * if (ptr) {
     *     caps |= GDK_SEAT_CAPABILITY_ALL_POINTING;
     *     cursor = vc->s->null_cursor;
     * }
     * FIXME: figure out how to replace this with XIGrabDevice
     * if (caps) {
     *     gdk_seat_grab(seat, window, caps, false, cursor,
     *                   NULL, NULL, NULL);
     * } else {
     *     gdk_seat_ungrab(seat);
     * }
     */
}

static GdkDevice *gd_get_pointer(GdkDisplay *dpy)
{
    return gdk_seat_get_pointer(gdk_display_get_default_seat(dpy));
}

void gd_ungrab_pointer(GtkDisplayState *s)
{
    VirtualConsole *vc = s->ptr_owner;

    if (vc == NULL) {
        return;
    }
    s->ptr_owner = NULL;

    gd_grab_update(vc, vc->s->kbd_owner == vc, false);
    /*
     * FIXME: replace with X11 specific APIs maybe?
     * GdkDisplay *display;
     * display = gtk_widget_get_display(vc->widget);
     * gdk_device_warp(gd_get_pointer(display),
     *                 gtk_widget_get_screen(vc->widget),
     *                 vc->s->grab_x_root, vc->s->grab_y_root);
     */
    gd_update_caption(s);
    trace_gtk4_gd_ungrab(vc->label, "ptr");
}

void gd_grab_pointer(VirtualConsole *vc, const char *reason)
{
    GdkDisplay *display = gtk_widget_get_display(vc->widget);

    if (vc->s->ptr_owner) {
        if (vc->s->ptr_owner == vc) {
            return;
        } else {
            gd_ungrab_pointer(vc->s);
        }
    }

    gd_grab_update(vc, vc->s->kbd_owner == vc, true);
    gdk_device_get_surface_at_position(gd_get_pointer(display),
                                    &vc->s->grab_x_root, &vc->s->grab_y_root);
    vc->s->ptr_owner = vc;
    gd_update_caption(vc->s);
    trace_gtk4_gd_grab(vc->label, "ptr", reason);
}

static void gd_ungrab_keyboard(GtkDisplayState *s)
{
    VirtualConsole *vc = s->kbd_owner;

    if (vc == NULL) {
        return;
    }
    s->kbd_owner = NULL;

    gd_grab_update(vc, false, vc->s->ptr_owner == vc);
    gd_update_caption(s);
    trace_gtk4_gd_ungrab(vc->label, "kbd");
}

static void gd_grab_keyboard(VirtualConsole *vc, const char *reason)
{
    if (vc->s->kbd_owner) {
        if (vc->s->kbd_owner == vc) {
            return;
        } else {
            gd_ungrab_keyboard(vc->s);
        }
    }

    gd_grab_update(vc, true, vc->s->ptr_owner == vc);
    vc->s->kbd_owner = vc;
    gd_update_caption(vc->s);
    trace_gtk4_gd_grab(vc->label, "kbd", reason);
}

/** GTK Events **/

static gboolean gd_window_close(GtkWidget *widget, GtkDisplayState *s)
{
    bool allow_close = true;

    if (s->opts->has_window_close && !s->opts->window_close) {
        allow_close = false;
    }

    if (allow_close) {
        qmp_quit(NULL);
        //g_application_quit(G_APPLICATION(s->app));
    }

    return TRUE;
}

void gd_update_monitor_refresh_rate(VirtualConsole *vc)
{
    QemuUIInfo info;

    GtkNative *native = gtk_widget_get_native(vc->widget);
    GdkSurface *surface = gtk_native_get_surface(native);
    int refresh_rate;

    if (surface) {
        GdkDisplay *dpy = gtk_widget_get_display(vc->widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_surface(dpy, surface);
        refresh_rate = gdk_monitor_get_refresh_rate(monitor); /* [mHz] */
    } else {
        refresh_rate = 0;
    }

    info = *dpy_get_ui_info(vc->gfx.dcl.con);
    info.refresh_rate = refresh_rate;
    dpy_set_ui_info(vc->gfx.dcl.con, &info, true);

    /* T = 1 / f = 1 [s*Hz] / f = 1000*1000 [ms*mHz] / f */
    vc->gfx.dcl.update_interval = refresh_rate ?
        MIN(1000 * 1000 / refresh_rate, GUI_REFRESH_INTERVAL_DEFAULT) :
        GUI_REFRESH_INTERVAL_DEFAULT;
}

static const guint16 *gd_get_keymap(size_t *maplen)
{
    GdkDisplay *dpy = gdk_display_get_default();

    if (GDK_IS_WAYLAND_DISPLAY(dpy)) {
        trace_gtk4_gd_keymap_windowing("wayland");
        *maplen = qemu_input_map_xorgevdev_to_qcode_len;
        return qemu_input_map_xorgevdev_to_qcode;
    }

    g_warning("Unsupported GDK Windowing platform.\n"
              "Disabling extended keycode tables.\n"
              "Please report to qemu-devel@nongnu.org\n"
              "including the following information:\n"
              "\n"
              "  - Operating system\n"
              "  - GDK Windowing system build\n");
    return NULL;
}

int gd_map_keycode(int scancode)
{
    if (!keycode_map) {
        return 0;
    }
    if (scancode > keycode_maplen) {
        return 0;
    }

    return keycode_map[scancode];
}

static void gd_change_page(GtkNotebook *nb, gpointer arg1, guint arg2,
                           GtkDisplayState *s)
{
    VirtualConsole *vc;
    gboolean on_vga;
    GAction *grab_action;
    GAction *copy_action;
    GAction *vc_action;
    GAction *zoom_to_fit_action;
    GAction *zoom_in_action;
    GAction *zoom_out_action;
    GAction *zoom_fixed_action;
    GtkWidget *active_window;

    if (!gtk_widget_get_realized(GTK_WIDGET(nb))) {
        return;
    }

    vc = gd_vc_find_by_page(s, arg2);
    if (!vc) {
        return;
    }
    active_window = vc->window ? vc->window : s->window;
    gboolean is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(active_window));
    gboolean is_gfx = VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget);

    grab_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab-input");
    copy_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "copy");
    vc_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "vc");
    zoom_to_fit_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit");
    zoom_in_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-in");
    zoom_out_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-out");
    zoom_fixed_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fixed");

    on_vga = (is_gfx &&
              qemu_console_is_graphic(vc->gfx.dcl.con));
    if (!on_vga) {
        s_action_set_state(G_SIMPLE_ACTION(grab_action), FALSE);
    } else if (is_fullscreen) {
        s_action_set_state(G_SIMPLE_ACTION(grab_action), TRUE);
    }
    g_simple_action_set_enabled(G_SIMPLE_ACTION(grab_action), on_vga);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(copy_action), !is_gfx);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(zoom_to_fit_action), is_gfx);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(zoom_in_action), is_gfx);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(zoom_out_action), is_gfx);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(zoom_fixed_action), is_gfx);

    g_simple_action_set_state(G_SIMPLE_ACTION(vc_action), g_variant_new_string(vc->label));
    gd_update_windowsize(vc);
    gd_update_cursor(vc);
}

static gboolean
gd_tab_window_close(GtkWindow *widget, VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    g_object_ref(vc->widget);
    gtk_window_set_child(GTK_WINDOW(vc->window), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), vc->widget, NULL);
    gtk_notebook_reorder_child(GTK_NOTEBOOK(s->notebook), vc->widget, vc->index);
    g_object_unref(vc->widget);
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(s->notebook),
                                    vc->widget, vc->label);
    /** Put the item back to the menu */
    const char *action_name = g_strdup_printf("win.vc('%s')", vc->label);
    g_menu_insert(s->vc_menu, vc->index, vc->label, action_name);

    gtk_window_destroy(GTK_WINDOW(vc->window));
    vc->window = NULL;

    /** true for stopping other handlers from being invoked for this signal */
    return TRUE;
}

/** Window Menu Actions **/

/**
 * (un)pause the running instance action handler.
 */
static void
on_pause_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    if (runstate_is_running()) {
        qmp_stop(NULL);
    } else {
        qmp_cont(NULL);
    }
}

/**
 * Reset system action handler.
 */
static void
on_reset_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    qmp_system_reset(NULL);
}

/**
 * Power down system action handler.
 */
static void
on_powerdown_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    qmp_system_powerdown(NULL);
}

/**
 * Quit application action handler.
 */
static void
on_quit_cb(GSimpleAction *action, GVariant *param,
           void *user_data)
{   
    qmp_quit(NULL);
}

/**
 * (un)grab input action handler.
 */
static void
on_grab_input_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);
    
    if (!s_action_get_state(action)) {
        s_action_set_state(action, TRUE);
        gd_grab_keyboard(vc, "user-request-main-window");
        gd_grab_pointer(vc, "user-request-main-window");
    } else {
        s_action_set_state(action, FALSE);
        gd_ungrab_keyboard(s);
        gd_ungrab_pointer(s);
    }

    gd_update_cursor(vc);
}


static void
on_switch_vc_cb(GSimpleAction *action, GVariant *param,
                GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_by_menu(s,
                                            g_variant_get_string(param, NULL));
    GtkNotebook *nb = GTK_NOTEBOOK(s->notebook);
    gint page;
    if (vc) {
        page = gtk_notebook_page_num(nb, vc->widget);
        gtk_notebook_set_current_page(nb, page);
    }
}

/**
 * Show/hide tabs action handler.
 */
static void
on_show_tabs_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);

    if (!s_action_get_state(action)) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), TRUE);
        s_action_set_state(action, TRUE);
    } else {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        s_action_set_state(action, FALSE);
    }
    gd_update_windowsize(vc);
}


/**
 * Show/hide menu bar action handler.
 */
static void
on_show_menubar_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    gboolean is_fullscreen;
    g_return_if_fail(vc != NULL);

    is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(s->window));
    if (is_fullscreen) {
        return;
    }

    if (!s_action_get_state(action)) {
        s_action_set_state(action, TRUE);
        gtk_widget_set_visible(s->menubar, TRUE);
    } else {
        s_action_set_state(action, FALSE);
        gtk_widget_set_visible(s->menubar, FALSE);
    }
    gd_update_windowsize(vc);
}

/**
 * (un)fullscreen action handler.
 */
static void
on_fullscreen_cb(GSimpleAction *action, GVariant* param, void *user_data)
{
    GAction *show_tabs_action;
    GAction *show_menubar_action;
    gboolean is_fullscreen;
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);
    
    show_tabs_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "show-tabs");
    show_menubar_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "show-menubar");
    is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(s->window));

    if (!is_fullscreen) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        gtk_widget_set_visible(s->menubar, FALSE);

        gtk_window_fullscreen(GTK_WINDOW(s->window));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(s->window));
        if (s_action_get_state(G_SIMPLE_ACTION(show_tabs_action))) {
            gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), TRUE);
        }
        if (s_action_get_state(G_SIMPLE_ACTION(show_menubar_action))) {
            gtk_widget_set_visible(s->menubar, TRUE);
        }
        if (VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget)) {
            virtual_console_gfx_widget_reset_zoom(
                VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget)
            );
            gd_update_windowsize(vc);
        }
    }

    gd_update_cursor(vc);
}

static void 
on_copy_cb(GSimpleAction *action, GVariant* param, void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);

    g_return_if_fail(vc != NULL);
    g_return_if_fail(VIRTUAL_CONSOLE_IS_VTE_WIDGET(vc->widget));

    virtual_console_vte_widget_copy(VIRTUAL_CONSOLE_VTE_WIDGET(vc->widget));
}

/**
 * Zoom to fit action handler.
 */
static void 
on_zoom_fit_cb(GSimpleAction *action, GVariant* param,
               void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);

    if (VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget))
    {
        VirtualConsoleGfxWidget *widget = VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget);
        if (s_action_get_state(action)) {
            s_action_set_state(action, FALSE);
            virtual_console_gfx_widget_set_free_scale(widget, TRUE);
        } else {
            s_action_set_state(action, TRUE);
            virtual_console_gfx_widget_set_free_scale(widget, FALSE);
        }
        gd_update_windowsize(vc);
    }
}
/**
 * Zoom out action handler.
 */
static void
on_zoom_out_cb(GSimpleAction *action, GVariant* param,
               void *user_data)
{
    GtkDisplayState *s = user_data;
    GAction *zoom_fit_action;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);

    VirtualConsoleGfxWidget *widget = VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget);
    virtual_console_gfx_widget_zoom_out(widget);

    zoom_fit_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit");
    s_action_set_state(G_SIMPLE_ACTION(zoom_fit_action), FALSE);
    gd_update_windowsize(vc);
}

/**
 * Zoom in action handler.
 */
static void
on_zoom_in_cb(GSimpleAction *action, GVariant* param,
              void *user_data)
{
    GtkDisplayState *s = user_data;
    GAction *zoom_fit_action;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);

    VirtualConsoleGfxWidget *widget = VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget);
    virtual_console_gfx_widget_zoom_in(widget);

    zoom_fit_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit");
    s_action_set_state(G_SIMPLE_ACTION(zoom_fit_action), FALSE);

    gd_update_windowsize(vc);
}

/**
 * Fixed zoom action handler.
 */
static void
on_zoom_fixed_cb(GSimpleAction *action, GVariant* param,
                 void *user_data)
{
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);

    VirtualConsoleGfxWidget *widget = VIRTUAL_CONSOLE_GFX_WIDGET(vc->widget);
    virtual_console_gfx_widget_reset_zoom(widget);

    gd_update_windowsize(vc);
}

/**
 * Untabify action handler.
 */
static void
on_untabify_cb(GSimpleAction *action, GVariant *param, void *user_data)
{
    GAction *grab_action;
    GtkDisplayState *s = user_data;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_return_if_fail(vc != NULL);
    g_return_if_fail(vc->window == NULL);

    if (VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget) &&
        qemu_console_is_graphic(vc->gfx.dcl.con)) {
        grab_action = g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab-input");
        s_action_set_state(G_SIMPLE_ACTION(grab_action), FALSE);
    }

    vc->window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(vc->window), 720, 360);
    gtk_widget_set_visible(s->menubar, FALSE);
    gtk_window_set_focus(GTK_WINDOW(vc->window), vc->widget);
    g_object_ref(vc->widget);
    gtk_notebook_detach_tab(GTK_NOTEBOOK(s->notebook), vc->widget);
    gtk_window_set_child(GTK_WINDOW(vc->window), vc->widget);
    g_object_unref(vc->widget);

    g_menu_remove(s->vc_menu, vc->index);

    g_signal_connect(vc->window, "close-request",
                        G_CALLBACK(gd_tab_window_close), vc);
    gtk_window_present(GTK_WINDOW(vc->window));

    gd_update_caption(s);
}

/** Virtual Console Callbacks **/

static void
gd_vc_widget_init(GtkDisplayState *s, VirtualConsole *vc)
{
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook),
                            vc->widget, gtk_label_new(vc->label));
    const char *action_name = g_strdup_printf("win.vc('%s')", vc->label);
    g_menu_append(s->vc_menu, vc->label, action_name);
    const char *accels[2] = { g_strdup_printf("<Ctrl><Alt>%d", vc->index + 1),
                              NULL };
    //gtk_application_set_accels_for_action(s->app, action_name, accels);
}

/** Window Creation **/

/**
 * Create a machine menu.
 */
static GMenu *
gd_create_menu_machine(GtkDisplayState *s)
{
    GMenu *machine_menu, *model;
    GMenuItem *section;
    machine_menu = g_menu_new();
    g_menu_append(machine_menu, _("_Pause"), "win.pause");

    model = g_menu_new();
    section = g_menu_item_new_section(NULL,
                                      G_MENU_MODEL(model));
    g_menu_append_item(machine_menu, section);

    g_menu_append(model, _("_Reset"), "win.reset");
    g_menu_append(model, _("Power _Down"), "win.power_down");

    model = g_menu_new();
    section = g_menu_item_new_section(NULL,
                                      G_MENU_MODEL(model));
    g_menu_append_item(machine_menu, section);

    g_menu_append(model, _("_Quit"), "win.quit");

    return machine_menu;
}

/**
 * Create a view menu.
 */
static GMenu *
gd_create_menu_view(GtkDisplayState *s, DisplayOptions *opts)
{
    GMenu *view_menu, *model;
    GMenuItem *section;
    view_menu = g_menu_new();

    model = g_menu_new();
    section = g_menu_item_new_section(NULL, G_MENU_MODEL(model));
    g_menu_append_item(view_menu, section);
#if defined(CONFIG_VTE4)
    g_menu_append(model, _("_Copy"), "win.copy");
#endif
    g_menu_append(model, _("_Fullscreen"), "win.fullscreen");

    model = g_menu_new();
    section = g_menu_item_new_section(NULL, G_MENU_MODEL(model));
    g_menu_append_item(view_menu, section);

    g_menu_append(model, _("Zoom _In"), "win.zoom-in");
    g_menu_append(model, _("Zoom _Out"), "win.zoom-out");
    g_menu_append(model, _("Best _Fit"), "win.zoom-fixed");
    g_menu_append(model, _("Zoom To _Fit"), "win.zoom-fit");

    model = g_menu_new();
    section = g_menu_item_new_section(NULL, G_MENU_MODEL(model));
    g_menu_append_item(view_menu, section);

    g_menu_append(model, _("Grab On _Hover"),
                  "win.grab-on-hover");
    g_menu_append(model, _("_Grab Input"),
                  "win.grab-input");

    s->vc_menu = g_menu_new();
    section = g_menu_item_new_section(NULL, G_MENU_MODEL(s->vc_menu));
    g_menu_append_item(view_menu, section);

    model = g_menu_new();
    section = g_menu_item_new_section(NULL, G_MENU_MODEL(model));
    g_menu_append_item(view_menu, section);

    g_menu_append(model, _("Show _Tabs"), "win.show-tabs");
    g_menu_append(model, _("Detach Tab"), "win.untabify");
    g_menu_append(model, _("Show Menubar"), "win.show-menubar");

    return view_menu;
}

/**
 * Create the menu bar model.
 */
static GMenu *
gd_create_menus_models(GtkDisplayState *s, DisplayOptions *opts)
{
    GMenu *model = g_menu_new();
    GMenuModel *machine_menu = G_MENU_MODEL(gd_create_menu_machine(s));
    GMenuModel *view_menu = G_MENU_MODEL(gd_create_menu_view(s, opts));

    GMenuItem *machine_item = g_menu_item_new_submenu(_("_Machine"),
                                                      machine_menu);
    g_menu_insert_item(model, 0, machine_item);

    GMenuItem *view_menu_item = g_menu_item_new_submenu(_("_View"),
                                                        view_menu);
    g_menu_insert_item(model, 1, view_menu_item);

    return model;
}

static GActionEntry window_entries[] =
{
  { "quit", on_quit_cb, NULL, NULL, NULL },
  { "power_down", on_powerdown_cb, NULL, NULL, NULL },
  { "pause", on_pause_cb, NULL, "false", NULL },
  { "reset", on_reset_cb, NULL, NULL, NULL },
  { "untabify", on_untabify_cb, NULL, NULL, NULL },
  { "grab-on-hover", NULL, NULL, "false", NULL },
  { "grab-input", on_grab_input_cb, NULL, "false", NULL },
  { "show-tabs", on_show_tabs_cb, NULL, "false", NULL },
  { "fullscreen", on_fullscreen_cb, NULL, NULL, NULL },
  { "show-menubar", on_show_menubar_cb, NULL, "false", NULL },
  { "zoom-fit", on_zoom_fit_cb, NULL, "false", NULL },
  { "zoom-in", on_zoom_in_cb, NULL, NULL, NULL },
  { "zoom-out", on_zoom_out_cb, NULL, NULL, NULL },
  { "zoom-fixed", on_zoom_fixed_cb, NULL, NULL, NULL },
  { "copy", on_copy_cb, NULL, NULL, NULL },
};

/**
 * Create the actions and add them to the window.
 */
static void 
setup_actions(GtkDisplayState *ds)
{
    GAction *show_menubar_action;
    GAction *copy_action;
    VirtualConsole *vc = gd_vc_find_current(ds);
    GSimpleAction *vc_action = g_simple_action_new_stateful("vc", G_VARIANT_TYPE_STRING,
                                                            g_variant_new_string(vc->label));
    g_signal_connect(vc_action, "activate",
                     G_CALLBACK(on_switch_vc_cb), ds);
    g_action_map_add_action(G_ACTION_MAP(ds->actions), G_ACTION(vc_action));
    g_action_map_add_action_entries(G_ACTION_MAP(ds->actions),
                                    window_entries, G_N_ELEMENTS(window_entries),
                                    ds);

    gboolean show_menubar = !ds->opts->u.gtk4.has_show_menubar ||
                            ds->opts->u.gtk4.show_menubar;
    show_menubar_action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "show-menubar");
    s_action_set_state(G_SIMPLE_ACTION(show_menubar_action), show_menubar);

    /** We disable copy action if it is not a VTE widget at start up */
    copy_action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "copy");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(copy_action), 
                                !VIRTUAL_CONSOLE_IS_GFX_WIDGET(vc->widget));
}

/**
 * Handle GApplication's startup signal.
 *
 * We only set up the keyboard shortcuts here.
 */
static void
on_app_startup(GtkApplication *app, GtkDisplayState *ds)
{
    const char *quit_accels[2] = { "<Ctrl><Alt>Q", NULL };
    const char *fullscreen_accels[2] = { "<Ctrl><Alt>F", NULL };
    const char *show_menubar_accels[2] = { "<Ctrl><Alt>M", NULL };
    const char *grab_input_accels[2] = { "<Ctrl><Alt>G", NULL };
    const char *best_fit_accels[2] = { "<Ctrl><Alt>0", NULL };
    const char *zoom_out_accels[2] = { "<Ctrl><Alt>minus", NULL };
    const char *zoom_in_accels[2] = { "<Ctrl><Alt>plus", NULL };

    gtk_application_set_accels_for_action(app, "win.zoom-in",
                                          zoom_in_accels);
    gtk_application_set_accels_for_action(app, "win.zoom-out",
                                          zoom_out_accels);
    gtk_application_set_accels_for_action(app, "win.zoom-fixed",
                                          best_fit_accels);
    gtk_application_set_accels_for_action(app, "win.grab-input",
                                          grab_input_accels);
    gtk_application_set_accels_for_action(app, "win.show-menubar",
                                          show_menubar_accels);
    gtk_application_set_accels_for_action(app, "win.fullscreen",
                                          fullscreen_accels);
    gtk_application_set_accels_for_action(app, "win.quit",
                                          quit_accels);
}

/**
 * Handle GApplication's activate signal.
 */
static void
on_app_activate(GtkApplication *app, GtkDisplayState *ds)
{
    VirtualConsole *vc;
    GtkIconTheme *theme;
    GMenu *menu;
    GtkWidget *vbox;
    QemuConsole *con;
    int n_vc, i;
    GdkDisplay *display;
    char *dir;
    GAction *action;
    bool zoom_to_fit = false;

    g_set_prgname("qemu");

    ds->window = gtk_window_new();
    //gtk_window_set_application(GTK_WINDOW(ds->window), ds->app);
    ds->actions = g_simple_action_group_new();
    gtk_widget_insert_action_group(ds->window, "win", G_ACTION_GROUP(ds->actions));
    gtk_window_set_default_size(GTK_WINDOW(ds->window), 720, 360);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ds->notebook = gtk_notebook_new();
    menu = gd_create_menus_models(ds, ds->opts);
    ds->menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_visible(ds->menubar, TRUE);

    display = gtk_widget_get_display(ds->window);
    theme = gtk_icon_theme_get_for_display(display);
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR);
    gtk_icon_theme_add_search_path(theme, dir);
    g_free(dir);

    if (ds->opts->has_show_cursor && ds->opts->show_cursor) {
        ds->null_cursor = NULL; /* default pointer */
    } else {
        ds->null_cursor = gdk_cursor_new_from_name("none", NULL);
    }

    gtk_window_set_icon_name(GTK_WINDOW(ds->window), "qemu");

    g_signal_connect(ds->window, "close-request",
                     G_CALLBACK(gd_window_close), ds);
    g_signal_connect(ds->notebook, "switch-page",
                     G_CALLBACK(gd_change_page), ds);

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(ds->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ds->notebook), FALSE);

    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), ds->menubar);
    gtk_box_append(GTK_BOX(vbox), ds->notebook);

    gtk_window_set_child(GTK_WINDOW(ds->window), vbox);

    gtk_window_present(GTK_WINDOW(ds->window));
    if (ds->opts->u.gtk4.has_show_menubar &&
        !ds->opts->u.gtk4.show_menubar) {
        gtk_widget_set_visible(ds->menubar, FALSE);
    }

    for (n_vc = 0;; n_vc++) {
        con = qemu_console_lookup_by_index(n_vc);
        if (!con) {
            break;
        }
        VirtualConsole *vc = &ds->vc[n_vc];
        vc->index = n_vc;
        vc->s = ds;
        virtual_console_gfx_widget_new(vc, con);
        gd_vc_widget_init(ds, vc);
        ds->nb_vcs++;
    }
#if defined(CONFIG_VTE4)
    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = &ds->vc[ds->nb_vcs];
        vc->s = ds;
        vc->index = ds->nb_vcs;
        virtual_console_vte_widget_new(vc);
        gd_vc_widget_init(ds, vc);
        ds->nb_vcs++;
    }
#endif
    setup_actions(ds);
    
    vc = gd_vc_find_current(ds);

    gtk_window_set_focus(GTK_WINDOW(ds->window), vc->widget);

    if (ds->opts->has_full_screen &&
        ds->opts->full_screen) {
        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "fullscreen");
        g_action_activate(action, NULL);
    }
    if (ds->opts->u.gtk4.has_grab_on_hover &&
        ds->opts->u.gtk4.grab_on_hover) {
        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "grab-on-hover");
        g_action_activate(action, g_variant_new_boolean(TRUE));
    }
    if (ds->opts->u.gtk4.has_show_tabs &&
        ds->opts->u.gtk4.show_tabs) {
        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "show-tabs");
        g_action_activate(action, g_variant_new_boolean(TRUE));
    }

    if (dpy_ui_info_supported(vc->gfx.dcl.con)) {
        zoom_to_fit = true;
    }
    if (ds->opts->u.gtk4.has_zoom_to_fit) {
        zoom_to_fit = ds->opts->u.gtk4.zoom_to_fit;
    }
    if (zoom_to_fit) {
        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "zoom-fit");
        g_action_activate(action, NULL);
    }

    /** We disable show tabs / untabify actions as they are not useful */
    if (ds->nb_vcs == 1) {
        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "untabify");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), FALSE);

        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "show-tabs");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), FALSE);

        action = g_action_map_lookup_action(G_ACTION_MAP(ds->actions), "vc");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), FALSE);
    }

    ds->mouse_mode_notifier.notify = gd_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&ds->mouse_mode_notifier);
    qemu_add_vm_change_state_handler(on_runstate_change_cb, ds);
    gd_update_caption(ds);
}

static void
gtk_display_init(DisplayState *ds, DisplayOptions *opts)
{
    GtkDisplayState *s = g_malloc0(sizeof(*s));
    char *dir;

    if (!gtkinit) {
        fprintf(stderr, "gtk initialization failed\n");
        exit(1);
    }

    /*
     * Mostly LC_MESSAGES only. See early_gtk_display_init() for details. For
     * LC_CTYPE, we need to make sure that non-ASCII characters are considered
     * printable, but without changing any of the character classes to make
     * sure that we don't accidentally break implicit assumptions.
     */
    setlocale(LC_MESSAGES, "");
    setlocale(LC_CTYPE, "C.UTF-8");
    dir = get_relocated_path(CONFIG_QEMU_LOCALEDIR);
    bindtextdomain("qemu", dir);
    g_free(dir);
    bind_textdomain_codeset("qemu", "UTF-8");
    textdomain("qemu");

    assert(opts->type == DISPLAY_TYPE_GTK4);
    s->opts = opts;
    s->app = gtk_application_new("org.qemu.qemu", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(s->app, "startup",
                    G_CALLBACK(on_app_startup), s);
    g_signal_connect(s->app, "activate",
                    G_CALLBACK(on_app_activate), s);
    g_application_run(G_APPLICATION(s->app), 0, NULL);
    /**
     * Potential problem: stop using GtkApplication as that would potentially create
     * a new MainLoop which would stop the timer from working
     * 
     */
}

static void
early_gtk_display_init(DisplayOptions *opts)
{
    display_opengl = 1;
    /*
     * The QEMU code relies on the assumption that it's always run in
     * the C locale. Therefore it is not prepared to deal with
     * operations that produce different results depending on the
     * locale, such as printf's formatting of decimal numbers, and
     * possibly others.
     *
     * Since GTK calls setlocale() by default -importing the locale
     * settings from the environment- we must prevent it from doing so
     * using gtk_disable_setlocale().
     *
     * QEMU's GTK UI, however, _does_ have translations for some of
     * the menu items. As a trade-off between a functionally correct
     * QEMU and a fully internationalized UI we support importing
     * LC_MESSAGES from the environment (see the setlocale() call
     * earlier in this file). This allows us to display translated
     * messages leaving everything else untouched.
     */
    gtk_disable_setlocale();
    gtkinit = gtk_init_check();
    if (!gtkinit) {
        /* don't exit yet, that'll break -help */
        return;
    }
    assert(opts->type == DISPLAY_TYPE_GTK4);
    assert(opts->gl != DISPLAYGL_MODE_OFF);

    keycode_map = gd_get_keymap(&keycode_maplen);
#if defined(CONFIG_VTE4)
    vte_vc_type_register();
#endif
}

static QemuDisplay qemu_display_gtk = {
    .type       = DISPLAY_TYPE_GTK4,
    .early_init = early_gtk_display_init,
    .init       = gtk_display_init,
};

static void
register_gtk(void)
{
    qemu_display_register(&qemu_display_gtk);
}

type_init(register_gtk);

module_dep("ui-opengl");
