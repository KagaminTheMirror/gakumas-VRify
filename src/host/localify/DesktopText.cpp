#include "DesktopText.hpp"
#include "vr/config/VrifyConfig.hpp"
#include "gkmsGUI/i18nData/strings_en.hpp"
#include "gkmsGUI/i18nData/strings_ja.hpp"
#include "gkmsGUI/i18nData/strings_zh-rCN.hpp"
#include "gkmsGUI/i18nData/strings_zh-rTW.hpp"
namespace GakumasVR::Localify {
const char* DesktopTextOverride(const std::string& key, unsigned short language) {
    if (key != "enable_free_camera" || !GakumasLocal::Config::vrRuntimeStartupEnabled) return nullptr;
    switch (language) {
    case 0x0004: case 0x0804: case 0x1004: return "自由相机（VR 运行期间不生效）";
    case 0x0404: case 0x0c04: case 0x1404: case 0x048E: return "自由相機（VR 執行期間不生效）";
    case 0x0011: case 0x0411: return "フリーカメラ（VR 実行中は無効）";
    default: return "Free camera (inactive while VR is running)";
    }
}
const char* CommonMenuText(const std::string& key, int language) {
    const auto* table = &I18nData::i18nData_default;
    switch (language) {
    case 1: table = &I18nData::i18nData_zh_rCN; break;
    case 2: table = &I18nData::i18nData_zh_rTW; break;
    case 3: table = &I18nData::i18nData_ja; break;
    }
    const auto it = table->find(key);
    return it == table->end() ? nullptr : it->second.c_str();
}
void AppendCommonMenuStrings(std::vector<std::string>& strings) {
    for (int language = 1; language <= 4; ++language)
        for (const char* key : {"hign", "middle", "low", "cancel", "ok"})
            if (const char* text = CommonMenuText(key, language)) strings.emplace_back(text);
}
}
