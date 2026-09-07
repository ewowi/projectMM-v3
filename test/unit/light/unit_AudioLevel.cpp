// @module AudioService

#include "doctest.h"
#include "core/AudioLevel.h"
#include "core/AudioService.h"
#include "core/ModuleFactory.h"
#include "light/effects/AudioSpectrumEffect.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

// The success spec for the level path, written RED before AudioService's reader
// exists: the two I2S-mic facts that AudioLevel.h handles must hold on
// synthesized blocks — DC offset is removed (a biased-but-silent block reads 0),
// the noise floor gates quiet hiss, gain scales what survives — and the whole
// thing is crash-safe on empty/degenerate input.

namespace {

// A pure sine of `cycles` periods across `n` samples at 24-bit-ish amplitude,
// left-justified into the int32 slot (<<8) the INMP441 produces, plus an
// optional DC bias to prove the bias is stripped.
std::vector<int32_t> sine(size_t n, double cycles, double amp24, double dc24 = 0.0) {
    constexpr double kPi = std::numbers::pi_v<double>;
    std::vector<int32_t> v(n);
    for (size_t i = 0; i < n; i++) {
        const double s = amp24 * std::sin(2.0 * kPi * cycles * static_cast<double>(i) / n) + dc24;
        // <<8 on a wider signed type — left-shifting a negative int32 is UB.
        const int64_t sample = static_cast<int64_t>(s) << 8;   // 24-bit into the high bits
        v[i] = static_cast<int32_t>(sample);
    }
    return v;
}

} // namespace

TEST_CASE("DcBlocker: a constant DC offset is filtered out") {
    mm::DcBlocker dc;
    std::vector<int32_t> s(1024, 100000);   // a big constant offset, no AC
    dc.process(s.data(), s.size());
    // After the filter settles, the output rides near zero (the DC is gone).
    int32_t lo = s[1023], hi = s[1023];
    for (size_t i = 900; i < s.size(); i++) {   // look past the transient
        if (s[i] < lo) lo = s[i];
        if (s[i] > hi) hi = s[i];
    }
    CHECK(std::abs(lo) < 2000);
    CHECK(std::abs(hi) < 2000);
}

TEST_CASE("DcBlocker: an audio tone passes through (DC removed, AC kept)") {
    mm::DcBlocker dc;
    // A mid-frequency sine on a big DC pedestal — the DC must go, the swing stay.
    const int32_t amp = 1 << 18;
    auto biased = sine(1024, 40, amp, 1 << 21);   // amp on a much larger DC pedestal
    dc.process(biased.data(), biased.size());
    int32_t lo = biased[512], hi = biased[512];
    for (size_t i = 512; i < biased.size(); i++) {     // past the transient
        if (biased[i] < lo) lo = biased[i];
        if (biased[i] > hi) hi = biased[i];
    }
    const int32_t swing = hi - lo;
    // Centred near zero (DC removed): the midpoint is tiny vs the swing.
    CHECK(std::abs(lo + hi) < swing / 4);
    // The tone survived: the swing is on the order of the AC amplitude (<<8 slot).
    CHECK(swing > amp);
}

TEST_CASE("DcBlocker: reset clears state, null-safe") {
    mm::DcBlocker dc;
    std::vector<int32_t> s(64, 50000);
    dc.process(s.data(), s.size());
    dc.reset();
    CHECK(dc.xPrev == 0.0f);
    CHECK(dc.yPrev == 0.0f);
    dc.process(nullptr, 64);   // no crash
    CHECK(true);
}

TEST_CASE("AudioLevel: silence reads zero") {
    std::vector<int32_t> s(512, 0);
    mm::AudioFrame f;
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/0, /*gain*/16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: pure DC reads zero (DC offset stripped)") {
    // A big constant bias, no AC — the DC must be stripped: RMS ~0, not huge.
    std::vector<int32_t> s(512, (1 << 22) << 8);
    mm::AudioFrame f;
    mm::computeLevel(s.data(), s.size(), 0, 16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: a loud sine reads a higher level than a quiet one") {
    // A wide dB window (gain 40) so both land inside it, not both at 255.
    auto loud = sine(512, 8, 1 << 14);
    auto quiet = sine(512, 8, 1 << 11);
    mm::AudioFrame fl, fq;
    mm::computeLevel(loud.data(), loud.size(), 0, 40, fl);
    mm::computeLevel(quiet.data(), quiet.size(), 0, 40, fq);
    CHECK(fl.level > fq.level);          // the log-scaled level tracks amplitude
}

TEST_CASE("AudioLevel: DC bias does not change the level of a sine") {
    auto clean = sine(512, 8, 1 << 14, 0.0);
    auto biased = sine(512, 8, 1 << 14, 1 << 22);   // same AC, huge DC
    mm::AudioFrame fc, fb;
    mm::computeLevel(clean.data(), clean.size(), 0, 40, fc);
    mm::computeLevel(biased.data(), biased.size(), 0, 40, fb);
    // The DC strip makes the two read the same level (within quantisation).
    const int diff = static_cast<int>(fc.level) - static_cast<int>(fb.level);
    CHECK(std::abs(diff) <= 2);
}

TEST_CASE("AudioLevel: a high noiseFloor (dB floor) gates a modest signal to zero") {
    auto s = sine(512, 8, 1 << 14);   // a modest level
    mm::AudioFrame lo, hi;
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/0, /*gain*/40, lo);
    REQUIRE(lo.level > 0);                                  // shows with a low floor
    // Raising the dB floor above the signal's level zeroes the displayed level.
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/255, /*gain*/40, hi);
    CHECK(hi.level == 0);
}

// `gain` reads the same way on both paths (higher = narrower window = hotter), but scales the
// level's OWN base span rather than being used raw: a block RMS covers far more dB than a single
// bin's peak, and sharing the raw number left the VU in the bottom third of the meter at the
// settings that made the spectrum look right (measured on a Dig-Next-2: RMS 39-83 of 255).
TEST_CASE("AudioLevel: higher gain (narrower dB window) reads a higher level") {
    auto s = sine(512, 8, 1 << 14);
    mm::AudioFrame lo, hi;
    mm::computeLevel(s.data(), s.size(), 0, /*gain*/20, lo);   // wide window
    mm::computeLevel(s.data(), s.size(), 0, /*gain*/120, hi);  // narrower = hotter
    REQUIRE(lo.level > 0);
    CHECK(hi.level > lo.level);
}

TEST_CASE("AudioLevel: empty / null input is silence, never a crash") {
    mm::AudioFrame f;
    mm::computeLevel(nullptr, 512, 0, 16, f);
    CHECK(f.level == 0);
    int32_t dummy = 0;
    mm::computeLevel(&dummy, 0, 0, 16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: isqrt64 matches floor(sqrt) on a spread of values") {
    const uint64_t xs[] = {0, 1, 2, 3, 4, 99, 100, 12345, 1ull << 40, (1ull << 46) + 7};
    for (uint64_t x : xs) {
        const uint64_t r = mm::isqrt64(x);
        CHECK(r * r <= x);
        CHECK((r + 1) * (r + 1) > x);
    }
}

// Regression: the boot wiring in main.cpp does
//   create("AudioService")->markWiredByCode()
// and create() returns nullptr for an UNREGISTERED type — so a missing
// registerType<AudioService> made the deref crash and the device boot-looped (found
// on the S3 bench). These pin that AudioService and the two audio effects are all
// registered + createable through the factory, and that latestFrame() is never
// null even with no mic (so a consumer added before the mic can't deref null).
TEST_CASE("AudioService + audio effects are registered and createable (boot-loop guard)") {
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect");

    auto* mic = mm::ModuleFactory::create("AudioService");
    REQUIRE(mic != nullptr);
    CHECK(mic->role() == mm::ModuleRole::Service);

    auto* spec = mm::ModuleFactory::create("AudioSpectrumEffect");
    REQUIRE(spec != nullptr);

    delete mic;
    delete spec;
}

TEST_CASE("AudioService::latestFrame is never null (silent frame with no active mic)") {
    const mm::AudioFrame* f = mm::AudioService::latestFrame();
    REQUIRE(f != nullptr);
    // With no mic having run setup(), it's the static silent frame.
    CHECK(f->level == 0);
    CHECK(f->peakHz == 0);
}
