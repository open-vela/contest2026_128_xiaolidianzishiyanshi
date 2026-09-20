#pragma once

#include <cstddef>
#include <cstdint>

/* One 1-kHz cycle at 16 kHz.  The BSP speaker test repeats this PCM waveform
 * to produce a short, deterministic two-tone chime without filesystem or
 * network dependencies.
 */

static constexpr int16_t kBspTestTone[] = {
    0,     4592,  8485,  11087, 12000, 11087, 8485,  4592,
    0,    -4592, -8485, -11087, -12000, -11087, -8485, -4592,
};

static constexpr size_t kBspTestToneSamples =
    sizeof(kBspTestTone) / sizeof(kBspTestTone[0]);
