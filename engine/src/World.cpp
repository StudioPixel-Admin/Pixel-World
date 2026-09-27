#include "pixel/World.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace pixel {
namespace {
constexpr int Cells = ChunkSize / int(TerrainCellSize);
constexpr int MinY = -12, MaxY = 80;
constexpr int VerticalCells = (MaxY - MinY) / int(TerrainCellSize);
constexpr size_t MaxPendingJobs = 32;
constexpr std::array<glm::ivec3, 8> CornerOffsets{{
    {0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}
}};
constexpr int Edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
constexpr int FaceCorners[6][4] = {{0,1,2,3},{4,5,6,7},{0,1,5,4},{3,2,6,7},{0,3,7,4},{1,2,6,5}};
constexpr int FaceEdges[6][4] = {{0,1,2,3},{4,5,6,7},{0,9,4,8},{2,10,6,11},{3,11,7,8},{1,10,5,9}};

float smooth(float value) { return value * value * (3.0f - 2.0f * value); }
uint32_t hash(uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352dU;
    value ^= value >> 15; value *= 0x846ca68bU;
    return value ^ (value >> 16);
}
float randomAt(int x, int y, int z, uint32_t seed) {
    // Unsigned arithmetic has defined overflow, including negative world cells.
    const uint32_t value = hash(uint32_t(x) * 0x8da6b343U ^ uint32_t(y) * 0xd8163841U ^
                                uint32_t(z) * 0xcb1ab31fU ^ seed);
    return float(value & 0x00ffffffU) / float(0x01000000U);
}
float noise(glm::vec3 p, uint32_t seed) {
    const glm::ivec3 cell(glm::floor(p));
    const glm::vec3 f = p - glm::vec3(cell);
    const glm::vec3 t(smooth(f.x), smooth(f.y), smooth(f.z));
    float layer[2]{};
    for (int z = 0; z < 2; ++z) {
        const float a = glm::mix(randomAt(cell.x,cell.y,cell.z+z,seed), randomAt(cell.x+1,cell.y,cell.z+z,seed), t.x);
        const float b = glm::mix(randomAt(cell.x,cell.y+1,cell.z+z,seed), randomAt(cell.x+1,cell.y+1,cell.z+z,seed), t.x);
        layer[z] = glm::mix(a,b,t.y);
    }
    return glm::mix(layer[0],layer[1],t.z) * 2.0f - 1.0f;
}
float terrainHeight(float x, float z, uint32_t seed) {
    const float continental = noise({x * .007f, 0, z * .007f}, seed);
    const float hills = noise({x * .023f, 17, z * .023f}, seed ^ 0x6713U);
    const float detail = noise({x * .066f, 41, z * .066f}, seed ^ 0x5f31U);
    const float h = 22.0f + continental * 15.0f + hills * 8.0f + detail * 2.0f;
    // A small smooth terrace component keeps the sculpted voxel character while
    // preserving continuous heights at cell and chunk boundaries.
    const float band = h / 3.0f, base = std::floor(band), fraction = band - base;
    return glm::mix(h, (base + smooth(fraction)) * 3.0f, .32f);
}
float sampleDensity(glm::vec3 p, uint32_t seed, const std::vector<TerrainEdit>& edits) {
    const float h = terrainHeight(p.x,p.z,seed);
    float value = p.y - h;
    if (p.y > 1 && p.y < h - 5) {
        const float cave = noise(p * glm::vec3(.046f,.065f,.046f) + glm::vec3(91,12,33), seed ^ 0xaa117U);
        const float caveGate = std::min({1.0f, (p.y-1) * .3f, (h-5-p.y) * .25f});
        // Thin noise ridges become underground passages; the gate closes them
        // before reaching bedrock or the surface. Digging can reveal the caves.
        value = std::max(value, (.10f - std::abs(cave)) * 19.0f - (1.0f-caveGate) * 9.0f);
    }
    for (const auto& edit : edits) {
        const glm::vec3 delta = p-edit.center;
        const float d2 = glm::dot(delta,delta), r2 = edit.radius*edit.radius;
        if (d2 < r2) {
            const float falloff = 1.0f - d2/r2;
            value += edit.strength * falloff * falloff;
        }
    }
    return value;
}
glm::vec3 gradient(const std::array<float,8>& v, glm::vec3 t) {
    // Analytic gradient of this cube's trilinear scalar interpolant.
    return {
        glm::mix(glm::mix(v[1]-v[0],v[2]-v[3],t.y),glm::mix(v[5]-v[4],v[6]-v[7],t.y),t.z),
        glm::mix(glm::mix(v[3]-v[0],v[2]-v[1],t.x),glm::mix(v[7]-v[4],v[6]-v[5],t.x),t.z),
        glm::mix(glm::mix(v[4]-v[0],v[5]-v[1],t.x),glm::mix(v[7]-v[3],v[6]-v[2],t.x),t.y)
    };
}
void triangle(Mesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 color, float material,
              glm::vec3 outward = glm::vec3(0), float smoothMix = 0.0f) {
    glm::vec3 n = glm::cross(b-a,c-a);
    const float n2 = glm::dot(n,n);
    if (n2 < 1e-10f) return;
    if (glm::dot(n,outward) < 0) { std::swap(b,c); n = -n; }
    n /= std::sqrt(n2);
    if (glm::dot(outward,outward) > 1e-10f)
        n = glm::normalize(glm::mix(n,glm::normalize(outward),smoothMix));
    const auto index = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({a,n,color,material});
    mesh.vertices.push_back({b,n,color,material});
    mesh.vertices.push_back({c,n,color,material});
    mesh.indices.insert(mesh.indices.end(),{index,index+1,index+2});
}
void terrainAppearance(Vertex& vertex, uint32_t seed) {
    const glm::vec3 p=vertex.position,n=vertex.normal;
    const float moisture = noise({p.x*.013f,31,p.z*.013f},seed ^ 0x3921U);
    vertex.material=0;
    vertex.color=glm::mix(glm::vec3(1.06f,1.00f,.89f),glm::vec3(.91f,1.04f,.95f),glm::clamp(moisture*.65f+.5f,0.0f,1.0f));
    if(n.y<.66f) {vertex.material=1;vertex.color=glm::vec3(.95f);}
    else if(p.y<SeaLevel+1.1f) {vertex.material=2;vertex.color=glm::vec3(1.0f);}
    else if(p.y>36.0f) {vertex.material=3;vertex.color=glm::vec3(1.0f);}
}
void cone(Mesh& mesh, glm::vec3 base, float radius, float height, glm::vec3 color, float material) {
    constexpr int sides = 7;
    const glm::vec3 apex = base + glm::vec3(0,height,0);
    for (int i=0;i<sides;++i) {
        const float a = float(i)*6.2831853f/sides, b = float(i+1)*6.2831853f/sides;
        glm::vec3 p = base + glm::vec3(std::cos(a)*radius,0,std::sin(a)*radius);
        glm::vec3 q = base + glm::vec3(std::cos(b)*radius,0,std::sin(b)*radius);
        triangle(mesh,p,apex,q,color,material,(p+q)*.5f-base);
        triangle(mesh,base,p,q,color*.75f,material,{0,-1,0});
    }
}
void trunk(Mesh& mesh, glm::vec3 base, float height) {
    constexpr int sides = 5;
    for (int i=0;i<sides;++i) {
        const float a = float(i)*6.2831853f/sides, b = float(i+1)*6.2831853f/sides;
        glm::vec3 p=base+glm::vec3(std::cos(a)*.25f,0,std::sin(a)*.25f), q=base+glm::vec3(std::cos(b)*.25f,0,std::sin(b)*.25f);
        const glm::vec3 up(0,height,0), out=(p+q)*.5f-base;
        triangle(mesh,p,p+up,q+up,{.94f,.96f,.94f},4,out);
        triangle(mesh,p,q+up,q,{.94f,.96f,.94f},4,out);
    }
}
ChunkMesh generateChunk(ChunkKey key, uint64_t revision, uint32_t seed,
                        const std::vector<TerrainEdit>& edits, const std::atomic<bool>& cancelled) {
    ChunkMesh result;
    result.key=key; result.revision=revision;
    result.min={float(key.x*ChunkSize),float(MinY),float(key.z*ChunkSize)};
    result.max=result.min+glm::vec3(ChunkSize,MaxY-MinY,ChunkSize);
    auto& mesh=result.mesh;
    mesh.vertices.reserve(14000); mesh.indices.reserve(14000);
    constexpr int Stride=Cells+1;
    std::vector<float> samples(Stride*Stride*(VerticalCells+1));
    const auto index=[](int x,int y,int z){ return (y*Stride+z)*Stride+x; };
    for (int y=0;y<=VerticalCells;++y) {
        if (cancelled.load(std::memory_order_relaxed)) return result;
        for (int z=0;z<=Cells;++z) for(int x=0;x<=Cells;++x)
            samples[index(x,y,z)] = sampleDensity(result.min+glm::vec3(x,y,z)*TerrainCellSize,seed,edits);
    }
    for(int y=0;y<VerticalCells;++y) {
        if(cancelled.load(std::memory_order_relaxed)) return result;
        for(int z=0;z<Cells;++z) for(int x=0;x<Cells;++x) {
            std::array<glm::vec3,8> corners;
            std::array<float,8> values;
            for(int c=0;c<8;++c) {
                const auto offset=CornerOffsets[c]+glm::ivec3(x,y,z);
                corners[c]=result.min+glm::vec3(offset)*TerrainCellSize;
                values[c]=samples[index(offset.x,offset.y,offset.z)];
            }
            const size_t start=mesh.vertices.size();
            detail::polygonizeCube(mesh,corners,values);
            for(size_t i=start;i<mesh.vertices.size();++i)
                terrainAppearance(mesh.vertices[i],seed);
        }
    }
    // Water is tiled so land can naturally occlude the shoreline. The shader
    // gives material 6 a subtle moving highlight without requiring a texture.
    for(int z=0;z<Cells;++z) for(int x=0;x<Cells;++x) {
        const glm::vec3 a(result.min.x+x*TerrainCellSize,SeaLevel,result.min.z+z*TerrainCellSize);
        const glm::vec3 b=a+glm::vec3(TerrainCellSize,0,0),c=a+glm::vec3(TerrainCellSize,0,TerrainCellSize),d=a+glm::vec3(0,0,TerrainCellSize);
        if(std::min({terrainHeight(a.x,a.z,seed),terrainHeight(b.x,b.z,seed),terrainHeight(c.x,c.z,seed),terrainHeight(d.x,d.z,seed)}) < SeaLevel+.5f) {
            triangle(mesh,a,d,c,{.95f,1.02f,1.02f},6,{0,1,0});
            triangle(mesh,a,c,b,{.95f,1.02f,1.02f},6,{0,1,0});
        }
    }
    // Sparse, deterministic decoration stays in the owning chunk's mesh: no
    // object draw call or per-frame allocation for every tree and boulder.
    for(int z=0;z<3;++z) for(int x=0;x<3;++x) {
        const int gx=key.x*3+x,gz=key.z*3+z;
        const float chance=randomAt(gx,71,gz,seed);
        const float px=result.min.x+x*8+1.8f+randomAt(gx,21,gz,seed)*4.4f;
        const float pz=result.min.z+z*8+1.8f+randomAt(gx,22,gz,seed)*4.4f;
        const float py=terrainHeight(px,pz,seed);
        if(py<SeaLevel+1.5f || py>37 || std::abs(terrainHeight(px+1,pz,seed)-py)>.8f || std::abs(terrainHeight(px,pz+1,seed)-py)>.8f) continue;
        // Hide decorations when their foundation has been dug away.
        if(sampleDensity({px,py-.5f,pz},seed,edits)>0) continue;
        const glm::vec3 base(px,py-.15f,pz);
        if(chance<.50f) {
            const float h=4.5f+randomAt(gx,12,gz,seed)*3.5f;
            trunk(mesh,base,h*.7f);
            const glm::vec3 green=glm::mix(glm::vec3(.91f,.94f,.95f),glm::vec3(1.06f,1.04f,.91f),randomAt(gx,13,gz,seed));
            cone(mesh,base+glm::vec3(0,h*.25f,0),h*.31f,h*.61f,green,5);
            cone(mesh,base+glm::vec3(0,h*.53f,0),h*.23f,h*.55f,green*1.03f,5);
        } else if(chance>.85f) {
            cone(mesh,base,1.0f+chance*.6f,.8f+randomAt(gx,10,gz,seed),{.97f,.99f,.96f},1);
        }
    }
    return result;
}
bool validEdit(const TerrainEdit& edit) {
    return std::isfinite(edit.center.x)&&std::isfinite(edit.center.y)&&std::isfinite(edit.center.z)&&
        std::isfinite(edit.radius)&&std::isfinite(edit.strength)&&edit.radius>=.1f&&edit.radius<=32&&std::abs(edit.strength)<=64;
}
bool editTouches(const TerrainEdit& edit,ChunkKey key) {
    const glm::vec2 lo(key.x*ChunkSize-TerrainCellSize,key.z*ChunkSize-TerrainCellSize);
    const glm::vec2 hi=lo+glm::vec2(ChunkSize+2*TerrainCellSize);
    const glm::vec2 p(edit.center.x,edit.center.z),d=p-glm::clamp(p,lo,hi);
    return glm::dot(d,d)<=edit.radius*edit.radius;
}
ChunkKey chunkAt(glm::vec3 point) {return {int(std::floor(point.x/ChunkSize)),int(std::floor(point.z/ChunkSize))};}
using EditBins=std::unordered_map<ChunkKey,std::shared_ptr<const std::vector<TerrainEdit>>,ChunkKeyHash>;
void indexEdit(EditBins& bins,const TerrainEdit& edit) {
    const int minX=int(std::floor((edit.center.x-edit.radius-TerrainCellSize)/ChunkSize));
    const int maxX=int(std::floor((edit.center.x+edit.radius+TerrainCellSize)/ChunkSize));
    const int minZ=int(std::floor((edit.center.z-edit.radius-TerrainCellSize)/ChunkSize));
    const int maxZ=int(std::floor((edit.center.z+edit.radius+TerrainCellSize)/ChunkSize));
    for(int z=minZ;z<=maxZ;++z) for(int x=minX;x<=maxX;++x) {
        const ChunkKey key{x,z};if(!editTouches(edit,key)) continue;
        const auto found=bins.find(key);
        auto local=found==bins.end()?std::make_shared<std::vector<TerrainEdit>>():std::make_shared<std::vector<TerrainEdit>>(*found->second);
        local->push_back(edit);bins.insert_or_assign(key,std::move(local));
    }
}
int distanceSquared(ChunkKey a,ChunkKey b) { const int x=a.x-b.x,z=a.z-b.z;return x*x+z*z; }
}

namespace detail {
void polygonizeCube(Mesh& mesh,const std::array<glm::vec3,8>& corners,const std::array<float,8>& values,glm::vec3 color) {
    // Original table-free implementation of marching cubes. As in Lorensen &
    // Cline (1987), crossings are interpolated on the twelve CUBE edges.
    // Reference: https://graphics.stanford.edu/courses/cs348a-21-winter/Papers/Marching_Cubes.pdf
    // Each face connects its crossings; closed edge contours define the cell's
    // polygons. A face-center decision is shared by both neighboring cubes,
    // avoiding cracks in the ambiguous checkerboard face cases. This is not
    // MC33 and does not claim topological guarantees for interior ambiguities.
    std::array<glm::vec3,12> points{};
    std::array<bool,12> crosses{},visited{};
    int links[12][2]{};
    std::array<int,12> degree{};
    int count=0;
    for(int e=0;e<12;++e) {
        const int a=Edges[e][0],b=Edges[e][1];
        crosses[e]=(values[a]<0)!=(values[b]<0);
        if(crosses[e]) {
            const float t=glm::clamp(values[a]/(values[a]-values[b]),0.0f,1.0f);
            points[e]=glm::mix(corners[a],corners[b],t); ++count;
        }
    }
    if(count<3) return;
    const auto connect=[&](int a,int b) {
        if(degree[a]<2&&degree[b]<2) {links[a][degree[a]++]=b;links[b][degree[b]++]=a;}
    };
    for(int face=0;face<6;++face) {
        int active[4]{},n=0;
        for(int i=0;i<4;++i) if(crosses[FaceEdges[face][i]]) active[n++]=FaceEdges[face][i];
        if(n==2) connect(active[0],active[1]);
        else if(n==4) {
            float center=0;for(int i=0;i<4;++i) center+=values[FaceCorners[face][i]];
            for(int i=0;i<4;++i) {
                const int next=(i+1)%4;
                if((values[FaceCorners[face][next]]<0)!=(center<0))
                    connect(FaceEdges[face][i],FaceEdges[face][next]);
            }
        }
    }
    const glm::vec3 size=corners[6]-corners[0];
    for(int start=0;start<12;++start) {
        if(!crosses[start]||visited[start]||degree[start]!=2) continue;
        std::array<glm::vec3,12> polygon{};
        int n=0,current=start,previous=-1;
        do {
            if(visited[current]||degree[current]!=2||n>=12) break;
            visited[current]=true;polygon[n++]=points[current];
            const int next=links[current][0]==previous?links[current][1]:links[current][0];
            previous=current;current=next;
        } while(current!=start);
        if(current!=start||n<3) continue;
        const auto emit=[&](glm::vec3 a,glm::vec3 b,glm::vec3 c) {
            const glm::vec3 mid=(a+b+c)/3.0f;
            const glm::vec3 normal=gradient(values,glm::clamp((mid-corners[0])/size,glm::vec3(0),glm::vec3(1)))/size;
            triangle(mesh,a,b,c,color,0,normal,.28f);
        };
        if(n==3) emit(polygon[0],polygon[1],polygon[2]);
        else {
            // The centroid remains local to the cube and preserves every face
            // segment, so triangles from adjacent chunks meet exactly.
            glm::vec3 center(0);for(int i=0;i<n;++i) center+=polygon[i];center/=float(n);
            for(int i=0;i<n;++i) emit(center,polygon[i],polygon[(i+1)%n]);
        }
    }
}
}

struct World::Impl {
    struct Job {
        ChunkKey key{};uint64_t revision=0;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::shared_ptr<const std::vector<TerrainEdit>> snapshot;
    };
    uint32_t seed;
    uint64_t nextRevision=1;
    std::unordered_map<ChunkKey,ChunkMesh,ChunkKeyHash> chunks;
    std::unordered_map<ChunkKey,Job,ChunkKeyHash> pending;
    std::shared_ptr<const std::vector<TerrainEdit>> edits=std::make_shared<const std::vector<TerrainEdit>>();
    const std::shared_ptr<const std::vector<TerrainEdit>> noEdits=std::make_shared<const std::vector<TerrainEdit>>();
    EditBins editBins;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Job> jobs;
    std::deque<ChunkMesh> completed;
    std::vector<std::thread> workers;
    bool stopping=false;

    explicit Impl(uint32_t s):seed(s) {
        const unsigned cores=std::thread::hardware_concurrency();
        const unsigned workerCount=cores>=4?2:1;
        for(unsigned i=0;i<workerCount;++i) workers.emplace_back([this] {
            for(;;) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock,[this]{return stopping||!jobs.empty();});
                    if(stopping) return;
                    job=std::move(jobs.front());jobs.pop_front();
                }
                if(job.cancelled->load(std::memory_order_relaxed)) continue;
                auto mesh=generateChunk(job.key,job.revision,seed,*job.snapshot,*job.cancelled);
                std::lock_guard lock(mutex);
                if(!stopping&&!job.cancelled->load(std::memory_order_relaxed)) completed.push_back(std::move(mesh));
            }
        });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);stopping=true;
            for(auto& [key,job]:pending) job.cancelled->store(true,std::memory_order_relaxed);
            jobs.clear();
        }
        ready.notify_all();
        for(auto& worker:workers) worker.join();
    }
};

World::World(uint32_t seed):impl_(std::make_unique<Impl>(seed)) {}
World::~World()=default;
const std::unordered_map<ChunkKey,ChunkMesh,ChunkKeyHash>& World::chunks() const {return impl_->chunks;}
uint32_t World::seed() const {return impl_->seed;}
float World::height(float x,float z) const {return terrainHeight(x,z,impl_->seed);}
float World::density(glm::vec3 p) const {
    // Public calls are on the simulation thread; the shared pointer is also
    // copied under the queue lock so collision/raycast snapshots stay coherent.
    std::shared_ptr<const std::vector<TerrainEdit>> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found=impl_->editBins.find(chunkAt(p));
        snapshot=found==impl_->editBins.end()?impl_->noEdits:found->second;
    }
    return sampleDensity(p,impl_->seed,*snapshot);
}
void World::update(glm::vec3 camera,int radius,int uploadBudget) {
    radius=std::clamp(radius,1,8);uploadBudget=std::clamp(uploadBudget,0,8);
    const ChunkKey center{int(std::floor(camera.x/ChunkSize)),int(std::floor(camera.z/ChunkSize))};
    const int keepDistance=(radius+1)*(radius+1)+1;
    std::lock_guard lock(impl_->mutex);
    for(auto it=impl_->chunks.begin();it!=impl_->chunks.end();)
        if(distanceSquared(it->first,center)>keepDistance) it=impl_->chunks.erase(it);else ++it;
    for(auto it=impl_->pending.begin();it!=impl_->pending.end();)
        if(distanceSquared(it->first,center)>keepDistance) {it->second.cancelled->store(true,std::memory_order_relaxed);it=impl_->pending.erase(it);}else ++it;
    std::erase_if(impl_->jobs,[](const auto& job){return job.cancelled->load(std::memory_order_relaxed);});
    // Dropped or edited chunks may already have finished. Prune those results
    // even with a zero publication budget, keeping teleport/edit bursts bounded.
    std::erase_if(impl_->completed,[&](const auto& mesh){const auto found=impl_->pending.find(mesh.key);return found==impl_->pending.end()||found->second.revision!=mesh.revision;});
    for(int budget=0;budget<uploadBudget&&!impl_->completed.empty();) {
        auto mesh=std::move(impl_->completed.front());impl_->completed.pop_front();
        const auto found=impl_->pending.find(mesh.key);
        if(found==impl_->pending.end()||found->second.revision!=mesh.revision) continue;
        impl_->chunks.insert_or_assign(mesh.key,std::move(mesh));impl_->pending.erase(found);++budget;
    }
    std::vector<ChunkKey> candidates;
    for(int z=-radius;z<=radius;++z) for(int x=-radius;x<=radius;++x) {
        const ChunkKey key{center.x+x,center.z+z};
        if(x*x+z*z<=radius*radius+1&&!impl_->chunks.contains(key)&&!impl_->pending.contains(key)) candidates.push_back(key);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[&](auto a,auto b){return distanceSquared(a,center)<distanceSquared(b,center);});
    for(const auto key:candidates) {
        if(impl_->pending.size()>=MaxPendingJobs) break;
        const auto found=impl_->editBins.find(key);
        Impl::Job job{key,impl_->nextRevision++,std::make_shared<std::atomic<bool>>(false),found==impl_->editBins.end()?impl_->noEdits:found->second};
        impl_->pending.emplace(key,job);impl_->jobs.push_back(std::move(job));
    }
    std::stable_sort(impl_->jobs.begin(),impl_->jobs.end(),[&](const auto& a,const auto& b){return distanceSquared(a.key,center)<distanceSquared(b.key,center);});
    impl_->ready.notify_all();
}
void World::edit(const TerrainEdit& edit) {
    if(!validEdit(edit)) return;
    std::lock_guard lock(impl_->mutex);
    auto snapshot=std::make_shared<std::vector<TerrainEdit>>(*impl_->edits);snapshot->push_back(edit);impl_->edits=snapshot;
    indexEdit(impl_->editBins,edit);
    for(auto it=impl_->chunks.begin();it!=impl_->chunks.end();)
        if(editTouches(edit,it->first)) it=impl_->chunks.erase(it);else ++it;
    for(auto it=impl_->pending.begin();it!=impl_->pending.end();)
        if(editTouches(edit,it->first)) {it->second.cancelled->store(true,std::memory_order_relaxed);it=impl_->pending.erase(it);}else ++it;
    std::erase_if(impl_->jobs,[](const auto& job){return job.cancelled->load(std::memory_order_relaxed);});
    std::erase_if(impl_->completed,[&](const auto& mesh){return editTouches(edit,mesh.key);});
}
void World::setEdits(const std::vector<TerrainEdit>& edits) {
    auto snapshot=std::make_shared<std::vector<TerrainEdit>>();snapshot->reserve(edits.size());
    EditBins bins;
    for(const auto& edit:edits) if(validEdit(edit)) {snapshot->push_back(edit);indexEdit(bins,edit);}
    std::lock_guard lock(impl_->mutex);
    impl_->edits=snapshot;impl_->editBins=std::move(bins);impl_->chunks.clear();
    for(auto& [key,job]:impl_->pending) job.cancelled->store(true,std::memory_order_relaxed);
    impl_->pending.clear();impl_->jobs.clear();impl_->completed.clear();
}
std::vector<TerrainEdit> World::edits() const {std::lock_guard lock(impl_->mutex);return *impl_->edits;}
int World::pendingJobs() const {std::lock_guard lock(impl_->mutex);return static_cast<int>(impl_->pending.size());}
bool World::raycast(glm::vec3 origin,glm::vec3 direction,float maxDistance,glm::vec3& hit) const {
    const float length=glm::length(direction);
    if(length<1e-6f||maxDistance<=0) return false;
    direction/=length;
    // Cache the local edit bin until the ray enters another chunk. Far-away
    // sculpting has constant rather than linear cost for collision and picking.
    std::shared_ptr<const std::vector<TerrainEdit>> snapshot;
    ChunkKey lastKey{std::numeric_limits<int>::max(),std::numeric_limits<int>::max()};
    const auto sample=[&](float t) {
        const glm::vec3 p=origin+direction*t;const ChunkKey key=chunkAt(p);
        if(key!=lastKey) {
            std::lock_guard lock(impl_->mutex);lastKey=key;
            const auto found=impl_->editBins.find(key);
            snapshot=found==impl_->editBins.end()?impl_->noEdits:found->second;
        }
        return sampleDensity(p,impl_->seed,*snapshot);
    };
    float previous=sample(0),previousT=0;
    for(float t=.3f;t<=maxDistance+.3f;t+=.3f) {
        const float currentT=std::min(t,maxDistance),value=sample(currentT);
        if((previous<0)!=(value<0)) {
            float low=previousT,high=currentT;
            for(int i=0;i<9;++i) {const float mid=(low+high)*.5f;if((sample(mid)<0)==(previous<0)) low=mid;else high=mid;}
            hit=origin+direction*((low+high)*.5f);return true;
        }
        previous=value;previousT=currentT;if(currentT>=maxDistance) break;
    }
    return false;
}
}
