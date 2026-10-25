#include "dsp/wavetable.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace saeurekiste {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

using Complex = std::complex<double>;

// An in-place radix-2 FFT. Only ever run on the main thread while a table is
// being built, so it is written for clarity rather than speed; the twiddles
// are a recurrence in double, which is good to far below an int16 over the
// largest size used here.
void fft(std::vector<Complex> &a, bool inverse) {
   const size_t n = a.size();
   for (size_t i = 1, j = 0; i < n; ++i) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
         std::swap(a[i], a[j]);
   }
   for (size_t len = 2; len <= n; len <<= 1) {
      const double ang = kTwoPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
      const Complex wlen(std::cos(ang), std::sin(ang));
      for (size_t i = 0; i < n; i += len) {
         Complex w(1.0, 0.0);
         for (size_t j = 0; j < len / 2; ++j) {
            const Complex u = a[i + j];
            const Complex v = a[i + j + len / 2] * w;
            a[i + j] = u + v;
            a[i + j + len / 2] = u - v;
            w *= wlen;
         }
      }
   }
   if (inverse)
      for (Complex &c : a)
         c /= static_cast<double>(n);
}

bool isPowerOfTwo(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

// The harmonics of one cycle as complex amplitudes: the cycle is
// sum over h of Re(c[h] * e^(i 2 pi h t)). c[0] is left at zero, so DC is gone.
std::vector<Complex> harmonicsOf(const double *cycle, size_t n, int maxHarmonic) {
   std::vector<Complex> a(n);
   for (size_t i = 0; i < n; ++i)
      a[i] = Complex(cycle[i], 0.0);
   fft(a, false);
   std::vector<Complex> c(static_cast<size_t>(maxHarmonic) + 1, Complex(0.0, 0.0));
   const int top = std::min(maxHarmonic, static_cast<int>(n / 2) - 1);
   for (int h = 1; h <= top; ++h)
      c[static_cast<size_t>(h)] = a[static_cast<size_t>(h)] * (2.0 / static_cast<double>(n));
   return c;
}

// One cycle of `n` samples from complex amplitudes, using harmonics 1 to
// `maxHarmonic` and nothing above n/2 - 1.
void cycleFrom(const std::vector<Complex> &c, int maxHarmonic, size_t n, double *out) {
   std::vector<Complex> a(n, Complex(0.0, 0.0));
   const int top = std::min({maxHarmonic, static_cast<int>(c.size()) - 1,
                             static_cast<int>(n / 2) - 1});
   for (int h = 1; h <= top; ++h) {
      const Complex x = c[static_cast<size_t>(h)] * (static_cast<double>(n) * 0.5);
      a[static_cast<size_t>(h)] = x;
      a[n - static_cast<size_t>(h)] = std::conj(x);
   }
   fft(a, true);
   for (size_t i = 0; i < n; ++i)
      out[i] = a[i].real();
}

constexpr int kSourceHarmonics = kWtSourceLen / 2 - 1; // 511

int levelHarmonics(int level) { return 512 >> level; }

// How many samples a level is stored at. Linear interpolation between them
// leaves images of every harmonic around multiples of the table's own rate,
// and a short table puts them low enough to fold back: at 16 samples per
// period of the top harmonic they came out 45 dB down on a bright table, which
// is audible. Eight per period at the top levels, where the highest harmonics
// are faint anyway, and never fewer than 512 samples, which puts the levels a
// note mostly reads at 64 and more. The lowest level, read only below 23 Hz,
// stays at four.
int levelSize(int level) {
   const int size = 8 * levelHarmonics(level);
   return size < 512 ? 512 : (size > 2048 ? 2048 : size);
}

} // namespace

// ------------------------------------------------------------------ the table

bool Wavetable::build(const float *cycles, int frames) {
   mFrames = 0;
   mData.clear();
   if (!cycles || frames <= 0)
      return false;
   frames = std::min(frames, kWtMaxFrames);

   int offset = 0;
   for (int k = 0; k < kWtLevels; ++k) {
      mOffset[k] = offset;
      mSize[k] = levelSize(k);
      offset += mSize[k] + 1; // and a guard sample for the interpolation
   }
   mFrameStride = offset;
   mData.assign(static_cast<size_t>(frames) * static_cast<size_t>(mFrameStride), 0.0f);

   std::vector<double> source(kWtSourceLen);
   std::vector<double> level;
   double peak = 0.0;
   for (int f = 0; f < frames; ++f) {
      for (int i = 0; i < kWtSourceLen; ++i)
         source[static_cast<size_t>(i)] = cycles[static_cast<size_t>(f) * kWtSourceLen + i];
      const std::vector<Complex> c = harmonicsOf(source.data(), kWtSourceLen, kSourceHarmonics);
      float *frame = mData.data() + static_cast<size_t>(f) * static_cast<size_t>(mFrameStride);
      for (int k = 0; k < kWtLevels; ++k) {
         const size_t n = static_cast<size_t>(mSize[k]);
         level.assign(n, 0.0);
         cycleFrom(c, levelHarmonics(k), n, level.data());
         float *dst = frame + mOffset[k];
         for (size_t i = 0; i < n; ++i)
            dst[i] = static_cast<float>(level[i]);
         dst[n] = dst[0];
         if (k == 0)
            for (double v : level)
               peak = std::max(peak, std::fabs(v));
      }
   }
   // The whole table to one scale, not each frame to its own: a frame that is
   // quieter than its neighbours is meant to be.
   if (peak > 1.0e-9) {
      const float g = static_cast<float>(1.0 / peak);
      for (float &v : mData)
         v *= g;
   }
   mFrames = frames;
   mStored.clear();
   return true;
}

bool Wavetable::build(const int16_t *cycles, int frames) {
   if (!cycles || frames <= 0) {
      mFrames = 0;
      mData.clear();
      mStored.clear();
      return false;
   }
   frames = std::min(frames, kWtMaxFrames);
   const size_t count = static_cast<size_t>(frames) * kWtSourceLen;
   std::vector<float> asFloat(count);
   for (size_t i = 0; i < count; ++i)
      asFloat[i] = static_cast<float>(cycles[i]) * (1.0f / 32767.0f);
   if (!build(asFloat.data(), frames))
      return false;
   mStored.assign(cycles, cycles + count);
   return true;
}

float Wavetable::sample(float phase, float position, float level) const {
   if (mFrames == 0)
      return 0.0f;
   const float pos = (position <= 0.0f ? 0.0f : (position >= 1.0f ? 1.0f : position)) *
                     static_cast<float>(mFrames - 1);
   const int f0 = static_cast<int>(pos);
   const int f1 = f0 + 1 < mFrames ? f0 + 1 : f0;
   const float ff = pos - static_cast<float>(f0);

   const float lv = level <= 0.0f ? 0.0f
                  : (level >= static_cast<float>(kWtLevels - 1) ? static_cast<float>(kWtLevels - 1)
                                                                 : level);
   const int l0 = static_cast<int>(lv);
   const int l1 = l0 + 1 < kWtLevels ? l0 + 1 : l0;
   const float lf = lv - static_cast<float>(l0);

   auto read = [&](int f, int l) {
      const float *t = mData.data() + static_cast<size_t>(f) * static_cast<size_t>(mFrameStride) +
                       mOffset[l];
      const float x = phase * static_cast<float>(mSize[l]);
      int i = static_cast<int>(x);
      if (i >= mSize[l])
         i = mSize[l] - 1;
      if (i < 0)
         i = 0;
      const float fr = x - static_cast<float>(i);
      return t[i] + (t[i + 1] - t[i]) * fr;
   };
   auto atLevel = [&](int l) {
      const float a = read(f0, l);
      return ff > 0.0f ? a + (read(f1, l) - a) * ff : a;
   };
   const float a = atLevel(l0);
   return lf > 0.0f ? a + (atLevel(l1) - a) * lf : a;
}

float Wavetable::levelFor(float hz, float sampleRate) {
   // Level k carries 512 >> k harmonics. Reading floor(L) with
   // L = log2(2048 hz / rate) keeps (512 >> floor(L)) * hz at or under half
   // the rate, so the richer of the two levels being crossfaded already fits.
   if (!(hz > 0.0f) || !(sampleRate > 0.0f))
      return 0.0f;
   const float l = std::log2(2048.0f * hz / sampleRate);
   return l <= 0.0f ? 0.0f : (l >= static_cast<float>(kWtLevels - 1)
                                 ? static_cast<float>(kWtLevels - 1)
                                 : l);
}

// ----------------------------------------------------------- built-in tables
//
// Eight tables of 32 frames, each a sweep of one idea from its plainest to its
// most extreme, so Shape at zero is always the calmest frame. Some are written
// as spectra and some as waveforms; a waveform is drawn at eight times the
// stored resolution and reduced to its harmonics, which is what keeps a hard
// edge in it -- the reset of a synced saw, the step of a narrow pulse -- from
// folding back into the table.

namespace {

constexpr int kBuiltinFrames = 32;
constexpr int kOversampled = kWtSourceLen * 8;

using Series = std::function<double(int h)>;    // sine amplitude of harmonic h
using Shape = std::function<double(double t)>;  // one cycle, t in 0..1

void fromSineSeries(const Series &amp, float *out) {
   std::vector<Complex> c(kSourceHarmonics + 1, Complex(0.0, 0.0));
   for (int h = 1; h <= kSourceHarmonics; ++h)
      c[static_cast<size_t>(h)] = Complex(0.0, -amp(h)); // Re(-i a e^(ix)) = a sin x
   std::vector<double> cycle(kWtSourceLen);
   cycleFrom(c, kSourceHarmonics, kWtSourceLen, cycle.data());
   for (int i = 0; i < kWtSourceLen; ++i)
      out[i] = static_cast<float>(cycle[static_cast<size_t>(i)]);
}

void fromShape(const Shape &shape, float *out) {
   std::vector<double> wave(kOversampled);
   for (int i = 0; i < kOversampled; ++i)
      wave[static_cast<size_t>(i)] = shape(static_cast<double>(i) / kOversampled);
   const std::vector<Complex> c = harmonicsOf(wave.data(), kOversampled, kSourceHarmonics);
   std::vector<double> cycle(kWtSourceLen);
   cycleFrom(c, kSourceHarmonics, kWtSourceLen, cycle.data());
   for (int i = 0; i < kWtSourceLen; ++i)
      out[i] = static_cast<float>(cycle[static_cast<size_t>(i)]);
}

double frac(double x) { return x - std::floor(x); }

// The classic analogue shapes as sine series. The ramp falls, like the
// machine's own.
double sawAmp(int h) { return 2.0 / (kPi * h); }
double squareAmp(int h) { return (h & 1) ? 4.0 / (kPi * h) : 0.0; }
double triangleAmp(int h) {
   if (!(h & 1))
      return 0.0;
   const double sign = ((h - 1) / 2) & 1 ? -1.0 : 1.0;
   return sign * 8.0 / (kPi * kPi * h * h);
}
double sineAmp(int h) { return h == 1 ? 1.0 : 0.0; }

// A vowel as its first three formants, in Hz. Typical values for an adult
// voice; the table is not trying to say a word, only to sound like one.
struct Vowel {
   double f[3];
};
constexpr Vowel kVowels[] = {
   {{730.0, 1090.0, 2440.0}}, // a
   {{530.0, 1840.0, 2480.0}}, // e
   {{270.0, 2290.0, 3010.0}}, // i
   {{570.0, 840.0, 2410.0}},  // o
   {{300.0, 870.0, 2240.0}},  // u
};

void makeFrame(int table, double t, float *out) {
   switch (table) {
   case kTableAnalog: {
      // Saw to square to triangle to sine, a third of the table each.
      const Series shapes[] = {sawAmp, squareAmp, triangleAmp, sineAmp};
      const double seg = t * 3.0;
      const int i = std::min(static_cast<int>(seg), 2);
      const double x = seg - i;
      fromSineSeries([&](int h) { return shapes[i](h) * (1.0 - x) + shapes[i + 1](h) * x; },
                     out);
      break;
   }
   case kTablePwm: {
      const double width = 0.5 - 0.47 * t;
      fromShape([width](double p) { return p < width ? 1.0 : -1.0; }, out);
      break;
   }
   case kTableVocal: {
      // Through a, e, i, o and u, the formants moving in a straight line
      // between each pair, over a source with a fundamental of 110 Hz -- which
      // fixes where the formants sit in the cycle, so like every formant
      // table it shifts them with the note.
      const double seg = t * 4.0;
      const int i = std::min(static_cast<int>(seg), 3);
      const double x = seg - i;
      double f[3];
      for (int k = 0; k < 3; ++k)
         f[k] = kVowels[i].f[k] * (1.0 - x) + kVowels[i + 1].f[k] * x;
      const double gain[3] = {1.0, 0.6, 0.3};
      const double width[3] = {90.0, 110.0, 150.0};
      fromSineSeries(
         [&](int h) {
            const double hz = 110.0 * h;
            double a = 0.02;
            for (int k = 0; k < 3; ++k) {
               const double d = (hz - f[k]) / width[k];
               a += gain[k] / (1.0 + d * d);
            }
            return a / std::sqrt(static_cast<double>(h));
         },
         out);
      break;
   }
   case kTableAdditive: {
      // One harmonic at a time, from a sine to forty-eight of them, the last
      // one fading in so the steps between frames are smooth.
      const double count = 1.0 + 47.0 * t;
      fromSineSeries(
         [count](int h) {
            const double in = count - (h - 1);
            if (in <= 0.0)
               return 0.0;
            return (in >= 1.0 ? 1.0 : in) / h;
         },
         out);
      break;
   }
   case kTableSync: {
      // A falling ramp hard-synced to the note, its own pitch climbing three
      // octaves across the table.
      const double ratio = std::pow(2.0, 3.0 * t);
      fromShape([ratio](double p) { return 1.0 - 2.0 * frac(ratio * p); }, out);
      break;
   }
   case kTableFm: {
      // Two operators at the same frequency, the index rising to 6.
      const double index = 6.0 * t;
      fromShape([index](double p) { return std::sin(kTwoPi * p + index * std::sin(kTwoPi * p)); },
                out);
      break;
   }
   case kTableFold: {
      // A sine driven through a sine: the folds multiply as the gain rises.
      const double gain = 1.0 + 7.0 * t;
      fromShape([gain](double p) { return std::sin(kPi * 0.5 * gain * std::sin(kTwoPi * p)); },
                out);
      break;
   }
   case kTableResonant:
   default: {
      // A ramp's spectrum with a resonant peak climbing from the second
      // harmonic to the forty-eighth: a filter sweep, frozen into the table.
      const double centre = std::log2(2.0) + t * std::log2(24.0);
      fromSineSeries(
         [centre](int h) {
            const double d = (std::log2(static_cast<double>(h)) - centre) / 0.18;
            return (1.0 + 7.0 * std::exp(-0.5 * d * d)) / h;
         },
         out);
      break;
   }
   }
}

const char *const kBuiltinNames[kNumBuiltinTables] = {"Analog", "PWM",  "Vocal", "Additive",
                                                      "Sync",   "FM",   "Fold",  "Resonant"};

struct BuiltinSet {
   Wavetable tables[kNumBuiltinTables];
   BuiltinSet() {
      std::vector<float> cycles(static_cast<size_t>(kBuiltinFrames) * kWtSourceLen);
      for (int t = 0; t < kNumBuiltinTables; ++t) {
         for (int f = 0; f < kBuiltinFrames; ++f)
            makeFrame(t, static_cast<double>(f) / (kBuiltinFrames - 1),
                      cycles.data() + static_cast<size_t>(f) * kWtSourceLen);
         tables[t].build(cycles.data(), kBuiltinFrames);
         tables[t].name = kBuiltinNames[t];
      }
   }
};

} // namespace

const Wavetable &builtinTable(int index) {
   // Built once per process, on first use, and never written again: the one
   // kind of file-scope state that is the same for every instance by
   // definition.
   static const BuiltinSet set;
   if (index < 0 || index >= kNumBuiltinTables)
      index = 0;
   return set.tables[index];
}

// ------------------------------------------------------------------- WAV files

namespace {

uint32_t le32(const unsigned char *p) {
   return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const unsigned char *p) {
   return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

bool readWavetableWav(const std::string &path, std::vector<int16_t> &stored, int &frames,
                      std::string &error) {
   // Through a UTF-8 path, so a file name outside the ANSI code page still
   // opens on Windows.
   std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
   if (!file) {
      error = "cannot open '" + path + "'";
      return false;
   }
   const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
   if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
       std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
      error = "not a WAV file";
      return false;
   }

   int format = 0, channels = 0, bits = 0;
   const unsigned char *data = nullptr;
   size_t dataBytes = 0;
   long clm = 0;
   size_t pos = 12;
   while (pos + 8 <= bytes.size()) {
      const unsigned char *chunk = bytes.data() + pos;
      const size_t size = le32(chunk + 4);
      const size_t body = pos + 8;
      const size_t avail = std::min(size, bytes.size() - body);
      if (std::memcmp(chunk, "fmt ", 4) == 0 && avail >= 16) {
         format = le16(chunk + 8);
         channels = le16(chunk + 10);
         bits = le16(chunk + 22);
         // WAVE_FORMAT_EXTENSIBLE carries the real format in its sub-format.
         if (format == 0xFFFE && avail >= 26)
            format = le16(chunk + 8 + 24);
      } else if (std::memcmp(chunk, "data", 4) == 0) {
         data = chunk + 8;
         dataBytes = avail;
      } else if (std::memcmp(chunk, "clm ", 4) == 0) {
         // "<!>2048 ..." -- the frame length, as text.
         const std::string text(reinterpret_cast<const char *>(chunk + 8), avail);
         const size_t mark = text.find("<!>");
         if (mark != std::string::npos)
            clm = std::strtol(text.c_str() + mark + 3, nullptr, 10);
      }
      pos = body + size + (size & 1);
   }
   if (!data || channels < 1) {
      error = "the WAV file has no audio in it";
      return false;
   }
   const bool isFloat = format == 3 && bits == 32;
   const bool isPcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
   if (!isFloat && !isPcm) {
      error = "unsupported WAV format (PCM 8/16/24/32-bit or 32-bit float only)";
      return false;
   }
   const size_t stride = static_cast<size_t>(channels) * static_cast<size_t>(bits / 8);
   const size_t total = dataBytes / stride;
   if (total < 8) {
      error = "the WAV file is too short to hold a cycle";
      return false;
   }
   std::vector<double> samples(total);
   for (size_t i = 0; i < total; ++i) {
      const unsigned char *s = data + i * stride; // the first channel
      double v = 0.0;
      if (isFloat) {
         float f;
         std::memcpy(&f, s, 4);
         v = std::isfinite(f) ? f : 0.0;
      } else if (bits == 8) {
         v = (static_cast<int>(s[0]) - 128) / 128.0;
      } else if (bits == 16) {
         v = static_cast<int16_t>(le16(s)) / 32768.0;
      } else if (bits == 24) {
         int32_t x = static_cast<int32_t>(s[0] | (s[1] << 8) | (s[2] << 16));
         if (x & 0x800000)
            x -= 0x1000000;
         v = x / 8388608.0;
      } else {
         v = static_cast<int32_t>(le32(s)) / 2147483648.0;
      }
      samples[i] = v;
   }

   size_t frameLen;
   if (clm > 0 && static_cast<size_t>(clm) <= total)
      frameLen = static_cast<size_t>(clm);
   else if (total % 2048 == 0)
      frameLen = 2048;
   else if (total < 4096)
      frameLen = total;
   else
      frameLen = 2048;
   const size_t available = total / frameLen;
   const int count = static_cast<int>(std::min<size_t>(available, kWtMaxFrames));

   // Each frame to a power-of-two length, so it can be reduced to its
   // harmonics: as it is if it already is one, or otherwise resampled round
   // its own cycle to at least twice its length, which keeps what the linear
   // interpolation loses at the top of the spectrum small.
   size_t n = frameLen;
   if (!isPowerOfTwo(frameLen)) {
      n = kWtSourceLen;
      while (n < 2 * frameLen)
         n <<= 1;
   }

   std::vector<double> cycles(static_cast<size_t>(count) * kWtSourceLen);
   std::vector<double> frame(n);
   double peak = 0.0;
   for (int f = 0; f < count; ++f) {
      // Thinned evenly across the file when it has more frames than fit.
      const size_t src = count > 1 ? static_cast<size_t>(std::llround(
                                        static_cast<double>(f) * (available - 1) / (count - 1)))
                                   : 0;
      const double *in = samples.data() + src * frameLen;
      if (n == frameLen) {
         std::copy(in, in + frameLen, frame.begin());
      } else {
         for (size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) * frameLen / n;
            const size_t a = static_cast<size_t>(x);
            const double fr = x - a;
            const double v0 = in[a % frameLen];
            const double v1 = in[(a + 1) % frameLen];
            frame[i] = v0 + (v1 - v0) * fr;
         }
      }
      const std::vector<Complex> c = harmonicsOf(frame.data(), n, kSourceHarmonics);
      double *out = cycles.data() + static_cast<size_t>(f) * kWtSourceLen;
      cycleFrom(c, kSourceHarmonics, kWtSourceLen, out);
      for (int i = 0; i < kWtSourceLen; ++i)
         peak = std::max(peak, std::fabs(out[i]));
   }
   if (!(peak > 1.0e-6)) {
      error = "the WAV file is silent";
      return false;
   }
   stored.resize(cycles.size());
   for (size_t i = 0; i < cycles.size(); ++i)
      stored[i] = static_cast<int16_t>(std::lround(cycles[i] / peak * 32767.0));
   frames = count;
   return true;
}

// ------------------------------------------------------------- the folder

namespace {

constexpr int kMaxFolderDepth = 8;

std::string lowered(const std::string &s) {
   std::string out = s;
   for (char &c : out)
      if (c >= 'A' && c <= 'Z')
         c = static_cast<char>(c - 'A' + 'a');
   return out;
}

bool isWav(const std::filesystem::path &p) { return lowered(p.extension().u8string()) == ".wav"; }

// Appends `dir`'s tree to `out` and says whether it held any WAV file at all.
bool scanFolder(const std::filesystem::path &dir, const std::string &prefix, int depth,
                std::vector<TableFile> &out) {
   std::vector<std::filesystem::path> folders, files;
   std::error_code ec;
   for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().u8string();
      if (name.empty() || name[0] == '.')
         continue;
      std::error_code typeEc;
      if (entry.is_directory(typeEc)) {
         if (depth + 1 < kMaxFolderDepth)
            folders.push_back(entry.path());
      } else if (entry.is_regular_file(typeEc) && isWav(entry.path())) {
         files.push_back(entry.path());
      }
   }
   auto byName = [](const std::filesystem::path &a, const std::filesystem::path &b) {
      return lowered(a.filename().u8string()) < lowered(b.filename().u8string());
   };
   std::sort(folders.begin(), folders.end(), byName);
   std::sort(files.begin(), files.end(), byName);

   bool any = false;
   for (const auto &f : folders) {
      const std::string name = f.filename().u8string();
      const size_t at = out.size();
      out.push_back({prefix + name, name, depth, true});
      if (scanFolder(f, prefix + name + "/", depth + 1, out))
         any = true;
      else
         out.erase(out.begin() + static_cast<std::ptrdiff_t>(at)); // nothing in it
   }
   for (const auto &f : files) {
      const std::string name = f.filename().u8string();
      out.push_back({prefix + name, f.stem().u8string(), depth, false});
      any = true;
   }
   return any;
}

} // namespace

std::vector<TableFile> listWavetableFolder(const std::string &root) {
   std::vector<TableFile> out;
   std::error_code ec;
   const std::filesystem::path dir = std::filesystem::u8path(root);
   if (!root.empty() && std::filesystem::is_directory(dir, ec))
      scanFolder(dir, std::string(), 0, out);
   return out;
}

std::string tableNameFor(const std::string &relativePath) {
   const size_t dot = relativePath.find_last_of('.');
   const size_t slash = relativePath.find_last_of('/');
   if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
      return relativePath;
   return relativePath.substr(0, dot);
}

// ----------------------------------------------------------------- base64

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string encodeTable(const std::vector<int16_t> &samples) {
   std::vector<unsigned char> raw(samples.size() * 2);
   for (size_t i = 0; i < samples.size(); ++i) {
      const uint16_t u = static_cast<uint16_t>(samples[i]);
      raw[2 * i] = static_cast<unsigned char>(u & 0xFF);
      raw[2 * i + 1] = static_cast<unsigned char>(u >> 8);
   }
   std::string out;
   out.reserve((raw.size() + 2) / 3 * 4);
   for (size_t i = 0; i < raw.size(); i += 3) {
      const uint32_t b = (static_cast<uint32_t>(raw[i]) << 16) |
                         (i + 1 < raw.size() ? static_cast<uint32_t>(raw[i + 1]) << 8 : 0u) |
                         (i + 2 < raw.size() ? static_cast<uint32_t>(raw[i + 2]) : 0u);
      out.push_back(kB64[(b >> 18) & 63]);
      out.push_back(kB64[(b >> 12) & 63]);
      out.push_back(i + 1 < raw.size() ? kB64[(b >> 6) & 63] : '=');
      out.push_back(i + 2 < raw.size() ? kB64[b & 63] : '=');
   }
   return out;
}

bool decodeTable(const std::string &text, std::vector<int16_t> &samples) {
   auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z')
         return c - 'A';
      if (c >= 'a' && c <= 'z')
         return c - 'a' + 26;
      if (c >= '0' && c <= '9')
         return c - '0' + 52;
      if (c == '+')
         return 62;
      if (c == '/')
         return 63;
      return -1;
   };
   std::vector<unsigned char> raw;
   raw.reserve(text.size() / 4 * 3);
   uint32_t acc = 0;
   int bitsHeld = 0;
   for (char c : text) {
      if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
         continue;
      const int v = value(c);
      if (v < 0)
         return false;
      acc = (acc << 6) | static_cast<uint32_t>(v);
      bitsHeld += 6;
      if (bitsHeld >= 8) {
         bitsHeld -= 8;
         raw.push_back(static_cast<unsigned char>((acc >> bitsHeld) & 0xFF));
      }
   }
   if (raw.size() % 2)
      return false;
   samples.resize(raw.size() / 2);
   for (size_t i = 0; i < samples.size(); ++i)
      samples[i] = static_cast<int16_t>(raw[2 * i] | (raw[2 * i + 1] << 8));
   return true;
}

} // namespace saeurekiste
