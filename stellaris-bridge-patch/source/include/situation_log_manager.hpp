#pragma once

#include "common.hpp"
#include <string>
#include <vector>

namespace bridge {

struct SituationItem {
    uint32_t id{ 0 };
    std::string key;
    std::string name;
    std::string current_approach;
    std::string current_approach_name;
    double progress{ 0.0 };
    double monthly_change{ 0.0 };
    int32_t stage{ 0 };                 // CSituation::stage (+0x578), 0-based
    double stage_min{ 0.0 };            // cached stage range: {begin,end} at +0x588 / +0x590
    double stage_max{ 0.0 };
    uint32_t owner_country_id{ 0 };
    bool is_player{ false };
};

struct SpecialProjectItem {
    uint32_t id{ 0 };
    std::string key;
    std::string name;
    std::string kind;                   // project, species_modification, uplift, debris
    int32_t days_left{ -1 };            // -1: no deadline
    uint32_t species_id{ 0xFFFFFFFF };  // species modification / uplift: the resulting species
    uint32_t debris_id{ 0xFFFFFFFF };   // debris projects: the debris
    // where the project is researched: an ESpatialObjectType reference (0xFFFFFFFF: nowhere)
    int32_t status{ 0 };                // CSpecialProjectInstance::status (+0x1C8)
    std::string location_name;          // 位置的名字（行星/舰队/星系/残骸所在星系…），取不到就空
    uint32_t location_type{ 0xFFFFFFFF };
    uint32_t location_id{ 0xFFFFFFFF };
};

// A discovered anomaly the country has not researched yet (the planet still holds its category).
struct AnomalyItem {
    uint32_t planet_id{ 0xFFFFFFFF };
    std::string planet_name;
    std::string key;
    std::string name;
    uint32_t system_id{ 0xFFFFFFFF };   // 异常所在星系（research_anomalies 要的就是它）
    std::string system_name;
    int32_t difficulty{ 0 };            // anomaly 对象 +0x60，1–10 档
};

struct SituationLogSummary {
    uint32_t situations_count{ 0 };
    uint32_t special_projects_count{ 0 };
    uint32_t anomalies_count{ 0 };
};

struct FullSituationLogState {
    SituationLogSummary summary;
    std::vector<SituationItem> situations;
    std::vector<SpecialProjectItem> special_projects;
    std::vector<AnomalyItem> anomalies;
};

class SituationLogManager {
public:

    static SituationLogManager& Get();

    bool Init(uintptr_t base_address);

    // Layer 1: High-level overview/indicator
    SituationLogSummary GetSummary();

    // Layer 2: Detailed domain query
    FullSituationLogState GetSituationLogState(bool player_only = true);
    nlohmann::json GetSituationLogJson(bool player_only = true);

    // The approaches a situation offers: the type object (CSituation+0x208) points at a block
    // (type+0x68) that holds the approach objects; they are recognised by the vtable of the
    // currently selected approach (CSituation+0x210). Each approach's key is the string at +0x18.
    nlohmann::json GetSituationApproachesJson(uint32_t situation_id);

    // CCountry::events (CCountryEventManager) of the country
    std::vector<SpecialProjectItem> ReadSpecialProjects(void* country);
    std::vector<AnomalyItem> ReadAnomalies(void* country);

    // Layer 3: Action command execution
    nlohmann::json SetSituationApproach(uint32_t situation_id, const std::string& approach_key);

    // 只跑 IsValid、不 Post：回答「这个取向现在能不能选」，拒绝原因用引擎原文
    nlohmann::json ValidateSituationApproachJson(uint32_t situation_id, const std::string& approach_key);

    // Read-only probe: hex dump of one situation object, [sit+offset, +length).
    // Used to locate the cached stage range (the UI's "active between x and y").
        // 只读：行星上 anomaly 对象的原始字节（对比“正在调查”与未调查的差异）
    nlohmann::json ProbeAnomalyJson(uint32_t planet_id, int32_t offset, int32_t length);
    // 起/停「直接研究」类特殊项目（CResearchProjectCommand / CCancelProjectCommand）
    nlohmann::json ResearchProjectJson(uint32_t project_id, bool cancel);

    nlohmann::json ProbeSituationJson(uint32_t situation_id, uint32_t offset, uint32_t length, uint64_t vec_vtable = 0);

    // Read-only probe: hex dump of the situation's type object (CSituationType, pointed to by
    // CSituation+0x208) — the place the stage list, the stage effects and the approach list live.
    // follow=false (default) only dumps bytes; follow=true also chases every qword as a pointer,
    // which is the risky part (a bad pointer turns into an allocation, not an access violation).
    nlohmann::json ProbeSituationTypeJson(uint32_t situation_id, uint32_t offset, uint32_t length, bool follow,
                                          uint64_t match_vtable = 0, uint64_t vec_vtable = 0);

private:
    SituationLogManager() = default;

    uintptr_t base_address_{ 0 };



    void* GetPlayerCountry();
    uint32_t GetPlayerCountryId();

    std::string LocalizeKey(const std::string& key);
};

} // namespace bridge
