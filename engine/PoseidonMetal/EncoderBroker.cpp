#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EncoderBroker.hpp>

namespace Poseidon::Metal
{
void EncoderBroker::BeginFrame(MTL::CommandBuffer* commandBuffer)
{
    EndCurrent();
    _commandBuffer = commandBuffer;
    _openedCount = 0;
}

void EncoderBroker::SetCommandBuffer(MTL::CommandBuffer* commandBuffer)
{
    EndCurrent();
    _commandBuffer = commandBuffer;
}

bool EncoderBroker::WasOpened(const TargetId& target) const
{
    for (std::size_t i = 0; i < _openedCount; ++i)
        if (_opened[i] == target)
            return true;
    return false;
}

void EncoderBroker::MarkOpened(const TargetId& target)
{
    if (!WasOpened(target) && _openedCount < _opened.size())
        _opened[_openedCount++] = target;
}

bool EncoderBroker::HasOpenedFrameTarget(MTL::Texture* color) const
{
    return WasOpened({color, 0, TargetKind::Frame});
}

MTL::RenderCommandEncoder* EncoderBroker::EnsureFrame(const FrameTarget& target, ReplayCallback replay,
                                                       void* replayContext)
{
    const TargetId id = {target.color, 0, TargetKind::Frame};
    if (_encoder && _current == id)
        return _encoder;
    EndCurrent();
    if (!_commandBuffer || !target.color || !target.depthStencil)
        return nullptr;

    const bool firstOpen = !WasOpened(id);
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!pass)
        return nullptr;

    MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
    color->setTexture(target.color);
    if (target.resolve)
        color->setResolveTexture(target.resolve);
    color->setLoadAction(firstOpen && target.clearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
    color->setStoreAction(MTL::StoreActionUnknown);
    color->setClearColor(
        MTL::ClearColor::Make(target.clearRed, target.clearGreen, target.clearBlue, target.clearAlpha));

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(target.depthStencil);
    depth->setLoadAction(firstOpen && target.clearDepthStencil ? MTL::LoadActionClear : MTL::LoadActionLoad);
    depth->setStoreAction(MTL::StoreActionUnknown);
    depth->setClearDepth(1.0);

    MTL::RenderPassStencilAttachmentDescriptor* stencil = pass->stencilAttachment();
    stencil->setTexture(target.depthStencil);
    stencil->setLoadAction(firstOpen && target.clearDepthStencil ? MTL::LoadActionClear : MTL::LoadActionLoad);
    stencil->setStoreAction(MTL::StoreActionUnknown);
    stencil->setClearStencil(0);

    _encoder = _commandBuffer->renderCommandEncoder(pass);
    if (!_encoder)
        return nullptr;
    _current = id;
    _currentResolve = target.resolve;
    MarkOpened(id);
    _encoder->setStencilReferenceValue(0);
    if (replay)
        replay(replayContext, _encoder);
    return _encoder;
}

MTL::RenderCommandEncoder* EncoderBroker::EnsureCascade(MTL::Texture* depthArray, std::uint32_t layer)
{
    const TargetId id = {depthArray, layer, TargetKind::Cascade};
    if (_encoder && _current == id)
        return _encoder;
    EndCurrent();
    if (!_commandBuffer || !depthArray || layer >= depthArray->arrayLength())
        return nullptr;

    const bool firstOpen = !WasOpened(id);
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!pass)
        return nullptr;
    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(depthArray);
    depth->setSlice(layer);
    depth->setLoadAction(firstOpen ? MTL::LoadActionClear : MTL::LoadActionLoad);
    depth->setStoreAction(MTL::StoreActionUnknown);
    depth->setClearDepth(1.0);

    _encoder = _commandBuffer->renderCommandEncoder(pass);
    if (!_encoder)
        return nullptr;
    _current = id;
    _currentResolve = nullptr;
    MarkOpened(id);
    return _encoder;
}

void EncoderBroker::EndCurrent(bool terminalFrame, bool resolveFrameForReadback)
{
    if (!_encoder)
        return;

    if (_current.kind == TargetKind::Frame)
    {
        MTL::StoreAction colorStore = MTL::StoreActionStore;
        if (_currentResolve)
        {
            colorStore = terminalFrame
                             ? MTL::StoreActionMultisampleResolve
                             : resolveFrameForReadback ? MTL::StoreActionStoreAndMultisampleResolve
                                                       : MTL::StoreActionStore;
        }
        _encoder->setColorStoreAction(colorStore, 0);
        _encoder->setDepthStoreAction(terminalFrame ? MTL::StoreActionDontCare : MTL::StoreActionStore);
        _encoder->setStencilStoreAction(terminalFrame ? MTL::StoreActionDontCare : MTL::StoreActionStore);
    }
    else
    {
        _encoder->setDepthStoreAction(MTL::StoreActionStore);
    }
    _encoder->endEncoding();
    _encoder = nullptr;
    _current = {};
    _currentResolve = nullptr;
}

void EncoderBroker::Reset()
{
    EndCurrent();
    _commandBuffer = nullptr;
    _openedCount = 0;
}
} // namespace Poseidon::Metal
