#include <PoseidonMetal/Private/MetalCppFirst.hpp>
#include <imgui.h>

#ifdef DebugLog
#undef DebugLog
#endif

#include <PoseidonMetal/OverlayRendererMetal.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <PoseidonMetal/CopyLayout.hpp>
#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/HandleRegistry.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

namespace Poseidon::Metal
{
namespace
{
using TextureHandle = HandleRegistry<MTL::Texture>::Handle;

TextureHandle TextureHandleFromID(ImTextureID textureId)
{
    if (textureId == ImTextureID_Invalid ||
        textureId > static_cast<ImTextureID>(std::numeric_limits<TextureHandle>::max()))
        return 0;
    return static_cast<TextureHandle>(textureId);
}

TextureHandle TextureHandleFromUserData(void* userData)
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(userData);
    if (value == 0 || value > std::numeric_limits<TextureHandle>::max())
        return 0;
    return static_cast<TextureHandle>(value);
}

void* TextureHandleToUserData(TextureHandle handle)
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle));
}
} // namespace

void OverlayRendererMetal::RecordTextureDiagnosticOnce(ImTextureData* texture, const char* message)
{
    if (!texture)
    {
        RecordDrawDiagnosticOnce(1ull << 18, message);
        return;
    }
    if (_textureFailures.insert(texture).second)
        _engine.RecordDiagnostic(message);
}

bool OverlayRendererMetal::UpdateTextures(ImDrawData* drawData)
{
    // C4 calls this only after it has established that drawData contains visible
    // command lists. Keeping servicing behind that draw-path gate means a panel
    // that was never opened performs no texture allocation or GPU work.
    if (!drawData)
    {
        RecordDrawDiagnosticOnce(1ull << 16, "imgui Metal texture update received null draw data");
        return false;
    }
    if (!drawData->Textures)
    {
        RecordDrawDiagnosticOnce(1ull << 17, "imgui Metal draw data has no texture request list");
        return false;
    }

    for (ImTextureData* texture : *drawData->Textures)
    {
        if (!texture)
        {
            RecordTextureDiagnosticOnce(nullptr, "imgui Metal texture request list contains a null entry");
            continue;
        }

        bool success = true;
        switch (texture->Status)
        {
            case ImTextureStatus_OK:
            case ImTextureStatus_Destroyed:
                break;
            case ImTextureStatus_WantCreate:
            case ImTextureStatus_WantUpdates:
                success = UpdateTexture(texture);
                break;
            case ImTextureStatus_WantDestroy:
                // Match ImGui's reference backends: a premature destroy is
                // silently deferred until the core reports the texture unused.
                if (texture->UnusedFrames > 0)
                    success = DestroyTexture(texture);
                break;
        }
        if (success)
            _textureFailures.erase(texture);
    }
    // Individual failures remain retryable. Draw commands whose handles are
    // unresolved use the renderer's white fallback while other textures work.
    return true;
}

bool OverlayRendererMetal::UploadTextureRegion(ImTextureData* textureData, MTL::Texture* texture, unsigned x,
                                               unsigned y, unsigned width, unsigned height, bool waitForCompletion)
{
    if (!textureData || !texture)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload received a null texture");
        return false;
    }
    if (!_engine._metal.device || !_engine._metal.commandQueue)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload requires a live device and command queue");
        return false;
    }
    if (textureData->Format != ImTextureFormat_RGBA32 || textureData->BytesPerPixel != 4)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload requires RGBA32 pixels");
        return false;
    }
    if (!textureData->Pixels || textureData->Width <= 0 || textureData->Height <= 0)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload has invalid pixel storage");
        return false;
    }
    if (width == 0 || height == 0 || x >= static_cast<unsigned>(textureData->Width) ||
        y >= static_cast<unsigned>(textureData->Height) || width > static_cast<unsigned>(textureData->Width) - x ||
        height > static_cast<unsigned>(textureData->Height) - y)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload region is empty or out of bounds");
        return false;
    }
    if (texture->width() != static_cast<NS::UInteger>(textureData->Width) ||
        texture->height() != static_cast<NS::UInteger>(textureData->Height))
    {
        RecordTextureDiagnosticOnce(textureData,
                                    "imgui Metal texture upload dimensions do not match the registered texture");
        return false;
    }

    const CopyLayout layout = CopyLayout::Compute(CopyFormat::RGBA8, static_cast<int>(width), static_cast<int>(height));
    const int sourcePitch = textureData->GetPitch();
    const std::size_t requiredSourceRowBytes =
        (static_cast<std::size_t>(x) + width) * static_cast<std::size_t>(textureData->BytesPerPixel);
    if (sourcePitch <= 0 || static_cast<std::size_t>(sourcePitch) < requiredSourceRowBytes)
    {
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture upload source pitch is too small");
        return false;
    }

    MTL::Buffer* staging = _engine._metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    if (!staging)
    {
        RecordTextureDiagnosticOnce(textureData, "failed to allocate the imgui Metal texture staging buffer");
        return false;
    }
    if (!staging->contents())
    {
        staging->release();
        RecordTextureDiagnosticOnce(textureData, "imgui Metal texture staging buffer has no CPU mapping");
        return false;
    }

    const std::size_t sourcePitchBytes = static_cast<std::size_t>(sourcePitch);
    const auto* source =
        static_cast<const std::uint8_t*>(textureData->GetPixelsAt(static_cast<int>(x), static_cast<int>(y)));
    auto* destination = static_cast<std::uint8_t*>(staging->contents());
    for (unsigned row = 0; row < height; ++row)
        std::memcpy(destination + static_cast<std::size_t>(row) * layout.bytesPerRow,
                    source + static_cast<std::size_t>(row) * sourcePitchBytes, layout.tightBytesPerRow);

    MTL::CommandBuffer* commandBuffer = _engine._metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        staging->release();
        RecordTextureDiagnosticOnce(textureData, "failed to create the imgui Metal texture upload command buffer");
        return false;
    }
    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        staging->release();
        RecordTextureDiagnosticOnce(textureData, "failed to create the imgui Metal texture upload blit encoder");
        return false;
    }

    blit->copyFromBuffer(staging, 0, layout.bytesPerRow, layout.bytesPerImage, MTL::Size::Make(width, height, 1),
                         texture, 0, 0, MTL::Origin::Make(x, y, 0));
    blit->endEncoding();
    commandBuffer->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });
    _engine.AttachDiagnostics(commandBuffer);
    commandBuffer->commit();
    if (!waitForCompletion)
        return true;

    commandBuffer->waitUntilCompleted();
    if (commandBuffer->status() != MTL::CommandBufferStatusCompleted)
    {
        RecordTextureDiagnosticOnce(textureData,
                                    "imgui Metal texture upload command buffer did not complete successfully");
        return false;
    }
    return true;
}

bool OverlayRendererMetal::UpdateTexture(ImTextureData* texture)
{
    if (!texture)
    {
        RecordTextureDiagnosticOnce(nullptr, "imgui Metal texture update received a null request");
        return false;
    }

    if (texture->Status == ImTextureStatus_WantCreate)
    {
        if (texture->TexID != ImTextureID_Invalid || texture->BackendUserData)
        {
            RecordTextureDiagnosticOnce(texture, "imgui Metal texture creation found stale backend state");
            return false;
        }
        if (texture->Format != ImTextureFormat_RGBA32 || texture->BytesPerPixel != 4 || !texture->Pixels ||
            texture->Width <= 0 || texture->Height <= 0)
        {
            RecordTextureDiagnosticOnce(texture, "imgui Metal texture creation requires valid RGBA32 pixels");
            return false;
        }
        if (!_engine._metal.device || !_engine._metal.commandQueue)
        {
            RecordTextureDiagnosticOnce(texture,
                                        "imgui Metal texture creation requires a live device and command queue");
            return false;
        }

        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA8Unorm, texture->Width, texture->Height, false);
        if (!descriptor)
        {
            RecordTextureDiagnosticOnce(texture, "failed to create the imgui Metal texture descriptor");
            return false;
        }
        descriptor->setStorageMode(MTL::StorageModePrivate);
        descriptor->setUsage(MTL::TextureUsageShaderRead);

        // This texture is deliberately owned here rather than by TextBankMetal:
        // ResetForRemount releases the text bank on every MODS-tab reload, which
        // must not invalidate the font atlas used to draw that same panel.
        MTL::Texture* metalTexture = _engine._metal.device->newTexture(descriptor);
        if (!metalTexture)
        {
            RecordTextureDiagnosticOnce(texture, "failed to allocate the imgui Metal texture");
            return false;
        }

        if (!UploadTextureRegion(texture, metalTexture, 0, 0, static_cast<unsigned>(texture->Width),
                                 static_cast<unsigned>(texture->Height), true))
        {
            metalTexture->release();
            return false;
        }

        const TextureHandle handle = _engine._textureRegistry.Allocate(metalTexture);
        if (handle == 0)
        {
            metalTexture->release();
            RecordTextureDiagnosticOnce(texture, "imgui Metal texture registry returned an invalid zero handle");
            return false;
        }

        texture->BackendUserData = TextureHandleToUserData(handle);
        texture->SetTexID(static_cast<ImTextureID>(handle));
        texture->SetStatus(ImTextureStatus_OK);
        ++_textureCreates;
        ++_liveTextureHandles;
        LOG_INFO(Graphics, "Metal: imgui texture {} created {}x{} handle={} creates={} destroys={} live={}",
                 texture->UniqueID, texture->Width, texture->Height, handle, _textureCreates, _textureDestroys,
                 _liveTextureHandles);
        return true;
    }

    if (texture->Status == ImTextureStatus_WantUpdates)
    {
        const TextureHandle idHandle = TextureHandleFromID(texture->TexID);
        const TextureHandle userHandle = TextureHandleFromUserData(texture->BackendUserData);
        if (idHandle == 0 || idHandle != userHandle)
        {
            RecordTextureDiagnosticOnce(texture, "imgui Metal texture update has invalid or mismatched handles");
            return false;
        }
        MTL::Texture* metalTexture = _engine._textureRegistry.Resolve(idHandle);
        if (!metalTexture)
        {
            RecordTextureDiagnosticOnce(texture, "imgui Metal texture update could not resolve its registry handle");
            return false;
        }

        const ImTextureRect& region = texture->UpdateRect;
        if (!UploadTextureRegion(texture, metalTexture, region.x, region.y, region.w, region.h, false))
            return false;

        texture->SetStatus(ImTextureStatus_OK);
        LOG_DEBUG(Graphics, "Metal: imgui texture {} updated rect=({},{} {}x{}) handle={}", texture->UniqueID,
                  region.x, region.y, region.w, region.h, idHandle);
        return true;
    }

    RecordTextureDiagnosticOnce(texture, "imgui Metal texture update received an unsupported lifecycle status");
    return false;
}

bool OverlayRendererMetal::DestroyTexture(ImTextureData* texture)
{
    if (!texture)
    {
        RecordTextureDiagnosticOnce(nullptr, "imgui Metal texture destruction received a null request");
        return false;
    }

    const TextureHandle idHandle = TextureHandleFromID(texture->TexID);
    const TextureHandle userHandle = TextureHandleFromUserData(texture->BackendUserData);
    if (idHandle == 0 || idHandle != userHandle)
    {
        RecordTextureDiagnosticOnce(texture, "imgui Metal texture destruction has invalid or mismatched handles");
        return false;
    }
    MTL::Texture* metalTexture = _engine._textureRegistry.Resolve(idHandle);
    if (!metalTexture)
    {
        RecordTextureDiagnosticOnce(texture,
                                    "imgui Metal texture destruction could not resolve its registry handle");
        return false;
    }
    if (_liveTextureHandles == 0 || _textureDestroys >= _textureCreates)
    {
        RecordTextureDiagnosticOnce(texture, "imgui Metal texture lifetime counters would underflow");
        return false;
    }
    if (!_engine._textureRegistry.Release(idHandle))
    {
        RecordTextureDiagnosticOnce(texture, "imgui Metal texture registry rejected a live handle release");
        return false;
    }

    metalTexture->release();
    texture->SetTexID(ImTextureID_Invalid);
    texture->BackendUserData = nullptr;
    texture->SetStatus(ImTextureStatus_Destroyed);
    ++_textureDestroys;
    --_liveTextureHandles;
    LOG_INFO(Graphics, "Metal: imgui texture {} destroyed handle={} creates={} destroys={} live={}", texture->UniqueID,
             idHandle, _textureCreates, _textureDestroys, _liveTextureHandles);
    return true;
}

void OverlayRendererMetal::ShutdownTextures()
{
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    for (ImTextureData* texture : platformIO.Textures)
    {
        if (!texture)
        {
            _engine.RecordDiagnostic("imgui Metal platform texture list contains a null entry at shutdown");
            continue;
        }
        if (texture->RefCount != 1)
            continue;

        if (texture->TexID == ImTextureID_Invalid && !texture->BackendUserData)
        {
            LOG_DEBUG(Graphics, "Metal: imgui texture {} had no Metal allocation at shutdown", texture->UniqueID);
            continue;
        }
        if (!DestroyTexture(texture))
            _engine.RecordDiagnostic("failed to release an imgui Metal texture during shutdown");
    }

    LOG_INFO(Graphics, "Metal: imgui texture shutdown creates={} destroys={} live={}", _textureCreates,
             _textureDestroys, _liveTextureHandles);
    if (_textureCreates != _textureDestroys + _liveTextureHandles)
        _engine.RecordDiagnostic("imgui Metal texture lifetime invariant creates == destroys + live failed");
    if (_liveTextureHandles != 0)
        _engine.RecordDiagnostic("imgui Metal texture shutdown left live registry handles");
}

MTL::Texture* OverlayRendererMetal::ResolveTexture(std::uint64_t textureId) const
{
    const TextureHandle handle = TextureHandleFromID(static_cast<ImTextureID>(textureId));
    MTL::Texture* texture = handle ? _engine._textureRegistry.Resolve(handle) : nullptr;
    if (!texture && !_loggedInvalidTextureResolve)
    {
        _engine.RecordDiagnostic("imgui Metal draw could not resolve a texture registry handle");
        _loggedInvalidTextureResolve = true;
    }
    return texture;
}
} // namespace Poseidon::Metal
