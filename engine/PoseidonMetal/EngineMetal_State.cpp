#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <PoseidonMetal/Shaders/PoseidonShaderTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>

namespace Poseidon
{
namespace
{
const char* VertexFunctionName(Metal::VertexStage stage)
{
    static const char* names[] = {"vsScreen",           "vsTransform",        "vsShadow",
                                  "vsShadowDepthSolid", "vsShadowDepthAlpha", "vsBlitScale"};
    return names[static_cast<unsigned>(stage)];
}

const char* FragmentFunctionName(Metal::FragmentStage stage)
{
    static const char* names[] = {
        "psNormal",           "psDetail",           "psGrass",    "psWater", "psShadow", "psFlat",
        "psShadowDepthSolid", "psShadowDepthAlpha", "psBlitScale"};
    return names[static_cast<unsigned>(stage)];
}

void ConfigureTLVertexDescriptor(MTL::VertexDescriptor* descriptor)
{
    struct Attribute
    {
        MTL::VertexFormat format;
        std::size_t offset;
    };
    const Attribute attributes[] = {
        {MTL::VertexFormatFloat3, offsetof(TLVertex, pos)},
        {MTL::VertexFormatFloat, offsetof(TLVertex, rhw)},
        {MTL::VertexFormatUChar4Normalized_BGRA, offsetof(TLVertex, color)},
        {MTL::VertexFormatUChar4Normalized_BGRA, offsetof(TLVertex, specular)},
        {MTL::VertexFormatFloat2, offsetof(TLVertex, t0)},
        {MTL::VertexFormatFloat2, offsetof(TLVertex, t1)},
    };
    for (unsigned i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i)
    {
        MTL::VertexAttributeDescriptor* attribute = descriptor->attributes()->object(i);
        attribute->setFormat(attributes[i].format);
        attribute->setOffset(attributes[i].offset);
        attribute->setBufferIndex(30);
    }
    MTL::VertexBufferLayoutDescriptor* layout = descriptor->layouts()->object(30);
    layout->setStride(sizeof(TLVertex));
    layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    layout->setStepRate(1);
}

void ConfigureWorldVertexDescriptor(MTL::VertexDescriptor* descriptor)
{
    const MTL::VertexFormat formats[] = {
        MTL::VertexFormatFloat3,
        MTL::VertexFormatFloat3,
        MTL::VertexFormatFloat2,
    };
    const std::size_t offsets[] = {0, sizeof(Vector3P), sizeof(Vector3P) * 2};
    for (unsigned i = 0; i < 3; ++i)
    {
        MTL::VertexAttributeDescriptor* attribute = descriptor->attributes()->object(i);
        attribute->setFormat(formats[i]);
        attribute->setOffset(offsets[i]);
        attribute->setBufferIndex(29);
    }
    MTL::VertexBufferLayoutDescriptor* layout = descriptor->layouts()->object(29);
    layout->setStride(sizeof(Vector3P) * 2 + sizeof(UVPair));
    layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    layout->setStepRate(1);
}

MTL::ColorWriteMask ColorMask(std::uint8_t mask)
{
    return static_cast<MTL::ColorWriteMask>(mask & 0xf);
}

std::string PipelineKeyLabel(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

Metal::PipelineBlend ToPipelineBlend(render::BlendMode blend)
{
    static_assert(static_cast<unsigned>(Metal::PipelineBlend::Opaque) ==
                  static_cast<unsigned>(render::BlendMode::Opaque));
    static_assert(static_cast<unsigned>(Metal::PipelineBlend::AlphaBlend) ==
                  static_cast<unsigned>(render::BlendMode::AlphaBlend));
    static_assert(static_cast<unsigned>(Metal::PipelineBlend::Additive) ==
                  static_cast<unsigned>(render::BlendMode::Additive));
    static_assert(static_cast<unsigned>(Metal::PipelineBlend::Shadow) ==
                  static_cast<unsigned>(render::BlendMode::Shadow));
    return static_cast<Metal::PipelineBlend>(blend);
}

Metal::DepthMode ToDepthMode(render::DepthMode depth)
{
    static_assert(static_cast<unsigned>(Metal::DepthMode::Normal) == static_cast<unsigned>(render::DepthMode::Normal));
    static_assert(static_cast<unsigned>(Metal::DepthMode::ReadOnly) ==
                  static_cast<unsigned>(render::DepthMode::ReadOnly));
    static_assert(static_cast<unsigned>(Metal::DepthMode::Disabled) ==
                  static_cast<unsigned>(render::DepthMode::Disabled));
    static_assert(static_cast<unsigned>(Metal::DepthMode::Shadow) == static_cast<unsigned>(render::DepthMode::Shadow));
    return static_cast<Metal::DepthMode>(depth);
}
} // namespace

bool EngineMetal::InitializeM1Resources()
{
    if (!_metal.device->supportsBCTextureCompression())
    {
        RecordDiagnostic("device does not support the BC texture compression required by the Metal backend");
        return false;
    }
    return RebuildFrameTargets() && InitializeDepthStates() && InitializeSamplers() && InitializePipelines() &&
           InitializeFallbackTextures();
}

void EngineMetal::DestroyM1Resources()
{
    EndFrameEncoder();
    if (_frameColor)
        _frameColor->release();
    if (_frameDepthStencil)
        _frameDepthStencil->release();
    if (_captureColor)
        _captureColor->release();
    _frameColor = nullptr;
    _frameDepthStencil = nullptr;
    _captureColor = nullptr;

    for (MTL::Texture*& texture : _fallbackWhite)
    {
        if (texture)
            texture->release();
        texture = nullptr;
    }
    for (auto& entry : _pipelineCache)
        if (entry.second)
            entry.second->release();
    _pipelineCache.clear();
    for (MTL::DepthStencilState*& state : _depthStates)
    {
        if (state)
            state->release();
        state = nullptr;
    }
    for (MTL::SamplerState*& sampler : _samplers)
    {
        if (sampler)
            sampler->release();
        sampler = nullptr;
    }
    _textureRegistry.Clear();
    _meshRegistry.Clear();
    _frameMeshHandles.clear();
    _frameMeshes.clear();
}

bool EngineMetal::RebuildFrameTargets()
{
    if (!_metal.device || _w <= 0 || _h <= 0)
        return false;
    const int targetWidth = std::max(1, static_cast<int>(_w * _renderScale + 0.5f));
    const int targetHeight = std::max(1, static_cast<int>(_h * _renderScale + 0.5f));

    MTL::TextureDescriptor* color =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, targetWidth, targetHeight, false);
    MTL::TextureDescriptor* depth =
        MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatDepth32Float_Stencil8, targetWidth, targetHeight, false);
    if (!color || !depth)
    {
        RecordDiagnostic("failed to create frame-target texture descriptors");
        return false;
    }
    color->setStorageMode(MTL::StorageModePrivate);
    color->setUsage(static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));
    MTL::Texture* newColor = _metal.device->newTexture(color);

    depth->setStorageMode(MTL::StorageModePrivate);
    depth->setUsage(MTL::TextureUsageRenderTarget);
    MTL::Texture* newDepth = _metal.device->newTexture(depth);
    MTL::Texture* newCapture = nullptr;
    if (_renderScale != 1.0f)
    {
        MTL::TextureDescriptor* capture =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, _w, _h, false);
        if (capture)
        {
            capture->setStorageMode(MTL::StorageModePrivate);
            capture->setUsage(
                static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));
            newCapture = _metal.device->newTexture(capture);
        }
    }
    if (!newColor || !newDepth || (_renderScale != 1.0f && !newCapture))
    {
        if (newColor)
            newColor->release();
        if (newDepth)
            newDepth->release();
        if (newCapture)
            newCapture->release();
        RecordDiagnostic("failed to create " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight) +
                         " frame targets and window-sized capture target");
        return false;
    }

    if (_frameColor)
        _frameColor->release();
    if (_frameDepthStencil)
        _frameDepthStencil->release();
    if (_captureColor)
        _captureColor->release();
    _frameColor = newColor;
    _frameDepthStencil = newDepth;
    _captureColor = newCapture;
    _captureResolvedThisFrame = false;
    _frameNeedsClear = true;
    _clearDepth = true;
    return true;
}

bool EngineMetal::InitializeDepthStates()
{
    for (unsigned i = 0; i < _depthStates.size(); ++i)
    {
        const Metal::DepthMode mode = static_cast<Metal::DepthMode>(i);
        MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
        if (!descriptor)
        {
            RecordDiagnostic("failed to allocate depth-stencil descriptor " + std::to_string(i));
            return false;
        }
        const bool normal = mode == Metal::DepthMode::Normal;
        const bool disabled = mode == Metal::DepthMode::Disabled;
        const bool shadow = mode == Metal::DepthMode::Shadow;
        const bool clear = mode == Metal::DepthMode::ClearDepthStencil;
        const bool colorOnlyClear = mode == Metal::DepthMode::ColorOnlyClear;
        descriptor->setDepthCompareFunction(disabled || clear || colorOnlyClear ? MTL::CompareFunctionAlways
                                                                                : MTL::CompareFunctionLessEqual);
        descriptor->setDepthWriteEnabled(normal || clear);

        MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
        if (!stencil)
        {
            descriptor->release();
            RecordDiagnostic("failed to allocate stencil descriptor " + std::to_string(i));
            return false;
        }
        stencil->setStencilCompareFunction(shadow ? MTL::CompareFunctionEqual : MTL::CompareFunctionAlways);
        stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
        stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
        stencil->setDepthStencilPassOperation(
            colorOnlyClear ? MTL::StencilOperationKeep
                           : shadow ? MTL::StencilOperationIncrementClamp : MTL::StencilOperationReplace);
        stencil->setReadMask(0xff);
        stencil->setWriteMask(colorOnlyClear ? 0 : 0xff);
        descriptor->setFrontFaceStencil(stencil);
        descriptor->setBackFaceStencil(stencil);
        _depthStates[i] = _metal.device->newDepthStencilState(descriptor);
        stencil->release();
        descriptor->release();
        if (!_depthStates[i])
        {
            RecordDiagnostic("failed to create depth-stencil state " + std::to_string(i));
            return false;
        }
    }
    return true;
}

bool EngineMetal::InitializeSamplers()
{
    for (unsigned index = 0; index < _samplers.size(); ++index)
    {
        const bool point = (index & 4u) != 0;
        const bool clampU = (index & 2u) != 0;
        const bool clampV = (index & 1u) != 0;
        MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
        if (!descriptor)
        {
            RecordDiagnostic("failed to allocate sampler descriptor " + std::to_string(index));
            return false;
        }
        descriptor->setMinFilter(point ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear);
        descriptor->setMagFilter(point ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear);
        descriptor->setMipFilter(point ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear);
        descriptor->setSAddressMode(clampU ? MTL::SamplerAddressModeClampToEdge : MTL::SamplerAddressModeRepeat);
        descriptor->setTAddressMode(clampV ? MTL::SamplerAddressModeClampToEdge : MTL::SamplerAddressModeRepeat);
        descriptor->setMaxAnisotropy(point ? 1 : 16);
        _samplers[index] = _metal.device->newSamplerState(descriptor);
        descriptor->release();
        if (!_samplers[index])
        {
            RecordDiagnostic("failed to create sampler state " + std::to_string(index));
            return false;
        }
    }
    return true;
}

bool EngineMetal::InitializePipelines()
{
    for (Metal::FragmentStage fragment : {Metal::FragmentStage::Normal, Metal::FragmentStage::Flat})
    {
        for (unsigned blend = 0; blend < static_cast<unsigned>(Metal::PipelineBlend::Count); ++blend)
        {
            const Metal::PipelineKey key = Metal::PipelineKey::Make(
                Metal::VertexStage::Screen, fragment, static_cast<Metal::PipelineBlend>(blend), 0xf,
                Metal::VertexLayout::TLVertex, Metal::AttachmentConfig::FramePass, 0, false);
            if (!ResolvePipeline(key, false))
                return false;
        }
    }
    for (Metal::FragmentStage fragment : {Metal::FragmentStage::Normal, Metal::FragmentStage::Detail,
                                          Metal::FragmentStage::Grass, Metal::FragmentStage::Water})
    {
        for (unsigned blend = 0; blend < static_cast<unsigned>(Metal::PipelineBlend::Count); ++blend)
        {
            const Metal::PipelineKey key = Metal::PipelineKey::Make(
                Metal::VertexStage::Transform, fragment, static_cast<Metal::PipelineBlend>(blend), 0xf,
                Metal::VertexLayout::SVertex, Metal::AttachmentConfig::FramePass, 0, false);
            if (!ResolvePipeline(key, false))
                return false;
        }
    }
    const Metal::PipelineKey clear = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, 0,
        Metal::VertexLayout::None, Metal::AttachmentConfig::FramePass, 0, false);
    if (!ResolvePipeline(clear, false))
        return false;
    const Metal::PipelineKey blackFill = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, 0xf,
        Metal::VertexLayout::None, Metal::AttachmentConfig::FramePass, 0, false);
    if (!ResolvePipeline(blackFill, false))
        return false;
    const Metal::PipelineKey blit = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, 0xf,
        Metal::VertexLayout::None, Metal::AttachmentConfig::PresentPass, 0, false);
    return ResolvePipeline(blit, false) != nullptr;
}

MTL::RenderPipelineState* EngineMetal::ResolvePipeline(Metal::PipelineKey key, bool logMiss)
{
    const auto found = _pipelineCache.find(key.value);
    if (found != _pipelineCache.end())
        return found->second;
    if (logMiss)
        LOG_WARN(Graphics, "Metal: PSO prebuild miss for key 0x{:08x}", key.value);
    MTL::RenderPipelineState* pipeline = BuildPipeline(key);
    if (pipeline)
        _pipelineCache.emplace(key.value, pipeline);
    return pipeline;
}

MTL::RenderPipelineState* EngineMetal::BuildPipeline(Metal::PipelineKey key)
{
    const Metal::VertexStage vs = static_cast<Metal::VertexStage>(key.value & 0x7u);
    const Metal::FragmentStage ps = static_cast<Metal::FragmentStage>((key.value >> 3) & 0xfu);
    const Metal::PipelineBlend blend = static_cast<Metal::PipelineBlend>((key.value >> 7) & 0x3u);
    const std::uint8_t colorMask = static_cast<std::uint8_t>((key.value >> 9) & 0xfu);
    const Metal::VertexLayout layout = static_cast<Metal::VertexLayout>((key.value >> 13) & 0x7u);
    const Metal::AttachmentConfig attachments = static_cast<Metal::AttachmentConfig>((key.value >> 16) & 0x3u);
    const unsigned samples = 1u << ((key.value >> 18) & 0x3u);
    const bool alphaToCoverage = ((key.value >> 20) & 1u) != 0;

    NS::String* vsName = NS::String::string(VertexFunctionName(vs), NS::UTF8StringEncoding);
    NS::String* psName = NS::String::string(FragmentFunctionName(ps), NS::UTF8StringEncoding);
    MTL::Function* vertex = _metal.shaderLibrary->newFunction(vsName);
    MTL::Function* fragment = _metal.shaderLibrary->newFunction(psName);
    if (!vertex || !fragment)
    {
        RecordDiagnostic(std::string("shader entry point missing: ") + VertexFunctionName(vs) + " / " +
                         FragmentFunctionName(ps));
        if (vertex)
            vertex->release();
        if (fragment)
            fragment->release();
        return nullptr;
    }

    MTL::RenderPipelineDescriptor* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    if (!descriptor)
    {
        RecordDiagnostic("failed to allocate PSO descriptor for key " + PipelineKeyLabel(key.value));
        vertex->release();
        fragment->release();
        return nullptr;
    }
    descriptor->setVertexFunction(vertex);
    descriptor->setFragmentFunction(fragment);
    descriptor->setSampleCount(samples);
    descriptor->setAlphaToCoverageEnabled(alphaToCoverage);
    descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);

    if (layout == Metal::VertexLayout::TLVertex || layout == Metal::VertexLayout::SVertex)
    {
        MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::vertexDescriptor();
        if (!vertexDescriptor)
        {
            RecordDiagnostic("failed to create TLVertex descriptor for key " + PipelineKeyLabel(key.value));
            descriptor->release();
            vertex->release();
            fragment->release();
            return nullptr;
        }
        if (layout == Metal::VertexLayout::TLVertex)
            ConfigureTLVertexDescriptor(vertexDescriptor);
        else
            ConfigureWorldVertexDescriptor(vertexDescriptor);
        descriptor->setVertexDescriptor(vertexDescriptor);
    }

    MTL::RenderPipelineColorAttachmentDescriptor* color = descriptor->colorAttachments()->object(0);
    if (attachments != Metal::AttachmentConfig::CascadePass)
    {
        color->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        color->setWriteMask(ColorMask(colorMask));
        if (blend != Metal::PipelineBlend::Opaque)
        {
            color->setBlendingEnabled(true);
            color->setRgbBlendOperation(MTL::BlendOperationAdd);
            color->setAlphaBlendOperation(MTL::BlendOperationAdd);
            if (blend == Metal::PipelineBlend::AlphaBlend)
            {
                color->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                color->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                color->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            }
            else if (blend == Metal::PipelineBlend::Additive)
            {
                color->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                color->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                color->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            }
            else
            {
                color->setSourceRGBBlendFactor(MTL::BlendFactorZero);
                color->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                color->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            }
        }
    }
    if (attachments == Metal::AttachmentConfig::FramePass)
    {
        descriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
        descriptor->setStencilAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
    }
    else if (attachments == Metal::AttachmentConfig::CascadePass)
    {
        descriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    }

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = _metal.device->newRenderPipelineState(descriptor, &error);
    if (!pipeline)
    {
        const char* message =
            error && error->localizedDescription() ? error->localizedDescription()->utf8String() : "unknown error";
        RecordDiagnostic("PSO creation failed for key " + PipelineKeyLabel(key.value) + ": " + message);
    }
    descriptor->release();
    vertex->release();
    fragment->release();
    return pipeline;
}

MTL::RenderCommandEncoder* EngineMetal::EnsureFrameEncoder()
{
    if (_frameEncoder)
        return _frameEncoder;
    if (!_frameCommandBuffer || !_frameColor || !_frameDepthStencil)
        return nullptr;

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!pass)
    {
        RecordDiagnostic("failed to create the frame render-pass descriptor");
        return nullptr;
    }
    MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
    color->setTexture(_frameColor);
    color->setLoadAction(_frameNeedsClear ? MTL::LoadActionClear : MTL::LoadActionLoad);
    color->setStoreAction(MTL::StoreActionUnknown);
    color->setClearColor(*_clearColor);

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(_frameDepthStencil);
    depth->setLoadAction(_clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    depth->setStoreAction(MTL::StoreActionUnknown);
    depth->setClearDepth(1.0);
    MTL::RenderPassStencilAttachmentDescriptor* stencil = pass->stencilAttachment();
    stencil->setTexture(_frameDepthStencil);
    stencil->setLoadAction(_clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    stencil->setStoreAction(MTL::StoreActionUnknown);
    stencil->setClearStencil(0);

    _frameEncoder = _frameCommandBuffer->renderCommandEncoder(pass);
    if (_frameEncoder)
    {
        _frameEncoder->setViewport(MTL::Viewport{_currentViewport.x, _currentViewport.y, _currentViewport.width,
                                                 _currentViewport.height, 0.0, 1.0});
        _frameEncoder->setScissorRect(MTL::ScissorRect{_currentScissor.x, _currentScissor.y, _currentScissor.width,
                                                       _currentScissor.height});
        _frameEncoder->setStencilReferenceValue(0);
        _currentPipeline = nullptr;
        _currentPipelineWorld = false;
        _currentDepthState = nullptr;
        _encoderVSBuffer = nullptr;
        _encoderPSBuffer = nullptr;
        _encoderWorldBuffer = nullptr;
    }
    else
    {
        RecordDiagnostic("failed to create the frame render encoder");
        return nullptr;
    }
    _frameNeedsClear = false;
    _clearDepth = false;
    return _frameEncoder;
}

void EngineMetal::EndFrameEncoder(bool terminal)
{
    if (_frameEncoder)
    {
        _frameEncoder->setColorStoreAction(MTL::StoreActionStore, 0);
        _frameEncoder->setDepthStoreAction(terminal ? MTL::StoreActionDontCare : MTL::StoreActionStore);
        _frameEncoder->setStencilStoreAction(terminal ? MTL::StoreActionDontCare : MTL::StoreActionStore);
        _frameEncoder->endEncoding();
        _frameEncoder = nullptr;
    }
}

bool EngineMetal::ApplyScreenState(const render::RenderPassDescriptor& descriptor, Metal::FragmentStage fragment)
{
    if (_in3DPass)
    {
        EndWorldViewport();
        _in3DPass = false;
        _activePassId = static_cast<int>(PassId::ScreenSpace);
    }
    _texGenMode = render::TexGenMode::Fixed;
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return false;
    const Metal::PipelineBlend blend = ToPipelineBlend(descriptor.blend);
    const Metal::PipelineKey key =
        Metal::PipelineKey::Make(Metal::VertexStage::Screen, fragment, blend, 0xf, Metal::VertexLayout::TLVertex,
                                 Metal::AttachmentConfig::FramePass, 0, false);
    MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true);
    if (!pipeline)
        return false;
    encoder->setRenderPipelineState(pipeline);
    _currentPipeline = pipeline;
    _currentPipelineWorld = false;
    encoder->setCullMode(MTL::CullModeNone);
    _currentCull = render::CullMode::None;
    encoder->setFrontFacingWinding(MTL::WindingClockwise);
    _currentWinding = render::FrontFaceMode::CW;
    const Metal::DepthMode depth = ToDepthMode(descriptor.depth);
    encoder->setDepthStencilState(_depthStates[static_cast<unsigned>(depth)]);
    _currentDepthState = _depthStates[static_cast<unsigned>(depth)];

    VSConstantsPod vs = {};
    vs.slots[PoseidonVSSlotViewportScale] = {
        2.0f / std::max(_w, 1), 2.0f / std::max(_h, 1), 0.0f, 0.0f};
    PSConstantsPod ps = {};
    ps.slots[PoseidonPSSlotFogColor] = {_fogColor.R(), _fogColor.G(), _fogColor.B(), 1.0f};
    const bool alphaTest =
        descriptor.alpha == render::AlphaMode::Test || descriptor.alpha == render::AlphaMode::TestAndBlend;
    ps.slots[PoseidonPSSlotAlphaRef] = {
        descriptor.alphaRef / 255.0f, alphaTest ? 1.0f : 0.0f, 0.0f, 0.0f};
    ps.slots[PoseidonPSSlotConstantColor] = {1.0f, 1.0f, 1.0f, 1.0f};
    ps.slots[PoseidonPSSlotNightEye] = {0.299f, 0.587f, 0.114f, 1.0f};

    FrameRing::Allocation vsAllocation = _frameRing.Allocate(sizeof(vs));
    FrameRing::Allocation psAllocation = _frameRing.Allocate(sizeof(ps));
    if (!vsAllocation || !psAllocation)
    {
        RecordDiagnostic("failed to allocate frame-ring constants for a screen draw");
        return false;
    }
    std::memcpy(vsAllocation.contents, &vs, sizeof(vs));
    std::memcpy(psAllocation.contents, &ps, sizeof(ps));
    encoder->setVertexBuffer(vsAllocation.buffer, vsAllocation.offset, 0);
    encoder->setFragmentBuffer(psAllocation.buffer, psAllocation.offset, 1);
    _encoderVSBuffer = vsAllocation.buffer;
    _encoderVSOffset = vsAllocation.offset;
    _encoderPSBuffer = psAllocation.buffer;
    _encoderPSOffset = psAllocation.offset;

    const unsigned sampler = (descriptor.sampler.filter == render::SamplerFilter::Point ? 4u : 0u) |
                             (descriptor.sampler.clampU ? 2u : 0u) | (descriptor.sampler.clampV ? 1u : 0u);
    encoder->setFragmentSamplerState(_samplers[sampler], 0);
    return true;
}

Metal::FragmentStage EngineMetal::FragmentStageFor(const render::RenderPassDescriptor& descriptor) const
{
    switch (descriptor.shader)
    {
        case render::ShaderFamily::Detail:
            return Metal::FragmentStage::Detail;
        case render::ShaderFamily::Grass:
            return Metal::FragmentStage::Grass;
        case render::ShaderFamily::Water:
            return Metal::FragmentStage::Water;
        case render::ShaderFamily::Flat:
            return Metal::FragmentStage::Flat;
        case render::ShaderFamily::Shadow:
            return Metal::FragmentStage::Shadow;
        default:
            return Metal::FragmentStage::Normal;
    }
}

bool EngineMetal::ApplyWorldState(const render::RenderPassDescriptor& descriptor)
{
    // Shadow and flat world stages are intentionally outside M2. Keep frame
    // replay from turning that milestone boundary into a missing-function
    // Metal validation error.
    if (descriptor.shader == render::ShaderFamily::Shadow || descriptor.shader == render::ShaderFamily::Flat)
        return false;
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return false;
    if (_currentPipelineWorld && _currentPipeline && _currentDescriptor == descriptor)
        return true;
    const Metal::PipelineKey key =
        Metal::PipelineKey::Make(Metal::VertexStage::Transform, FragmentStageFor(descriptor),
                                 ToPipelineBlend(descriptor.blend), 0xf, Metal::VertexLayout::SVertex,
                                 Metal::AttachmentConfig::FramePass, 0, false);
    MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true);
    if (!pipeline)
        return false;
    encoder->setRenderPipelineState(pipeline);
    _currentPipeline = pipeline;
    _currentPipelineWorld = true;

    MTL::CullMode cull = MTL::CullModeBack;
    if (descriptor.cull == render::CullMode::Front)
        cull = MTL::CullModeFront;
    else if (descriptor.cull == render::CullMode::None)
        cull = MTL::CullModeNone;
    encoder->setCullMode(cull);
    _currentCull = descriptor.cull;
    const MTL::Winding winding =
        descriptor.frontFace == render::FrontFaceMode::CW ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
    encoder->setFrontFacingWinding(winding);
    _currentWinding = descriptor.frontFace;

    const Metal::DepthMode depth = ToDepthMode(descriptor.depth);
    _currentDepthState = _depthStates[static_cast<unsigned>(depth)];
    encoder->setDepthStencilState(_currentDepthState);

    const bool alphaTest =
        descriptor.alpha == render::AlphaMode::Test || descriptor.alpha == render::AlphaMode::TestAndBlend;
    const float alpha[4] = {descriptor.alphaRef / 255.0f, alphaTest ? 1.0f : 0.0f, 0.0f, 0.0f};
    UploadPSConstant(PoseidonPSSlotAlphaRef, alpha);
    _frameState.fogParams[2] = descriptor.fog == render::FogMode::Enabled ? 1.0f : 0.0f;
    if (std::memcmp(_vsConstants.data() + PoseidonVSSlotFog * 4, _frameState.fogParams,
                    sizeof(_frameState.fogParams)) != 0)
    {
        std::memcpy(_vsConstants.data() + PoseidonVSSlotFog * 4, _frameState.fogParams,
                    sizeof(_frameState.fogParams));
        _constantsVSDirty = true;
    }

    const unsigned sampler = (descriptor.sampler.filter == render::SamplerFilter::Point ? 4u : 0u) |
                             (descriptor.sampler.clampU ? 2u : 0u) | (descriptor.sampler.clampV ? 1u : 0u);
    encoder->setFragmentSamplerState(_samplers[sampler], 0);
    encoder->setFragmentSamplerState(_samplers[0], 1);
    _currentDescriptor = descriptor;
    return true;
}

void EngineMetal::SetViewport(const ViewportState& viewport)
{
    _currentViewport = viewport;
    if (MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder())
        encoder->setViewport(MTL::Viewport{viewport.x, viewport.y, viewport.width, viewport.height, 0.0, 1.0});
}

void EngineMetal::SetFullScissor()
{
    const unsigned width = _frameColor ? static_cast<unsigned>(_frameColor->width()) : static_cast<unsigned>(_w);
    const unsigned height = _frameColor ? static_cast<unsigned>(_frameColor->height()) : static_cast<unsigned>(_h);
    _currentScissor = {0, 0, width, height};
    if (MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder())
        encoder->setScissorRect(MTL::ScissorRect{0, 0, width, height});
}

void EngineMetal::ApplyWorldViewport()
{
    const int targetWidth = _frameColor ? static_cast<int>(_frameColor->width()) : _w;
    const int targetHeight = _frameColor ? static_cast<int>(_frameColor->height()) : _h;
    _minGuardX = -4096;
    _maxGuardX = _w + 4096;
    _minGuardY = -4096;
    _maxGuardY = _h + 4096;

    const AspectSettings& aspect = _aspectSettings;
    const bool full = aspect.worldLeft <= 0.0f && aspect.worldTop <= 0.0f && aspect.worldRight >= 1.0f &&
                      aspect.worldBottom >= 1.0f;
    if (full)
    {
        _worldViewportActive = false;
        SetViewport({0, 0, static_cast<double>(targetWidth), static_cast<double>(targetHeight)});
        return;
    }
    const int x0 = static_cast<int>(aspect.worldLeft * targetWidth + 0.5f);
    const int y0 = static_cast<int>(aspect.worldTop * targetHeight + 0.5f);
    const int x1 = static_cast<int>(aspect.worldRight * targetWidth + 0.5f);
    const int y1 = static_cast<int>(aspect.worldBottom * targetHeight + 0.5f);
    if (x1 <= x0 || y1 <= y0)
    {
        _worldViewportActive = false;
        return;
    }
    SetViewport({static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1 - x0),
                 static_cast<double>(y1 - y0)});
    _worldViewportActive = true;
}

void EngineMetal::FillFrameRectBlack(unsigned x, unsigned y, unsigned width, unsigned height)
{
    if (width == 0 || height == 0)
        return;
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return;
    const Metal::PipelineKey key = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, 0xf,
        Metal::VertexLayout::None, Metal::AttachmentConfig::FramePass, 0, false);
    MTL::RenderPipelineState* pipeline = ResolvePipeline(key, true);
    if (!pipeline)
        return;
    encoder->setRenderPipelineState(pipeline);
    encoder->setDepthStencilState(_depthStates[static_cast<unsigned>(Metal::DepthMode::ColorOnlyClear)]);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(_frameColor->width()),
                                       static_cast<double>(_frameColor->height()), 0, 1});
    encoder->setScissorRect(MTL::ScissorRect{x, y, width, height});
    encoder->setFragmentTexture(_fallbackWhite[0], 0);
    encoder->setFragmentSamplerState(_samplers[0], 0);
    const float black[4] = {0, 0, 0, 1};
    encoder->setFragmentBytes(black, sizeof(black), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    ++Poseidon::gPerfDrawCalls;
}

void EngineMetal::EndWorldViewport()
{
    if (!_worldViewportActive)
        return;
    _worldViewportActive = false;
    const unsigned targetWidth = static_cast<unsigned>(_frameColor->width());
    const unsigned targetHeight = static_cast<unsigned>(_frameColor->height());
    const AspectSettings& aspect = _aspectSettings;
    const unsigned x0 = static_cast<unsigned>(aspect.worldLeft * targetWidth + 0.5f);
    const unsigned x1 = static_cast<unsigned>(aspect.worldRight * targetWidth + 0.5f);
    const unsigned y0 = static_cast<unsigned>(aspect.worldTop * targetHeight + 0.5f);
    const unsigned y1 = static_cast<unsigned>(aspect.worldBottom * targetHeight + 0.5f);
    FillFrameRectBlack(0, 0, x0, targetHeight);
    FillFrameRectBlack(x1, 0, targetWidth > x1 ? targetWidth - x1 : 0, targetHeight);
    FillFrameRectBlack(0, 0, targetWidth, y0);
    FillFrameRectBlack(0, y1, targetWidth, targetHeight > y1 ? targetHeight - y1 : 0);
    SetViewport({0, 0, static_cast<double>(targetWidth), static_cast<double>(targetHeight)});
    SetFullScissor();
    _currentPipeline = nullptr;
    _currentPipelineWorld = false;
    _currentDepthState = nullptr;
}

void EngineMetal::DrawClear(bool clearDepthStencil, bool clearColor, const MTL::ClearColor& color)
{
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return;
    const ViewportState viewport = _currentViewport;
    const ScissorState scissor = _currentScissor;
    MTL::RenderPipelineState* pipeline = _currentPipeline;
    MTL::DepthStencilState* depth = _currentDepthState;
    const render::CullMode cull = _currentCull;
    const render::FrontFaceMode winding = _currentWinding;

    const std::uint8_t colorMask = clearColor ? 0xf : 0;
    const Metal::PipelineKey key = Metal::PipelineKey::Make(
        Metal::VertexStage::BlitScale, Metal::FragmentStage::BlitScale, Metal::PipelineBlend::Opaque, colorMask,
        Metal::VertexLayout::None, Metal::AttachmentConfig::FramePass, 0, false);
    MTL::RenderPipelineState* clearPipeline = ResolvePipeline(key, true);
    if (!clearPipeline)
        return;
    const unsigned width = static_cast<unsigned>(_frameColor->width());
    const unsigned height = static_cast<unsigned>(_frameColor->height());
    encoder->setRenderPipelineState(clearPipeline);
    encoder->setDepthStencilState(_depthStates[static_cast<unsigned>(
        clearDepthStencil ? Metal::DepthMode::ClearDepthStencil : Metal::DepthMode::ColorOnlyClear)]);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setViewport(MTL::Viewport{0, 0, static_cast<double>(width), static_cast<double>(height), 0, 1});
    encoder->setScissorRect(MTL::ScissorRect{0, 0, width, height});
    encoder->setFragmentTexture(_fallbackWhite[0], 0);
    encoder->setFragmentSamplerState(_samplers[0], 0);
    const float tint[4] = {
        static_cast<float>(color.red), static_cast<float>(color.green), static_cast<float>(color.blue),
        static_cast<float>(color.alpha)};
    encoder->setFragmentBytes(tint, sizeof(tint), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    ++Poseidon::gPerfDrawCalls;

    encoder->setViewport(MTL::Viewport{viewport.x, viewport.y, viewport.width, viewport.height, 0, 1});
    encoder->setScissorRect(MTL::ScissorRect{scissor.x, scissor.y, scissor.width, scissor.height});
    encoder->setCullMode(cull == render::CullMode::Back
                             ? MTL::CullModeBack
                             : cull == render::CullMode::Front ? MTL::CullModeFront : MTL::CullModeNone);
    encoder->setFrontFacingWinding(winding == render::FrontFaceMode::CW ? MTL::WindingClockwise
                                                                        : MTL::WindingCounterClockwise);
    if (pipeline)
        encoder->setRenderPipelineState(pipeline);
    if (depth)
        encoder->setDepthStencilState(depth);
    // The clear draw temporarily owns texture/sampler bindings that are not
    // cached independently yet. Invalidate the pipeline cache so the next
    // world draw replays its complete descriptor state.
    _currentPipeline = nullptr;
    _currentPipelineWorld = false;
    _currentDepthState = nullptr;
}

bool EngineMetal::GetGLViewport(int outRect[4]) const
{
    if (!_initialized || !outRect)
        return false;
    outRect[0] = static_cast<int>(_currentViewport.x + 0.5);
    outRect[1] = static_cast<int>(_currentViewport.y + 0.5);
    outRect[2] = static_cast<int>(_currentViewport.width + 0.5);
    outRect[3] = static_cast<int>(_currentViewport.height + 0.5);
    return true;
}

void EngineMetal::SetRenderScale(float scale)
{
    _pendingRenderScale = std::max(1.0f, std::min(scale, 2.0f));
}
} // namespace Poseidon
