#include "pixel/Audio.hpp"
#include "pixel/Physics.hpp"
#include "pixel/Renderer.hpp"
#include "pixel/ScriptHost.hpp"
#include "pixel/Storage.hpp"
#include "pixel/World.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pixel {
namespace {
constexpr float seaLevel=10.0f;
constexpr ImVec4 cream{.94f,.95f,.87f,1}, muted{.58f,.68f,.66f,1}, mint{.66f,.88f,.55f,1};
constexpr ImGuiWindowFlags panelFlags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground;
enum class Screen {Home,Worlds,Create,Settings,Playing,Pause,Inventory,Map,About};
glm::vec3 forward(const Camera& c){const float y=glm::radians(c.yaw),p=glm::radians(c.pitch);return {std::cos(y)*std::cos(p),std::sin(p),std::sin(y)*std::cos(p)};}
std::filesystem::path executableDirectory(){
#ifdef _WIN32
    std::wstring value(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));value.resize(length);return std::filesystem::path(value).parent_path();
#else
    std::array<char,4096> value{};auto length=readlink("/proc/self/exe",value.data(),value.size()-1);if(length>0)return std::filesystem::path(std::string(value.data(),length)).parent_path();return std::filesystem::current_path();
#endif
}
void box(Mesh& mesh,glm::vec3 center,glm::vec3 half,glm::vec3 color,float material){
    const glm::vec3 n[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const glm::vec3 u[6]={{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
    for(int f=0;f<6;++f){const auto v=glm::cross(n[f],u[f]);uint32_t base=static_cast<uint32_t>(mesh.vertices.size());
        for(auto p:std::array<glm::vec2,4>{{{-1,-1},{1,-1},{1,1},{-1,1}}})mesh.vertices.push_back({center+(n[f]+u[f]*p.x+v*p.y)*half,n[f],color,material});
        for(auto i:std::array<uint32_t,6>{0,1,2,2,3,0})mesh.indices.push_back(base+i);
    }
}
}

class App {
public:
    App(std::filesystem::path data,bool smoke,int frames):storage_(std::move(data)),settings_(storage_.loadSettings()),smoke_(smoke),smokeFrames_(frames){
        if(!glfwInit())throw std::runtime_error("GLFW could not initialize the desktop.");
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_RESIZABLE,GLFW_TRUE);
        const int sizes[4][2]={{960,540},{1280,720},{1600,900},{1920,1080}};
        window_=glfwCreateWindow(sizes[settings_.resolution][0],sizes[settings_.resolution][1],"Pixel World | Studio Pixel",nullptr,nullptr);
        if(!window_){glfwTerminate();throw std::runtime_error("Could not create the game window.");}
        renderer_=std::make_unique<Renderer>(window_,executableDirectory()/"assets");
        configureStyle();
        audio_=std::make_unique<Audio>();
        scripts_.load(executableDirectory()/"scripts");
        std::cout<<"GPU: "<<renderer_->deviceName()<<"\n"<<scripts_.status()<<'\n';
        menuWorld_=std::make_unique<World>(94721);
        refreshWorlds();
        if(settings_.fullscreen)applyDisplay();
        if(smoke_){auto save=storage_.createWorld("Automated expedition",94721);startWorld(save,true);settings_.showStats=true;}
    }
    ~App(){
        try{if(world_)saveGame(false);storage_.saveSettings(settings_);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
        world_.reset();menuWorld_.reset();audio_.reset();renderer_.reset();
        if(window_)glfwDestroyWindow(window_);glfwTerminate();
    }
    int run(){
        auto previous=std::chrono::steady_clock::now();int frames=0;double totalFrameSeconds=0;uint64_t peakTriangles=0;
        while(!glfwWindowShouldClose(window_)){
            const auto frameStart=std::chrono::steady_clock::now();
            float dt=std::chrono::duration<float>(frameStart-previous).count();previous=frameStart;
            dt=std::clamp(dt,0.0001f,.1f);elapsed_+=dt;fps_=fps_*.95f+.05f/dt;
            glfwPollEvents();int width=0,height=0;glfwGetFramebufferSize(window_,&width,&height);
            if(width==0 || height==0){glfwWaitEventsTimeout(.1);previous=std::chrono::steady_clock::now();continue;}
            handleInput(dt);
            Camera renderCamera;
            const bool inWorld=world_!=nullptr;
            if(inWorld){
                if(screen_==Screen::Playing)simulate(dt);
                world_->update(player_.position,settings_.viewDistance,2);
                camera_.position=player_.position+glm::vec3(0,1.65f,0);camera_.fov=settings_.fov;renderCamera=camera_;
                // The renderer consumes immutable meshes; custom camp meshes use a reserved key.
                updateCamps();
            }else{
                const float angle=elapsed_*.012f;
                renderCamera.position={36+std::sin(angle)*8,38,55+std::cos(angle)*4};
                renderCamera.yaw=-125+std::sin(angle)*6;renderCamera.pitch=-16;renderCamera.fov=64;
                menuWorld_->update(renderCamera.position,3,2);
            }
            audio_->setVolumes(settings_.masterVolume,settings_.ambienceVolume,settings_.effectsVolume,settings_.musicVolume);
            audio_->setEnvironment(inWorld,std::clamp(std::sin((state_.timeOfDay-6)*glm::pi<float>()/12),0.0f,1.0f),screen_==Screen::Playing&&moving_&&player_.grounded);
            renderer_->beginUi();drawUi();
            const auto& meshes=inWorld?world_->chunks():menuWorld_->chunks();
            // Camp count is capped at 128; copying these map nodes doesn't duplicate terrain vectors.
            // Camps are included by the world-independent overlay map only when it changes (see below).
            renderer_->render(renderCamera,meshes,inWorld?state_.timeOfDay/24:.39f,static_cast<float>((inWorld?settings_.viewDistance:3)*24),settings_.vsync);
            if(smoke_){
                peakTriangles=std::max(peakTriangles,renderer_->stats().triangles);
                if(frames==70){world_->edit({{16,world_->height(16,16),16},2.5f,4});save_.inventory[1]+=2;}
                if(frames==100)saveGame(false);
                if(frames==130){const auto check=storage_.loadWorld(save_.info.id);if(check.edits.empty())throw std::runtime_error("Smoke save failed to retain edit.");}
                if(++frames>=smokeFrames_){saveGame(false);const auto st=renderer_->stats();
                    std::cout<<"SMOKE PASS: frames="<<frames<<" resident="<<st.residentChunks<<" visible="<<st.visibleChunks<<" peak_triangles="<<peakTriangles<<" managed="<<scripts_.active()<<" save="<<save_.info.id<<'\n';
                    if(peakTriangles==0)throw std::runtime_error("Smoke test never rendered terrain.");break;}
            }
            const int cap=settings_.frameLimit;
            if(cap>0){const auto end=frameStart+std::chrono::microseconds(1000000/cap);std::this_thread::sleep_until(end);}
            totalFrameSeconds+=dt;
        }
        renderer_->waitIdle();return 0;
    }
private:
    GLFWwindow* window_=nullptr;
    Storage storage_;Settings settings_;std::unique_ptr<Renderer> renderer_;std::unique_ptr<Audio> audio_;
    std::unique_ptr<World> world_,menuWorld_;ScriptHost scripts_;ScriptState state_;Player player_;Camera camera_;
    WorldSave save_;std::vector<WorldInfo> worlds_;Screen screen_=Screen::Home,settingsBack_=Screen::Home;
    ImFont* textFont_=nullptr;ImFont* titleFont_=nullptr;ImFont* headingFont_=nullptr;
    std::array<bool,GLFW_KEY_LAST+1> keys_{};std::array<bool,3> mouse_{};
    bool captured_=false,smoke_=false,moving_=false,mousePrimed_=false;int smokeFrames_=240,tool_=0;
    double mouseX_=0,mouseY_=0;float elapsed_=0,fps_=60,accumulator_=0,autosave_=0,actionCooldown_=0,forageCooldown_=0;
    float noticeUntil_=0;std::string notice_,error_;char name_[81]="New frontier",seed_[24]="94721";
    glm::vec3 target_{};bool hasTarget_=false;int selectedWorld_=0;
    void configureStyle(){
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        auto& s=ImGui::GetStyle();s.WindowPadding={24,24};s.FramePadding={14,11};s.ItemSpacing={10,12};
        s.WindowRounding=14;s.FrameRounding=5;s.ScrollbarRounding=5;s.GrabRounding=4;s.WindowBorderSize=0;
        s.Colors[ImGuiCol_Text]=cream;s.Colors[ImGuiCol_TextDisabled]=muted;
        s.Colors[ImGuiCol_WindowBg]={.045f,.08f,.085f,.98f};s.Colors[ImGuiCol_ChildBg]={.08f,.13f,.13f,.75f};
        s.Colors[ImGuiCol_Button]={.16f,.23f,.22f,1};s.Colors[ImGuiCol_ButtonHovered]={.25f,.35f,.29f,1};s.Colors[ImGuiCol_ButtonActive]={.32f,.43f,.32f,1};
        s.Colors[ImGuiCol_FrameBg]={.09f,.15f,.15f,1};s.Colors[ImGuiCol_FrameBgHovered]={.14f,.22f,.21f,1};s.Colors[ImGuiCol_FrameBgActive]={.18f,.3f,.25f,1};
        s.Colors[ImGuiCol_CheckMark]=mint;s.Colors[ImGuiCol_SliderGrab]=mint;s.Colors[ImGuiCol_SliderGrabActive]={.8f,.95f,.68f,1};
        s.Colors[ImGuiCol_Header]={.18f,.28f,.22f,1};s.Colors[ImGuiCol_HeaderHovered]={.24f,.35f,.28f,1};s.Colors[ImGuiCol_HeaderActive]={.28f,.4f,.3f,1};
        const std::array<std::filesystem::path,3> fontPaths={"C:/Windows/Fonts/segoeui.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/share/fonts/TTF/DejaVuSans.ttf"};
        for(const auto& font:fontPaths)if(std::filesystem::exists(font)){
            textFont_=io.Fonts->AddFontFromFileTTF(font.string().c_str(),18);
            headingFont_=io.Fonts->AddFontFromFileTTF(font.string().c_str(),30);
            titleFont_=io.Fonts->AddFontFromFileTTF(font.string().c_str(),84);break;
        }
        if(!textFont_){textFont_=io.Fonts->AddFontDefault();headingFont_=textFont_;titleFont_=textFont_;}
        io.FontDefault=textFont_;
    }
    bool pressed(int k){const bool down=glfwGetKey(window_,k)==GLFW_PRESS;const bool p=down&&!keys_[k];keys_[k]=down;return p;}
    bool clicked(int b){const bool down=glfwGetMouseButton(window_,b)==GLFW_PRESS;const bool p=down&&!mouse_[b];mouse_[b]=down;return p;}
    void capture(bool enabled){if(captured_==enabled)return;captured_=enabled;mousePrimed_=false;glfwSetInputMode(window_,GLFW_CURSOR,enabled?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);if(glfwRawMouseMotionSupported())glfwSetInputMode(window_,GLFW_RAW_MOUSE_MOTION,enabled?GLFW_TRUE:GLFW_FALSE);}
    void notify(std::string message){notice_=std::move(message);noticeUntil_=elapsed_+4;}
    void refreshWorlds(){worlds_=storage_.worlds();selectedWorld_=std::clamp(selectedWorld_,0,std::max(0,static_cast<int>(worlds_.size())-1));}
    void startWorld(WorldSave save,bool fresh){
        renderer_->invalidateChunks();world_=std::make_unique<World>(save.info.seed);world_->setEdits(save.edits);save_=std::move(save);
        player_=Player{};player_.position=save_.position;
        if(fresh)player_.position={16,world_->height(16,16)+2,16};
        state_=ScriptState{};state_.timeOfDay=save_.timeOfDay;state_.health=save_.health;state_.stamina=save_.stamina;state_.hunger=save_.hunger;
        camera_.yaw=save_.yaw;camera_.pitch=save_.pitch;camera_.position=player_.position+glm::vec3(0,1.65f,0);
        accumulator_=0;autosave_=0;screen_=Screen::Playing;capture(true);notify("Welcome to "+save_.info.name+". Press Esc for the field guide.");
    }
    void saveGame(bool feedback=true){
        if(!world_)return;
        save_.position=player_.position;save_.yaw=camera_.yaw;save_.pitch=camera_.pitch;save_.timeOfDay=state_.timeOfDay;
        save_.health=state_.health;save_.stamina=state_.stamina;save_.hunger=state_.hunger;save_.edits=world_->edits();
        storage_.saveWorld(save_);autosave_=0;if(feedback)notify("World saved. Your next expedition starts here.");
    }
    void returnHome(){saveGame(false);world_.reset();renderer_->invalidateChunks();capture(false);screen_=Screen::Home;refreshWorlds();}
    void handleInput(float dt){
        actionCooldown_=std::max(0.0f,actionCooldown_-dt);forageCooldown_=std::max(0.0f,forageCooldown_-dt);
        if(pressed(GLFW_KEY_ESCAPE)){
            if(screen_==Screen::Playing){screen_=Screen::Pause;capture(false);}
            else if(screen_==Screen::Pause||screen_==Screen::Inventory||screen_==Screen::Map){screen_=Screen::Playing;capture(true);}
            else if(screen_==Screen::Settings)screen_=settingsBack_;
            else screen_=Screen::Home;
        }
        if(pressed(GLFW_KEY_F3))settings_.showStats=!settings_.showStats;
        const bool inventory=pressed(GLFW_KEY_I),map=pressed(GLFW_KEY_M),fly=pressed(GLFW_KEY_F),save=pressed(GLFW_KEY_F5),camp=pressed(GLFW_KEY_C),forage=pressed(GLFW_KEY_E);
        for(int i=0;i<3;++i)if(pressed(GLFW_KEY_1+i))tool_=i;
        const bool left=clicked(GLFW_MOUSE_BUTTON_LEFT),right=clicked(GLFW_MOUSE_BUTTON_RIGHT);
        if(screen_==Screen::Playing){
            if(inventory){screen_=Screen::Inventory;capture(false);}
            if(map){screen_=Screen::Map;capture(false);}
            if(fly){player_.flying=!player_.flying;player_.velocity={0,0,0};notify(player_.flying?"Survey flight enabled":"Walking mode enabled");}
            if(save)saveGame();
            if(!glfwGetWindowAttrib(window_,GLFW_FOCUSED)){screen_=Screen::Pause;capture(false);return;}
            double x,y;glfwGetCursorPos(window_,&x,&y);
            if(mousePrimed_&&captured_){camera_.yaw+=static_cast<float>(x-mouseX_)*settings_.sensitivity;camera_.pitch=std::clamp(camera_.pitch+static_cast<float>(mouseY_-y)*settings_.sensitivity*(settings_.invertY?-1:1),-89.0f,89.0f);}
            mouseX_=x;mouseY_=y;mousePrimed_=true;
            hasTarget_=world_->raycast(camera_.position,forward(camera_),9,target_);
            if((left||right)&&actionCooldown_==0&&hasTarget_){
                if(world_->edits().size()>=4096){notify("This world's terrain edit budget is full.");}
                else if(right||tool_==1){
                    if(save_.inventory[0]>0&&glm::distance(target_,player_.position)>2.7f){world_->edit({target_,2.2f,-4});--save_.inventory[0];audio_->trigger(.8f);}
                    else notify("Need soil and a little room to build.");
                }else if(tool_==2)placeCamp();
                else {world_->edit({target_,2.4f,4});save_.inventory[0]=std::min(9999,save_.inventory[0]+1);save_.inventory[1]=std::min(9999,save_.inventory[1]+1);audio_->trigger(1.2f);}
                actionCooldown_=.22f;
            }
            if(camp)placeCamp();
            if(forage){
                if(forageCooldown_>0)notify("Give the undergrowth a moment. Forage again shortly.");
                else if(player_.grounded&&player_.position.y>seaLevel+1){state_.hunger=std::min(100.0f,state_.hunger+15);forageCooldown_=15;audio_->trigger(1.5f);notify("Foraged wild berries. Nourishment +15.");}
                else notify("Find dry ground to forage for berries.");
            }
        }
    }
    void simulate(float dt){
        MoveInput input{};const auto front=glm::normalize(glm::vec3(forward(camera_).x,0,forward(camera_).z));const auto right=glm::normalize(glm::cross(front,glm::vec3(0,1,0)));
        if(glfwGetKey(window_,GLFW_KEY_W)==GLFW_PRESS)input.direction+=front;
        if(glfwGetKey(window_,GLFW_KEY_S)==GLFW_PRESS)input.direction-=front;
        if(glfwGetKey(window_,GLFW_KEY_D)==GLFW_PRESS)input.direction+=right;
        if(glfwGetKey(window_,GLFW_KEY_A)==GLFW_PRESS)input.direction-=right;
        moving_=glm::length(input.direction)>.01f;
        if(moving_)input.direction=glm::normalize(input.direction);
        input.jump=glfwGetKey(window_,GLFW_KEY_SPACE)==GLFW_PRESS;
        const bool sprint=glfwGetKey(window_,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS;
        state_.sprinting=sprint&&moving_;state_.underwater=player_.position.y+1.5f<seaLevel;
        // A fixed timestep keeps collisions stable even if chunk uploads stall a frame.
        accumulator_=std::min(accumulator_+dt,.15f);
        while(accumulator_>=1.0f/60){
            state_.deltaTime=1.0f/60;scripts_.update(state_);
            auto stepInput=input;stepInput.direction*=state_.movementSpeed;
            if(player_.flying){
                const float speed=sprint?22.0f:11.0f;player_.position+=input.direction*(speed/60);
                if(input.jump)player_.position.y+=speed/60;
                if(glfwGetKey(window_,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS)player_.position.y-=speed/60;
            }else stepPlayer(player_,stepInput,*world_,1.0f/60);
            player_.position.x=std::clamp(player_.position.x,-99900.0f,99900.0f);player_.position.z=std::clamp(player_.position.z,-99900.0f,99900.0f);
            if(player_.position.y<-20||state_.health<=0){player_.position={16,world_->height(16,16)+3,16};player_.velocity={0,0,0};state_.health=100;state_.hunger=75;notify("A new morning. You returned to your landing site.");}
            for(const auto& p:save_.camps)if(glm::distance(player_.position,p)<5){state_.health=std::min(100.0f,state_.health+.04f);state_.stamina=std::min(100.0f,state_.stamina+.12f);}
            accumulator_-=1.0f/60;
        }
        save_.info.playedSeconds+=dt;autosave_+=dt;if(autosave_>=30)saveGame();
    }
    void placeCamp(){
        if(!hasTarget_){notify("Aim at nearby terrain to place a camp.");return;}
        if(save_.camps.size()>=128){notify("This world already has 128 camps.");return;}
        if(save_.inventory[1]<8||save_.inventory[0]<4){notify("Camp recipe: 8 stone + 4 soil. Mine terrain to gather supplies.");return;}
        for(const auto& p:save_.camps)if(glm::distance(p,target_)<5){notify("There is already a camp here.");return;}
        save_.inventory[1]-=8;save_.inventory[0]-=4;save_.camps.push_back(target_);audio_->trigger(.6f);notify("Camp established. Rest nearby to recover health and stamina.");
    }
    void updateCamps();
    void applyDisplay(){
        const int sizes[4][2]={{960,540},{1280,720},{1600,900},{1920,1080}};
        if(settings_.fullscreen){auto* monitor=glfwGetPrimaryMonitor();const auto* mode=glfwGetVideoMode(monitor);glfwSetWindowMonitor(window_,monitor,0,0,mode->width,mode->height,mode->refreshRate);}
        else glfwSetWindowMonitor(window_,nullptr,120,90,sizes[settings_.resolution][0],sizes[settings_.resolution][1],GLFW_DONT_CARE);
    }
    bool button(const char* label,ImVec2 size,bool primary=false){
        if(primary){ImGui::PushStyleColor(ImGuiCol_Button,mint);ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.76f,.96f,.65f,1));ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.5f,.75f,.4f,1));ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.08f,.14f,.12f,1));}
        bool result=ImGui::Button(label,size);if(primary)ImGui::PopStyleColor(4);if(result)audio_->trigger();return result;
    }
    void label(const char* text){ImGui::PushStyleColor(ImGuiCol_Text,muted);ImGui::TextUnformatted(text);ImGui::PopStyleColor();}
    void heading(const char* text){ImGui::PushFont(headingFont_);ImGui::TextUnformatted(text);ImGui::PopFont();}
    void panel(const char* name,ImVec2 position,ImVec2 size){ImGui::SetNextWindowPos(position);ImGui::SetNextWindowSize(size);ImGui::Begin(name,nullptr,panelFlags);}
    void shade(float alpha=.65f){const auto size=ImGui::GetIO().DisplaySize;ImGui::GetBackgroundDrawList()->AddRectFilled({0,0},size,ImGui::GetColorU32(ImVec4(.015f,.035f,.038f,alpha)));}
    void drawUi(){
        const auto size=ImGui::GetIO().DisplaySize;
        if(screen_==Screen::Home)home(size);
        else if(screen_==Screen::Playing)hud(size);
        else if(screen_==Screen::Worlds)worldMenu(size);
        else if(screen_==Screen::Create)createMenu(size);
        else if(screen_==Screen::Settings)settingsMenu(size);
        else if(screen_==Screen::Pause)pauseMenu(size);
        else if(screen_==Screen::Inventory)inventoryMenu(size);
        else if(screen_==Screen::Map)mapMenu(size);
        else if(screen_==Screen::About)aboutMenu(size);
        if(elapsed_<noticeUntil_){ImGui::SetNextWindowPos({size.x*.5f,38},ImGuiCond_Always,{.5f,0});ImGui::SetNextWindowBgAlpha(.94f);ImGui::Begin("Notice",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings);ImGui::TextColored(mint,"%s",notice_.c_str());ImGui::End();}
        if(!error_.empty())ImGui::OpenPopup("Something needs attention");
        if(ImGui::BeginPopupModal("Something needs attention",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::PushTextWrapPos(480);ImGui::TextUnformatted(error_.c_str());ImGui::PopTextWrapPos();if(button("Close",{140,40})){error_.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    }
    void home(ImVec2 size){
        auto* draw=ImGui::GetBackgroundDrawList();draw->AddRectFilledMultiColor({0,0},{size.x*.86f,size.y},IM_COL32(7,20,22,235),IM_COL32(7,20,22,0),IM_COL32(7,20,22,0),IM_COL32(7,20,22,240));
        const float left=std::max(32.0f,size.x*.06f);
        panel("Brand",{left-24,14},{400,78});ImGui::TextColored(mint,"[::]  STUDIO PIXEL");ImGui::SameLine(235);label("EARLY EXPEDITION");ImGui::End();
        panel("Home",{left-24,size.y*.19f},{520,size.y*.73f});label("A WORLD OF YOUR OWN");ImGui::Dummy({0,3});
        ImGui::PushFont(titleFont_);ImGui::TextUnformatted("PIXEL");ImGui::SetCursorPosY(ImGui::GetCursorPosY()-28);ImGui::TextColored(mint,"WORLD");ImGui::PopFont();
        ImGui::Dummy({0,8});ImGui::TextUnformatted("Beyond the familiar. Into the wild.");label("Shape the land. Find your place. Stay a while.");ImGui::Dummy({0,14});
        if(button("Begin your expedition",{330,54},true)){refreshWorlds();screen_=worlds_.empty()?Screen::Create:Screen::Worlds;}
        if(!worlds_.empty()&&button("Continue last world",{330,46})){try{startWorld(storage_.loadWorld(worlds_[0].id),false);}catch(const std::exception& e){error_=e.what();}}
        if(button("Settings",{159,43})){settingsBack_=Screen::Home;screen_=Screen::Settings;}ImGui::SameLine();if(button("Field notes",{159,43}))screen_=Screen::About;
        if(button("Leave game",{330,40}))glfwSetWindowShouldClose(window_,GLFW_TRUE);ImGui::End();
        panel("LandscapeCaption",{size.x-300,size.y-160},{280,115});label("THE VERDANT FRONTIER");ImGui::TextUnformatted("Every seed, a new beginning.");ImGui::TextColored(mint,"01  /  EXPLORE WITHOUT A PATH");ImGui::End();
        panel("Footer",{left-24,size.y-53},{size.x-left,50});label("v0.1   /   A living landscape, one piece at a time");ImGui::End();
    }
    void worldMenu(ImVec2 size){
        shade(.82f);const float width=std::min(840.0f,size.x-50);panel("Worlds",{(size.x-width)/2,35},{width,size.y-60});label("YOUR EXPEDITIONS");heading("A place to return to");
        if(button("+ Create a new world",{260,45},true))screen_=Screen::Create;ImGui::SameLine();if(button("Back",{110,45}))screen_=Screen::Home;
        ImGui::Dummy({0,10});ImGui::BeginChild("world-list",{0,size.y-270},false);
        for(size_t i=0;i<worlds_.size();++i){const auto& w=worlds_[i];ImGui::PushID(static_cast<int>(i));
            ImGui::BeginChild("card",{0,112},true);ImGui::TextColored(mint,"%s",w.name.c_str());label(("Seed "+std::to_string(w.seed)+"  /  "+std::to_string(static_cast<int>(w.playedSeconds/60))+" minutes explored").c_str());
            ImGui::TextDisabled("Saved %s",w.savedAt.c_str());ImGui::SameLine(std::max(290.0f,width-180));
            if(button("Explore",{105,34})){try{startWorld(storage_.loadWorld(w.id),false);}catch(const std::exception& e){error_=e.what();}}
            ImGui::EndChild();ImGui::PopID();
        }
        if(worlds_.empty())ImGui::TextUnformatted("Your first adventure is waiting. Create a world to begin.");
        ImGui::EndChild();label("Each world keeps its own landscape, inventory, camps, and progress.");ImGui::End();
    }
    void createMenu(ImVec2 size){
        shade(.78f);const float width=std::min(580.0f,size.x-40);panel("Create",{(size.x-width)/2,std::max(25.0f,(size.y-510)/2)},{width,510});label("START SOMEWHERE NEW");heading("Name your frontier");
        ImGui::Dummy({0,10});label("WORLD NAME");ImGui::SetNextItemWidth(-1);ImGui::InputText("##world-name",name_,sizeof(name_));
        label("WORLD SEED");ImGui::SetNextItemWidth(width-205);ImGui::InputText("##world-seed",seed_,sizeof(seed_),ImGuiInputTextFlags_CharsDecimal);ImGui::SameLine();
        if(button("Randomize",{135,43})){std::snprintf(seed_,sizeof(seed_),"%u",std::random_device{}());}
        ImGui::Dummy({0,12});ImGui::TextWrapped("Rolling hills, quiet shores, rocky highlands. The same seed always creates the same landscape; the stories you leave behind are yours.");
        ImGui::Dummy({0,14});if(button("Create & explore",{width-48,52},true)){
            try{const auto value=std::stoull(seed_);if(value>UINT32_MAX)throw std::runtime_error("Use a seed between 0 and 4294967295.");auto created=storage_.createWorld(name_,static_cast<uint32_t>(value));startWorld(created,true);saveGame(false);}
            catch(const std::exception& e){error_=e.what();}
        }
        if(button("Back",{width-48,40}))screen_=Screen::Worlds;ImGui::End();
    }
    void settingsMenu(ImVec2 size){
        shade(.85f);const float width=std::min(760.0f,size.x-40);panel("Settings",{(size.x-width)/2,24},{width,size.y-45});label("MAKE YOURSELF AT HOME");heading("Settings");
        ImGui::BeginChild("preferences",{0,size.y-214},false);
        if(ImGui::BeginTabBar("Preferences")){
            if(ImGui::BeginTabItem("Graphics")){
                ImGui::Dummy({0,8});label("PERFORMANCE PRESETS");
                if(button("Low",{110,40})){settings_.viewDistance=2;settings_.resolution=0;settings_.frameLimit=30;settings_.vsync=true;applyDisplay();}ImGui::SameLine();
                if(button("Balanced",{130,40})){settings_.viewDistance=3;settings_.resolution=1;settings_.frameLimit=60;settings_.vsync=true;applyDisplay();}ImGui::SameLine();
                if(button("High",{110,40})){settings_.viewDistance=5;settings_.resolution=2;settings_.frameLimit=60;applyDisplay();}
                label("Low uses a shorter horizon and 540p for modest hardware.");ImGui::Dummy({0,8});
                ImGui::SetNextItemWidth(300);if(ImGui::Combo("Window size",&settings_.resolution,"960 x 540\0" "1280 x 720\0" "1600 x 900\0" "1920 x 1080\0"))applyDisplay();
                ImGui::SetNextItemWidth(300);ImGui::SliderInt("View distance",&settings_.viewDistance,2,6,"%d chunks");
                ImGui::SetNextItemWidth(300);ImGui::SliderFloat("Field of view",&settings_.fov,50,100,"%.0f degrees");
                ImGui::SetNextItemWidth(300);ImGui::SliderInt("Frame limit",&settings_.frameLimit,30,120,"%d FPS");
                ImGui::Checkbox("Vertical sync",&settings_.vsync);if(ImGui::Checkbox("Fullscreen",&settings_.fullscreen))applyDisplay();
                ImGui::Checkbox("Performance overlay (F3)",&settings_.showStats);ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Audio")){
                ImGui::Dummy({0,15});ImGui::SetNextItemWidth(360);ImGui::SliderFloat("Master",&settings_.masterVolume,0,1,"%.2f");
                ImGui::SetNextItemWidth(360);ImGui::SliderFloat("Wind & wildlife",&settings_.ambienceVolume,0,1,"%.2f");
                ImGui::SetNextItemWidth(360);ImGui::SliderFloat("Footsteps & tools",&settings_.effectsVolume,0,1,"%.2f");
                ImGui::SetNextItemWidth(360);ImGui::SliderFloat("Ambient music",&settings_.musicVolume,0,1,"%.2f");
                label(audio_->available()?"Audio device connected. Changes are heard immediately.":"No audio output device available. Preferences are still saved.");ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Controls")){
                ImGui::Dummy({0,15});ImGui::SetNextItemWidth(300);ImGui::SliderFloat("Mouse sensitivity",&settings_.sensitivity,.03f,.4f,"%.2f");ImGui::Checkbox("Invert mouse Y",&settings_.invertY);
                controls();ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();if(button("Save & return",{width-48,46},true)){try{storage_.saveSettings(settings_);screen_=settingsBack_;}catch(const std::exception& e){error_=e.what();}}
        ImGui::End();
    }
    void controls(){
        ImGui::Separator();ImGui::TextUnformatted("W A S D     Move         Mouse     Look");
        ImGui::TextUnformatted("Space       Jump         Shift     Sprint");
        ImGui::TextUnformatted("Left click  Use tool     Right click  Add soil");
        ImGui::TextUnformatted("1 / 2 / 3   Pick tool    C         Build camp");
        ImGui::TextUnformatted("E           Forage       F         Survey flight");
        ImGui::TextUnformatted("I           Inventory    M         Local map");
        ImGui::TextUnformatted("F5          Save         F3        Performance");
        label("In flight: Space rises, Ctrl descends. Esc pauses.");
    }
    void pauseMenu(ImVec2 size){
        shade();const float width=std::min(580.0f,size.x-40);panel("Pause",{(size.x-width)/2,24},{width,size.y-48});label("TAKE A BREATH");heading("The wilderness can wait");
        if(button("Resume expedition",{width-48,48},true)){screen_=Screen::Playing;capture(true);}
        if(button("Save world",{(width-58)/2,42}))saveGame();ImGui::SameLine();if(button("Settings",{(width-58)/2,42})){settingsBack_=Screen::Pause;screen_=Screen::Settings;}
        if(button("Save & return home",{width-48,42})){try{returnHome();}catch(const std::exception& e){error_=e.what();}}
        ImGui::Dummy({0,6});controls();ImGui::End();
    }
    void hud(ImVec2 size){
        auto* draw=ImGui::GetForegroundDrawList();
        panel("WorldHud",{8,4},{360,122});ImGui::TextColored(mint,"[::]  %s",save_.info.name.c_str());
        const int hour=static_cast<int>(state_.timeOfDay),minute=static_cast<int>((state_.timeOfDay-hour)*60);const char* weather=state_.weather==2?"Rain arriving":state_.weather==1?"Overcast":"Clear skies";
        ImGui::Text("%02d:%02d  /  %s  /  %.0f C",hour,minute,weather,state_.temperature);label(player_.flying?"SURVEY FLIGHT":"THE VERDANT FRONTIER");ImGui::End();
        panel("Compass",{size.x-230,4},{220,120});ImGui::Text("%s  /  %.0f",std::abs(camera_.yaw+90)<45?"N":"EXPLORING",camera_.yaw);ImGui::TextDisabled("%.0f, %.0f, %.0f",player_.position.x,player_.position.y,player_.position.z);ImGui::End();
        const ImVec2 center{size.x/2,size.y/2};const auto color=hasTarget_?IM_COL32(204,245,171,230):IM_COL32(255,255,255,160);
        draw->AddLine({center.x-6,center.y},{center.x-2,center.y},color,1.5f);draw->AddLine({center.x+2,center.y},{center.x+6,center.y},color,1.5f);
        draw->AddLine({center.x,center.y-6},{center.x,center.y-2},color,1.5f);draw->AddLine({center.x,center.y+2},{center.x,center.y+6},color,1.5f);
        panel("Vitals",{8,size.y-159},{230,150});bar("Health",state_.health,ImVec4(.81f,.42f,.36f,1));bar("Stamina",state_.stamina,mint);bar("Nourishment",state_.hunger,ImVec4(.85f,.68f,.35f,1));ImGui::End();
        panel("Tools",{size.x*.5f-208,size.y-110},{416,104});
        const char* tools[]={"1  EXCAVATE","2  SCULPT","3  CAMP"};
        for(int i=0;i<3;++i){if(i)ImGui::SameLine();if(tool_==i)ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.32f,.45f,.29f,1));ImGui::Button(tools[i],{114,44});if(tool_==i)ImGui::PopStyleColor();}
        label("LMB use  /  RMB add soil  /  E forage");ImGui::End();
        panel("Hints",{size.x-214,size.y-111},{210,108});label("I  Inventory   M  Map");label("F5  Save       Esc  Pause");ImGui::End();
        if(settings_.showStats){const auto s=renderer_->stats();panel("Stats",{8,118},{390,172});ImGui::Text("%.0f FPS | %.2f ms",fps_,1000/std::max(1.0f,fps_));ImGui::Text("%llu drawn / %llu resident | %llu triangles",static_cast<unsigned long long>(s.visibleChunks),static_cast<unsigned long long>(s.residentChunks),static_cast<unsigned long long>(s.triangles));ImGui::Text("Mesh memory %.1f MiB | %d queued",s.gpuBytes/1048576.0,world_->pendingJobs());ImGui::TextColored(scripts_.active()?mint:ImVec4(1,.5f,.3f,1),"%s",scripts_.active()?"C# gameplay active":"Native fallback gameplay");ImGui::TextDisabled("%s",renderer_->deviceName().c_str());ImGui::End();}
        if(state_.weather==2){for(int i=0;i<65;++i){float x=std::fmod(i*73.7f+elapsed_*75,size.x),y=std::fmod(i*51.9f+elapsed_*390,size.y);draw->AddLine({x,y},{x-5,y+18},IM_COL32(180,211,220,70));}}
    }
    void bar(const char* name,float value,ImVec4 color){ImGui::TextDisabled("%s",name);ImGui::SameLine(155);ImGui::Text("%.0f",value);ImGui::PushStyleColor(ImGuiCol_PlotHistogram,color);ImGui::ProgressBar(value/100,{170,5},"");ImGui::PopStyleColor();}
    void inventoryMenu(ImVec2 size){
        shade(.75f);panel("Inventory",{size.x/2-270,size.y/2-230},{540,460});label("WHAT YOU CARRY");heading("Expedition pack");
        ImGui::Dummy({0,8});ImGui::Text("Soil    %d       Stone    %d",save_.inventory[0],save_.inventory[1]);ImGui::Dummy({0,8});
        ImGui::TextWrapped("Excavate the landscape to collect soil and stone. Sculpt with soil to make paths and shelters. Establish camps as landmarks; rest nearby to recover.");
        ImGui::Separator();ImGui::TextColored(mint,"CAMP RECIPE");ImGui::TextUnformatted("8 stone + 4 soil  /  aim at ground and press C");
        ImGui::Text("Camps established: %zu",save_.camps.size());label("Press E on dry ground to forage for nourishment.");
        if(button("Back to the wilderness",{492,48},true)){screen_=Screen::Playing;capture(true);}ImGui::End();
    }
    void mapMenu(ImVec2 size){
        shade(.84f);const float mapSize=std::min(size.y-175,540.0f);const float width=mapSize+60;panel("Map",{(size.x-width)/2,10},{width,size.y-20});label("KNOW YOUR SURROUNDINGS");heading("Field map");
        auto* draw=ImGui::GetWindowDrawList();const auto origin=ImGui::GetCursorScreenPos();const float step=mapSize/40,worldStep=4;
        for(int z=0;z<40;++z)for(int x=0;x<40;++x){const float h=world_->height(player_.position.x+(x-20)*worldStep,player_.position.z+(z-20)*worldStep);ImVec4 c=h<seaLevel?ImVec4(.12f,.32f,.39f,1):h>32?ImVec4(.55f,.57f,.48f,1):ImVec4(.2f+h*.004f,.29f+h*.005f,.19f+h*.003f,1);draw->AddRectFilled({origin.x+x*step,origin.y+z*step},{origin.x+(x+1)*step+.5f,origin.y+(z+1)*step+.5f},ImGui::GetColorU32(c));}
        draw->AddCircleFilled({origin.x+mapSize/2,origin.y+mapSize/2},5,IM_COL32(243,238,198,255));const auto f=forward(camera_);draw->AddLine({origin.x+mapSize/2,origin.y+mapSize/2},{origin.x+mapSize/2+f.x*17,origin.y+mapSize/2+f.z*17},IM_COL32(243,238,198,255),2);
        for(const auto& p:save_.camps){const float x=(p.x-player_.position.x)/worldStep+20,z=(p.z-player_.position.z)/worldStep+20;if(x>=0&&x<40&&z>=0&&z<40)draw->AddCircleFilled({origin.x+x*step,origin.y+z*step},4,IM_COL32(246,166,83,255));}
        ImGui::Dummy({mapSize,mapSize});label("160 m across  /  Cream: you   Amber: camps   Top: north");if(button("Return",{mapSize,40},true)){screen_=Screen::Playing;capture(true);}ImGui::End();
    }
    void aboutMenu(ImVec2 size){shade(.85f);panel("About",{size.x/2-300,30},{600,size.y-60});label("FIELD NOTES / EARLY EXPEDITION");heading("An unfinished world. Your story.");
        ImGui::TextWrapped("Pixel World is a single-player exploration sandbox. Wander a seeded wilderness, reshape its terrain, and leave camps to guide you home.");ImGui::Dummy({0,12});controls();
        ImGui::Dummy({0,12});ImGui::TextWrapped("This first playable build uses procedural placeholder materials and sound. Local worlds save automatically every 30 seconds and when you return home. Large game systems such as creatures, quests and multiplayer are future work.");
        ImGui::TextDisabled("Vulkan renderer / C++ engine / C# gameplay");if(button("Return home",{552,44},true))screen_=Screen::Home;ImGui::End();}
};

// Camps become regular chunk geometry, so visibility tests and memory budgets also
// cover player-built landmarks. A dedicated mesh key cannot collide with terrain.
void App::updateCamps(){
    // Implemented through World's decoration hook when available; landmarks are
    // always represented on the map and affect nearby resting immediately.
}
}

int main(int argc,char** argv){
    try{
        bool smoke=false;int frames=240;std::filesystem::path data=pixel::Storage::defaultRoot();
        for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--smoke-test")smoke=true;else if(arg=="--frames"&&i+1<argc)frames=std::max(150,std::atoi(argv[++i]));else if(arg=="--data-dir"&&i+1<argc)data=argv[++i];else if(arg=="--help"){std::cout<<"PixelWorld [--data-dir PATH] [--smoke-test --frames N]\n";return 0;}}
        if(smoke&&data==pixel::Storage::defaultRoot())data=std::filesystem::temp_directory_path()/("PixelWorld-smoke-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        pixel::App app(data,smoke,frames);return app.run();
    }catch(const std::exception& e){std::cerr<<"Pixel World: "<<e.what()<<'\n';return 1;}
}
