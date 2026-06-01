#include "tcxGlitch.h"

// stb_image_write ships with TrussC core (compiled there with
// STB_IMAGE_WRITE_IMPLEMENTATION, no _STATIC). We only need the declarations
// of the *_to_func encoders so we can capture the encoded bytes in memory
// instead of writing a file — the implementation is linked from core.
#include "stb/stb_image_write.h"

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
    out.allocate(w, h, 4);
    unsigned char* d = out.getPixelsData();
    const size_t n = (size_t)w * h * 4;
    for (size_t i = 0; i < n; i += 4) {
        d[i] = d[i + 1] = d[i + 2] = 0;
        d[i + 3] = 255;
    }
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
    out.allocate(dw, dh, 4);
    memcpy(out.getPixelsData(), decoded.getData(), (size_t)dw * dh * 4);
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
    const size_t count = (size_t)(amount_ * (double)(end - start));

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

    mt19937 rng(seed_);
    uniform_int_distribution<size_t> pos(start, end - 1);
    uniform_int_distribution<int> val(0, 255);
    const size_t count = (size_t)(amount_ * (double)(end - start));

    for (size_t k = 0; k < count; ++k) {
        b[pos(rng)] = (uint8_t)val(rng);
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
    size_t count = (size_t)(amount_ * (double)(end - start));
    if (count < 1 && amount_ > 0.0f) count = 1;

    for (size_t k = 0; k < count; ++k) {
        b[pos(rng)] = (uint8_t)val(rng);
    }
}

} // namespace tcx
