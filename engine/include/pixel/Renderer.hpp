#pragma once

#include "pixel/Types.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

struct GLFWwindow;

namespace pixel {
struct RenderStats {
    // Terrain counters only. gpuBytes is allocated live + retiring chunk-buffer
    // memory; it excludes swapchain/depth/UI/driver allocations and is not VRAM usage.
    uint64_t drawCalls = 0, triangles = 0, residentChunks = 0, visibleChunks = 0, gpuBytes = 0;
};

// Vulkan owns the ImGui context. Build widgets between beginUi() and render();
// render() calls ImGui::Render(), records the scene and UI, then presents it.
class Renderer {
public:
    Renderer(GLFWwindow* window, const std::filesystem::path& assets);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void beginUi();
    void render(const Camera& camera,
                const std::unordered_map<ChunkKey, ChunkMesh, ChunkKeyHash>& chunks,
                float timeOfDay, float fogDistance, bool vsync);
    RenderStats stats() const;
    const std::string& deviceName() const;
    void waitIdle();
    // Call after selecting another world, whose mesh revisions may repeat.
    void invalidateChunks();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
