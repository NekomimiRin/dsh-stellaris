#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include <optional>

#include "army_access.hpp"

namespace bridge {

class NativeCommand;

struct ConstructionCard {
    std::string type;            // "building", "district"
    std::string name;            // Localized name, e.g. "场动力学中心 (Physics Lab)"
    std::string key;             // "building_physics_lab_1"
    double progress{ 0.0 };      // e.g. 0.17
    int32_t remaining_days{ 0 }; // e.g. 298
};

struct StatusAlertCard {
    std::string id;              // blockaded, occupied, construction_available, upgrade_available, unemployment,
                                 // excess_civilians, overcrowding, low_stability, blocker_available
    std::string name;            // Localized alert title
    std::string desc;            // Detailed description / tooltip
};

struct ColonyCard {
    uint32_t colony_id{ 0xFFFFFFFF };
    uint32_t planet_id{ 0xFFFFFFFF };
    std::string name;
    std::string system_name;
    uint32_t pops{ 0 };
    uint32_t size{ 0 };
    bool is_capital{ false };
    bool is_colonizing{ false };
    double colonization_progress{ 0.0 };
    int32_t remaining_days{ 0 };
    std::optional<ConstructionCard> current_construction;
    std::vector<StatusAlertCard> status_alerts;
    std::string status;
};

struct SectorGroup {
    bool unassigned{ false };  // colonies in no sector (sector_id null in JSON)
    int32_t sector_id{ 0 };
    std::string sector_name;
    uint32_t capital_planet_id{ 0 };
    std::string capital_planet_name;
    std::string focus_type;  // the sector type key (CSector::type)
    bool is_core{ false };
    uint32_t total_colonies{ 0 };
    uint32_t total_pops{ 0 };
    std::vector<ColonyCard> colonies;
};

struct MilitaryFleetCard {
    uint32_t fleet_id{ 0 };
    uint32_t template_id{ 0 };
    std::string name;
    double military_power{ 0.0 };
    uint32_t total_ships{ 0 };
    uint32_t total_quota{ 0 };
    bool can_reinforce{ false };
    std::string commander_name;
};

struct CivilianFleetCard {
    uint32_t fleet_id{ 0 };
    std::string ship_type; // "science", "construction", "colony"
    std::string name;
    std::string leader_name;
    uint32_t leader_level{ 0 };
};

struct ArmyCard {
    uint32_t army_id{ 0 };
    std::string name;
    std::string army_type; // "defense", "assault"
    std::string location_name;
    bool is_in_space{ false };
};

class OutlinerManager {
public:
    // The country's name as the game shows it (CCountry name).
    std::string CountryDisplayName(uint32_t country_id);

    static OutlinerManager& Get();

    bool Init(uintptr_t base_address);

    // Layer 1: Global Outliner Summary
    nlohmann::json GetOutlinerSummaryJson();

    // Read-only: one colony's building tree, starting from the CColony object instead of a planet.
    // Ark colonies (Nomads) have no CPlanet at all — the same CColony hangs off CShip::colony +0x100
    // — so everything below the colony is read the same way for both kinds.
    nlohmann::json ProbeColonyStructuresJson(uint32_t colony_id);
    // 宇宙风暴（COSMIC STORMS DLC）：风暴本体与影响场，场自带 timed_modifier
    nlohmann::json ProbeStormsJson();
    // 某个星系里正在刮的风暴（用于“殖民地是否在风暴内”的判定）
    nlohmann::json StormsInSystem(uint32_t system_id);
    // 以该星系为中心的影响场（风暴影响范围按场算，不只按风暴当前星系）
    nlohmann::json StormFieldsAtSystem(uint32_t system_id);

    // Read-only: deposits (planetary features) by holder. A deposit is NOT a field of the holder:
    // it lives in the CDeposit table and points back via deposit_holder {type, id} (+0x28).
    //   type 0/1 = planet-ish, type 2 = ship — ark ships carry features this way.
    // holder_id 0xFFFFFFFF = every deposit (bounded by limit).
    // Read-only: the buildable object a queued construction item points at (item +0x18), dumped as
    // u32 words. Used to compare the engine's own buildable against the one we synthesise.
    nlohmann::json ProbeBuildableJson(uint32_t item_id, uint32_t words);
    nlohmann::json ProbeDepositsJson(const std::string& key_filter, uint32_t holder_type, uint32_t holder_id,
                                     uint32_t limit, uint32_t dump_words);

    // Layer 2: Category Details
    nlohmann::json GetSectorsJson(int32_t sector_id = -1);
    nlohmann::json GetMilitaryFleetsJson();
    // 轰炸姿态：可选档位 / 给一支舰队设档（引擎的 CSetFleetGroundSupportStanceCommand）
    nlohmann::json GetBombardmentStancesJson();
    nlohmann::json SetBombardmentStanceJson(uint32_t fleet_id, const std::string& stance_key, bool dispatch);
    nlohmann::json GetCivilianFleetsJson();
    nlohmann::json GetArmiesJson();

    // Layer 3: Entity Deep Inspection
    nlohmann::json GetPlanetDetailsJson(uint32_t planet_id);

    // District Zones & Building Construction
    // The zone slots of the colony's districts and, per slot, every zone type the game would
    // queue there (CBuildableZone through the construction queue's own validation).
    nlohmann::json GetAvailableDistrictZonesJson(uint32_t planet_id, const std::string& district_type,
                                                 bool include_blocked = false);
    nlohmann::json SetDistrictZoneJson(uint32_t planet_id, uint32_t district_id, int32_t slot, const std::string& zone_key);
    // Buildings the game accepts in each zone of the planet (or one zone), with cost and build
    // time; with building_key, whether/where that building can be built and why not.
    nlohmann::json GetBuildableBuildingsJson(uint32_t planet_id, const std::string& building_key, int32_t zone_id = -1, uint32_t colony_id = 0xFFFFFFFF);
    // Which modules each free slot of a starbase accepts (probe only — nothing is queued)
    nlohmann::json GetBuildableStarbaseModulesJson(uint32_t starbase_id, int32_t slot = -1,
                                                   const std::string& module_key = "", bool include_blocked = false);
    nlohmann::json GetBuildableStarbaseBuildingsJson(uint32_t starbase_id, int32_t slot = -1,
                                                    const std::string& building_key = "", bool include_blocked = false);
    nlohmann::json BuildBuildingJson(uint32_t planet_id, const std::string& building_key, const std::string& district_type = "", int32_t slot_index = -1, uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json GetBuildableDistrictsJson(uint32_t planet_id, const std::string& district_key, uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json BuildDistrictJson(uint32_t planet_id, const std::string& district_key, uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json DemolishDistrictJson(uint32_t planet_id, const std::string& district_key,
                                        uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json CancelConstructionJson(uint32_t item_id);

    // CAddBuildableToQueueCommand with a buildable already on the engine heap (it takes it over);
    // dispatch=false only runs the command's IsValid
    bool QueueOwnedBuildable(void* heap_buildable, uint32_t country_id, uint32_t queue_id, bool dispatch, std::string* why);
    // cost and build days of a buildable (CalcCost / CalcProgressionTimeNeeded, after modifiers)
    nlohmann::json BuildableCostJsonPtr(void* buildable);
    nlohmann::json ExtractConstructionQueue(uint32_t queue_id);
    // 恒星基地的建造队列（CStarbase +0x880 → ExtractConstructionQueue）
    nlohmann::json GetStarbaseQueueJson(uint32_t starbase_id);
    // 诊断用：按 id 直接读一条建造队列（方舟/驿站排查时用，不接成日常工具）
    nlohmann::json GetQueueJson(uint32_t queue_id);
    // 扫 CShip 表（防御平台也是 CShip）：design_id 传 -1 表示全列
    nlohmann::json FindShipsJson(int design_id, uint32_t limit = 200, int system_id_filter = -1);
    // 恒星基地模块/建筑的名字：sm_ 前缀 + 格式塔变体链
    std::string LocalizeStarbaseSm(const std::string& key);
    void* FindShipDesignObj(uint32_t design_id);
    nlohmann::json UpgradeBuildingJson(uint32_t planet_id, uint32_t building_id, const std::string& upgrade_to_key = "",
                                     uint32_t colony_id = 0xFFFFFFFF);

    // Planetary Features & Subpage Operations
    nlohmann::json GetPlanetaryFeaturesJson(uint32_t planet_id);
    nlohmann::json AscendColonyJson(uint32_t planet_id, uint32_t colony_id = 0xFFFFFFFF);
    std::string LocalizeModifierType(uint32_t mod_type_id);

    // Deposit Blockers
    nlohmann::json GetClearableBlockersJson(uint32_t planet_id);
    nlohmann::json ClearBlockerJson(uint32_t planet_id, uint32_t deposit_id = 0, const std::string& deposit_key = "");

    // Planetary Decisions
    nlohmann::json GetPlanetaryDecisionsJson(uint32_t planet_id, uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json EnactDecisionJson(uint32_t planet_id, const std::string& decision_key,
                                     uint32_t colony_id = 0xFFFFFFFF);

    // Planetary Terraforming
    nlohmann::json GetTerraformingOptionsJson(uint32_t planet_id);
    nlohmann::json StartTerraformingJson(uint32_t planet_id, const std::string& target_class = "", int32_t link_index = -1);
    nlohmann::json CancelTerraformingJson(uint32_t planet_id);

    // Economy & Jobs Management (4.5.0 Cygnus Workforce Model)
    nlohmann::json GetPlanetJobCapacityJson(uint32_t planet_id);
    nlohmann::json GetPlanetJobsJson(uint32_t planet_id);
    // Probe: dump the colony job entries raw (field discovery).
    nlohmann::json ProbeColonyJobsRaw(uint32_t planet_id);
    nlohmann::json ScanColonyForInt(uint32_t planet_id, int64_t value, uint32_t radius);
    nlohmann::json DumpColonyMap(uint32_t planet_id);
    nlohmann::json ScanColonyForIntDeep(uint32_t planet_id, int64_t value);
    nlohmann::json FindPopGroupSlots(uint32_t planet_id, const std::vector<int64_t>& sizes, uint32_t radius);
    nlohmann::json DumpPopGroup(uint32_t group_id);
    nlohmann::json PlanetPointer(uint32_t planet_id);
    nlohmann::json SetJobPriorityJson(uint32_t planet_id, const std::string& job_key);
    nlohmann::json SetJobWorkforceLimitJson(uint32_t planet_id, const std::string& job_key, int32_t limit);

    // Armies Management (Planet Armies View)
    nlohmann::json GetPlanetArmiesJson(uint32_t planet_id, uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json SetPlanetArmySettingsJson(uint32_t planet_id, std::optional<bool> deploy_in_orbit, std::optional<bool> include_in_builder);
    nlohmann::json EmbarkAllArmiesJson(uint32_t planet_id);
    nlohmann::json DisbandArmyJson(uint32_t planet_id, uint32_t army_id);
    nlohmann::json RecruitArmyJson(uint32_t planet_id, const std::string& army_key, std::optional<uint32_t> species_id = std::nullopt,
                                     uint32_t colony_id = 0xFFFFFFFF);
    nlohmann::json ExtractArmiesSummary(void* planet_obj, void* colony_obj);

private:
    // Fills the CPdxArray<TPdxRef<CArmy>> at array_off of a command with engine-heap data.
    bool SetArmyRefs(NativeCommand& cmd, std::ptrdiff_t array_off, const std::vector<uint32_t>& ids, std::string* why);
    // Queues (dispatch=true) or only validates a copy of `buildable` via CAddBuildableToQueueCommand.
    bool QueueBuildable(const void* buildable, size_t size, uint32_t country_id, uint32_t queue_id,
                        bool dispatch, std::string* why);
    OutlinerManager() = default;
    uintptr_t base_address_{ 0 };

    std::string LocalizeDecisionKey(const std::string& key);
    std::string LocalizePlanetClass(const std::string& class_key);
    // engine-rendered names (CPersistentName) and the planet's system (coordinate origin)
    std::string PlanetName(void* planet);
    double ColonizationProgress(void* colony);  // 0..1 (CColony::CalcColonizationProgressPerc)
    static nlohmann::json SectorIdJson(const SectorGroup& s);
    struct DistrictSlots {
        uint32_t id{ 0xFFFFFFFF };
        std::string type_key;
        std::vector<uint32_t> zone_ids;  // one per zone slot
        void* obj{ nullptr };            // the CDistrict itself (for level and other fields)
        uint32_t level{ 0 };             // CDistrict::level (+0x1B0) — the save token "level"
    };
    std::vector<DistrictSlots> ColonyDistrictSlots(void* colony_obj);
    // 定时修正（CTimedModifierCollection）的读取：键名在 modifier 类型对象 +0x178 的 CPdxString 里
    std::string TimedModifierKey(void* type_obj);
    nlohmann::json ReadTimedModifiers(void* carrier, std::ptrdiff_t base);
    void FillZoneBuildable(uint8_t (&obj)[0x20], void* zone_type, uint32_t colony_id, uint32_t district_id, int32_t slot);
    uint32_t PlanetSystemId(void* planet);
    std::string SystemName(uint32_t system_id);

    using FnEngineAlloc = void* (*)(size_t size);
    using FnPostCommand = void (*)(void* cmd, bool flag);
    using FnConstructCmd = void* (*)(void* cmd, uint32_t country_id, void* planet_queue_obj, void* bldg_def);
    using FnConstructBuildableBuilding = void* (*)(void* this_ptr, uint32_t colony_id, uint32_t zone_id, void* bldg_def);
    using FnEnqueueCmd = void (*)(void* cmd_queue_mgr, void* cmd);

    FnEngineAlloc fn_engine_alloc_{ nullptr };
    FnPostCommand fn_post_command_{ nullptr };
    FnConstructCmd fn_construct_cmd_{ nullptr };
    FnConstructBuildableBuilding fn_construct_bldg_{ nullptr };
    FnEnqueueCmd fn_enqueue_cmd_{ nullptr };

    void* GetPlayerCountry();
    uint32_t GetPlayerCountryId();
    std::string LocalizeKey(const std::string& key);
public:
    // 公开的本地化入口（IPC 用）
    std::string Localize(const std::string& key) { return LocalizeKey(key); }
    // 把岗位句柄按 {index, generation} 拆开，读出岗位键
    nlohmann::json ResolvePopJob(uint32_t handle);
    // 手动切换殖民地设计
    nlohmann::json SetColonyDesignationJson(uint32_t planet_id, const std::string& key, uint64_t designation_ptr = 0);
    nlohmann::json ListColonyDesignationsJson(uint32_t planet_id);
    nlohmann::json ScanDesignationObjects(uint32_t planet_id, int budget_ms, const std::string& only_key);

    void* FindFleet(uint32_t fleet_id);
    void* FindColony(uint32_t colony_id);
    void* FindPlanet(uint32_t planet_id);
    void* FindSystem(uint32_t system_id);
    uint32_t GetPlanetQueueId(uint32_t planet_id);
    // The construction queue behind a colony, resolved through its carrier (planet +0xE4 / ark ship +0x104).
    uint32_t GetColonyQueueId(uint32_t colony_id);

    std::optional<ConstructionCard> ExtractColonyConstruction(void* colony_obj);
    // The colony alerts the game's outliner shows (status frames and crisis icons).
    std::vector<StatusAlertCard> ReadColonyStatus(void* colony_obj, void* planet_obj, uint32_t planet_id);
    struct ZoneRef {
        void* zone{ nullptr };
        uint32_t id{ 0xFFFFFFFF };
        std::string key, district_key;
        uint32_t buildings{ 0 };
        int max_buildings{ -1 };
    };
    std::vector<ZoneRef> ColonyZones(void* colony_obj);
    void FillBuildable(uint8_t (&obj)[0x20], void* building_type, uint32_t colony_id, uint32_t zone_id);
    void FillDistrictBuildable(uint8_t (&obj)[0x20], void* district_type, uint32_t colony_id);
    bool PlanetColonyQueue(uint32_t planet_id, uint32_t* cid, uint32_t* queue_id, std::string* error, uint32_t colony_id = 0xFFFFFFFF);
    std::vector<std::pair<std::string, int>> ColonyDistrictCounts(void* colony_obj);
    nlohmann::json QueueStarbaseModuleJson(uint32_t starbase_id, int32_t slot,
                                                 const std::string& key);
    nlohmann::json QueueStarbaseBuildingJson(uint32_t starbase_id, int32_t slot,
                                             const std::string& key);
    // 下一级能不能升（只校验，不下单）
    nlohmann::json GetStarbaseUpgradeJson(uint32_t starbase_id, bool dispatch = false);
    nlohmann::json BuildableCostJson(uint8_t (&obj)[0x20]);
    bool QueueCommandWithBuildable(NativeCommand& cmd, void* heap_buildable, uint32_t country_id, uint32_t queue_id,
                                   bool dispatch, std::string* why);
    // Armies stationed at a colony (its army list), read through bridge::armies.
    std::vector<armies::ArmyInfo> ReadColonyArmies(void* colony_obj);
    std::string SpeciesName(uint32_t species_id);
    nlohmann::json ExtractPlanetConstructionQueue(uint32_t planet_id);

    nlohmann::json ExtractPlanetaryFeatures(void* p_obj, uint32_t cid, uint32_t queue_id, uint32_t country_id);
    nlohmann::json ExtractMonthlyPopulationSummary(void* colony_obj);
    nlohmann::json ExtractPopulationBreakdown(void* colony_obj);
    nlohmann::json ExtractColonyAscension(void* colony_obj, uint32_t cid);
    nlohmann::json ExtractWorkforceSummary(void* colony_obj);

    void BuildSectorGroups(std::vector<SectorGroup>& out_sectors);
};

} // namespace bridge
