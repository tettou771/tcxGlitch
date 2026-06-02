#include "BmpGlitch.h"

// stb_image_write ships with TrussC core; we only need the *_to_func
// declaration to capture the encoded BMP in memory.
#include "stb/stb_image_write.h"

#include <cstring>
#include <random>

using namespace std;
using namespace tc;

namespace tcx {

namespace {
// Read a 32-bit little-endian value (BMP headers).
uint32_t readLE32(const vector<uint8_t>& b, size_t i) {
    return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) |
           ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24);
}
} // namespace

bool BmpGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    bytes.clear();
    int ok = stbi_write_bmp_to_func(detail::appendToVector, &bytes,
                                    src.getWidth(), src.getHeight(),
                                    src.getChannels(), src.getData());
    return ok != 0 && !bytes.empty();
}

void BmpGlitch::corrupt(vector<uint8_t>& b) {
    if (b.size() < 14) return;

    // bfOffBits (offset to the pixel array) lives at byte 10 of the file
    // header. Reading it keeps us robust to whichever info-header variant stb
    // emitted for 32-bit output.
    uint32_t off = readLE32(b, 10);
    if (off >= b.size()) off = 54; // fallback: classic 14 + 40 header
    const size_t start = off;
    const size_t end = b.size();
    if (start >= end) return;
    const double range = (double)(end - start);

    mt19937 rng(seed_);
    uniform_int_distribution<size_t> pos(start, end - 1);
    uniform_int_distribution<int> byteVal(0, 255);
    uniform_int_distribution<int> bitSel(0, 7);
    uniform_int_distribution<int> dropLen13(1, 3);
    uniform_int_distribution<int> unitN(1, 64);

    // BMP is uncompressed, so it always decodes — every byte is a pixel channel.
    // Five independent operators, each driven by its own knob. The three "drop"
    // operators delete a run and shift the tail (an O(n) memmove / bit-shift);
    // the granularity of the delete is what decides the colour behaviour.

    // Tear: delete 1-3 bytes -> not pixel-aligned, so channels rotate below the
    // cut into uniform hue-shift bands.
    {
        const size_t cuts = (size_t)(tear_ * 20.0);
        for (size_t k = 0; k < cuts; ++k) {
            size_t i = pos(rng), d = (size_t)dropLen13(rng);
            if (i + d < end) memmove(&b[i], &b[i + d], end - i - d);
        }
    }
    // Shift: delete N*8 bytes (multiple of the 4-byte pixel) -> colours intact,
    // position jumps (jagged offset).
    {
        const size_t cuts = (size_t)(shift_ * 20.0);
        for (size_t k = 0; k < cuts; ++k) {
            size_t i = pos(rng), d = (size_t)unitN(rng) * 8;
            if (i + d < end) memmove(&b[i], &b[i + d], end - i - d);
        }
    }
    // Rainbow: delete N 4-bit nibbles. Odd N shifts the tail half a byte, so each
    // following byte is rebuilt from two source nibbles -> false-colour contour.
    {
        const size_t cuts = (size_t)(rainbow_ * 20.0);
        for (size_t k = 0; k < cuts; ++k) {
            size_t i = pos(rng), N = (size_t)unitN(rng), t = N / 2;
            if (N & 1) {
                for (size_t p = i; p + t + 1 < end; ++p)
                    b[p] = (uint8_t)(((b[p + t] & 0x0F) << 4) | (b[p + t + 1] >> 4));
            } else if (t > 0 && i + t < end) {
                memmove(&b[i], &b[i + t], end - i - t);
            }
        }
    }
    // Speckle: random byte replacement -> colour dots.
    {
        const size_t count = (size_t)(speckle_ * 0.10 * range);
        for (size_t k = 0; k < count; ++k) b[pos(rng)] = (uint8_t)byteVal(rng);
    }
    // Noise: random bit flips -> subtle per-channel noise.
    {
        const size_t count = (size_t)(noise_ * 0.10 * range);
        for (size_t k = 0; k < count; ++k) b[pos(rng)] ^= (uint8_t)(1 << bitSel(rng));
    }
}

} // namespace tcx
