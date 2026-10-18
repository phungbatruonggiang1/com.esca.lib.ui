// A fullscreen Wayland surface backed by shared memory.
//
// No GPU, no toolkit. The frame is already in system memory when the camera
// service hands it over, converting it costs a few milliseconds of CPU, and
// wl_shm hands the result straight to the compositor. Bringing in EGL would
// buy nothing at 30 fps and 1280x720, and would cost a dependency on the
// Mesa stack.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace esca::ui {

/// What the pointer or the finger did since the last frame.
///
/// Deliberately not a widget system. An application here has a handful of
/// rectangles to press, and asking "was this rectangle clicked" each frame
/// is the whole of what it needs; callbacks, focus and a retained tree
/// would be machinery with nothing to carry.
struct Input {
    bool has_pointer = false; ///< a mouse is present
    bool has_touch = false;   ///< a touchscreen is present
    int x = -1;               ///< last known position, -1 when outside
    int y = -1;
    bool down = false;        ///< held at this instant
    bool clicked = false;     ///< pressed and released since the last frame
    int click_x = -1;         ///< where that click landed
    int click_y = -1;
};

class Window {
public:
    Window();
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    /// Connect to the compositor and map a fullscreen surface. Blocks until
    /// the compositor has told us how big it is.
    bool open(const std::string &title, const std::string &app_id, std::string &error);
    void close();

    /// Handle whatever the compositor has sent. Returns false when the
    /// connection is gone, which is how a viewer notices weston restarting.
    bool dispatch();

    /// True once the compositor or the user has asked the window to go away.
    bool closed() const;

    /// A buffer to draw the next frame into, or nullptr when both buffers are
    /// still held by the compositor -- in which case this frame is skipped
    /// rather than waited for, because the next camera frame is 33 ms away
    /// and more current anyway.
    uint32_t *acquire(int &width, int &height, int &stride_px);

    /// Hand the acquired buffer to the compositor.
    void present();

    int width() const;
    int height() const;

    /// Input seen since the last take_input(). Reading it clears the
    /// one-shot fields, so a click is delivered exactly once however many
    /// times the frame loop asks.
    Input take_input();

    /// True when the point is inside the rectangle, for hit testing.
    static bool hit(const Input &input, int x, int y, int w, int h)
    {
        return input.clicked && input.click_x >= x && input.click_x < x + w
               && input.click_y >= y && input.click_y < y + h;
    }

    /// Defined in window.cpp. Public only because Wayland listeners are free
    /// functions handed a void*, so they have to be able to name this type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::ui
