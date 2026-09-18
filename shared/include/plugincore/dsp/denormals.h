#pragma once

#if defined(__SSE2__) || defined(__x86_64__)
#   include <xmmintrin.h>
#   define PLUGINCORE_HAVE_MXCSR 1
#endif

namespace plugincore {

// Disables denormal arithmetic for the lifetime of the object and restores the
// previous FPU mode afterwards.
//
// Reverb feedback paths and filter states decay towards zero and spend a long
// time in the denormal range, where x86 arithmetic slows down by an order of
// magnitude. Setting flush-to-zero here -- rather than compiling with
// -ffast-math, which links crtfastmath.o and changes the mode for the whole
// host process -- keeps the fix contained to our own process() call.
class ScopedNoDenormals {
public:
   ScopedNoDenormals() {
#ifdef PLUGINCORE_HAVE_MXCSR
      mSaved = _mm_getcsr();
      // 0x8000 = flush-to-zero, 0x0040 = denormals-are-zero
      _mm_setcsr((mSaved | 0x8000u | 0x0040u));
#endif
   }

   ~ScopedNoDenormals() {
#ifdef PLUGINCORE_HAVE_MXCSR
      _mm_setcsr(mSaved);
#endif
   }

   ScopedNoDenormals(const ScopedNoDenormals &) = delete;
   ScopedNoDenormals &operator=(const ScopedNoDenormals &) = delete;

private:
#ifdef PLUGINCORE_HAVE_MXCSR
   unsigned int mSaved = 0;
#endif
};

} // namespace plugincore
