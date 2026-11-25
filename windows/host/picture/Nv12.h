#pragma once

// Small NV12 helpers for the host side of the picture path. NV12 here is always packed
// (stride == width): a full-size luma plane followed by interleaved, half-size U/V.

#include <cstdint>
#include <cstring>
#include <vector>

#include "picture/Framing.h"

namespace phonecam::picture {

inline size_t Nv12Size(uint32_t width, uint32_t height) { return static_cast<size_t>(width) * height * 3 / 2; }

// crop must be even-aligned and inside the source, which ComputeCrop guarantees.
inline void CropNv12(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, const Crop& crop,
                     uint8_t* destination) {
    const uint8_t* luma = source + static_cast<size_t>(crop.y) * sourceWidth + crop.x;
    for (uint32_t row = 0; row < crop.height; ++row) {
        std::memcpy(destination, luma, crop.width);
        destination += crop.width;
        luma += sourceWidth;
    }
    const uint8_t* chroma =
        source + static_cast<size_t>(sourceHeight) * sourceWidth + static_cast<size_t>(crop.y / 2) * sourceWidth + crop.x;
    for (uint32_t row = 0; row < crop.height / 2; ++row) {
        std::memcpy(destination, chroma, crop.width);
        destination += crop.width;
        chroma += sourceWidth;
    }
}

namespace detail {
inline uint8_t ClampToByte(int value) { return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value)); }
}  // namespace detail

// step 1 = every pixel, step 2 = half size (2x2 luma average). BT.709 limited range, the same
// coefficients windows/vcam/Tools.cpp uses for HD, so the preview's colors match the camera's.
inline void Nv12ToBgra(const uint8_t* source, uint32_t width, uint32_t height, uint32_t step, uint32_t& outWidth,
                       uint32_t& outHeight, std::vector<uint8_t>& bgra) {
    outWidth = width / step;
    outHeight = height / step;
    bgra.resize(static_cast<size_t>(outWidth) * outHeight * 4);
    const uint8_t* chromaPlane = source + static_cast<size_t>(height) * width;
    uint8_t* out = bgra.data();
    for (uint32_t row = 0; row < outHeight; ++row) {
        const uint32_t sourceRow = row * step;
        const uint8_t* luma = source + static_cast<size_t>(sourceRow) * width;
        const uint8_t* chroma = chromaPlane + static_cast<size_t>(sourceRow / 2) * width;
        for (uint32_t column = 0; column < outWidth; ++column) {
            const uint32_t x = column * step;
            const int y = step == 1 ? luma[x] : (luma[x] + luma[x + 1] + luma[x + width] + luma[x + width + 1] + 2) / 4;
            const uint32_t pair = x & ~1u;
            const int c = 298 * (y - 16) + 128;
            const int d = chroma[pair] - 128;
            const int e = chroma[pair + 1] - 128;
            out[0] = detail::ClampToByte((c + 541 * d) >> 8);
            out[1] = detail::ClampToByte((c - 55 * d - 136 * e) >> 8);
            out[2] = detail::ClampToByte((c + 459 * e) >> 8);
            out[3] = 255;
            out += 4;
        }
    }
}

}  // namespace phonecam::picture
