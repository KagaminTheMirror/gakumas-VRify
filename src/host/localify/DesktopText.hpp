#pragma once
#include <string>
#include <vector>
namespace GkmsGUII18n { int SystemMenuLanguage(); }
namespace GakumasVR::Localify {
    const char* DesktopTextOverride(const std::string& key, unsigned short language);
    const char* CommonMenuText(const std::string& key, int language);
    void AppendCommonMenuStrings(std::vector<std::string>& strings);
}
