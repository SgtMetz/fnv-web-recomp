/* Force-included (by tools/overlay.py) into decomp units that bring their own MSVC-layout union __m128 and _mm_*
 * declarations. Pre-defining clang's include guards keeps its vector-typed SSE headers out, so the unit's own
 * declarations are the only ones in scope. The functions themselves come from port/compat/fnv_msvc_xmm.cpp. */
#pragma once
#define __MMINTRIN_H
#define __XMMINTRIN_H
#define __EMMINTRIN_H
#define __PMMINTRIN_H
#define __IMMINTRIN_H
#define __X86INTRIN_H
