#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/CopyLayout.hpp>
#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/Shared/ScreenshotWriter.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <cstring>

namespace Poseidon
{
namespace
{
class ScopedAutoreleasePool
{
  public:
    explicit ScopedAutoreleasePool(bool enabled) : _pool(enabled ? NS::AutoreleasePool::alloc()->init() : nullptr) {}

    ~ScopedAutoreleasePool()
    {
        if (_pool)
            _pool->drain();
    }

    ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
    ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

  private:
    NS::AutoreleasePool* _pool = nullptr;
};

void DecodeBGRA8(const std::uint8_t* source, std::uint8_t* destination, bool includeAlpha)
{
    destination[0] = source[2];
    destination[1] = source[1];
    destination[2] = source[0];
    if (includeAlpha)
        destination[3] = source[3];
}
} // namespace

bool EngineMetal::SubmitSynchronousReadback(MTL::CommandBuffer* readback)
{
    if (!readback)
        return false;

    MTL::CommandBuffer* continuation = nullptr;
    if (_frameOpen)
    {
        FlushQueues();
        for (const TriQueue& queue : _triQueues)
        {
            if (!queue.indices.empty())
            {
                RecordDiagnostic("cannot submit a mid-frame readback with an unencoded queue batch");
                return false;
            }
        }
        EndFrameEncoder();
        if (!_frameCommandBuffer)
        {
            RecordDiagnostic("mid-frame readback has no active frame command buffer");
            return false;
        }

        // Reserve the replacement before committing anything, so a command
        // buffer allocation failure leaves the current span recoverable.
        continuation = _frameRing.CreateContinuation(_metal.commandQueue);
        if (!continuation)
        {
            RecordDiagnostic("failed to create a continuation command buffer for mid-frame readback");
            return false;
        }

        if (!_loggedMidFrameReadback)
        {
            LOG_WARN(Graphics, "Metal: using synchronous mid-frame readback continuation");
            _loggedMidFrameReadback = true;
        }

        MTL::CommandBuffer* current = _frameCommandBuffer;
        AttachDiagnostics(current);
        current->commit();
    }

    // The shared command queue executes committed buffers in submission
    // order: prior frame span, readback, and finally the continuation when
    // FinishDraw ends the span.
    AttachDiagnostics(readback);
    readback->commit();
    readback->waitUntilCompleted();
    const bool success = readback->status() != MTL::CommandBufferStatusError;
    if (_frameOpen)
        _frameCommandBuffer = continuation;
    return success;
}

MTL::Texture* EngineMetal::EncodeCaptureResolve(MTL::CommandBuffer* commandBuffer)
{
    if (!_frameColor)
        return nullptr;
    if (static_cast<int>(_frameColor->width()) == _w && static_cast<int>(_frameColor->height()) == _h)
        return _frameColor;
    if (!_captureColor || static_cast<int>(_captureColor->width()) != _w ||
        static_cast<int>(_captureColor->height()) != _h)
    {
        RecordDiagnostic("non-unit render scale has no window-sized capture target");
        return nullptr;
    }
    if (_captureResolvedThisFrame && !_frameOpen)
        return _captureColor;
    if (!commandBuffer)
        return nullptr;

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!pass)
    {
        RecordDiagnostic("failed to create the capture-resolve render-pass descriptor");
        return nullptr;
    }
    MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
    color->setTexture(_captureColor);
    color->setLoadAction(MTL::LoadActionDontCare);
    color->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
    if (!encoder)
    {
        RecordDiagnostic("failed to create the capture-resolve render encoder");
        return nullptr;
    }
    const Metal::PipelineKey key = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, 0xf,
        Metal::VertexLayout::None, Metal::AttachmentConfig::PresentPass, 0, false);
    MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true);
    if (!pipeline)
    {
        encoder->endEncoding();
        return nullptr;
    }
    encoder->setRenderPipelineState(pipeline);
    encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(_w), static_cast<double>(_h), 0, 1});
    encoder->setScissorRect(MTL::ScissorRect{0, 0, static_cast<NS::UInteger>(_w), static_cast<NS::UInteger>(_h)});
    encoder->setFragmentTexture(_frameColor, 0);
    encoder->setFragmentSamplerState(_samplers[3], 0);
    const float tint[4] = {1, 1, 1, 1};
    encoder->setFragmentBytes(tint, sizeof(tint), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    encoder->endEncoding();
    return _captureColor;
}

bool EngineMetal::ReadCapture(std::vector<std::uint8_t>& bgra, int& width, int& height)
{
    if (!_frameColor || !_metal.commandQueue)
        return false;

    width = _w;
    height = _h;
    if (width <= 0 || height <= 0)
        return false;
    const Metal::CopyLayout layout = Metal::CopyLayout::Compute(Metal::CopyFormat::BGRA8, width, height);
    MTL::Buffer* staging = _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    if (!staging)
    {
        RecordDiagnostic("failed to allocate the full-frame readback staging buffer");
        return false;
    }
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        RecordDiagnostic("failed to create the full-frame readback command buffer");
        staging->release();
        return false;
    }
    MTL::Texture* captureSource = EncodeCaptureResolve(commandBuffer);
    if (!captureSource)
    {
        RecordDiagnostic("failed to prepare the full-frame capture source");
        staging->release();
        return false;
    }
    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        RecordDiagnostic("failed to create the full-frame readback blit encoder");
        staging->release();
        return false;
    }
    blit->copyFromTexture(captureSource, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width, height, 1), staging,
                          0, layout.bytesPerRow, layout.bytesPerImage);
    blit->endEncoding();
    if (!SubmitSynchronousReadback(commandBuffer))
    {
        staging->release();
        return false;
    }
    if (_captureColor && !_frameOpen)
        _captureResolvedThisFrame = true;

    bgra.resize(static_cast<std::size_t>(width) * height * 4);
    const std::uint8_t* source = static_cast<const std::uint8_t*>(staging->contents());
    for (int row = 0; row < height; ++row)
        std::memcpy(bgra.data() + static_cast<std::size_t>(row) * width * 4,
                    source + static_cast<std::size_t>(row) * layout.bytesPerRow, layout.tightBytesPerRow);
    staging->release();
    return true;
}

bool EngineMetal::ReadCapturePixel(int x, int y, std::uint8_t rgba[4])
{
    if (!rgba || !_frameColor || !_metal.commandQueue || x < 0 || y < 0 || x >= _w || y >= _h)
        return false;

    const Metal::CopyLayout layout = Metal::CopyLayout::Compute(Metal::CopyFormat::BGRA8, 1, 1);
    MTL::Buffer* staging = _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    if (!staging)
    {
        RecordDiagnostic("failed to allocate the pixel readback staging buffer");
        return false;
    }
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        RecordDiagnostic("failed to create the pixel readback command buffer");
        staging->release();
        return false;
    }
    MTL::Texture* captureSource = EncodeCaptureResolve(commandBuffer);
    if (!captureSource)
    {
        RecordDiagnostic("failed to prepare the pixel capture source");
        staging->release();
        return false;
    }
    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        RecordDiagnostic("failed to create the pixel readback blit encoder");
        staging->release();
        return false;
    }
    blit->copyFromTexture(captureSource, 0, 0, MTL::Origin::Make(x, y, 0), MTL::Size::Make(1, 1, 1), staging, 0,
                          layout.bytesPerRow, layout.bytesPerImage);
    blit->endEncoding();
    const bool success = SubmitSynchronousReadback(commandBuffer);
    if (success)
    {
        if (_captureColor && !_frameOpen)
            _captureResolvedThisFrame = true;
        const std::uint8_t* bgra = static_cast<const std::uint8_t*>(staging->contents());
        DecodeBGRA8(bgra, rgba, true);
    }
    staging->release();
    return success;
}

void EngineMetal::Screenshot(RString filename)
{
    _pendingScreenshotPath = filename;
}

void EngineMetal::FlushPendingScreenshot()
{
    CaptureScreenshotIfPending();
}

void EngineMetal::CaptureScreenshotIfPending()
{
    if (_pendingScreenshotPath.GetLength() == 0)
        return;
    const RString path = _pendingScreenshotPath;
    _pendingScreenshotPath = "";

    std::vector<std::uint8_t> bgra;
    int width = 0, height = 0;
    if (!ReadCapture(bgra, width, height))
        return;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y)
    {
        const std::uint8_t* source = bgra.data() + static_cast<std::size_t>(y) * width * 4;
        std::uint8_t* destination = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x)
            DecodeBGRA8(source + x * 4, destination + x * 3, false);
    }
    ScreenshotWriter::WriteRGB(path, width, height, rgb.data());
}

int EngineMetal::SampleBackBufferNonBlack()
{
    ScopedAutoreleasePool pool(!_frameOpen);
    std::vector<std::uint8_t> bgra;
    int width = 0, height = 0;
    if (!ReadCapture(bgra, width, height))
        return -1;
    int nonBlack = 0;
    for (int sampleY = 0; sampleY < 16; ++sampleY)
    {
        const int y = height * sampleY / 16;
        // GL33's grid uses bottom-left glReadPixels rows; the CPU capture is top-left.
        const int sourceRow = height - 1 - y;
        for (int sampleX = 0; sampleX < 16; ++sampleX)
        {
            const int x = width * sampleX / 16;
            const std::uint8_t* source = bgra.data() + (static_cast<std::size_t>(sourceRow) * width + x) * 4;
            std::uint8_t pixel[4] = {};
            DecodeBGRA8(source, pixel, true);
            if (pixel[0] > 2 || pixel[1] > 2 || pixel[2] > 2)
                ++nonBlack;
        }
    }
    return nonBlack;
}

bool EngineMetal::SamplePixel(int x, int y, std::uint8_t* outRGB)
{
    ScopedAutoreleasePool pool(!_frameOpen);
    if (!outRGB)
        return false;
    std::uint8_t rgba[4] = {};
    if (!ReadCapturePixel(x, y, rgba))
        return false;
    outRGB[0] = rgba[0];
    outRGB[1] = rgba[1];
    outRGB[2] = rgba[2];
    return true;
}
} // namespace Poseidon
