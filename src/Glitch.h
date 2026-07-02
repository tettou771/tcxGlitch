#pragma once

// =============================================================================
// Glitch — abstract base for every tcxGlitch codec.
// =============================================================================
//
// "Databending" = encode an image to a real file format, poke at the encoded
// bytes, then decode it back. The decoder does its best to make sense of the
// now-broken stream, which is where the glitch comes from.
//
// The base owns the whole pipeline:
//
//     input (Image / Fbo / Texture)
//        -> RGBA8 Pixels
//        -> encode()   : pixels      -> codec byte stream    (subclass)
//        -> corrupt()  : byte stream -> broken byte stream   (subclass)
//        -> decode     : Pixels::loadFromMemory()
//        -> output Image (drawable + saveable)
//
// Output is always a tc::Image, so you can draw() and save() it. Corruption is
// driven by a seeded std::mt19937 (setSeed), so a fixed seed is reproducible —
// freeze a frame and scrub the seed, or animate by bumping it each frame.
//
// -----------------------------------------------------------------------------
// HOW TO ADD YOUR OWN GLITCH CODEC
// -----------------------------------------------------------------------------
// 1. Copy a codec pair as a starting point — JpegGlitch.{h,cpp} is the simplest
//    (BmpGlitch is the multi-knob example, PngGlitch the most involved).
// 2. Subclass Glitch and implement the two virtuals:
//       encode()  — turn the RGBA8 pixels into your format's bytes
//       corrupt() — break those bytes (skip the header so it still decodes)
//    Add whatever parameters/setters your effect needs (return *this to chain).
// 3. #include your new header from tcxGlitch.h.
// That's the whole extension surface — input conversion, decoding, the black
// fallback, and the return value all live here in the base.
// =============================================================================

#include <TrussC.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tcx::glitch {

namespace detail {
// stb_image_write `*_to_func` callback: append the produced bytes to a
// std::vector<uint8_t>. Codec encode()s pass this to capture the encoded image
// in memory instead of writing a file.
inline void appendToVector(void* context, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(context);
    auto* src = static_cast<unsigned char*>(data);
    buf->insert(buf->end(), src, src + size);
}
} // namespace detail

// -----------------------------------------------------------------------------
// Glitch — abstract base. Owns the databend pipeline; codecs fill in the gaps.
// -----------------------------------------------------------------------------
class Glitch {
public:
    virtual ~Glitch() = default;

    // RNG seed for the corruption pattern.
    Glitch& setSeed(uint32_t seed) { seed_ = seed; return *this; }
    uint32_t getSeed() const { return seed_; }

    // Apply to any input; the result always lands in `out`.
    // Returns true on success. Returns false if the corrupted stream could not
    // be decoded at all — in that case `out` is filled solid black, so callers
    // can detect the wipe-out and retry with milder parameters.
    bool apply(const tc::Image& src, tc::Image& out);
    bool apply(const tc::Fbo& src, tc::Image& out);
    bool apply(const tc::Texture& src, tc::Image& out);

    // In-place convenience: glitch an Image's own contents.
    bool apply(tc::Image& img);

protected:
    // Encode RGBA8 source pixels into this codec's byte stream.
    // Return false if encoding failed.
    virtual bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) = 0;

    // Corrupt the encoded bytes in place. Implementations must skip their own
    // header / critical region so the stream still (usually) decodes.
    virtual void corrupt(std::vector<uint8_t>& bytes) = 0;

    uint32_t seed_ = 0;

private:
    // Shared pipeline: srcPixels -> encode -> corrupt -> decode -> out.
    bool process(const tc::Pixels& srcPixels, tc::Image& out);

    // Scratch render target used to pull pixels out of a bare Texture.
    tc::Fbo scratchFbo_;
};

} // namespace tcx::glitch

// -----------------------------------------------------------------------------
// Backward compatibility. The canonical namespace is now `tcx::glitch`. These
// silent aliases keep older code compiling: flat `tcx::Glitch` and legacy
// `trussc::Glitch`. DEPRECATED — removed in v1.0.0.
// (No [[deprecated]] attribute: under the usual `using namespace tc;` it would
//  warn on idiomatic unqualified use too. See tcxGlitch README for migration.)
// -----------------------------------------------------------------------------
namespace tcx    { using glitch::Glitch; } // deprecated: remove at v1.0.0
namespace trussc { using tcx::glitch::Glitch; } // deprecated: remove at v1.0.0
