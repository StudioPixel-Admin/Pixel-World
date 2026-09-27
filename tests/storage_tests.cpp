#include "pixel/Storage.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    const auto root=std::filesystem::temp_directory_path()/("pixel-storage-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try{
        pixel::Storage store(root);
        require(store.loadSettings().viewDistance==3,"Missing preferences must use conservative defaults");
        pixel::Settings settings;settings.masterVolume=.23f;settings.viewDistance=2;settings.invertY=true;
        store.saveSettings(settings);const auto loaded=store.loadSettings();
        require(loaded.masterVolume==.23f && loaded.viewDistance==2 && loaded.invertY,"Preferences roundtrip");
        auto a=store.createWorld("A world / with a display name",732);auto b=store.createWorld("Second world",733);
        require(a.info.id!=b.info.id && store.worlds().size()==2,"Worlds must be isolated");
        a.position={-123.5f,18.25f,52};a.inventory={8,12,1,2};a.timeOfDay=17.5f;
        a.edits.push_back({{-24,16,24},2.5f,4});a.camps.push_back({12,16,25});store.saveWorld(a);
        const auto c=store.loadWorld(a.info.id);
        require(c.position==a.position && c.inventory==a.inventory && c.timeOfDay==17.5f,"Player state roundtrip");
        require(c.edits.size()==1 && c.edits[0].center==a.edits[0].center && c.camps.size()==1,"Terrain/camp roundtrip");
        require(store.loadWorld(b.info.id).edits.empty(),"World edits must not leak");
        // Create a known backup then truncate the current file, as a disk interruption could.
        store.saveWorld(a);
        {std::ofstream out(root/"worlds"/a.info.id/"world.json");out<<"{corrupt";}
        require(store.loadWorld(a.info.id).edits.size()==1,"Backup recovery");
        bool rejected=false;try{store.loadWorld("../outside");}catch(...){rejected=true;}
        require(rejected,"Traversal must be rejected");
        {std::ofstream out(root/"settings.json");out<<"{\"viewDistance\":999,\"masterVolume\":-5}";}
        require(store.loadSettings().viewDistance==6 && store.loadSettings().masterVolume==0,"Clamp invalid settings");
        std::filesystem::remove_all(root);
        std::cout<<"PASS: settings, isolated worlds, edits, camps, atomic save recovery and path validation\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\nTest data retained at "<<root<<'\n';return 1;}
}
