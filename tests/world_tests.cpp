#include "pixel/Physics.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
bool near(float a,float b,float tolerance=.0001f) {return std::abs(a-b)<tolerance;}
void waitForChunk(pixel::World& world,pixel::ChunkKey key,glm::vec3 position,int radius=1) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!world.chunks().contains(key)&&std::chrono::steady_clock::now()<deadline) {
        world.update(position,radius,2);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(world.chunks().contains(key),"background meshing did not finish");
}
}

int main() {
    try {
        pixel::World a(12345),b(12345),different(9917);
        bool seedChangesHeight=false;
        for(int x=-60;x<=60;x+=7) for(int z=-60;z<=60;z+=9) {
            check(near(a.height(float(x),float(z)),b.height(float(x),float(z))),"seeded heights are not deterministic");
            check(near(a.density({float(x),12,float(z)}),b.density({float(x),12,float(z)})),"seeded density is not deterministic");
            seedChangesHeight|=!near(a.height(float(x),float(z)),different.height(float(x),float(z)),.01f);
        }
        check(seedChangesHeight,"different seeds produced identical terrain");
        // The same global coordinate is reached from both chunk origins,
        // including the negative-coordinate boundary where truncation fails.
        for(int boundary:{-48,-24,0,24,48}) for(int y=-8;y<70;y+=2) {
            const glm::vec3 fromLeft(float(boundary-24)+24,float(y),-11);
            const glm::vec3 fromRight(float(boundary),float(y),-11);
            check(near(a.density(fromLeft),a.density(fromRight)),"chunk boundary density mismatch");
        }
        std::array<glm::vec3,8> corners{{{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}}};
        std::array<float,8> values{};
        for(int i=0;i<8;++i) values[i]=corners[i].y-.4f;
        pixel::Mesh plane;
        pixel::detail::polygonizeCube(plane,corners,values);
        check(!plane.indices.empty(),"marching cubes missed a plane");
        float area=0;
        for(size_t i=0;i<plane.indices.size();i+=3) {
            const auto& p=plane.vertices[plane.indices[i]];
            const auto& q=plane.vertices[plane.indices[i+1]];
            const auto& r=plane.vertices[plane.indices[i+2]];
            const glm::vec3 cross=glm::cross(q.position-p.position,r.position-p.position);
            check(glm::length(cross)>1e-6f,"marching cubes emitted an empty triangle");
            check(cross.y>0&&p.normal.y>.99f,"terrain winding points into solid");
            check(near(p.position.y,.4f),"marching cubes edge interpolation is incorrect");
            area+=glm::length(cross)*.5f;
        }
        check(near(area,1.0f),"marching cubes plane has gaps or overlap");
        for(unsigned mask=0;mask<256;++mask) {
            pixel::Mesh mesh;
            for(int i=0;i<8;++i) values[i]=(mask&(1U<<i))?-1.0f:1.0f;
            pixel::detail::polygonizeCube(mesh,corners,values);
            check((mask==0||mask==255)==mesh.indices.empty(),"marching cubes case failed to produce a surface");
            for(const auto& vertex:mesh.vertices) check(std::isfinite(vertex.normal.x)&&std::isfinite(vertex.normal.y)&&std::isfinite(vertex.normal.z),"invalid marching-cubes normal");
        }
        glm::vec3 hit;
        const float surface=a.height(3,5);
        check(a.raycast({3,surface+10,5},{0,-1,0},20,hit),"raycast missed terrain");
        check(near(hit.y,surface,.01f),"raycast did not converge to terrain surface");
        check(!a.raycast({3,surface+10,5},{0,0,0},20,hit),"zero-direction ray was accepted");
        const glm::vec3 dig(-24,a.height(-24,0)-1,0);
        const float before=a.density(dig);
        a.edit({dig,3,12});
        check(a.density(dig)>before+11.9f,"terrain edit did not change density");
        b.setEdits(a.edits());
        check(near(a.density(dig),b.density(dig)),"restored edits changed density");
        b.setEdits({});check(near(b.density(dig),before),"clearing edits failed");

        pixel::Player player;
        glm::vec2 dry(3,5);
        for(int x=0;x<1000&&b.height(dry.x,dry.y)<pixel::SeaLevel+3;x+=25) dry={float(x),float(x)};
        const float drySurface=b.height(dry.x,dry.y);
        check(drySurface>pixel::SeaLevel+3,"test did not find dry ground");
        player.position={dry.x,drySurface+5,dry.y};
        for(int i=0;i<240;++i) pixel::stepPlayer(player,{},b,1.0f/60);
        check(player.grounded,"falling player never became grounded");
        check(std::abs(player.position.y-drySurface)<.6f,"player fell through terrain");
        const float restingY=player.position.y;
        pixel::stepPlayer(player,{{},true,false},b,1.0f/60);
        check(player.velocity.y>0&&!player.grounded,"grounded jump failed");
        for(int i=0;i<20;++i) pixel::stepPlayer(player,{},b,1.0f/60);
        check(player.position.y>restingY+1,"jump did not lift player");
        player.flying=true;
        for(int i=0;i<60;++i) pixel::stepPlayer(player,{{0,1,0},false,false},b,1.0f/60);
        check(player.position.y>restingY+5,"flight input did not move vertically");

        waitForChunk(a,{-1,0},{-1,40,1});
        check(a.pendingJobs()<=32,"background job queue exceeded its bound");
        const auto originalRevision=a.chunks().at({-1,0}).revision;
        a.edit({{-1,a.height(-1,1),1},3,8});
        check(!a.chunks().contains({-1,0}),"edit did not invalidate affected chunk");
        waitForChunk(a,{-1,0},{-1,40,1});
        check(a.chunks().at({-1,0}).revision>originalRevision,"stale chunk revision was published");
        a.update({2400,40,2400},1,1);
        check(!a.chunks().contains({-1,0}),"distant chunk was not unloaded");
        check(a.pendingJobs()<=32,"stale generation jobs were not cancelled");
        std::cout<<"World tests passed: determinism, seams, 256 cube cases, winding, edits, physics and streaming.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"World test failed: "<<error.what()<<'\n';return 1;
    }
}
