#include <Windows.h>
#include <string>
#include <unordered_map>
#include "gkmsGUI/GUII18n.hpp"
#include "gkmsGUI/i18nData/strings_en.hpp"
#include "gkmsGUI/i18nData/strings_ja.hpp"
#include "gkmsGUI/i18nData/strings_zh-rCN.hpp"
#include "gkmsGUI/i18nData/strings_zh-rTW.hpp"
#include "vr/config/VrifyConfig.hpp"

// The desktop keeps upstream's system language. It must not read the VR
// menu's mutable language setting on its separate thread.
namespace GkmsGUII18n {
namespace {
const auto language = GetUserDefaultUILanguage();
bool Simplified() { return language == 0x0004 || language == 0x0804 || language == 0x1004; }
bool Traditional() { return language == 0x0404 || language == 0x0c04 || language == 0x1404 || language == 0x048E; }
const std::unordered_map<std::string, std::string>& Table() {
    if (Simplified()) return I18nData::i18nData_zh_rCN;
    if (Traditional()) return I18nData::i18nData_zh_rTW;
    if (language == 0x0011 || language == 0x0411) return I18nData::i18nData_ja;
    return I18nData::i18nData_default;
}
}

const char* ts(const std::string& key) {
    if (key == "enable_free_camera" && GakumasLocal::Config::vrRuntimeStartupEnabled) {
        if (Simplified()) return "自由相机（VR 运行期间不生效）";
        if (Traditional()) return "自由相機（VR 執行期間不生效）";
        if (language == 0x0011 || language == 0x0411) return "フリーカメラ（VR 実行中は無効）";
        return "Free camera (inactive while VR is running)";
    }
    const auto& table = Table();
    if (auto it = table.find(key); it != table.end()) return it->second.c_str();
    if (auto it = I18nData::i18nData_default.find(key); it != I18nData::i18nData_default.end()) {
        return it->second.c_str();
    }
    static thread_local std::unordered_map<std::string, std::string> fallback;
    return fallback.emplace(key, key).first->second.c_str();
}
}
