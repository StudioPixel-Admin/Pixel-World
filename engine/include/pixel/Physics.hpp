#pragma once

#include "pixel/World.hpp"

namespace pixel {
struct Player {
    glm::vec3 position{}, velocity{}; // Feet position; camera is 1.65 m above it.
    bool grounded = false, flying = false;
};
struct MoveInput {
    glm::vec3 direction{};
    bool jump = false, sprint = false;
    float speed = 4.8f; // Set from C# movementSpeed; sprint adds a 1.77 multiplier.
};

// Call at a fixed simulation step (1/60 s); substeps prevent wall tunnelling.
void stepPlayer(Player& player, const MoveInput& input, const World& world, float dt);
}
