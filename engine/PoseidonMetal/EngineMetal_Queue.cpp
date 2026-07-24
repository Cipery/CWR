#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Graphics/Core/FanDecompose.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Frame/Frame.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <cstring>
#include <utility>

namespace Poseidon
{
void EngineMetal::QueueVertices(const TLVertex* vertices, int count)
{
    if (!vertices || count <= 0)
        return;
    if (count > 32 * 1024)
    {
        LOG_ERROR(Graphics, "Metal: soup upload needs {} vertices, limit is {}", count, 32 * 1024);
        return;
    }
    if (_queuedVertices.size() + static_cast<std::size_t>(count) > 32 * 1024)
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
    TriQueue& queue = _triQueues[_activeQueue];
    const std::size_t begin = queue.indices.size();
    queue.indices.resize(begin + indexCount);
    render::geom::FanToTriangles(indices, count, _meshBase, queue.indices.data() + begin);
}

void EngineMetal::Queue2DPoly(int count)
{
    if (_activeQueue < 0 || count < 3)
        return;
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
        const render::LegacySpec spec = render::SplitLegacy(queue.special);
        const render::RenderPassDescriptor descriptor = render::BuildRenderPassDescriptor(spec, context);
        if (!ApplyScreenState(descriptor, Metal::FragmentStage::Normal))
            return;
        encoder->setVertexBuffer(vertices.buffer, vertices.offset, 30);

        MTL::Texture* texture = queue.texture ? _textureRegistry.Resolve(queue.texture->GetHandle()) : nullptr;
        encoder->setFragmentTexture(texture ? texture : _fallbackWhite[0], 0);
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
    if (!SnapshotConstants() || !BindWorldMatrix(draw.world))
        return;
    encoder->setVertexBuffer(mesh->vertices, mesh->vertexOffset, 29);
    MTL::Texture* texture0 = _textureRegistry.Resolve(draw.textures[0].id);
    MTL::Texture* texture1 = _textureRegistry.Resolve(draw.textures[1].id);
    encoder->setFragmentTexture(texture0 ? texture0 : _fallbackWhite[0], 0);
    encoder->setFragmentTexture(texture1 ? texture1 : _fallbackWhite[1], 1);
    encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, draw.indexCount, MTL::IndexTypeUInt16, mesh->indices,
                                   mesh->indexOffset + draw.indexBegin * sizeof(std::uint16_t), 1);
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
