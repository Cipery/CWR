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
        50,
        &CreateMetalBackend,
        &IsMetalAvailable,
    });
}
} // namespace Poseidon
