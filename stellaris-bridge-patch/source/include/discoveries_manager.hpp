#pragma once

#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace bridge {

class DiscoveriesManager {
public:
    static DiscoveriesManager& Get();


    bool Init(uintptr_t base_address);

    // Queries
    nlohmann::json GetDiscoveriesInfo(const std::string& tab, bool probe = false);
    nlohmann::json GetSummaryJson();
    nlohmann::json ProbeGrandArchive(uint32_t country_id, bool decode = false);

    // Actions
    bool ActivateRelic(const std::string& relic_key, std::string& out_message);
    bool ActivateAstralAction(const std::string& action_key, std::string& out_message);
    nlohmann::json GetArtifactActions(bool probe);
    bool ActivateArtifactAction(const std::string& action_key, std::string& out_message);
    nlohmann::json ProbeCountryFlags();
    bool ToggleExhibit(uint32_t exhibit_id, std::string& out_message, bool force_toggle = false);
    bool MoveSpecimen(uint32_t exhibit_id, bool to_storage, std::string& out_message, bool force = false);

private:
    DiscoveriesManager() = default;

    void* GetPlayerCountry();
    std::string LocalizeKey(const std::string& key);

    uintptr_t base_address_{ 0 };
};

} // namespace bridge






