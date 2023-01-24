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

#include "gtk4-gfx-vc.h"

#include "qemu/osdep.h"
#include "qemu/main-loop.h"

#include "trace.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/gtk4.h"
#include "ui/input.h"

#include "sysemu/sysemu.h"

#define VC_SCALE_STEP   0.25
#define VC_SCALE_MIN    0.25

static void
virtual_console_gfx_widget_set_scale(VirtualConsoleGfxWidget *self,
                                     double scale);


static void gtk_gl_area_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
    if (!vc->gfx.scanout_mode) {
        egl_fb_destroy(&vc->gfx.guest_fb);
        if (vc->gfx.ds) {
            surface_gl_destroy_texture(vc->gfx.dgc.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.dgc.gls, vc->gfx.ds);
        }
    }
}

static void gd_cursor_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{
    info_report("Calling gd_cursor_define");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GBytes *bytes;
    GdkTexture *texture;
    GdkCursor *cursor;

    if (!gtk_widget_get_realized(vc->widget)) {
        return;
    }
    bytes = g_bytes_new(c->data, c->height * c->width * 4);
    texture = gdk_memory_texture_new(c->width, c->height,
                                     GDK_MEMORY_R8G8B8A8,
                                     bytes, c->width * 4);
    cursor = gdk_cursor_new_from_texture(texture, c->hot_x, c->hot_y, NULL);
    gtk_widget_set_cursor(vc->widget, cursor);
    g_object_unref(cursor);
}

static void
gd_gl_area_scanout_disable(DisplayChangeListener *dcl)
{
    info_report("Calling gl_area_scanout_disable");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gtk_gl_area_set_scanout_mode(vc, false);
}

static void
gd_gl_area_scanout_texture(DisplayChangeListener *dcl,
                           uint32_t backing_id,
                           bool backing_y_0_top,
                           uint32_t backing_width,
                           uint32_t backing_height,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h)
{
    info_report("Calling gl_area_scanout_texture");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.x = x;
    vc->gfx.y = y;
    vc->gfx.w = w;
    vc->gfx.h = h;
    vc->gfx.y0_top = backing_y_0_top;


    if (backing_id == 0 || vc->gfx.w == 0 || vc->gfx.h == 0) {
        gtk_gl_area_set_scanout_mode(vc, false);
        return;
    }

    gtk_gl_area_set_scanout_mode(vc, true);
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, backing_width, backing_height,
                         backing_id, false);
}

static void
gd_gl_area_scanout_dmabuf(DisplayChangeListener *dcl,
                          QemuDmaBuf *dmabuf)
{
    info_report("Calling gl_area_scanout_dmabuf");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        return;
    }
    info_report("Importing texture");
    gd_gl_area_scanout_texture(dcl, dmabuf->texture,
                               false, dmabuf->width, dmabuf->height,
                               0, 0, dmabuf->width, dmabuf->height);

    if (dmabuf->allow_fences) {
        vc->gfx.guest_fb.dmabuf = dmabuf;
    }
}

static void gd_gl_area_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    info_report("Calling gl_area_scanout_flush");
}

static
void gd_dpy_refresh(DisplayChangeListener *dcl)
{
    //info_report("gd_dpy_refresh");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    gd_update_monitor_refresh_rate(vc);

    graphic_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
        gtk_widget_queue_draw(vc->widget);
    }
}

static
void gd_dpy_gfx_update(DisplayChangeListener *dcl,
                       int x, int y, int w, int h)
{
    //info_report("gd_dpy_gfx_update");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gdk_gl_context_make_current(vc->gfx.context);
    surface_gl_update_texture(vc->gfx.dgc.gls, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
}

static
void gd_dpy_gfx_switch(DisplayChangeListener *dcl,
                      struct DisplaySurface *surface)
{
    info_report("gd_dpy_gfx_switch");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    DisplaySurface *old_surface = vc->gfx.ds;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));
    
    //gdk_gl_context_make_current(vc->gfx.context);
    surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
    vc->gfx.ds = surface;

    if (is_placeholder(surface) && qemu_console_get_index(dcl->con)) {
        qemu_gl_fini_shader(vc->gfx.gls);
        vc->gfx.gls = NULL;
        return;
    }

    if (!vc->gfx.gls)  {
        vc->gfx.gls = qemu_gl_init_shader();
    }else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(surface)) ||
                (surface_height(old_surface) != surface_height(surface)))) {
        gd_update_windowsize(vc);
    }

    surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);

}
static void gd_mouse_set(DisplayChangeListener *dcl,
                         int x, int y, int visible)
{
    info_report("calling gd_mouse_set");
    /*
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkDisplay *dpy;
    gint x_root, y_root;
    if (qemu_input_is_absolute()) {
        return;
    }
    dpy = gtk_widget_get_display(vc->widget);
    gdk_window_get_root_coords(gtk_widget_get_window(vc->widget),
                                x, y, &x_root, &y_root);
    gdk_device_warp(gd_get_pointer(dpy),
                    gtk_widget_get_screen(vc->widget),
                    x_root, y_root);
    vc->s->last_x = x;
    vc->s->last_y = y;
*/

}
static void gd_gl_release_dmabuf(DisplayChangeListener *dcl,
                                 QemuDmaBuf *dmabuf)
{
    info_report("calling gd_gl_release_dmabuf");
    egl_dmabuf_release_texture(dmabuf);
}

static bool gd_has_dmabuf(DisplayChangeListener *dcl)
{
    info_report("calling gd_has_dmabuf");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    return vc->gfx.has_dmabuf;
}

static const DisplayChangeListenerOps dcl_gl_area_ops = {
    .dpy_name             = "gtk4-egl",
    .dpy_refresh = gd_dpy_refresh,
    .dpy_gfx_update = gd_dpy_gfx_update,
    .dpy_gfx_switch = gd_dpy_gfx_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_scanout_texture  = gd_gl_area_scanout_texture,
    .dpy_gl_scanout_disable  = gd_gl_area_scanout_disable,
    .dpy_gl_update           = gd_gl_area_scanout_flush,
    .dpy_gl_scanout_dmabuf   = gd_gl_area_scanout_dmabuf,
    .dpy_gl_release_dmabuf   = gd_gl_release_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

static bool
gd_gl_area_is_compatible_dcl(DisplayGLCtx *dgc,
                             DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_area_ops;
}

/**
 * Make the GL context as current one.
 * 
 * Returns 0 on success.
 */
static int 
gd_gl_area_make_current(DisplayGLCtx *dgc,
                        QEMUGLContext ctx)
{
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);
    vc->gfx.context = g_object_ref(ctx);

    gdk_gl_context_make_current(ctx);
    return 0;
}

static QEMUGLContext 
gd_gl_area_create_context(DisplayGLCtx *dgc,
                          QEMUGLParams *params)
{
    info_report("Creating gl context");
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);
    GdkGLContext *ctx;
    GError *err = NULL;

    GtkNative *native = gtk_widget_get_native(vc->widget);
    GdkSurface *surface = gtk_native_get_surface(native);
    ctx = gdk_surface_create_gl_context(surface, &err);
    if (err) {
        error_report("Create gdk gl context failed: %s\n", err->message);
        g_error_free(err);
        return NULL;
    }
    gdk_gl_context_set_required_version(ctx,
                                        params->major_ver,
                                        params->minor_ver);
    gdk_gl_context_set_debug_enabled(ctx, TRUE);
    gdk_gl_context_set_forward_compatible(ctx, TRUE);
    //gdk_gl_context_set_allowed_apis(ctx, GDK_GL_API_GL);
    gdk_gl_context_realize(ctx, &err);
    if (err) {
        error_report("Realize gdk gl context failed: %s\n", err->message);
        g_error_free(err);
        g_clear_object(&ctx);
        return NULL;
    }

    vc->gfx.context = g_object_ref(ctx);
    trace_gtk4_gd_gl_area_create_context(ctx, params->major_ver, params->minor_ver);
    return ctx;
}

static void
gd_gl_area_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    info_report("Calling destroy_context");
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);
    GdkGLContext *current_ctx = gdk_gl_context_get_current();

    trace_gtk4_gd_gl_area_destroy_context(ctx, current_ctx);
    if (ctx == current_ctx) {
        info_report("destroying current ctx");
        gdk_gl_context_clear_current();
        if (vc->gfx.context) {
            g_object_unref(vc->gfx.context);
            vc->gfx.context = NULL;
        }
    }
    g_clear_object(&ctx);
}

static const DisplayGLCtxOps gl_area_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gd_gl_area_is_compatible_dcl,
    .dpy_gl_ctx_create       = gd_gl_area_create_context,
    .dpy_gl_ctx_destroy      = gd_gl_area_destroy_context,
    .dpy_gl_ctx_make_current = gd_gl_area_make_current,
};


struct _VirtualConsoleGfxWidget {
    GtkWidget parent_instance;

    double scale;
    gboolean free_scale;

    VirtualConsole *vc;
    QemuConsole *con;
};

enum {
    PROP_0,
    
    PROP_VC,
    PROP_CON,

    PROP_SCALE,
    PROP_FREE_SCALE,
    
    N_PROPS
};

static GParamSpec *props[N_PROPS] = { 0 };

G_DEFINE_TYPE(VirtualConsoleGfxWidget, virtual_console_gfx_widget,
             GTK_TYPE_WIDGET);

/** Helpers **/

static void release_modifiers(VirtualConsole *vc)
{
    qkbd_state_lift_all_keys(vc->gfx.kbd);
}

static gboolean gd_win_grab(GtkWidget *widget, GVariant *args,
                            void *opaque)
{
    VirtualConsole *vc = opaque;
    fprintf(stderr, "%s: %s\n", __func__, vc->label);
    if (vc->s->ptr_owner) {
        gd_ungrab_pointer(vc->s);
    } else {
        gd_grab_pointer(vc, "user-request-detached-tab");
    }
     /** The action was handled successfully */
    return TRUE;
}

/** Event Controllers handlers **/

static void gd_key_event(guint keyval, guint keycode, bool is_press,
                         VirtualConsole *vc)
{
    int qcode;

    if (keyval == GDK_KEY_Pause) {
        qkbd_state_key_event(vc->gfx.kbd, Q_KEY_CODE_PAUSE,
                             is_press);
        return;
    }

    qcode = gd_map_keycode(keycode);

    trace_gtk4_gd_key_event(vc->label, keycode, qcode,
                       (is_press) ? "down" : "up");

    qkbd_state_key_event(vc->gfx.kbd, qcode,
                         is_press);
}

/**
 * Scroll event handler.
 *
 * @param controller The event controller handler
 * @param delta_x Delta x
 * @param delta_y Delta y
 * @param vc The virutal console
 * @return gboolean Whether the event was handled or not
 */
static gboolean on_scroll(GtkEventController *controller,
                          double delta_x, double delta_y,
                          VirtualConsole *vc)
{
    GdkEvent *event = gtk_event_controller_get_current_event(controller);
    GdkScrollDirection direction = gdk_scroll_event_get_direction(event);
    InputButton btn_vertical;
    InputButton btn_horizontal;
    bool has_vertical = false;
    bool has_horizontal = false;

    switch (direction) {
    case GDK_SCROLL_UP:
        btn_vertical = INPUT_BUTTON_WHEEL_UP;
        has_vertical = true;
    break;
    case GDK_SCROLL_DOWN:
        btn_vertical = INPUT_BUTTON_WHEEL_DOWN;
        has_vertical = true;
    break;
    case GDK_SCROLL_LEFT:
        btn_horizontal = INPUT_BUTTON_WHEEL_LEFT;
        has_horizontal = true;
    break;
    case GDK_SCROLL_RIGHT:
        btn_horizontal = INPUT_BUTTON_WHEEL_RIGHT;
        has_horizontal = true;
    break;
    case GDK_SCROLL_SMOOTH:
        if (delta_y > 0) {
            btn_vertical = INPUT_BUTTON_WHEEL_DOWN;
            has_vertical = true;
        } else if (delta_y < 0) {
            btn_vertical = INPUT_BUTTON_WHEEL_UP;
            has_vertical = true;
        } else if (delta_x > 0) {
            btn_horizontal = INPUT_BUTTON_WHEEL_RIGHT;
            has_horizontal = true;
        } else if (delta_x < 0) {
            btn_horizontal = INPUT_BUTTON_WHEEL_LEFT;
            has_horizontal = true;
        }
    break;
    }

    if (has_vertical) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, false);
        qemu_input_event_sync();
    }

    if (has_horizontal) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, false);
        qemu_input_event_sync();
    }

    return TRUE;
}

static void on_focus_leave(GtkEventControllerFocus *controller,
                           VirtualConsole *vc)
{
    release_modifiers(vc);
}

static void gd_key_event_pressed(GtkEventControllerKey *controller,
                                 guint keyval, guint keycode,
                                 GdkModifierType state,
                                 VirtualConsole *vc)
{
    gd_key_event(keyval, keycode, true, vc);

}

static void gd_key_event_released(GtkEventControllerKey *controller,
                                  guint keyval, guint keycode,
                                  GdkModifierType state,
                                  VirtualConsole *vc)
{
    gd_key_event(keyval, keycode, false, vc);
}

static gboolean gd_enter_event(GtkEventControllerMotion *controller,
                               double x, double y,
                               VirtualConsole *vc)
{
  
/*   GtkDisplayState *s = vc->s;
   if (s_action_get_state(s->grab_on_hover_action)) {
        gd_grab_keyboard(vc, "grab-on-hover");
    }*/
    return TRUE;
}

static void gd_button_event(GtkEventController *controller, gint n_presses,
                            double x, double y, VirtualConsole *vc)
{
    GdkEvent *button = gtk_event_controller_get_current_event(controller);
    GtkDisplayState *s = vc->s;
    guint button_nbr = gdk_button_event_get_button(GDK_EVENT(button));
    InputButton btn;

    /* implicitly grab the input at the first click in the relative mode */
    if (button_nbr == 1 && n_presses == 1 &&
        !qemu_input_is_absolute() && s->ptr_owner != vc) {
        if (!vc->window) {
            //s_action_set_state(s->grab_action, TRUE);
        } else {
            gd_grab_pointer(vc, "relative-mode-click");
        }
    }

    switch (button_nbr) {
    case 1:
        btn = INPUT_BUTTON_LEFT;
    break;
    case 2:
        btn = INPUT_BUTTON_MIDDLE;
    break;
    case 3:
        btn = INPUT_BUTTON_RIGHT;
    break;
    case 8:
        btn = INPUT_BUTTON_SIDE;
    break;
    case 9:
        btn = INPUT_BUTTON_EXTRA;
    break;
    default:
        return;
    break;
    }

    if (n_presses == 2 || n_presses == 3) {
        return;
    }

    qemu_input_queue_btn(vc->gfx.dcl.con, btn, n_presses == 1);
    qemu_input_event_sync();
}


static gboolean gd_leave_event(GtkEventControllerMotion *controller,
                               double x, double y,
                               VirtualConsole *vc)
{
     /* GtkDisplayState *s = vc->s;

  if (s_action_get_state(s->grab_on_hover_action)) {
        gd_ungrab_keyboard(s);
    }*/
    return TRUE;
}

static void gd_motion_event(GtkEventControllerMotion *controller,
                            double pointer_x, double pointer_y,
                            VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GtkNative *native;
    GdkSurface *surface;
    GtkWidget *widget = vc->widget;
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(widget);

    int x, y;
    int mx = 0;
    int my = 0;
    int fbh, fbw;
    int ww, wh, ws;
    if (!vc->gfx.ds) {
        return;
    }

    fbw = surface_width(vc->gfx.ds) * self->scale;
    fbh = surface_height(vc->gfx.ds) * self->scale;

    native = gtk_widget_get_native(widget);
    surface = gtk_native_get_surface(native);
    ww = gdk_surface_get_width(surface);
    wh = gdk_surface_get_height(surface);
    ws = gdk_surface_get_scale_factor(surface);

    if (ww > fbw) {
        mx = (ww - fbw) / 2;
    }
    if (wh > fbh) {
        my = (wh - fbh) / 2;
    }

    x = ((int)pointer_x - mx) / self->scale * ws;
    y = ((int)pointer_y - my) / self->scale * ws;

    if (qemu_input_is_absolute()) {
        if (x < 0 || y < 0 ||
            x >= surface_width(vc->gfx.ds) ||
            y >= surface_height(vc->gfx.ds)) {
            return;
        }
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, x,
                             0, surface_width(vc->gfx.ds));
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, y,
                             0, surface_height(vc->gfx.ds));
        qemu_input_event_sync();
    } else if (s->last_set && s->ptr_owner == vc) {
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_X, x - s->last_x);
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_Y, y - s->last_y);
        qemu_input_event_sync();
    }
    s->last_x = x;
    s->last_y = y;
    s->last_set = TRUE;

    if (!qemu_input_is_absolute() && s->ptr_owner == vc) {
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_surface(dpy, surface);
        GdkRectangle geometry;

        int x = (int)pointer_x;
        int y = (int)pointer_y;

        gdk_monitor_get_geometry(monitor, &geometry);

        /*
         * In relative mode check to see if client pointer hit
         * one of the monitor edges, and if so move it back to the
         * center of the monitor. This is important because the pointer
         * in the server doesn't correspond 1-for-1, and so
         * may still be only half way across the screen. Without
         * this warp, the server pointer would thus appear to hit
         * an invisible wall
         */
        if (x <= geometry.x || x - geometry.x >= geometry.width - 1 ||
            y <= geometry.y || y - geometry.y >= geometry.height - 1) {
            x = geometry.x + geometry.width / 2;
            y = geometry.y + geometry.height / 2;

            /*
             * FIXME: replace with X11 spexific API
             * GdkScreen *screen = gtk_widget_get_screen(vc->widget);
             * GdkDevice *dev = gdk_event_get_device((GdkEvent *)motion);
             * gdk_device_warp(dev, screen, x, y);
             */
            s->last_set = FALSE;
            return;
        }
    }
    return;
}


/** Actions **/

static void
virtual_console_gfx_widget_init(VirtualConsoleGfxWidget *widget)
{
}

static void
virtual_console_gfx_widget_constructed(GObject *object)
{
    GtkWidget *widget = GTK_WIDGET(object);
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(object);
    VirtualConsole *vc = self->vc;
    vc->widget = widget;
    vc->gfx.context = gdk_gl_context_get_current();

    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_set_focusable(widget, TRUE);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(widget, key_controller);

    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES
    );
    g_signal_connect(scroll_controller, "scroll",
                    G_CALLBACK(on_scroll), &vc);
    gtk_widget_add_controller(widget, scroll_controller);

    g_signal_connect(key_controller, "key-pressed",
                        G_CALLBACK(gd_key_event_pressed), vc);
    g_signal_connect(key_controller, "key-released",
                        G_CALLBACK(gd_key_event_released), vc);

    GtkEventController *controller = gtk_event_controller_focus_new();
    g_signal_connect(controller, "leave",
                     G_CALLBACK(on_focus_leave), vc);
    gtk_widget_add_controller(widget, controller);

    GtkGesture *gesture = gtk_gesture_click_new();
    g_signal_connect(gesture, "pressed",
                     G_CALLBACK(gd_button_event), vc);
    g_signal_connect(gesture, "released",
                     G_CALLBACK(gd_button_event), vc);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion",
                        G_CALLBACK(gd_motion_event), vc);
    g_signal_connect(motion, "enter",
                        G_CALLBACK(gd_enter_event), vc);
    g_signal_connect(motion, "leave",
                        G_CALLBACK(gd_leave_event), vc);
    gtk_widget_add_controller(widget, motion);

    vc->gfx.dcl.con = self->con;
    vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
    vc->gfx.kbd = qkbd_state_init(self->con);
    vc->label = qemu_console_get_label(self->con);
    vc->gfx.dgc.gls = qemu_gl_init_shader();
    vc->gfx.dcl.ops = &dcl_gl_area_ops;
    vc->gfx.dgc.ops = &gl_area_ctx_ops;

    qemu_console_set_display_gl_ctx(self->con, &vc->gfx.dgc);
    register_displaychangelistener(&vc->gfx.dcl);

    if (qemu_console_is_graphic(self->con)) {
        GtkEventController *ctrl = gtk_shortcut_controller_new();
        GdkModifierType modifiers = GDK_CONTROL_MASK | GDK_ALT_MASK;
        GtkShortcutTrigger *trigger = gtk_keyval_trigger_new(GDK_KEY_g,
                                                                modifiers);
        GtkShortcutAction *action = gtk_callback_action_new(gd_win_grab,
                                                            vc, NULL);
        gtk_widget_add_controller(widget, ctrl);
        GtkShortcut *shortcut = gtk_shortcut_new(trigger, action);
        GtkShortcutController *controller = GTK_SHORTCUT_CONTROLLER(ctrl);
        gtk_shortcut_controller_add_shortcut(controller, shortcut);
    }
    

    G_OBJECT_CLASS(virtual_console_gfx_widget_parent_class)->constructed(object);
}

static void
virtual_console_gfx_widget_get_property(GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(object);
    switch (prop_id) {
    case PROP_VC:
        g_value_set_pointer(value, &self->vc);
    break;
    case PROP_CON:
        g_value_set_pointer(value, &self->con);
    break;
    case PROP_SCALE:
        g_value_set_double(value, self->scale);
    break;
    case PROP_FREE_SCALE:
        g_value_set_boolean(value, self->free_scale);
    break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
virtual_console_gfx_widget_set_property(GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(object);

    switch (prop_id) {
    case PROP_VC:
        self->vc = (VirtualConsole *)g_value_get_pointer(value);
    break;
    case PROP_CON:
        self->con = (QemuConsole *)g_value_get_pointer(value);
    break;
    case PROP_SCALE:
        self->scale = g_value_get_double(value);
    break;
    case PROP_FREE_SCALE:
        self->free_scale = g_value_get_boolean(value);
    break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
virtual_console_gfx_widget_unmap(GtkWidget *widget)
{
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(widget);

    release_modifiers(self->vc);

    GTK_WIDGET_CLASS(virtual_console_gfx_widget_parent_class)->unmap(widget);
}

static void
virtual_console_gfx_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    VirtualConsoleGfxWidget *self = VIRTUAL_CONSOLE_GFX_WIDGET(widget);
    VirtualConsole *vc = self->vc;
    GdkTexture *texture;
    GdkRGBA bg;
    gdk_rgba_parse(&bg, "#000");

    int surface_w = surface_width(self->vc->gfx.ds);
    int surface_h = surface_height(self->vc->gfx.ds);
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    int x = MAX(0, (width - surface_w * self->scale) / 2);
    int y = MAX(0, (height - surface_h * self->scale) / 2);


    gtk_snapshot_append_color(snapshot, &bg, &GRAPHENE_RECT_INIT (0, 0, width, height));

    if (!gtk_widget_get_realized(widget) || !vc->gfx.ds ) {
        return;
    }
    GdkGLContext *ctx = gdk_gl_context_get_current();

    texture = gdk_gl_texture_new (ctx, vc->gfx.ds->texture, surface_w, surface_h, NULL, NULL);
    gtk_snapshot_append_texture(snapshot, texture, &GRAPHENE_RECT_INIT (x, y, surface_w * self->scale, surface_h * self->scale));
}

static void
virtual_console_gfx_widget_class_init(VirtualConsoleGfxWidgetClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

    object_class->constructed = virtual_console_gfx_widget_constructed;
    object_class->get_property = virtual_console_gfx_widget_get_property;
    object_class->set_property = virtual_console_gfx_widget_set_property;

    widget_class->unmap = virtual_console_gfx_widget_unmap;
    widget_class->snapshot = virtual_console_gfx_widget_snapshot;

  props[PROP_VC] =
    g_param_spec_pointer("vc",
                         "Virtual Console",
                         "Associated Virtual Console",
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  props[PROP_CON] =
    g_param_spec_pointer("con",
                         "QEmu Console",
                         "QEmu Console Connection",
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  props[PROP_SCALE] =
    g_param_spec_double("scale",
                         "Scale",
                         "Scale",
                         VC_SCALE_MIN, 10.0, 1.0,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);
  props[PROP_FREE_SCALE] =
    g_param_spec_boolean("free-scale",
                         "Free Scale",
                         "Lock scale",
                         FALSE,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, props);

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

}

GtkWidget *virtual_console_gfx_widget_new(VirtualConsole *vc, QemuConsole *con)
{
    return g_object_new(VIRTUAL_CONSOLE_GFX_WIDGET_TYPE,
                          "vc", vc,
                          "con", con,
                          NULL);
}


void
virtual_console_gfx_widget_set_scale(VirtualConsoleGfxWidget *self,
                                     double scale)
{
    g_object_set(self, "scale", scale, NULL);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
virtual_console_gfx_widget_set_free_scale(VirtualConsoleGfxWidget *self,
                                          gboolean free_scale)
{
    g_object_set(self, "free-scale", free_scale, NULL);
    if (!free_scale) {
        virtual_console_gfx_widget_set_scale(self, 1.0);
    }
}

void
virtual_console_gfx_widget_get_size(VirtualConsoleGfxWidget *self, int *width, int *height)
{
    if (!self->vc->gfx.ds) {
        return;
    }
    if (self->free_scale) {
        *width  = (int)(surface_width(self->vc->gfx.ds) * VC_SCALE_MIN);
        *height = (int)(surface_height(self->vc->gfx.ds) * VC_SCALE_MIN);
    } else {
        *width  = (int)(surface_width(self->vc->gfx.ds) * self->scale);
        *height = (int)(surface_height(self->vc->gfx.ds) * self->scale);
    }
}

void virtual_console_gfx_widget_zoom_in(VirtualConsoleGfxWidget *self)
{
    double scale = self->scale + VC_SCALE_STEP; 

    virtual_console_gfx_widget_set_scale(self, scale);    
}

void virtual_console_gfx_widget_zoom_out(VirtualConsoleGfxWidget *self) 
{
    double scale = self->scale; 
    
    scale = MAX(scale - VC_SCALE_STEP, VC_SCALE_MIN);

    virtual_console_gfx_widget_set_scale(self, scale);
}

void virtual_console_gfx_widget_reset_zoom(VirtualConsoleGfxWidget *self) {
    virtual_console_gfx_widget_set_scale(self, 1.0);
}
