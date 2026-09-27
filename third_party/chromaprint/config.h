/* third_party/chromaprint/config.h
 *
 * Chromaprint's CMakeLists.txt generates this header from config.h.in by
 * probing the toolchain. Because third_party/ is vendored source that has to
 * build with no configure step and no network, the detected values are frozen
 * here instead:
 *
 *   HAVE_ROUND / HAVE_LRINTF : MSVC's <math.h> provides both. They must be
 *                              defined, otherwise utils.h defines its own
 *                              static inline round(), which collides with the
 *                              one the CRT already declared.
 *   USE_KISSFFT              : FFT backend. This build uses the kissfft copy
 *                              in third_party/chromaprint/kissfft -- Chromaprint's
 *                              own bundled copy, which fft_lib_kissfft.cpp was
 *                              written against. It is the only backend here
 *                              that needs no FFmpeg/FFTW/Accelerate.
 *
 * Upstream #cmakedefines everything else conditionally (USE_AVFFT, USE_FFTW3,
 * USE_VDSP, USE_INTERNAL_AVRESAMPLE, USE_SWRESAMPLE, ...). Leaving them
 * undefined is exactly upstream's generated header for a feature-less build:
 * `#if UNDEFINED_MACRO` evaluates to 0, so those code paths stay compiled out.
 */

#ifndef CHROMAPRINT_CONFIG_H_
#define CHROMAPRINT_CONFIG_H_

#define HAVE_ROUND 1
#define HAVE_LRINTF 1
#define USE_KISSFFT 1

#endif /* CHROMAPRINT_CONFIG_H_ */
