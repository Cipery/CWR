#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Graphics/Core/FanDecompose.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Frame/Frame.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <cstring>
#include <iterator>
#include <utility>

namespace Poseidon
{
namespace
{
constexpr std::size_t kMaxQueuedVertices = 32 * 1024;

int FractionalAlpha(float alpha)
{
    int value = toInt(alpha);
    saturate(value, 0, 255);
    return value;
}
} // namespace

void EngineMetal::QueueVertices(const TLVertex* vertices, int count)
{
    if (!vertices || count <= 0)
        return;
    if (static_cast<std::size_t>(count) > kMaxQueuedVertices)
    {
        LOG_ERROR(Graphics, "Metal: soup upload needs {} vertices, limit is {}", count, kMaxQueuedVertices);
        return;
    }
    if (_queuedVertices.size() + static_cast<std::size_t>(count) > kMaxQueuedVertices)
        FlushQueueBatch();
    _meshBase = static_cast<int>(_queuedVertices.size());
    _queuedVertices.insert(_queuedVertices.end(), vertices, vertices + count);
    // Per-primitive 2D vertices have not been referenced by queue indices yet
    // and may be peeled across an ordering flush. A BeginMesh block is shared
    // by all of its texture queues, so it is never such a trailing primitive.
    _currentPrimitiveBase = _mesh ? static_cast<int>(_queuedVertices.size()) : _meshBase;
}

EngineMetal::TriQueue& EngineMetal::QueueFor(TextureMetal* texture, int level, int spec)
{
    const bool alpha = (texture && texture->IsAlpha()) || !_enableReorder;
    for (std::size_t i = 0; i < _triQueues.size(); ++i)
    {
        TriQueue& queue = _triQueues[i];
        if (queue.texture == texture && queue.special == spec)
        {
            saturateMin(queue.level, level);
            _activeQueue = static_cast<int>(i);
            return queue;
        }
    }

    auto flushBeforeCurrentVertices = [&]()
    {
        std::vector<TLVertex> pending;
        if (_currentPrimitiveBase >= 0 && _currentPrimitiveBase < static_cast<int>(_queuedVertices.size()))
        {
            pending.assign(_queuedVertices.begin() + _currentPrimitiveBase, _queuedVertices.end());
            _queuedVertices.resize(_currentPrimitiveBase);
        }

        // A mesh-soup block may already be referenced by several texture
        // queues. Flush it with those queues, then establish a fresh copy for
        // the queue that follows the ordering boundary.
        TLVertexTable* activeMesh = _mesh;
        FlushQueueBatch();
        if (activeMesh)
        {
            _queuedVertices.assign(activeMesh->VertexData(), activeMesh->VertexData() + activeMesh->NVertex());
            _meshBase = 0;
            _currentPrimitiveBase = static_cast<int>(_queuedVertices.size());
        }
        else
        {
            _queuedVertices = std::move(pending);
            _meshBase = 0;
            _currentPrimitiveBase = 0;
        }
    };
    if (alpha && !_triQueues.empty())
        flushBeforeCurrentVertices();

    if (_triQueues.size() >= 32)
        flushBeforeCurrentVertices();
    TriQueue queue;
    queue.texture = texture;
    queue.level = level;
    queue.special = spec;
    queue.passId = SpecToPassId(spec);
    _triQueues.push_back(std::move(queue));
    _activeQueue = static_cast<int>(_triQueues.size() - 1);
    return _triQueues.back();
}

void EngineMetal::QueueFan(const VertexIndex* indices, int count)
{
    if (_activeQueue < 0 || !indices)
        return;
    const int indexCount = render::geom::FanTriangleIndexCount(count);
    if (indexCount <= 0)
        return;
    if (_instCount > 1)
        _instImpure = true;
    TriQueue& queue = _triQueues[_activeQueue];
    const std::size_t begin = queue.indices.size();
    queue.indices.resize(begin + indexCount);
    render::geom::FanToTriangles(indices, count, _meshBase, queue.indices.data() + begin);
}

void EngineMetal::Queue2DPoly(int count)
{
    if (_activeQueue < 0 || count < 3)
        return;
    if (_instCount > 1)
        _instImpure = true;
    TriQueue& queue = _triQueues[_activeQueue];
    for (int i = 2; i < count; ++i)
    {
        queue.indices.push_back(static_cast<std::uint16_t>(_meshBase));
        queue.indices.push_back(static_cast<std::uint16_t>(_meshBase + i - 1));
        queue.indices.push_back(static_cast<std::uint16_t>(_meshBase + i));
    }
}

void EngineMetal::FlushQueueBatch()
{
    if (_triQueues.empty())
        return;
    if (_queuedVertices.empty())
    {
        RecordDiagnostic("queue batch has queue entries but no vertex block");
        return;
    }
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return;

    const std::size_t vertexBytes = _queuedVertices.size() * sizeof(TLVertex);
    FrameRing::Allocation vertices = _frameRing.Allocate(vertexBytes, alignof(TLVertex));
    if (!vertices)
    {
        RecordDiagnostic("failed to allocate frame-ring vertices for a queue batch");
        return;
    }
    std::memcpy(vertices.contents, _queuedVertices.data(), vertexBytes);

    auto emitQueue = [&](TriQueue& queue)
    {
        if (queue.indices.empty())
            return;
        const std::size_t indexBytes = queue.indices.size() * sizeof(std::uint16_t);
        FrameRing::Allocation indices = _frameRing.Allocate(indexBytes, alignof(std::uint16_t));
        if (!indices)
        {
            RecordDiagnostic("failed to allocate frame-ring indices for a queue batch");
            return;
        }
        std::memcpy(indices.contents, queue.indices.data(), indexBytes);

        render::BuildContext context;
        context.isIn3DPass = false;
        context.shadowAlphaRef = static_cast<std::uint8_t>((_shadowFactor * 7) >> 4);
        const render::LegacySpec spec = render::SplitLegacy(queue.special);
        const render::RenderPassDescriptor descriptor = render::BuildRenderPassDescriptor(spec, context);
        const Metal::FragmentStage fragment = descriptor.shader == render::ShaderFamily::Shadow
                                                  ? Metal::FragmentStage::Shadow
                                                  : Metal::FragmentStage::Normal;
        if (!ApplyScreenState(descriptor, fragment))
            return;
        encoder->setVertexBuffer(vertices.buffer, vertices.offset, 30);

        MTL::Texture* texture = queue.texture ? _textureRegistry.Resolve(queue.texture->GetHandle()) : nullptr;
        _stickyFragmentTextures[0] = texture ? texture : _fallbackWhite[0];
        encoder->setFragmentTexture(_stickyFragmentTextures[0], 0);
        encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, queue.indices.size(), MTL::IndexTypeUInt16,
                                       indices.buffer, indices.offset);
        ++Poseidon::gPerfDrawCalls;

        auto mesh = std::make_unique<MeshResource>();
        mesh->vertices = vertices.buffer;
        mesh->indices = indices.buffer;
        mesh->vertexOffset = vertices.offset;
        mesh->indexOffset = indices.offset;
        const std::uint32_t meshHandle = _meshRegistry.Allocate(mesh.get());
        _frameMeshHandles.push_back(meshHandle);
        _frameMeshes.push_back(std::move(mesh));

        DrawItem item = {};
        item.specFlags = spec;
        item.passId = queue.passId;
        item.texture = queue.texture;
        item.textureLevel = queue.level;
        item.firstIndex = 0;
        item.indexCount = static_cast<int>(queue.indices.size());
        item.backendMeshHandle = meshHandle;
        item.backendTextureHandle = queue.texture ? queue.texture->GetHandle() : 0;
        item.isTLDraw = false;
        _drawItems.push_back(item);
    };

    if (_enableReorder)
    {
        // Match GL33's queue partition: opaque queues occupy the regular
        // batching slots and the alpha queue is always submitted last.
        for (TriQueue& queue : _triQueues)
            if (!queue.texture || !queue.texture->IsAlpha())
                emitQueue(queue);
        for (TriQueue& queue : _triQueues)
            if (queue.texture && queue.texture->IsAlpha())
                emitQueue(queue);
    }
    else
    {
        for (TriQueue& queue : _triQueues)
            emitQueue(queue);
    }

    _triQueues.clear();
    _queuedVertices.clear();
    _meshBase = 0;
    _currentPrimitiveBase = 0;
    _activeQueue = -1;
}

void EngineMetal::PrepareTriangle(const MipInfo& mip, int spec)
{
    if (_in3DPass)
    {
        EndWorldViewport();
        _in3DPass = false;
        _activePassId = static_cast<int>(PassId::ScreenSpace);
    }
    TextureMetal* texture = static_cast<TextureMetal*>(mip._texture);
    QueueFor(texture, mip._level, spec);
}

void EngineMetal::DrawDecal(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color,
                            const MipInfo& mip, int spec)
{
    if (sizeX <= 0.0f || sizeY <= 0.0f)
        return;

    float xBegin = screen.X() - sizeX;
    float xEnd = screen.X() + sizeX;
    float yBegin = screen.Y() - sizeY;
    float yEnd = screen.Y() + sizeY;
    float uBegin = 0.0f;
    float uEnd = 1.0f;
    float vBegin = 0.0f;
    float vEnd = 1.0f;

    if (xBegin < 0.0f)
    {
        uBegin = -xBegin / (2.0f * sizeX);
        xBegin = 0.0f;
    }
    if (xEnd > _w)
    {
        uEnd = 1.0f - (xEnd - _w) / (2.0f * sizeX);
        xEnd = static_cast<float>(_w);
    }
    if (yBegin < 0.0f)
    {
        vBegin = -yBegin / (2.0f * sizeY);
        yBegin = 0.0f;
    }
    if (yEnd > _h)
    {
        vEnd = 1.0f - (yEnd - _h) / (2.0f * sizeY);
        yEnd = static_cast<float>(_h);
    }
    if (xBegin >= xEnd || yBegin >= yEnd)
        return;

    TLVertex vertices[4] = {};
    vertices[0].pos = Vector3P(xBegin, yBegin, screen.Z());
    vertices[1].pos = Vector3P(xEnd, yBegin, screen.Z());
    vertices[2].pos = Vector3P(xEnd, yEnd, screen.Z());
    vertices[3].pos = Vector3P(xBegin, yEnd, screen.Z());
    vertices[0].t0 = {uBegin, vBegin};
    vertices[1].t0 = {uEnd, vBegin};
    vertices[2].t0 = {uEnd, vEnd};
    vertices[3].t0 = {uBegin, vEnd};

    const PackedColor vertexColor =
        (spec & IsAlphaFog) ? color : PackedColor(color | 0xff000000);
    const PackedColor vertexSpecular =
        (spec & IsAlphaFog) ? PackedColor(0xff000000)
                            : PackedColor(0xff000000 - (color & 0xff000000));
    for (TLVertex& vertex : vertices)
    {
        vertex.rhw = rhw;
        vertex.color = vertexColor;
        vertex.specular = vertexSpecular;
    }

    // Vertices MUST be queued before PrepareTriangle selects/flushes the
    // texture queue (GL33 oracle: AddVertices -> QueuePrepareTriangle,
    // EngineGL33_DrawShared.cpp). Reversed order lets a texture change peel
    // the PREVIOUS decal's already-indexed vertices as "trailing unindexed"
    // — visible as torn/missing smoke cloudlet quads (animated smoke cycles
    // textures every few frames).
    QueueVertices(vertices, 4);
    PrepareTriangle(mip, spec);
    static const VertexIndex indices[4] = {0, 1, 2, 3};
    QueueFan(indices, 4);
}

void EngineMetal::DrawPolygon(const VertexIndex* indices, int count)
{
    QueueFan(indices, count);
}

void EngineMetal::DrawSection(const FaceArray& face, Offset begin, Offset end)
{
    for (Offset offset = begin; offset < end; face.Next(offset))
    {
        const Poly& polygon = face[offset];
        QueueFan(polygon.GetVertexList(), polygon.N());
    }
}

void EngineMetal::DrawPoints(int begin, int end)
{
    if (!_mesh || _activeQueue < 0)
        return;

    for (int i = begin; i < end; ++i)
    {
        if (_mesh->Clip(i) & ClipAll)
            continue;

        const TLVertex& point = _mesh->GetVertex(i);
        const PackedColor color = point.color;
        if (color.A8() < 8)
            continue;

        const int x = toIntFloor(point.pos[0]);
        const int y = toIntFloor(point.pos[1]);
        const float xFraction = point.pos[0] - x;
        const float yFraction = point.pos[1] - y;
        const float inverseX = 1.0f - xFraction;
        const float inverseY = 1.0f - yFraction;
        const float alpha = color.A8();

        TLVertex vertices[4] = {};
        vertices[0] = point;
        vertices[0].pos[0] = x + 0.5f;
        vertices[0].pos[1] = y + 0.5f;
        vertices[0].color = PackedColorRGB(color, FractionalAlpha(inverseX * inverseY * alpha));
        vertices[0].specular = PackedColor(0xff000000);
        vertices[1] = vertices[0];
        vertices[1].pos[0] = x + 2.5f;
        vertices[1].color = PackedColorRGB(color, FractionalAlpha(xFraction * inverseY * alpha));
        vertices[2] = vertices[1];
        vertices[2].pos[1] = y + 2.5f;
        vertices[2].color = PackedColorRGB(color, FractionalAlpha(xFraction * yFraction * alpha));
        vertices[3] = vertices[2];
        vertices[3].pos[0] = vertices[0].pos[0];
        vertices[3].color = PackedColorRGB(color, FractionalAlpha(inverseX * yFraction * alpha));

        // QueueVertices flushes the 32K soup on overflow, and that flush
        // deliberately invalidates _activeQueue. Snapshot its identity only
        // for that path so this point can establish the continuation queue.
        const bool queueWillOverflow = _queuedVertices.size() + std::size(vertices) > kMaxQueuedVertices;
        TextureMetal* texture = nullptr;
        int level = 0;
        int special = 0;
        if (queueWillOverflow)
        {
            const TriQueue& queue = _triQueues[static_cast<std::size_t>(_activeQueue)];
            texture = queue.texture;
            level = queue.level;
            special = queue.special;
        }
        QueueVertices(vertices, 4);
        if (queueWillOverflow && _activeQueue < 0)
            QueueFor(texture, level, special);
        static const VertexIndex indices[4] = {0, 1, 2, 3};
        QueueFan(indices, 4);
    }
}

void EngineMetal::PrepareMesh(const render::LegacySpec&)
{
    if (_in3DPass)
    {
        EndWorldViewport();
        _in3DPass = false;
        _activePassId = static_cast<int>(PassId::ScreenSpace);
    }
}

void EngineMetal::BeginMesh(TLVertexTable& mesh, const render::LegacySpec&)
{
    _mesh = &mesh;
    QueueVertices(mesh.VertexData(), mesh.NVertex());
}

void EngineMetal::EndMesh(TLVertexTable&)
{
    _mesh = nullptr;
}

void EngineMetal::EnableReorderQueues(bool enable)
{
    if (_enableReorder == enable)
        return;
    _enableReorder = enable;
    if (!enable)
        FlushQueues();
}

void EngineMetal::FlushQueues()
{
    FlushQueueBatch();
}

void EngineMetal::BeginShadowPass()
{
    // The live projected-shadow path is per-poly; both brackets are ordering
    // barriers only. Shadow depth/stencil and blend are descriptor-owned.
    FlushQueues();
}

void EngineMetal::EndShadowPass()
{
    FlushQueues();
}

void EngineMetal::EmitDraw(const render::frame::Draw& draw)
{
    if (draw.mesh.vao == 0 || draw.indexCount <= 0)
        return;
    MeshResource* mesh = _meshRegistry.Resolve(draw.mesh.vao);
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!mesh || !encoder)
        return;

    if ((!_currentPipelineWorld || !_currentPipeline || _currentDescriptor != draw.descriptor) &&
        !ApplyWorldState(draw.descriptor))
        return;
    if (!SnapshotConstants() || !BindWorldSlot(draw.world))
        return;
    encoder->setVertexBuffer(mesh->vertices, mesh->vertexOffset, 29);
    MTL::Texture* texture0 = _textureRegistry.Resolve(draw.textures[0].id);
    MTL::Texture* texture1 = _textureRegistry.Resolve(draw.textures[1].id);
    _stickyFragmentTextures[0] = texture0 ? texture0 : _fallbackWhite[0];
    _stickyFragmentTextures[1] = texture1 ? texture1 : _fallbackWhite[1];
    encoder->setFragmentTexture(_stickyFragmentTextures[0], 0);
    encoder->setFragmentTexture(_stickyFragmentTextures[1], 1);
    const NS::UInteger instanceCount = static_cast<NS::UInteger>(_instCount > 1 ? _instCount : 1);
    encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, draw.indexCount, MTL::IndexTypeUInt16, mesh->indices,
                                   mesh->indexOffset + draw.indexBegin * sizeof(std::uint16_t), instanceCount);
    ++Poseidon::gPerfDrawCalls;

    DrawItem item = _currentDrawItem;
    item.firstIndex = draw.indexBegin;
    item.indexCount = draw.indexCount;
    item.backendMeshHandle = draw.mesh.vao;
    item.backendTextureHandle = draw.textures[0].id;
    item.backendTexture1Handle = draw.textures[1].id;
    item.isTLDraw = true;
    _drawItems.push_back(item);
}
} // namespace Poseidon
