# tcxGlitch

Databending-style glitch effects for [TrussC](https://github.com/TrussC-org/TrussC).

Encode an image to a real codec (JPEG / BMP / PNG), scramble the encoded bytes,
then decode it back. The decoder tries to make sense of the broken stream — and
that's the glitch. No shaders, just honest data corruption.

```cpp
#include <tcxGlitch.h>
using namespace tcx;

JpegGlitch glitch;          // also: BmpGlitch, PngGlitch

void setup() {
    glitch.setQuality(30).setAmount(0.2f);
}

void draw() {
    glitch.setSeed(getFrameNum());   // animate by bumping the seed
    glitch.apply(srcFbo, outImage);  // input: Fbo / Texture / Image
    outImage.draw(0, 0);
    // outImage.save("glitched.png"); // clean PNG export of the result
}
```

## API

`Glitch` is an abstract base that owns the whole databend pipeline. The
concrete codecs differ only in how they encode and how they corrupt:

| Class | Knobs | Character |
|-------|-------|-----------|
| `JpegGlitch` | `setQuality(1-100)`, `setAmount(0-1)` | DCT-block smears (the classic look). `amount` is log-scaled. |
| `BmpGlitch` | `setTear`/`setShift`/`setRainbow`/`setSpeckle`/`setNoise` (each 0-1) | per-operator: hue-shift tears, colour-safe jagged shift, rainbow contour, speckles, channel noise |
| `PngGlitch` | `setAmount(0-1)` | scanline filter-byte scramble → rows smear/bleed downward. Always decodes, but heavier (per-frame zlib). |

Common to all:

- `setSeed(uint32_t)` — corruption is deterministic per seed. Freeze a frame and
  scrub the seed, or animate by bumping it each frame.
- `apply(input, Image& out)` — input can be a `Fbo`, `Texture`, or `Image`.
  The output is always an `Image` (so you can `draw()` and `save()` it).
- `apply(Image&)` — in-place convenience.
- Returns `false` when the corrupted stream fails to decode; in that case `out`
  is filled solid black, so you can detect the wipe-out and retry milder.

### Roll your own

One class per file under `src/`:

```
src/
├── tcxGlitch.h     # public umbrella (just #includes the rest)
├── Glitch.{h,cpp}  # abstract base + the databend pipeline + how-to-extend guide
├── JpegGlitch.{h,cpp}
├── BmpGlitch.{h,cpp}
└── PngGlitch.{h,cpp}
```

To add a codec: copy a codec's `.h`/`.cpp` pair (JpegGlitch is the simplest),
subclass `Glitch`, implement `encode()` + `corrupt()`, add any setters you need,
then `#include` your header from `tcxGlitch.h`. The base handles input
conversion, decoding, the black fallback, and the return value — you only decide
how bytes are produced and how they get broken.

## Notes

- The output is a `tc::Image`, so PNG export is just `out.save("foo.png")`.
- Pulling pixels from a bare `Texture` costs an extra draw + readback (it is
  bounced through an internal scratch FBO). `Fbo`/`Image` inputs are cheaper.
- Image encode/decode uses stb_image / stb_image_write, which ship with TrussC
  core — nothing is vendored here.

## Example

`example-basic/` — a live glitch playground (uses [tcxImGui](https://github.com/TrussC-org/TrussC)
for the controls): pick a codec, tweak its parameters, toggle random seed or
scrub it by hand.

## License

MIT — see [LICENSES.md](LICENSES.md).
