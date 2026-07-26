#pragma once

#include <cstdint>

namespace Poseidon::Metal
{
enum class VertexStage : std::uint8_t
{
    Screen,
    Transform,
    Shadow,
    ShadowDepthSolid,
    ShadowDepthAlpha,
    BlitScale,
    ImGui,
    Count
};

enum class FragmentStage : std::uint8_t
{
    Normal,
    Detail,
    Grass,
    Water,
    Shadow,
    Flat,
    ShadowDepthSolid,
    ShadowDepthAlpha,
    BlitScale,
    ImGui,
    Count
};

enum class PipelineBlend : std::uint8_t
{
    Opaque,
    AlphaBlend,
    Additive,
    Shadow,
    Count
};

enum class DepthMode : std::uint8_t
{
    Normal,
    ReadOnly,
    Disabled,
    Shadow,
    ClearDepthStencil,
    ColorOnlyClear,
    Count
};

enum class VertexLayout : std::uint8_t
{
    TLVertex,
    SVertex,
    ShadowDepthSolid,
    ShadowDepthAlpha,
    None,
    ImGui,
    Count
};

enum class AttachmentConfig : std::uint8_t
{
    FramePass,
    PresentPass,
    CascadePass,
    Count
};

struct PipelineKey
{
    std::uint32_t value = 0;

    static PipelineKey Make(VertexStage vs, FragmentStage ps, PipelineBlend blend, std::uint8_t colorWriteMask,
                            VertexLayout layout, AttachmentConfig attachments, std::uint8_t sampleCountLog2,
                            bool alphaToCoverage)
    {
        return {(static_cast<std::uint32_t>(vs) & 0x7u) | ((static_cast<std::uint32_t>(ps) & 0xfu) << 3) |
                ((static_cast<std::uint32_t>(blend) & 0x3u) << 7) |
                ((static_cast<std::uint32_t>(colorWriteMask) & 0xfu) << 9) |
                ((static_cast<std::uint32_t>(layout) & 0x7u) << 13) |
                ((static_cast<std::uint32_t>(attachments) & 0x3u) << 16) |
                ((static_cast<std::uint32_t>(sampleCountLog2) & 0x3u) << 18) |
                ((static_cast<std::uint32_t>(alphaToCoverage) & 0x1u) << 20)};
    }
};

static_assert(static_cast<unsigned>(VertexStage::Count) <= (1u << 3), "VS id exceeds PSO key");
static_assert(static_cast<unsigned>(FragmentStage::Count) <= (1u << 4), "PS id exceeds PSO key");
static_assert(static_cast<unsigned>(PipelineBlend::Count) <= (1u << 2), "blend exceeds PSO key");
static_assert(static_cast<unsigned>(VertexLayout::Count) <= (1u << 3), "layout exceeds PSO key");
static_assert(static_cast<unsigned>(AttachmentConfig::Count) <= (1u << 2), "attachments exceed PSO key");
} // namespace Poseidon::Metal
