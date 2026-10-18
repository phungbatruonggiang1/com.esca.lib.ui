#include "esca/ui/window.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

namespace esca::ui {

namespace {

/// Used only until the compositor says otherwise.
constexpr int kFallbackWidth = 1280;
constexpr int kFallbackHeight = 720;

struct Buffer {
    wl_buffer *buffer = nullptr;
    uint32_t *data = nullptr;
    size_t bytes = 0;
    bool busy = false; ///< held by the compositor until wl_buffer.release
};

} // namespace

struct Window::Impl {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *wm_base = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    wl_touch *touch = nullptr;

    Input input;
    bool press_pending = false; ///< a press whose release has not arrived

    wl_surface *surface = nullptr;
    xdg_surface *xdg_surface_ = nullptr;
    xdg_toplevel *toplevel = nullptr;

    // Two buffers: one on screen, one being drawn. A third would only add
    // latency, since a frame older than the one in hand is of no use.
    std::array<Buffer, 2> buffers{};
    wl_shm_pool *pool = nullptr;
    int pool_fd = -1;
    size_t pool_bytes = 0;

    int width = kFallbackWidth;
    int height = kFallbackHeight;
    int pending_width = 0;
    int pending_height = 0;

    bool configured = false;
    bool closed = false;
    Buffer *drawing = nullptr;

    bool allocate(std::string &error);
    void release_buffers();
};

namespace {

// --- listeners -------------------------------------------------------------

// --- pointer -----------------------------------------------------------

void pointer_enter(void *data, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x,
                   wl_fixed_t y)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.x = wl_fixed_to_int(x);
    impl->input.y = wl_fixed_to_int(y);
}

void pointer_leave(void *data, wl_pointer *, uint32_t, wl_surface *)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.x = impl->input.y = -1;
    // A pointer that leaves mid-press will never deliver the release, so
    // the press is abandoned rather than firing when it comes back.
    impl->input.down = false;
    impl->press_pending = false;
}

void pointer_motion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.x = wl_fixed_to_int(x);
    impl->input.y = wl_fixed_to_int(y);
}

void pointer_button(void *data, wl_pointer *, uint32_t, uint32_t, uint32_t button,
                    uint32_t state)
{
    constexpr uint32_t kLeftButton = 0x110; // BTN_LEFT
    if (button != kLeftButton)
        return;

    auto *impl = static_cast<Window::Impl *>(data);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        impl->input.down = true;
        impl->press_pending = true;
    } else {
        impl->input.down = false;
        // A click is press *and* release, so dragging off a button and
        // letting go still counts here -- which is what a touchscreen user
        // expects, and close enough for a handful of large buttons.
        if (impl->press_pending) {
            impl->input.clicked = true;
            impl->input.click_x = impl->input.x;
            impl->input.click_y = impl->input.y;
            impl->press_pending = false;
        }
    }
}

void pointer_axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}

const wl_pointer_listener kPointerListener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
};

// --- touch -------------------------------------------------------------

void touch_down(void *data, wl_touch *, uint32_t, uint32_t, wl_surface *, int32_t,
                wl_fixed_t x, wl_fixed_t y)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.x = wl_fixed_to_int(x);
    impl->input.y = wl_fixed_to_int(y);
    impl->input.down = true;
    impl->press_pending = true;
}

void touch_up(void *data, wl_touch *, uint32_t, uint32_t, int32_t)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.down = false;
    if (impl->press_pending) {
        impl->input.clicked = true;
        impl->input.click_x = impl->input.x;
        impl->input.click_y = impl->input.y;
        impl->press_pending = false;
    }
}

void touch_motion(void *data, wl_touch *, uint32_t, int32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.x = wl_fixed_to_int(x);
    impl->input.y = wl_fixed_to_int(y);
}

void touch_frame(void *, wl_touch *) {}
void touch_cancel(void *data, wl_touch *)
{
    auto *impl = static_cast<Window::Impl *>(data);
    impl->input.down = false;
    impl->press_pending = false;
}
void touch_shape(void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) {}
void touch_orientation(void *, wl_touch *, int32_t, wl_fixed_t) {}

const wl_touch_listener kTouchListener = {
    touch_down,  touch_up,     touch_motion,     touch_frame,
    touch_cancel, touch_shape, touch_orientation,
};

// --- seat --------------------------------------------------------------

void seat_capabilities(void *data, wl_seat *seat, uint32_t caps)
{
    auto *impl = static_cast<Window::Impl *>(data);

    const bool pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    if (pointer && !impl->pointer) {
        impl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(impl->pointer, &kPointerListener, impl);
        impl->input.has_pointer = true;
    } else if (!pointer && impl->pointer) {
        wl_pointer_release(impl->pointer);
        impl->pointer = nullptr;
        impl->input.has_pointer = false;
    }

    const bool touch = caps & WL_SEAT_CAPABILITY_TOUCH;
    if (touch && !impl->touch) {
        impl->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(impl->touch, &kTouchListener, impl);
        impl->input.has_touch = true;
    } else if (!touch && impl->touch) {
        wl_touch_release(impl->touch);
        impl->touch = nullptr;
        impl->input.has_touch = false;
    }
}

void seat_name(void *, wl_seat *, const char *) {}

const wl_seat_listener kSeatListener = { seat_capabilities, seat_name };

// --- registry ----------------------------------------------------------

void registry_global(void *data, wl_registry *registry, uint32_t name, const char *interface,
                     uint32_t version)
{
    auto *impl = static_cast<Window::Impl *>(data);

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        impl->compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        impl->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        impl->seat = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        impl->wm_base = static_cast<xdg_wm_base *>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}

void registry_global_remove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener kRegistryListener = { registry_global, registry_global_remove };

void wm_base_ping(void *, xdg_wm_base *wm_base, uint32_t serial)
{
    // Answering this is what keeps the compositor from deciding we have hung.
    xdg_wm_base_pong(wm_base, serial);
}

const xdg_wm_base_listener kWmBaseListener = { wm_base_ping };

void surface_configure(void *data, xdg_surface *surface, uint32_t serial)
{
    auto *impl = static_cast<Window::Impl *>(data);
    xdg_surface_ack_configure(surface, serial);

    if (impl->pending_width > 0 && impl->pending_height > 0) {
        impl->width = impl->pending_width;
        impl->height = impl->pending_height;
    }
    impl->configured = true;
}

const xdg_surface_listener kSurfaceListener = { surface_configure };

void toplevel_configure(void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *)
{
    // Zero means "you choose"; the fallback is then the camera's own size.
    auto *impl = static_cast<Window::Impl *>(data);
    impl->pending_width = width;
    impl->pending_height = height;
}

void toplevel_close(void *data, xdg_toplevel *)
{
    static_cast<Window::Impl *>(data)->closed = true;
}

const xdg_toplevel_listener kToplevelListener = { toplevel_configure, toplevel_close };


void buffer_release(void *data, wl_buffer *)
{
    static_cast<Buffer *>(data)->busy = false;
}

const wl_buffer_listener kBufferListener = { buffer_release };

} // namespace

// --- buffer allocation ------------------------------------------------------

void Window::Impl::release_buffers()
{
    for (Buffer &b : buffers) {
        if (b.buffer)
            wl_buffer_destroy(b.buffer);
        b.buffer = nullptr;
        b.data = nullptr;
        b.busy = false;
    }
    drawing = nullptr;

    if (pool) {
        wl_shm_pool_destroy(pool);
        pool = nullptr;
    }
    if (pool_fd >= 0) {
        ::close(pool_fd);
        pool_fd = -1;
    }
    pool_bytes = 0;
}

bool Window::Impl::allocate(std::string &error)
{
    release_buffers();

    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t one = stride * height;
    pool_bytes = one * buffers.size();

    pool_fd = memfd_create("esca-camview", MFD_CLOEXEC);
    if (pool_fd < 0) {
        error = "memfd_create failed";
        return false;
    }
    if (ftruncate(pool_fd, static_cast<off_t>(pool_bytes)) < 0) {
        error = "cannot size the shared buffer";
        return false;
    }

    void *base = mmap(nullptr, pool_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (base == MAP_FAILED) {
        error = "cannot map the shared buffer";
        return false;
    }

    pool = wl_shm_create_pool(shm, pool_fd, static_cast<int32_t>(pool_bytes));
    for (size_t i = 0; i < buffers.size(); ++i) {
        buffers[i].data = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(base) + one * i);
        buffers[i].bytes = one;
        buffers[i].busy = false;
        buffers[i].buffer = wl_shm_pool_create_buffer(
            pool, static_cast<int32_t>(one * i), width, height, static_cast<int32_t>(stride),
            WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(buffers[i].buffer, &kBufferListener, &buffers[i]);
        std::memset(buffers[i].data, 0, one);
    }
    return true;
}

// --- Window -----------------------------------------------------------------

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window() { close(); }

bool Window::open(const std::string &title, const std::string &app_id, std::string &error)
{
    impl_->display = wl_display_connect(nullptr);
    if (!impl_->display) {
        error = "cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)";
        return false;
    }

    impl_->registry = wl_display_get_registry(impl_->display);
    wl_registry_add_listener(impl_->registry, &kRegistryListener, impl_.get());
    wl_display_roundtrip(impl_->display);

    if (!impl_->compositor || !impl_->shm || !impl_->wm_base) {
        error = "compositor is missing wl_compositor, wl_shm or xdg_wm_base";
        return false;
    }
    xdg_wm_base_add_listener(impl_->wm_base, &kWmBaseListener, impl_.get());

    impl_->surface = wl_compositor_create_surface(impl_->compositor);
    impl_->xdg_surface_ = xdg_wm_base_get_xdg_surface(impl_->wm_base, impl_->surface);
    xdg_surface_add_listener(impl_->xdg_surface_, &kSurfaceListener, impl_.get());

    impl_->toplevel = xdg_surface_get_toplevel(impl_->xdg_surface_);
    xdg_toplevel_add_listener(impl_->toplevel, &kToplevelListener, impl_.get());
    xdg_toplevel_set_title(impl_->toplevel, title.c_str());
    xdg_toplevel_set_app_id(impl_->toplevel, app_id.c_str());
    xdg_toplevel_set_fullscreen(impl_->toplevel, nullptr);

    // The surface has to be committed empty once before the compositor will
    // send the configure that tells us what size to draw.
    wl_surface_commit(impl_->surface);
    wl_display_roundtrip(impl_->display);
    if (!impl_->configured)
        wl_display_roundtrip(impl_->display);

    if (!impl_->configured) {
        error = "compositor never configured the surface";
        return false;
    }
    return impl_->allocate(error);
}

void Window::close()
{
    if (!impl_)
        return;

    impl_->release_buffers();
    if (impl_->toplevel)
        xdg_toplevel_destroy(impl_->toplevel);
    if (impl_->xdg_surface_)
        xdg_surface_destroy(impl_->xdg_surface_);
    if (impl_->surface)
        wl_surface_destroy(impl_->surface);
    if (impl_->pointer)
        wl_pointer_release(impl_->pointer);
    if (impl_->touch)
        wl_touch_release(impl_->touch);
    if (impl_->seat)
        wl_seat_destroy(impl_->seat);
    if (impl_->wm_base)
        xdg_wm_base_destroy(impl_->wm_base);
    if (impl_->registry)
        wl_registry_destroy(impl_->registry);
    if (impl_->display)
        wl_display_disconnect(impl_->display);

    impl_->toplevel = nullptr;
    impl_->xdg_surface_ = nullptr;
    impl_->surface = nullptr;
    impl_->pointer = nullptr;
    impl_->touch = nullptr;
    impl_->seat = nullptr;
    impl_->wm_base = nullptr;
    impl_->registry = nullptr;
    impl_->display = nullptr;
}

bool Window::dispatch()
{
    if (!impl_->display)
        return false;

    // Non-blocking, because the camera paces this application, not the
    // compositor. dispatch_pending() alone is not enough: it drains the queue
    // but never reads the socket, so wl_buffer.release would never arrive and
    // both buffers would stay busy forever -- a picture frozen on the first
    // frame. The prepare/read/cancel dance is the documented way to do that
    // read without blocking.
    while (wl_display_prepare_read(impl_->display) != 0) {
        if (wl_display_dispatch_pending(impl_->display) < 0)
            return false;
    }
    if (wl_display_flush(impl_->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(impl_->display);
        return false;
    }

    pollfd pfd{ wl_display_get_fd(impl_->display), POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(impl_->display) < 0)
            return false;
    } else {
        wl_display_cancel_read(impl_->display);
    }

    if (wl_display_dispatch_pending(impl_->display) < 0)
        return false;

    // A configure may have changed the size while we were not looking.
    if (impl_->pending_width > 0 && impl_->pending_height > 0
        && (impl_->pending_width != impl_->width || impl_->pending_height != impl_->height)) {
        impl_->width = impl_->pending_width;
        impl_->height = impl_->pending_height;
        std::string error;
        if (!impl_->allocate(error)) {
            std::fprintf(stderr, "esca-camview: %s\n", error.c_str());
            return false;
        }
    }
    return true;
}

bool Window::closed() const { return impl_->closed; }

uint32_t *Window::acquire(int &width, int &height, int &stride_px)
{
    for (Buffer &b : impl_->buffers) {
        if (b.busy || !b.data)
            continue;
        impl_->drawing = &b;
        width = impl_->width;
        height = impl_->height;
        stride_px = impl_->width;
        return b.data;
    }
    return nullptr;
}

void Window::present()
{
    if (!impl_->drawing)
        return;

    impl_->drawing->busy = true;
    wl_surface_attach(impl_->surface, impl_->drawing->buffer, 0, 0);
    wl_surface_damage_buffer(impl_->surface, 0, 0, impl_->width, impl_->height);
    wl_surface_commit(impl_->surface);
    wl_display_flush(impl_->display);
    impl_->drawing = nullptr;
}

Input Window::take_input()
{
    Input snapshot = impl_->input;
    impl_->input.clicked = false;
    impl_->input.click_x = impl_->input.click_y = -1;
    return snapshot;
}

int Window::width() const { return impl_->width; }
int Window::height() const { return impl_->height; }

} // namespace esca::ui
