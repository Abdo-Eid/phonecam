#pragma once

// How the decoded phone frame becomes the picture video apps receive.
//
// The user chooses a shape (1:1, 16:9, ...). That decides two things, both made here:
//   - the one size the virtual camera offers, so every app gets exactly that shape, and
//   - the crop: which part of the phone's picture fills it.
// Cropping happens in the host, before the frame enters the ring. The virtual camera DLL
// (windows/vcam/FrameGenerator) then rotates, mirrors and scales whatever it is given to fit the
// app's frame; because the crop already has that shape, "fit" leaves no black bars.
//
// Pure functions, no Windows calls: covered by PictureTests.cpp.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>

#include "shm/SharedFrameRing.h"

namespace phonecam::picture {

using phonecam::shm::Rotation;

// Saved by numeric value: append only.
enum class Shape : uint32_t {
    Follow = 0,        // tall while the phone is upright, wide while it is sideways
    Wide = 1,          // 16:9
    Classic = 2,       // 4:3
    Square = 3,        // 1:1
    ClassicTall = 4,   // 3:4
    Tall = 5,          // 9:16
};
inline constexpr Shape kLastShape = Shape::Tall;

// The smallest crop, as a share of the largest one that fits.
inline constexpr float kMinCropSize = 0.2f;

// What the user chose. Positions and turns are in terms of the picture AS SEEN, never in sensor
// axes, so they keep their meaning when the phone is turned or the picture mirrored.
struct Picture {
    Rotation turn = Rotation::Deg0;  // an extra turn on top of what the phone's position needs
    bool mirror = false;
    Shape shape = Shape::Follow;
    bool compact = false;     // offer the lighter of the two sizes of this shape
    float cropSize = 1.0f;    // 1 = as much as fits
    float cropX = 0.5f;       // 0 = left .. 1 = right
    float cropY = 0.5f;       // 0 = top .. 1 = bottom

    bool operator==(const Picture&) const = default;
};

struct Size {
    uint32_t width = 0, height = 0;
    bool operator==(const Size&) const = default;
};

struct Crop {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Crop&) const = default;
};

// A rectangle of the picture as seen, in fractions of its width and height.
struct SeenRect {
    float left = 0, top = 0, width = 1, height = 1;
};

// A frame from the phone and how it is turned to be seen.
struct View {
    Size source;
    Rotation rotation = Rotation::Deg0;
    bool mirror = false;

    bool Swaps() const { return rotation == Rotation::Deg90 || rotation == Rotation::Deg270; }
    Size Seen() const { return Swaps() ? Size{source.height, source.width} : source; }
};

inline Rotation Combine(Rotation a, Rotation b) {
    return static_cast<Rotation>((static_cast<uint32_t>(a) + static_cast<uint32_t>(b)) % 4);
}

// The size the virtual camera offers. Every width is a multiple of 128 (64 for compact Tall):
// Windows' DirectShow bridge for virtual cameras, which OBS and classic Zoom go through, garbles
// rows whose byte length isn't aligned. That is why Tall is 1152x2048 rather than 1080x1920 --
// the same 9:16, on an aligned width. See docs/architecture.md, "Portrait through DirectShow".
inline Size OutputSize(const Picture& picture, const View& view) {
    Shape shape = picture.shape;
    if (shape == Shape::Follow) {
        const Size seen = view.Seen();
        shape = seen.height > seen.width ? Shape::Tall : Shape::Wide;
    }
    switch (shape) {
        case Shape::Classic: return picture.compact ? Size{1024, 768} : Size{1408, 1056};
        case Shape::Square: return picture.compact ? Size{768, 768} : Size{1152, 1152};
        case Shape::ClassicTall: return picture.compact ? Size{768, 1024} : Size{1152, 1536};
        case Shape::Tall: return picture.compact ? Size{576, 1024} : Size{1152, 2048};
        case Shape::Wide:
        case Shape::Follow: break;
    }
    return picture.compact ? Size{1280, 720} : Size{1920, 1080};
}

// Whether moving along a sensor axis moves left/up in the picture as seen. Follows the DLL's
// order: rotate clockwise ((x, y) -> (-y, x) per quarter turn), then mirror left-right.
inline bool RunsBackwards(const View& view, bool sensorX) {
    bool backwards = false;
    switch (view.rotation) {
        case Rotation::Deg0: backwards = false; break;
        case Rotation::Deg90: backwards = !sensorX; break;
        case Rotation::Deg180: backwards = true; break;
        case Rotation::Deg270: backwards = sensorX; break;
    }
    const bool horizontal = sensorX != view.Swaps();
    return horizontal && view.mirror ? !backwards : backwards;
}

// The part of the source frame to publish. Always inside the frame, even-aligned (NV12 stores
// color per 2x2 block) and EXACTLY the output's proportions: rounding to the nearest even size
// would leave a faint one-pixel black line, more visible than the couple of percent an exact
// ratio gives up.
inline Crop ComputeCrop(const View& view, const Picture& picture, Size output) {
    const Crop full{0, 0, view.source.width, view.source.height};
    if (view.source.width == 0 || view.source.height == 0 || output.width == 0 || output.height == 0) return full;

    Size aspect = view.Swaps() ? Size{output.height, output.width} : output;
    const uint32_t divisor = std::gcd(aspect.width, aspect.height);
    aspect.width /= divisor;
    aspect.height /= divisor;
    const uint32_t step = ((aspect.width | aspect.height) & 1u) ? 2 : 1;  // keeps both sides even

    const uint32_t largest = std::min(view.source.width / aspect.width, view.source.height / aspect.height) / step;
    if (largest == 0) return full;  // a shape this frame cannot express; showing it all beats showing nothing
    const float wanted = std::clamp(picture.cropSize, kMinCropSize, 1.0f) * static_cast<float>(largest);
    const uint32_t scale = std::clamp(static_cast<uint32_t>(wanted + 0.5f), 1u, largest) * step;

    Crop crop{0, 0, aspect.width * scale, aspect.height * scale};
    const auto place = [&](uint32_t room, bool sensorX) {
        const bool horizontal = sensorX != view.Swaps();
        const float seen = std::clamp(horizontal ? picture.cropX : picture.cropY, 0.0f, 1.0f);
        const float along = RunsBackwards(view, sensorX) ? 1.0f - seen : seen;
        const auto offset = static_cast<uint32_t>(static_cast<float>(room) * along + 0.5f) & ~1u;
        return std::min(offset, room & ~1u);
    };
    crop.x = place(view.source.width - crop.width, true);
    crop.y = place(view.source.height - crop.height, false);
    return crop;
}

// Where a crop lies in the picture as seen.
inline SeenRect ToSeen(const View& view, const Crop& crop) {
    if (view.source.width == 0 || view.source.height == 0) return {};
    const auto span = [](uint32_t start, uint32_t length, uint32_t total, bool backwards) {
        const float size = static_cast<float>(length) / static_cast<float>(total);
        const float begin = static_cast<float>(start) / static_cast<float>(total);
        return std::pair{backwards ? 1.0f - begin - size : begin, size};
    };
    const auto x = span(crop.x, crop.width, view.source.width, RunsBackwards(view, true));
    const auto y = span(crop.y, crop.height, view.source.height, RunsBackwards(view, false));
    const auto& horizontal = view.Swaps() ? y : x;
    const auto& vertical = view.Swaps() ? x : y;
    return {horizontal.first, vertical.first, horizontal.second, vertical.second};
}

// --- editing the crop on the preview ---

// The crop box at its largest, i.e. cropSize 1.
inline SeenRect LargestCrop(const View& view, const Picture& picture, Size output) {
    Picture largest = picture;
    largest.cropSize = 1.0f;
    return ToSeen(view, ComputeCrop(view, largest, output));
}

// Puts the box's top left corner at a point of the picture.
inline void MoveCropTo(Picture& picture, const SeenRect& box, float left, float top) {
    const auto position = [](float start, float length) {
        const float room = 1.0f - length;
        return room > 0.0005f ? std::clamp(start / room, 0.0f, 1.0f) : 0.5f;
    };
    picture.cropX = position(left, box.width);
    picture.cropY = position(top, box.height);
}

// Resizes the box by dragging one corner to a point; the opposite corner stays where it is.
// left/top: which corner is being dragged.
inline void ResizeCrop(Picture& picture, const SeenRect& box, const SeenRect& largest, bool left, bool top,
                       float pointX, float pointY) {
    if (largest.width <= 0 || largest.height <= 0) return;
    const float anchorX = left ? box.left + box.width : box.left;
    const float anchorY = top ? box.top + box.height : box.top;
    // Follow whichever direction the hand moved further in, so the corner keeps up with it.
    const float byX = (left ? anchorX - pointX : pointX - anchorX) / largest.width;
    const float byY = (top ? anchorY - pointY : pointY - anchorY) / largest.height;
    const float roomX = (left ? anchorX : 1.0f - anchorX) / largest.width;
    const float roomY = (top ? anchorY : 1.0f - anchorY) / largest.height;
    const float limit = std::min({1.0f, roomX, roomY});
    picture.cropSize = std::clamp(std::max(byX, byY), std::min(kMinCropSize, limit), std::max(limit, kMinCropSize));
    const SeenRect sized{0, 0, largest.width * picture.cropSize, largest.height * picture.cropSize};
    MoveCropTo(picture, sized, left ? anchorX - sized.width : anchorX, top ? anchorY - sized.height : anchorY);
}

// Grows or shrinks the box around its own center (wheel, keyboard, slider).
inline void ScaleCrop(Picture& picture, const SeenRect& box, const SeenRect& largest, float size) {
    const float centerX = box.left + box.width / 2, centerY = box.top + box.height / 2;
    picture.cropSize = std::clamp(size, kMinCropSize, 1.0f);
    const SeenRect sized{0, 0, largest.width * picture.cropSize, largest.height * picture.cropSize};
    MoveCropTo(picture, sized, centerX - sized.width / 2, centerY - sized.height / 2);
}

}  // namespace phonecam::picture
