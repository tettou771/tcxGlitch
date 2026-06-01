#include "tcxGlitch.h"

// stb_image_write ships with TrussC core (compiled there with
// STB_IMAGE_WRITE_IMPLEMENTATION, no _STATIC). We only need the declarations
// of the *_to_func encoders so we can capture the encoded bytes in memory
// instead of writing a file — the implementation is linked from core.
#include "stb/stb_image_write.h"

#include <cmath>
#include <cstring>
#include <random>

using namespace std;
using namespace tc;

namespace tcx {

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------
namespace {

// stb write callback: append the produced bytes to a std::vector.
void appendToVector(void* context, void* data, int size) {
    auto* buf = static_cast<vector<uint8_t>*>(context);
    auto* src = static_cast<uint8_t*>(data);
    buf->insert(buf->end(), src, src + size);
}

// Read a 32-bit little-endian value (BMP headers).
uint32_t readLE32(const vector<uint8_t>& b, size_t i) {
    return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) |
           ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24);
}

// Read a 32-bit big-endian value (PNG chunk lengths).
uint32_t readBE32(const vector<uint8_t>& b, size_t i) {
    return ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
           ((uint32_t)b[i + 2] << 8) | (uint32_t)b[i + 3];
}

// Fill an Image with solid black (opaque) at the given size and push to GPU.
void fillBlack(Image& out, int w, int h) {
    w = max(w, 1);
    h = max(h, 1);
    // Reuse the existing GPU texture when the size is unchanged; reallocating a
    // sg_image every frame and sampling it within the same already-open render
    // pass is unreliable on Metal.
    if (out.getWidth() != w || out.getHeight() != h) out.allocate(w, h, 4);
    unsigned char* d = out.getPixelsData();
    const size_t n = (size_t)w * h * 4;
    for (size_t i = 0; i < n; i += 4) {
        d[i] = d[i + 1] = d[i + 2] = 0;
        d[i + 3] = 255;
    }
    // We wrote straight into the pixel buffer, so flag it dirty — update() only
    // re-uploads to the GPU texture when the Image is marked dirty.
    out.setDirty();
    out.update();
}

} // namespace

// -----------------------------------------------------------------------------
// Glitch base — pipeline + input conversion
// -----------------------------------------------------------------------------
bool Glitch::process(const Pixels& srcPixels, Image& out) {
    const int w = srcPixels.getWidth();
    const int h = srcPixels.getHeight();

    vector<uint8_t> bytes;
    if (!encode(srcPixels, bytes) || bytes.empty()) {
        fillBlack(out, w, h);
        return false;
    }

    corrupt(bytes);

    Pixels decoded;
    if (!decoded.loadFromMemory(bytes.data(), (int)bytes.size())) {
        // Corruption was fatal — wipe to black so the caller can retry.
        fillBlack(out, w, h);
        return false;
    }

    const int dw = decoded.getWidth();
    const int dh = decoded.getHeight();
    // Allocate only on a size change (see fillBlack note); otherwise just
    // refresh the pixels of the existing texture.
    if (out.getWidth() != dw || out.getHeight() != dh) out.allocate(dw, dh, 4);
    memcpy(out.getPixelsData(), decoded.getData(), (size_t)dw * dh * 4);
    // Flag dirty so update() actually re-uploads the buffer we just wrote.
    out.setDirty();
    out.update();
    return true;
}

bool Glitch::apply(const Image& src, Image& out) {
    return process(src.getPixels(), out);
}

bool Glitch::apply(const Fbo& src, Image& out) {
    const int w = src.getWidth();
    const int h = src.getHeight();
    Pixels p;
    p.allocate(w, h, 4);
    if (!src.readPixels(p.getData())) {
        fillBlack(out, w, h);
        return false;
    }
    return process(p, out);
}

bool Glitch::apply(const Texture& src, Image& out) {
    const int w = src.getWidth();
    const int h = src.getHeight();
    if (w <= 0 || h <= 0) {
        fillBlack(out, w, h);
        return false;
    }
    // Bounce the texture through a scratch FBO so we can read its pixels back.
    if (scratchFbo_.getWidth() != w || scratchFbo_.getHeight() != h) {
        scratchFbo_.allocate(w, h);
    }
    scratchFbo_.begin(0, 0, 0, 1);
    src.draw(0, 0, (float)w, (float)h);
    scratchFbo_.end();
    return apply(scratchFbo_, out);
}

bool Glitch::apply(Image& img) {
    // src and out are the same Image. process() fully consumes the source
    // pixels into the byte buffer (encode) before it ever writes to `out`,
    // so the aliasing is safe.
    return process(img.getPixels(), img);
}

// -----------------------------------------------------------------------------
// JpegGlitch
// -----------------------------------------------------------------------------
bool JpegGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    bytes.clear();
    int ok = stbi_write_jpg_to_func(appendToVector, &bytes,
                                    src.getWidth(), src.getHeight(),
                                    src.getChannels(), src.getData(), quality_);
    return ok != 0 && !bytes.empty();
}

void JpegGlitch::corrupt(vector<uint8_t>& b) {
    if (b.size() < 4) return;

    // Only disturb the entropy-coded scan, i.e. everything after the SOS
    // (Start Of Scan, 0xFFDA) marker and its segment header. Touching the
    // tables/headers before it tends to kill the decode outright.
    size_t start = 2;
    for (size_t i = 2; i + 1 < b.size(); ++i) {
        if (b[i] == 0xFF && b[i + 1] == 0xDA) {
            if (i + 4 <= b.size()) {
                size_t segLen = ((size_t)b[i + 2] << 8) | b[i + 3];
                start = i + 2 + segLen;
            } else {
                start = i + 2;
            }
            break;
        }
    }
    if (start + 2 >= b.size()) return;
    const size_t end = b.size() - 2; // leave the EOI (0xFFD9) intact

    mt19937 rng(seed_);
    uniform_int_distribution<size_t> pos(start, end - 1);
    uniform_int_distribution<int> val(0x00, 0xFE); // never write 0xFF

    // stb's JPEG decoder is strict: it bails on the first invalid Huffman code,
    // so the usable window is small (~1% of the scan body before it mostly
    // fails to decode). Map amount geometrically (log scale) into that window:
    // count = maxCount^amount. Equal slider steps give roughly equal perceptual
    // change, with fine control in the subtle low end; amount=1 is the
    // near-destruction edge.
    const double maxCount = 0.012 * (double)(end - start);
    size_t count = 0;
    if (amount_ > 0.0f && maxCount > 1.0) count = (size_t)pow(maxCount, (double)amount_);

    for (size_t k = 0; k < count; ++k) {
        size_t i = pos(rng);
        // Don't disturb a byte stuffed right after a 0xFF marker prefix, and
        // never introduce a new 0xFF (would forge a marker and break decode).
        if (b[i - 1] == 0xFF) continue;
        b[i] = (uint8_t)val(rng);
    }
}

// -----------------------------------------------------------------------------
// BmpGlitch
// -----------------------------------------------------------------------------
bool BmpGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    bytes.clear();
    int ok = stbi_write_bmp_to_func(appendToVector, &bytes,
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
    uniform_int_distribution<int> replaceOrFlip(0, 1);

    // BMP is uncompressed, so it always decodes — every byte is a pixel channel.
    // We mix three operators for a richer look than plain snow:

    // (1) Byte drops (data loss). Delete a short run and shift the tail up; the
    //     row alignment cascades below the cut into a diagonal tear / channel
    //     shift — the classic "missing data" databend. Bounded, since each is an
    //     O(n) memmove. (Byte-level = clean shift; for a harsher full scramble a
    //     bit-level drop would shift the whole bitstream — not used here.)
    const size_t cuts = (size_t)(amount_ * 24.0);
    uniform_int_distribution<int> dropLen(1, 6);
    for (size_t k = 0; k < cuts; ++k) {
        size_t i = pos(rng);
        size_t d = (size_t)dropLen(rng);
        if (i + d < end) memmove(&b[i], &b[i + d], end - i - d);
    }

    // (2)+(3) Scattered byte replaces (colour speckles) and bit flips (subtle
    //     per-channel noise). Cheap, in-place.
    const size_t count = (size_t)(amount_ * 0.06 * range);
    for (size_t k = 0; k < count; ++k) {
        size_t i = pos(rng);
        if (replaceOrFlip(rng) == 0) b[i] = (uint8_t)byteVal(rng);
        else b[i] ^= (uint8_t)(1 << bitSel(rng));
    }
}

// -----------------------------------------------------------------------------
// PngGlitch
// -----------------------------------------------------------------------------
bool PngGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    bytes.clear();
    const int stride = src.getWidth() * src.getChannels();
    int ok = stbi_write_png_to_func(appendToVector, &bytes,
                                    src.getWidth(), src.getHeight(),
                                    src.getChannels(), src.getData(), stride);
    return ok != 0 && !bytes.empty();
}

void PngGlitch::corrupt(vector<uint8_t>& b) {
    // 8-byte signature + at least one IHDR (25) + IEND (12).
    if (b.size() < 8 + 25 + 12) return;

    // Walk the chunk list to find the first IDAT's data region.
    size_t start = 8;
    for (size_t i = 8; i + 8 <= b.size();) {
        uint32_t len = readBE32(b, i);
        const uint8_t* type = &b[i + 4];
        bool isIDAT = type[0] == 'I' && type[1] == 'D' &&
                      type[2] == 'A' && type[3] == 'T';
        if (isIDAT) {
            start = i + 8; // skip length(4) + type(4) -> data starts here
            break;
        }
        i += 8 + (size_t)len + 4; // length + type + data + crc
    }
    if (start + 4 >= b.size()) return;
    const size_t end = b.size() - 12; // leave the trailing IEND chunk alone
    if (end <= start) return;

    mt19937 rng(seed_);
    uniform_int_distribution<size_t> pos(start, end - 1);
    uniform_int_distribution<int> val(0, 255);
    // PNG's IDAT is a zlib/DEFLATE stream: almost any change makes inflate fail,
    // so this codec collapses to black most of the time (by design — the wild
    // one). The scale is kept small; even a single hit is usually fatal.
    size_t count = (size_t)(amount_ * 0.02 * (double)(end - start));
    if (count < 1 && amount_ > 0.0f) count = 1;

    for (size_t k = 0; k < count; ++k) {
        b[pos(rng)] = (uint8_t)val(rng);
    }
}

} // namespace tcx
