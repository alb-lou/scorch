#pragma once
#include "AutoGainLut.h"
#include <algorithm>
#include <cmath>

namespace sat
{
// Static compensation (dB) for a style / punish / drive setting, interpolated from the built-in table.
inline float lookupAutoGainDb (int style, bool punish, float drive)
{
    const float pos = std::min (std::max (drive, 0.0f), 10.0f) * 2.0f;
    const int i = std::min ((int) pos, 19);
    const float fr = pos - (float) i;
    const auto& row = kAutoGainLut[std::min (std::max (style, 0), 4)][punish ? 1 : 0];
    return row[i] + fr * (row[i + 1] - row[i]);
}
}
