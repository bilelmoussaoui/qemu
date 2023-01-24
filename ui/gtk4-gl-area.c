/*
 * GTK 4 UI backend based on the GTK 3 backend implementation
 * 
 * GLArea OpenGL code.
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

#include "qemu/osdep.h"
#include "qemu/main-loop.h"

#include "trace.h"

#include "ui/console.h"
#include "ui/gtk4.h"
#include "ui/egl-helpers.h"

#include "sysemu/sysemu.h"

static void gtk_gl_area_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    info_report("Setting scanout mode %i", scanout);
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
    if (!vc->gfx.scanout_mode) {
        egl_fb_destroy(&vc->gfx.guest_fb);
    }
}

static void gd_hw_gl_flushed(void *vcon)
{
    VirtualConsole *vc = vcon;
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;

    qemu_set_fd_handler(dmabuf->fence_fd, NULL, NULL, NULL);
    close(dmabuf->fence_fd);
    dmabuf->fence_fd = -1;
    graphic_hw_gl_block(vc->gfx.dcl.con, false);
}
/** DisplayState Callbacks (opengl version) **/

void gd_gl_area_refresh(DisplayChangeListener *dcl)
{
    info_report("Calling gl_area_refresh");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GtkWidget *widget = vc->window ? vc->window : vc->gfx.gl_area;

    gd_update_monitor_refresh_rate(vc, widget);

    graphic_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
        gtk_gl_area_set_scanout_mode(vc, false);
        gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.gl_area));
    }
}

void gd_gl_area_switch(DisplayChangeListener *dcl,
                       DisplaySurface *surface)
{
    info_report("Calling gl_area_switch");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }

    if (vc->gfx.gls) {
        //gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
        surface_gl_create_texture(vc->gfx.gls, surface);
    }
    vc->gfx.ds = surface;


    if (resized) {
        gd_update_windowsize(vc);
    }
}

void gd_gl_area_scanout_texture(DisplayChangeListener *dcl,
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

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.gl_area));

    if (backing_id == 0 || vc->gfx.w == 0 || vc->gfx.h == 0) {
        gtk_gl_area_set_scanout_mode(vc, false);
        return;
    }

    gtk_gl_area_set_scanout_mode(vc, true);
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, backing_width, backing_height,
                         backing_id, false);
}

void gd_gl_area_scanout_disable(DisplayChangeListener *dcl)
{
    info_report("Calling gl_area_scanout_disable");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gtk_gl_area_set_scanout_mode(vc, false);
}

void gd_gl_area_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    info_report("Calling gl_area_scanout_flush");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (vc->gfx.guest_fb.dmabuf && !vc->gfx.guest_fb.dmabuf->draw_submitted) {
        graphic_hw_gl_block(vc->gfx.dcl.con, true);
        vc->gfx.guest_fb.dmabuf->draw_submitted = true;
    }
    gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.gl_area));
}

void gd_gl_area_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    info_report("Calling gl_area_scanout_dmabuf");
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.gl_area));
    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        return;
    }

    gd_gl_area_scanout_texture(dcl, dmabuf->texture,
                               false, dmabuf->width, dmabuf->height,
                               0, 0, dmabuf->width, dmabuf->height);

    if (dmabuf->allow_fences) {
        vc->gfx.guest_fb.dmabuf = dmabuf;
    }
}

void gtk_gl_area_init(void)
{
    info_report("Initializing gl area");
    display_opengl = 1;
}

int gd_gl_area_make_current(DisplayGLCtx *dgc,
                            QEMUGLContext ctx)
{
    info_report("Making gl context as current");
    gdk_gl_context_make_current(ctx);
    return 0;
}
