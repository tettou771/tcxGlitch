#include "JpegGlitch.h"

// Everything we use here — stbi_write_jpg_to_func (stb, bundled in TrussC core),
// std::mt19937, std::pow — comes in through TrussC.h, which Glitch.h includes.

using namespace std;
using namespace tc;

namespace tcx::glitch {

bool JpegGlitch::encode(const Pixels& src, vector<uint8_t>& bytes) {
    bytes.clear();
    int ok = stbi_write_jpg_to_func(detail::appendToVector, &bytes,
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

} // namespace tcx::glitch
