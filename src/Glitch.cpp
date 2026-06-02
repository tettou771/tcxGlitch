#include "Glitch.h"

#include <cstring>

using namespace std;
using namespace tc;

namespace tcx {

namespace {

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

} // namespace tcx
