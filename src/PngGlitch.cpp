#include "PngGlitch.h"

// stb's zlib helpers (bundled in TrussC core, reachable via TrussC.h):
//   stbi_zlib_decode_malloc  inflate — declared in stb_image.h
//   stbi_zlib_compress       deflate — only declared inside stb's impl block, so
//                            we forward-declare it here (extern "C", linked from
//                            core). stb_image ignores chunk CRCs.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len,
                                             int* out_len, int quality);

using namespace std;
using namespace tc;

namespace tcx::glitch {

namespace {
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
} // namespace

// Authentic PNG filter-databending. PNG stores each row as [filter-type byte]
// [filtered pixels]; the decoder reverses that filter per row. encode() builds
// the raw scanlines (choosing the best real filter per row, so the bytes are
// deltas); corrupt() scrambles the filter byte on a fraction of rows, compresses
// ONCE and wraps a PNG. Splitting it this way keeps a single zlib compress per
// frame (plus the base class's single decode).
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

} // namespace tcx::glitch
