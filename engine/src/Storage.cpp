#include "pixel/Storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pixel {
using Json = nlohmann::json;
namespace {
std::string timestamp() {
    const auto raw = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &raw);
#else
    gmtime_r(&raw, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M UTC");
    return out.str();
}
bool validId(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-';
    });
}
Json read(const std::filesystem::path& file) {
    if (std::filesystem::file_size(file) > 8 * 1024 * 1024) throw std::runtime_error("Save file exceeds 8 MB safety limit.");
    std::ifstream in(file);
    if (!in) throw std::runtime_error("Cannot read " + file.string());
    Json result; in >> result; return result;
}
// Flush the temporary file before atomic replacement. Keep one previous complete save
// so a damaged JSON file can be recovered without destroying the user's world.
void atomicWrite(const std::filesystem::path& file, const Json& json) {
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file; temporary += ".tmp";
    { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << json.dump(2) << '\n'; out.flush();
      if (!out) throw std::runtime_error("Could not write save; check free disk space."); }
    if (std::filesystem::exists(file)) {
        auto backup = file; backup += ".bak";
        std::filesystem::copy_file(file, backup, std::filesystem::copy_options::overwrite_existing);
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace save file (Windows error " + std::to_string(GetLastError()) + ").");
#else
    std::filesystem::rename(temporary, file);
#endif
}
float bounded(const Json& j, const char* key, float fallback, float low, float high) {
    float value = j.value(key, fallback);
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
glm::vec3 vec(const Json& j) {
    if (!j.is_array() || j.size() != 3) throw std::runtime_error("Invalid position in save.");
    glm::vec3 v(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
        std::abs(v.x) > 100000 || std::abs(v.z) > 100000 || std::abs(v.y) > 10000)
        throw std::runtime_error("Save position is outside supported world bounds.");
    return v;
}
Json array(glm::vec3 v) { return Json::array({v.x, v.y, v.z}); }
WorldSave decode(const Json& j, const std::string& id) {
    if (j.at("version").get<int>() != 1) throw std::runtime_error("Unsupported world save version.");
    WorldSave w;
    w.info.id = id; w.info.name = j.at("name").get<std::string>().substr(0, 80);
    w.info.seed = j.at("seed").get<uint32_t>(); w.info.savedAt = j.value("savedAt", "Unknown");
    w.info.playedSeconds = bounded(j, "playedSeconds", 0, 0, 1e9f);
    w.position = vec(j.at("position"));
    w.yaw = bounded(j,"yaw",-90,-36000,36000); w.pitch = bounded(j,"pitch",-15,-89,89);
    w.timeOfDay = bounded(j,"timeOfDay",9,0,24); w.health = bounded(j,"health",100,0,100);
    w.stamina = bounded(j,"stamina",100,0,100); w.hunger = bounded(j,"hunger",100,0,100);
    if (j.contains("inventory")) {
        if (j["inventory"].size() != 4) throw std::runtime_error("Invalid inventory.");
        for (size_t i=0;i<4;++i) w.inventory[i] = std::clamp(j["inventory"][i].get<int>(),0,9999);
    }
    if (j.contains("edits")) {
        if (!j["edits"].is_array() || j["edits"].size() > 4096) throw std::runtime_error("Invalid terrain edit count.");
        for (const auto& e : j["edits"]) w.edits.push_back({vec(e.at("center")), bounded(e,"radius",2.5f,.5f,8), bounded(e,"strength",4,-16,16)});
    }
    if (j.contains("camps")) {
        if (!j["camps"].is_array() || j["camps"].size()>128) throw std::runtime_error("Invalid camp count.");
        for (const auto& p:j["camps"]) w.camps.push_back(vec(p));
    }
    return w;
}
}

Storage::Storage(std::filesystem::path root) : root_(std::move(root)) { std::filesystem::create_directories(root_/"worlds"); }
std::filesystem::path Storage::defaultRoot() {
#ifdef _WIN32
    if (const auto p = std::getenv("LOCALAPPDATA")) return std::filesystem::path(p)/"PixelWorld";
#else
    if (const auto p = std::getenv("XDG_DATA_HOME")) return std::filesystem::path(p)/"pixel-world";
    if (const auto p = std::getenv("HOME")) return std::filesystem::path(p)/".local/share/pixel-world";
#endif
    return "userdata";
}
Settings Storage::loadSettings() const {
    Settings s;
    try {
        const auto j = read(root_/"settings.json");
        s.viewDistance=std::clamp(j.value("viewDistance",3),2,6);
        s.frameLimit=std::clamp(j.value("frameLimit",60),0,240);
        s.resolution=std::clamp(j.value("resolution",1),0,3);
        s.masterVolume=bounded(j,"masterVolume",.65f,0,1); s.ambienceVolume=bounded(j,"ambienceVolume",.5f,0,1);
        s.effectsVolume=bounded(j,"effectsVolume",.7f,0,1); s.musicVolume=bounded(j,"musicVolume",.25f,0,1);
        s.sensitivity=bounded(j,"sensitivity",.12f,.03f,.4f); s.fov=bounded(j,"fov",72,50,100);
        s.vsync=j.value("vsync",true); s.fullscreen=j.value("fullscreen",false);
        s.invertY=j.value("invertY",false); s.showStats=j.value("showStats",false);
    } catch (...) { /* Missing/corrupt preferences safely use the low-cost defaults. */ }
    return s;
}
void Storage::saveSettings(const Settings& s) const {
    atomicWrite(root_/"settings.json",{{"version",1},{"viewDistance",s.viewDistance},{"frameLimit",s.frameLimit},{"resolution",s.resolution},
        {"masterVolume",s.masterVolume},{"ambienceVolume",s.ambienceVolume},{"effectsVolume",s.effectsVolume},{"musicVolume",s.musicVolume},
        {"sensitivity",s.sensitivity},{"fov",s.fov},{"vsync",s.vsync},{"fullscreen",s.fullscreen},{"invertY",s.invertY},{"showStats",s.showStats}});
}
std::vector<WorldInfo> Storage::worlds() const {
    std::vector<WorldInfo> result;
    for (const auto& entry : std::filesystem::directory_iterator(root_/"worlds")) {
        if (!entry.is_directory() || !validId(entry.path().filename().string())) continue;
        try { result.push_back(loadWorld(entry.path().filename().string()).info); } catch (...) { /* One bad save cannot hide the others. */ }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){ return a.savedAt > b.savedAt || (a.savedAt==b.savedAt && a.id>b.id); });
    return result;
}
WorldSave Storage::createWorld(const std::string& name, uint32_t seed) const {
    if (name.empty() || name.size()>80 || name.find_first_not_of(" \t\r\n")==std::string::npos) throw std::runtime_error("Give your world a name.");
    WorldSave w; w.info.name=name; w.info.seed=seed;
    std::random_device random;
    // The display name never becomes a filesystem path (names may contain slashes).
    do { w.info.id = "world-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(random()); }
    while (std::filesystem::exists(root_/"worlds"/w.info.id));
    w.info.savedAt=timestamp(); saveWorld(w); return w;
}
WorldSave Storage::loadWorld(const std::string& id) const {
    if (!validId(id)) throw std::runtime_error("Invalid world identifier.");
    const auto file=root_/"worlds"/id/"world.json";
    try { return decode(read(file),id); }
    catch (const std::exception&) { auto backup=file; backup += ".bak"; if (std::filesystem::exists(backup)) return decode(read(backup),id); throw; }
}
void Storage::saveWorld(const WorldSave& w) const {
    if (!validId(w.info.id)) throw std::runtime_error("Invalid world identifier.");
    if(w.edits.size()>4096 || w.camps.size()>128) throw std::runtime_error("This prototype world's edit budget is full.");
    Json edits=Json::array(), camps=Json::array();
    for(const auto& e:w.edits) edits.push_back({{"center",array(e.center)},{"radius",e.radius},{"strength",e.strength}});
    for(const auto& p:w.camps) camps.push_back(array(p));
    atomicWrite(root_/"worlds"/w.info.id/"world.json",{{"version",1},{"name",w.info.name},{"seed",w.info.seed},{"savedAt",timestamp()},
        {"playedSeconds",w.info.playedSeconds},{"position",array(w.position)},{"yaw",w.yaw},{"pitch",w.pitch},{"timeOfDay",w.timeOfDay},
        {"health",w.health},{"stamina",w.stamina},{"hunger",w.hunger},{"inventory",w.inventory},{"edits",edits},{"camps",camps}});
}
}
