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

float4 FinishWorldColor(float4 color, RasterVertex input, constant PSConstantsPod& constants)
{
    const float4 fogColor = constants.slots[PoseidonPSSlotFogColor];
    const float4 alphaRef = constants.slots[PoseidonPSSlotAlphaRef];
    const float4 rgbEyeCoefficient = constants.slots[PoseidonPSSlotNightEye];
    if (color.a - alphaRef.x * alphaRef.y < 0.0f)
        discard_fragment();
    const float luminance = clamp(dot(color.rgb, rgbEyeCoefficient.rgb), 0.0f, 1.0f);
    const float nightBlend = clamp(luminance + rgbEyeCoefficient.a, 0.0f, 1.0f);
    color.rgb = mix(float3(luminance), color.rgb, nightBlend);
    color.rgb = mix(fogColor.rgb, color.rgb, input.fogTC);
    return alphaRef.w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

float4 FinishGrassColor(float4 color, RasterVertex input, constant PSConstantsPod& constants)
{
    const float4 alphaRef = constants.slots[PoseidonPSSlotAlphaRef];
    if (color.a - alphaRef.x * alphaRef.y < 0.0f)
        discard_fragment();
    color.rgb = mix(constants.slots[PoseidonPSSlotFogColor].rgb, color.rgb, input.fogTC);
    return alphaRef.w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

fragment float4 psNormal(RasterVertex input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         sampler tex0Sampler [[sampler(0)]])
{
    const float4 constColor = constants.slots[PoseidonPSSlotConstantColor];
    float4 color = input.color * tex0.sample(tex0Sampler, input.uv0);
    color *= constColor;
    color.rgb += input.specular.rgb;
    return FinishWorldColor(color, input, constants);
}

fragment float4 psDetail(RasterVertex input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         texture2d<float> tex1 [[texture(1)]],
                         sampler sampler0 [[sampler(0)]],
                         sampler sampler1 [[sampler(1)]])
{
    float4 color = input.color * tex0.sample(sampler0, input.uv0);
    color *= constants.slots[PoseidonPSSlotConstantColor];
    color.rgb *= tex1.sample(sampler1, input.uv1).a * 2.0f;
    color.rgb += input.specular.rgb;
    return FinishWorldColor(color, input, constants);
}

fragment float4 psGrass(RasterVertex input [[stage_in]],
                        constant PSConstantsPod& constants [[buffer(1)]],
                        texture2d<float> tex0 [[texture(0)]],
                        texture2d<float> tex1 [[texture(1)]],
                        sampler sampler0 [[sampler(0)]],
                        sampler sampler1 [[sampler(1)]])
{
    if (input.fogTC < 0.0f)
        discard_fragment();
    const float4 base = tex0.sample(sampler0, input.uv0);
    const float4 grass = tex1.sample(sampler1, input.uv1);
    float4 color;
    color.rgb = clamp(input.color.rgb * base.rgb * grass.rgb * 2.0f, 0.0f, 1.0f);
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
