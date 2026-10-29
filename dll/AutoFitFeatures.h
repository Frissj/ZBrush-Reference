#pragma once
#include <array>
#include "AutoFit.h"

// Gradient registration over the whole visible model. No object labels,
// landmarks, face regions, or assumed anatomy enter sampling or scoring.
namespace auto_fit_features {
struct Field {
    int w=0,h=0;
    double epsilon=1;
    double depthPixelStep=2;
    int erosion=10;
    std::vector<double> image,dx,dy;
    std::vector<unsigned char> mask;
};
inline std::vector<double> Blur(const std::vector<double>& a,int w,int h,double sigma=1) {
    const int radius=int(ceil(4*sigma)),count=2*radius+1;
    std::vector<double> kernel(count); double sum=0;
    for(int i=0;i<count;++i) { kernel[i]=exp(-0.5*(i-radius)*(i-radius)/(sigma*sigma)); sum+=kernel[i]; }
    for(double& k:kernel) k/=sum;
    std::vector<double> temp(a.size()),out(a.size());
    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
        for(int i=0;i<count;++i) temp[y*w+x]+=kernel[i]*a[y*w+std::clamp(x+i-radius,0,w-1)];
    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
        for(int i=0;i<count;++i) out[y*w+x]+=kernel[i]*temp[std::clamp(y+i-radius,0,h-1)*w+x];
    return out;
}
inline void Gradients(Field& f,double sigma=1) {
    const auto smooth=Blur(f.image,f.w,f.h,sigma);
    f.dx.resize(smooth.size()); f.dy.resize(smooth.size());
    for(int y=0;y<f.h;++y) for(int x=0;x<f.w;++x) {
        double gx=0,gy=0;
        for(int d=-1;d<=1;++d) {
            const double weight=d==0?2:1;
            gx+=weight*(smooth[std::clamp(y+d,0,f.h-1)*f.w+std::min(x+1,f.w-1)]-
                smooth[std::clamp(y+d,0,f.h-1)*f.w+std::max(x-1,0)]);
            gy+=weight*(smooth[std::min(y+1,f.h-1)*f.w+std::clamp(x+d,0,f.w-1)]-
                smooth[std::max(y-1,0)*f.w+std::clamp(x+d,0,f.w-1)]);
        }
        f.dx[y*f.w+x]=gx/8; f.dy[y*f.w+x]=gy/8;
    }
}
inline void ShadeDepth(Field& f) {
    auto z=f.image;
    for(size_t i=0;i<z.size();++i) if(!f.mask[i]) z[i]=0;
    z=Blur(z,f.w,f.h,2/f.depthPixelStep);
    for(int y=0;y<f.h;++y) for(int x=0;x<f.w;++x) {
        const int i=y*f.w+x;
        const double gx=(z[y*f.w+std::min(x+1,f.w-1)]-z[y*f.w+std::max(x-1,0)])*.5;
        const double gy=(z[std::min(y+1,f.h-1)*f.w+x]-z[std::max(y-1,0)*f.w+x])*.5;
        const double nz=f.depthPixelStep;
        f.image[i]=f.mask[i]?std::clamp(100+160*(-.35*gx-.5*gy+.75*nz)/sqrt(gx*gx+gy*gy+nz*nz),0.0,255.0):210;
    }
}
struct Sample { double x,y,nx,ny,weight; };
inline std::vector<Sample> Samples(const Field& f,int percentile=85) {
    auto interior=f.mask;
    // Depth smoothing and image smoothing each have finite support. Erode
    // beyond their combined radius so a flat object does not gain artificial
    // "features" from filling the outside of its depth buffer with zero.
    for(int pass=0;pass<f.erosion;++pass) {
        auto next=interior;
        for(int y=0;y<f.h;++y) for(int x=0;x<f.w;++x)
            next[y*f.w+x]=x && y && x<f.w-1 && y<f.h-1 && interior[y*f.w+x] &&
                interior[y*f.w+x-1] && interior[y*f.w+x+1] && interior[(y-1)*f.w+x] && interior[(y+1)*f.w+x];
        interior.swap(next);
    }
    std::vector<double> magnitudes;
    for(int y=0;y<f.h;y+=2) for(int x=0;x<f.w;x+=2)
        if(interior[y*f.w+x]) magnitudes.push_back(std::hypot(f.dx[y*f.w+x],f.dy[y*f.w+x]));
    if(magnitudes.size()<80) return {};
    const size_t at=magnitudes.size()*percentile/100;
    std::nth_element(magnitudes.begin(),magnitudes.begin()+at,magnitudes.end());
    const double threshold=std::max(.15,magnitudes[at]);
    std::vector<Sample> out;
    for(int y=0;y<f.h;y+=2) for(int x=0;x<f.w;x+=2) {
        const int i=y*f.w+x;
        const double mag=std::hypot(f.dx[i],f.dy[i]);
        if(!interior[i] || mag<=threshold) continue;
        const double d=sqrt(mag*mag+f.epsilon*f.epsilon);
        out.push_back({double(x),double(y),f.dx[i]/d,f.dy[i]/d,std::min(mag*2/f.depthPixelStep,10.0)});
    }
    if(out.size()>4096) {
        std::vector<Sample> sample;
        for(size_t i=0;i<4096;++i) sample.push_back(out[i*out.size()/4096]);
        out.swap(sample);
    }
    double weight=0; for(auto p:out) weight+=p.weight;
    if(weight>0) for(auto& p:out) p.weight/=weight;
    return out;
}
inline double Pixel(const std::vector<double>& a,int w,int h,double x,double y) {
    if(x<0 || y<0 || x>=w-1 || y>=h-1) return 0;
    const int ix=int(x),iy=int(y); const double fx=x-ix,fy=y-iy;
    return (1-fy)*((1-fx)*a[iy*w+ix]+fx*a[iy*w+ix+1])+
        fy*((1-fx)*a[(iy+1)*w+ix]+fx*a[(iy+1)*w+ix+1]);
}
inline double Score(const Field& photo,const std::vector<Sample>& samples,const auto_fit::Fit& fit,size_t step=1,bool strength=false) {
    const double c=cos(fit.angle),s=sin(fit.angle);
    double score=0,weight=0;
    for(size_t i=0;i<samples.size();i+=step) {
        const auto& p=samples[i];
        const double dx=p.x-fit.x,dy=p.y-fit.y;
        const double x=(c*dx+s*dy)/fit.scale+(photo.w-1)*.5;
        const double y=(-s*dx+c*dy)/fit.scale+(photo.h-1)*.5;
        const double gx=Pixel(photo.dx,photo.w,photo.h,x,y),gy=Pixel(photo.dy,photo.w,photo.h,x,y);
        const double dot=p.nx*(c*gx-s*gy)+p.ny*(s*gx+c*gy);
        double similarity=dot*dot/(gx*gx+gy*gy+photo.epsilon*photo.epsilon);
        // As in the supplied registration script, give coherent strong
        // reference features more influence than weak texture/noise. The
        // scale factor expresses gradient strength in model coordinates.
        if(strength) similarity*=sqrt(std::hypot(gx,gy)/fit.scale);
        score+=p.weight*similarity;
        weight+=p.weight;
    }
    return weight>0?score/weight:0;
}
inline std::vector<Sample> BalancedSamples(const std::vector<Sample>& samples) {
    if(samples.empty()) return {};
    double x0=samples[0].x,x1=x0,y0=samples[0].y,y1=y0;
    for(const auto& p:samples) { x0=std::min(x0,p.x);x1=std::max(x1,p.x);y0=std::min(y0,p.y);y1=std::max(y1,p.y); }
    const double cx=(x0+x1)*.5,cy=(y0+y1)*.5;
    std::array<double,4> mass={}; std::array<int,4> count={};
    auto region=[&](const Sample& p) { return (p.x>cx?1:0)+(p.y>cy?2:0); };
    for(const auto& p:samples) { mass[region(p)]+=p.weight; ++count[region(p)]; }
    // Four spatial regions prevent one dense cluster of high-contrast marks
    // from deciding the turn. Keep sparse regions from amplifying noise.
    for(int n:count) if(n<8) return samples;
    auto balanced=samples; double total=0;
    for(auto& p:balanced) { p.weight/=std::max(.10,mass[region(p)]); total+=p.weight; }
    for(auto& p:balanced) p.weight/=total;
    return balanced;
}
inline auto_fit::Fit RefineTurn(const Field& photo,const std::vector<Sample>& samples,const auto_fit::Fit& initial) {
    const auto balanced=BalancedSamples(samples);
    if(balanced.size()<32) return initial;
    // Hold scale and position after the joint fit. Otherwise the stronger
    // spatial balance can trade a translation error for a better angle.
    constexpr int steps=48;
    constexpr double radius=.02094395102393195; // 1.2 degrees
    const double spacing=2*radius/steps;
    std::array<double,steps+1> scores;
    auto score=[&](double angle) { auto f=initial; f.angle=angle; return Score(photo,balanced,f,1,true); };
    concurrency::parallel_for(0,steps+1,[&](int i) { scores[i]=score(initial.angle-radius+i*spacing); });
    const int peak=int(std::max_element(scores.begin(),scores.end())-scores.begin());
    // An optimum at the limit is evidence for another basin, not a fine
    // angular correction. Leave it to the full registration search.
    if(peak==0 || peak==steps || scores[peak]-scores[steps/2]<1e-6) return initial;
    double lo=initial.angle-radius+(peak-1)*spacing,hi=lo+2*spacing;
    constexpr double golden=.61803398874989485;
    double a=hi-golden*(hi-lo),b=lo+golden*(hi-lo),sa=score(a),sb=score(b);
    for(int i=0;i<28;++i) {
        if(sa>sb) { hi=b;b=a;sb=sa;a=hi-golden*(hi-lo);sa=score(a); }
        else { lo=a;a=b;sa=sb;b=lo+golden*(hi-lo);sb=score(b); }
    }
    auto result=initial; result.angle=sa>sb?a:b;
    return result;
}
inline auto_fit::Fit Search(const Field& photo,const Field& model,const auto_fit::Fit& initial) {
    const auto samples=Samples(model);
    auto_fit::Fit failure=initial; failure.error=1;
    if(samples.size()<60) return failure;
    int x0=model.w,y0=model.h,x1=0,y1=0;
    for(const auto& p:samples) { x0=std::min(x0,int(p.x));y0=std::min(y0,int(p.y));x1=std::max(x1,int(p.x));y1=std::max(y1,int(p.y)); }
    const double span=std::hypot(double(x1-x0),double(y1-y0));
    auto evaluate = [&](std::vector<auto_fit::Fit>& candidates,size_t stride) {
        concurrency::parallel_for(size_t(0),candidates.size(),[&](size_t i) {
            candidates[i].error=1-Score(photo,samples,candidates[i],stride);
        });
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) { return a.error<b.error; });
    };
    std::vector<auto_fit::Fit> coarse;
    coarse.push_back(initial);
    // Fifteen-degree coverage keeps a small, strongly rotated object in
    // the correct basin before the continuous refinement takes over.
    for(int scale=-6;scale<=6;++scale) for(int a=-12;a<12;++a)
        for(int y=-7;y<=7;++y) for(int x=-7;x<=7;++x) {
            auto f=initial; f.scale*=exp(scale*.08); f.angle+=a*3.141592653589793/12;
            f.x+=x*span*.04; f.y+=y*span*.04; coarse.push_back(f);
        }
    evaluate(coarse,4);
    std::vector<auto_fit::Fit> seeds;
    for(const auto& f:coarse) {
        bool distinct=true;
        for(const auto& s:seeds)
            if(std::hypot(f.x-s.x,f.y-s.y)<span*.045 && std::abs(log(f.scale/s.scale))<.15 &&
                std::abs(std::remainder(f.angle-s.angle,2*3.141592653589793))<.35) distinct=false;
        if(distinct) seeds.push_back(f);
        if(seeds.size()==12) break;
    }
    auto_fit::Fit best=failure;
    for(int pass=0;pass<6;++pass) {
        std::vector<auto_fit::Fit> candidates;
        const double step=std::pow(.45,pass);
        for(auto centre:seeds) for(int z=-2;z<=2;++z) for(int a=-2;a<=2;++a)
            for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
                auto f=centre; f.scale*=exp(z*.045*step); f.angle+=a*.130899694*step;
                f.x+=x*span*.025*step; f.y+=y*span*.025*step; candidates.push_back(f);
            }
        evaluate(candidates,pass<2?2:1);
        best=candidates.front(); seeds.clear();
        for(const auto& f:candidates) {
            bool distinct=true;
            for(const auto& s:seeds)
                if(std::hypot(f.x-s.x,f.y-s.y)<span*.018*step && std::abs(log(f.scale/s.scale))<.05*step &&
                    std::abs(std::remainder(f.angle-s.angle,2*3.141592653589793))<.12*step) distinct=false;
            if(distinct) seeds.push_back(f);
            if(seeds.size()==12) break;
        }
    }
    // Continuous final refinement: the coarse search finds the basin;
    // Nelder-Mead avoids quantising the final position, scale and turn.
    struct Vertex { std::array<double,4> p; double error; };
    const auto geometric=best;
    auto refinementSamples=Samples(model,95);
    if(refinementSamples.size()<24) refinementSamples=samples;
    best.error=1-Score(photo,refinementSamples,best,1,true);
    const std::array<double,4> origin={log(best.scale),best.x,best.y,best.angle};
    auto from = [&](const std::array<double,4>& p) {
        auto f=best; f.scale=exp(p[0]); f.x=p[1]; f.y=p[2]; f.angle=p[3]; return f;
    };
    auto vertex = [&](std::array<double,4> p) {
        if(std::abs(p[0]-origin[0])>.06 || std::abs(p[1]-origin[1])>4*geometric.scale ||
            std::abs(p[2]-origin[2])>4*geometric.scale || std::abs(p[3]-origin[3])>.020943951)
            return Vertex{p,1e6};
        return Vertex{p,1-Score(photo,refinementSamples,from(p),1,true)};
    };
    std::array<Vertex,5> simplex;
    simplex[0]=vertex(origin);
    const double delta[4]={.005,.75,.75,.00872664626};
    for(int i=0;i<4;++i) { auto p=origin;p[i]+=delta[i];simplex[i+1]=vertex(p); }
    for(int iteration=0;iteration<250;++iteration) {
        std::sort(simplex.begin(),simplex.end(),[](const auto& a,const auto& b){return a.error<b.error;});
        double spread=0;
        for(int i=1;i<5;++i) for(int j=0;j<4;++j)
            spread=std::max(spread,std::abs(simplex[i].p[j]-simplex[0].p[j])/delta[j]);
        if(spread<.001 && simplex[4].error-simplex[0].error<1e-8) break;
        std::array<double,4> centre={};
        for(int i=0;i<4;++i) for(int j=0;j<4;++j) centre[j]+=simplex[i].p[j]/4;
        auto point = [&](double factor) {
            auto p=centre; for(int j=0;j<4;++j) p[j]+=factor*(centre[j]-simplex[4].p[j]); return vertex(p);
        };
        const auto reflected=point(1);
        if(reflected.error<simplex[0].error) {
            const auto expanded=point(2); simplex[4]=expanded.error<reflected.error?expanded:reflected;
        } else if(reflected.error<simplex[3].error) simplex[4]=reflected;
        else {
            const auto contracted=point(reflected.error<simplex[4].error?.5:-.5);
            if(contracted.error<std::min(reflected.error,simplex[4].error)) simplex[4]=contracted;
            else for(int i=1;i<5;++i) {
                auto p=simplex[i].p; for(int j=0;j<4;++j) p[j]=(p[j]+simplex[0].p[j])*.5;
                simplex[i]=vertex(p);
            }
        }
    }
    for(const auto& v:simplex) if(v.error<best.error) { best=from(v.p);best.error=v.error; }
    const auto turned=RefineTurn(photo,refinementSamples,best);
    if(Score(photo,samples,turned)>=Score(photo,samples,best)-.01) best=turned;
    best.error=1-Score(photo,samples,best);
    if(best.error>geometric.error+.03) return geometric;
    return best;
}
} // namespace auto_fit_features
