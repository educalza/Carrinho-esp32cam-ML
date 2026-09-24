#include "../teacher_policy.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
uint8_t picture[teacher::W*teacher::H];
void line(int offset, float slope=0) {
    std::fill_n(picture,sizeof(picture),220);
    for(int y=0;y<120;++y) {
        int center=80+offset+(int)((100-y)*slope);
        for(int x=center-4;x<center+4;++x) if(x>=0 && x<160) picture[y*160+x]=20;
    }
}
int main(int argc, char** argv) {
    if(argc==2 && std::string(argv[1])=="--frame") {
        for(auto& p:picture) {int value;if(!(std::cin>>value)) return 2;p=static_cast<uint8_t>(value);}
        auto o=teacher::inspect(picture,sizeof(picture));
        std::cout<<"valid="<<o.valid<<" reason="<<o.reason<<" rows="<<o.rows<<" threshold="<<o.threshold<<" near="<<o.nearX<<" far="<<o.farX<<std::endl;
        return o.valid?0:1;
    }
    assert(!teacher::inspect(nullptr,0).valid);
    std::fill_n(picture,sizeof(picture),0); assert(!teacher::inspect(picture,sizeof(picture)).valid);
    std::fill_n(picture,sizeof(picture),220); assert(!teacher::inspect(picture,sizeof(picture)).valid);
    line(0); auto straight=teacher::inspect(picture,sizeof(picture)); assert(straight.valid && std::fabs(straight.error)<0.001f);
    line(20,0.3f); auto right=teacher::inspect(picture,sizeof(picture)); assert(right.valid && right.bend>0);
    line(-20,-0.3f); auto left=teacher::inspect(picture,sizeof(picture)); assert(left.valid && left.bend<0);
    teacher::Controller c; auto s=c.update(straight,0.03f); c.reset(); auto r=c.update(right,0.03f); c.reset(); auto l=c.update(left,0.03f);
    assert(r.steer>0 && l.steer<0 && r.throttle<s.throttle && l.throttle>0.02f);
    assert(c.update(left,0.3f).throttle==0);
    // Central stripe with a dark floor touching the right border (real failure).
    line(8);for(int y=0;y<120;++y) for(int x=125;x<160;++x) picture[y*160+x]=70;
    auto floor=teacher::inspect(picture,sizeof(picture));
    assert(floor.valid && floor.nearX>85 && floor.nearX<90);
    // Strong bends leave the upper image, but retain a usable lower segment.
    line(0,1.5f); auto sharpRight=teacher::inspect(picture,sizeof(picture));
    assert(sharpRight.valid && sharpRight.partial && sharpRight.bend>0);
    c.reset(); assert(c.update(sharpRight,0.03f).steer>r.steer);
    line(0,-1.5f); auto sharpLeft=teacher::inspect(picture,sizeof(picture));
    assert(sharpLeft.valid && sharpLeft.partial && sharpLeft.bend<0);
    // A crossbar obscures the track briefly; require its visible continuation.
    line(0); for(int y=64;y<=80;++y) for(int x=0;x<160;++x) picture[y*160+x]=20;
    auto cross=teacher::inspect(picture,sizeof(picture));
    assert(cross.valid && cross.crossing && std::fabs(cross.error)<0.001f);
    c.reset(); assert(c.update(cross,0.03f).throttle==0.10f);
    // Missing crossing exit and excessive occlusion must still stop.
    for(int y=0;y<64;++y) for(int x=0;x<160;++x) picture[y*160+x]=220;
    assert(!teacher::inspect(picture,sizeof(picture)).valid);
    line(0); for(int y=40;y<=96;++y) for(int x=0;x<160;++x) picture[y*160+x]=20;
    assert(!teacher::inspect(picture,sizeof(picture)).valid);
    line(0); for(int y=0;y<120;++y) for(int x=110;x<118;++x) picture[y*160+x]=20;
    assert(!teacher::inspect(picture,sizeof(picture)).valid); // Ambiguous parallel line.
    line(0); for(int y=0;y<120;++y) for(int x=40;x<120;++x) picture[y*160+x]=20;
    assert(!teacher::inspect(picture,sizeof(picture)).valid); // Wide shadow.
    // Thick tape, clipped curve, and a line whose lower section is out of view.
    line(0); for(int y=0;y<120;++y) for(int x=60;x<100;++x) picture[y*160+x]=20;
    assert(teacher::inspect(picture,sizeof(picture)).valid);
    line(-76); assert(teacher::inspect(picture,sizeof(picture)).valid);
    line(0); for(int y=96;y<120;++y) for(int x=0;x<160;++x) picture[y*160+x]=220;
    assert(teacher::inspect(picture,sizeof(picture)).valid);
    // Crossbar moves through the bottom region: previous trajectory supplies anchor.
    auto previous=straight;
    for(int bottom=76;bottom<=128;bottom+=4) {
        line(0);for(int y=bottom-28;y<=bottom && y<120;++y) for(int x=0;x<160;++x) picture[y*160+x]=20;
        auto result=teacher::inspect(picture,sizeof(picture),&previous);
        assert(result.valid && std::fabs(result.error)<0.001f);previous=result;
    }
    // Complete blindness must not be accepted even with a previous trajectory.
    std::fill_n(picture,sizeof(picture),20);
    assert(!teacher::inspect(picture,sizeof(picture),&previous).valid);
    teacher::Gate g; assert(!g.start());
    for(int i=0;i<5;++i) g.observe(true,i*20);
    assert(g.start()); g.observe(false,100); assert(g.armed() && g.paused());
    g.observe(true,160); assert(g.armed() && !g.paused());
    g.observe(false,180); g.observe(false,300); assert(!g.armed());
    for(int i=0;i<5;++i) g.observe(true,320+i*20);
    assert(!g.armed()); // Never automatically rearms after recovery.
    assert(g.start());g.observe(false,500);g.observe(true,621);assert(!g.armed());
    puts("teacher: detector, control and stop tests passed");
}
