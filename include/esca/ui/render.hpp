// Everything that writes pixels into the Wayland buffer.
//
// The buffer is XRGB8888, which is what wl_shm gives us and what the GPU
// scans out; nothing here needs a drawing library. The two jobs are putting
// the camera frame on screen the right way up, and drawing boxes over it.

#pragma once

#include <cstdint>
#include <string>

namespace esca::ui {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// Where a normalised point sits once rotation and letterboxing are applied.
struct Viewport {
    Rect area;        ///< the part of the window the image occupies
    int rotation = 0; ///< 0, 90, 180 or 270 degrees clockwise
};

/// Draws a planar YUV420 frame into an XRGB8888 buffer, rotating by
/// `rotation` degrees clockwise, scaling to fit and centring what is left
/// over. The area the image ended up in is returned, because detection boxes
/// have to land in the same place.
///
/// @param stride  bytes per luma row in the source; chroma rows are half that
/// @param dst_stride_px  pixels per row in the destination, not bytes
Viewport blit_yuv420(const uint8_t *yuv, int width, int height, int stride, int rotation,
                     uint32_t *dst, int dst_w, int dst_h, int dst_stride_px);

/// Maps a box in source-frame coordinates (0..1, before rotation) to pixels
/// in the window. Boxes from the detector can stick slightly outside 0..1,
/// so the result is clamped to the viewport.
Rect map_box(const Viewport &view, double left, double top, double right, double bottom);

void fill_rect(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, const Rect &r,
               uint32_t colour);

/// An outline, drawn inward from the rectangle's edges.
void stroke_rect(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, const Rect &r,
                 uint32_t colour, int thickness);

/// Draws text with the embedded 5x7 font, magnified `scale` times. Returns
/// the width in pixels of what was drawn.
int draw_text(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, int x, int y,
              const std::string &text, uint32_t colour, int scale);

/// Width in pixels the same call would occupy, for laying a label out before
/// drawing it.
int text_width(const std::string &text, int scale);
int text_height(int scale);

/// A colour per class, so two objects of different kinds do not share a box
/// colour and the eye can follow one of them between frames.
uint32_t colour_for_class(uint16_t class_id);

} // namespace esca::ui
