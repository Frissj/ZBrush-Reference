#include "../AutoFit.h"
#include <cstdio>
#include <chrono>

// Asymmetric profile with an ear/nose-like projection and a neck; rasterise
// known transforms independently and measure recovered parameters.
static bool Subject(double x,double y) {
    const bool head=(x*x/(38*38)+y*y/(61*61))<1;
    const bool nose=x>22 && x<57 && y>-8 && y<10 && x<57-std::abs(y)*1.8;
    const bool neck=x>-19 && x<12 && y>40 && y<83;
    return head || nose || neck;
}
int main() {
    int failed=0;
    auto_fit::Shape photo; photo.w=240; photo.h=280;
    std::vector<unsigned char> rgb(size_t(photo.w)*photo.h*3,240);
    for(int y=0;y<photo.h;++y) for(int x=0;x<photo.w;++x)
        if(Subject(x-110,y-120)) for(int c=0;c<3;++c) rgb[(y*photo.w+x)*3+c]=70+c*15;
    if(!auto_fit::Photo(rgb,photo.w,photo.h,photo)) return 2;
    const double cases[][4]={{1.0,0,180,175},{1.45,0.24,190,170},
        {0.72,-0.32,145,160},{1.12,0.48,175,185},{0.85,-0.18,205,180}};
    for(const auto& tc:cases) {
        auto_fit::Shape model; model.w=400; model.h=400; model.mask.resize(160000);
        for(int y=0;y<400;++y) for(int x=0;x<400;++x) {
            double dx=x-tc[2],dy=y-tc[3],c=cos(tc[1]),s=sin(tc[1]);
            model.mask[y*400+x]=Subject((c*dx+s*dy)/tc[0],(-s*dx+c*dy)/tc[0]);
        }
        if(!auto_fit::Prepare(model)) return 3;
        auto start=std::chrono::steady_clock::now();
        const auto fit=auto_fit::Search(photo,model);
        const double c=cos(tc[1]),s=sin(tc[1]);
        const double px=photo.cx-110,py=photo.cy-120;
        const double pos=std::hypot(fit.x-(tc[2]+tc[0]*(c*px-s*py)),fit.y-(tc[3]+tc[0]*(s*px+c*py)));
        const double angle=std::abs(fit.angle-tc[1])*180/3.14159265358979;
        const double scale=std::abs(fit.scale/tc[0]-1);
        const bool ok=pos<1.5 && angle<1.5 && scale<0.02 && fit.error<0.01;
        failed+=!ok;
        printf("%s scale error %.3f%% angle error %.3f deg position error %.3f px score %.6f (%lld ms)\n",
            ok?"PASS":"FAIL",scale*100,angle,pos,fit.error,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
    }
    std::fill(rgb.begin(),rgb.end(),240);
    if(auto_fit::Photo(rgb,240,280,photo)) { puts("FAIL blank photo accepted"); ++failed; }
    else puts("PASS blank photo rejected");
    // Dense multicolour border must not be mistaken for a plain background.
    for(size_t i=0;i<rgb.size();++i) rgb[i]=static_cast<unsigned char>((i*73+i/7)%256);
    if(auto_fit::Photo(rgb,240,280,photo)) { puts("FAIL busy background accepted"); ++failed; }
    else puts("PASS busy background rejected");
    return failed?1:0;
}
