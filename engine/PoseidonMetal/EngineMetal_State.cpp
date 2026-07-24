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
    _frameColor = nullptr;
    _frameDepthStencil = nullptr;

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
}

bool EngineMetal::RebuildFrameTargets()
{
    if (!_metal.device || _w <= 0 || _h <= 0)
        return false;

    MTL::TextureDescriptor* color =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, _w, _h, false);
    MTL::TextureDescriptor* depth =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatDepth32Float_Stencil8, _w, _h, false);
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
    if (!newColor || !newDepth)
    {
        if (newColor)
            newColor->release();
        if (newDepth)
            newDepth->release();
        RecordDiagnostic("failed to create " + std::to_string(_w) + "x" + std::to_string(_h) + " frame targets");
        return false;
    }

    if (_frameColor)
        _frameColor->release();
    if (_frameDepthStencil)
        _frameDepthStencil->release();
    _frameColor = newColor;
    _frameDepthStencil = newDepth;
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
        const bool readOnly = mode == Metal::DepthMode::ReadOnly;
        const bool disabled = mode == Metal::DepthMode::Disabled;
        const bool shadow = mode == Metal::DepthMode::Shadow;
        const bool clear = mode == Metal::DepthMode::ClearDepthStencil;
        descriptor->setDepthCompareFunction(disabled || clear ? MTL::CompareFunctionAlways
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
        stencil->setDepthStencilPassOperation(shadow ? MTL::StencilOperationIncrementClamp
                                                     : MTL::StencilOperationReplace);
        stencil->setReadMask(0xff);
        stencil->setWriteMask(shadow || normal || readOnly || disabled || clear ? 0xff : 0);
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

    if (layout == Metal::VertexLayout::TLVertex)
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
        ConfigureTLVertexDescriptor(vertexDescriptor);
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
    color->setStoreAction(MTL::StoreActionStore);
    color->setClearColor(*_clearColor);

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(_frameDepthStencil);
    depth->setLoadAction(_clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    depth->setStoreAction(MTL::StoreActionStore);
    depth->setClearDepth(1.0);
    MTL::RenderPassStencilAttachmentDescriptor* stencil = pass->stencilAttachment();
    stencil->setTexture(_frameDepthStencil);
    stencil->setLoadAction(_clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    stencil->setStoreAction(MTL::StoreActionStore);
    stencil->setClearStencil(0);

    _frameEncoder = _frameCommandBuffer->renderCommandEncoder(pass);
    if (_frameEncoder)
    {
        _frameEncoder->setViewport(MTL::Viewport{0.0, 0.0, static_cast<double>(_w), static_cast<double>(_h), 0.0, 1.0});
        _frameEncoder->setStencilReferenceValue(0);
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

void EngineMetal::EndFrameEncoder()
{
    if (_frameEncoder)
    {
        _frameEncoder->endEncoding();
        _frameEncoder = nullptr;
    }
}

bool EngineMetal::ApplyScreenState(const render::RenderPassDescriptor& descriptor, Metal::FragmentStage fragment)
{
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
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setFrontFacingWinding(MTL::WindingClockwise);
    const Metal::DepthMode depth = ToDepthMode(descriptor.depth);
    encoder->setDepthStencilState(_depthStates[static_cast<unsigned>(depth)]);

    VSConstantsPod vs = {};
    vs.slots[21] = {2.0f / std::max(_w, 1), 2.0f / std::max(_h, 1), 0.0f, 0.0f};
    PSConstantsPod ps = {};
    ps.slots[0] = {_fogColor.R(), _fogColor.G(), _fogColor.B(), 1.0f};
    const bool alphaTest =
        descriptor.alpha == render::AlphaMode::Test || descriptor.alpha == render::AlphaMode::TestAndBlend;
    ps.slots[1] = {descriptor.alphaRef / 255.0f, alphaTest ? 1.0f : 0.0f, 0.0f, 0.0f};
    ps.slots[3] = {1.0f, 1.0f, 1.0f, 1.0f};
    ps.slots[7] = {0.299f, 0.587f, 0.114f, 1.0f};

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

    const unsigned sampler = (descriptor.sampler.filter == render::SamplerFilter::Point ? 4u : 0u) |
                             (descriptor.sampler.clampU ? 2u : 0u) | (descriptor.sampler.clampV ? 1u : 0u);
    encoder->setFragmentSamplerState(_samplers[sampler], 0);
    return true;
}
} // namespace Poseidon
