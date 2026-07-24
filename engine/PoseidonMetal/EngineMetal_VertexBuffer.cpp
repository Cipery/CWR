#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/Rendering/Frame/Frame.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <vector>

namespace Poseidon
{
namespace
{
struct WorldVertex
{
    Vector3P pos;
    Vector3P norm;
    UVPair uv;
};
static_assert(offsetof(WorldVertex, norm) == sizeof(Vector3P), "world normal layout drift");
static_assert(offsetof(WorldVertex, uv) == sizeof(Vector3P) * 2, "world UV layout drift");
static_assert(sizeof(WorldVertex) == sizeof(Vector3P) * 2 + sizeof(UVPair), "world vertex stride drift");
static_assert(sizeof(VertexIndex) == sizeof(std::uint16_t), "Metal world index type must stay 16-bit");

struct SectionRange
{
    int begin = 0;
    int end = 0;
    int beginVertex = 0;
    int endVertex = 0;
};
} // namespace

class VertexBufferMetal final : public VertexBuffer
{
  public:
    explicit VertexBufferMetal(EngineMetal& engine) : _engine(&engine) {}
    ~VertexBufferMetal() override
    {
        if (_engine)
            _engine->ReleaseMesh(_handle, _resource);
    }

    bool Init(const Shape& source, VBType type)
    {
        if (source.NVertex() <= 0)
            return false;
        _dynamic = type == VBDynamic || type == VBSmallDiscardable;
        return Rebuild(source);
    }

    void Update(const Shape& source, bool dynamic) override
    {
        if ((_dynamic || dynamic || bufferDirty) && Rebuild(source))
            bufferDirty = false;
    }

  private:
    bool Rebuild(const Shape& source)
    {
        std::vector<WorldVertex> vertices(static_cast<std::size_t>(source.NVertex()));
        for (int i = 0; i < source.NVertex(); ++i)
        {
            const Vector3& pos = source.Pos(i);
            const Vector3& norm = source.Norm(i);
            vertices[static_cast<std::size_t>(i)] = {
                Vector3P(pos.X(), pos.Y(), pos.Z()), Vector3P(-norm.X(), -norm.Y(), -norm.Z()), source.UV(i)};
        }

        int indexCount = 0;
        for (Offset offset = source.BeginFaces(); offset < source.EndFaces(); source.NextFace(offset))
            indexCount += (source.Face(offset).N() - 2) * 3;
        std::vector<VertexIndex> indices(static_cast<std::size_t>(std::max(indexCount, 0)));
        VertexIndex* write = indices.data();
        for (Offset offset = source.BeginFaces(); offset < source.EndFaces(); source.NextFace(offset))
        {
            const Poly& polygon = source.Face(offset);
            for (int i = 2; i < polygon.N(); ++i)
            {
                *write++ = polygon.GetVertex(0);
                *write++ = polygon.GetVertex(i - 1);
                *write++ = polygon.GetVertex(i);
            }
        }

        std::vector<SectionRange> sections(static_cast<std::size_t>(source.NSections()));
        int firstIndex = 0;
        for (int i = 0; i < source.NSections(); ++i)
        {
            const ShapeSection& section = source.GetSection(i);
            int count = 0;
            int minVertex = INT_MAX;
            int maxVertex = 0;
            for (Offset offset = section.beg; offset < section.end; source.NextFace(offset))
            {
                const Poly& polygon = source.Face(offset);
                count += (polygon.N() - 2) * 3;
                for (int vertex = 0; vertex < polygon.N(); ++vertex)
                {
                    saturateMin(minVertex, static_cast<int>(polygon.GetVertex(vertex)));
                    saturateMax(maxVertex, static_cast<int>(polygon.GetVertex(vertex)));
                }
            }
            sections[static_cast<std::size_t>(i)] = {
                firstIndex, firstIndex + count, minVertex == INT_MAX ? 0 : minVertex, maxVertex + 1};
            firstIndex += count;
        }

        EngineMetal::MeshResource replacement;
        if (!_engine->UploadStaticMesh(replacement, vertices.data(), vertices.size() * sizeof(WorldVertex),
                                       indices.data(), indices.size() * sizeof(VertexIndex)))
            return false;

        if (_handle)
            _engine->ReleaseMesh(0, _resource);
        _resource = replacement;
        _sections = std::move(sections);
        if (!_handle)
            _handle = _engine->_meshRegistry.Allocate(&_resource);
        return _handle != 0;
    }

    friend class EngineMetal;
    EngineMetal* _engine = nullptr;
    EngineMetal::MeshResource _resource;
    std::vector<SectionRange> _sections;
    std::uint32_t _handle = 0;
    bool _dynamic = false;
};

bool EngineMetal::UploadStaticMesh(MeshResource& resource, const void* vertices, std::size_t vertexBytes,
                                   const void* indices, std::size_t indexBytes)
{
    if (!_metal.device || !_metal.commandQueue || !vertices || vertexBytes == 0 || !indices || indexBytes == 0)
        return false;

    MTL::Buffer* vertexBuffer = _metal.device->newBuffer(vertexBytes, MTL::ResourceStorageModePrivate);
    MTL::Buffer* indexBuffer = _metal.device->newBuffer(indexBytes, MTL::ResourceStorageModePrivate);
    MTL::Buffer* staging =
        _metal.device->newBuffer(vertexBytes + indexBytes, MTL::ResourceStorageModeShared);
    if (!vertexBuffer || !indexBuffer || !staging)
    {
        if (vertexBuffer)
            vertexBuffer->release();
        if (indexBuffer)
            indexBuffer->release();
        if (staging)
            staging->release();
        RecordDiagnostic("failed to allocate static world mesh buffers");
        return false;
    }
    std::memcpy(staging->contents(), vertices, vertexBytes);
    std::memcpy(static_cast<std::uint8_t*>(staging->contents()) + vertexBytes, indices, indexBytes);

    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    MTL::BlitCommandEncoder* blit = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
    if (!commandBuffer || !blit)
    {
        vertexBuffer->release();
        indexBuffer->release();
        staging->release();
        RecordDiagnostic("failed to create static world mesh upload encoder");
        return false;
    }
    blit->copyFromBuffer(staging, 0, vertexBuffer, 0, vertexBytes);
    blit->copyFromBuffer(staging, vertexBytes, indexBuffer, 0, indexBytes);
    blit->endEncoding();
    commandBuffer->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });
    AttachDiagnostics(commandBuffer);
    commandBuffer->commit();

    resource.vertices = vertexBuffer;
    resource.indices = indexBuffer;
    resource.vertexBytes = vertexBytes;
    resource.indexBytes = indexBytes;
    resource.owned = true;
    return true;
}

void EngineMetal::ReleaseMesh(std::uint32_t handle, MeshResource& resource)
{
    if (handle)
        _meshRegistry.Release(handle);
    if (resource.owned)
    {
        if (resource.vertices)
            resource.vertices->release();
        if (resource.indices)
            resource.indices->release();
    }
    resource = {};
}

VertexBuffer* EngineMetal::CreateVertexBuffer(const Shape& source, VBType type)
{
    auto* buffer = new VertexBufferMetal(*this);
    if (buffer->Init(source, type))
        return buffer;
    delete buffer;
    return nullptr;
}

void EngineMetal::BeginMeshTL(const Shape& mesh, int, bool dynamic)
{
    if (mesh.GetVertexBuffer())
        mesh.GetVertexBuffer()->Update(mesh, dynamic);
}

void EngineMetal::EndMeshTL(const Shape&)
{
}

void EngineMetal::DrawSectionTL(const Shape& mesh, int begin, int end)
{
    if (_skipCurrentWorldDraw)
        return;
    auto* buffer = static_cast<VertexBufferMetal*>(mesh.GetVertexBuffer());
    if (!buffer || begin < 0 || end <= begin || end > static_cast<int>(buffer->_sections.size()))
        return;

    const SectionRange& first = buffer->_sections[static_cast<std::size_t>(begin)];
    const SectionRange& last = buffer->_sections[static_cast<std::size_t>(end - 1)];
    const int indexCount = last.end - first.begin;
    if (indexCount <= 0)
        return;

    _currentDrawItem.isTLDraw = true;
    _currentDrawItem.sectionBegin = begin;
    _currentDrawItem.sectionEnd = end;
    _currentDrawItem.firstIndex = first.begin;
    _currentDrawItem.indexCount = indexCount;
    _currentDrawItem.vertexBuffer = buffer;
    _currentDrawItem.backendMeshHandle = buffer->_handle;
    _currentDrawItem.passId = SpecToPassId(_currentDrawItem.specFlags);

    render::frame::Draw draw;
    draw.world = _currentDrawItem.worldMatrix;
    draw.mesh.vao = buffer->_handle;
    draw.indexBegin = first.begin;
    draw.indexCount = indexCount;
    draw.textures[0].id = _currentDrawItem.backendTextureHandle;
    draw.textures[1].id = _currentDrawItem.backendTexture1Handle;
    draw.descriptor = _currentDescriptor;
    EmitDraw(draw);
}
} // namespace Poseidon
