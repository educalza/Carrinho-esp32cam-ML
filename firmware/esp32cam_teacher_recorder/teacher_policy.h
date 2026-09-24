#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace teacher {
constexpr int W = 160, H = 120;
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
struct Observation {
    bool valid = false;
    float nearX = 0, midX = 0, farX = 0, error = 0, bend = 0, confidence = 0;
    bool crossing = false, partial = false;
    unsigned rows = 0;
    int threshold = 0;
    const char* reason = "imagem invalida";
};
// Five rows per zone. Each row must contain exactly one bounded dark run.
// Multiple branches, broad shadows and uniform images are rejected.
inline bool rowCenter(const uint8_t* row, float& center) {
    unsigned hist[256] = {};
    for (int x = 0; x < W; ++x) ++hist[row[x]];
    unsigned n = 0; int low = -1, high = 255;
    for (int i = 0; i < 256; ++i) {
        n += hist[i];
        if (low < 0 && n >= 3) low = i;
        if (n >= 128) { high = i; break; }
    }
    if (high - low < 30) return false;
    const int threshold = low + (high - low) * 2 / 5;
    int candidates = 0;
    for (int x = 0; x < W;) {
        if (row[x] > threshold) { ++x; continue; }
        const int start = x;
        int sum = 0;
        while (x < W && row[x] <= threshold) sum += row[x++];
        const int width = x - start;
        if (width < 3 || width > 30 || start < 3 || x > W - 3) continue;
        const int left = (row[start-1]+row[start-2]+row[start-3])/3;
        const int right = (row[x]+row[x+1]+row[x+2])/3;
        if ((left < right ? left : right) - sum/width < 30) continue;
        center = (start + x - 1) * 0.5f;
        ++candidates;
    }
    return candidates == 1;
}
inline bool zone(const uint8_t* image, int y, float& center, unsigned& support) {
    float sum = 0, previous = 0; support = 0;
    for (int k = -4; k <= 4; k += 2) {
        float value = 0;
        if (!rowCenter(image + (y+k)*W, value)) continue;
        if (support && std::fabs(value - previous) > 10) return false;
        previous = value; sum += value; ++support;
    }
    if (support < 4) return false;
    center = sum / support;
    return true;
}
inline Observation inspect(const uint8_t* image, size_t length, const Observation* previous=nullptr) {
    Observation o;
    if (!image || length != W*H) return o;
    // One robust threshold for the ROI: a crossbar covering an entire row
    // has no local contrast, but must not destroy the threshold for that row.
    unsigned hist[256]={};
    for(int y=28;y<=112;y+=4) for(int x=0;x<W;++x) ++hist[image[y*W+x]];
    unsigned total=0; int low=-1,high=255;
    for(int i=0;i<256;++i) {
        total+=hist[i];
        if(low<0 && total>=35) low=i;
        if(total>=2992) {high=i;break;}
    }
    o.threshold=low+(high-low)*2/5;
    if(high-low<25) {o.reason="contraste insuficiente";return o;}
    int count=0, firstY=0, lastY=0, wideGap=0, whiteGap=0;
    float firstX=0, lastX=0, slope=0;
    float sumY=0, sumX=0, sumYY=0, sumYX=0;
    bool bridged=false, clipped=false;
    for (int y=112; y>=28; y-=4) {
        const uint8_t* row=image+y*W;
        float centers[W/2]={}; bool edges[W/2]={}; int candidates=0;
        bool wide=false;
        for(int x=0;x<W;) {
            if(row[x]>o.threshold) {++x;continue;}
            const int start=x;
            while(x<W && row[x]<=o.threshold) ++x;
            const int width=x-start;
            if(width>52) {wide=true;continue;}
            if(width<2) continue;
            int sum=0;for(int j=start;j<x;++j) sum+=row[j];
            int flanks=0, sides=0;
            if(start>=3) {flanks+=(row[start-1]+row[start-2]+row[start-3])/3;++sides;}
            if(x<=W-3) {flanks+=(row[x]+row[x+1]+row[x+2])/3;++sides;}
            if(!sides || flanks/sides-sum/width<25) continue;
            centers[candidates]=(start+x-1)*0.5f;
            edges[candidates]=(start==0 || x==W);
            ++candidates;
        }
        if(wide && !candidates) {
            wideGap+=4;
            if(wideGap>48) {o.reason="faixa larga demais sem saida";return o;}
            continue;
        }
        if(!candidates) {
            if(wideGap) {o.reason="cruzamento sem saida visivel";return o;}
            whiteGap+=4;
            if(count && whiteGap>8) break;
            continue;
        }
        // Acquisition: prefer a stripe with two visible boundaries over floor
        // regions cut by the image edge. If only clipped evidence exists, keep it.
        if(!count && !previous) {
            int bounded=0;
            for(int i=0;i<candidates;++i) if(!edges[i]) ++bounded;
            if(bounded) {
                int kept=0;
                for(int i=0;i<candidates;++i) if(!edges[i]) {centers[kept]=centers[i];edges[kept++]=false;}
                candidates=kept;
            }
        }
        float predicted=count?lastX+slope*(lastY-y):
            previous&&previous->valid?previous->nearX+(100-y)*(previous->farX-previous->nearX)/60:79.5f;
        int selected=0; float best=1000,second=1000;
        for(int i=0;i<candidates;++i) {
            const float distance=std::fabs(centers[i]-predicted);
            if(distance<best) {second=best;best=distance;selected=i;} else if(distance<second) second=distance;
        }
        if(candidates>1 && ((!count && !previous) || second-best<8)) {
            o.reason="mais de uma continuacao possivel";return o;
        }
        const float center=centers[selected];
        if (count) {
            const float dy=(float)(lastY-y);
            if (std::fabs(center-(lastX+slope*dy))>18.0f) {
                o.reason="salto na posicao da linha";return o;
            }
            const float localSlope=(center-lastX)/dy;
            if (std::fabs(localSlope)>3.5f) {o.reason="mudanca de direcao abrupta";return o;}
            slope=count==1?localSlope:0.5f*slope+0.5f*localSlope;
        } else {
            if(wideGap && (!previous || !previous->valid || best>24)) {
                o.reason="cruzamento na base sem referencia anterior";return o;
            }
            firstY=y; firstX=center;
        }
        if(wideGap) bridged=true;
        clipped=clipped||edges[selected];
        wideGap=0; whiteGap=0;
        ++count; lastY=y; lastX=center;
        o.rows=count;
        sumY+=y; sumX+=center; sumYY+=y*y; sumYX+=y*center;
    }
    if(wideGap) {o.reason="cruzamento sem saida visivel";return o;}
    if(count<5 || firstY<64 || firstY-lastY<20) {o.reason="trecho visivel curto ou distante";return o;}
    const float denominator=count*sumYY-sumY*sumY;
    if(denominator<=0) return o;
    const float m=(count*sumYX-sumY*sumX)/denominator;
    const float intercept=(sumX-m*sumY)/count;
    o.nearX=clamp(intercept+m*100,0,W-1);
    o.midX=clamp(intercept+m*70,0,W-1);
    o.farX=clamp(intercept+m*40,0,W-1);
    o.bend=(lastX-firstX)/80.0f;
    o.error=(0.45f*o.nearX+0.55f*o.farX-79.5f)/80.0f;
    o.confidence=count/22.0f; // Evidence coverage, not probability.
    o.crossing=bridged;
    o.partial=lastY>40 || firstY<96 || clipped;
    o.reason=bridged?"cruzamento com continuacao":o.partial?"linha parcialmente visivel":"linha acompanhada";
    o.valid = true;
    return o;
}
struct Command { float steer = 0, throttle = 0; };
class Controller {
    bool previousValid = false;
    float previous = 0;
public:
    void reset() { previousValid = false; previous = 0; }
    Command update(const Observation& o, float dt) {
        if (!o.valid || !std::isfinite(dt) || dt <= 0 || dt > 0.2f) { reset(); return {}; }
        const float derivative = previousValid ? (o.error-previous)/dt : 0;
        previous = o.error; previousValid = true;
        Command cmd;
        cmd.steer = clamp(1.4f*o.error + 0.025f*derivative + 0.35f*o.bend, -1, 1);
        const float severity = clamp(std::fabs(cmd.steer)+std::fabs(o.bend),0,1);
        // Current Dev firmware maps these values to approximately PWM 120..103.
        cmd.throttle = 0.20f - 0.10f*severity;
        if(o.crossing || o.partial) cmd.throttle=0.10f;
        return cmd;
    }
};
class Gate {
    unsigned good = 0;
    bool active = false;
    bool waiting = false;
    uint32_t missingSince = 0;
public:
    void observe(bool valid, uint32_t now) {
        if(waiting && static_cast<uint32_t>(now-missingSince)>=120) stop();
        if(!valid) {
            good=0;
            if(active && !waiting) {waiting=true;missingSince=now;}
        } else {waiting=false;if(good<5) ++good;}
    }
    bool start() { active = good >= 5; return active; }
    void stop() { active = false; good = 0; waiting=false; }
    bool armed() const { return active; }
    bool paused() const { return waiting; }
    bool ready() const { return good >= 5; }
    unsigned validFrames() const { return good; }
};
}
