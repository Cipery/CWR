#pragma once

#ifdef __APPLE__

#include <mach-o/dyld.h>

#include <stdlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits.h>

namespace TestHelpers
{

inline bool GetExecutablePath(char* destination, std::size_t destinationSize)
{
    if (destination == nullptr || destinationSize == 0)
    {
        return false;
    }

    destination[0] = '\0';
    uint32_t pathSize = static_cast<uint32_t>(destinationSize);
    char* path = destination;
    char* allocatedPath = nullptr;
    if (_NSGetExecutablePath(path, &pathSize) != 0)
    {
        allocatedPath = static_cast<char*>(malloc(pathSize));
        path = allocatedPath;
        if (path == nullptr || _NSGetExecutablePath(path, &pathSize) != 0)
        {
            path = nullptr;
        }
    }

    char canonicalPath[PATH_MAX];
    const bool resolved = path != nullptr && realpath(path, canonicalPath) != nullptr;
    // A truncated path would silently point tests at the wrong directory — treat it as failure.
    const bool fits = resolved && std::strlen(canonicalPath) < destinationSize;
    if (fits)
    {
        std::strcpy(destination, canonicalPath);
    }

    free(allocatedPath);
    return fits;
}

} // namespace TestHelpers

#endif
