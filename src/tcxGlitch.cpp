#include "tcxGlitch.h"

// stb_image_write ships with TrussC core (compiled there with
// STB_IMAGE_WRITE_IMPLEMENTATION, no _STATIC). We only need the declarations
// of the *_to_func encoders so we can capture the encoded bytes in memory
// instead of writing a file — the implementation is linked from core.
#include "stb/stb_image_write.h"
#include "stb/stb_image.h" // stbi_zlib_decode_malloc (declared here, linked from core)

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

// zlib deflate helper from stb_image_write — only declared inside its
// implementation block, so forward-declare it (it's extern "C" under STBIWDEF;
// the definition is compiled into TrussC core). Mirrors how core forward-
// declares stbi_write_png_to_mem.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len,
                                             int* out_len, int quality);

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

// CRC-32 (PNG / zlib polynomial) over a byte range.
uint32_t crc32Range(const uint8_t* p, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
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

    // (1) Drops (data loss). Delete a run and shift the tail up, so the row
    //     alignment cascades below the cut into a diagonal tear. Two flavours,
    //     split ~half and half:
    //       (a) byte drop   — delete 1-6 bytes (can rotate channels -> colour shift)
    //       (b) 8-byte drop — delete N*8 bytes (N random 1-64). A 32-bit BMP is
    //           4 bytes/pixel, so a multiple-of-4 shift keeps every byte on the
    //           same channel: colours stay intact and only the position jumps
    //           (jagged offset, not a colour scramble).
    //     Bounded, since each is an O(n) shift.
    const size_t cuts = (size_t)(amount_ * 24.0);
    uniform_int_distribution<int> dropLen(1, 6);
    uniform_int_distribution<int> unitN(1, 64);
    for (size_t k = 0; k < cuts; ++k) {
        size_t i = pos(rng);
        size_t d = (k % 2 == 0) ? (size_t)dropLen(rng)      // (a) byte-aligned
                                : (size_t)unitN(rng) * 8;    // (b) 8-byte units
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
// Authentic PNG filter-databending. PNG stores each row as [filter-type byte]
// [filtered pixels]; the decoder reverses that filter per row. We build the raw
// scanlines ourselves (filter 0 = None), scramble the filter byte on a fraction
// of rows, then compress ONCE — so the decoder un-filters those rows the wrong
// way and they smear/bleed downward. Always decodes (0..4 are valid filters).
//
// encode() builds the raw scanlines; corrupt() scrambles + compresses + wraps a
// PNG. Splitting it this way keeps a single zlib compress per frame (plus the
// base class's single decode), instead of round-tripping a stb-compressed PNG.
bool PngGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    width_ = src.getWidth();
    height_ = src.getHeight();
    channels_ = src.getChannels();
    if (width_ <= 0 || height_ <= 0 || channels_ <= 0) return false;

    const size_t stride = (size_t)width_ * channels_;
    const size_t rowSize = stride + 1; // + filter-type byte
    const int bpp = channels_;
    raw_.assign(rowSize * (size_t)height_, 0);
    const unsigned char* img = src.getData();

    auto paeth = [](int a, int b, int c) {
        int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
        return (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
    };

    // Pick the best PNG filter per row (stb's min-sum-of-absolute-residuals
    // heuristic) so the stored rows are real deltas. This is what makes the
    // later filter-byte scramble diverge dramatically (mismatching a real filter
    // on delta data), rather than the milder result of filtering everything with
    // None. Pure CPU — no extra zlib pass.
    vector<uint8_t> cand(stride), bestRow(stride);
    for (int y = 0; y < height_; ++y) {
        const unsigned char* row = img + (size_t)y * stride;
        const unsigned char* up = (y > 0) ? img + (size_t)(y - 1) * stride : nullptr;
        int best = 0;
        long bestSum = -1;
        for (int f = 0; f < 5; ++f) {
            long sum = 0;
            for (size_t x = 0; x < stride; ++x) {
                int a = (x >= (size_t)bpp) ? row[x - bpp] : 0;
                int b = up ? up[x] : 0;
                int c = (up && x >= (size_t)bpp) ? up[x - bpp] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b
                         : f == 3 ? ((a + b) >> 1) : paeth(a, b, c);
                uint8_t v = (uint8_t)(row[x] - pred);
                cand[x] = v;
                sum += (signed char)v < 0 ? -(signed char)v : (signed char)v;
            }
            if (bestSum < 0 || sum < bestSum) { bestSum = sum; best = f; bestRow = cand; }
        }
        raw_[(size_t)y * rowSize] = (uint8_t)best;
        memcpy(&raw_[(size_t)y * rowSize + 1], bestRow.data(), stride);
    }
    bytes.assign(1, 0); // placeholder; corrupt() produces the real PNG
    return true;
}

void PngGlitch::corrupt(vector<uint8_t>& b) {
    if (raw_.empty() || width_ <= 0 || height_ <= 0) return;
    const size_t rowSize = (size_t)width_ * channels_ + 1;

    mt19937 rng(seed_);
    uniform_int_distribution<size_t> rowPick(0, (size_t)height_ - 1);
    uniform_int_distribution<int> filt(0, 4); // valid PNG filter types

    // Scramble the filter byte on a fraction of rows -> smear cascade. Geometric
    // (log-scale) mapping like JpegGlitch: rows = height^amount, so equal slider
    // steps give roughly equal perceptual change with fine control at the subtle
    // low end. amount=0 -> none; amount=1 -> every row.
    const size_t rows = (amount_ <= 0.0f) ? 0 : (size_t)pow((double)height_, (double)amount_);
    for (size_t k = 0; k < rows; ++k) {
        raw_[rowPick(rng) * rowSize] = (uint8_t)filt(rng);
    }
    // A light sprinkle of residual-byte hits adds colour streaks (still decodes),
    // scaled by the same log-mapped intensity.
    const double frac = (double)rows / (double)height_;
    const size_t sprinkle = (size_t)(frac * 0.001 * (double)raw_.size());
    if (!raw_.empty()) {
        uniform_int_distribution<size_t> anyByte(0, raw_.size() - 1);
        uniform_int_distribution<int> val(0, 255);
        for (size_t k = 0; k < sprinkle; ++k) raw_[anyByte(rng)] = (uint8_t)val(rng);
    }

    // Compress once (quality 5 = stb's fastest) — ratio is irrelevant since we
    // decode it again immediately.
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw_.data(), (int)raw_.size(), &zlen, 5);
    if (!z || zlen <= 0) { if (z) free(z); return; }

    // Assemble the PNG: signature + IHDR + IDAT + IEND, with valid CRCs.
    vector<uint8_t> out;
    out.reserve(8 + 25 + 12 + (size_t)zlen + 12);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    auto put32 = [&](uint32_t v) {
        out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
    };
    auto chunk = [&](const char* type, const uint8_t* data, size_t len) {
        put32((uint32_t)len);
        size_t typePos = out.size();
        out.insert(out.end(), type, type + 4);
        if (len) out.insert(out.end(), data, data + len);
        put32(crc32Range(&out[typePos], 4 + len));
    };

    uint8_t ihdr[13] = {0};
    ihdr[0] = (uint8_t)(width_ >> 24);  ihdr[1] = (uint8_t)(width_ >> 16);
    ihdr[2] = (uint8_t)(width_ >> 8);   ihdr[3] = (uint8_t)width_;
    ihdr[4] = (uint8_t)(height_ >> 24); ihdr[5] = (uint8_t)(height_ >> 16);
    ihdr[6] = (uint8_t)(height_ >> 8);  ihdr[7] = (uint8_t)height_;
    ihdr[8] = 8; // bit depth
    ihdr[9] = (channels_ == 1 ? 0 : channels_ == 2 ? 4 : channels_ == 3 ? 2 : 6);
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z, (size_t)zlen);
    chunk("IEND", nullptr, 0);
    free(z);

    b.swap(out);
}

} // namespace tcx
