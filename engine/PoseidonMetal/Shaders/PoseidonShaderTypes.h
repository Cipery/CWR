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

// Shared named offsets for the byte-identical GL33/Metal constant blocks.
// Values are float4 slots, not byte offsets.
enum PoseidonVSConstantSlot
{
    PoseidonVSSlotProjection = 0,
    PoseidonVSSlotView = 4,
    PoseidonVSSlotWorld = 8,
    PoseidonVSSlotSunDirection = 12,
    PoseidonVSSlotAmbient = 13,
    PoseidonVSSlotDiffuse = 14,
    PoseidonVSSlotEmissive = 15,
    PoseidonVSSlotFog = 16,
    PoseidonVSSlotCamera = 17,
    PoseidonVSSlotSpecular = 18,
    PoseidonVSSlotSpecularEnabled = 19,
    PoseidonVSSlotSunEnabled = 20,
    PoseidonVSSlotViewportScale = 21,
    PoseidonVSSlotTexMatrix0 = 24,
    PoseidonVSSlotTexMatrix1 = 28,
    PoseidonVSSlotTexControl = 32,
    PoseidonVSSlotLightCount = 33,
    PoseidonVSSlotLightPosition = 34,
    PoseidonVSSlotLightDiffuse = 42,
    PoseidonVSSlotLightAmbient = 50,
    PoseidonVSSlotLightDirection = 58,
    PoseidonVSSlotLightViewProjection = 66,
};

enum PoseidonPSConstantSlot
{
    PoseidonPSSlotFogColor = 0,
    PoseidonPSSlotAlphaRef = 1,
    PoseidonPSSlotShadowControl = 2,
    PoseidonPSSlotConstantColor = 3,
    PoseidonPSSlotLightDirection = 4,
    PoseidonPSSlotGrassCoefficient1 = 5,
    PoseidonPSSlotGrassCoefficient2 = 6,
    PoseidonPSSlotNightEye = 7,
    PoseidonPSSlotCascadeViewProjection = 8,
    PoseidonPSSlotCascadeSplits = 24,
    PoseidonPSSlotCascadeControl = 25,
    PoseidonPSSlotCameraForward = 26,
};

#ifndef __METAL_VERSION__
static_assert(sizeof(VSConstantsPod) == 70 * 16, "VSConstantsPod layout drift");
static_assert(sizeof(PSConstantsPod) == 27 * 16, "PSConstantsPod layout drift");
static_assert(PoseidonVSSlotLightViewProjection + 4 == 70, "VS constant slots do not cover the full block");
static_assert(PoseidonPSSlotCameraForward + 1 == 27, "PS constant slots do not cover the full block");
#endif

#undef POSEIDON_FLOAT4

#endif
