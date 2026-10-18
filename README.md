# libesca-ui

The windowing and drawing the ESCA applications share: a fullscreen Wayland
surface backed by shared memory, a software renderer, a 5x7 bitmap font, and
enough input to press a button.

Extracted from `esca-camview` once a second application needed it. No
toolkit: the frame is already in system memory when a service hands it over,
converting it costs a few milliseconds of CPU, and `wl_shm` passes the
result straight to the compositor. EGL would buy nothing at this size and
cost a dependency on the whole Mesa stack, and Flutter needs an engine that
does not fit on the disk this is built on.

## Input is not a widget system

An application here has a handful of large rectangles to press. Asking
"was this rectangle clicked this frame" is all it needs:

```cpp
const Input in = window.take_input();
if (Window::hit(in, x, y, w, h))
    start_listening();
```

Pointer and touch both feed it, because the board may have either. A click
is a press *and* a release, and a press that wanders off the rectangle
before release still counts -- which is what a finger does, and close
enough for buttons this size. `take_input()` clears the one-shot fields, so
a click is delivered exactly once however many times the loop asks.

## Contents

    include/esca/ui/window.hpp   fullscreen surface, two shm buffers, input
    include/esca/ui/render.hpp   rectangles, outlines, text, YUV420 blitting
    include/esca/ui/font5x7.hpp  the font

`window.cpp` dispatches Wayland without blocking, using
prepare_read/read_events/cancel_read. `wl_display_dispatch_pending` on its
own is not enough -- it drains the queue but never reads the socket, so
`wl_buffer.release` never arrives, both buffers stay busy, and the picture
freezes on the first frame while the process sits at 4% CPU looking healthy.
