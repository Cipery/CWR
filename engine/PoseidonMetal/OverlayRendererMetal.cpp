#include <PoseidonMetal/Private/MetalCppFirst.hpp>
#include <imgui.h>

#ifdef DebugLog
#undef DebugLog
#endif

#include <PoseidonMetal/OverlayRendererMetal.hpp>

#include <Poseidon/Dev/Debug/OverlayScissor.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <PoseidonMetal/EngineMetal.hpp>

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

#include <cstddef>
#include <cstring>
#include <limits>

static_assert(sizeof(ImDrawVert) == sizeof(Poseidon::Metal::OverlayVertex), "ImDrawVert layout drift");
static_assert(offsetof(ImDrawVert, pos) == 0, "ImDrawVert::pos moved");
static_assert(offsetof(ImDrawVert, uv) == 8, "ImDrawVert::uv moved");
static_assert(offsetof(ImDrawVert, col) == 16, "ImDrawVert::col moved");
static_assert(sizeof(ImDrawIdx) == 2, "backend assumes 16-bit ImDrawIdx");

namespace Poseidon::Metal
{
namespace
{
const PipelineKey kOverlayKey =
    PipelineKey::Make(VertexStage::ImGui, FragmentStage::ImGui, PipelineBlend::AlphaBlend,
                      /*colorWriteMask*/ 0xf, VertexLayout::ImGui, AttachmentConfig::PresentPass,
                      /*sampleCountLog2*/ 0, /*alphaToCoverage*/ false);

class ScopedAutoreleasePool
{
  public:
    ScopedAutoreleasePool() : _pool(NS::AutoreleasePool::alloc()->init()) {}
    ~ScopedAutoreleasePool()
    {
        if (_pool)
            _pool->drain();
    }

    explicit operator bool() const { return _pool != nullptr; }

  private:
    NS::AutoreleasePool* _pool = nullptr;
};

bool ByteCount(int count, std::size_t stride, std::size_t& result)
{
    if (count < 0 || static_cast<std::size_t>(count) >
                         std::numeric_limits<std::size_t>::max() / stride)
        return false;
    result = static_cast<std::size_t>(count) * stride;
    return true;
}

class ScopedCallbackExecution
{
  public:
    explicit ScopedCallbackExecution(bool& active) : _active(active) { _active = true; }
    ~ScopedCallbackExecution() { _active = false; }

  private:
    bool& _active;
};
} // namespace

OverlayRendererMetal::OverlayRendererMetal(EngineMetal& engine) : _engine(engine) {}

OverlayRendererMetal::~OverlayRendererMetal()
{
    Shutdown();
}

bool OverlayRendererMetal::Init(SDL_Window* window)
{
    if (_platformInitialized || _pipeline)
    {
        _engine.RecordDiagnostic("imgui overlay renderer was initialized more than once");
        return false;
    }
    if (!window)
    {
        _engine.RecordDiagnostic("imgui overlay renderer received a null SDL window");
        return false;
    }

    _pipeline = _engine.ResolvePipeline(kOverlayKey, /*logMiss*/ false);
    if (!_pipeline)
    {
        _engine.RecordDiagnostic("failed to prebuild the imgui overlay PSO");
        return false;
    }
    LOG_INFO(Graphics, "Metal: imgui overlay PSO prebuilt");

    if (!_bufferRing.Initialize(_engine._metal.device))
    {
        _engine.RecordDiagnostic("failed to initialize the imgui overlay buffer ring");
        _pipeline = nullptr;
        return false;
    }

    if (!ImGui_ImplSDL3_InitForMetal(window))
    {
        _engine.RecordDiagnostic("ImGui SDL3 Metal platform initialization failed");
        _bufferRing.Shutdown();
        _pipeline = nullptr;
        return false;
    }
    _platformInitialized = true;

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "PoseidonMetal";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.DrawCallback_ResetRenderState = ResetRenderStateSentinel;
    _rendererStateInstalled = true;
    return true;
}

void OverlayRendererMetal::Shutdown()
{
    if (!PrepareShutdown())
        return;

    if (ImGui::GetCurrentContext())
    {
        if (_rendererStateInstalled && (_platformInitialized || _liveTextureHandles != 0))
            ShutdownTextures();

        if (_rendererStateInstalled)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
            if (io.BackendRendererName && std::strcmp(io.BackendRendererName, "PoseidonMetal") == 0)
                io.BackendRendererName = nullptr;

            ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
            if (platformIO.DrawCallback_ResetRenderState == ResetRenderStateSentinel)
                platformIO.DrawCallback_ResetRenderState = nullptr;
        }
    }
    else if (_liveTextureHandles != 0)
    {
        _engine.RecordDiagnostic("imgui Metal shutdown cannot release live textures without an ImGui context");
    }
    if (_platformInitialized)
        ImGui_ImplSDL3_Shutdown();

    _bufferRing.Shutdown();
    _platformInitialized = false;
    _rendererStateInstalled = false;
    _pipeline = nullptr;
}

bool OverlayRendererMetal::PrepareShutdown()
{
    if (!_executingUserCallback)
        return true;
    RecordDrawDiagnosticOnce(1ull << 15, "imgui Metal renderer shutdown is forbidden during a user callback");
    return false;
}

void OverlayRendererMetal::RecordDrawDiagnosticOnce(std::uint64_t bit, const char* message)
{
    if ((_drawDiagnosticMask & bit) != 0)
        return;
    _drawDiagnosticMask |= bit;
    _engine.RecordDiagnostic(message);
}

void OverlayRendererMetal::CorrectDisplayMetrics()
{
    if (SDL_GetWindowFlags(_engine._sdlWindow) & SDL_WINDOW_MINIMIZED)
        return;

    int logicalWidth = 0;
    int logicalHeight = 0;
    SDL_GetWindowSize(_engine._sdlWindow, &logicalWidth, &logicalHeight);
    if (logicalWidth <= 0 || logicalHeight <= 0)
    {
        RecordDrawDiagnosticOnce(1ull << 0, "overlay received non-positive non-minimized window metrics");
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(logicalWidth), static_cast<float>(logicalHeight));
    io.DisplayFramebufferScale =
        ImVec2(static_cast<float>(_engine._w) / static_cast<float>(logicalWidth),
               static_cast<float>(_engine._h) / static_cast<float>(logicalHeight));
}

void OverlayRendererMetal::ResetRenderStateSentinel(const ImDrawList*, const ImDrawCmd*) {}

void OverlayRendererMetal::RenderDrawData(ImDrawData* drawData)
{
    ScopedAutoreleasePool pool;
    if (!pool)
    {
        RecordDrawDiagnosticOnce(1ull << 1, "failed to allocate the imgui overlay autorelease pool");
        return;
    }
    if (!drawData)
    {
        RecordDrawDiagnosticOnce(1ull << 2, "imgui Metal draw received null draw data");
        return;
    }

    const int framebufferWidth =
        static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int framebufferHeight =
        static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0)
    {
        LOG_DEBUG(Graphics, "Metal: skipping imgui draw for a zero-sized framebuffer");
        return;
    }
    if (drawData->CmdLists.Size == 0)
    {
        LOG_DEBUG(Graphics, "Metal: skipping imgui draw with no command lists");
        return;
    }

    // Texture servicing deliberately follows the zero-list early-out. A panel
    // that has never been opened therefore performs no overlay GPU work.
    if (!UpdateTextures(drawData))
    {
        RecordDrawDiagnosticOnce(1ull << 3, "imgui Metal texture request list is unavailable; skipping the draw pass");
        return;
    }
    if (!_pipeline || !_engine._metal.commandQueue || !_engine._samplers[3] ||
        !_engine._fallbackWhite[0])
    {
        RecordDrawDiagnosticOnce(1ull << 4, "imgui Metal draw pass is missing required engine resources");
        return;
    }

    std::size_t vertexBytes = 0;
    std::size_t indexBytes = 0;
    if (!ByteCount(drawData->TotalVtxCount, sizeof(ImDrawVert), vertexBytes) ||
        !ByteCount(drawData->TotalIdxCount, sizeof(ImDrawIdx), indexBytes))
    {
        RecordDrawDiagnosticOnce(1ull << 5, "imgui Metal draw buffer byte count overflowed");
        return;
    }

    OverlayBufferRing::Lease lease = _bufferRing.Acquire(vertexBytes, indexBytes);
    if (!lease)
    {
        // Pre-acquisition rejection consumes nothing; a growth failure returns
        // its acquired permit and restores the previous slot rotation.
        RecordDrawDiagnosticOnce(1ull << 6, "failed to acquire or grow the imgui overlay buffer-ring slot");
        return;
    }
    const OverlayBufferRing::Slot& slot = lease.Get();
    if (!slot.vertices || !slot.indices || !slot.vertices->contents() || !slot.indices->contents())
    {
        RecordDrawDiagnosticOnce(1ull << 7, "imgui overlay buffer-ring slot has invalid CPU mappings");
        return; // Lease cancellation returns the permit and rewinds rotation.
    }

    auto* vertexDestination = static_cast<std::uint8_t*>(slot.vertices->contents());
    auto* indexDestination = static_cast<std::uint8_t*>(slot.indices->contents());
    std::size_t vertexCursor = 0;
    std::size_t indexCursor = 0;
    for (const ImDrawList* commandList : drawData->CmdLists)
    {
        if (!commandList)
        {
            RecordDrawDiagnosticOnce(1ull << 8, "imgui Metal draw data contains a null command list");
            return;
        }
        const std::size_t listVertexBytes =
            static_cast<std::size_t>(commandList->VtxBuffer.Size) * sizeof(ImDrawVert);
        const std::size_t listIndexBytes =
            static_cast<std::size_t>(commandList->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vertexCursor > vertexBytes || listVertexBytes > vertexBytes - vertexCursor ||
            indexCursor > indexBytes || listIndexBytes > indexBytes - indexCursor)
        {
            RecordDrawDiagnosticOnce(1ull << 9, "imgui Metal command-list totals exceed the acquired buffers");
            return;
        }
        if (listVertexBytes)
            std::memcpy(vertexDestination + vertexCursor, commandList->VtxBuffer.Data,
                        listVertexBytes);
        if (listIndexBytes)
            std::memcpy(indexDestination + indexCursor, commandList->IdxBuffer.Data,
                        listIndexBytes);
        vertexCursor += listVertexBytes;
        indexCursor += listIndexBytes;
    }
    if (vertexCursor != vertexBytes || indexCursor != indexBytes)
    {
        RecordDrawDiagnosticOnce(1ull << 10, "imgui Metal aggregate draw counts do not match command-list data");
        return;
    }

    MTL::CommandBuffer* commandBuffer = _engine._metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        RecordDrawDiagnosticOnce(1ull << 11, "failed to create the imgui overlay command buffer");
        return; // The lease destructor cancels the acquisition.
    }

    MTL::Texture* target = _engine.EncodeOverlayCaptureResolve(commandBuffer);
    if (!target)
    {
        // EncodeOverlayCaptureResolve ends any encoder it created. The
        // autorelease pool discards this uncommitted command buffer, including
        // any partially encoded resolve/copy work. Presentation falls back to
        // the ordinary scene capture, and the resolve path diagnoses once.
        return;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!pass)
    {
        RecordDrawDiagnosticOnce(1ull << 13, "failed to create the imgui overlay render-pass descriptor");
        _engine.DiscardOverlayCaptureResolve();
        // The resolve/copy remains uncommitted and is discarded with the CB.
        return;
    }
    MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
    color->setTexture(target);
    color->setLoadAction(MTL::LoadActionLoad);
    color->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
    if (!encoder)
    {
        RecordDrawDiagnosticOnce(1ull << 14, "failed to create the imgui overlay render encoder");
        _engine.DiscardOverlayCaptureResolve();
        // No live encoder remains; discard the uncommitted resolve/copy CB.
        return;
    }

    const float left = drawData->DisplayPos.x;
    const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float top = drawData->DisplayPos.y;
    const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
    const float projection[16] = {
        2.0f / (right - left), 0.0f,                 0.0f, 0.0f,
        0.0f,                  2.0f / (top - bottom), 0.0f, 0.0f,
        0.0f,                  0.0f,                 1.0f, 0.0f,
        (right + left) / (left - right), (top + bottom) / (bottom - top), 0.0f, 1.0f,
    };

    const auto setupRenderState = [&]() {
        encoder->setRenderPipelineState(_pipeline);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(target->width()),
                                           static_cast<double>(target->height()), 0, 1});
        encoder->setFragmentSamplerState(_engine._samplers[3], 0);
        encoder->setVertexBytes(projection, sizeof(projection), 0);
        encoder->setVertexBuffer(slot.vertices, 0, 30);
    };
    setupRenderState();

    const Dev::OverlayDisplayMetrics overlayMetrics{drawData->DisplayPos.x,
                                                    drawData->DisplayPos.y,
                                                    drawData->FramebufferScale.x,
                                                    drawData->FramebufferScale.y,
                                                    static_cast<std::size_t>(target->width()),
                                                    static_cast<std::size_t>(target->height())};
    std::size_t globalVertexOffset = 0;
    std::size_t globalIndexOffset = 0;
    for (const ImDrawList* commandList : drawData->CmdLists)
    {
        for (const ImDrawCmd& command : commandList->CmdBuffer)
        {
            if (command.UserCallback)
            {
                if (command.UserCallback == ResetRenderStateSentinel)
                    setupRenderState();
                else
                {
                    // The callback runs synchronously while the local lease
                    // owns one ring permit. Shutdown detects this guard and
                    // refuses to wait on or invalidate that external lease.
                    ScopedCallbackExecution callbackExecution(_executingUserCallback);
                    command.UserCallback(commandList, &command);
                }
                continue;
            }
            if (command.ElemCount == 0)
                continue;

            Dev::OverlayScissor scissor{};
            if (!Dev::ComputeOverlayScissor(
                    {command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w}, overlayMetrics,
                    scissor))
                continue;
            encoder->setScissorRect(
                MTL::ScissorRect{static_cast<NS::UInteger>(scissor.x), static_cast<NS::UInteger>(scissor.y),
                                 static_cast<NS::UInteger>(scissor.width), static_cast<NS::UInteger>(scissor.height)});

            MTL::Texture* texture = ResolveTexture(static_cast<std::uint64_t>(command.GetTexID()));
            encoder->setFragmentTexture(texture ? texture : _engine._fallbackWhite[0], 0);
            encoder->setVertexBufferOffset(
                globalVertexOffset +
                    static_cast<std::size_t>(command.VtxOffset) * sizeof(ImDrawVert),
                30);
            encoder->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle, command.ElemCount, MTL::IndexTypeUInt16,
                slot.indices,
                globalIndexOffset +
                    static_cast<std::size_t>(command.IdxOffset) * sizeof(ImDrawIdx));
        }
        globalVertexOffset +=
            static_cast<std::size_t>(commandList->VtxBuffer.Size) * sizeof(ImDrawVert);
        globalIndexOffset +=
            static_cast<std::size_t>(commandList->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }

    encoder->endEncoding();
    lease.ReleaseOnCompletion(commandBuffer);
    _engine.AttachDiagnostics(commandBuffer);
    commandBuffer->commit();
    _engine.MarkOverlayCaptureCommitted();
}
} // namespace Poseidon::Metal
