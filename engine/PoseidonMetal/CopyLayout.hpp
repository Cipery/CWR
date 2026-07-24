#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Poseidon::Metal
{
enum class CopyFormat : std::uint8_t
{
    RGBA8,
    BGRA8,
    RG8,
    BC1,
    BC2,
    BC3,
    Depth32Float
};

struct CopyLayout
{
    std::size_t tightBytesPerRow = 0;
    std::size_t bytesPerRow = 0;
    std::size_t rowCount = 0;
    std::size_t bytesPerImage = 0;

    static constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment)
    {
        return alignment ? (value + alignment - 1) & ~(alignment - 1) : value;
    }

    static CopyLayout Compute(CopyFormat format, int width, int height, std::size_t rowAlignment = 256)
    {
        const std::size_t w = static_cast<std::size_t>(std::max(width, 1));
        const std::size_t h = static_cast<std::size_t>(std::max(height, 1));
        std::size_t tight = 0;
        std::size_t rows = h;
        switch (format)
        {
            case CopyFormat::RGBA8:
            case CopyFormat::BGRA8:
            case CopyFormat::Depth32Float:
                tight = w * 4;
                break;
            case CopyFormat::RG8:
                tight = w * 2;
                break;
            case CopyFormat::BC1:
                tight = ((w + 3) / 4) * 8;
                rows = (h + 3) / 4;
                break;
            case CopyFormat::BC2:
            case CopyFormat::BC3:
                tight = ((w + 3) / 4) * 16;
                rows = (h + 3) / 4;
                break;
        }
        const std::size_t pitch = AlignUp(tight, rowAlignment);
        return {tight, pitch, rows, pitch * rows};
    }

    static constexpr int MipDimension(int base, int level)
    {
        return std::max(base >> level, 1);
    }

    static std::size_t AppendMip(std::size_t currentOffset, CopyFormat format, int width, int height,
                                 std::size_t offsetAlignment = 256)
    {
        const CopyLayout layout = Compute(format, width, height);
        return AlignUp(currentOffset, offsetAlignment) + layout.bytesPerImage;
    }
};
} // namespace Poseidon::Metal
