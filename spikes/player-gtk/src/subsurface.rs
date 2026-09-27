//! The picture on a Wayland surface of our own, a desync subsurface of the
//! GTK window's: wgpu presents to it directly (Mailbox, as the player's
//! window does), so no frame waits for GTK's frame clock. GTK keeps the
//! widget's space and its input; the subsurface takes none (an empty input
//! region) and sits above the window's own surface.

use gtk4::glib::translate::ToGlibPtr;
use gtk4::prelude::*;
use gtk4::{gdk, glib};
use std::ffi::c_void;
use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{wl_compositor, wl_region, wl_registry, wl_subcompositor, wl_subsurface, wl_surface};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, delegate_noop};
use wayland_protocols::wp::viewporter::client::{wp_viewport, wp_viewporter};

unsafe extern "C" {
    fn gdk_wayland_display_get_wl_display(display: *mut gdk::ffi::GdkDisplay) -> *mut c_void;
    fn gdk_wayland_surface_get_wl_surface(surface: *mut gdk::ffi::GdkSurface) -> *mut c_void;
}

pub struct State;

pub struct Sub {
    conn: Connection,
    queue: EventQueue<State>,
    pub display: *mut c_void,
    pub surface: wl_surface::WlSurface,
    sub: wl_subsurface::WlSubsurface,
    viewport: wp_viewport::WpViewport,
    placed: Option<(i32, i32, i32, i32)>,
}

impl Sub {
    /// A subsurface of `widget`'s window. `None` when the window is not a
    /// Wayland one.
    pub fn new(widget: &impl IsA<gtk4::Widget>) -> Result<Sub, String> {
        let native = widget.native().ok_or("no native")?;
        let gsurface = native.surface().ok_or("no surface")?;
        let gdisplay = widget.display();
        let (display, parent) = unsafe {
            (
                gdk_wayland_display_get_wl_display(gdisplay.to_glib_none().0),
                gdk_wayland_surface_get_wl_surface(gsurface.to_glib_none().0),
            )
        };
        if display.is_null() || parent.is_null() {
            return Err("not a Wayland window".into());
        }
        let conn = unsafe { Connection::from_backend(Backend::from_foreign_display(display.cast())) };
        let (globals, queue) = registry_queue_init::<State>(&conn).map_err(|e| e.to_string())?;
        let qh = queue.handle();
        let compositor: wl_compositor::WlCompositor =
            globals.bind(&qh, 1..=4, ()).map_err(|e| format!("wl_compositor: {e}"))?;
        let subcompositor: wl_subcompositor::WlSubcompositor =
            globals.bind(&qh, 1..=1, ()).map_err(|e| format!("wl_subcompositor: {e}"))?;
        let viewporter: wp_viewporter::WpViewporter =
            globals.bind(&qh, 1..=1, ()).map_err(|e| format!("wp_viewporter: {e}"))?;
        let parent = unsafe { ObjectId::from_ptr(wl_surface::WlSurface::interface(), parent.cast()) }
            .map_err(|e| e.to_string())?;
        let parent = wl_surface::WlSurface::from_id(&conn, parent).map_err(|e| e.to_string())?;
        let surface = compositor.create_surface(&qh, ());
        let sub = subcompositor.get_subsurface(&surface, &parent, &qh, ());
        sub.set_desync();
        let region = compositor.create_region(&qh, ());
        surface.set_input_region(Some(&region));
        region.destroy();
        let viewport = viewporter.get_viewport(&surface, &qh, ());
        conn.flush().map_err(|e| e.to_string())?;
        Ok(Sub { conn, queue, display, surface, sub, viewport, placed: None })
    }

    pub fn surface_ptr(&self) -> *mut c_void {
        self.surface.id().as_ptr().cast()
    }

    /// Put the subsurface over `widget` (logical px in the window's surface)
    /// and size it. The position takes effect with GTK's next commit of the
    /// window, so a move asks GTK for a frame.
    pub fn place(&mut self, widget: &impl IsA<gtk4::Widget>) {
        let (Some(native), Some(root)) = (widget.native(), widget.root()) else { return };
        let Some(p) = widget.compute_point(&root, &gtk4::graphene::Point::new(0.0, 0.0)) else { return };
        let (tx, ty) = native.surface_transform();
        let at = ((p.x() as f64 + tx) as i32, (p.y() as f64 + ty) as i32, widget.width(), widget.height());
        if self.placed == Some(at) || at.2 <= 0 || at.3 <= 0 {
            return;
        }
        self.placed = Some(at);
        self.sub.set_position(at.0, at.1);
        self.viewport.set_destination(at.2, at.3);
        let _ = self.conn.flush();
        widget.queue_draw();
    }

    /// Events for our objects (enter/leave), filed under our queue when
    /// GTK reads the socket.
    pub fn poll(&mut self) {
        let _ = self.queue.dispatch_pending(&mut State);
    }
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

delegate_noop!(State: ignore wl_compositor::WlCompositor);
delegate_noop!(State: ignore wl_subcompositor::WlSubcompositor);
delegate_noop!(State: ignore wl_surface::WlSurface);
delegate_noop!(State: ignore wl_subsurface::WlSubsurface);
delegate_noop!(State: ignore wl_region::WlRegion);
delegate_noop!(State: ignore wp_viewporter::WpViewporter);
delegate_noop!(State: ignore wp_viewport::WpViewport);

#[allow(dead_code)]
fn _unused(_: glib::Propagation) {}
