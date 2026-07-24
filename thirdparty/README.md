# Third-Party Dependencies

Vendored libraries and headers included directly in the build.

| Directory | License | Description |
|-----------|---------|-------------|
| `glad/` | Generated | OpenGL 4.5 Core loader |
| `metal-cpp/` | Apache-2.0 | [Apple metal-cpp](https://developer.apple.com/metal/cpp/) macOS 15 / iOS 18 release |
| `renderdoc/` | MIT | RenderDoc in-application API header |
| `sse2neon/` | MIT | [sse2neon](https://github.com/DLTcollab/sse2neon) v1.9.1 |

Additional dependencies managed via **vcpkg** (see `vcpkg.json`):
catch2, cli11, stb, mimalloc, freetype, sdl3, openal-soft, opus, enkits, imgui, spdlog.
