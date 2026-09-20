#include <string>
#include <vector>
#include <unordered_map>
#include "host/localify/DesktopText.hpp"
#include "i18nData/strings_en.hpp"
#include "i18nData/strings_ja.hpp"
#include "i18nData/strings_zh-rCN.hpp"
#include "i18nData/strings_zh-rTW.hpp"
#include "../config/VrifyConfig.hpp"


namespace GakumasVrI18n {
	int DetectSystemMenuLanguage() { return GkmsGUII18n::SystemMenuLanguage(); }

	int ResolvedMenuLanguage() {
		const int stored = GakumasLocal::Config::vrMenuLanguage;
		if (stored == GakumasLocal::Config::kVrMenuLanguageSystem ||
			stored < GakumasLocal::Config::kVrMenuLanguageSystem ||
			stored > GakumasLocal::Config::kVrMenuLanguageEn) {
			return DetectSystemMenuLanguage();
		}
		return stored;
	}

	const std::unordered_map<std::string, std::string>& TableForLanguage(
		int language) {
		switch (language) {
		case GakumasLocal::Config::kVrMenuLanguageZhCN:
			return I18nData::i18nData_zh_rCN;
		case GakumasLocal::Config::kVrMenuLanguageZhTW:
			return I18nData::i18nData_zh_rTW;
		case GakumasLocal::Config::kVrMenuLanguageJa:
			return I18nData::i18nData_ja;
		default:
			return I18nData::i18nData_default;
		}
	}

	void AppendTable(
		std::vector<std::string>& strings,
		const std::unordered_map<std::string, std::string>& table) {
		for (const auto& [key, value] : table) {
			strings.push_back(value);
		}
	}

    std::vector<std::string> ActiveStrings() {
		std::vector<std::string> strings;
		strings.reserve(
			I18nData::i18nData_zh_rCN.size() +
			I18nData::i18nData_zh_rTW.size() +
			I18nData::i18nData_ja.size() +
			I18nData::i18nData_default.size());
		// Atlas every language so a mid-session switch cannot fall back to '?'.
		AppendTable(strings, I18nData::i18nData_zh_rCN);
		AppendTable(strings, I18nData::i18nData_zh_rTW);
		AppendTable(strings, I18nData::i18nData_ja);
		AppendTable(strings, I18nData::i18nData_default);
        GakumasVR::Localify::AppendCommonMenuStrings(strings);
        return strings;
    }

    const char* ts(const std::string& key) {
        const auto& i18nData = TableForLanguage(ResolvedMenuLanguage());

        if (auto it = i18nData.find(key); it != i18nData.end()) {
            return it->second.c_str();
        }
        if (const char* text = GakumasVR::Localify::CommonMenuText(key, ResolvedMenuLanguage())) return text;
        if (auto it = I18nData::i18nData_default.find(key); it != I18nData::i18nData_default.end()) {
            return it->second.c_str();
        }


        static thread_local std::unordered_map<std::string, std::string> fallbackMap;
        auto [iter, inserted] = fallbackMap.emplace(key, key);
        return iter->second.c_str();
    }

}
