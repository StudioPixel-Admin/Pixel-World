#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace pixel {
// Shared CPU/GPU vertex layout. Material chooses a procedural surface in the shader.
struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0, 1, 0};
    glm::vec3 color{1};
    float material = 0;
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};
struct ChunkKey {
    int x = 0, z = 0;
    bool operator==(const ChunkKey&) const = default;
};
struct ChunkKeyHash {
    size_t operator()(ChunkKey p) const noexcept {
        return static_cast<size_t>(static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32U |
                                   static_cast<uint32_t>(p.z));
    }
};
struct ChunkMesh {
    ChunkKey key{};
    uint64_t revision = 0;
    Mesh mesh;
    glm::vec3 min{}, max{};
};
struct TerrainEdit {
    glm::vec3 center{};
    float radius = 2.5f;
    float strength = 4.0f; // Positive removes solid; negative adds it.
};
struct Camera {
    glm::vec3 position{0, 45, 0};
    float yaw = -90, pitch = -15, fov = 72;
};
}
