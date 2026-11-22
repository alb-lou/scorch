// Scorch DSP core - plain C++ (no JUCE dependency) so it can be unit-tested.
// Every style is an original model built from published tube/transformer/transistor
// behaviour. Nothing here is copied from, or measured against, any commercial plugin.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sat
{
constexpr double kPi = 3.14159265358979323846;
inline double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
inline double linToDb (double l)  { return 20.0 * std::log10 (std::max (l, 1e-12)); }

enum Style { TapeTube = 0, Console, Germanium, Triode, Pentode, NumStyles };

// Drive 0..10 -> pre-gain in dB. Punish adds a big extra push.
inline double driveToDb (double drive, bool punish) { return drive * 4.0 + (punish ? 15.0 : 0.0); }

// ---------------------------------------------------------------- filters
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void lowpass (double fs, double f, double q = 0.70710678)
    {
        f = std::min (f, 0.49 * fs);
        const double w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass (double fs, double f, double q = 0.70710678)
    {
        f = std::min (f, 0.49 * fs);
        const double w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void onePoleLP (double fs, double f)
    {
        f = std::min (f, 0.49 * fs);
        const double K = std::tan (kPi * f / fs);
        set (K, K, 0, 1 + K, K - 1, 0);
    }
    void onePoleHP (double fs, double f)
    {
        f = std::min (f, 0.49 * fs);
        const double K = std::tan (kPi * f / fs);
        set (1, -1, 0, 1 + K, K - 1, 0);
    }
    void lowShelf (double fs, double f, double gDb, double q = 0.70710678)
    {
        f = std::min (f, 0.49 * fs);
        const double A = std::pow (10.0, gDb / 40), w = 2 * kPi * f / fs, c = std::cos (w);
        const double al = std::sin (w) / (2 * q), sq = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
             (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
    }
    void highShelf (double fs, double f, double gDb, double q = 0.70710678)
    {
        f = std::min (f, 0.49 * fs);
        const double A = std::pow (10.0, gDb / 40), w = 2 * kPi * f / fs, c = std::cos (w);
        const double al = std::sin (w) / (2 * q), sq = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
             (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
    }
    void peaking (double fs, double f, double gDb, double q)
    {
        f = std::min (f, 0.49 * fs);
        const double A = std::pow (10.0, gDb / 40), w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
};

// ------------------------------------------------- tube stage (load-line table)
// A common-cathode stage: plate current from a valve model, plate resistor to B+,
// solved for plate voltage at each grid voltage and stored in a lookup table.
// The valve curve is scaled so the quiescent point sits where we choose (auto-bias).
struct TubeTable
{
    static constexpr int N = 2048;
    std::array<float, N + 1> t {};
    double vmax = 4, inScale = 0.5;

    template <class Shape>
    void build (double Vb, double Rp, double Vbias, double vp0Frac, double vmaxIn, double inScaleIn, Shape shape)
    {
        vmax = vmaxIn; inScale = inScaleIn;
        const double vp0 = Vb * vp0Frac;
        const double s = (Vb - vp0) / (Rp * shape (Vbias, vp0));
        auto solve = [&] (double vgk)
        {
            double lo = 0, hi = Vb;
            for (int i = 0; i < 60; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (mid + Rp * s * shape (vgk, mid) - Vb > 0 ? hi : lo) = mid;
            }
            return 0.5 * (lo + hi);
        };
        auto dev = [&] (double vin) { return -(solve (vin + Vbias) - vp0); };   // non-inverting, volts
        const double h = 0.01;
        const double gain0 = (dev (h) - dev (-h)) / (2 * h);
        const double norm = 1.0 / (gain0 * inScale);                             // unity small-signal gain
        for (int i = 0; i <= N; ++i)
            t[(size_t) i] = (float) (dev (-vmax + 2 * vmax * i / N) * norm);
    }

    inline float process (float x) const
    {
        const float vin = x * (float) inScale;
        const float v = (float) vmax * std::tanh (vin / (float) vmax);          // grid-current limiting
        const float pos = (v + (float) vmax) * (float) (N / (2 * vmax));
        int i = (int) pos; i = std::min (std::max (i, 0), N - 1);
        const float fr = pos - (float) i;
        return t[(size_t) i] + fr * (t[(size_t) i + 1] - t[(size_t) i]);
    }
};

inline double softplus (double a) { return a > 30 ? a : std::log1p (std::exp (a)); }

// Koren (2003) triode plate-current shape
inline double korenTriode (double vgk, double vpk, double mu, double ex, double kp, double kvb)
{
    if (vpk <= 0) return 0;
    const double e1 = vpk / kp * softplus (kp * (1.0 / mu + vgk / std::sqrt (kvb + vpk * vpk)));
    return std::pow (e1, ex);
}

// Simple pentode shape: softly-cut-off 3/2 power law with a knee in plate voltage
inline double pentodeShape (double vgk, double vpk)
{
    if (vpk <= 0) return 0;
    const double d = softplus (1.6 * (vgk + 9.0)) / 1.6;
    return std::pow (d, 1.5) * std::tanh (vpk / 35.0);
}

struct Tables
{
    TubeTable ax7, at7, tri, pen;
    Tables()
    {
        ax7.build (250, 100e3, -1.4, 0.60, 4.0, 0.45, [] (double g, double p) { return korenTriode (g, p, 100, 1.4, 600, 300); });
        at7.build (250,  47e3, -1.8, 0.55, 5.0, 0.90, [] (double g, double p) { return korenTriode (g, p,  60, 1.35, 300, 300); });
        tri.build (250,  68e3, -2.0, 0.55, 5.0, 0.70, [] (double g, double p) { return korenTriode (g, p,  30, 1.4, 200, 300); });
        pen.build (250,  56e3, -3.5, 0.55, 7.0, 1.00, [] (double g, double p) { return pentodeShape (g, p); });
    }
    static const Tables& get() { static const Tables t; return t; }
};

// Transformer-style low-frequency flux saturation: lows saturate before mids/highs.
struct FluxSat
{
    double a = 0, low = 0, g = 1;
    void setup (double fs, double fc, double gg) { a = 1.0 - std::exp (-2.0 * kPi * fc / fs); g = gg; }
    void reset() { low = 0; }
    inline double process (double x)
    {
        low += a * (x - low);
        return (x - low) + std::tanh (g * low) / g;
    }
};

// ------------------------------------------------------------- style core
// Input is the post-drive signal (1.0 = 0 dBFS pre-gain). Small-signal gain is unity.
class StyleCore
{
public:
    void prepare (double fs)
    {
        tapeHp1.onePoleHP (fs, 30);  tapeHp2.onePoleHP (fs, 25);
        tapeLp.onePoleLP  (fs, 15000); tapeDc.onePoleHP (fs, 8);
        conFlux.setup (fs, 70, 0.45);  conDc.onePoleHP (fs, 8);  conShelf.highShelf (fs, 9000, 1.5);
        gerFlux.setup (fs, 110, 0.55); gerDc.onePoleHP (fs, 8);  gerPeak.peaking (fs, 2500, 1.5, 0.8);
        triDc.onePoleHP (fs, 10);      penDc.onePoleHP (fs, 10);
        reset();
    }
    void reset()
    {
        for (auto* b : { &tapeHp1, &tapeHp2, &tapeLp, &tapeDc, &conDc, &conShelf, &gerDc, &gerPeak, &triDc, &penDc }) b->reset();
        conFlux.reset(); gerFlux.reset();
    }

    inline float process (int style, float xf)
    {
        const auto& T = Tables::get();
        double x = xf;
        switch (style)
        {
            case TapeTube:   // two cascaded valve stages, coupling caps, gentle top roll-off
            {
                x = tapeHp1.process (x);
                x = T.ax7.process ((float) x);
                x = tapeHp2.process (x);
                x = T.at7.process ((float) x);
                x = tapeLp.process (x);
                return (float) tapeDc.process (x);
            }
            case Console:    // transformer flux saturation into a slightly biased class-A stage
            {
                x = conFlux.process (x);
                constexpr double bias = 0.10;
                static const double tb = std::tanh (bias), sl = 1.0 - tb * tb;
                x = (std::tanh (x + bias) - tb) / sl;
                x = conDc.process (x);
                return (float) conShelf.process (x);
            }
            case Germanium:  // transformer -> asymmetric exponential (germanium-style) clipper
            {
                x = gerFlux.process (x);
                constexpr double a = 0.7, b = 1.1;
                x = x >= 0 ? (1.0 - std::exp (-a * x)) / a : -(1.0 - std::exp (b * x)) / b;
                x = gerDc.process (x);
                return (float) gerPeak.process (x);
            }
            case Triode:
                return (float) triDc.process (T.tri.process ((float) x));
            case Pentode:
            default:
            {   // single-ended curve blended with its mirror image (push-pull-like): odd-weighted, edgy
                const float f = (float) x;
                const float sym = 0.5f * (T.pen.process (f) - T.pen.process (-f));
                return (float) penDc.process (0.7f * sym + 0.3f * T.pen.process (f));
            }
        }
    }

private:
    Biquad tapeHp1, tapeHp2, tapeLp, tapeDc, conDc, conShelf, gerDc, gerPeak, triDc, penDc;
    FluxSat conFlux, gerFlux;
};

// ------------------------------------------- static auto-gain table generator
struct PinkNoise
{
    uint32_t s = 22222; double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    double next()
    {
        s = s * 1664525u + 1013904223u;
        const double w = (double) (s >> 8) * (1.0 / 8388608.0) - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        return p * 0.11;
    }
};

constexpr int kLutDrivePoints = 21;   // drive 0..10 in 0.5 steps
constexpr double kLutRefDbfs = -18.0; // reference pink noise level (RMS)

// Returns dB of gain needed to bring post-saturation RMS back to the input RMS.
inline std::vector<float> computeAutoGainLut (double fsOS = 192000.0)
{
    const int len = (int) (fsOS * 0.3), warm = (int) (fsOS * 0.05);
    std::vector<double> ref ((size_t) len);
    PinkNoise pn; double ss = 0;
    Biquad hp, lp; hp.highpass (fsOS, 40.0); lp.lowpass (fsOS, 16000.0);   // musical band, no sub-bass
    for (auto& v : ref) { v = lp.process (hp.process (pn.next())); }
    for (int i = warm; i < len; ++i) ss += ref[(size_t) i] * ref[(size_t) i];
    const double scale = dbToLin (kLutRefDbfs) / std::sqrt (ss / (len - warm));
    for (auto& v : ref) v *= scale;

    std::vector<float> lut ((size_t) (NumStyles * 2 * kLutDrivePoints));
    StyleCore core; core.prepare (fsOS);
    for (int st = 0; st < NumStyles; ++st)
        for (int p = 0; p < 2; ++p)
            for (int d = 0; d < kLutDrivePoints; ++d)
            {
                core.reset();
                const double g = dbToLin (driveToDb (d * 0.5, p == 1));
                double sIn = 0, sOut = 0;
                for (int i = 0; i < len; ++i)
                {
                    const double y = core.process (st, (float) (ref[(size_t) i] * g));
                    if (i >= warm) { sIn += ref[(size_t) i] * ref[(size_t) i]; sOut += y * y; }
                }
                lut[(size_t) ((st * 2 + p) * kLutDrivePoints + d)] = (float) (10.0 * std::log10 (sIn / std::max (sOut, 1e-18)));
            }
    return lut;
}
} // namespace sat
