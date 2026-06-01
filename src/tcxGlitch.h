#pragma once

// =============================================================================
// tcxGlitch — databending-style glitch effects for TrussC
// =============================================================================
//
// "Databending" = encode an image to a real file format (JPEG / BMP / PNG),
// poke at the encoded bytes, then decode it back. The decoder does its best to
// make sense of the now-broken stream, which is where the glitch comes from.
//
// Pipeline (lives in the Glitch base class):
//
//     input (Image / Fbo / Texture)
//        -> RGBA8 Pixels
//        -> encode()   : pixels  -> codec byte stream     (subclass)
//        -> corrupt()  : byte stream -> broken byte stream (subclass)
//        -> decode     : Pixels::loadFromMemory()
//        -> output Image (drawable + saveable)
//
// To add your own codec / corruption style, subclass Glitch and implement
// encode() + corrupt(). That's the whole extension surface — the base owns
// input conversion, decoding, the black fallback, and the return value.
//
// The output is always a tc::Image, so you can both draw() it and save() it
// (PNG export is just out.save("foo.png"), handled by core).
//
// Determinism: corruption is driven by a seeded std::mt19937. With a fixed
// seed the result is reproducible, so you can freeze a frame and scrub the
// seed by hand, or animate by bumping it every frame.
// =============================================================================

#include <TrussC.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tcx {

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

// -----------------------------------------------------------------------------
// JpegGlitch — classic DCT-block databending. The signature blocky smears.
//   quality : JPEG encode quality (1-100). Lower = bigger/chunkier blocks.
//   amount  : glitch intensity (0-1), mapped on a geometric/log scale so the
//             subtle low end has fine control. stb's JPEG decoder is strict, so
//             the window is narrow; amount~1 is the near-destruction edge
//             (frames may flip to black).
// -----------------------------------------------------------------------------
class JpegGlitch : public Glitch {
public:
    JpegGlitch& setQuality(int q) { quality_ = std::clamp(q, 1, 100); return *this; }
    JpegGlitch& setAmount(float a) { amount_ = std::clamp(a, 0.0f, 1.0f); return *this; }
    int getQuality() const { return quality_; }
    float getAmount() const { return amount_; }

protected:
    bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) override;
    void corrupt(std::vector<uint8_t>& bytes) override;

private:
    int quality_ = 30;
    float amount_ = 0.5f;
};

// -----------------------------------------------------------------------------
// BmpGlitch — uncompressed databending. Always decodes. Mixes three operators:
// byte replacement (colour speckles), bit flips (subtle channel noise), and
// byte drops (delete + shift the tail, cascading into diagonal tears / channel
// shifts — the "missing data" look).
//   amount : glitch intensity (0-1).
// -----------------------------------------------------------------------------
class BmpGlitch : public Glitch {
public:
    BmpGlitch& setAmount(float a) { amount_ = std::clamp(a, 0.0f, 1.0f); return *this; }
    float getAmount() const { return amount_; }

protected:
    bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) override;
    void corrupt(std::vector<uint8_t>& bytes) override;

private:
    float amount_ = 0.15f;
};

// -----------------------------------------------------------------------------
// PngGlitch — the wild one. PNG is zlib-compressed with per-chunk CRCs, so
// poking the IDAT stream usually makes the whole image collapse (-> black,
// apply() returns false). Sometimes a few bytes survive and you get a glorious
// half-decoded mess.
//   amount : glitch intensity (0-1). Mostly collapses to black regardless.
// -----------------------------------------------------------------------------
class PngGlitch : public Glitch {
public:
    PngGlitch& setAmount(float a) { amount_ = std::clamp(a, 0.0f, 1.0f); return *this; }
    float getAmount() const { return amount_; }

protected:
    bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) override;
    void corrupt(std::vector<uint8_t>& bytes) override;

private:
    float amount_ = 0.3f;
};

} // namespace tcx
