#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)
#include "sse2neon.h"
#elif defined(_MSC_VER)
#include <intrin.h>
#elif defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace Poseidon::Foundation
{
typedef __m128 Vec4M128[4];

struct Quatrix4
{
    Vec4M128 qa[3];

    operator Vec4M128*() { return qa; }
    operator const Vec4M128*() const { return qa; }
};

typedef __m128 Vec3M128[3];

struct Quatrix3
{
    Vec3M128 qa[3];

    operator Vec3M128*() { return qa; }
    operator const Vec3M128*() const { return qa; }
};

} // namespace Poseidon::Foundation

using ::Poseidon::Foundation::Quatrix3;
using ::Poseidon::Foundation::Quatrix4;
