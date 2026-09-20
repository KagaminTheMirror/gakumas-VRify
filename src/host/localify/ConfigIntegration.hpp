#pragma once
#include <string>
#include <nlohmann/json_fwd.hpp>
namespace GakumasLocal::Config::Integration {
    void BeginLoad();
    void CheckDocument(const nlohmann::json& document);
    void Parsed(const nlohmann::json& document);
    void LoadFailed();
    nlohmann::json SaveBase(const std::string& path);
}
