// VR-owned integration, included by the patched upstream Hook translation unit.
    bool LocalizationActive() {
        return Config::vrLocalizeText;
    }

    void EnsureLocalizationData() {
        if (!LocalizationActive()) {
            return;
        }
        static std::once_flag once;
        std::call_once(once, []() {
            Local::LoadData();
            MasterLocal::LoadData();
        });
    }

    bool TryGetI18n(const std::string& key, std::string* out) {
        if (!LocalizationActive() || out == nullptr) {
            return false;
        }
        EnsureLocalizationData();
        return Local::GetI18n(key, out);
    }

    bool TryGetGenericText(const std::string& origText, std::string* out) {
        if (!LocalizationActive() || out == nullptr) {
            return false;
        }
        EnsureLocalizationData();
        return Local::GetGenericText(origText, out);
    }
