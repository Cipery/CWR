#pragma once

#include <PoseidonMetal/MetalFwd.hpp>

#include <SDL3/SDL_metal.h>

struct MetalContext
{
    MTL::Device* device = nullptr;             // Owned.
    MTL::CommandQueue* commandQueue = nullptr; // Owned.
    MTL::Library* shaderLibrary = nullptr;     // Owned.

    SDL_MetalView view = nullptr;
    CA::MetalLayer* layer = nullptr; // Borrowed from view; never release.
};
