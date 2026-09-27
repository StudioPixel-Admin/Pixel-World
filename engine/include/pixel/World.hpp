#pragma once

#include "pixel/Types.hpp"
#include <array>
#include <memory>
#include <unordered_map>

namespace pixel {
inline constexpr int ChunkSize = 24;
inline constexpr float TerrainCellSize = 2.0f;
inline constexpr float SeaLevel = 13.0f;

// Owns deterministic terrain and a bounded background meshing queue. Public
// methods belong to the game thread; worker jobs read immutable edit snapshots.
class World {
public:
    explicit World(uint32_t seed);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void update(glm::vec3 camera, int radius, int uploadBudget = 2);
    const std::unordered_map<ChunkKey, ChunkMesh, ChunkKeyHash>& chunks() const;
    float density(glm::vec3 point) const; // Positive = air; negative = solid.
    float height(float x, float z) const;
    bool raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance, glm::vec3& hit) const;
    void edit(const TerrainEdit& edit);
    void setEdits(const std::vector<TerrainEdit>& edits);
    std::vector<TerrainEdit> edits() const;
    int pendingJobs() const;
    uint32_t seed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace detail {
// Exposed for invariant tests. Corners: 000,100,110,010,001,101,111,011.
// This walks contours on cube faces, never decomposing a cube into tetrahedra.
void polygonizeCube(Mesh& mesh, const std::array<glm::vec3, 8>& corners,
                    const std::array<float, 8>& values, glm::vec3 color = glm::vec3(0.5f));
}
}
