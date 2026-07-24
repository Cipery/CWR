#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp>

#include <cstring>

namespace Poseidon
{
void EngineMetal::DrawTestPattern(const char* name)
{
    if (!_frameOpen || !name)
        return;
    FlushQueues();

    auto makeVertex = [](float x, float y, float z, DWORD color)
    {
        TLVertex vertex = {};
        vertex.pos = Vector3P(x, y, z);
        vertex.rhw = 1.0f;
        vertex.color = PackedColor(color);
        vertex.specular = PackedColor(0xff000000);
        return vertex;
    };

    auto drawQuad = [&](const TLVertex (&quad)[4], render::DepthMode depth)
    {
        const TLVertex triangles[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};
        FrameRing::Allocation allocation = _frameRing.Allocate(sizeof(triangles), alignof(TLVertex));
        if (!allocation)
            return;
        std::memcpy(allocation.contents, triangles, sizeof(triangles));
        render::RenderPassDescriptor descriptor;
        descriptor.depth = depth;
        descriptor.blend = render::BlendMode::Opaque;
        descriptor.cull = render::CullMode::None;
        descriptor.shader = render::ShaderFamily::Flat;
        if (!ApplyScreenState(descriptor, Metal::FragmentStage::Flat))
            return;
        MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
        if (!encoder)
            return;
        encoder->setVertexBuffer(allocation.buffer, allocation.offset, 30);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
        ++Poseidon::gPerfDrawCalls;
    };

    if (std::strcmp(name, "gradient3d") == 0)
    {
        const TLVertex quad[4] = {
            makeVertex(0, 0, 0.5f, 0xffff0000), makeVertex(_w, 0, 0.5f, 0xff00ff00),
            makeVertex(_w, _h, 0.5f, 0xffffffff), makeVertex(0, _h, 0.5f, 0xff0000ff),
        };
        drawQuad(quad, render::DepthMode::Normal);
        return;
    }

    if (std::strcmp(name, "colorbar") == 0)
    {
        const DWORD colors[5] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff};
        const float barWidth = _w / 5.0f;
        for (int i = 0; i < 5; ++i)
        {
            const float x0 = barWidth * i;
            const float x1 = barWidth * (i + 1);
            const TLVertex quad[4] = {
                makeVertex(x0, 0, 0.5f, colors[i]), makeVertex(x1, 0, 0.5f, colors[i]),
                makeVertex(x1, _h, 0.5f, colors[i]), makeVertex(x0, _h, 0.5f, colors[i]),
            };
            drawQuad(quad, render::DepthMode::Disabled);
        }
    }
}
} // namespace Poseidon
