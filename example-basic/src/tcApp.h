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
// The source is the webcam (VJ-style). When no camera is available it falls
// back to an animated test pattern, so the example still runs anywhere.
class tcApp : public App {
public:
    void setup() override;
    void draw() override;
    void cleanup() override;

private:
    void drawSource();           // render the source (camera or pattern) into srcFbo_
    void drawTestPattern();      // animated fallback pattern
    Glitch* currentGlitch();     // push GUI params into the selected codec

    VideoGrabber grabber_;       // camera source; falls back to the test pattern
    bool useCamera_ = false;

    Fbo srcFbo_;                 // source image to be glitched
    Image out_;                  // glitched result

    JpegGlitch jpeg_;
    BmpGlitch bmp_;
    PngGlitch png_;

    int codecIndex_ = 2;         // 0=JPEG, 1=BMP, 2=PNG

    int jpegQuality_ = 30;
    float jpegAmount_ = 0.5f;
    float bmpAmount_ = 0.15f;
    float pngAmount_ = 0.3f;

    bool randomSeed_ = true;
    int seed_ = 0;
    bool lastOk_ = true;
    int frame_ = 0;
};
