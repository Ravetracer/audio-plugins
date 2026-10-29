#include "dsp/curve.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace shapeau {

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

// A quadratic Bezier whose x is exactly t, so the segment's value can be found
// without solving anything: the control point sits at (0.5, mid + bend / 2).
// bend 0 is therefore the straight line, positive bows the segment towards the
// top of the editor and negative towards the bottom, whatever direction the
// segment runs in. Clamped to the editor's range because a deep bend on a flat
// segment can otherwise overshoot it.
float bentValue(const CurvePoint &a, const CurvePoint &b, float t) {
   if (a.bend == 0.0f)
      return a.y + (b.y - a.y) * t;
   const float bend = a.bend < -1.0f ? -1.0f : (a.bend > 1.0f ? 1.0f : a.bend);
   const float controlY = 0.5f * (a.y + b.y) + 0.5f * bend;
   const float u = 1.0f - t;
   return clamp01(u * u * a.y + 2.0f * u * t * controlY + t * t * b.y);
}

} // namespace

Curve::Curve() { setFlat(1.0f); }

void Curve::setFlat(float y) {
   mCount = 2;
   mPoints[0] = {0.0f, clamp01(y), 0};
   mPoints[1] = {1.0f, clamp01(y), 0};
}

float Curve::valueAt(float phase) const {
   if (!(phase >= 0.0f))
      phase = 0.0f;
   // Exactly one is the end of the cycle, not the start of the next one: the
   // editor samples the whole domain including both ends, and wrapping 1.0 to
   // 0.0 would draw the shape's right-hand edge as a vertical drop to the first
   // point. A phase past one still wraps, which is what the engine's modulated
   // phases rely on.
   if (phase >= 1.0f) {
      if (phase == 1.0f)
         return mPoints[mCount - 1].y;
      phase -= std::floor(phase);
   }

   // Before the first or after the last point cannot happen with the endpoints
   // enforced, but a curve loaded from a file that ignored that would still be
   // answered rather than reading out of bounds.
   if (phase <= mPoints[0].x)
      return mPoints[0].y;
   if (phase >= mPoints[mCount - 1].x)
      return mPoints[mCount - 1].y;

   int i = 0;
   while (i + 1 < mCount && mPoints[i + 1].x < phase)
      ++i;

   const CurvePoint &a = mPoints[i];
   const CurvePoint &b = mPoints[i + 1];
   const float span = b.x - a.x;
   if (!(span > 1.0e-6f))
      return b.y;

   const float t = clamp01((phase - a.x) / span);
   // The harder of the two weights shapes the segment: a soft point smooths
   // both of its sides, which is what makes a single soft point read as a hump.
   const uint8_t w = std::max(a.weight, b.weight);
   float shape = t;
   if (w == 1)
      shape = 0.5f * t + 0.5f * smoothstep(t);
   else if (w >= 2)
      shape = smoothstep(t);
   return bentValue(a, b, shape);
}

int Curve::addPoint(float x, float y, uint8_t weight) {
   x = clamp01(x);
   y = clamp01(y);
   if (weight > 2)
      weight = 2;

   // On top of an existing point: move it instead of stacking two there, so a
   // double click in the editor never leaves an invisible knot.
   for (int i = 0; i < mCount; ++i) {
      if (std::fabs(mPoints[i].x - x) < 0.001f) {
         mPoints[i].y = y;
         mPoints[i].weight = weight;
         return i;
      }
   }
   if (mCount >= kMaxCurvePoints)
      return -1;

   // The endpoints stay put horizontally; a click at either end moves its
   // value rather than replacing the point that holds the shape down.
   if (x < 0.001f) {
      mPoints[0].y = y;
      mPoints[0].weight = weight;
      return 0;
   }
   if (x > 0.999f) {
      mPoints[mCount - 1].y = y;
      mPoints[mCount - 1].weight = weight;
      return mCount - 1;
   }

   int at = mCount;
   for (int i = 1; i < mCount; ++i) {
      if (mPoints[i].x > x) {
         at = i;
         break;
      }
   }
   for (int i = mCount; i > at; --i)
      mPoints[i] = mPoints[i - 1];
   mPoints[at] = {x, y, weight};
   ++mCount;
   return at;
}

void Curve::removePoint(int i) {
   // The two endpoints are the curve's domain, not its contents.
   if (i <= 0 || i >= mCount - 1 || mCount <= 2)
      return;
   for (int k = i; k + 1 < mCount; ++k)
      mPoints[k] = mPoints[k + 1];
   --mCount;
}

void Curve::movePoint(int i, float x, float y) {
   if (i < 0 || i >= mCount)
      return;
   y = clamp01(y);
   if (i == 0 || i == mCount - 1) {
      mPoints[i].y = y; // endpoints move vertically only
      return;
   }
   mPoints[i].x = clamp01(x);
   mPoints[i].y = y;
   sortMiddle();
}

void Curve::setWeight(int i, uint8_t weight) {
   if (i < 0 || i >= mCount)
      return;
   mPoints[i].weight = weight > 2 ? 2 : weight;
}

void Curve::setBend(int i, float bend) {
   if (i < 0 || i >= mCount)
      return;
   mPoints[i].bend = bend < -1.0f ? -1.0f : (bend > 1.0f ? 1.0f : bend);
}

void Curve::removeRange(float lo, float hi) {
   if (hi <= lo)
      return;
   int out = 0;
   for (int i = 0; i < mCount; ++i) {
      const bool interior = i > 0 && i < mCount - 1;
      if (interior && mPoints[i].x > lo && mPoints[i].x < hi)
         continue;
      mPoints[out++] = mPoints[i];
   }
   mCount = out;
   if (mCount < 2) {
      // The endpoints cannot be removed, but a malformed curve could have
      // fewer points than that; a flat one is still a shape.
      setFlat(1.0f);
   }
}

void Curve::setStep(int step, int steps, float y) {
   if (steps < 1 || steps > 32)
      return;
   step = std::max(0, std::min(step, steps - 1));
   y = clamp01(y);

   const float a = static_cast<float>(step) / static_cast<float>(steps);
   const float b = static_cast<float>(step + 1) / static_cast<float>(steps);
   // A hand-drawn curve can have points in the cell; a step has none. The gap
   // is wide enough for the plateau's two boundary points to stay distinct in
   // addPoint(), and narrow enough (a third of a pixel at the editor's width)
   // to read as a vertical edge.
   const float edge = b >= 1.0f ? 1.0f : b - 0.002f;
   removeRange(a + 0.0016f, edge - 0.0016f);

   addPoint(a, y, 0);
   addPoint(edge, y, 0);
   mPoints[0].x = 0.0f;
   mPoints[mCount - 1].x = 1.0f;
}

void Curve::fillSteps(int steps) {
   if (steps < 1 || steps > 32)
      return;
   float values[32];
   for (int i = 0; i < steps; ++i)
      values[i] = valueAt((static_cast<float>(i) + 0.5f) / static_cast<float>(steps));

   setFlat(values[0]);
   for (int i = 0; i < steps; ++i)
      setStep(i, steps, values[i]);
}

void Curve::sortMiddle() {
   // Insertion sort over the interior only, which is at most 30 elements and
   // almost always already in order by one position.
   for (int i = 1; i < mCount - 1; ++i)
      for (int k = i; k > 0; --k) {
         if (mPoints[k - 1].x <= mPoints[k].x)
            break;
         std::swap(mPoints[k - 1], mPoints[k]);
      }
}

std::string Curve::format() const {
   std::string out;
   char buf[64];
   for (int i = 0; i < mCount; ++i) {
      std::snprintf(buf, sizeof(buf), "%s%.4f %.4f %u %.3f", i ? ", " : "", mPoints[i].x,
                    mPoints[i].y, static_cast<unsigned>(mPoints[i].weight), mPoints[i].bend);
      out += buf;
   }
   return out;
}

bool Curve::parse(const std::string &text) {
   Curve parsed;
   parsed.mCount = 0;

   size_t pos = 0;
   for (;;) {
      const size_t comma = text.find(',', pos);
      const std::string group =
         text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);

      const char *p = group.c_str();
      float v[4] = {0.0f, 1.0f, 0.0f, 0.0f};
      int n = 0;
      while (n < 4) {
         while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
         if (!*p)
            break;
         char *end = nullptr;
         v[n] = std::strtof(p, &end);
         if (end == p)
            return false;
         p = end;
         ++n;
      }
      // Three numbers is the format before segment bend existed: a straight
      // segment. More than four is not a point.
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
         ++p;
      if ((n != 3 && n != 4) || *p != '\0')
         return false;
      // Out of range on either axis is a malformed curve, not something to
      // clamp: clamping would silently reshape a file somebody hand-edited.
      if (v[0] < 0.0f || v[0] > 1.0f || v[1] < 0.0f || v[1] > 1.0f)
         return false;
      if (parsed.mCount >= kMaxCurvePoints)
         return false;
      const uint8_t weight = static_cast<uint8_t>(v[2] < 0.0f ? 0.0f : (v[2] > 2.0f ? 2.0f : v[2]));
      const float bend = n == 4 ? (v[3] < -1.0f ? -1.0f : (v[3] > 1.0f ? 1.0f : v[3])) : 0.0f;
      parsed.mPoints[parsed.mCount++] = {v[0], v[1], weight, bend};

      if (comma == std::string::npos)
         break;
      pos = comma + 1;
   }

   if (parsed.mCount < 2)
      return false;

   parsed.sortMiddle();
   // The domain is enforced rather than validated: the endpoints are what make
   // the shape cover one cycle, and a file without them is a file to fix.
   parsed.mPoints[0].x = 0.0f;
   parsed.mPoints[parsed.mCount - 1].x = 1.0f;
   *this = parsed;
   return true;
}

float neutralValue(int module) {
   // Pan's middle is the centre and width's middle is the plain image; every
   // other module does nothing at the top of its travel.
   return (module == kModulePan || module == kModuleWidth) ? 0.5f : 1.0f;
}

void resetCurves(CurveSet &set) {
   for (int m = 0; m < kNumModules; ++m)
      for (int b = 0; b < kNumBands; ++b)
         set.curve[m][b].setFlat(neutralValue(m));
   set.chainCount = 0;
}

int effectiveChain(const CurveSet &set, const bool *on, int *out) {
   int n = 0;
   for (int i = 0; i < set.chainCount; ++i)
      out[n++] = set.chain[i];
   for (int module : kDefaultChain)
      if (on[module] && !chainHas(set, module))
         out[n++] = module;
   return n;
}

bool chainHas(const CurveSet &set, int module) {
   for (int i = 0; i < set.chainCount; ++i)
      if (set.chain[i] == module)
         return true;
   return false;
}

void chainAdd(CurveSet &set, int module) {
   if (module < 0 || module >= kNumModules || chainHas(set, module))
      return;
   if (set.chainCount < 0)
      set.chainCount = 0;
   set.chain[set.chainCount++] = static_cast<int8_t>(module);
}

void chainRemove(CurveSet &set, int module) {
   int out = 0;
   for (int i = 0; i < set.chainCount; ++i)
      if (set.chain[i] != module)
         set.chain[out++] = set.chain[i];
   set.chainCount = static_cast<int8_t>(out);
}

void chainMove(CurveSet &set, int module, int place) {
   if (!chainHas(set, module))
      return;
   chainRemove(set, module);
   place = std::max(0, std::min(place, static_cast<int>(set.chainCount)));
   for (int i = set.chainCount; i > place; --i)
      set.chain[i] = set.chain[i - 1];
   set.chain[place] = static_cast<int8_t>(module);
   ++set.chainCount;
}

std::string formatChain(const CurveSet &set) {
   std::string out;
   for (int i = 0; i < set.chainCount; ++i) {
      if (i)
         out += ' ';
      out += moduleKey(set.chain[i]);
   }
   return out;
}

bool parseChain(const std::string &text, CurveSet &set) {
   set.chainCount = 0;
   std::istringstream in(text);
   std::string word;
   while (in >> word) {
      const int m = moduleFromKey(word);
      if (m < 0)
         return false;
      chainAdd(set, m);
   }
   return true;
}

const char *moduleKey(int module) {
   switch (module) {
   case kModuleVolume:
      return "vol";
   case kModuleFilter:
      return "flt";
   case kModuleDrive:
      return "drv";
   case kModuleTime:
      return "tim";
   case kModuleCrush:
      return "crs";
   case kModulePan:
      return "pan";
   case kModuleWidth:
      return "wid";
   default:
      return "vol";
   }
}

int moduleFromKey(const std::string &key) {
   for (int m = 0; m < kNumModules; ++m)
      if (key == moduleKey(m))
         return m;
   return -1;
}

} // namespace shapeau
