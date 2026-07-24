#include <metal_stdlib>
#include "PoseidonShaderTypes.h"

using namespace metal;

struct M0VertexOut
{
    float4 position [[position]];
};

vertex M0VertexOut m0Vertex(uint vertexId [[vertex_id]])
{
    float2 position = float2(-1.0, -1.0);
    if (vertexId == 1)
        position = float2(3.0, -1.0);
    else if (vertexId == 2)
        position = float2(-1.0, 3.0);

    M0VertexOut out;
    out.position = float4(position, 0.0, 1.0);
    return out;
}

fragment float4 m0Fragment()
{
    return float4(0.04, 0.10, 0.22, 1.0);
}
