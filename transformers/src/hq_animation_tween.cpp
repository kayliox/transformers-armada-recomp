#include "hq_animation_tween.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#ifndef DECEPTICON_FLIGHT_TEST
#include <algorithm>
#include <cmath>
#endif
namespace hq_animation_tween {
namespace {
constexpr uint32_t target=0x151a30,ram_size=PS2_RAM_SIZE;
PS2Runtime* installed=nullptr;
PS2Runtime::RecompiledFunction original=nullptr;
bool tracing(){
#ifdef _MSC_VER
 char value[8]{};std::size_t length=0;
 return getenv_s(&length,value,sizeof(value),"TF_DECEPTICON_TRACE")==0&&length==2&&value[0]=='1';
#else
 const char* value=std::getenv("TF_DECEPTICON_TRACE");return value&&value[0]=='1'&&!value[1];
#endif
}
bool span(std::size_t size,uint32_t p,std::size_t n){return !(p&3)&&p>=0x100000&&p<=size&&n<=size-p;}
uint32_t word(const uint8_t*r,uint32_t p){uint32_t v;std::memcpy(&v,r+p,4);return v;}
int16_t half(const uint8_t*r,uint32_t p){int16_t v;std::memcpy(&v,r+p,2);return v;}
bool text(const uint8_t*r,std::size_t size,uint32_t p,const char* expected){
 if(!span(size,p,12))return false;const std::size_t n=std::strlen(expected);uint32_t data=p+1;
 if(r[p]&1){if(word(r,p+4)!=n)return false;data=word(r,p+8);if(data<0x100000||data>size||n+1>size-data)return false;}
 else if((r[p]>>1)!=n||n>10)return false;
 return std::memcmp(r+data,expected,n)==0&&r[data+n]==0;
}
bool vector(const uint8_t*r,std::size_t size,uint32_t p,uint32_t& data,uint32_t& count){
 if(!span(size,p,12))return false;const uint32_t capacity=word(r,p);count=word(r,p+4);data=word(r,p+8);
 return count>0&&count<=capacity&&capacity<=512&&span(size,data,std::size_t(count)*4);
}
}
bool (*g_decepticonHQ)(const uint8_t*) = nullptr;
#ifndef DECEPTICON_FLIGHT_TEST
constexpr uint32_t updateActors = 0x154860;
PS2Runtime::RecompiledFunction originalUpdate = nullptr;

void blendMegatronTreads(uint8_t* ram, float dt)
{
    static uint32_t previousActor = 0;
    static float weight = 0.0f;
    const uint32_t hq = word(ram, 0x6cf688);
    const uint32_t cameraHandler = word(ram, 0x6cf808);
    uint32_t models = 0, count = 0;
    if (!g_decepticonHQ || !g_decepticonHQ(ram) ||
        !span(ram_size, hq, 0x54) || word(ram, hq) != 0x6b7360 ||
        !span(ram_size, cameraHandler, 0x80) || word(ram, cameraHandler) != 0x6bb560 ||
        word(ram, cameraHandler + 4) != word(ram, hq + 12) ||
        word(ram, cameraHandler + 0x60) != word(ram, hq + 20) ||
        !vector(ram, ram_size, word(ram, 0x6cf7e0), models, count)) {
        previousActor = 0;
        weight = 0.0f;
        return;
    }

    uint32_t actor = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t model = word(ram, models + 4 * i);
        if (span(ram_size, model, 0x24) && word(ram, model) == 0x6bb320 &&
            word(ram, model + 0x10) == word(ram, hq + 12) &&
            text(ram, ram_size, model + 4, "Char_Optimus")) {
            actor = word(ram, model + 0x20);
            break;
        }
    }
    if (actor != previousActor) {
        previousActor = actor;
        weight = 0.0f;
    }
    if (!span(ram_size, actor, 0x9c) || word(ram, actor) != 0x6af490 ||
        word(ram, actor + 0x94) != 82 || !std::isfinite(dt)) return;
    const uint32_t bones = word(ram, actor + 0x98);
    if (!span(ram_size, bones, 82 * 0x70)) return;

    const uint32_t camera = word(ram, cameraHandler + 0x64);
    const uint32_t waypoint = span(ram_size, camera, 0x74) && word(ram, camera) == 0x6be6d0
        ? word(ram, camera + 0x70) : 0;
    constexpr char picker[] = "UICharacterSelectCamOptimusPrime";
    const uint32_t waypointName = span(ram_size, waypoint, 0x44) ? word(ram, waypoint + 0x40) : 0;
    const bool angled = waypointName >= 0x100000 && waypointName <= ram_size - sizeof(picker) &&
        word(ram, waypoint) == 0x6be690 && word(ram, waypoint + 4) == cameraHandler &&
        std::memcmp(ram + waypointName, picker, sizeof(picker)) == 0;
    const float step = std::clamp(dt, 0.0f, 0.05f) * 5.0f;
    const float next = std::clamp(weight + (angled ? step : -step), 0.0f, 1.0f);
    if (next == 0.0f && weight == 0.0f) return;

    uint32_t treads[2]{};
    for (unsigned i = 0; i < 82; ++i) {
        const uint32_t bone = bones + i * 0x70;
        const uint32_t neutral = word(ram, bone + 0x54);
        if (!span(ram_size, neutral, 0x58)) continue;
        const uint32_t name = word(ram, neutral + 0x54);
        const bool left = text(ram, ram_size, name, "LHQTread");
        if (!left && !text(ram, ram_size, name, "RHQTread")) continue;
        const unsigned side = left ? 0 : 1;
        constexpr float identity[] = {1.0f, 0.0f, 0.0f, 0.0f};
        if (treads[side] || std::memcmp(ram + neutral + 0x40, identity, sizeof(identity)) != 0)
            return;
        treads[side] = bone;
    }
    if (!treads[0] || !treads[1]) return;
    weight = next;
    const float eased = weight * weight * (3.0f - 2.0f * weight);
    const float halfAngle = eased * 0.4188790205f;
    const float sine = std::sin(halfAngle);
    const float axes[2][3] = {{-0.9890769f, -0.0000540f, -0.1474074f},
                              {0.9890751f, -0.0000350f, 0.1474195f}};
    for (unsigned i = 0; i < 2; ++i) {
        const float rotation[] = {std::cos(halfAngle), axes[i][0] * sine,
                                  axes[i][1] * sine, axes[i][2] * sine};
        std::memcpy(ram + treads[i] + 0x40, rotation, sizeof(rotation));
        ram[treads[i] + 0x0c] = 0; // Rebuild the cached local matrix on demand.
    }
}

// TF_HQ_GESTURE_TRACE=1: one stderr line per HQ actor update while a UI clip
// controller of a seated actor plays (clock, clip length, fade state). Read only.
bool gestureTracing()
{
    static int cached = -1;
    if (cached < 0) {
        const char* value = std::getenv("TF_HQ_GESTURE_TRACE");
        cached = value && value[0] == '1' && !value[1];
    }
    return cached == 1;
}

void clipName(const uint8_t* r, uint32_t p, char (&out)[24])
{
    out[0] = 0;
    if (!span(ram_size, p, 12)) return;
    uint32_t data = p + 1, n = r[p] >> 1;
    if (r[p] & 1) { n = word(r, p + 4); data = word(r, p + 8); }
    if (n > 22 || data < 0x100000 || data > ram_size - 24) return;
    std::memcpy(out, r + data, n);
    out[n] = 0;
}

void traceGestures(uint8_t* r, float dt)
{
    static unsigned lines = 0, frame = 0;
    ++frame;
    if (lines > 20000) return;
    uint32_t models = 0, count = 0;
    const uint32_t hq = word(r, 0x6cf688);
    if (!g_decepticonHQ || !g_decepticonHQ(r) || !span(ram_size, hq, 0x54) || word(r, hq) != 0x6b7360 ||
        !vector(r, ram_size, word(r, 0x6cf7e0), models, count)) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t model = word(r, models + 4 * i);
        if (!span(ram_size, model, 0x24) || word(r, model) != 0x6bb320) continue;
        char who[24];
        clipName(r, model + 4, who);
        if (std::strncmp(who, "Char_", 5)) continue;
        const uint32_t actor = word(r, model + 0x20);
        uint32_t list = 0, used = 0;
        if (!span(ram_size, actor, 0xa0) || word(r, actor) != 0x6af490 || !vector(r, ram_size, actor + 0x64, list, used)) continue;
        char line[512];
        int at = std::snprintf(line, sizeof line, "[HQ gesture] f=%u dt=%.4f %s", frame, dt, who + 5);
        bool ack = false;
        for (uint32_t k = 0; k < used && at < 440; ++k) {
            const uint32_t c = word(r, list + 4 * k);
            if (!span(ram_size, c, 0x34) || word(r, c) != 0x6af250) continue;
            const uint32_t source = word(r, c + 0xc);
            if (!span(ram_size, source, 0x1c)) continue;
            char clip[24];
            clipName(r, source + 4, clip);
            if (std::strncmp(clip, "UI", 2)) continue;
            if (!std::strncmp(clip, "UIAck", 5)) ack = true;
            float time = 0, length = 0;
            std::memcpy(&time, r + c + 4, 4);
            std::memcpy(&length, r + source + 0x18, 4);
            at += std::snprintf(line + at, sizeof line - at, " | %s t=%.3f/%.3f f%02x s%d p%d/%d", clip, time, length,
                                r[c + 0x14], r[c + 0x16], half(r, c + 0x1c), half(r, c + 0x20));
        }
        static int tail[3] = {};
        const unsigned slot = who[5] == 'O' ? 0 : who[5] == 'H' ? 1 : 2;
        if (ack) tail[slot] = 90;  // keep tracing the hand-off back to the idle
        if (tail[slot] > 0) { std::fprintf(stderr, "%s\n", line); ++lines; if (!ack) --tail[slot]; }
    }
}

void updateTreads(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime)
{
    const uint32_t caller = getRegU32(ctx, 31);
    const uint32_t stack = getRegU32(ctx, 29);
    const bool hqUpdate = runtime == installed && ctx->pc == updateActors && caller == 0x333898;
    const auto thread = hqUpdate ? runtime->eeScheduler().currentThreadId() : 0;
    const float dt = ctx->f[12];
    originalUpdate(ram, ctx, runtime);
    if (hqUpdate && ctx->pc == caller && getRegU32(ctx, 29) == stack &&
        runtime->eeScheduler().currentThreadId() == thread)
    {
        blendMegatronTreads(ram, dt);
        if (gestureTracing()) traceGestures(ram, dt);
    }
}
#endif
bool preserve_pending(const uint8_t*r,std::size_t size,uint32_t c,uint32_t caller){
 // Original inline Update151AD0 and standalone UpdateTween1511B0 callers.
 if(!r||size<0x6cf7e4||(caller!=0x151c6c&&caller!=0x151330)||!span(size,c,0x34))return false;
 if(word(r,c)!=0x6af250||word(r,0x6af258)!=target||!(r[c+0x14]&1)||r[c+0x16]!=2||
    half(r,c+0x1a)!=0||half(r,c+0x1c)<0||half(r,c+0x1c)>=1000||half(r,c+0x20)<=0||
    half(r,c+0x20)>1000||half(r,c+0x22)<=0)return false;
 const uint32_t hq=word(r,0x6cf688),handler=word(r,0x6cf7e0),actor=word(r,c+0x10),source=word(r,c+0xc);
 if(!span(size,hq,0x80)||word(r,hq)!=0x6b7360||!span(size,actor,0xa0)||word(r,actor)!=0x6af490||!span(size,source,16))return false;
 bool ui=false;for(const char* name:{"UIIdle01","UIIdle02","UIAcknowledge01","UIAcknowledge02","UIAcknowledge03"})if(text(r,size,source+4,name))ui=true;
 if(!ui)return false;
 uint32_t list=0,count=0;if(!vector(r,size,actor+0x64,list,count))return false;
 bool member=false;for(uint32_t i=0;i<count;++i)if(word(r,list+i*4)==c)member=true;if(!member)return false;
 if(!vector(r,size,handler,list,count))return false;
 for(uint32_t i=0;i<count;++i){const uint32_t item=word(r,list+i*4);if(!span(size,item,0x24)||word(r,item)!=0x6bb320||word(r,item+0x20)!=actor)continue;
  if(!text(r,size,item+4,"Char_Optimus")&&!text(r,size,item+4,"Char_HotShot")&&!text(r,size,item+4,"Char_RedAlert"))continue;
  const uint32_t parent=word(r,item+0x10);if(!span(size,parent,0x44))return false;
  uint32_t node=actor;for(unsigned depth=0;depth<32;++depth){if(node==parent)return true;if(!span(size,node,8))break;const uint32_t next=word(r,node+4);if(next==node)break;node=next;}
 }
 return false;
}
void dispatch(uint8_t*r,R5900Context*c,PS2Runtime*rt){
 if(rt!=installed||!original)return;
 const uint32_t self=getRegU32(c,4),caller=getRegU32(c,31);
 if(getRegU32(c,5)==0&&getRegU32(c,16)==self&&preserve_pending(r,ram_size,self,caller)){
  SET_GPR_U32(c,5,1);static unsigned traces=0;
  if(tracing()&&traces++<32)std::fprintf(stderr,"[hq animation tween] retain pending fade controller=%08x actor=%08x progress=%d target=%d caller=%08x\n",self,word(r,self+0x10),half(r,self+0x1c),half(r,self+0x20),caller);
 }
 // Exactly once, even when it yields. No post-return writes or invented unwind.
 original(r,c,rt);
}
void setDecepticonHQ(bool (*active)(const uint8_t*)){g_decepticonHQ=active;}
bool install(PS2Runtime&rt){if(installed)return installed==&rt;if(!rt.hasFunction(target))return false;auto previous=rt.lookupFunction(target);if(!previous||previous==dispatch||!rt.registerFunction(target,dispatch))return false;original=previous;
#ifndef DECEPTICON_FLIGHT_TEST
 originalUpdate=rt.lookupFunction(updateActors);
 if(!originalUpdate||!rt.registerFunction(updateActors,updateTreads))return false;
#endif
 installed=&rt;return true;}
}
