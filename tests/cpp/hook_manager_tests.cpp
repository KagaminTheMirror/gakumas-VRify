#include <cstdio>
#include <cassert>
#include <set>
#include "hooks/HookManager.hpp"
namespace GakumasLocal::Log { void ErrorFmt(const char*, ...) {} }
namespace {
std::set<void*> created;
int createCalls = 0, createFailure = -1;
bool queueFailure = false, applyFailure = false, enableFailure = false;
void* trampoline(void* target) { return reinterpret_cast<void*>(reinterpret_cast<std::size_t>(target)+0x1000); }
}
MH_STATUS WINAPI MH_Initialize() {return MH_OK;}
MH_STATUS WINAPI MH_Uninitialize() {created.clear();return MH_OK;}
MH_STATUS WINAPI MH_CreateHook(LPVOID target, LPVOID, LPVOID* original) {
    if (++createCalls == createFailure) return MH_ERROR_UNSUPPORTED_FUNCTION;
    if (!created.insert(target).second) return MH_ERROR_ALREADY_CREATED;
    *original = trampoline(target); return MH_OK;
}
MH_STATUS WINAPI MH_RemoveHook(LPVOID target) {created.erase(target);return MH_OK;}
MH_STATUS WINAPI MH_EnableHook(LPVOID) {return enableFailure ? MH_ERROR_MEMORY_ALLOC : MH_OK;}
MH_STATUS WINAPI MH_DisableHook(LPVOID) {return MH_OK;}
MH_STATUS WINAPI MH_QueueEnableHook(LPVOID) {return queueFailure ? MH_ERROR_NOT_CREATED : MH_OK;}
MH_STATUS WINAPI MH_ApplyQueued() {return applyFailure ? MH_ERROR_MEMORY_ALLOC : MH_OK;}
const char* WINAPI MH_StatusToString(MH_STATUS) {return "fixture";}
int main() {
    using namespace GakumasVR::Hooks;
    void *first=nullptr,*second=nullptr;
    Request requests[]{{reinterpret_cast<void*>(1),reinterpret_cast<void*>(3),&first,"first"},
                       {reinterpret_cast<void*>(2),reinterpret_cast<void*>(4),&second,"second"}};
    assert(!CreateAndEnableBatch(requests,2));
    assert(Initialize()); assert(Initialize());
    createFailure = 2;
    assert(!CreateAndEnableBatch(requests,2));
    assert(created.empty() && !first && !second);
    createFailure=-1; queueFailure=true;
    assert(!CreateAndEnableBatch(requests,2));
    assert(created.empty() && !first && !second);
    queueFailure=false; applyFailure=true;
    assert(!CreateAndEnableBatch(requests,2));
    // ApplyQueued can have enabled a prefix. Live detours retain valid trampolines.
    assert(created.size()==2 && first==trampoline(requests[0].target) && second==trampoline(requests[1].target));
    Shutdown(); assert(!IsInitialized()); assert(created.empty());
    assert(Initialize()); applyFailure=false; enableFailure=true;
    assert(CreateAndEnableHook(requests[0].target,requests[0].detour,&first)!=MH_OK);
    assert(created.empty() && !first);
    enableFailure=false;
    assert(CreateAndEnableBatch(requests,2));
    assert(first==trampoline(requests[0].target) && second==trampoline(requests[1].target));
    Shutdown();
    puts("HookManager: partial create/queue/apply failures, rollback and trampoline lifetime passed");
}
