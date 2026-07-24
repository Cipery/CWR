#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/Dummy/TextBankDummy.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <SDL3/SDL_filesystem.h>

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
} // namespace

EngineMetal::EngineMetal(int width, int height, bool windowed, int bpp)
    : _w(width), _h(height), _pixelSize(bpp), _windowedRestoreW(width), _windowedRestoreH(height), _windowed(windowed),
      _clearColor(std::make_unique<MTL::ClearColor>(0.04, 0.10, 0.22, 1.0))
{
    LOG_INFO(Graphics, "Metal: Initializing M0 backend — bootstrap {}x{} {}bpp {}", _w, _h, _pixelSize,
             _windowed ? "windowed" : "fullscreen");

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    _textBank = new TextBankDummy();

    _metal.device = MTL::CreateSystemDefaultDevice();
    if (!_metal.device)
    {
        LOG_ERROR(Graphics, "Metal: MTL::CreateSystemDefaultDevice failed");
        pool->drain();
        return;
    }

    _metal.commandQueue = _metal.device->newCommandQueue();
    if (!_metal.commandQueue)
    {
        LOG_ERROR(Graphics, "Metal: newCommandQueue failed");
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
        LOG_ERROR(Graphics, "Metal: failed to allocate the three frame-ring arenas");
        pool->drain();
        return;
    }
    if (!LoadShaderLibrary())
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
        pool->drain();
    }

    if (_initialized)
        SaveConfig();

    ClearFontCache();
    delete _textBank;
    _textBank = nullptr;

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
    _frameCommandBuffer = _frameRing.BeginSpan(_metal.commandQueue);
    if (!_frameCommandBuffer)
    {
        LOG_ERROR(Graphics, "Metal: failed to begin a frame-ring command-buffer span");
        _framePool->drain();
        _framePool = nullptr;
        return;
    }

    base::InitDraw(clear, color);
    if (clear)
        Clear(true, true, color);
    _frameOpen = true;
}

void EngineMetal::FinishDraw()
{
    if (!_frameOpen)
        return;

    AttachDiagnostics(_frameCommandBuffer);
    _frameRing.EndSpan(_frameCommandBuffer);
    _frameCommandBuffer = nullptr;
    _frameOpen = false;
    base::FinishDraw();

    _framePool->drain();
    _framePool = nullptr;
}

void EngineMetal::NextFrame()
{
    if (!IsAbleToDraw())
        return;

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    CA::MetalDrawable* drawable = (_w > 0 && _h > 0) ? _metal.layer->nextDrawable() : nullptr;
    if (drawable)
    {
        MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
        if (commandBuffer)
        {
            MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            MTL::RenderPassColorAttachmentDescriptor* colorAttachment = pass->colorAttachments()->object(0);
            colorAttachment->setTexture(drawable->texture());
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(*_clearColor);

            MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
            if (encoder)
            {
                encoder->endEncoding();
                commandBuffer->presentDrawable(drawable);
                AttachDiagnostics(commandBuffer);
                commandBuffer->commit();
            }
        }
    }

    // Resize only at the post-present frame boundary. This also runs when
    // drawable acquisition fails (for example while occluded or minimized).
    ApplyPendingResize();
    base::NextFrame();
    pool->drain();
}

void EngineMetal::Clear(bool, bool clear, PackedColor color)
{
    if (!clear)
        return;

    const double r = ((color >> 16) & 0xFF) / 255.0;
    const double g = ((color >> 8) & 0xFF) / 255.0;
    const double b = (color & 0xFF) / 255.0;
    *_clearColor = MTL::ClearColor::Make(r, g, b, 1.0);
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
            LOG_ERROR(Graphics, "Metal: failed to load {}: {}", metallibPath, ErrorText(error));
            return false;
        }
    }

    const std::string sourcePath = RuntimeFilePath("PoseidonShaders.metal");
    std::string source;
    if (!ReadTextFile(sourcePath, source))
    {
        LOG_ERROR(Graphics, "Metal: neither PoseidonShaders.metallib nor staged PoseidonShaders.metal exists next to "
                            "the executable");
        return false;
    }

    NS::Error* error = nullptr;
    NS::String* sourceString = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
    MTL::CompileOptions* compileOptions = MTL::CompileOptions::alloc()->init();
    if (!compileOptions)
    {
        LOG_ERROR(Graphics, "Metal: failed to allocate runtime shader compile options");
        return false;
    }
    compileOptions->setLanguageVersion(MTL::LanguageVersion3_2);
    _metal.shaderLibrary = _metal.device->newLibrary(sourceString, compileOptions, &error);
    compileOptions->release();
    if (!_metal.shaderLibrary)
    {
        LOG_ERROR(Graphics, "Metal: runtime compilation of {} failed: {}", sourcePath, ErrorText(error));
        return false;
    }

    LOG_WARN(Graphics, "Metal: runtime-compiled pre-expanded shader source from {}", sourcePath);
    return true;
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
                const std::string message = ErrorText(completed->error());
                {
                    std::lock_guard<std::mutex> lock(diagnostics->mutex);
                    ++diagnostics->errorCount;
                    diagnostics->lastMessage = message;
                }
                LOG_ERROR(Graphics, "Metal command buffer failed: {}", message);
            }
            pool->release();
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
