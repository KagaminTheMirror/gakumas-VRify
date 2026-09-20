#include <cassert>
#include <atomic>
#include <string>
#include <vector>
#include <iostream>
std::vector<std::string> events;
namespace Config {
bool enabled, vrRuntimeStartupEnabled, vrDiagnosticsStartupEnabled, vrNativeOnly, enableFreeCamera;
}
namespace UnityResolve::UnityType {
struct Vector3 { float x,y,z; Vector3(float a,float b,float c):x(a),y(b),z(c){} };
struct Quaternion { float x,y,z,w; Quaternion(float a,float b,float c,float d):x(a),y(b),z(c),w(d){} };
struct Camera { void SetNearClipPlane(float) {events.push_back("near");} };
}
namespace GKCamera {
struct { float fov = 45; } baseCamera;
enum class CameraMode { FREE, FIRST_PERSON };
CameraMode GetCameraMode() {return CameraMode::FREE;}
}
UnityResolve::UnityType::Camera* mainCameraCache = nullptr;
void* cameraTransformCache = nullptr;
UnityResolve::UnityType::Camera sampleCamera;
void* mainOverride = nullptr;
bool sourceCameraQueryActive = false;
std::atomic<unsigned> sourceAdmissionMainCalls{}, sourceAdmissionMainNonNull{};
std::atomic<void*> sourceAdmissionLastMain{};
void TrackVrSourceCamera(void*) {events.push_back("track");}
UnityResolve::UnityType::Camera* Camera_get_main_Orig(void*) {events.push_back("main-original");return &sampleCamera;}
bool IsNativeObjectAlive(void*) {return true;}
void CheckAndUpdateMainCamera(UnityResolve::UnityType::Camera* = nullptr) {events.push_back("desktop");}
bool ComputeFreeCameraPose(UnityResolve::UnityType::Vector3*, UnityResolve::UnityType::Quaternion*) {return false;}
void BeforeCameraState(void*,void*) {events.push_back("before");}
void AfterCameraState(void*) {events.push_back("after");}
void CinemachineBrain_PushStateToUnityCamera_Orig(void*,void*,void*) {events.push_back("original");}
void EndCameraRendering_Hook(void*,void*,void*);
bool nested = false;
void EndCameraRendering_Orig(void*,void*,void*) {
    events.push_back("end-original");
    if (nested) { nested = false; EndCameraRendering_Hook(nullptr,reinterpret_cast<void*>(2),nullptr); }
}
void Unity_set_fieldOfView_Orig(void*,float) {events.push_back("fov");}
bool originalDof = true;
bool VLDOF_IsActive_Orig(void*) {events.push_back("dof-original");return originalDof;}
struct Renderer {
    void* MainCameraOverride() {return mainOverride;}
    bool suppress = false, throws = false;
    bool OnEndCamera(void* camera) {
        events.push_back("end-callback");
        if (throws) throw 1;
        return camera == reinterpret_cast<void*>(2);
    }
    bool ShouldSuppressDepthOfField(void*,bool) {events.push_back("dof-callback");return suppress;}
} unityStereoRenderer;
namespace gakumas::vr {
bool LiveSourcePhotoProtectionActive() {return false;}
namespace camera { bool IsVrFreeCameraFirstPerson(){return false;} }
}
void SetFpHeadColorSkip(bool,const char*){}
void ObserveUnityCameraRender(void*) {events.push_back("observe");}
void ReportVrUnityHookException() {events.push_back("caught");}
struct Timing {void Stop(){}};
#define VR_PERF_SCOPE(name, ...) Timing name
#define DEFINE_HOOK(ret,name,args) ret name##_Hook args
#define GKMS_WINDOWS
#include "actual_hooks.inc"
void expect(std::initializer_list<const char*> wanted) {
    std::vector<std::string> expected(wanted.begin(), wanted.end());
    assert(events == expected);
    events.clear();
}
int main() {
    alignas(16) unsigned char state[256]{};
    for (bool localify : {false,true}) for (bool vr : {false,true}) {
        Config::enabled = localify; Config::enableFreeCamera = localify;
        Config::vrRuntimeStartupEnabled = vr;
        for (bool nativeOnly : {false,true}) for (bool diagnostics : {false,true}) {
            Config::vrNativeOnly = nativeOnly;
            Config::vrDiagnosticsStartupEnabled = vr && diagnostics;
            assert(IsVrUnityRuntimeEnabled() == (vr && !nativeOnly));
            assert(AreVrUnityCameraDiagnosticsEnabled() == (vr && diagnostics && !nativeOnly));
            assert(IsLocalifyFreeCameraEnabled() == (localify && !vr));
            CinemachineBrain_PushStateToUnityCamera_Hook(nullptr,state,nullptr);
            if (localify && !vr) expect({"before","desktop","original","after"});
            else expect({"before","original","after"});
        }
    }
    Config::enableFreeCamera = false;
    nested = true;
    EndCameraRendering_Hook(nullptr,nullptr,nullptr);
    expect({"end-original","end-original","end-callback","end-callback","observe"});
    unityStereoRenderer.throws = true;
    EndCameraRendering_Hook(nullptr,nullptr,nullptr);
    expect({"end-original","end-callback","caught"});
    unityStereoRenderer.throws = false;
    for (bool original : {false,true}) for (bool suppress : {false,true}) {
        originalDof = original; unityStereoRenderer.suppress = suppress;
        assert(VLDOF_IsActive_Hook(nullptr) == (original && !suppress));
        expect({"dof-original","dof-callback"});
    }
    Config::vrRuntimeStartupEnabled = false; Config::enableFreeCamera = true;
    assert(!VLDOF_IsActive_Hook(nullptr)); expect({});
    mainOverride = &sampleCamera;
    assert(Camera_get_main_Hook(nullptr) == &sampleCamera);
    assert(mainCameraCache == nullptr); // Even an identical override must return early.
    expect({"main-original","track"});
    mainOverride = nullptr;
    assert(Camera_get_main_Hook(nullptr) == &sampleCamera);
    assert(mainCameraCache == &sampleCamera);
    expect({"main-original","track"});
    std::cout << "Shared entry contracts: runtime matrix, original counts/order, nested render, exception and early return passed\n";
}
