#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/CopyLayout.hpp>
#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace Poseidon
{
namespace
{
bool IsCompressed(PacFormat format)
{
    return format == PacDXT1 || format == PacDXT2 || format == PacDXT3 || format == PacDXT4 || format == PacDXT5;
}

Metal::CopyFormat CopyFormatFor(PacFormat format)
{
    switch (format)
    {
        case PacDXT1:
            return Metal::CopyFormat::BC1;
        case PacDXT2:
        case PacDXT3:
            return Metal::CopyFormat::BC2;
        case PacDXT4:
        case PacDXT5:
            return Metal::CopyFormat::BC3;
        case PacAI88:
            return Metal::CopyFormat::RG8;
        case PacARGB8888:
            return Metal::CopyFormat::BGRA8;
        default:
            return Metal::CopyFormat::RGBA8;
    }
}

MTL::PixelFormat PixelFormatFor(PacFormat format)
{
    switch (format)
    {
        case PacDXT1:
            return MTL::PixelFormatBC1_RGBA;
        case PacDXT2:
        case PacDXT3:
            return MTL::PixelFormatBC2_RGBA;
        case PacDXT4:
        case PacDXT5:
            return MTL::PixelFormatBC3_RGBA;
        case PacAI88:
            return MTL::PixelFormatRG8Unorm;
        case PacARGB8888:
            return MTL::PixelFormatBGRA8Unorm;
        default:
            return MTL::PixelFormatRGBA8Unorm;
    }
}

std::uint8_t Expand5(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t Expand6(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value << 2) | (value >> 4));
}

std::uint8_t Expand4(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value << 4) | value);
}

void ConvertToTight(PacFormat format, const void* source, int sourcePitch, int width, int height,
                    std::vector<std::uint8_t>& destination)
{
    const Metal::CopyFormat copyFormat = CopyFormatFor(format);
    const Metal::CopyLayout tight = Metal::CopyLayout::Compute(copyFormat, width, height, 1);
    destination.resize(tight.bytesPerImage);

    if (format == PacARGB1555 || format == PacRGB565 || format == PacARGB4444)
    {
        for (int y = 0; y < height; ++y)
        {
            const std::uint16_t* src =
                reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(source) + y * sourcePitch);
            std::uint8_t* dst = destination.data() + static_cast<std::size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x)
            {
                const std::uint16_t pixel = src[x];
                if (format == PacARGB1555)
                {
                    dst[0] = Expand5((pixel >> 10) & 0x1f);
                    dst[1] = Expand5((pixel >> 5) & 0x1f);
                    dst[2] = Expand5(pixel & 0x1f);
                    dst[3] = (pixel & 0x8000) ? 255 : 0;
                }
                else if (format == PacRGB565)
                {
                    dst[0] = Expand5((pixel >> 11) & 0x1f);
                    dst[1] = Expand6((pixel >> 5) & 0x3f);
                    dst[2] = Expand5(pixel & 0x1f);
                    dst[3] = 255;
                }
                else
                {
                    dst[0] = Expand4((pixel >> 8) & 0xf);
                    dst[1] = Expand4((pixel >> 4) & 0xf);
                    dst[2] = Expand4(pixel & 0xf);
                    dst[3] = Expand4((pixel >> 12) & 0xf);
                }
                dst += 4;
            }
        }
        return;
    }

    const std::size_t rows = tight.rowCount;
    const std::size_t sourceRow = IsCompressed(format) ? tight.tightBytesPerRow : static_cast<std::size_t>(sourcePitch);
    for (std::size_t row = 0; row < rows; ++row)
        std::memcpy(destination.data() + row * tight.tightBytesPerRow,
                    static_cast<const std::uint8_t*>(source) + row * sourceRow, tight.tightBytesPerRow);
}

struct UploadMip
{
    int width = 0;
    int height = 0;
    std::size_t offset = 0;
    Metal::CopyLayout layout;
    std::vector<std::uint8_t> bytes;
};
} // namespace

PacFormat UploadFormatForTextureMetal(PacFormat format, bool interpolate)
{
    return interpolate && IsCompressed(format) ? PacARGB1555 : format;
}

bool TextureMetal::EnsureResident(int levelMin)
{
    DoLoadHeaders();
    if (_dynamic)
        return _surface != nullptr;
    if (_surface && _residentLevel <= levelMin)
        return true;
    return _bank && _bank->Engine() && _bank->Engine()->UploadTexture(*this, levelMin);
}

bool TextureMetal::InitFromRGBA(int w, int h, const void* rgba, std::uint32_t size, bool mipmap)
{
    if (!_bank || !_bank->Engine() || !rgba || w <= 0 || h <= 0)
        return false;
    _initialized = true;
    _dynamic = true;
    _dynamicMipmapped = mipmap;
    _maxSize = std::max(w, h);
    _nMipmaps = 1;
    if (mipmap)
        for (int dimension = _maxSize; dimension > 1; dimension >>= 1)
            ++_nMipmaps;
    _mipmaps[0]._w = static_cast<short>(w);
    _mipmaps[0]._h = static_cast<short>(h);
    _mipmaps[0]._pitch = static_cast<short>(w * 4);
    return _bank->Engine()->UploadDynamicTexture(*this, w, h, rgba, size, mipmap);
}

void TextureMetal::UpdateRGBA(const void* rgba, std::uint32_t size)
{
    if (_dynamic && _bank && _bank->Engine() && _nMipmaps > 0)
        _bank->Engine()->UploadDynamicTexture(*this, _mipmaps[0]._w, _mipmaps[0]._h, rgba, size, _dynamicMipmapped);
}

void TextureMetal::ReleaseMemory()
{
    if (_bank && _bank->Engine())
        _bank->Engine()->ReleaseTexture(*this);
    else if (_surface)
    {
        _surface->release();
        _surface = nullptr;
        _handle = 0;
    }
    _residentLevel = _nMipmaps;
}

bool EngineMetal::UploadTexture(TextureMetal& texture, int levelMin)
{
    if (!texture._src || levelMin < 0 || levelMin >= texture._nMipmaps)
        return false;
    if (texture._interpolate)
    {
        texture._interpolate->DoLoadHeaders();
        if (!texture._interpolate->_src || texture._interpolate->_nMipmaps != texture._nMipmaps)
            return false;
    }
    const PacFormat format =
        UploadFormatForTextureMetal(texture._mipmaps[levelMin].DstFormat(), texture._interpolate != nullptr);
    const int mipCount = texture._nMipmaps - levelMin;

    std::vector<UploadMip> mips;
    mips.reserve(mipCount);
    std::size_t stagingSize = 0;
    for (int sourceLevel = levelMin; sourceLevel < texture._nMipmaps; ++sourceLevel)
    {
        PacLevelMem& sourceMip = texture._mipmaps[sourceLevel];
        PacLevelMem mip = sourceMip;
        if (mip.DstFormat() != format)
            mip.SetDestFormat(format, 8);
        const auto sourceLayout = render::mipmap::ComputeLayout(format, sourceMip._w, sourceMip._h);
        AUTO_STATIC_ARRAY(char, sourceBytes, 256 * 256 * 4);
        sourceBytes.Realloc(sourceLayout.dataSize);
        sourceBytes.Resize(sourceLayout.dataSize);
        if (!texture._src->GetMipmapData(sourceBytes.Data(), mip, sourceLevel))
            std::memset(sourceBytes.Data(), 0, sourceLayout.dataSize);

        if (texture._interpolate)
        {
            PacLevelMem otherMip = texture._interpolate->_mipmaps[sourceLevel];
            if (otherMip.DstFormat() != format)
                otherMip.SetDestFormat(format, 8);
            AUTO_STATIC_ARRAY(char, otherBytes, 256 * 256 * 4);
            otherBytes.Realloc(sourceLayout.dataSize);
            otherBytes.Resize(sourceLayout.dataSize);
            texture._interpolate->_src->GetMipmapData(otherBytes.Data(), otherMip, sourceLevel);
            mip.Interpolate(sourceBytes.Data(), otherBytes.Data(), otherMip, texture._iFactor);
        }

        UploadMip upload;
        upload.width = sourceMip._w;
        upload.height = sourceMip._h;
        upload.layout = Metal::CopyLayout::Compute(CopyFormatFor(format), upload.width, upload.height);
        stagingSize = Metal::CopyLayout::AlignUp(stagingSize, 256);
        upload.offset = stagingSize;
        stagingSize += upload.layout.bytesPerImage;
        ConvertToTight(format, sourceBytes.Data(), mip._pitch, upload.width, upload.height, upload.bytes);
        mips.push_back(std::move(upload));
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        PixelFormatFor(format), texture._mipmaps[levelMin]._w, texture._mipmaps[levelMin]._h, mipCount > 1);
    if (!descriptor)
    {
        RecordDiagnostic("failed to create the texture upload descriptor");
        return false;
    }
    descriptor->setMipmapLevelCount(mipCount);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    if (format == PacAI88)
        descriptor->setSwizzle(MTL::TextureSwizzleChannels::Make(MTL::TextureSwizzleRed, MTL::TextureSwizzleRed,
                                                                 MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen));
    MTL::Texture* surface = _metal.device->newTexture(descriptor);
    MTL::Buffer* staging = _metal.device->newBuffer(stagingSize, MTL::ResourceStorageModeShared);
    if (!surface || !staging)
    {
        if (surface)
            surface->release();
        if (staging)
            staging->release();
        RecordDiagnostic("failed to allocate texture surface or staging buffer");
        return false;
    }
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        surface->release();
        staging->release();
        RecordDiagnostic("failed to create the texture upload command buffer");
        return false;
    }
    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        surface->release();
        staging->release();
        RecordDiagnostic("failed to create the texture upload blit encoder");
        return false;
    }

    for (unsigned level = 0; level < mips.size(); ++level)
    {
        const UploadMip& mip = mips[level];
        std::uint8_t* destination = static_cast<std::uint8_t*>(staging->contents()) + mip.offset;
        for (std::size_t row = 0; row < mip.layout.rowCount; ++row)
            std::memcpy(destination + row * mip.layout.bytesPerRow,
                        mip.bytes.data() + row * mip.layout.tightBytesPerRow, mip.layout.tightBytesPerRow);
        blit->copyFromBuffer(staging, mip.offset, mip.layout.bytesPerRow, mip.layout.bytesPerImage,
                             MTL::Size::Make(mip.width, mip.height, 1), surface, 0, level, MTL::Origin::Make(0, 0, 0));
    }
    blit->endEncoding();
    commandBuffer->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });
    AttachDiagnostics(commandBuffer);
    commandBuffer->commit();

    MTL::Texture* oldSurface = texture._surface;
    texture._surface = surface;
    if (texture._handle && _textureRegistry.Resolve(texture._handle))
        _textureRegistry.Replace(texture._handle, surface);
    else
        texture._handle = _textureRegistry.Allocate(surface);
    texture._residentLevel = levelMin;
    if (oldSurface)
        oldSurface->release();
    return texture._handle != 0;
}

bool EngineMetal::UploadDynamicTexture(TextureMetal& texture, int width, int height, const void* rgba,
                                       std::uint32_t size, bool mipmap)
{
    const std::size_t required = static_cast<std::size_t>(width) * height * 4;
    if (!rgba || size < required)
        return false;

    MTL::Texture* surface = texture._surface;
    if (!surface || static_cast<int>(surface->width()) != width || static_cast<int>(surface->height()) != height)
    {
        MTL::TextureDescriptor* descriptor =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width, height, mipmap);
        if (!descriptor)
        {
            RecordDiagnostic("failed to create the dynamic texture descriptor");
            return false;
        }
        descriptor->setStorageMode(MTL::StorageModePrivate);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        surface = _metal.device->newTexture(descriptor);
        if (!surface)
        {
            RecordDiagnostic("failed to create a dynamic texture surface");
            return false;
        }
    }

    const Metal::CopyLayout layout = Metal::CopyLayout::Compute(Metal::CopyFormat::RGBA8, width, height);
    MTL::Buffer* staging = _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    if (!staging)
    {
        if (surface != texture._surface)
            surface->release();
        RecordDiagnostic("failed to allocate the dynamic texture staging buffer");
        return false;
    }
    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    if (!commandBuffer)
    {
        if (surface != texture._surface)
            surface->release();
        staging->release();
        RecordDiagnostic("failed to create the dynamic texture upload command buffer");
        return false;
    }
    MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
    if (!blit)
    {
        if (surface != texture._surface)
            surface->release();
        staging->release();
        RecordDiagnostic("failed to create the dynamic texture upload blit encoder");
        return false;
    }
    for (int row = 0; row < height; ++row)
        std::memcpy(static_cast<std::uint8_t*>(staging->contents()) + row * layout.bytesPerRow,
                    static_cast<const std::uint8_t*>(rgba) + static_cast<std::size_t>(row) * width * 4,
                    layout.tightBytesPerRow);
    blit->copyFromBuffer(staging, 0, layout.bytesPerRow, layout.bytesPerImage, MTL::Size::Make(width, height, 1),
                         surface, 0, 0, MTL::Origin::Make(0, 0, 0));
    if (mipmap)
        blit->generateMipmaps(surface);
    blit->endEncoding();
    commandBuffer->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });
    AttachDiagnostics(commandBuffer);
    commandBuffer->commit();

    if (surface != texture._surface)
    {
        MTL::Texture* oldSurface = texture._surface;
        texture._surface = surface;
        if (texture._handle && _textureRegistry.Resolve(texture._handle))
            _textureRegistry.Replace(texture._handle, surface);
        else
            texture._handle = _textureRegistry.Allocate(surface);
        if (oldSurface)
            oldSurface->release();
    }
    texture._residentLevel = 0;
    return texture._handle != 0;
}

void EngineMetal::ReleaseTexture(TextureMetal& texture)
{
    if (texture._handle)
        _textureRegistry.Release(texture._handle);
    texture._handle = 0;
    if (texture._surface)
        texture._surface->release();
    texture._surface = nullptr;
}

bool EngineMetal::InitializeFallbackTextures()
{
    const Metal::CopyLayout layout = Metal::CopyLayout::Compute(Metal::CopyFormat::RGBA8, 1, 1);
    MTL::Buffer* staging = _metal.device->newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared);
    if (!staging)
    {
        RecordDiagnostic("failed to allocate the fallback-texture staging buffer");
        return false;
    }
    std::uint8_t* bytes = static_cast<std::uint8_t*>(staging->contents());
    bytes[0] = bytes[1] = bytes[2] = bytes[3] = 255;

    MTL::CommandBuffer* commandBuffer = _metal.commandQueue->commandBuffer();
    MTL::BlitCommandEncoder* blit = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
    if (!commandBuffer || !blit)
    {
        staging->release();
        RecordDiagnostic(commandBuffer ? "failed to create the fallback-texture blit encoder"
                                       : "failed to create the fallback-texture command buffer");
        return false;
    }
    for (MTL::Texture*& texture : _fallbackWhite)
    {
        MTL::TextureDescriptor* descriptor =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 1, 1, false);
        if (!descriptor)
        {
            blit->endEncoding();
            staging->release();
            RecordDiagnostic("failed to create a fallback texture descriptor");
            return false;
        }
        descriptor->setStorageMode(MTL::StorageModePrivate);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        texture = _metal.device->newTexture(descriptor);
        if (!texture)
        {
            blit->endEncoding();
            staging->release();
            RecordDiagnostic("failed to create a fallback texture surface");
            return false;
        }
        blit->copyFromBuffer(staging, 0, layout.bytesPerRow, layout.bytesPerImage, MTL::Size::Make(1, 1, 1), texture, 0,
                             0, MTL::Origin::Make(0, 0, 0));
    }
    blit->endEncoding();
    AttachDiagnostics(commandBuffer);
    commandBuffer->commit();
    commandBuffer->waitUntilCompleted();
    staging->release();
    return commandBuffer->status() != MTL::CommandBufferStatusError;
}

bool EngineMetal::IsTextureHandleLive(std::uint32_t handle) const
{
    return _textureRegistry.Resolve(handle) != nullptr;
}

void EngineMetal::TextureDestroyed(Texture*) {}
} // namespace Poseidon
