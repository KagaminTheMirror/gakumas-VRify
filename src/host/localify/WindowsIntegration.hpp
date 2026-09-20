#pragma once
#include <Windows.h>
#include <string>
namespace GakumasVR::Localify {
    void* LoadBundle(const std::string& path);
    bool WindowMessage(HWND window, UINT message, WPARAM w, LPARAM l, WNDPROC previous, LRESULT& result);
    void InstallWindowProcedure(WNDPROC callback, WNDPROC* previous);
}
