    void EnsureExtraAssetBundle() {
        if (!LocalizationActive()) {
            return;
        }
#ifdef GKMS_WINDOWS
        if (g_extra_assetbundle_paths.empty()) {
            g_extra_assetbundle_paths.push_back(
                (gakumasLocalPath / "local-files/gakumasassets").string());
        }
        extraAssetBundleLoadAllowed.store(true, std::memory_order_release);
#endif
        if (!extraAssetBundleLoadAllowed.load(std::memory_order_acquire)) {
            return;
        }
        LoadExtraAssetBundle();
    }
