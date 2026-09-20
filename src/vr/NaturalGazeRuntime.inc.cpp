// Included inside LiveGaze's private namespace. Only registered ordinary-Live
// effectors reach this adapter. Evidence: live-gaze-dev472-hardware/NOTES.md.
Method *naturalDelta = nullptr, *naturalPosition = nullptr, *naturalLimit = nullptr;
bool naturalReady = false;
using CurveFn = float(*)(void*, void*);
std::array<CurveFn, 6> naturalOriginalCurves{};
void* NaturalClass(void* object) { void* c=nullptr; Copy(object,&c,sizeof(c)); return c; }
int NaturalOffset(void* c, const char* name, const char* type) {
    struct CachedField { void* klass; std::string name, type; int offset; };
    static std::vector<CachedField> fields;
    for(const auto& f:fields) if(f.klass==c && f.name==name && f.type==type)return f.offset;
    auto* f = c ? UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name",c,name) : nullptr;
    if (!f || (UnityResolve::Invoke<int>("il2cpp_field_get_flags",f)&0x10)) return -1;
    auto* t=UnityResolve::Invoke<void*>("il2cpp_field_get_type",f);
    auto* n=UnityResolve::Invoke<char*>("il2cpp_type_get_name",t);
    const bool match=n && std::strcmp(n,type)==0;
    if(n) UnityResolve::Invoke<void>("il2cpp_free",n);
    const int offset=match ? UnityResolve::Invoke<int>("il2cpp_field_get_offset",f) : -1;
    if(fields.size()<256)fields.push_back({c,name,type,offset});
    std::ostringstream s;s<<"NATURAL_GAZE_FIELD class="<<c<<" name="<<name<<" type="<<type<<" offset="<<offset;Log(s.str());
    return offset;
}
template<class T> bool NaturalRead(void* object,const char* name,const char* type,T& value) {
    const int offset=NaturalOffset(NaturalClass(object),name,type);
    return object && offset>=16 && Copy(static_cast<char*>(object)+offset,&value,sizeof(value));
}
template<class T> bool NaturalBox(Method* m,void* object,void** args,T& value) {
    void* box=nullptr;
    return Invoke(m,object,args,&box) && box &&
        Copy(UnityResolve::Invoke<void*>("il2cpp_object_unbox",box),&value,sizeof(value));
}
template<size_t I> float NaturalCurve(void* effector,void* info) {
    const float official=naturalOriginalCurves[I](effector,info);
    float result=official;
    if(GakumasLocal::Config::vrRuntimeStartupEnabled && naturalReady)
        for(auto& b:smoothBindings) if(Target(b.effector)==effector && !b.naturalFailed) {
            result*=static_cast<float>(b.natural.output.participation);
            if constexpr(I==1 || I==4) {
                if(GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
                    b.naturalOfficialHead=official;b.naturalConsumedHead=result;b.naturalHeadFrame=ReadUnityFrame();
                }
            }
            break;
        }
    TraceLiveGazeCurve(effector,static_cast<unsigned>(I),result);
    return result;
}
bool ResolveNaturalCurves(Class* effector) {
    auto* time=Il2cppUtils::GetClass("UnityEngine.CoreModule.dll","UnityEngine","Time");
    auto* transform=Il2cppUtils::GetClass("UnityEngine.CoreModule.dll","UnityEngine","Transform");
    auto* utility=Il2cppUtils::GetClass("campus-submodule.Runtime.dll","Campus.Common.LookAt","LookAtUtility");
    naturalDelta=Exact(time,"get_deltaTime","System.Single",true,{});
    naturalPosition=Exact(transform,"get_position","UnityEngine.Vector3",false,{});
    naturalLimit=Exact(utility,"CalcLookTargetLimitAngle","System.ValueTuple<System.Single,System.Single,System.Single>",true,
        {"Campus.Common.ICampusActorController","Campus.Common.LookAt.LookTargetType","UnityEngine.Transform","System.Boolean"});
    if(!naturalDelta || !naturalPosition || !naturalLimit) {Log("NATURAL_GAZE_SKIP reason=api-shape");return false;}
    const char* names[]={"GetInEyesWeight","GetInHeadWeight","GetInBodyWeight","GetOutEyesWeight","GetOutHeadWeight","GetOutBodyWeight"};
    CurveFn hooks[]={NaturalCurve<0>,NaturalCurve<1>,NaturalCurve<2>,NaturalCurve<3>,NaturalCurve<4>,NaturalCurve<5>};
    std::array<GakumasVR::Hooks::Request,6> requests{};
    for(size_t i=0;i<6;++i) {
        auto* m=Exact(effector,names[i],"System.Single",false,{});
        if(!m) {Log("NATURAL_GAZE_SKIP reason=curve-api");return false;}
        requests[i]={m->function,reinterpret_cast<void*>(hooks[i]),reinterpret_cast<void**>(&naturalOriginalCurves[i]),names[i]};
    }
    const bool ok=GakumasVR::Hooks::CreateAndEnableBatch(requests.data(),requests.size());
    std::ostringstream line;
    line << "NATURAL_GAZE_API delta="<<naturalDelta->address<<" position="<<naturalPosition->address
         <<" limit="<<naturalLimit->address<<" ready="<<ok<<" stage=actor-late-after consume=next-official-weight";
    Log(line.str());return ok;
}
bool NaturalInputs(SmoothBinding& b,void* actor,gaze::NaturalInput& in,gaze::V3& euler,gaze::V3& head,gaze::V3& target) {
    void* effector=Target(b.effector);void* controller=Target(b.controller);
    void* animation=nullptr;void* transform=nullptr;void* inner=nullptr;
    if(!Alive(effector)||!Alive(controller))return false;
    const auto shapeFailure=[&b]() { b.naturalFailed=true;Log("NATURAL_GAZE_SKIP reason=input-shape fallback=official");return false; };
    if(!NaturalRead(actor,"_actorAnimation","Campus.Common.CampusActorAnimation",animation)||
       !NaturalRead(effector,"target","UnityEngine.Transform",transform)||
       !NaturalRead(controller,"_lookAtController","Campus.Common.LookAt.CampusActorLookAtController",inner))return shapeFailure();
    if(!Alive(animation)||!Alive(transform)||!Alive(inner))return false;
    const char* rotation[]={"fullBodyIkHeadOriginalRotationX","fullBodyIkHeadOriginalRotationY","fullBodyIkHeadOriginalRotationZ"};
    const char* position[]={"fullBodyIkHeadOriginalPositionX","fullBodyIkHeadOriginalPositionY","fullBodyIkHeadOriginalPositionZ"};
    float r[3]{},p[3]{},t[3]{};
    for(size_t i=0;i<3;++i) {
        if(!NaturalRead(animation,rotation[i],"System.Single",r[i]) ||
           !NaturalRead(animation,position[i],"System.Single",p[i]))return shapeFailure();
        if(!std::isfinite(r[i]) || !std::isfinite(p[i]))return false;
    }
    if(!NaturalBox(naturalPosition,transform,nullptr,t))return false;
    euler={r[0],r[1],r[2]};head={p[0],p[1],p[2]};target={t[0],t[1],t[2]};
    unsigned char live=0,prohibit=1,outRange=1;
    if(!NaturalRead(controller,"_liveEnableLookAt","System.Boolean",live)||
       !NaturalRead(controller,"_isProhibitLookEnable","System.Boolean",prohibit)||
       !NaturalRead(controller,"_isConstraintOutRange","System.Boolean",outRange))return shapeFailure();
    // Do not pretend the utility's default cone includes controller overrides.
    for(const char* name:{"_overwriteEyeLookOutVirtualAngle","_overwriteEyeLookAgainVirtualAngle",
                         "_overwriteEyeLookOutHorizontalAngle","_overwriteEyeLookAgainHorizontalAngle"}) {
        float v=0;if(!NaturalRead(inner,name,"System.Single",v))return shapeFailure();
        if(!std::isfinite(v)||v>=0)return false;
    }
    int eye=1;bool cameraTarget=false;void* args[]={actor,&eye,transform,&cameraTarget};void* tuple=nullptr;
    float angle=0,out=0,again=0;
    if(!Invoke(naturalLimit,nullptr,args,&tuple)||!tuple)return false;
    if(!NaturalRead(tuple,"Item1","System.Single",angle)||!NaturalRead(tuple,"Item2","System.Single",out)||
       !NaturalRead(tuple,"Item3","System.Single",again))return shapeFailure();
    if(!std::isfinite(angle)||!std::isfinite(out)||!std::isfinite(again)||
       angle<0||angle>180||out<=25||out>180||again<=25||again>180)return false;
    in.prohibited=prohibit!=0 || live==0;in.officialInRange=outRange==0;
    in.officialAngle=angle;in.officialLimit=std::min(out,again);
    in.horizontalLimit=in.verticalLimit=in.officialLimit;
    return true;
}
void NaturalTick(SmoothBinding& b,void* actor) {
    const int frame=ReadUnityFrame();
    if(frame<0 || frame==b.decisionFrame || b.naturalFailed)return;
    float dt=0;if(!NaturalBox(naturalDelta,nullptr,nullptr,dt))return;
    if(IsOfficialLivePaused() || dt==0)return;
    const bool gap=b.decisionFrame>=0 && frame!=b.decisionFrame+1;
    b.decisionFrame=frame;
    gaze::NaturalInput in;in.actor=reinterpret_cast<std::uintptr_t>(actor);in.dt=dt;
    void* follow=camera::ReadVrFollowActorController();
    float centerPosition[3]{};
    in.requested=b.requested && liveGazeRequested.load() &&
        (LiveGazeWantsAllActors() || !follow || follow==actor) && ReadCenter(centerPosition);
    gaze::V3 euler,head,target;
    in.valid=NaturalInputs(b,actor,in,euler,head,target);
    if(b.naturalFailed)return;
    const bool hadPrevious=b.motion.previous && !gap;
    if(gap)b.motion.Reset();
    if(in.valid) {
        in=b.motion.Sample(in,euler,head,target);
        if(hadPrevious && dt>0 && dt<=0.1)in.officialAngleRate=(in.officialAngle-b.previousAngle)/dt;
        b.previousAngle=in.officialAngle;
    }
    const auto before=b.natural.output;
    const int preset=liveGazePreset.load();
    const auto output=b.natural.Step(in,preset);
    if(GakumasLocal::Config::vrDiagnosticsStartupEnabled && b.naturalSamples++<36000) {
        std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(9);
        s<<"NATURAL_GAZE_SAMPLE actor="<<actor<<" frame="<<frame<<" preset="<<preset
         <<" scope="<<liveGazeScope.load()<<" dt="<<dt<<" valid="<<in.valid
         <<" requested="<<in.requested<<" prohibit="<<in.prohibited<<" inRange="<<in.officialInRange
         <<" angle="<<in.officialAngle<<" hard="<<in.officialLimit<<" angleRate="<<in.officialAngleRate
         <<" yaw="<<in.yaw<<" pitch="<<in.pitch<<" yawRate="<<in.yawRate<<" pitchRate="<<in.pitchRate
         <<" speed="<<in.animationSpeed<<" gain="<<output.participation<<" state="<<int(output.state)
         <<" reason="<<int(output.reason)<<" changed="<<(before.state!=output.state)
         <<" headCurveFrame="<<b.naturalHeadFrame<<" officialHeadCurve="<<b.naturalOfficialHead
         <<" consumedHeadCurve="<<b.naturalConsumedHead
         <<" originalEuler="<<euler.x<<','<<euler.y<<','<<euler.z<<" originalPosition="<<head.x<<','<<head.y<<','<<head.z
         <<" target="<<target.x<<','<<target.y<<','<<target.z;
        Log(s.str());
    }
    if(GakumasLocal::Config::vrDiagnosticsStartupEnabled && b.naturalSamples==36000)
        Log("NATURAL_GAZE_GAP reason=sample-limit limit=36000");
}
