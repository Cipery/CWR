#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/CopyLayout.hpp>
#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace Poseidon
{
namespace
{
constexpr int kMaxShadowCascades = 4;

MTL::Texture* CreateDepthArray(MTL::Device* device, int resolution, int layers)
{
    if (!device || resolution <= 0 || layers <= 0)
        return nullptr;
    MTL::TextureDescriptor* descriptor =
        MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatDepth32Float, resolution, resolution, false);
    if (!descriptor)
        return nullptr;
    descriptor->setTextureType(MTL::TextureType2DArray);
    descriptor->setArrayLength(static_cast<NS::UInteger>(layers));
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setUsage(
        static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));
    return device->newTexture(descriptor);
}
} // namespace

void EngineMetal::SetShadowMapSunFactor(float factor)
{
    _shadowSunFactor = std::max(0.0f, std::min(factor, 1.0f));
}

bool EngineMetal::EnsureShadowDepthArray(int resolution, int layers)
{
    layers = std::max(1, std::min(layers, kMaxShadowCascades));
    if (_shadowDepthArray && _shadowMapRes == resolution && _shadowMapLayers == layers)
        return true;
    MTL::Texture* replacement = CreateDepthArray(_metal.device, resolution, layers);
    if (!replacement)
    {
        RecordDiagnostic("failed to create " + std::to_string(resolution) + "x" +
                         std::to_string(resolution) + "x" + std::to_string(layers) +
                         " cascade depth array");
        return false;
    }
    if (_shadowDepthArray && _stickyFragmentTextures[2] == _shadowDepthArray)
        _stickyFragmentTextures[2] = _fallbackShadowDepth;
    if (_shadowDepthArray)
        _shadowDepthArray->release();
    _shadowDepthArray = replacement;
    _shadowMapRes = resolution;
    _shadowMapLayers = layers;
    return true;
}

void EngineMetal::RenderShadowDepthScene(const float* lightVPs, const float* splitViewDist,
                                         const float* camFwd3, int numCascades, int omniCount, int res,
                                         const ShadowCasterSet& casters)
{
    numCascades = std::max(0, std::min(numCascades, kMaxShadowCascades));
    const bool haveSolid = casters.solidXYZ && casters.solidVertexCount >= 3;
    const bool haveAlpha = casters.alphaXYZUV && casters.alphaVertexCount >= 3 &&
                           casters.alphaBatches && casters.alphaBatchCount > 0;
    if (!_frameOpen || !_frameCommandBuffer || !lightVPs || !splitViewDist || !camFwd3 ||
        res <= 0 || numCascades == 0 || (!haveSolid && !haveAlpha) ||
        !EnsureShadowDepthArray(res, numCascades))
    {
        _shadowMapActive = false;
        return;
    }

    FrameRing::Allocation solid;
    FrameRing::Allocation alpha;
    if (haveSolid)
    {
        const std::size_t bytes = static_cast<std::size_t>(casters.solidVertexCount) * 3 * sizeof(float);
        solid = _frameRing.Allocate(bytes, alignof(float));
        if (!solid)
        {
            RecordDiagnostic("failed to allocate cascade solid-caster vertices");
            _shadowMapActive = false;
            return;
        }
        std::memcpy(solid.contents, casters.solidXYZ, bytes);
    }
    if (haveAlpha)
    {
        const std::size_t bytes = static_cast<std::size_t>(casters.alphaVertexCount) * 5 * sizeof(float);
        alpha = _frameRing.Allocate(bytes, alignof(float));
        if (!alpha)
        {
            RecordDiagnostic("failed to allocate cascade alpha-caster vertices");
            _shadowMapActive = false;
            return;
        }
        std::memcpy(alpha.contents, casters.alphaXYZUV, bytes);
    }

    struct ResolvedBatch
    {
        MTL::Texture* texture = nullptr;
        int firstVertex = 0;
        int vertexCount = 0;
    };
    std::vector<ResolvedBatch> batches;
    if (haveAlpha)
        batches.reserve(static_cast<std::size_t>(casters.alphaBatchCount));
    for (int i = 0; haveAlpha && i < casters.alphaBatchCount; ++i)
    {
        const ShadowCasterBatch& source = casters.alphaBatches[i];
        if (source.firstVertex < 0 || source.firstVertex >= casters.alphaVertexCount ||
            source.vertexCount < 3)
            continue;
        TextureMetal* texture = static_cast<TextureMetal*>(source.texture);
        if (_textBank && texture)
            _textBank->UseMipmap(texture, 0, 0);
        MTL::Texture* resolved = texture ? _textureRegistry.Resolve(texture->GetHandle()) : nullptr;
        const int count = std::min(source.vertexCount, casters.alphaVertexCount - source.firstVertex);
        if (count >= 3)
            batches.push_back({resolved ? resolved : _fallbackWhite[0], source.firstVertex, count});
    }

    const Metal::PipelineKey solidKey = Metal::PipelineKey::Make(
        Metal::VertexStage::ShadowDepthSolid, Metal::FragmentStage::ShadowDepthSolid,
        Metal::PipelineBlend::Opaque, 0, Metal::VertexLayout::ShadowDepthSolid,
        Metal::AttachmentConfig::CascadePass, 0, false);
    const Metal::PipelineKey alphaKey = Metal::PipelineKey::Make(
        Metal::VertexStage::ShadowDepthAlpha, Metal::FragmentStage::ShadowDepthAlpha,
        Metal::PipelineBlend::Opaque, 0, Metal::VertexLayout::ShadowDepthAlpha,
        Metal::AttachmentConfig::CascadePass, 0, false);
    MTL::RenderPipelineState* solidPipeline = haveSolid ? ResolvePipeline(solidKey, true) : nullptr;
    MTL::RenderPipelineState* alphaPipeline = !batches.empty() ? ResolvePipeline(alphaKey, true) : nullptr;
    if ((haveSolid && !solidPipeline) || (!batches.empty() && !alphaPipeline))
    {
        _shadowMapActive = false;
        return;
    }

    bool rendered = false;
    bool complete = true;
    for (int cascade = 0; cascade < numCascades; ++cascade)
    {
        MTL::RenderCommandEncoder* encoder = EnsureCascadeEncoder(static_cast<unsigned>(cascade));
        if (!encoder)
        {
            complete = false;
            break;
        }
        encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(res), static_cast<double>(res), 0, 1});
        encoder->setScissorRect(
            MTL::ScissorRect{0, 0, static_cast<NS::UInteger>(res), static_cast<NS::UInteger>(res)});
        encoder->setFrontFacingWinding(MTL::WindingClockwise);
        encoder->setDepthStencilState(_shadowDepthState);
        encoder->setVertexBytes(lightVPs + cascade * 16, 16 * sizeof(float), 0);

        if (haveSolid)
        {
            encoder->setRenderPipelineState(solidPipeline);
            encoder->setCullMode(MTL::CullModeFront);
            encoder->setVertexBuffer(solid.buffer, solid.offset, 29);
            encoder->drawPrimitives(
                MTL::PrimitiveTypeTriangle, NS::UInteger(0), static_cast<NS::UInteger>(casters.solidVertexCount));
            ++Poseidon::gPerfDrawCalls;
            rendered = true;
        }
        if (alphaPipeline)
        {
            encoder->setRenderPipelineState(alphaPipeline);
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(alpha.buffer, alpha.offset, 29);
            encoder->setFragmentSamplerState(_samplers[0], 0);
            for (const ResolvedBatch& batch : batches)
            {
                encoder->setFragmentTexture(batch.texture, 0);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        static_cast<NS::UInteger>(batch.firstVertex),
                                        static_cast<NS::UInteger>(batch.vertexCount));
                ++Poseidon::gPerfDrawCalls;
                rendered = true;
            }
        }
    }

    _encoderBroker.EndCurrent();
    _frameEncoder = nullptr;
    _encoderVSBuffer = nullptr;
    _encoderPSBuffer = nullptr;
    _encoderWorldBuffer = nullptr;
    if (!complete || !rendered)
    {
        _shadowMapActive = false;
        return;
    }

    _shadowCascades = numCascades;
    _shadowOmniCount = std::max(0, std::min(omniCount, numCascades));
    std::memcpy(_shadowMapVP.data(), lightVPs,
                static_cast<std::size_t>(numCascades) * 16 * sizeof(float));
    std::memcpy(_shadowSplits.data(), splitViewDist,
                static_cast<std::size_t>(numCascades) * sizeof(float));
    std::memcpy(_shadowCamFwd.data(), camFwd3, 3 * sizeof(float));
    _shadowMapActive = true;
    // Match GL33's frame-lagged contract: BeginPass bound the previous map
    // and constants; this pass becomes visible to receivers next frame.
}

bool EngineMetal::ShadowDepthProbe(const float* lightVP16, const float* triXYZ, int vertCount,
                                   int res, float* outDepth)
{
    if (!lightVP16 || !triXYZ || vertCount < 3 || res <= 0 || !outDepth)
        return false;
    if (!_shadowProbeDepth || static_cast<int>(_shadowProbeDepth->width()) != res)
    {
        MTL::Texture* replacement = CreateDepthArray(_metal.device, res, 1);
        if (!replacement)
            return false;
        if (_shadowProbeDepth)
            _shadowProbeDepth->release();
        _shadowProbeDepth = replacement;
    }

    const Metal::CopyLayout layout =
        Metal::CopyLayout::Compute(Metal::CopyFormat::Depth32Float, res, res);
    MTL::Buffer* vertices = _metal.device->newBuffer(
        triXYZ, static_cast<NS::UInteger>(vertCount) * 3 * sizeof(float),
        MTL::ResourceStorageModeShared);
    MTL::Buffer* staging =
        _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    if (!vertices || !staging || !commandBuffer)
    {
        if (vertices)
            vertices->release();
        if (staging)
            staging->release();
        return false;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    MTL::RenderPassDepthAttachmentDescriptor* depth = pass ? pass->depthAttachment() : nullptr;
    if (!depth)
    {
        vertices->release();
        staging->release();
        return false;
    }
    depth->setTexture(_shadowProbeDepth);
    depth->setSlice(0);
    depth->setLoadAction(MTL::LoadActionClear);
    depth->setStoreAction(MTL::StoreActionStore);
    depth->setClearDepth(1.0);
    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
    const Metal::PipelineKey key = Metal::PipelineKey::Make(
        Metal::VertexStage::ShadowDepthSolid, Metal::FragmentStage::ShadowDepthSolid,
        Metal::PipelineBlend::Opaque, 0, Metal::VertexLayout::ShadowDepthSolid,
        Metal::AttachmentConfig::CascadePass, 0, false);
    MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true);
    if (!encoder || !pipeline)
    {
        if (encoder)
            encoder->endEncoding();
        vertices->release();
        staging->release();
        return false;
    }
    encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(res), static_cast<double>(res), 0, 1});
    encoder->setScissorRect(
        MTL::ScissorRect{0, 0, static_cast<NS::UInteger>(res), static_cast<NS::UInteger>(res)});
    encoder->setRenderPipelineState(pipeline);
    encoder->setDepthStencilState(_shadowDepthState);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setFrontFacingWinding(MTL::WindingClockwise);
    encoder->setVertexBytes(lightVP16, 16 * sizeof(float), 0);
    encoder->setVertexBuffer(vertices, 0, 29);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), static_cast<NS::UInteger>(vertCount));
    ++Poseidon::gPerfDrawCalls;
    encoder->endEncoding();

    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        vertices->release();
        staging->release();
        return false;
    }
    blit->copyFromTexture(_shadowProbeDepth, 0, 0, MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(res, res, 1), staging, 0, layout.bytesPerRow,
                          layout.bytesPerImage);
    blit->endEncoding();
    AttachDiagnostics(commandBuffer);
    commandBuffer->commit();
    commandBuffer->waitUntilCompleted();
    const bool success = commandBuffer->status() != MTL::CommandBufferStatusError;
    if (success)
    {
        const float* source = static_cast<const float*>(staging->contents());
        const std::size_t sourceStride = layout.bytesPerRow / sizeof(float);
        // Metal readback is top-left; the oracle contract is row 0 = bottom.
        for (int y = 0; y < res; ++y)
            std::memcpy(outDepth + static_cast<std::size_t>(res - 1 - y) * res,
                        source + static_cast<std::size_t>(y) * sourceStride,
                        static_cast<std::size_t>(res) * sizeof(float));
    }
    vertices->release();
    staging->release();
    return success;
}

bool EngineMetal::ReadShadowDepthLayer(MTL::Texture* texture, int layer, std::vector<float>& depth)
{
    if (!texture || layer < 0 || static_cast<NS::UInteger>(layer) >= texture->arrayLength())
        return false;
    const int width = static_cast<int>(texture->width());
    const int height = static_cast<int>(texture->height());
    const Metal::CopyLayout layout =
        Metal::CopyLayout::Compute(Metal::CopyFormat::Depth32Float, width, height);
    MTL::Buffer* staging =
        _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    MTL::BlitCommandEncoder* blit = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
    if (!staging || !commandBuffer || !blit)
    {
        if (staging)
            staging->release();
        return false;
    }
    blit->copyFromTexture(texture, static_cast<NS::UInteger>(layer), 0, MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(width, height, 1), staging, 0, layout.bytesPerRow,
                          layout.bytesPerImage);
    blit->endEncoding();
    if (!SubmitSynchronousReadback(commandBuffer))
    {
        staging->release();
        return false;
    }
    depth.resize(static_cast<std::size_t>(width) * height);
    const float* source = static_cast<const float*>(staging->contents());
    const std::size_t sourceStride = layout.bytesPerRow / sizeof(float);
    for (int y = 0; y < height; ++y)
        std::memcpy(depth.data() + static_cast<std::size_t>(y) * width,
                    source + static_cast<std::size_t>(y) * sourceStride,
                    static_cast<std::size_t>(width) * sizeof(float));
    staging->release();
    return true;
}

bool EngineMetal::DumpShadowMap(const char* path)
{
    if (!path || !_shadowMapActive || !_shadowDepthArray || _shadowCascades < 1)
        return false;
    std::vector<float> depth;
    if (!ReadShadowDepthLayer(_shadowDepthArray, 0, depth))
        return false;
    std::vector<std::uint8_t> gray(depth.size());
    for (std::size_t i = 0; i < depth.size(); ++i)
    {
        const float value = depth[i];
        gray[i] = value >= 0.999f
                      ? static_cast<std::uint8_t>(35)
                      : static_cast<std::uint8_t>((0.15f + (1.0f - value) * 0.85f) * 255.0f);
    }
    return PNGWriter::WritePNG(path, _shadowMapRes, _shadowMapRes, 1, gray.data());
}

bool EngineMetal::ShadowMapCacheSelfTest()
{
    // EncoderBroker replays the complete frame sticky-state snapshot whenever
    // TargetId changes back from a cascade slice. There is no independent
    // descriptor-dedup cache that a depth pass can poison.
    return true;
}
} // namespace Poseidon
