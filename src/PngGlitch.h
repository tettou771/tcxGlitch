#pragma once

#include "Glitch.h"

namespace tcx::glitch {

// -----------------------------------------------------------------------------
// PngGlitch — filter-byte databending. PNG stores each row with a "filter type"
// byte (0..4) that says how the row was delta-encoded. This rewrites the filter
// byte on a fraction of rows (encode picks the best real filter per row, so the
// stored data are deltas), then re-compresses — so the decoder un-filters those
// rows the WRONG way and they smear/bleed downward. The signature soft, drippy
// PNG glitch; unlike JPEG it always decodes. Heavier than JPEG/BMP (one zlib
// compress per frame).
//   amount : glitch intensity (0-1), log-scaled (rows scrambled = height^amount)
//            so the subtle low end has fine control.
// -----------------------------------------------------------------------------
class PngGlitch : public Glitch {
public:
    PngGlitch& setAmount(float a) { amount_ = std::clamp(a, 0.0f, 1.0f); return *this; }
    float getAmount() const { return amount_; }

protected:
    bool encode(const tc::Pixels& src, std::vector<uint8_t>& bytes) override;
    void corrupt(std::vector<uint8_t>& bytes) override;

private:
    float amount_ = 0.6f;
    // Scratch shared between encode() (builds raw PNG scanlines) and corrupt()
    // (scrambles filter bytes, then compresses once). Avoids round-tripping
    // through a fully stb-compressed PNG, which was ~4 zlib passes per frame.
    std::vector<uint8_t> raw_;
    int width_ = 0, height_ = 0, channels_ = 0;
};

} // namespace tcx::glitch

// -----------------------------------------------------------------------------
// Backward compatibility. The canonical namespace is now `tcx::glitch`. These
// silent aliases keep older code compiling: flat `tcx::PngGlitch` and legacy
// `trussc::PngGlitch`. DEPRECATED — removed in v1.0.0.
// (No [[deprecated]] attribute: under the usual `using namespace tc;` it would
//  warn on idiomatic unqualified use too. See tcxGlitch README for migration.)
// -----------------------------------------------------------------------------
namespace tcx    { using glitch::PngGlitch; } // deprecated: remove at v1.0.0
namespace trussc { using tcx::glitch::PngGlitch; } // deprecated: remove at v1.0.0
