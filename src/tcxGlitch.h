#pragma once

// =============================================================================
// tcxGlitch — databending-style glitch effects for TrussC
// =============================================================================
//
// Public umbrella header — `#include <tcxGlitch.h>` pulls in everything.
//
// One class per file:
//   Glitch.h      — abstract base + the databend pipeline (read this to extend)
//   JpegGlitch.h  — DCT-block smears
//   BmpGlitch.h   — uncompressed: tears / shifts / rainbow / speckle / noise
//   PngGlitch.h   — scanline filter-byte smear
//
// To add your own codec: copy a codec's .h/.cpp pair, subclass Glitch, implement
// encode()/corrupt(), and add an #include below. See the guide in Glitch.h.
// =============================================================================

#include "Glitch.h"
#include "JpegGlitch.h"
#include "BmpGlitch.h"
#include "PngGlitch.h"
