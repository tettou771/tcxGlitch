#pragma once

#include "Glitch.h"

namespace tcx::glitch {

// -----------------------------------------------------------------------------
// BmpGlitch — uncompressed databending. Always decodes. BMP is 32-bit (4 bytes
// per pixel: B,G,R,A), which is why the shift granularity decides whether colour
// survives. Five independent operators (each 0-1), no single "amount":
//   setTear    — delete 1-3 bytes + shift the tail. Non-pixel-aligned, so the
//                channels ROTATE below the cut -> uniform hue-shift bands.
//   setShift   — delete N*8 bytes (N 1-64). A multiple of the 4-byte pixel, so
//                colours stay intact and only the position jumps -> jagged offset.
//   setRainbow — delete N 4-bit nibbles (N 1-64). Odd N shifts the tail by half
//                a byte, pushing each channel's low bits into high bits, so
//                smooth gradients wrap into false-colour contour rings.
//   setSpeckle — random byte replacement -> colour speckles.
//   setNoise   — random bit flips -> subtle per-channel noise.
// -----------------------------------------------------------------------------
class BmpGlitch : public Glitch {
public:
    BmpGlitch& setTear(float v)    { tear_    = std::clamp(v, 0.0f, 1.0f); return *this; }
    BmpGlitch& setShift(float v)   { shift_   = std::clamp(v, 0.0f, 1.0f); return *this; }
    BmpGlitch& setRainbow(float v) { rainbow_ = std::clamp(v, 0.0f, 1.0f); return *this; }
    BmpGlitch& setSpeckle(float v) { speckle_ = std::clamp(v, 0.0f, 1.0f); return *this; }
    BmpGlitch& setNoise(float v)   { noise_   = std::clamp(v, 0.0f, 1.0f); return *this; }
    float getTear() const { return tear_; }
    float getShift() const { return shift_; }
    float getRainbow() const { return rainbow_; }
    float getSpeckle() const { return speckle_; }
    float getNoise() const { return noise_; }

protected:
    bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) override;
    void corrupt(std::vector<uint8_t>& bytes) override;

private:
    float tear_ = 0.05f;
    float shift_ = 0.12f;
    float rainbow_ = 0.05f;
    float speckle_ = 0.04f;
    float noise_ = 0.04f;
};

} // namespace tcx::glitch

// -----------------------------------------------------------------------------
// Backward compatibility. The canonical namespace is now `tcx::glitch`. These
// silent aliases keep older code compiling: flat `tcx::BmpGlitch` and legacy
// `trussc::BmpGlitch`. DEPRECATED — removed in v1.0.0.
// (No [[deprecated]] attribute: under the usual `using namespace tc;` it would
//  warn on idiomatic unqualified use too. See tcxGlitch README for migration.)
// -----------------------------------------------------------------------------
namespace tcx    { using glitch::BmpGlitch; } // deprecated: remove at v1.0.0
namespace trussc { using tcx::glitch::BmpGlitch; } // deprecated: remove at v1.0.0
