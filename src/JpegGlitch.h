#pragma once

#include "Glitch.h"

namespace tcx::glitch {

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

} // namespace tcx::glitch

// -----------------------------------------------------------------------------
// Backward compatibility. The canonical namespace is now `tcx::glitch`. These
// silent aliases keep older code compiling: flat `tcx::JpegGlitch` and legacy
// `trussc::JpegGlitch`. DEPRECATED — removed in v1.0.0.
// (No [[deprecated]] attribute: under the usual `using namespace tc;` it would
//  warn on idiomatic unqualified use too. See tcxGlitch README for migration.)
// -----------------------------------------------------------------------------
namespace tcx    { using glitch::JpegGlitch; } // deprecated: remove at v1.0.0
namespace trussc { using tcx::glitch::JpegGlitch; } // deprecated: remove at v1.0.0
