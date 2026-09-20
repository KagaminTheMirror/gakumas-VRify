#include "LiveGazeTrace.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "GakumasLocalify/Il2cppUtils.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <iomanip>
#include <locale>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace gakumas::vr {
namespace {
using U = UnityResolve;
struct Subject {
    void* actorRoot{}; void* effectorRoot{};
    int endFrame = -1, lastEnabled = -1;
    unsigned lines = 0;
    bool gap = false;
    std::array<float,4> lastHead{};
    bool hasHead = false;
    std::array<float,6> curves{};
    unsigned curveMask = 0;
    std::deque<std::string> history;
};
std::vector<Subject> subjects;
std::unordered_set<void*> dumped;
unsigned codeBytes = 0;
unsigned focusedCodeBytes = 0;
std::unordered_set<void*> focusedFunctions;
constexpr unsigned sampleLimit = 60000;
std::uint64_t sequence = 0;
void Log(const std::string& s) { WriteVrLog("[VR][gaze-trace] " + s); }
std::ostringstream Stream() { std::ostringstream s; s.imbue(std::locale::classic()); return s; }
bool Read(const void* p, void* out, size_t size) noexcept {
    if (!p) return false;
    __try { memcpy(out,p,size); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* Klass(void* p) { void* c=nullptr; Read(p,&c,sizeof(c)); return c; }
bool Alive(void* p) {
    void* n=nullptr;
    return p && Read(static_cast<char*>(p)+offsetof(U::UnityType::UnityObject,m_CachedPtr),&n,sizeof(n)) && n;
}
void* Target(void* h) { return h ? U::Invoke<void*>("il2cpp_gchandle_get_target",h) : nullptr; }
void Free(void* h) { if(h) U::Invoke<void>("il2cpp_gchandle_free",h); }
std::string Type(void* t) {
    if(!t) return {};
    auto* p=U::Invoke<char*>("il2cpp_type_get_name",t);
    std::string s=p?p:"";
    if(p) U::Invoke<void>("il2cpp_free",p);
    return s;
}
void DumpFocusedCode(void* fn,const char* role,unsigned count=16384) {
    constexpr unsigned budget=196608;
    if(!fn || focusedCodeBytes>=budget || !focusedFunctions.insert(fn).second)return;
    count=(count<budget-focusedCodeBytes)?count:budget-focusedCodeBytes;
    std::vector<unsigned char> bytes(count);
    if(!Read(fn,bytes.data(),bytes.size())) {Log("GAZE_TRACE_SKIP reason=focused-code-read");return;}
    auto b=Stream();b<<"GAZE_TRACE_CODE function="<<fn<<" module="<<GetModuleHandleW(L"GameAssembly.dll")
        <<" bytes="<<bytes.size()<<" hex="<<std::hex<<std::setfill('0');
    for(auto v:bytes)b<<std::setw(2)<<unsigned(v);
    Log(b.str());focusedCodeBytes+=count;
    auto label=Stream();label<<"GAZE_TRACE_CODE_ROLE function="<<fn<<" role="<<role;Log(label.str());
}
void Dump(void* c) {
    for(unsigned depth=0;c && depth<12;++depth,c=U::Invoke<void*>("il2cpp_class_get_parent",c)) {
        if(!dumped.insert(c).second) continue;
        const char* name=U::Invoke<const char*>("il2cpp_class_get_name",c);
        auto header=Stream(); header<<"GAZE_TRACE_CLASS class="<<c<<" name="<<name; Log(header.str());
        void* it=nullptr;
        while(auto* f=U::Invoke<void*>("il2cpp_class_get_fields",c,&it)) {
            auto s=Stream(); s<<"GAZE_TRACE_FIELD class="<<c<<" name="<<U::Invoke<const char*>("il2cpp_field_get_name",f)
                <<" type="<<Type(U::Invoke<void*>("il2cpp_field_get_type",f))
                <<" offset="<<U::Invoke<int>("il2cpp_field_get_offset",f)
                <<" flags="<<U::Invoke<int>("il2cpp_field_get_flags",f); Log(s.str());
        }
        it=nullptr;
        while(auto* m=U::Invoke<void*>("il2cpp_class_get_methods",c,&it)) {
            const char* mn=U::Invoke<const char*>("il2cpp_method_get_name",m);
            const auto argc=U::Invoke<unsigned>("il2cpp_method_get_param_count",m);
            void* fn=nullptr; Read(m,&fn,sizeof(fn));
            auto s=Stream(); s<<"GAZE_TRACE_METHOD class="<<c<<" name="<<mn<<" info="<<m<<" function="<<fn
                <<" return="<<Type(U::Invoke<void*>("il2cpp_method_get_return_type",m));
            for(unsigned i=0;i<argc;++i) s<<" arg"<<i<<"="<<Type(U::Invoke<void*>("il2cpp_method_get_param",m,i));
            Log(s.str());
            const std::string n=mn?mn:"";
            const std::string owner=name?name:"";
            if(owner=="ActorAnimationFullBodyIKJobSkeleton" && (n=="Execute"||n=="WriteBack"||n=="SetLookAtEffector"))
                DumpFocusedCode(fn,n.c_str());
            if(owner=="ActorAnimationFullBodyIKWriteBack" && n==".ctor")DumpFocusedCode(fn,"writeback-binding",8192);
            if(owner=="ActorAnimationJobBinder`2" && n=="Update")DumpFocusedCode(fn,"binder-update",8192);
            if(owner=="CampusActorAnimationJob" && n=="ProcessFullBodyIK" && fn) {
                // Current dev.471 in-process code: lea r8,[rdi+0x300], lea rdx,
                // lea rcx, call rel32 at +0x7f. Read only; never call this address.
                const unsigned char expected[]={0x4c,0x8d,0x87,0x00,0x03,0x00,0x00,
                    0x48,0x8d,0x54,0x24,0x20,0x48,0x8d,0x4c,0x24,0x60,0xe8};
                std::array<unsigned char,sizeof(expected)> actual{};
                auto* call=static_cast<char*>(fn)+0x7f;
                std::int32_t relative=0;
                if(Read(static_cast<char*>(fn)+0x6e,actual.data(),actual.size()) &&
                   std::memcmp(actual.data(),expected,sizeof(expected))==0 && Read(call+1,&relative,sizeof(relative))) {
                    auto* destination=call+5+relative;
                    auto edge=Stream();edge<<"GAZE_TRACE_DIRECT_EDGE source="<<fn<<" call="<<static_cast<void*>(call)
                        <<" destination="<<static_cast<void*>(destination)<<" role=generic-fullbody";Log(edge.str());
                    DumpFocusedCode(destination,"generic-fullbody",65536);
                } else Log("GAZE_TRACE_SKIP reason=fullbody-call-prefix-mismatch");
            }
            if(fn && codeBytes<262144 && (n.find("Update")!=std::string::npos || n.find("LookAt")!=std::string::npos || n.find("Process")!=std::string::npos || n.find("Weight")!=std::string::npos)) {
                const unsigned wanted=n.find("ProcessAnimation")!=std::string::npos ? 16384u : 2048u;
                const unsigned remaining=262144-codeBytes;
                std::vector<unsigned char> bytes(wanted<remaining?wanted:remaining);
                if(Read(fn,bytes.data(),bytes.size())) {
                    auto b=Stream(); b<<"GAZE_TRACE_CODE function="<<fn<<" module="<<GetModuleHandleW(L"GameAssembly.dll")
                        <<" bytes="<<bytes.size()<<" hex="<<std::hex<<std::setfill('0');
                    for(auto v:bytes) b<<std::setw(2)<<unsigned(v);
                    Log(b.str()); codeBytes+=unsigned(bytes.size());
                }
            }
        }
    }
}
void DumpAnimationTypes(void* c,unsigned depth=0) {
    static std::unordered_set<void*> visited;
    if(!c || depth>3 || visited.size()>=64 || !visited.insert(c).second)return;
    Dump(c);
    void* it=nullptr;
    while(auto* field=U::Invoke<void*>("il2cpp_class_get_fields",c,&it)) {
        auto* type=U::Invoke<void*>("il2cpp_field_get_type",field);
        const auto name=Type(type);
        if(name.rfind("ActorAnimation.",0)==0)
            DumpAnimationTypes(U::Invoke<void*>("il2cpp_class_from_type",type),depth+1);
    }
}
void* Method(void* c,const char* name,const char* result,std::initializer_list<const char*> args,bool isStatic=false) {
    for(unsigned depth=0;c && depth<12;++depth,c=U::Invoke<void*>("il2cpp_class_get_parent",c)) {
        void* it=nullptr;
        while(auto* m=U::Invoke<void*>("il2cpp_class_get_methods",c,&it)) {
            unsigned impl=0;
            const auto flags=U::Invoke<unsigned>("il2cpp_method_get_flags",m,&impl);
            if(bool(flags&0x10)!=isStatic) continue;
            if(strcmp(U::Invoke<const char*>("il2cpp_method_get_name",m),name)!=0 ||
               U::Invoke<unsigned>("il2cpp_method_get_param_count",m)!=args.size() ||
               Type(U::Invoke<void*>("il2cpp_method_get_return_type",m))!=result) continue;
            unsigned i=0; bool ok=true;
            for(auto a:args) if(Type(U::Invoke<void*>("il2cpp_method_get_param",m,i++))!=a) ok=false;
            if(ok) return m;
        }
    }
    return nullptr;
}
int Offset(void* c,const char* name) {
    auto* f=U::Invoke<void*>("il2cpp_class_get_field_from_name",c,name);
    if(!f || (U::Invoke<int>("il2cpp_field_get_flags",f)&0x10)) return -1;
    return U::Invoke<int>("il2cpp_field_get_offset",f);
}
template<class T> bool Value(void* obj,const char* name,T& v) {
    if(!obj) return false;
    const auto offset=Offset(Klass(obj),name);
    return offset>=16 && Read(static_cast<char*>(obj)+offset,&v,sizeof(v));
}
bool Call(void* m,void* obj,void** args,void** out) noexcept {
    if(!m) return false;
    using Fn=void*(*)(void*,void*,void**,void**);
    static auto f=reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"il2cpp_runtime_invoke"));
    if(!f) return false;
    void* ex=nullptr;
    __try {*out=f(m,obj,args,&ex);return !ex;} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T> bool BoxValue(void* method,void* obj,void** args,T& v) {
    void* box=nullptr;
    return Call(method,obj,args,&box) && box && Read(U::Invoke<void*>("il2cpp_object_unbox",box),&v,sizeof(v));
}
void* boneMethod=nullptr; void* positionMethod=nullptr; void* rotationMethod=nullptr; void* frameMethod=nullptr;
std::array<int,4> bones{-1,-1,-1,-1};
bool resolved=false;
void Resolve(void* actor) {
    if(resolved) return;
    resolved=true;
    auto* transform=Il2cppUtils::GetClass("UnityEngine.CoreModule.dll","UnityEngine","Transform");
    auto* time=Il2cppUtils::GetClass("UnityEngine.CoreModule.dll","UnityEngine","Time");
    auto* ids=Il2cppUtils::GetClass("UnityEngine.AnimationModule.dll","UnityEngine","HumanBodyBones");
    Dump(Klass(actor));
    if(transform) {Dump(transform->address);positionMethod=Method(transform->address,"get_position","UnityEngine.Vector3",{});rotationMethod=Method(transform->address,"get_rotation","UnityEngine.Quaternion",{});}
    if(time) {Dump(time->address);frameMethod=Method(time->address,"get_frameCount","System.Int32",{},true);}
    boneMethod=Method(Klass(actor),"GetHumanBodyBoneTransform","UnityEngine.Transform",{"UnityEngine.HumanBodyBones"});
    if(ids) {
        Dump(ids->address);
        const char* names[]={"Hips","Chest","Neck","Head"};
        for(size_t i=0;i<bones.size();++i) {
            auto* field=U::Invoke<void*>("il2cpp_class_get_field_from_name",ids->address,names[i]);
            if(field) U::Invoke<void>("il2cpp_field_static_get_value",field,&bones[i]);
        }
    }
    auto s=Stream(); s<<"GAZE_TRACE_READY boneMethod="<<boneMethod<<" position="<<positionMethod<<" rotation="<<rotationMethod;
    for(auto id:bones) s<<" bone="<<id; Log(s.str());
}
}
void TraceLiveGazeCurve(void* effector,unsigned index,float value) noexcept {
    if(!GakumasLocal::Config::vrDiagnosticsStartupEnabled || index>=6)return;
    for(auto& s:subjects) if(Target(s.effectorRoot)==effector) {
        s.curves[index]=value;s.curveMask|=1u<<index;break;
    }
}
void RegisterLiveGazeTrace(void* actor,void* effector) noexcept {
    if(!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    try {
        if(!Alive(actor)||!Alive(effector)||subjects.size()>=16) return;
        for(auto& s:subjects) if(Target(s.actorRoot)==actor) return;
        Resolve(actor); Dump(Klass(effector));
        void* animation=nullptr;
        if(Value(actor,"_actorAnimation",animation) && animation) {
            Dump(Klass(animation));
            void* rig=nullptr;
            if(Value(animation,"_actorAnimationRig",rig) && rig) {
                Dump(Klass(rig));
                // dev.469 live generic rig table: _job is IAnimationJob.
                // This is a managed box, not a UnityEngine.Object.
                void* job=nullptr;
                if(Value(rig,"_job",job)&&job) {
                    auto line=Stream();line<<"GAZE_TRACE_JOB rig="<<rig<<" job="<<job<<" class="<<Klass(job);Log(line.str());
                    DumpAnimationTypes(Klass(job));
                } else Log("GAZE_TRACE_SKIP reason=job-not-ready");
                // Actual generic RigConstraint table captured in dev.469.
                auto* binderField=U::Invoke<void*>("il2cpp_class_get_field_from_name",Klass(rig),"s_Binder");
                if(binderField && (U::Invoke<int>("il2cpp_field_get_flags",binderField)&0x10)) {
                    void* binder=nullptr;
                    U::Invoke<void>("il2cpp_field_static_get_value",binderField,&binder);
                    if(binder)DumpAnimationTypes(Klass(binder));
                }
            }
        }
        Subject s;
        s.actorRoot=U::Invoke<void*>("il2cpp_gchandle_new",actor,false);
        s.effectorRoot=U::Invoke<void*>("il2cpp_gchandle_new",effector,false);
        if(!s.actorRoot||!s.effectorRoot){Free(s.actorRoot);Free(s.effectorRoot);return;}
        subjects.push_back(std::move(s));
    } catch(...) {Log("GAZE_TRACE_SKIP reason=registration");}
}
void ClearLiveGazeTrace() noexcept {
    for(auto& s:subjects){Free(s.actorRoot);Free(s.effectorRoot);} subjects.clear();
}
void TraceLiveGazeStage(void* actor,void* effector,const char* stage,void* caller,void* controller) noexcept {
    if(!GakumasLocal::Config::vrDiagnosticsStartupEnabled || (!actor&&!effector)) return;
    try {
        for(auto& s:subjects) {
            auto* a=Target(s.actorRoot); auto* e=Target(s.effectorRoot);
            if((actor && actor!=a)||(effector && effector!=e)||!Alive(a)||!Alive(e)) continue;
            if(s.lines>=sampleLimit) {if(!s.gap){Log("GAZE_TRACE_GAP reason=subject-line-limit");s.gap=true;} return;}
            int frame=-1; BoxValue(frameMethod,nullptr,nullptr,frame);
            if(strcmp(stage,"weight-before")==0 || strcmp(stage,"weight-recompute")==0)s.curveMask=0;
            auto line=Stream(); line<<"GAZE_TRACE_SAMPLE seq="<<++sequence<<" actor="<<a<<" effector="<<e<<" frame="<<frame<<" stage="<<stage<<" caller="<<caller;
            line<<" sampleMicros="<<std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            // Read only: the names are proven fields, but their timing and space
            // remain unverified until WriteBack's actual consumer is decoded.
            void* animation=nullptr;
            if(Value(a,"_actorAnimation",animation)&&Alive(animation)) {
                const char* names[]={"fullBodyIkHeadOriginalPositionX","fullBodyIkHeadOriginalPositionY",
                    "fullBodyIkHeadOriginalPositionZ","fullBodyIkHeadOriginalRotationX",
                    "fullBodyIkHeadOriginalRotationY","fullBodyIkHeadOriginalRotationZ"};
                for(size_t i=0;i<6;++i) {float v=0;if(Value(animation,names[i],v))line<<" authoredField"<<i<<'='<<v;}
            }
            unsigned char enabled=0; Value(e,"isEnableLookAt",enabled);
            line<<" enabled="<<unsigned(enabled);
            if(Alive(controller)) {
                for(auto name:{"_liveEnableLookAt","_isProhibitLookEnable","_isConstraintOutRange","_isFocusTarget"}) {
                    unsigned char v=0;if(Value(controller,name,v))line<<' '<<name<<'='<<unsigned(v);
                }
                void* inner=nullptr;
                if(Value(controller,"_lookAtController",inner)&&Alive(inner)) {
                    unsigned char v=0;if(Value(inner,"_shouldLookEnabled",v))line<<" shouldLook="<<unsigned(v);
                }
            }
            for(auto name:{"sampleTime","weight","eyesWeight","headWeight","bodyWeight","enableLimitWeight"}) {
                float v=0; if(Value(e,name,v))line<<' '<<name<<'='<<v;else line<<' '<<name<<"=missing";
            }
            line<<" curveMask="<<s.curveMask;
            for(size_t i=0;i<6;++i) if(s.curveMask&(1u<<i))line<<" curve"<<i<<'='<<s.curves[i];
            void* target=nullptr; std::array<float,3> tp{};
            if(Value(e,"target",target)&&Alive(target)&&BoxValue(positionMethod,target,nullptr,tp))
                line<<" target="<<tp[0]<<','<<tp[1]<<','<<tp[2];
            bool jump=false; void* boneMap=nullptr;
            if(Value(a,"_humanBodyBoneMap",boneMap)&&boneMap&&boneMethod) for(size_t i=0;i<bones.size();++i) {
                if(bones[i]<0)continue;
                void* args[]={&bones[i]};void* t=nullptr;
                std::array<float,3> p{};std::array<float,4> q{};
                if(!Call(boneMethod,a,args,&t)||!Alive(t)||!BoxValue(positionMethod,t,nullptr,p)||!BoxValue(rotationMethod,t,nullptr,q))continue;
                line<<" bone"<<i<<'='<<p[0]<<','<<p[1]<<','<<p[2]<<','<<q[0]<<','<<q[1]<<','<<q[2]<<','<<q[3];
                if(i==3 && strcmp(stage,"actor-late-after")==0) {
                    float dot=0;for(size_t j=0;j<4;++j)dot+=q[j]*s.lastHead[j];
                    jump=s.hasHead&&std::abs(dot)<0.994522f; // >12 degrees between final samples.
                    s.lastHead=q;s.hasHead=true;
                }
            }
            const bool event=s.lastEnabled!=enabled||jump;
            s.lastEnabled=enabled;
            if(event) s.endFrame=frame+45;
            if(event || (frame>=0 && frame<=s.endFrame)) {
                while(!s.history.empty() && s.lines<sampleLimit){Log(s.history.front());s.history.pop_front();++s.lines;}
                if(s.lines<sampleLimit){Log(line.str());++s.lines;}
            } else {
                s.history.push_back(line.str());if(s.history.size()>512)s.history.pop_front();
            }
            return;
        }
    } catch(...) {Log("GAZE_TRACE_SKIP reason=sample");}
}
}
