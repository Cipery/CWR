#pragma once

// metal-cpp reaches objc/runtime.h, whose BOOL conflicts with Poseidon's
// legacy BOOL typedef when Poseidon headers have already been parsed. Keep
// this umbrella as the first include in every metal-cpp translation unit.
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
