#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <PoseidonMetal/Shaders/PoseidonShaderTypes.h>
#include <PoseidonMetal/TextureMetal.hpp>

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Poseidon
{
namespace
{
std::string RuntimeFilePath(const char* filename)
{
    const char* basePath = SDL_GetBasePath();
    return std::string(basePath ? basePath : "") + filename;
}

bool ReadTextFile(const std::string& path, std::string& contents)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    return !contents.empty();
}

const char* ErrorText(NS::Error* error)
{
    if (!error)
        return "unknown Metal error";
    NS::String* description = error->localizedDescription();
    return description ? description->utf8String() : "unknown Metal error";
}

void StoreDiagnostic(const std::shared_ptr<MetalDiagnostics>& diagnostics, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(diagnostics->mutex);
        ++diagnostics->errorCount;
        diagnostics->lastMessage = message;
    }
    LOG_ERROR(Graphics, "Metal: {}", message);
}
} // namespace

EngineMetal::EngineMetal(int width, int height, bool windowed, int bpp)
    : _w(width), _h(height), _pixelSize(bpp), _windowedRestoreW(width), _windowedRestoreH(height), _windowed(windowed),
      _clearColor(std::make_unique<MTL::ClearColor>(0.04, 0.10, 0.22, 1.0))
{
    LOG_INFO(Graphics, "Metal: Initializing M4 alpha/MSAA backend — {}x{} {}bpp {}", _w, _h, _pixelSize,
             _windowed ? "windowed" : "fullscreen");
    const float white[4] = {1, 1, 1, 1};
    const float eye[4] = {0.299f, 0.587f, 0.114f, 1.0f};
    std::memcpy(_psConstants.data() + PoseidonPSSlotConstantColor * 4, white, sizeof(white));
    std::memcpy(_psConstants.data() + PoseidonPSSlotNightEye * 4, eye, sizeof(eye));

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if (!pool)
    {
        RecordDiagnostic("failed to allocate the initialization autorelease pool");
        return;
    }
    _metal.device = MTL::CreateSystemDefaultDevice();
    if (!_metal.device)
    {
        RecordDiagnostic("MTL::CreateSystemDefaultDevice failed");
        pool->drain();
        return;
    }

    _metal.commandQueue = _metal.device->newCommandQueue();
    if (!_metal.commandQueue)
    {
        RecordDiagnostic("newCommandQueue failed");
        pool->drain();
        return;
    }

    if (!InitializeWindow(width, height, windowed))
    {
        pool->drain();
        return;
    }
    if (!_frameRing.Initialize(_metal.device))
    {
        RecordDiagnostic("failed to allocate the three frame-ring arenas");
        pool->drain();
        return;
    }
    if (!LoadShaderLibrary())
    {
        pool->drain();
        return;
    }
    _textBank = new TextBankMetal(this);
    if (!InitializeM1Resources())
    {
        pool->drain();
        return;
    }

    _initialized = true;
    LoadConfig();

    NS::String* deviceName = _metal.device->name();
    LOG_INFO(Graphics, "Metal: {} — drawable {}x{} — {}", deviceName ? deviceName->utf8String() : "unknown device", _w,
             _h, _windowed ? "windowed" : "fullscreen");
    pool->drain();
}

EngineMetal::~EngineMetal()
{
    LOG_INFO(Graphics, "Metal: Destroying engine");

    if (_frameOpen)
        FinishDraw();
    _frameRing.WaitIdle();

    // A same-queue fence also covers the present command buffer, which is
    // intentionally outside the frame-ring lifetime.
    if (_metal.commandQueue)
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        if (MTL::CommandBuffer* fence = _metal.commandQueue->commandBuffer())
        {
            fence->commit();
            fence->waitUntilCompleted();
        }
        if (pool)
            pool->drain();
    }

    if (_initialized)
        SaveConfig();

    ClearFontCache();
    delete _textBank;
    _textBank = nullptr;
    DestroyM1Resources();

    // Release the ring's device-created buffers deterministically while the
    // owning MTLDevice is still alive. FrameRing's destructor remains an
    // idempotent fallback for partially initialized engines.
    _frameRing.Shutdown();

    if (_metal.shaderLibrary)
    {
        _metal.shaderLibrary->release();
        _metal.shaderLibrary = nullptr;
    }
    if (_metal.commandQueue)
    {
        _metal.commandQueue->release();
        _metal.commandQueue = nullptr;
    }
    DestroyWindow();
    if (_metal.device)
    {
        _metal.device->release();
        _metal.device = nullptr;
    }
}

bool EngineMetal::IsAbleToDraw()
{
    return _initialized && _sdlWindow && _metal.layer && _metal.commandQueue;
}

void EngineMetal::InitDraw(bool clear, PackedColor color)
{
    if (_frameOpen)
    {
        LOG_DEBUG(Graphics, "Metal: InitDraw done twice");
        return;
    }
    if (!IsAbleToDraw())
        return;

    _framePool = NS::AutoreleasePool::alloc()->init();
    if (!_framePool)
    {
        RecordDiagnostic("failed to allocate the frame autorelease pool");
        return;
    }
    _frameCommandBuffer = _frameRing.BeginSpan(_metal.commandQueue);
    if (!_frameCommandBuffer)
    {
        RecordDiagnostic("failed to begin a frame-ring command-buffer span");
        _framePool->drain();
        _framePool = nullptr;
        return;
    }

    _frameOpen = true;
    for (std::uint32_t handle : _frameMeshHandles)
        _meshRegistry.Release(handle);
    _frameMeshHandles.clear();
    _frameMeshes.clear();
    _drawItems.clear();
    _queuedVertices.clear();
    _triQueues.clear();
    _mesh = nullptr;
    _meshBase = 0;
    _currentPrimitiveBase = 0;
    _activeQueue = -1;
    _in3DPass = false;
    _worldViewportActive = false;
    _skipCurrentWorldDraw = false;
    _instCount = 0;
    _instImpure = false;
    _instPending = 0;
    _runWorldBuffer = nullptr;
    _runWorldOffset = 0;
    _currentPipeline = nullptr;
    _currentPipelineWorld = false;
    _currentDepthState = nullptr;
    _captureResolvedThisFrame = false;
    _materialSetSpec = -1;
    _materialLightsSignature = 0;
    _texGenMode = render::TexGenMode::Fixed;
    _currentViewport = {0, 0, static_cast<double>(_frameColor->width()), static_cast<double>(_frameColor->height())};
    _currentScissor = {
        0, 0, static_cast<unsigned>(_frameColor->width()), static_cast<unsigned>(_frameColor->height())};
    _minGuardX = -4096;
    _maxGuardX = _w + 4096;
    _minGuardY = -4096;
    _maxGuardY = _h + 4096;
    MarkConstantsDirty();
    _frameNeedsClear = clear;
    _clearDepth = true;
    if (_textBank)
        _textBank->StartFrame();
    base::InitDraw(clear, color);
    if (clear)
        Clear(true, true, color);
}

void EngineMetal::FinishDraw()
{
    if (!_frameOpen)
        return;

    EndWorldViewport();
    _in3DPass = false;
    base::FinishDraw();
    base::DrawFinishTexts();
    FlushQueues();
    if (_frameNeedsClear)
        EnsureFrameEncoder();
    EndFrameEncoder(true);
    if (_textBank)
        _textBank->FinishFrame();
    const std::size_t worldDraws =
        static_cast<std::size_t>(std::count_if(_drawItems.begin(), _drawItems.end(),
                                              [](const DrawItem& item) { return item.isTLDraw; }));
    LOG_DEBUG(Graphics, "Metal: recorded draws={} world={} queued={}", _drawItems.size(), worldDraws,
              _drawItems.size() - worldDraws);
    AttachDiagnostics(_frameCommandBuffer);
    _frameRing.EndSpan(_frameCommandBuffer);
    _frameCommandBuffer = nullptr;
    _frameOpen = false;

    _framePool->drain();
    _framePool = nullptr;
}

void EngineMetal::NextFrame()
{
    if (!IsAbleToDraw())
        return;

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if (!pool)
    {
        RecordDiagnostic("failed to allocate the present autorelease pool");
        return;
    }

    CaptureScreenshotIfPending();

    CA::MetalDrawable* drawable = (_w > 0 && _h > 0 && _frameColor) ? _metal.layer->nextDrawable() : nullptr;
    if (drawable)
    {
        MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
        if (commandBuffer)
        {
            MTL::Texture* captureSource = EncodeCaptureResolve(commandBuffer);
            MTL::RenderPassDescriptor* pass =
                captureSource ? MTL::RenderPassDescriptor::renderPassDescriptor() : nullptr;
            if (!captureSource)
            {
                RecordDiagnostic("failed to prepare the present capture source");
            }
            else if (!pass)
            {
                RecordDiagnostic("failed to create the present render-pass descriptor");
            }
            else
            {
                MTL::RenderPassColorAttachmentDescriptor* colorAttachment = pass->colorAttachments()->object(0);
                colorAttachment->setTexture(drawable->texture());
                colorAttachment->setLoadAction(MTL::LoadActionDontCare);
                colorAttachment->setStoreAction(MTL::StoreActionStore);

                MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
                if (encoder)
                {
                    const Metal::PipelineKey key = Metal::PipelineKey::Make(
                        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque,
                        0xf, Metal::VertexLayout::None, Metal::AttachmentConfig::PresentPass, 0, false);
                    if (MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true))
                    {
                        encoder->setRenderPipelineState(pipeline);
                        encoder->setFragmentTexture(captureSource, 0);
                        encoder->setFragmentSamplerState(_samplers[3], 0);
                        const float tint[4] = {1, 1, 1, 1};
                        encoder->setFragmentBytes(tint, sizeof(tint), 0);
                        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
                    }
                    encoder->endEncoding();
                    commandBuffer->presentDrawable(drawable);
                    AttachDiagnostics(commandBuffer);
                    commandBuffer->commit();
                    if (_captureColor)
                        _captureResolvedThisFrame = true;
                }
                else
                {
                    RecordDiagnostic("failed to create the present render encoder");
                }
            }
        }
        else
        {
            RecordDiagnostic("failed to create the present command buffer");
        }
    }

    // Resize only at the post-present frame boundary. This also runs when
    // drawable acquisition fails (for example while occluded or minimized).
    ApplyPendingResize();
    base::NextFrame();
    pool->drain();
}

void EngineMetal::Clear(bool clearZ, bool clear, PackedColor color)
{
    if (_frameOpen)
    {
        const bool hasQueuedTriangles = std::any_of(_triQueues.begin(), _triQueues.end(),
                                                    [](const TriQueue& queue) { return !queue.indices.empty(); });
        if (hasQueuedTriangles)
            FlushQueues();
    }
    if (_frameOpen && _frameEncoder && clearZ && !clear)
    {
        DrawClear(true, false, *_clearColor);
        return;
    }
    if (_frameOpen && _frameEncoder && clear)
    {
        const MTL::ClearColor drawColor = MTL::ClearColor::Make(
            ((color >> 16) & 0xFF) / 255.0, ((color >> 8) & 0xFF) / 255.0, (color & 0xFF) / 255.0, 1.0);
        DrawClear(clearZ, true, drawColor);
        return;
    }
    if (clear)
    {
        const double r = ((color >> 16) & 0xFF) / 255.0;
        const double g = ((color >> 8) & 0xFF) / 255.0;
        const double b = (color & 0xFF) / 255.0;
        *_clearColor = MTL::ClearColor::Make(r, g, b, 1.0);
        _frameNeedsClear = true;
    }
    _clearDepth = _clearDepth || clearZ;
    if (_frameOpen && (clear || clearZ))
        EnsureFrameEncoder();
}

bool EngineMetal::LoadShaderLibrary()
{
    const std::string metallibPath = RuntimeFilePath("PoseidonShaders.metallib");
    {
        std::ifstream metallib(metallibPath, std::ios::binary);
        if (metallib)
        {
            NS::Error* error = nullptr;
            NS::String* path = NS::String::string(metallibPath.c_str(), NS::UTF8StringEncoding);
            _metal.shaderLibrary = _metal.device->newLibrary(path, &error);
            if (_metal.shaderLibrary)
            {
                LOG_INFO(Graphics, "Metal: loaded {}", metallibPath);
                return true;
            }
            RecordDiagnostic("failed to load " + metallibPath + ": " + ErrorText(error));
            return false;
        }
    }

    const std::string sourcePath = RuntimeFilePath("PoseidonShaders.metal");
    std::string source;
    if (!ReadTextFile(sourcePath, source))
    {
        RecordDiagnostic("neither PoseidonShaders.metallib nor staged PoseidonShaders.metal exists next to the "
                         "executable");
        return false;
    }

    NS::Error* error = nullptr;
    NS::String* sourceString = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
    MTL::CompileOptions* compileOptions = MTL::CompileOptions::alloc()->init();
    if (!compileOptions)
    {
        RecordDiagnostic("failed to allocate runtime shader compile options");
        return false;
    }
    compileOptions->setLanguageVersion(MTL::LanguageVersion3_2);
    _metal.shaderLibrary = _metal.device->newLibrary(sourceString, compileOptions, &error);
    compileOptions->release();
    if (!_metal.shaderLibrary)
    {
        RecordDiagnostic("runtime compilation of " + sourcePath + " failed: " + ErrorText(error));
        return false;
    }

    LOG_WARN(Graphics, "Metal: runtime-compiled pre-expanded shader source from {}", sourcePath);
    return true;
}

void EngineMetal::RecordDiagnostic(const std::string& message)
{
    StoreDiagnostic(_diagnostics, message);
}

void EngineMetal::AttachDiagnostics(MTL::CommandBuffer* commandBuffer)
{
    if (!commandBuffer)
        return;

    const std::shared_ptr<MetalDiagnostics> diagnostics = _diagnostics;
    commandBuffer->addCompletedHandler(
        [diagnostics](MTL::CommandBuffer* completed)
        {
            NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
            if (completed && completed->status() == MTL::CommandBufferStatusError)
            {
                StoreDiagnostic(diagnostics, std::string("command buffer failed: ") + ErrorText(completed->error()));
            }
            if (pool)
                pool->drain();
        });
}

unsigned int EngineMetal::GetDebugErrorCount() const
{
    std::lock_guard<std::mutex> lock(_diagnostics->mutex);
    return _diagnostics->errorCount;
}

std::string EngineMetal::GetLastDebugMessage() const
{
    std::lock_guard<std::mutex> lock(_diagnostics->mutex);
    return _diagnostics->lastMessage;
}

RString EngineMetal::GetDebugName() const
{
    if (_metal.device)
    {
        NS::String* name = _metal.device->name();
        if (name)
            return RString("Metal 3 — ") + RString(name->utf8String());
    }
    return "Metal 3";
}

AbstractTextBank* EngineMetal::TextBank()
{
    return _textBank;
}

void EngineMetal::ResetForRemount()
{
    FlushQueues();
    _drawItems.clear();
    _instCount = 0;
    _instImpure = false;
    _instPending = 0;
    _runWorldBuffer = nullptr;
    _runWorldOffset = 0;
    _currentPipeline = nullptr;
    _currentPipelineWorld = false;
    _currentDepthState = nullptr;
    MarkConstantsDirty();
    if (_textBank)
        _textBank->ReleaseAllTextures();
}

Engine* CreateEngineMetal(int width, int height, bool windowed, int bpp)
{
    EngineMetal* engine = new EngineMetal(width, height, windowed, bpp);
    if (!engine->IsAbleToDraw())
    {
        delete engine;
        return nullptr;
    }
    return engine;
}

} // namespace Poseidon
