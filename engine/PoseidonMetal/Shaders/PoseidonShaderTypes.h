#ifndef POSEIDON_METAL_SHADER_TYPES_H
#define POSEIDON_METAL_SHADER_TYPES_H

#ifdef __METAL_VERSION__
#define POSEIDON_FLOAT4 float4
#else
#include <cstddef>
struct alignas(16) PoseidonFloat4
{
    float x, y, z, w;
};
#define POSEIDON_FLOAT4 PoseidonFloat4
#endif

struct VSConstantsPod
{
    POSEIDON_FLOAT4 slots[70];
};

struct PSConstantsPod
{
    POSEIDON_FLOAT4 slots[27];
};

#ifndef __METAL_VERSION__
static_assert(sizeof(VSConstantsPod) == 70 * 16, "VSConstantsPod layout drift");
static_assert(sizeof(PSConstantsPod) == 27 * 16, "PSConstantsPod layout drift");
#endif

#undef POSEIDON_FLOAT4

#endif
