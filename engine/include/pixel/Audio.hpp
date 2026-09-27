#pragma once
#include <memory>
namespace pixel {
// Procedural audio has no external media dependency and keeps volume controls functional.
class Audio {
public:
    Audio(); ~Audio();
    void setVolumes(float master,float ambience,float effects,float music);
    void setEnvironment(bool playing,float daylight,bool moving);
    void trigger(float pitch=1);
    bool available() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
