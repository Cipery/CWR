#include <metal_stdlib>
#include "PoseidonShaderTypes.h"

using namespace metal;

struct ScreenVertexIn
{
    float3 position [[attribute(0)]];
    float rhw [[attribute(1)]];
    float4 color [[attribute(2)]];
    float4 specular [[attribute(3)]];
    float2 uv0 [[attribute(4)]];
    float2 uv1 [[attribute(5)]];
};

struct RasterVertex
{
    float4 position [[position]];
    float4 color;
    float4 specular;
    float2 uv0;
    float2 uv1;
    float fogTC;
    float3 worldRelative;
};

vertex RasterVertex vsScreen(ScreenVertexIn input [[stage_in]],
                             constant VSConstantsPod& constants [[buffer(0)]])
{
    const float4 viewportScale = constants.slots[PoseidonVSSlotViewportScale];
    const float w = 1.0f / input.rhw;
    RasterVertex output;
    output.position = float4((input.position.x * viewportScale.x - 1.0f) * w,
                             (1.0f - input.position.y * viewportScale.y) * w,
                             input.position.z * w, w);
    output.color = input.color;
    output.specular = input.specular;
    output.uv0 = input.uv0;
    output.uv1 = input.uv1;
    output.fogTC = input.specular.a;
    output.worldRelative = float3(0.0f);
    return output;
}

struct WorldVertexIn
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
};

vertex RasterVertex vsTransform(WorldVertexIn input [[stage_in]],
                                constant VSConstantsPod& constants [[buffer(0)]],
                                device const float4x4* worldInstances [[buffer(2)]],
                                uint instanceId [[instance_id]])
{
    const float4x4 projection =
        float4x4(constants.slots[PoseidonVSSlotProjection], constants.slots[PoseidonVSSlotProjection + 1],
                 constants.slots[PoseidonVSSlotProjection + 2], constants.slots[PoseidonVSSlotProjection + 3]);
    const float4x4 view =
        float4x4(constants.slots[PoseidonVSSlotView], constants.slots[PoseidonVSSlotView + 1],
                 constants.slots[PoseidonVSSlotView + 2], constants.slots[PoseidonVSSlotView + 3]);
    const float4x4 world = worldInstances[instanceId];
    const float4 sunDirection = constants.slots[PoseidonVSSlotSunDirection];
    const float4 ambient = constants.slots[PoseidonVSSlotAmbient];
    const float4 diffuse = constants.slots[PoseidonVSSlotDiffuse];
    const float4 emissive = constants.slots[PoseidonVSSlotEmissive];
    const float4 fog = constants.slots[PoseidonVSSlotFog];
    const float4 camera = constants.slots[PoseidonVSSlotCamera];
    const float4 specular = constants.slots[PoseidonVSSlotSpecular];
    const float4 specularEnabled = constants.slots[PoseidonVSSlotSpecularEnabled];
    const float4 sunEnabled = constants.slots[PoseidonVSSlotSunEnabled];
    const float4x4 texMatrix0 =
        float4x4(constants.slots[PoseidonVSSlotTexMatrix0], constants.slots[PoseidonVSSlotTexMatrix0 + 1],
                 constants.slots[PoseidonVSSlotTexMatrix0 + 2], constants.slots[PoseidonVSSlotTexMatrix0 + 3]);
    const float4x4 texMatrix1 =
        float4x4(constants.slots[PoseidonVSSlotTexMatrix1], constants.slots[PoseidonVSSlotTexMatrix1 + 1],
                 constants.slots[PoseidonVSSlotTexMatrix1 + 2], constants.slots[PoseidonVSSlotTexMatrix1 + 3]);
    const float4 texControl = constants.slots[PoseidonVSSlotTexControl];

    const float4 worldPosition = world * float4(input.position, 1.0f);
    const float3x3 normalMatrix = float3x3(world[0].xyz, world[1].xyz, world[2].xyz);
    const float3 worldNormal = normalize(normalMatrix * input.normal);
    const float4 viewPosition = view * worldPosition;

    RasterVertex output;
    output.position = projection * viewPosition;
    output.worldRelative = worldPosition.xyz;

    const float nDotL = max(0.0f, dot(worldNormal, -sunDirection.xyz));
    float4 litColor = emissive + (ambient + diffuse * nDotL) * sunEnabled.x;
    const int lightCount = min(int(constants.slots[PoseidonVSSlotLightCount].x), 8);
    constexpr float minInside2 = 0.95677279f;
    constexpr float maxInside2 = 0.98063081f;
    for (int i = 0; i < lightCount; ++i)
    {
        const float4 lightPosition = constants.slots[PoseidonVSSlotLightPosition + i];
        const float3 toLight = lightPosition.xyz - worldPosition.xyz;
        const float size2 = dot(toLight, toLight);
        const float startAttenuation2 = lightPosition.w * lightPosition.w;
        if (size2 >= startAttenuation2 * 100.0f || size2 <= 0.0f)
            continue;

        float cone = 1.0f;
        const float4 localDirection = constants.slots[PoseidonVSSlotLightDirection + i];
        if (localDirection.w > 0.5f)
        {
            const float inside = -dot(toLight, localDirection.xyz);
            if (inside <= 0.0f)
                continue;
            const float cosine2 = inside * inside / size2;
            if (cosine2 < minInside2)
                continue;
            cone = clamp((cosine2 - minInside2) / (maxInside2 - minInside2), 0.0f, 1.0f);
        }
        const float attenuation = size2 >= startAttenuation2 ? startAttenuation2 / size2 : 1.0f;
        const float cosine = dot(toLight, worldNormal);
        if (cosine > 0.0f)
        {
            const float3 contribution =
                (constants.slots[PoseidonVSSlotLightDiffuse + i].rgb * (cosine * rsqrt(size2)) +
                 constants.slots[PoseidonVSSlotLightAmbient + i].rgb) *
                (attenuation * cone);
            litColor.rgb += contribution;
        }
        else
        {
            litColor.rgb += constants.slots[PoseidonVSSlotLightAmbient + i].rgb * attenuation;
        }
    }
    output.color = clamp(litColor, 0.0f, 1.0f);

    float3 specularLight = float3(0.0f);
    if (specularEnabled.x > 0.5f && sunEnabled.x > 0.0f)
    {
        const float3 viewDirection = normalize(camera.xyz - worldPosition.xyz);
        const float3 halfVector = normalize(-sunDirection.xyz + viewDirection);
        const float nDotH = max(0.0f, dot(worldNormal, halfVector));
        specularLight = specular.rgb * pow(nDotH, max(1.0f, specular.w)) * sunEnabled.x;
    }
    output.specular = float4(clamp(specularLight, 0.0f, 1.0f), 0.0f);

    const float distanceFromCamera = length(worldPosition.xyz - camera.xyz);
    const float fogFactor = clamp(1.0f - (distanceFromCamera - fog.x) * fog.y, 0.0f, 1.0f);
    output.fogTC = fog.z > 0.5f ? fogFactor : 1.0f;
    output.uv0 = texControl.x > 0.5f ? (texMatrix0 * float4(input.uv, 0.0f, 1.0f)).xy : input.uv;
    output.uv1 = texControl.y > 0.5f ? (texMatrix1 * float4(input.uv, 0.0f, 1.0f)).xy : input.uv;
    return output;
}

vertex RasterVertex vsShadow(WorldVertexIn input [[stage_in]],
                             constant VSConstantsPod& constants [[buffer(0)]],
                             device const float4x4* worldInstances [[buffer(2)]],
                             uint instanceId [[instance_id]])
{
    const float4x4 projection =
        float4x4(constants.slots[PoseidonVSSlotProjection], constants.slots[PoseidonVSSlotProjection + 1],
                 constants.slots[PoseidonVSSlotProjection + 2], constants.slots[PoseidonVSSlotProjection + 3]);
    const float4x4 view =
        float4x4(constants.slots[PoseidonVSSlotView], constants.slots[PoseidonVSSlotView + 1],
                 constants.slots[PoseidonVSSlotView + 2], constants.slots[PoseidonVSSlotView + 3]);
    const float4x4 texMatrix =
        float4x4(constants.slots[PoseidonVSSlotTexMatrix0], constants.slots[PoseidonVSSlotTexMatrix0 + 1],
                 constants.slots[PoseidonVSSlotTexMatrix0 + 2], constants.slots[PoseidonVSSlotTexMatrix0 + 3]);
    const float4 texControl = constants.slots[PoseidonVSSlotTexControl];

    RasterVertex output;
    output.position = projection * view * worldInstances[instanceId] * float4(input.position, 1.0f);
    output.color = constants.slots[PoseidonVSSlotDiffuse];
    output.specular = float4(0.0f);
    output.uv0 = texControl.x > 0.5f ? (texMatrix * float4(input.uv, 0.0f, 1.0f)).xy : input.uv;
    output.uv1 = output.uv0;
    output.fogTC = 1.0f;
    output.worldRelative = float3(0.0f);
    return output;
}

float CascadeShadowFactor(RasterVertex input, constant PSConstantsPod& constants,
                          depth2d_array<float> shadowMap, sampler shadowSampler)
{
    const float4 shadowControl = constants.slots[PoseidonPSSlotShadowControl];
    if (shadowControl.x <= 0.5f)
        return 1.0f;

    const float4 splits = constants.slots[PoseidonPSSlotCascadeSplits];
    const float4 cascadeControl = constants.slots[PoseidonPSSlotCascadeControl];
    const float3 cameraForward = constants.slots[PoseidonPSSlotCameraForward].xyz;
    const int cascadeCount = clamp(int(cascadeControl.x), 0, 4);
    const int omniCount = clamp(int(cascadeControl.w), 0, cascadeCount);
    const float eyeDepth = dot(input.worldRelative, cameraForward);
    const float distance3D = length(input.worldRelative);

    int cascade = cascadeCount;
    for (int i = 0; i < 4; ++i)
    {
        if (i >= cascadeCount)
            break;
        const float metric = i < omniCount ? distance3D : eyeDepth;
        if (metric <= splits[i])
        {
            cascade = i;
            break;
        }
    }
    if (cascade >= cascadeCount)
        return 1.0f;

    const float previousEdge = cascade > 0 ? splits[cascade - 1] : 0.0f;
    const float primaryMetric = cascade < omniCount ? distance3D : eyeDepth;
    const float band = (splits[cascade] - previousEdge) * 0.15f;
    const float blendWeight = cascade + 1 < cascadeCount
                                  ? clamp((primaryMetric - (splits[cascade] - band)) / max(band, 0.001f),
                                          0.0f, 1.0f)
                                  : 0.0f;
    float litSum = 0.0f;
    float weightSum = 0.0f;
    for (int p = 0; p < 4; ++p)
    {
        const int layer = cascade + p;
        if (layer >= cascadeCount)
            break;
        const float weight =
            p == 0 ? 1.0f - blendWeight : (weightSum <= 0.0f ? 1.0f : (p == 1 ? blendWeight : 0.0f));
        if (weight <= 0.0f)
            continue;

        const int matrixSlot = PoseidonPSSlotCascadeViewProjection + layer * 4;
        const float4x4 lightViewProjection =
            float4x4(constants.slots[matrixSlot], constants.slots[matrixSlot + 1],
                     constants.slots[matrixSlot + 2], constants.slots[matrixSlot + 3]);
        const float4 projected = lightViewProjection * float4(input.worldRelative, 1.0f);
        const float3 shadowCoord = projected.xyz / projected.w;
        // GL's shadow target and texture coordinates are bottom-left. Metal's
        // render targets and texture coordinates are top-left, so only Y is inverted.
        const float2 uv = float2(shadowCoord.x * 0.5f + 0.5f, 0.5f - shadowCoord.y * 0.5f);
        if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f ||
            shadowCoord.z <= 0.0f || shadowCoord.z >= 1.0f)
            continue;

        const float bias = cascadeControl.z * float((layer + 1) * (layer + 1));
        float lit = 0.0f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                lit += shadowMap.sample_compare(
                    shadowSampler, uv + float2(float(x), float(y)) * shadowControl.w,
                    uint(layer), shadowCoord.z - bias);
        litSum += weight * (lit / 9.0f);
        weightSum += weight;
    }
    if (weightSum <= 0.0f)
        return 1.0f;

    const float lit = litSum / weightSum;
    const float lastSplit = splits[cascadeCount - 1];
    const float fade = clamp((lastSplit - eyeDepth) / max(cascadeControl.y, 0.001f), 0.0f, 1.0f);
    const float strength = (1.0f - lit) * fade * clamp(input.fogTC, 0.0f, 1.0f);
    return mix(1.0f, shadowControl.z, strength);
}

float4 FinishWorldColor(float4 color, RasterVertex input, constant PSConstantsPod& constants)
{
    const float4 fogColor = constants.slots[PoseidonPSSlotFogColor];
    const float4 alphaRef = constants.slots[PoseidonPSSlotAlphaRef];
    const float4 rgbEyeCoefficient = constants.slots[PoseidonPSSlotNightEye];
    if (alphaRef.z > 0.5f)
    {
        const float coverage =
            clamp((color.a - alphaRef.x) / max(fwidth(color.a), 1.0e-4f) + 0.5f, 0.0f, 1.0f);
        if (coverage <= 0.0f)
            discard_fragment();
        color.a = coverage;
    }
    else if (color.a - alphaRef.x * alphaRef.y < 0.0f)
    {
        discard_fragment();
    }
    const float luminance = clamp(dot(color.rgb, rgbEyeCoefficient.rgb), 0.0f, 1.0f);
    const float nightBlend = clamp(luminance + rgbEyeCoefficient.a, 0.0f, 1.0f);
    color.rgb = mix(float3(luminance), color.rgb, nightBlend);
    color.rgb = mix(fogColor.rgb, color.rgb, input.fogTC);
    return alphaRef.w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

float4 FinishGrassColor(float4 color, RasterVertex input, constant PSConstantsPod& constants)
{
    const float4 alphaRef = constants.slots[PoseidonPSSlotAlphaRef];
    if (alphaRef.z > 0.5f)
    {
        const float coverage =
            clamp((color.a - alphaRef.x) / max(fwidth(color.a), 1.0e-4f) + 0.5f, 0.0f, 1.0f);
        if (coverage <= 0.0f)
            discard_fragment();
        color.a = coverage;
    }
    else if (color.a - alphaRef.x * alphaRef.y < 0.0f)
    {
        discard_fragment();
    }
    color.rgb = mix(constants.slots[PoseidonPSSlotFogColor].rgb, color.rgb, input.fogTC);
    return alphaRef.w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

fragment float4 psNormal(RasterVertex input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         depth2d_array<float> shadowMap [[texture(2)]],
                         sampler tex0Sampler [[sampler(0)]],
                         sampler shadowSampler [[sampler(2)]])
{
    const float4 constColor = constants.slots[PoseidonPSSlotConstantColor];
    float4 color = input.color * tex0.sample(tex0Sampler, input.uv0);
    color *= constColor;
    color.rgb += input.specular.rgb;
    color.rgb *= CascadeShadowFactor(input, constants, shadowMap, shadowSampler);
    return FinishWorldColor(color, input, constants);
}

fragment float4 psDetail(RasterVertex input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         texture2d<float> tex1 [[texture(1)]],
                         depth2d_array<float> shadowMap [[texture(2)]],
                         sampler sampler0 [[sampler(0)]],
                         sampler sampler1 [[sampler(1)]],
                         sampler shadowSampler [[sampler(2)]])
{
    float4 color = input.color * tex0.sample(sampler0, input.uv0);
    color *= constants.slots[PoseidonPSSlotConstantColor];
    color.rgb *= tex1.sample(sampler1, input.uv1).a * 2.0f;
    color.rgb += input.specular.rgb;
    color.rgb *= CascadeShadowFactor(input, constants, shadowMap, shadowSampler);
    return FinishWorldColor(color, input, constants);
}

fragment float4 psGrass(RasterVertex input [[stage_in]],
                        constant PSConstantsPod& constants [[buffer(1)]],
                        texture2d<float> tex0 [[texture(0)]],
                        texture2d<float> tex1 [[texture(1)]],
                        depth2d_array<float> shadowMap [[texture(2)]],
                        sampler sampler0 [[sampler(0)]],
                        sampler sampler1 [[sampler(1)]],
                        sampler shadowSampler [[sampler(2)]])
{
    if (input.fogTC < 0.0f)
        discard_fragment();
    const float4 base = tex0.sample(sampler0, input.uv0);
    const float4 grass = tex1.sample(sampler1, input.uv1);
    float4 color;
    color.rgb = clamp(input.color.rgb * base.rgb * grass.rgb * 2.0f, 0.0f, 1.0f);
    color.rgb *= CascadeShadowFactor(input, constants, shadowMap, shadowSampler);
    color.a = clamp(constants.slots[PoseidonPSSlotGrassCoefficient2].a *
                        clamp((constants.slots[PoseidonPSSlotGrassCoefficient1].a * 2.0f - 1.0f) + grass.a,
                              0.0f, 1.0f) *
                        2.0f,
                    0.0f, 1.0f);
    return FinishGrassColor(color, input, constants);
}

fragment float4 psWater(RasterVertex input [[stage_in]],
                        constant PSConstantsPod& constants [[buffer(1)]],
                        texture2d<float> tex0 [[texture(0)]],
                        texture2d<float> tex1 [[texture(1)]],
                        sampler sampler0 [[sampler(0)]],
                        sampler sampler1 [[sampler(1)]])
{
    const float4 base = tex0.sample(sampler0, input.uv0);
    const float3 bumpNormal = -(tex1.sample(sampler1, input.uv1).xyz * 2.0f - 1.0f);
    const float specular =
        clamp(dot(constants.slots[PoseidonPSSlotLightDirection].xyz, bumpNormal), 0.0f, 1.0f);
    float4 color = input.color * base;
    color.rgb += specular;
    color.rgb = mix(constants.slots[PoseidonPSSlotFogColor].rgb, color.rgb, input.fogTC);
    return constants.slots[PoseidonPSSlotAlphaRef].w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

fragment float4 psFlat(RasterVertex input [[stage_in]])
{
    return input.color;
}

fragment float4 psShadow(RasterVertex input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         sampler tex0Sampler [[sampler(0)]])
{
    const float4 alphaRef = constants.slots[PoseidonPSSlotAlphaRef];
    const float alpha = input.color.a * tex0.sample(tex0Sampler, input.uv0).a;
    if (alpha - alphaRef.x * alphaRef.y < 0.0f)
        discard_fragment();
    // Fragment discard deliberately suppresses the Shadow state's stencil
    // INCR, preserving cutout gaps in the live per-poly darken path.
    return float4(0.0f, 0.0f, 0.0f, alpha);
}

struct ShadowDepthVertexOut
{
    float4 position [[position]];
    float2 uv;
};

struct ShadowDepthSolidIn
{
    float3 position [[attribute(0)]];
};

struct ShadowDepthAlphaIn
{
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

vertex ShadowDepthVertexOut vsShadowDepthSolid(ShadowDepthSolidIn input [[stage_in]],
                                                constant float4x4& lightViewProjection [[buffer(0)]])
{
    ShadowDepthVertexOut output;
    output.position = lightViewProjection * float4(input.position, 1.0f);
    output.uv = float2(0.0f);
    return output;
}

vertex ShadowDepthVertexOut vsShadowDepthAlpha(ShadowDepthAlphaIn input [[stage_in]],
                                                constant float4x4& lightViewProjection [[buffer(0)]])
{
    ShadowDepthVertexOut output;
    output.position = lightViewProjection * float4(input.position, 1.0f);
    output.uv = input.uv;
    return output;
}

fragment void psShadowDepthSolid()
{
}

fragment void psShadowDepthAlpha(ShadowDepthVertexOut input [[stage_in]],
                                 texture2d<float> casterTexture [[texture(0)]],
                                 sampler casterSampler [[sampler(0)]])
{
    if (casterTexture.sample(casterSampler, input.uv).a < 0.5f)
        discard_fragment();
}

struct BlitVertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex BlitVertexOut vsBlitScale(uint vertexId [[vertex_id]])
{
    float2 position = float2(-1.0f, -1.0f);
    float2 uv = float2(0.0f, 1.0f);
    if (vertexId == 1)
    {
        position = float2(3.0f, -1.0f);
        uv = float2(2.0f, 1.0f);
    }
    else if (vertexId == 2)
    {
        position = float2(-1.0f, 3.0f);
        uv = float2(0.0f, -1.0f);
    }
    BlitVertexOut output;
    output.position = float4(position, 1.0f, 1.0f);
    output.uv = uv;
    return output;
}

fragment float4 psBlitScale(BlitVertexOut input [[stage_in]],
                            texture2d<float> source [[texture(0)]],
                            sampler sourceSampler [[sampler(0)]],
                            constant float4& tint [[buffer(0)]])
{
    return source.sample(sourceSampler, input.uv) * tint;
}
