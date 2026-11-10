#include "runtime/ps2_stick_response.h"
#include <cstdio>
#include <initializer_list>
#include <cstring>
#include <limits>
using namespace ps2_stick_response;
float axis(uint8_t b) {
    const float f=guestRaw(b), center=guestRaw(128);
    if(std::fabs(f)<.2f)return 0;
    return f>center?(f-center)/(1-center):-(center-f)/(center+1);
}
float move(float x,float y,const Config& c,float w=0) {
    float rx=0,ry=0;map(x,y,rx,ry,c,w,.2f);
    float a=axis(axisByte(x)),b=axis(axisByte(y));
    float m=std::min(1.f,std::hypot(a,b));return m*m*m;
}
int main() {
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* what){++checks;if(!ok){if(failures<15)std::printf("FAIL %s\n",what);++failures;}};
    Config c;
#if defined(__linux__)
    for(int sign:{-1,1}) {
        float x=0,y=sign*.092f,rx=.12f,ry=-.17f;
        map(x,y,rx,ry,c,0,.2f,false,false);
        check(x==0&&y==sign*.092f&&rx==.12f&&ry==-.17f&&axis(axisByte(y))==0,
              "menu keeps small resting drift below the guest deadzone");
        check(move(0,sign*.092f,c)==0,"gameplay ignores a 9% resting drift (Linux dead zone)");
        check(move(0,sign*.13f,c)>.5f,"gameplay walks just outside the Linux dead zone");
    }
    c.leftDeadzone=c.rightDeadzone=.05f; // (the checks below pin the Windows response)
#endif
    for(int i=0;i<3600;++i) {
        const float a=i*3.14159265358979323846f/1800.f;
        for(float r:{.050001f,.05001f,.051f,.06f}) {
            float m=move(std::cos(a)*r,std::sin(a)*r,c);
            check(m>.5f&&m<=.85f,"near deadzone walks, never runs");
        }
        check(move(std::cos(a),std::sin(a),c)>.85f,"full tilt runs");
        check(move(std::cos(a)*.049f,std::sin(a)*.049f,c)==0,"inside host deadzone");
    }
    for(int degree:{0,8,45,90,180,225,270,352}) {
        float a=degree*3.14159265358979323846f/180.f,walk=0,run=0,ps2run=0;
        for(int i=1;i<=10000;++i) {
            float r=i/10000.f,m=move(std::cos(a)*r,std::sin(a)*r,c);
            if(!walk&&m>.5f)walk=r;if(!run&&m>.85f)run=r;if(!ps2run&&m>.99f)ps2run=r;
        }
        std::printf("angle %3d walk %.4f run(0.85) %.4f run(0.99) %.4f\n",degree,walk,run,ps2run);
    }
    Config off=c;off.enabled=false;
    for(int i=-10000;i<=10000;++i) {
        float x=i/10000.f,y=-x,rx=x*.7f,ry=y*.3f;
        const auto old=[](float v){float a=std::min(1.f,std::max(-1.f,v));return a<=-1?0u:static_cast<unsigned>(128+std::lround(a*127));};
        auto a=old(x),b=old(y),d=old(rx),e=old(ry);
        map(x,y,rx,ry,off,.37f,.2f);
        check(axisByte(x)==a&&axisByte(y)==b&&axisByte(rx)==d&&axisByte(ry)==e,"off equals old GLFW bytes");
    }
    for(int b=0;b<256;++b) {
        float v=(b-128.f)/127.f;
        float old=std::fabs(v)<.08f?0.f:std::clamp(v,-1.f,1.f);
        check(sonyAxis(static_cast<uint8_t>(b),false)==old,"off equals old Sony axis");
        check(axisByte(sonyAxis(static_cast<uint8_t>(b),false))==axisByte(old),"off Sony bytes");
    }
    for(int i=0;i<1000;++i) {
        float x=(i-500)/500.f,y=.17f,rx=.11f,ry=-.3f;
        float a=x,b=y,d=rx,e=ry;
        Config tuned=c;tuned.vehicleOriginal=false;tuned.vehicleLift=.7f;tuned.vehicleResponse=4;
        map(x,y,rx,ry,c,0,.2f);map(a,b,d,e,tuned,0,.4f);
        check(x==a&&y==b&&rx==d&&ry==e,"car tuning leaves robot and right stick exact");
    }
    for(int sign:{-1,1}) {
        float v=vehicleAxis(sign*.050001f,c,.2f);
        int b=axisByte(v);
        check(std::fabs(guestRaw(static_cast<uint8_t>(b)))>=.2f,"auto lift accepted");
        check(std::fabs(guestRaw(static_cast<uint8_t>(b-sign)))<.2f,"auto lift only first byte");
    }
    for(float lift:{-1.f,0.f,.1f,.2f,.3f})for(float exp:{.25f,1.f,1.15f,2.f,4.f}) {
        Config v=c;v.vehicleLift=lift;v.vehicleResponse=exp;
        float prev=-2;
        for(int i=-10000;i<=10000;++i) {
            float result=vehicleAxis(i/10000.f,v,.2f);
            check(result>=prev,"vehicle monotonic");prev=result;
        }
    }
    for(int i=-100;i<=100;++i) {
        float x=i/100.f,y=.37f,rx=.2f,ry=-.1f;
        float a=x,b=y,d=rx,e=ry;
        map(a,b,d,e,c,0,.2f);radial(x,y,c.leftLift,c.sensitivity,1.3f,c.leftDeadzone);radial(rx,ry,c.rightLift,1,1,c.rightDeadzone);
        check(a==x&&b==y&&d==rx&&e==ry,"robot endpoint exact");
        x=i/100.f;y=.37f;rx=.2f;ry=-.1f;
        Config tuned=c;tuned.vehicleOriginal=false;
        map(x,y,rx,ry,tuned,1,.2f);
        check(x==vehicleAxis(i/100.f,c,.2f)&&y==vehicleAxis(.37f,c,.2f),"vehicle endpoint exact");
    }
    check(c.vehicleOriginal,"original car mapping is the default");
    const auto originalCar = [&](float x, float y, float rx, float ry, bool sony)
    {
        float a=x,b=y,d=rx,e=ry;
        Config disabled=c;disabled.enabled=false;
        if (sony) {
            a=std::fabs(a)<.08f?0.f:a;b=std::fabs(b)<.08f?0.f:b;
            d=std::fabs(d)<.08f?0.f:d;e=std::fabs(e)<.08f?0.f:e;
        }
        map(a,b,d,e,disabled,1,.2f,sony);
        map(x,y,rx,ry,c,1,.2f,sony);
        check(axisByte(x)==axisByte(a)&&axisByte(y)==axisByte(b)&&
              axisByte(rx)==axisByte(d)&&axisByte(ry)==axisByte(e),"original car equals off on all four axes");
    };
    // Sweep the whole host range, with mixed independent axes and both signs.
    for(int i=-100000;i<=100000;++i) {
        float v=i/100000.f;
        originalCar(v,-v,v*.37f,-v*.83f,false);
    }
    // Check both neighbours of every linear byte rounding boundary, plus extreme floats.
    for(int i=-127;i<=127;++i) {
        float v=(i+.5f)/127.f;
        for(float f:{std::nextafter(v,-std::numeric_limits<float>::infinity()),v,
                     std::nextafter(v,std::numeric_limits<float>::infinity())})
            originalCar(f,-f,-1.f,1.f,false);
    }
    for(float f:{-std::numeric_limits<float>::max(),std::numeric_limits<float>::lowest(),
                 -std::numeric_limits<float>::min(),-0.f,0.f,std::numeric_limits<float>::denorm_min(),
                 std::numeric_limits<float>::min(),std::numeric_limits<float>::max()})
        originalCar(f,-f,f,-f,false);
    // Bit-stratified samples cover exponents, subnormals and the entire finite float range.
    for(uint32_t i=0;i<65536;++i) {
        uint32_t bits=(i<<16)|((i*40503u)&65535u);float v;std::memcpy(&v,&bits,4);
        if(std::isfinite(v))originalCar(v,-v,v,-v,false);
    }
    for(int i=0;i<256;++i)for(int j=0;j<256;++j) {
        float x=sonyAxis(static_cast<uint8_t>(i),true),y=sonyAxis(static_cast<uint8_t>(j),true);
        originalCar(x,y,-x,-y,true);
    }
    for(bool sony:{false,true})for(bool original:{false,true})for(int i=-100;i<=100;++i) {
        Config cfg=c;cfg.vehicleOriginal=original;
        float x=i/100.f,y=.037f,rx=-.06f,ry=.34f;
        float ax=x,ay=y,arx=rx,ary=ry,bx=x,by=y,brx=rx,bry=ry;
        map(ax,ay,arx,ary,cfg,0,.2f,sony);map(bx,by,brx,bry,cfg,1,.2f,sony);
        map(x,y,rx,ry,cfg,.5f,.2f,sony);
        check(x==ax+.5f*(bx-ax)&&y==ay+.5f*(by-ay)&&
              rx==arx+.5f*(brx-arx)&&ry==ary+.5f*(bry-ary),"transform midpoint interpolates exact endpoints");
    }
    check(blend(0,1,0)==1&&blend(1,0,0)==0,"stable blend endpoints");
    check(blend(0,2,.2f)==1&&blend(1,3,.2f)==0,"transform 0.2 seconds endpoints");
    for(float r:{.051f,.07f,.1f,.25f,1.f}) {
        float v=vehicleAxis(r,c,.2f);std::printf("tuned car r=%.3f byte=%u guest=%.6f\n",r,axisByte(v),axis(axisByte(v)));
    }
    // (TODO 274) Dead zone settings, and a menu (play = false) getting the stick itself.
    for(int i=0;i<360;++i) {
        const float a=i*3.14159265358979323846f/180.f;
        for(float r:{.06f,.12f,.19f}) {
            float x=std::cos(a)*r,y=std::sin(a)*r,rx=y,ry=x;const float ox=x,oy=y;
            map(x,y,rx,ry,c,0,.2f,false,false);
            check(x==ox&&y==oy&&rx==oy&&ry==ox,"a menu gets the stick as the pad gives it (no lift)");
            check(axis(axisByte(x))==0&&axis(axisByte(y))==0,"a resting stick under a fifth moves no menu");
        }
        Config wide=c;wide.leftDeadzone=.15f;wide.rightDeadzone=.25f;
        float x=std::cos(a)*.14f,y=std::sin(a)*.14f,rx=std::cos(a)*.24f,ry=std::sin(a)*.24f;
        map(x,y,rx,ry,wide,0,.2f);
        check(x==0&&y==0&&rx==0&&ry==0,"inside a wider dead zone: nothing, in play");
        x=std::cos(a)*.16f;y=std::sin(a)*.16f;rx=std::cos(a)*.26f;ry=std::sin(a)*.26f;
        map(x,y,rx,ry,wide,0,.2f);
        check(std::hypot(x,y)>.6f&&std::hypot(rx,ry)>.19f,"just outside a wider dead zone: the lift, in play");
        check(move(std::cos(a),std::sin(a),wide)>.85f,"full tilt still runs with a wider dead zone");
        for(int kind=0;kind<2;++kind)for(float w:{0.f,1.f}) {
            x=std::cos(a)*.14f;y=std::sin(a)*.14f;rx=std::cos(a)*.24f;ry=std::sin(a)*.24f;
            Config cfg=wide;cfg.enabled=kind==0; // (kind 0: the response on, in a menu; 1: the response off, in play)
            map(x,y,rx,ry,cfg,w,.2f,false,kind==1);
            check(x==0&&y==0&&rx==0&&ry==0,"a wider dead zone cuts in a menu and with the response off");
        }
        x=std::cos(a)*.14f;y=std::sin(a)*.14f;rx=std::cos(a)*.24f;ry=std::sin(a)*.24f;
        map(x,y,rx,ry,wide,1,.2f);
        check(x==0&&y==0&&rx==0&&ry==0,"a wider dead zone cuts the car's original axes");
    }
    std::printf("CHECKS %d FAILURES %d\n",checks,failures);return failures?1:0;
}
