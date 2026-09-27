#include "pixel/Physics.hpp"

#include <algorithm>
#include <cmath>

namespace pixel {
namespace {
constexpr float BodyRadius=.32f;
constexpr float StepHeight=.60f;

bool overlaps(const World& world,glm::vec3 feet) {
    // A compact kinematic capsule approximation: three rings and cap centers.
    // Collision samples the same continuous density as the terrain, so it also
    // works before a distant chunk mesh has finished streaming.
    constexpr glm::vec2 ring[8]={{1,0},{.7071f,.7071f},{0,1},{-.7071f,.7071f},{-1,0},{-.7071f,-.7071f},{0,-1},{.7071f,-.7071f}};
    for(float y:{.10f,.85f,1.60f}) {
        if(world.density(feet+glm::vec3(0,y,0))<-.035f) return true;
        const float r=y==.85f?BodyRadius:BodyRadius*.80f;
        for(const auto offset:ring)
            if(world.density(feet+glm::vec3(offset.x*r,y,offset.y*r))<-.035f) return true;
    }
    return false;
}
void horizontalMove(Player& player,const World& world,int axis,float distance,bool mayStep) {
    glm::vec3 target=player.position;target[axis]+=distance;
    if(!overlaps(world,target)) {player.position=target;return;}
    if(mayStep) {
        // Small ledges and slopes can be climbed only when the raised capsule
        // and its destination are clear; ceilings cannot be stepped through.
        for(float rise=.10f;rise<=StepHeight+.001f;rise+=.10f) {
            const glm::vec3 raised=player.position+glm::vec3(0,rise,0);
            if(overlaps(world,raised)) break;
            if(!overlaps(world,target+glm::vec3(0,rise,0))) {
                player.position=target+glm::vec3(0,rise,0);return;
            }
        }
    }
    // Sliding along one unobstructed axis keeps glancing contacts responsive.
    player.velocity[axis]=0;
}
}

void stepPlayer(Player& player,const MoveInput& input,const World& world,float dt) {
    if(!std::isfinite(dt)||dt<=0) return;
    dt=std::min(dt,.05f); // Never integrate an entire paused frame in one jump.
    if(!player.flying&&overlaps(world,player.position)) {
        // Sculpting can add solid inside the capsule, and flight can end inside
        // a hillside. Recover before a swept move: bisection requires a clear
        // starting position and otherwise leaves the player stuck forever.
        const glm::vec3 original=player.position;
        for(int attempt=1;attempt<=96;++attempt) {
            const float rise=attempt<=32?attempt*.20f:6.4f+(attempt-32)*1.0f;
            const glm::vec3 candidate=original+glm::vec3(0,rise,0);
            if(!overlaps(world,candidate)) {player.position=candidate;player.velocity={};player.grounded=false;break;}
        }
    }
    glm::vec3 direction=input.direction;
    if(!player.flying) direction.y=0;
    const float magnitude=glm::length(direction);
    if(magnitude>1) direction/=magnitude;
    const bool inWater=player.position.y+1.1f<SeaLevel&&world.height(player.position.x,player.position.z)<SeaLevel;
    const float baseSpeed=std::isfinite(input.speed)?std::clamp(input.speed,0.0f,25.0f):4.8f;
    // Managed gameplay supplies exhaustion, sprint and water speed policies.
    // Standalone callers may use sprint; do not reapply a swimming multiplier.
    const float speed=baseSpeed*(input.sprint?1.77f:1.0f)*(player.flying?2.6f:1.0f);
    const float blend=1.0f-std::exp(-(player.grounded||player.flying?15.0f:5.0f)*dt);
    player.velocity.x=glm::mix(player.velocity.x,direction.x*speed,blend);
    player.velocity.z=glm::mix(player.velocity.z,direction.z*speed,blend);
    if(player.flying) {
        player.velocity.y=glm::mix(player.velocity.y,direction.y*speed,blend);
        player.position+=player.velocity*dt;player.grounded=false;return;
    }
    if(input.jump&&player.grounded) {player.velocity.y=8.0f;player.grounded=false;}
    if(inWater&&input.jump) player.velocity.y=3.2f;
    player.velocity.y=std::max(player.velocity.y-(inWater?6.0f:24.0f)*dt,inWater?-3.0f:-35.0f);

    // Keep swept increments shorter than the capsule radius. Fixed-step input
    // plus these substeps prevents common fast-fall and thin-wall tunnelling.
    const int substeps=std::clamp(int(std::ceil(glm::length(player.velocity)*dt/.15f)),1,16);
    const float subDt=dt/float(substeps);
    for(int step=0;step<substeps;++step) {
        const bool groundedBefore=player.grounded;
        horizontalMove(player,world,0,player.velocity.x*subDt,groundedBefore);
        horizontalMove(player,world,2,player.velocity.z*subDt,groundedBefore);
        const float displacement=player.velocity.y*subDt;
        glm::vec3 target=player.position+glm::vec3(0,displacement,0);
        if(overlaps(world,target)) {
            float clear=0,blocked=1;
            for(int i=0;i<9;++i) {
                const float mid=(clear+blocked)*.5f;
                if(overlaps(world,player.position+glm::vec3(0,displacement*mid,0))) blocked=mid;else clear=mid;
            }
            player.position.y+=displacement*clear;
            player.grounded=displacement<=0;player.velocity.y=0;
        } else {
            player.position=target;
            player.grounded=player.velocity.y<=0&&overlaps(world,player.position-glm::vec3(0,.13f,0));
        }
        // Follow descending slopes while walking; jumping remains ballistic.
        if(groundedBefore&&!player.grounded&&player.velocity.y<=0) {
            for(float drop=.10f;drop<=.40f;drop+=.10f) {
                if(overlaps(world,player.position-glm::vec3(0,drop,0))) {
                    player.position.y-=drop-.10f;player.grounded=true;player.velocity.y=0;break;
                }
            }
        }
    }
}
}
