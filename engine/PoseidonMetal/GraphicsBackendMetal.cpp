#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>

namespace
{
Poseidon::Engine* CreateMetalBackend(const Poseidon::GraphicsEngineParams& params)
{
    return Poseidon::CreateEngineMetal(params.width, params.height, params.useWindow, params.bitsPerPixel);
}

bool IsMetalAvailable()
{
    static const bool available = []()
    {
        MTL::Device* device = MTL::CreateSystemDefaultDevice();
        const bool found = device != nullptr;
        if (device)
            device->release();
        return found;
    }();
    return available;
}
} // namespace

namespace Poseidon
{
void RegisterMetalGraphicsBackend()
{
    GraphicsEngineFactory::Register(GraphicsBackendDescriptor{
        "metal",
        "Metal 3 (SDL3)",
        // M6 parity+perf gate passed (A/B suite equivalent to GL33; 9.1x
        // faster mission frame times on Apple Silicon) — Metal outranks
        // GL33 (100) so Auto selects it; GL33 stays available via --render.
        200,
        &CreateMetalBackend,
        &IsMetalAvailable,
    });
}
} // namespace Poseidon
