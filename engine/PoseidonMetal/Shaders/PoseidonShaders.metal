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

struct ScreenVertexOut
{
    float4 position [[position]];
    float4 color;
    float4 specular;
    float2 uv0;
    float2 uv1;
    float fogTC;
    float3 worldRelative;
};

vertex ScreenVertexOut vsScreen(ScreenVertexIn input [[stage_in]],
                                constant VSConstantsPod& constants [[buffer(0)]])
{
    const float4 viewportScale = constants.slots[21];
    const float w = 1.0f / input.rhw;
    ScreenVertexOut output;
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

fragment float4 psNormal(ScreenVertexOut input [[stage_in]],
                         constant PSConstantsPod& constants [[buffer(1)]],
                         texture2d<float> tex0 [[texture(0)]],
                         sampler tex0Sampler [[sampler(0)]])
{
    const float4 fogColor = constants.slots[0];
    const float4 alphaRef = constants.slots[1];
    const float4 constColor = constants.slots[3];
    const float4 rgbEyeCoef = constants.slots[7];

    float4 color = input.color * tex0.sample(tex0Sampler, input.uv0);
    color *= constColor;
    color.rgb += input.specular.rgb;
    if (color.a - alphaRef.x * alphaRef.y < 0.0f)
        discard_fragment();

    const float luminance = clamp(dot(color.rgb, rgbEyeCoef.rgb), 0.0f, 1.0f);
    const float nightBlend = clamp(luminance + rgbEyeCoef.a, 0.0f, 1.0f);
    color.rgb = mix(float3(luminance), color.rgb, nightBlend);
    color.rgb = mix(fogColor.rgb, color.rgb, input.fogTC);
    return alphaRef.w > 0.5f ? float4(1.0f, 0.0f, 0.0f, 1.0f) : color;
}

fragment float4 psFlat(ScreenVertexOut input [[stage_in]])
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
    output.position = float4(position, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

fragment float4 psBlitScale(BlitVertexOut input [[stage_in]],
                            texture2d<float> source [[texture(0)]],
                            sampler sourceSampler [[sampler(0)]])
{
    return source.sample(sourceSampler, input.uv);
}
