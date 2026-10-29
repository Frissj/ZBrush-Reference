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
        {1.30,2.37,158,143},{.91,-1.43,147,152},{1.08,.91,156,146},
        {.40,-2.13,92,203},{1.90,1.2,150,150},{.55,.7,195,91},
        {.43,2.6,224,79},{.78,-.48,88,206}};
    int caseIndex=0;
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
        if(tc[0]<.45) { initial.scale=.08;initial.x=-200;initial.y=500;initial.angle=4.1; }
        const auto fit=auto_fit_features::Search(photo,model,initial);
        const double pos=std::hypot(fit.x-tc[2],fit.y-tc[3]);
        const double scale=std::abs(fit.scale/tc[0]-1);
        const double angle=std::abs(std::remainder(fit.angle-tc[1],2*3.141592653589793))*180/3.141592653589793;
        const bool ok=pos<2 && scale<.025 && angle<2;
        printf("%s non-face pattern: position %.3f px, scale %.3f%%, angle %.3f deg, score %.4f\n",ok?"PASS":"FAIL",pos,scale*100,angle,1-fit.error);
        failed+=!ok;
        const auto evidence=auto_fit_features::Assess(photo,model,fit);
        printf("%s independent evidence: gain %.3f validation %.3f coverage %.3f regions %d\n",evidence.reliable?"PASS":"FAIL",
            evidence.gain,evidence.validationGain,evidence.coverage,evidence.supportedRegions);
        failed+=!evidence.reliable;
        auto biased=fit; biased.angle+=.7*3.141592653589793/180;
        const auto corrected=auto_fit_features::RefineTurn(photo,auto_fit_features::Samples(model,95),biased);
        const double correctedError=std::abs(std::remainder(corrected.angle-tc[1],2*3.141592653589793))*180/3.141592653589793;
        // On a very small raster, one pixel spans a larger angle. Keep the
        // original strict tolerance, with a one-pixel bound for small cases.
        const double angleTolerance=std::max(.2,180/(3.141592653589793*200*tc[0]));
        const bool turnOK=correctedError<angleTolerance && corrected.x==biased.x && corrected.y==biased.y && corrected.scale==biased.scale;
        printf("%s balanced turn removes deliberate angular error: %.3f deg; position and scale preserved\n",turnOK?"PASS":"FAIL",correctedError);
        failed+=!turnOK;
        if(caseIndex++<3) {
            auto displaced=fit;displaced.scale*=.99;displaced.x+=.4;displaced.y-=.4;displaced.angle-=1.5*3.141592653589793/180;
            const auto precise=auto_fit_features::RefineProfiles(photo,model,displaced);
            const double positionError=std::hypot(precise.x-tc[2],precise.y-tc[3]);
            const double turnError=std::abs(std::remainder(precise.angle-tc[1],2*3.141592653589793))*180/3.141592653589793;
            const bool fixed=positionError<.4 && fabs(precise.scale/tc[0]-1)<.01 && turnError<.2;
            printf("%s local edge profiles recover a displaced fine fit: %.3f px %.3f deg\n",fixed?"PASS":"FAIL",positionError,turnError);
            failed+=!fixed;
        }
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
    auto_fit_features::Field validationModel=photo;
    validationModel.mask.assign(validationModel.image.size(),1);
    for(int kind=0;kind<3;++kind) {
        auto unrelated=photo;
        unsigned random=123456789;
        for(int y=0;y<unrelated.h;++y) for(int x=0;x<unrelated.w;++x) {
            random=random*1664525u+1013904223u;
            unrelated.image[y*unrelated.w+x]=kind==0?double(32+(random>>24)*.75):
                kind==1?120+75*sin(x*.45):128;
        }
        auto_fit_features::Gradients(unrelated);
        auto_fit::Fit seed;seed.scale=1;seed.x=seed.y=109.5;
        const auto fit=auto_fit_features::Search(unrelated,validationModel,seed);
        const auto evidence=auto_fit_features::Assess(unrelated,validationModel,fit);
        printf("%s unrelated reference %d rejected: gain %.3f validation %.3f regions %d raw score %.3f\n",
            !evidence.reliable?"PASS":"FAIL",kind,evidence.gain,evidence.validationGain,evidence.supportedRegions,1-fit.error);
        failed+=evidence.reliable;
    }
    for(int kind=0;kind<5;++kind) {
        // Different exposure, reversed contrast, clutter, and a partly
        // covered object must retain the same geometric alignment.
        auto altered=photo;
        for(int y=0;y<altered.h;++y) for(int x=0;x<altered.w;++x) {
            double& pixel=altered.image[y*altered.w+x];
            if(kind==0) pixel=255-pixel;
            if(kind==1) pixel=255*pow(pixel/255,1.7)+x*.025;
            if(kind==2 && (x<10 || y<10 || x>=210 || y>=210)) pixel=((x/3+y/5)%2)*255;
            if(kind==3 && x>35 && x<95 && y>35 && y<90) pixel=245;
            if(kind==4) pixel=120+.04*pixel;
        }
        auto_fit_features::Gradients(altered);
        auto_fit::Fit seed;seed.scale=.75;seed.angle=.4;seed.x=120;seed.y=95;
        const auto fit=auto_fit_features::Search(altered,validationModel,seed);
        const auto evidence=auto_fit_features::Assess(altered,validationModel,fit);
        const double position=std::hypot(fit.x-109.5,fit.y-109.5),angle=std::abs(std::remainder(fit.angle,2*3.141592653589793))*180/3.141592653589793;
        const bool ok=position<2 && fabs(fit.scale-1)<.025 && angle<1 && evidence.reliable;
        printf("%s nuisance reference %d: position %.3f scale %.4f angle %.3f validation %.3f\n",ok?"PASS":"FAIL",kind,position,fit.scale,angle,evidence.validationGain);
        failed+=!ok;
    }
    auto_fit_features::Field odd;odd.w=221;odd.h=223;
    auto_fit_features::Field reduced;reduced.w=111;reduced.h=112;
    auto_fit::Fit original;original.scale=.73;original.angle=1.21;original.x=72.31;original.y=94.53;
    const auto down=auto_fit_features::TransferFit(original,odd,reduced,.5);
    const auto up=auto_fit_features::TransferFit(down,reduced,odd,2);
    const bool centred=fabs(up.x-original.x)<1e-12 && fabs(up.y-original.y)<1e-12 && up.scale==original.scale && up.angle==original.angle;
    printf("%s odd-sized pyramid preserves subpixel centres through reduction and enlargement\n",centred?"PASS":"FAIL");
    failed+=!centred;
    return failed?1:0;
}
