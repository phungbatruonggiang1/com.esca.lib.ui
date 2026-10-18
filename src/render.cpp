#include "esca/ui/render.hpp"

#include <algorithm>

#include "esca/ui/font5x7.hpp"

namespace esca::ui {

namespace {

inline uint32_t pack(int r, int g, int b)
{
    return 0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8)
           | static_cast<uint32_t>(b);
}

inline int clamp_byte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/// Size of the image after rotation, in source pixels.
void rotated_size(int width, int height, int rotation, int &out_w, int &out_h)
{
    if (rotation == 90 || rotation == 270) {
        out_w = height;
        out_h = width;
    } else {
        out_w = width;
        out_h = height;
    }
}

} // namespace

Viewport blit_yuv420(const uint8_t *yuv, int width, int height, int stride, int rotation,
                     uint32_t *dst, int dst_w, int dst_h, int dst_stride_px)
{
    Viewport view;
    view.rotation = rotation;

    int rot_w = 0;
    int rot_h = 0;
    rotated_size(width, height, rotation, rot_w, rot_h);
    if (rot_w <= 0 || rot_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return view;

    // Letterbox: the largest centred rectangle with the image's aspect ratio.
    const int scaled_h = dst_w * rot_h / rot_w;
    if (scaled_h <= dst_h) {
        view.area = { 0, (dst_h - scaled_h) / 2, dst_w, scaled_h };
    } else {
        const int scaled_w = dst_h * rot_w / rot_h;
        view.area = { (dst_w - scaled_w) / 2, 0, scaled_w, dst_h };
    }

    const uint8_t *y_plane = yuv;
    const int chroma_stride = stride / 2;
    const int chroma_height = height / 2;
    const uint8_t *u_plane = y_plane + static_cast<size_t>(stride) * height;
    const uint8_t *v_plane = u_plane + static_cast<size_t>(chroma_stride) * chroma_height;

    // The bars around a letterboxed image, painted every frame: the previous
    // frame may have been a different shape after a resize.
    for (int y = 0; y < dst_h; ++y) {
        if (y >= view.area.y && y < view.area.y + view.area.h) {
            uint32_t *row = dst + static_cast<size_t>(y) * dst_stride_px;
            std::fill(row, row + view.area.x, 0xff000000u);
            std::fill(row + view.area.x + view.area.w, row + dst_w, 0xff000000u);
        } else {
            uint32_t *row = dst + static_cast<size_t>(y) * dst_stride_px;
            std::fill(row, row + dst_w, 0xff000000u);
        }
    }

    for (int dy = 0; dy < view.area.h; ++dy) {
        uint32_t *out = dst + static_cast<size_t>(view.area.y + dy) * dst_stride_px + view.area.x;

        for (int dx = 0; dx < view.area.w; ++dx) {
            // Position within the rotated image, then back to the source.
            // Nearest neighbour: a few milliseconds and no dependency, and
            // the source is larger than the screen area in one axis anyway.
            const int rx = dx * rot_w / view.area.w;
            const int ry = dy * rot_h / view.area.h;

            int sx = 0;
            int sy = 0;
            switch (rotation) {
            case 90:  sx = ry;              sy = height - 1 - rx; break;
            case 180: sx = width - 1 - rx;  sy = height - 1 - ry; break;
            case 270: sx = width - 1 - ry;  sy = rx;              break;
            default:  sx = rx;              sy = ry;              break;
            }

            const int y = y_plane[static_cast<size_t>(sy) * stride + sx];
            const int u = u_plane[static_cast<size_t>(sy / 2) * chroma_stride + sx / 2] - 128;
            const int v = v_plane[static_cast<size_t>(sy / 2) * chroma_stride + sx / 2] - 128;

            // BT.601, the conversion libcamera's YUV420 output follows.
            const int r = y + ((91881 * v) >> 16);
            const int g = y - ((22554 * u + 46802 * v) >> 16);
            const int b = y + ((116130 * u) >> 16);

            out[dx] = pack(clamp_byte(r), clamp_byte(g), clamp_byte(b));
        }
    }

    return view;
}

Rect map_box(const Viewport &view, double left, double top, double right, double bottom)
{
    // Rotate the normalised corners the same way the pixels were rotated.
    auto rotate_point = [&](double x, double y, double &u, double &v) {
        switch (view.rotation) {
        case 90:  u = 1.0 - y; v = x;       break;
        case 180: u = 1.0 - x; v = 1.0 - y; break;
        case 270: u = y;       v = 1.0 - x; break;
        default:  u = x;       v = y;       break;
        }
    };

    double u1 = 0, v1 = 0, u2 = 0, v2 = 0;
    rotate_point(left, top, u1, v1);
    rotate_point(right, bottom, u2, v2);

    // Rotation can swap which corner is which.
    if (u1 > u2) std::swap(u1, u2);
    if (v1 > v2) std::swap(v1, v2);

    auto to_x = [&](double u) {
        const int px = view.area.x + static_cast<int>(u * view.area.w);
        return std::clamp(px, view.area.x, view.area.x + view.area.w);
    };
    auto to_y = [&](double v) {
        const int py = view.area.y + static_cast<int>(v * view.area.h);
        return std::clamp(py, view.area.y, view.area.y + view.area.h);
    };

    const int x1 = to_x(u1);
    const int y1 = to_y(v1);
    return { x1, y1, to_x(u2) - x1, to_y(v2) - y1 };
}

void fill_rect(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, const Rect &r,
               uint32_t colour)
{
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(dst_w, r.x + r.w);
    const int y1 = std::min(dst_h, r.y + r.h);

    // Clamping alone is not enough. A rectangle that starts past the right
    // edge leaves x0 > x1, and std::fill with first > last does not draw
    // nothing -- it runs off the end of the buffer until it segfaults. That
    // is what a caption on a detection near the right edge used to do.
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int y = y0; y < y1; ++y) {
        uint32_t *row = dst + static_cast<size_t>(y) * dst_stride_px;
        std::fill(row + x0, row + x1, colour);
    }
}

void stroke_rect(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, const Rect &r,
                 uint32_t colour, int thickness)
{
    if (r.w <= 0 || r.h <= 0)
        return;

    const int t = std::max(1, std::min(thickness, std::min(r.w, r.h) / 2));
    fill_rect(dst, dst_w, dst_h, dst_stride_px, { r.x, r.y, r.w, t }, colour);
    fill_rect(dst, dst_w, dst_h, dst_stride_px, { r.x, r.y + r.h - t, r.w, t }, colour);
    fill_rect(dst, dst_w, dst_h, dst_stride_px, { r.x, r.y, t, r.h }, colour);
    fill_rect(dst, dst_w, dst_h, dst_stride_px, { r.x + r.w - t, r.y, t, r.h }, colour);
}

int draw_text(uint32_t *dst, int dst_w, int dst_h, int dst_stride_px, int x, int y,
              const std::string &text, uint32_t colour, int scale)
{
    const int s = std::max(1, scale);
    int pen = x;

    for (const char c : text) {
        const uint8_t *rows = glyph_rows(c);
        if (rows) {
            for (int gy = 0; gy < kGlyphHeight; ++gy) {
                const uint8_t bits = rows[gy];
                for (int gx = 0; gx < kGlyphWidth; ++gx) {
                    if (!(bits & (1u << (kGlyphWidth - 1 - gx))))
                        continue;
                    fill_rect(dst, dst_w, dst_h, dst_stride_px,
                              { pen + gx * s, y + gy * s, s, s }, colour);
                }
            }
        }
        pen += (kGlyphWidth + kGlyphSpacing) * s;
    }

    return pen - x;
}

int text_width(const std::string &text, int scale)
{
    const int s = std::max(1, scale);
    return static_cast<int>(text.size()) * (kGlyphWidth + kGlyphSpacing) * s;
}

int text_height(int scale) { return kGlyphHeight * std::max(1, scale); }

uint32_t colour_for_class(uint16_t class_id)
{
    // A fixed spread around the hue circle. Deterministic, so a class keeps
    // its colour between runs, and bright enough to read over video.
    static constexpr uint32_t kPalette[] = {
        0xff4fc3f7, 0xffaed581, 0xffffb74d, 0xffe57373, 0xffba68c8,
        0xff4db6ac, 0xffffd54f, 0xff90a4ae, 0xfff06292, 0xff7986cb,
    };
    return kPalette[class_id % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

} // namespace esca::ui
