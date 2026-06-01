#pragma once

#include <TrussC.h>
#include <tcxImGui.h>
#include <tcxGlitch.h>

using namespace std;
using namespace tc;
using namespace tcx;

// Live databending playground:
//   - pick a codec (JPEG / BMP / PNG) at the top of the GUI
//   - tweak that codec's parameters
//   - toggle "Random seed" to animate, or scrub the seed by hand when frozen
//
// During development the source is a procedurally drawn test pattern (no camera
// permission needed). The final version swaps that for a VideoGrabber.
class tcApp : public App {
public:
    void setup() override;
    void draw() override;
    void cleanup() override;

private:
    void drawTestPattern();      // render the animated source into srcFbo_
    Glitch* currentGlitch();     // push GUI params into the selected codec

    Fbo srcFbo_;                 // source image to be glitched
    Image out_;                  // glitched result

    JpegGlitch jpeg_;
    BmpGlitch bmp_;
    PngGlitch png_;

    int codecIndex_ = 0;         // 0=JPEG, 1=BMP, 2=PNG

    int jpegQuality_ = 30;
    float jpegAmount_ = 0.2f;
    float bmpAmount_ = 0.15f;
    float pngAmount_ = 0.3f;

    bool randomSeed_ = true;
    int seed_ = 0;
    bool lastOk_ = true;
    int frame_ = 0;
};
