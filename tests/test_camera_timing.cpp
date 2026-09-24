#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include "../firmware/esp32cam_autonomous/camera_profile.h"
#include "../firmware/esp32cam_autonomous/camera_timing.h"

struct Sensor {
    struct { int PID=0x26; } id;
    int value=3, writes=0;
    bool failFast=false, failRestore=false, badReadback=false, readFailure=false;
    int (*get_reg)(Sensor*,int,int)=read;
    int (*set_reg)(Sensor*,int,int,int)=write;
    static int read(Sensor* s,int reg,int mask) {
        assert(reg==0x111 && mask==0xff); // bank 1, not DSP register 0x11
        if(s->readFailure) return -1;
        return s->badReadback && s->writes==1 ? 0 : s->value;
    }
    static int write(Sensor* s,int reg,int mask,int value) {
        assert(reg==0x111);
        ++s->writes;
        if((mask==0x3f && s->failFast) || (mask==0xff && s->failRestore)) return -1;
        s->value=(s->value & ~mask)|(value & mask);
        return 0;
    }
};
int main() {
    using namespace car_camera;
    Sensor sensor;
    auto r=configureClock(&sensor,false);
    assert(r.ok && !r.fast && sensor.writes==0 && sensor.value==3);
    sensor.value=0x43; // Preserve reserved/high bits when changing the divider.
    r=configureClock(&sensor,true);
    assert(r.ok && r.fast && r.original==0x43 && sensor.value==0x41);
    for(int other:{0,1,2,7,0x83}) {
        sensor=Sensor(); sensor.value=other;
        r=configureClock(&sensor,true);
        assert(r.ok && !r.fast && sensor.writes==0 && sensor.value==other);
    }
    sensor=Sensor(); sensor.id.PID=0x3660;
    assert(!configureClock(&sensor,true).fast && sensor.writes==0);
    sensor=Sensor(); sensor.readFailure=true;
    assert(!configureClock(&sensor,true).fast && sensor.writes==0);
    sensor=Sensor(); sensor.failFast=true;
    r=configureClock(&sensor,true);
    assert(r.ok && !r.fast && sensor.value==3 && sensor.writes==2);
    sensor=Sensor(); sensor.badReadback=true;
    r=configureClock(&sensor,true);
    assert(r.ok && !r.fast && sensor.value==3 && sensor.writes==2);
    sensor=Sensor(); sensor.badReadback=true; sensor.failRestore=true;
    assert(!configureClock(&sensor,true).ok); // Never drive with unconfirmed clock state.
    sensor=Sensor(); sensor.set_reg=nullptr;
    assert(!configureClock(&sensor,true).fast && sensor.writes==0);

    Timing timing;
    // Fresh acquisition: request-to-start plus start-to-delivery equals get().
    assert(timing.observe(100000,223500,143000));
    assert(timing.window().getUs==123500 && timing.window().startRelativeUs==43000);
    assert(timing.window().deliveryAgeUs==80500 && timing.window().gaps==0);
    // A frame may have begun before the application asks for it; retain the sign.
    assert(timing.observe(310000,323000,303000));
    assert(timing.window().startRelativeUs==36000 && timing.window().frameGapUs==160000);
    assert(timing.window().getUs==timing.window().startRelativeUs+timing.window().deliveryAgeUs);
    assert(timing.window().maxDeliveryAgeUs==80500);
    assert(!timing.observe(400000,480000,303000)); // duplicate
    assert(!timing.observe(400000,480000,300000)); // backwards
    assert(!timing.observe(400000,480000,480001)); // future
    assert(!timing.observe(400000,399999,350000)); // invalid request interval
    assert(timing.window().invalid==4 && timing.window().frames==2);
    timing.failure(); timing.invalidFrame(); timing.lateFrame();
    assert(timing.window().failures==1 && timing.window().invalid==5 && timing.window().late==1);
    timing.clearWindow();
    assert(timing.window().frames==0 && timing.window().invalid==0);
    assert(timing.observe(480000,543000,463000));
    assert(timing.window().gaps==1 && timing.window().frameGapUs==160000);
    assert(timing.window().startRelativeUs==-17000);
    std::puts("camera: clock selection/readback/rollback and capture timing passed");
}
