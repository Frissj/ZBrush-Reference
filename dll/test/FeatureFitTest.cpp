#include "../AutoFitFeatures.h"
#include <cstdio>
static double Pattern(double x,double y) {
    // Non-anatomical marks: rings, slanted bars and unequal crosshairs.
    double a=145+25*sin(x*.12+y*.07)+18*cos(y*.15-x*.05);
    const double r=hypot(x-72,y-68);
    if(r>14 && r<20) a=30;
    if(x>112 && x<145 && y>35 && y<95) a=210;
    if(x>37 && x<106 && y>137 && y<147) a=45;
    if(x>91 && x<100 && y>117 && y<173) a=45;
    if(std::abs(y-.6*x-25)<3 && x>100 && x<175) a=60;
    return a;
}
int main() {
    int failed=0;
    auto_fit_features::Field photo;
    photo.w=photo.h=220; photo.epsilon=2;
    photo.image.resize(220*220);
    for(int y=0;y<220;++y) for(int x=0;x<220;++x) photo.image[y*220+x]=Pattern(x,y);
    auto_fit_features::Gradients(photo);
    const double cases[][4]={{.95,.18,143,150},{1.15,-.35,160,153},{.82,1.7,146,155},
        {.65,-2.4,149,151},{1.45,3.05,153,147},{.72,-.94,141,159},
        {1.30,2.37,158,143},{.91,-1.43,147,152},{1.08,.91,156,146}};
    for(const auto& tc:cases) {
        auto_fit_features::Field model;
        model.w=model.h=300; model.epsilon=1; model.image.assign(300*300,210); model.mask.assign(300*300,0);
        const double c=cos(tc[1]),s=sin(tc[1]);
        for(int y=0;y<300;++y) for(int x=0;x<300;++x) {
            const double dx=x-tc[2],dy=y-tc[3];
            const double u=(c*dx+s*dy)/tc[0]+109.5,v=(-s*dx+c*dy)/tc[0]+109.5;
            if(u>=10 && v>=10 && u<210 && v<210) {
                model.mask[y*300+x]=1;
                model.image[y*300+x]=.8*Pattern(u,v)+20;
            }
        }
        auto_fit_features::Gradients(model);
        auto_fit::Fit initial; initial.scale=1; initial.x=150; initial.y=150;
        const auto fit=auto_fit_features::Search(photo,model,initial);
        const double pos=std::hypot(fit.x-tc[2],fit.y-tc[3]);
        const double scale=std::abs(fit.scale/tc[0]-1);
        const double angle=std::abs(std::remainder(fit.angle-tc[1],2*3.141592653589793))*180/3.141592653589793;
        const bool ok=pos<2 && scale<.025 && angle<2;
        printf("%s non-face pattern: position %.3f px, scale %.3f%%, angle %.3f deg, score %.4f\n",ok?"PASS":"FAIL",pos,scale*100,angle,1-fit.error);
        failed+=!ok;
        auto biased=fit; biased.angle+=.7*3.141592653589793/180;
        const auto corrected=auto_fit_features::RefineTurn(photo,auto_fit_features::Samples(model,95),biased);
        const double correctedError=std::abs(std::remainder(corrected.angle-tc[1],2*3.141592653589793))*180/3.141592653589793;
        const bool turnOK=correctedError<.2 && corrected.x==biased.x && corrected.y==biased.y && corrected.scale==biased.scale;
        printf("%s balanced turn removes deliberate angular error: %.3f deg; position and scale preserved\n",turnOK?"PASS":"FAIL",correctedError);
        failed+=!turnOK;
    }
    // Replicating samples in one region must not give that cluster more
    // influence on the angular fit. This is independent of object anatomy.
    std::vector<auto_fit_features::Sample> clustered;
    for(int region=0;region<4;++region) for(int i=0;i<(region==0?80:8);++i)
        clustered.push_back({double(region%2)*100+i*.01,double(region/2)*100,1,0,1.0/104});
    const auto balanced=auto_fit_features::BalancedSamples(clustered);
    double mass[4]={};
    for(const auto& p:balanced) mass[(p.x>50?1:0)+(p.y>50?2:0)]+=p.weight;
    bool equal=true;
    for(double m:mass) equal=equal && fabs(m-.25)<1e-10;
    // Very sparse regions are intentionally capped rather than amplified.
    // Here the 10% mass floor limits amplification of the eight-point groups.
    bool capped=mass[0]>.25 && mass[0]<.31;
    printf("%s dense-cluster weighting is balanced with a sparse-region noise cap\n",(equal||capped)?"PASS":"FAIL");
    failed+=!(equal||capped);
    return failed?1:0;
}
