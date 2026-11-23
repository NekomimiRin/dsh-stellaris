#pragma once

#include "common.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

// The galaxy map as the player knows it. Every system and hyperlane is visible (as on the
// in-game map); everything else is gated by the player's own knowledge, asked from the engine:
//   * CCountry::GetIntelLevel(system): 0 none, 1 low, 2 medium, 3 high, 4 full
//   * surveyed planet: CCountry::HasAutoSurveyedSystem || CCountry::_HasSurveyedDepositHolder
// Owners need intel >= 1, planets intel >= 2 (or a survey), deposits a survey of that planet,
// foreign fleets intel >= 3. Own assets are always visible.
class GalaxyManager {
public:
    static GalaxyManager& Get();
    bool Init(uintptr_t base_address);

    // L1: counts and the player's position in the galaxy
    nlohmann::json GetOverviewJson();
    // L1.5: systems within `jumps` hyperlane jumps of `center_system` (default: the capital's),
    // as table rows plus the hyperlanes between them
    nlohmann::json GetMapJson(uint32_t center_system, int jumps);
    // L2: one system
    nlohmann::json GetSystemJson(uint32_t system_id);
    // 只读：全银河的 bypass（虫洞 / 星门），带「已探明 / 可用」
    nlohmann::json GetBypassesJson();
    // 只读：全银河舰队（含太空兽群 / 中立 / 海盗），type_filter 匹配 owner 的 country_type
    nlohmann::json GetFleetsAllJson(const std::string& type_filter, bool foreign_only, uint32_t limit);
    // 只读：本国舰队编制模板（不依赖舰队也能看；兽群设计会自动出现在条目里）
    nlohmann::json GetFleetTemplatesJson();
    // 只读：列全部巨型结构（可见性同单条读取）
    nlohmann::json GetMegastructuresJson();
    // 只读：电弧熔炉候选星系（按天体数排序；要求有熔融星球且还没电弧熔炉）
    // 只读：戴森云候选星系（单星、恒星产出折算能量币；1 文物=1 战略=2 科研=8 能量）
    nlohmann::json FindDysonSwarmSystems(uint32_t limit, uint32_t fleet_id, uint32_t from_system_id,
                                         bool sort_by_value);
    nlohmann::json FindArcFurnaceSystems(uint32_t min_bodies, uint32_t limit, uint32_t fleet_id,
                                        uint32_t from_system_id, bool sort_by_jumps);

    // 所有考古遗址（不分星系）：id / 类型名 / 章节 / 线索 / 难度 / 剩余天数 / locked / 谁在挖 / 可见性
    nlohmann::json GetArchaeologicalSitesJson();

    // 星界裂隙全库：id / 名字 / 类型 / status / 线索 / 所属 / 探索舰队与领袖
    nlohmann::json GetAstralRiftsJson();
    // 只读探针：一条 CAstralRift 的一段内存（找「所在星系」这类字段用）
    nlohmann::json ProbeAstralRiftJson(uint32_t rift_id, uint32_t offset, uint32_t length);
    // 只读探针：一个星系对象的一段内存（核对 astral_rifts 数组用）
    nlohmann::json ProbeSystemJson(uint32_t system_id, uint32_t offset, uint32_t length);
    // 扫全银河里 astral_rifts 非空的星系（看那个 vector 的元素布局）
    nlohmann::json ScanRiftSystemsJson();
    // 扫全银河里的「星界细缝」天体（pc_astral_scar）：裂隙真正占星系的应当是这个天体
    nlohmann::json ScanAstralScarJson();
    // 战争表：id / 名字 / 双方疲惫 / 战争目标 / 是否结束
    nlohmann::json GetWarsJson();
    // 玩家自己的国家：{ country_id, raw_ref, name }
    nlohmann::json GetPlayerCountryJson();

    // 只读：某个遗址的事件表（阶段弹窗就是里面未过期的那条）
    nlohmann::json ProbeSiteEventsJson(uint32_t site_id);
    // 只读：在可写内存里找存着某 8 字节值（通常是对象指针）的位置——反查持有者
    nlohmann::json ScanMemoryForQword(uint64_t value, int max_seconds, uint64_t start_addr, uint64_t max_bytes);

    // L3: fleet orders
    nlohmann::json MoveFleet(uint32_t fleet_id, uint32_t system_id, bool queue);
    nlohmann::json MergeFleetsJson(const std::vector<uint32_t>& fleet_ids);
    nlohmann::json DowngradeStarbaseJson(uint32_t starbase_id);
    nlohmann::json DismantleStarbaseJson(uint32_t starbase_id);
    nlohmann::json DisbandFleetsJson(const std::vector<uint32_t>& fleet_ids);
    // planet_id != 0xFFFFFFFF surveys one planet, otherwise the whole system
    nlohmann::json Survey(uint32_t fleet_id, uint32_t system_id, uint32_t planet_id, bool queue);
    // a construction ship builds an outpost (starbase) in the system
    nlohmann::json BuildOutpost(uint32_t fleet_id, uint32_t system_id, bool queue);
    // a construction ship builds an orbital station at a planet (its deposit decides what the
    // station yields): station "mining", "research" or "observation"; empty: mining, else research
    nlohmann::json BuildStation(uint32_t fleet_id, uint32_t planet_id, const std::string& station, bool queue);
    // a colony ship colonizes the planet
    nlohmann::json Colonize(uint32_t fleet_id, uint32_t planet_id, bool queue);
    // cancel every order of these fleets (they stop where they are)
    nlohmann::json CancelFleetOrders(const std::vector<uint32_t>& fleet_ids);
    // follow another fleet (attack: engage it when caught up)
    nlohmann::json FollowFleet(uint32_t fleet_id, uint32_t target_fleet_id, bool attack, bool queue);
    // passive / aggressive / evasive
    nlohmann::json SetFleetStance(uint32_t fleet_id, const std::string& stance);
    // activate / deactivate fleet cloaking (CActivateFleetCloakingCommand / CDeactivateFleetCloakingCommand)
    nlohmann::json SetCloaking(uint32_t fleet_id, bool active, bool dry_run);
    // Read-only: the fleet's flag word(s) at TFleetFlags (default +0x1268), plus its object pointer.
    nlohmann::json ProbeFleetFlags(uint32_t fleet_id, uint32_t offset, uint32_t length);
    // Read-only: every colony of the player country with its carrier resolved (planet name or ark
    // ship name) plus the CColony counters — works for planets and Nomads arks alike.
    nlohmann::json GetColoniesJson();
    // Read-only: one colony, planet-carried or ark(ship)-carried. Same CColony either way; only the
    // carrier differs (CPlanet::colony +0xE0 vs CShip::colony +0x100). Returns carrier info + a u32 window.
    nlohmann::json ProbeColony(uint32_t colony_id, uint32_t offset, uint32_t length);
    // Read-only: the construction queue behind a colony, resolved through its carrier
    // (CPlanet::build_queue +0xE4 / CShip::build_queue +0x104 — both are CColonyCarrier fields).
    nlohmann::json ProbeBuildQueue(uint32_t colony_id);
    // 模块基址（把探测到的运行时地址换算成 RVA 用）
    nlohmann::json GetModuleBase();
    // Read-only: the fleet's home base (CFleetTemplate::home_base) resolved to the starbase fleet.
    nlohmann::json GetFleetHomeBase(uint32_t fleet_id);
    // Read-only probe: every CStarbase in its database, with its id and object pointer.
    nlohmann::json GetStarbasePointers();
    // Read-only: one starbase — level, module slots and building slots (key + localised name each).
    nlohmann::json GetStarbaseDetails(uint32_t starbase_id);
    nlohmann::json GetStarbaseOverviewJson();
    // Read-only: our own starbases in three groups — arks (colony carriers), waystations (nomad networks)
    // and ordinary starbases (system owner). A nomad owns no systems, so the ordinary test alone returns 0.
    nlohmann::json GetOurStarbasesJson();
    // Hook NPlanetJobs::UpdatePopJobs and capture its arguments.
    nlohmann::json HookJobUpdate(bool enable);
    nlohmann::json ReadJobHook();
    // Hook the per-colony pop-job update and scan the colony at that moment.
    nlohmann::json HookColonyUpdate(bool enable);
    nlohmann::json ReadColonyHook();
    nlohmann::json ScanMemoryForU64Range(uint64_t lo, uint64_t hi, int max_seconds,
                                        uint64_t start_addr, uint64_t max_bytes);
    nlohmann::json HookPopGroupSize(bool enable);
    nlohmann::json ReadPopGroupSizeHook();
    // Read-only: dump a memory range as printable ASCII.
    nlohmann::json ReadMemoryAt(uint64_t address, uint32_t length);
    // Bounded read-only scan of committed memory for a text pattern (utf8/16/32).
    nlohmann::json ScanEdictArraysJson(int max_seconds);
    nlohmann::json ProbeColonyCarriersJson();
    // Read-only probe: where do researched technologies live inside the player country? Dumps
    // [country+offset, +length) and reports every qword that is a CTechnology of the database
    // (plus u32 hits on the technology ids, in case the set stores refs rather than pointers).
    nlohmann::json ProbeTechStatusJson(uint32_t offset, uint32_t length, bool dump_hex);
    nlohmann::json ScanMemoryForText(const std::string& needle, bool include_code, int max_seconds, uint64_t start_addr, uint64_t max_bytes);
    // Dump the console slot: follow the pointer, show printable text.
    nlohmann::json ProbeConsoleSlot(uint32_t slot_offset, uint32_t length);
    // Read-only probe: where does the console keep its printed output?
    nlohmann::json ScanConsoleForText(const std::string& needle);
    // Write CShip+0x98C (upgrade target) for every ship in a fleet, so upgrade_fleet can retrofit.
    nlohmann::json SetShipUpgradeDesign(uint32_t fleet_id, uint32_t to_design);
    // Point a fleet template entry at another design (then upgrade_fleet retrofits).
    nlohmann::json SetFleetTemplateDesign(uint32_t fleet_id, uint32_t from_design, uint32_t to_design);
    // Scan a ship's first 0x2000 bytes for given u32 values (design-id hunting).
    nlohmann::json ScanShipForValues(uint32_t ship_id, std::vector<uint32_t> values);
    // Read-only probe: the fleet's real ship list and candidate design-id offsets.
    nlohmann::json ProbeFleetShips(uint32_t fleet_id);
    // Post CSetHomeBaseCommand: move a fleet's home base to a CStarbase by id.
    nlohmann::json SetFleetHomeBase(uint32_t fleet_id, uint32_t starbase_id);
    // return_home / emergency_ftl (the fleets go missing in action)
    nlohmann::json FleetMia(const std::vector<uint32_t>& fleet_ids, const std::string& type);
    // add (count claims, default 1) or remove (count, default all) the player's claims on a system
    nlohmann::json ClaimSystem(uint32_t system_id, bool remove, int count);

    // the system an ESpatialObjectType reference (planet, system, debris, megastructure, natural
    // wormhole, astral rift, fleet) is in; 0xFFFFFFFF when unknown
    uint32_t SpatialSystem(uint32_t type, uint32_t id);
    // a science ship collects data for a special project (debris analysis and other located
    // projects): by project id, or every such project of the player in system_id
    nlohmann::json CollectData(uint32_t fleet_id, uint32_t project_id, uint32_t system_id, bool queue);

    // invade: an army fleet lands its armies on the planet's colony
    nlohmann::json LandArmies(uint32_t fleet_id, uint32_t planet_id, bool queue);

    // in-system orders
    nlohmann::json OrbitPlanet(uint32_t fleet_id, uint32_t planet_id, bool queue);
    nlohmann::json ResearchAnomalies(uint32_t fleet_id, uint32_t system_id, bool queue);
    nlohmann::json ExcavateSite(uint32_t fleet_id, uint32_t site_id, bool queue, bool dry_run = false);
    // read-only: ask the engine's own IsValid for every site, per fleet (no posting)
    nlohmann::json ProbeExcavatability(uint32_t fleet_id);
    // astral rifts: post an exploration order, or ask the engine per rift (IsValid only)
    nlohmann::json ExploreAstralRift(uint32_t fleet_id, uint32_t rift_id, bool queue, bool dry_run = false);
    nlohmann::json ProbeRiftExplorability(uint32_t fleet_id);
    // 按天体研究异常（CResearchAnomalyFleetOrderCommand：fleet + planet + queue）
    nlohmann::json ResearchAnomaly(uint32_t fleet_id, uint32_t planet_id, bool queue);
    // 只读：以 planet_id 为路标，从舰队对象出发找 +0x30 == planet_id 的对象（定位指令容器）
    nlohmann::json ScanFleetOrdersJson(uint32_t fleet_id, uint32_t planet_id, int depth);
    // 只读：读舰队指令数组（+0x2D8 data / +0x2E4 size），每条给 vtable/order_id/planet
    nlohmann::json ProbeFleetOrdersJson(uint32_t fleet_id);
    // jump through a bypass (gateway / wormhole / relay / L-gate) to to_system (one of its leads_to)
    nlohmann::json UseBypass(uint32_t fleet_id, uint32_t bypass_id, uint32_t to_system, bool queue);
    nlohmann::json ExploreBypass(uint32_t fleet_id, uint32_t bypass_id, bool queue);

    // the route the game plans for this fleet from where it is (CFleetPath::Create: closed
    // borders, gateways, wormholes and FTL as the fleet may use them) with its
    // CalcEstimatedDays travel time, the ETA the fleet gets when ordered there
    nlohmann::json FindPath(uint32_t fleet_id, uint32_t to_system);
    // megastructures: one in detail (with the upgrades it offers), upgrading, a construction
    // ship's build menu and building
    nlohmann::json GetMegastructureJson(uint32_t megastructure_id);
    nlohmann::json UpgradeMegastructure(uint32_t megastructure_id, const std::string& type_key);
    nlohmann::json GetBuildableMegastructures(uint32_t fleet_id, uint32_t system_id);
    nlohmann::json BuildMegastructure(uint32_t fleet_id, const std::string& type_key, uint32_t planet_id, uint32_t system_id,
                                      uint32_t toward_system_id, bool queue);
    // systems for a purpose ("unsurveyed", "outpost", "deposit"), nearest first;
    // name non-empty switches to a case-insensitive substring search over every system name
    nlohmann::json FindSystems(const std::string& purpose, uint32_t from_system, int limit, uint32_t fleet_id,
                               const std::string& resource, uint32_t species_id = 0xFFFFFFFF,
                               const std::string& name = std::string());

private:
    GalaxyManager() = default;

    struct System {
        uint32_t id;
        void* obj;
        double x, y;
        std::vector<std::pair<uint32_t, double>> lanes;  // (to, length)
    };
    struct PlanetRef {
        uint32_t id;
        void* obj;
    };

    // One snapshot per request: systems, their planets, the player
    struct Snapshot {
        void* player = nullptr;
        uint32_t player_id = 0xFFFFFFFF;
        std::vector<System> systems;
        std::unordered_map<uint32_t, size_t> index;                  // system id -> systems[]
        std::unordered_map<uint32_t, std::vector<PlanetRef>> planets;  // system id -> planets
    };
    Snapshot Take();

    int Intel(const Snapshot& s, void* system);
    bool PlanetSurveyed(const Snapshot& s, void* system, void* planet);
    bool SystemSurveyed(const Snapshot& s, uint32_t system_id, void* system);
    uint32_t Owner(void* system);
    std::string SystemName(uint32_t id, void* system);
    uint32_t CapitalSystem(const Snapshot& s);
    std::vector<uint32_t> StarbaseIds(void* system);
    std::vector<uint32_t> FleetIds(void* system);
    std::string CountryName(uint32_t country_id);
    std::ptrdiff_t LevelShipSizeField();  // -1 when not found
    std::unordered_map<uint32_t, int> JumpsFrom(const Snapshot& s, uint32_t from);
    bool OwnFleet(const Snapshot& s, uint32_t fleet_id);
    // build_orbital_station_order for an outpost; IsValid only unless post
    bool OutpostCommand(uint32_t fleet_id, uint32_t system_id, bool queue, bool post, std::string* why);
    bool StationCommand(uint32_t fleet_id, uint32_t planet_id, uint8_t ship_class, bool queue, bool post, std::string* why);
    bool SurveyCommand(uint32_t fleet_id, uint32_t system_id, uint32_t planet_id, bool queue, bool post, std::string* why);

    uintptr_t base_address_{ 0 };
    void* names_db_{ nullptr };  // the system database the name cache belongs to
    std::unordered_map<uint32_t, std::string> names_;
    std::ptrdiff_t level_ship_size_{ -2 };  // -2: not looked up yet
};

}  // namespace bridge


