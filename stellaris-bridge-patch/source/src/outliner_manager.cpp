#include "outliner_manager.hpp"
#include <set>
#include "command_builder.hpp"
#include "game_state.hpp"
#include "government_manager.hpp"
#include "fleet_access.hpp"
#include "army_access.hpp"
#include "sdk/stellaris_sdk.hpp"
#include "leader_manager.hpp"
#include "fleet_manager.hpp"
#include "species_manager.hpp"
#include "commands.hpp"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace bridge {

// Buildables queued through CAddBuildableToQueueCommand. The SDK sees only their CSerializer
// helpers (which confirm the field layouts), not the buildable classes' own vtables, so these
// stay hand-maintained (4.5.1):
//   CBuildableBuilding             new-building items in live construction queues
//   CBuildableUpgradeBuilding      only buildable vtable using BUILD_QUEUE_SUFFIX_UPGRADE
//   CBuildableClearDepositBlocker  live queue items {deposit, colony}; CLEAR_BLOCKER_* strings
//   CBuildableArmy                 (vtables: sdk::vt, located by tools/sdk_dumper/anchors.py)
// Every CBuildable vtable shares the base implementation in slots 2 and 3; QueueBuildable
// checks that before handing an object to the engine (a wrong pointer here once crashed the
// game with a pure virtual call).
constexpr uintptr_t kBuildableBuildingVt = sdk::vt::CBuildableBuilding;
constexpr uintptr_t kBuildableUpgradeBuildingVt = sdk::vt::CBuildableUpgradeBuilding;
constexpr uintptr_t kBuildableClearDepositBlockerVt = sdk::vt::CBuildableClearDepositBlocker;
constexpr uintptr_t kBuildableArmyVt = sdk::vt::CBuildableArmy;
constexpr uintptr_t kBuildableDistrictVt = sdk::vt::CBuildableDistrict;
constexpr uintptr_t kDistrictTypeDb = sdk::glob::TGameDatabase_CDistrictTypeDatabase_pInstance;  // +0x50 items, +0x5C count
constexpr uintptr_t kBuildingTypeDb = sdk::glob::TGameDatabase_CBuildingTypeDatabase_pInstance;  // +0x50 items, +0x5C count

// Raw Clausewitz String Layout
struct RawPdxString {
    union {
        char buf[16];
        char* heap_ptr;
    };
    uint64_t size;
    uint64_t capacity;
};

static bool SafeReadPtr(const void* addr, void** out) {
    if (!addr || !out) return false;
    __try {
        *out = *(void**)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU32(const void* addr, uint32_t* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const uint32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadI32(const void* addr, int32_t* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const int32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU8(const void* addr, uint8_t* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const uint8_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadFloat(const void* addr, float* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const float*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeConstructCmd(void* fn_construct_ptr, void* cmd, uint32_t country_id, void* owner_obj, void* bldg_def) {
    if (!fn_construct_ptr || !cmd || !owner_obj || !bldg_def) return false;
    __try {
        typedef void (*FnConstructCmdSig)(void*, uint32_t, void*, void*);
        ((FnConstructCmdSig)fn_construct_ptr)(cmd, country_id, owner_obj, bldg_def);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeConstructBuildableBuilding(void* fn_ptr, void* action_obj, uint32_t colony_id, uint32_t zone_id, void* bldg_def) {
    if (!fn_ptr || !action_obj || !bldg_def) return false;
    __try {
        typedef void* (*FnSig)(void*, uint32_t, uint32_t, void*);
        ((FnSig)fn_ptr)(action_obj, colony_id, zone_id, bldg_def);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU64(const void* addr, uint64_t* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const uint64_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadI64(const void* addr, int64_t* out) {
    if (!addr || !out) return false;
    __try {
        *out = *(const int64_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCopyChars(char* dst, const char* src, size_t count) {
    if (!dst || !src || count == 0) return false;
    __try {
        memcpy(dst, src, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadPdxString(const void* str_addr, std::string& out_str) {
    out_str.clear();
    if (!str_addr) return false;

    RawPdxString raw{};
    if (!SafeCopyChars((char*)&raw, (const char*)str_addr, sizeof(RawPdxString))) {
        return false;
    }

    if (raw.size == 0 || raw.size > 4096) {
        return true;
    }

    if (raw.capacity < 16) {
        size_t len = raw.size < 16 ? (size_t)raw.size : 15;
        char temp[16]{ 0 };
        if (SafeCopyChars(temp, raw.buf, len)) {
            temp[len] = '\0';
            out_str = std::string(temp, len);
            return true;
        }
    } else if (raw.heap_ptr) {
        uintptr_t addr = (uintptr_t)raw.heap_ptr;
        if (addr > 0x10000 && addr < 0x7FFFFFFFFFFF) {
            size_t len = raw.size < 1024 ? (size_t)raw.size : 1024;
            std::string result(len, '\0');
            if (SafeCopyChars(&result[0], raw.heap_ptr, len)) {
                out_str = result;
                return true;
            }
        }
    }
    return false;
}



namespace {
constexpr int kBuildableSlotCalcCost = sdk::vt::CBuildableBase_CalcCost;
constexpr int kBuildableSlotTimeNeeded = sdk::vt::CBuildableBase_CalcProgressionTimeNeeded;

// CFixedResourceTable: +0 points at the {data, capacity, size} holder at +8, +0x18 allocator.
// Pre-sized to every resource, so the engine only writes into it and never reallocates.
struct StackResourceTable {
    void* holder;
    int64_t* data;
    int32_t capacity;
    int32_t size;
    void* allocator;
    int64_t values[256];
};

struct BuildableCostCtx {
    void* buildable;
    StackResourceTable* cost;
    StackResourceTable* other;
    int64_t days;
};

void CallBuildableCost(void* c, void*) {
    auto* x = (BuildableCostCtx*)c;
    void** vt = *(void***)x->buildable;
    ((void (*)(void*, void*, void*, void*))vt[kBuildableSlotCalcCost])(x->buildable, x->cost, x->other, nullptr);
    ((int64_t* (*)(void*, int64_t*, void*))vt[kBuildableSlotTimeNeeded])(x->buildable, &x->days, nullptr);
}

struct MaxBuildingsCtx {
    uintptr_t fn;
    void* colony;
    void* zone;
    void* country;
    int result;
};

void CallMaxBuildings(void* c, void*) {
    auto* x = (MaxBuildingsCtx*)c;
    x->result = ((int (*)(const void*, const void*, int, const void*))x->fn)(x->colony, x->zone, 0, x->country);
}
}  // namespace

/* ---- 方舟（arkship）/ 驿站 的公共小工具（2026-11-23）----
   方舟的等级键是 starbase_level_{civilian,science,military}_arkship_{1,2,3}，
   驿站是 starbase_level_waystation_{1,2,3}（两个都住在同一个 CStarbase 表里）。
   方舟只有一条队列，挂在载体（方舟舰）的 CShip+0x104 上——与殖民地、陆军共用；
   CStarbase 自己的 +0x880 读 0、+0x884 读 0xFFFFFFFF，照那个投模块会投进别的队列。 */
static std::string StarbaseLevelKey(uintptr_t base_address, void* starbase) {
    std::string key;
    void* level_type = nullptr;
    if (!starbase || !SafeReadPtr((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::level), &level_type) ||
        !level_type) {
        return key;
    }
    SafeReadPdxString((const void*)((uintptr_t)level_type + 0x20), key);
    return key;
}

static bool IsArkshipLevel(const std::string& level_key) {
    return level_key.find("arkship") != std::string::npos;
}

// 方舟唯一的那条队列；不是方舟、或读不到时回 0xFFFFFFFF
static uint32_t ArkBuildQueue(uintptr_t base_address, void* starbase) {
    if (!base_address || !starbase) return 0xFFFFFFFF;
    if (!IsArkshipLevel(StarbaseLevelKey(base_address, starbase))) return 0xFFFFFFFF;
    uint32_t station = 0xFFFFFFFF;
    if (!SafeReadU32((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::station), &station) ||
        station == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    void* ship_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address + sdk::db::CShip), &ship_mgr) || !ship_mgr) return 0xFFFFFFFF;
    void* ship_arr = nullptr;
    uint32_t ship_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)ship_mgr + 0x18), &ship_arr);
    SafeReadU32((const void*)((uintptr_t)ship_mgr + 0x20), &ship_cap);
    const uint32_t slot = station & 0xFFFFFF;
    if (!ship_arr || slot >= ship_cap) return 0xFFFFFFFF;
    void* ship = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)ship_arr + slot * 16 + 8), &ship) || !ship) return 0xFFFFFFFF;
    uint32_t queue = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)ship + 0x104), &queue);
    return queue;
}

// 模块 / 建筑该投哪条队列：方舟 → 载体队列；其余 → CStarbase::build_queue(+0x880)
static uint32_t BuildQueueForStarbase(uintptr_t base_address, void* starbase, uint32_t* out_ark_queue) {
    const uint32_t ark = ArkBuildQueue(base_address, starbase);
    if (out_ark_queue) *out_ark_queue = ark;
    if (ark != 0xFFFFFFFF) return ark;
    uint32_t q = 0xFFFFFFFF;
    if (starbase) SafeReadU32((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::build_queue), &q);
    return q;
}

/* ---- 轰炸姿态（内部名 ground support stance）----
   档位对象在 TGameDatabase_CBombardmentStanceDatabase，键名在对象 +0x20；舰队上存的是指向它的指针。
   2026-11-23 实测（玩家在界面上分别设了 selective / indiscriminate，两份舰队内存一比）：
   **字段在 CFleet+0x458**（SDK 标的 0x968 在这一版不对）：selective → 0x…319350、indiscriminate → 0x…31A370。
   界面投的是 CSetFleetGroundSupportStanceCommand（vtable 0x23C0320，fleet +0x20、stance +0x28）。 */
static std::vector<std::pair<std::string, void*>> BombardmentStances(uintptr_t base_address) {
    std::vector<std::pair<std::string, void*>> out;
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address + sdk::glob::TGameDatabase_CBombardmentStanceDatabase_pInstance),
                     &db) || !db) {
        return out;
    }
    void* arr = nullptr;
    uint32_t n = 0;
    SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr);
    SafeReadU32((const void*)((uintptr_t)db + 0x5C), &n);
    for (uint32_t i = 0; i < n && arr && i < 64; ++i) {
        void* e = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &e) || !e) continue;
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)e + 0x20), key);
        if (!key.empty()) out.emplace_back(key, e);
    }
    return out;
}

static std::string BombardmentStanceOf(uintptr_t base_address, void* fleet) {
    void* p = nullptr;
    if (!fleet || !SafeReadPtr((const void*)((uintptr_t)fleet + 0x458), &p) || !p) return std::string();
    for (const auto& kv : BombardmentStances(base_address)) {
        if (kv.second == p) return kv.first;
    }
    return std::string();
}

static void* FindDbElementByKey(uintptr_t base_address, uintptr_t db_rva, const std::string& target_key) {
    if (!base_address) return nullptr;
    void* db_ptr = nullptr;
    if (!SafeReadPtr((const void*)(base_address + db_rva), &db_ptr) || !db_ptr) return nullptr;
    uint32_t count = 0;
    void* arr = nullptr;
    if (!SafeReadU32((const void*)((uintptr_t)db_ptr + 0x5C), &count) || count == 0 ||
        !SafeReadPtr((const void*)((uintptr_t)db_ptr + 0x50), &arr) || !arr) return nullptr;

    for (uint32_t i = 0; i < count; ++i) {
        void* elem = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &elem) || !elem) continue;
        std::string elem_key;
        if (SafeReadPdxString((const void*)((uintptr_t)elem + 0x20), elem_key)) {
            if (elem_key == target_key) {
                return elem;
            }
        }
    }
    return nullptr;
}

OutlinerManager& OutlinerManager::Get() {
    static OutlinerManager instance;
    return instance;
}

bool OutlinerManager::Init(uintptr_t base_address) {
    base_address_ = base_address;
    if (!base_address_) return false;

    fn_engine_alloc_ = (FnEngineAlloc)(base_address_ + sdk::kRvaEngineAlloc);
    fn_post_command_ = Commands::Get().GetPostCommand();  // null when the SDK does not match the exe
    fn_construct_cmd_ = nullptr;
    fn_construct_bldg_ = nullptr;
    fn_enqueue_cmd_ = nullptr;
    return true;
}

uint32_t OutlinerManager::GetPlayerCountryId() {
    void* country = GetPlayerCountry();
    if (!country) return 0;
    uint32_t cid = 0;
    SafeReadU32((const void*)((uintptr_t)country + 0x20), &cid);
    return cid;
}

uint32_t OutlinerManager::GetPlanetQueueId(uint32_t planet_id) {
    if (!base_address_) return 0xFFFFFFFF;
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return 0xFFFFFFFF;

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return 0xFFFFFFFF;

    uint32_t queue_id = 0xFFFFFFFF;
    if (SafeReadU32((const void*)((uintptr_t)p_obj + 0xe4), &queue_id)) {
        return queue_id;
    }
    return 0xFFFFFFFF;
}

uint32_t OutlinerManager::GetColonyQueueId(uint32_t colony_id) {
    if (!base_address_) return 0xFFFFFFFF;
    void* colony = FindColony(colony_id);
    if (!colony) return 0xFFFFFFFF;
    uint64_t carrier = 0;
    SafeReadI64((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), (int64_t*)&carrier);
    uint32_t id = (uint32_t)(carrier & 0xFFFFFFull);
    uint32_t type = (uint32_t)(carrier >> 32);
    uint32_t q = 0xFFFFFFFF;
    if (type == 0) {
        void* p = FindPlanet(id);
        if (p) SafeReadU32((const void*)((uintptr_t)p + sdk::ent::CPlanet::build_queue), &q);
        return q;
    }
    // ark: the queue hangs off the ship (CColonyCarrier::build_queue = ship +0x104)
    void* mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CShip), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || (id & 0xFFFFFF) >= cap) {
        return 0xFFFFFFFF;
    }
    void* ship = nullptr;
    uint32_t check = 0xFFFFFFFF;
    if (!SafeReadPtr((const void*)((uintptr_t)arr + (id & 0xFFFFFF) * 16 + 8), &ship) || !ship ||
        !SafeReadU32((const void*)((uintptr_t)ship + 0x18), &check) || check != id) {   // CShip id 在 +0x18（+8 恒 0）
        return 0xFFFFFFFF;
    }
    SafeReadU32((const void*)((uintptr_t)ship + sdk::ent::CShip::build_queue), &q);
    return q;
}

void* OutlinerManager::GetPlayerCountry() {
    return GameState::Get().GetPlayerCountry();  // the local player's country (not country 0)
}

std::string OutlinerManager::LocalizeKey(const std::string& key) {
    return key.empty() ? key : SafeLocalize(base_address_, key);
}

void* OutlinerManager::FindFleet(uint32_t fleet_id) {
    return fleets::Find(base_address_, fleet_id);
}

void* OutlinerManager::FindColony(uint32_t colony_id) {
    if (!base_address_) return nullptr;

    void* colony_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CColony), &colony_mgr) || !colony_mgr || (uintptr_t)colony_mgr < 0x10000) { return nullptr; }
    if (!colony_mgr || (uintptr_t)colony_mgr < 0x10000) {
        return nullptr;
    }

    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)colony_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)colony_mgr + 0x20), &cap) || cap == 0) {
        return nullptr;
    }

    uint32_t slot = colony_id & 0xFFFFFF;
    if (slot < cap) {
        void* ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &ptr) && ptr) {
            return ptr;
        }
    }
    return nullptr;
}

void* OutlinerManager::FindPlanet(uint32_t planet_id) {
    if (!base_address_ || planet_id == 0xFFFFFFFF) return nullptr;
    void* planet_mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CPlanet), &planet_mgr) || !planet_mgr ||
        !SafeReadPtr((const void*)((uintptr_t)planet_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)planet_mgr + 0x20), &cap)) {
        return nullptr;
    }
    uint32_t slot = planet_id & 0xFFFFFF;
    void* ptr = nullptr;
    if (slot >= cap || !SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &ptr) || !ptr) return nullptr;
    // TPdxRef<CPlanet> lookups compare the planet's own id (+0x18), generation included
    uint32_t pid = 0xFFFFFFFF;
    return SafeReadU32((const void*)((uintptr_t)ptr + 0x18), &pid) && pid == planet_id ? ptr : nullptr;
}

void* OutlinerManager::FindSystem(uint32_t system_id) {
    if (!base_address_) return nullptr;

    void* sys_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CGalacticObject), &sys_mgr) || !sys_mgr || (uintptr_t)sys_mgr < 0x10000) { return nullptr; }
    if (!sys_mgr || (uintptr_t)sys_mgr < 0x10000) {
        return nullptr;
    }

    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)sys_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)sys_mgr + 0x20), &cap) || cap == 0) {
        return nullptr;
    }

    uint32_t slot = system_id & 0xFFFFFF;
    if (slot < cap) {
        void* ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &ptr) && ptr) {
            uint32_t check_id = 0;
            if (SafeReadU32((const void*)((uintptr_t)ptr + 8), &check_id) && check_id == system_id) {
                return ptr;
            }
        }
    }
    return nullptr;
}

std::optional<ConstructionCard> OutlinerManager::ExtractColonyConstruction(void* colony_obj) {
    if (!colony_obj || !base_address_) return std::nullopt;

    uint64_t f_f78 = 0;
    if (!SafeReadU64((const void*)((uintptr_t)colony_obj + 0xf78), &f_f78)) {
        return std::nullopt;
    }
    uint32_t slot = (uint32_t)(f_f78 & 0xFFFFFFFF);

    void* mgr_3113128 = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CPlanet), &mgr_3113128) || !mgr_3113128 || (uintptr_t)mgr_3113128 < 0x10000) { return std::nullopt; }
    if (!mgr_3113128 || (uintptr_t)mgr_3113128 < 0x10000) {
        return std::nullopt;
    }
    void* arr_3113128 = nullptr;
    uint32_t cap_3113128 = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)mgr_3113128 + 0x18), &arr_3113128) || !arr_3113128 ||
        !SafeReadU32((const void*)((uintptr_t)mgr_3113128 + 0x20), &cap_3113128) || slot >= cap_3113128) {
        return std::nullopt;
    }
    void* slot_obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr_3113128 + slot * 16 + 8), &slot_obj) || !slot_obj) {
        return std::nullopt;
    }

    uint32_t queue_id = 0;
    if (!SafeReadU32((const void*)((uintptr_t)slot_obj + 0x20 + 0xc4), &queue_id)) {
        return std::nullopt;
    }

    void* mgr_eb8 = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8) || !mgr_eb8 || (uintptr_t)mgr_eb8 < 0x10000) {
        SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8);
    }
    if (!mgr_eb8 || (uintptr_t)mgr_eb8 < 0x10000) {
        return std::nullopt;
    }
    void* arr_eb8 = nullptr;
    uint32_t cap_eb8 = 0;
    uint32_t q_slot = queue_id & 0xFFFFFF;
    if (!SafeReadPtr((const void*)((uintptr_t)mgr_eb8 + 0x18), &arr_eb8) || !arr_eb8 ||
        !SafeReadU32((const void*)((uintptr_t)mgr_eb8 + 0x20), &cap_eb8) || q_slot >= cap_eb8) {
        return std::nullopt;
    }
    void* queue_obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr_eb8 + q_slot * 16 + 8), &queue_obj) || !queue_obj) {
        return std::nullopt;
    }

    uint32_t q_cnt = 0;
    if (!SafeReadU32((const void*)((uintptr_t)queue_obj + 0x2c), &q_cnt) || q_cnt == 0) {
        return std::nullopt;
    }
    void* q_items = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)queue_obj + 0x20), &q_items) || !q_items) {
        return std::nullopt;
    }
    uint32_t item_id = 0;
    if (!SafeReadU32((const void*)q_items, &item_id)) {
        return std::nullopt;
    }

    void* mgr_ea8 = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8) || !mgr_ea8 || (uintptr_t)mgr_ea8 < 0x10000) {
        SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8);
    }
    if (!mgr_ea8 || (uintptr_t)mgr_ea8 < 0x10000) {
        return std::nullopt;
    }
    void* arr_ea8 = nullptr;
    uint32_t cap_ea8 = 0;
    uint32_t i_slot = item_id & 0xFFFFFF;
    if (!SafeReadPtr((const void*)((uintptr_t)mgr_ea8 + 0x18), &arr_ea8) || !arr_ea8 ||
        !SafeReadU32((const void*)((uintptr_t)mgr_ea8 + 0x20), &cap_ea8) || i_slot >= cap_ea8) {
        return std::nullopt;
    }
    void* item_obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr_ea8 + i_slot * 16 + 8), &item_obj) || !item_obj) {
        return std::nullopt;
    }

    uint32_t prog = 0;
    uint32_t tot = 0;
    SafeReadU32((const void*)((uintptr_t)item_obj + 0x28), &prog);
    SafeReadU32((const void*)((uintptr_t)item_obj + 0x30), &tot);

    void* action_obj = nullptr;
    SafeReadPtr((const void*)((uintptr_t)item_obj + 0x18), &action_obj);
    std::string key;
    if (action_obj) {
        void* def_obj = nullptr;
        SafeReadPtr((const void*)((uintptr_t)action_obj + 8), &def_obj);
        if (def_obj) {
            SafeReadPdxString((const void*)((uintptr_t)def_obj + 0x20), key);
        }
    }

    ConstructionCard card{};
    card.key = key;
    if (key.rfind("district_", 0) == 0) {
        card.type = "district";
    } else if (key.rfind("building_", 0) == 0) {
        card.type = "building";
    } else {
        card.type = "construction";
    }

    card.name = LocalizeKey(key);

    if (tot > 0) {
        card.progress = std::round(((double)prog / (double)tot) * 100.0) / 100.0;
        card.remaining_days = (tot > prog) ? (int32_t)((tot - prog) / 100000) : 0;
    }

    return card;
}

// ---- colony status alerts -------------------------------------------------------------------
// The alerts the game's outliner shows for a colony, from the engine's own decisions:
//  * status frames from COutlinerPlanetStatusController::ShouldShowStatusFrame(frame, colony,
//    country, owner type). Only frame 1 (clearable blocker) uses the controller (a growth-data
//    cache), so frames 0/2/5/6/7/10 are asked with a dummy controller and frame 1 comes from the
//    planet's clearable, not yet queued blockers.
//  * crisis icons of COutlinerMemberPlanet::UpdateEntry: blockaded = the carrier is being
//    bombarded (CPlanet::IsBeingBombarded(false): a real ground support stance at
//    CPlanet::ground_support_stance), occupied = controller differs from owner.
namespace {

struct StatusFrameCtx {
    uintptr_t fn;
    void* controller;
    int frame;
    void* colony;
    uint32_t country;
    bool result;
};

void CallShouldShowStatusFrame(void* c, void*) {
    auto* x = (StatusFrameCtx*)c;
    x->result = ((bool (*)(void*, int, void*, uint32_t, int))x->fn)(x->controller, x->frame, x->colony, x->country, 0);
}

// NDefines::NGameplay::LOW_PLANET_STABILITY, read through the frame-10 case of
// ShouldShowStatusFrame: `mov rax, [rip + d]` followed by `cmp [rdi + stability], rax`.
double LowPlanetStabilityDefine(uintptr_t fn) {
    const uint8_t* p = (const uint8_t*)fn;
    for (int i = 0; i < 0x300; ++i) {
        uint8_t b[10];
        if (!SafeCopyChars((char*)b, (const char*)p + i, sizeof(b))) return -1;
        if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x05 && b[7] == 0x48 && b[8] == 0x39 && b[9] == 0x87) {
            int32_t disp = 0;
            memcpy(&disp, b + 3, sizeof(disp));
            int64_t raw = 0;
            if (SafeCopyChars((char*)&raw, (const char*)p + i + 7 + disp, sizeof(raw))) return raw / 100000.0;
            return -1;
        }
    }
    return -1;
}

}  // namespace

std::string OutlinerManager::CountryDisplayName(uint32_t country_id) {
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    void* country = nullptr;
    uint32_t check = 0xFFFFFFFF;
    if (country_id == 0xFFFFFFFF || !SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)arr + (country_id & 0xFFFFFF) * 16 + 8), &country) || !country ||
        !SafeReadU32((const void*)((uintptr_t)country + 0x20), &check) || check != country_id) {
        return "";
    }
    // The display name the outliner tooltips pass for $BLOCKADER$ etc. (a CString at +0x14F0).
    std::string name;
    SafeReadPdxString((const void*)((uintptr_t)country + sdk::ent::CCountry::name - 0x30 + 0x10), name);
    return name.empty() ? "" : LocalizeKey(name);
}

std::vector<StatusAlertCard> OutlinerManager::ReadColonyStatus(void* colony_obj, void* planet_obj, uint32_t planet_id) {
    std::vector<StatusAlertCard> alerts;
    if (!colony_obj || !planet_obj) return alerts;

    auto add = [&](const std::string& id, const std::string& key, std::string name, std::string desc) {
        if (name.empty()) name = LocalizeKey(key);
        if (desc.empty()) {
            desc = LocalizeKey(key + "_DESC");
            if (desc == key + "_DESC") desc.clear();
        }
        alerts.push_back({ id, name, desc });
    };

    // -- crisis states --------------------------------------------------------------------
    void* stance = nullptr;
    SafeReadPtr((const void*)((uintptr_t)planet_obj + sdk::ent::CPlanet::ground_support_stance), &stance);
    if (IsRealObject(stance)) {
        // Blockader, as the CColonyCarrier slot the tooltip calls picks it: among the fleets in
        // orbit (planet +0x490, count +0x49C) with a real bombardment stance (+0x458), the one
        // with the highest bombardment power (+0x1288); its controller is at +0x448.
        uint32_t blockader = 0xFFFFFFFF;
        int64_t best = INT64_MIN;
        void* orbit_ids = nullptr;
        int orbit_count = 0;
        void* fleet_db = nullptr;
        void* fleet_arr = nullptr;
        uint32_t fleet_cap = 0;
        if (SafeReadPtr((const void*)((uintptr_t)planet_obj + 0x490), &orbit_ids) && orbit_ids &&
            SafeReadU32((const void*)((uintptr_t)planet_obj + 0x49C), (uint32_t*)&orbit_count) && orbit_count > 0 && orbit_count < 4096 &&
            SafeReadPtr((const void*)(base_address_ + sdk::db::CFleet), &fleet_db) && fleet_db &&
            SafeReadPtr((const void*)((uintptr_t)fleet_db + 0x18), &fleet_arr) && fleet_arr &&
            SafeReadU32((const void*)((uintptr_t)fleet_db + 0x20), &fleet_cap)) {
            for (int i = 0; i < orbit_count; ++i) {
                uint32_t fid = 0xFFFFFFFF, check = 0xFFFFFFFF, fleet_controller = 0xFFFFFFFF;
                void* fleet = nullptr;
                void* fleet_stance = nullptr;
                int64_t power = 0;
                if (!SafeReadU32((const void*)((uintptr_t)orbit_ids + i * 4), &fid) || (fid & 0xFFFFFF) >= fleet_cap ||
                    !SafeReadPtr((const void*)((uintptr_t)fleet_arr + (fid & 0xFFFFFF) * 16 + 8), &fleet) || !fleet ||
                    !SafeReadU32((const void*)((uintptr_t)fleet + 0x30), &check) || check != fid) {
                    continue;
                }
                SafeReadPtr((const void*)((uintptr_t)fleet + 0x458), &fleet_stance);
                SafeCopyChars((char*)&power, (const char*)((uintptr_t)fleet + 0x1288), sizeof(power));
                SafeReadU32((const void*)((uintptr_t)fleet + 0x448), &fleet_controller);
                if (IsRealObject(fleet_stance) && power > best) {
                    best = power;
                    blockader = fleet_controller;
                }
            }
        }
        std::string who = CountryDisplayName(blockader);
        std::string name = LocalizeWithParam(base_address_, "OUTLINER_PLANET_BLOCKADED", "BLOCKADER", who.empty() ? "?" : who);
        // The bombardment stance in use (CBombardmentStance: key at +0x20), e.g. "indiscriminate".
        std::string stance_key, desc;
        SafeReadPdxString((const void*)((uintptr_t)stance + 0x20), stance_key);
        if (!stance_key.empty()) {
            desc = LocalizeKey("bombardment_" + stance_key);
            std::string stance_desc = LocalizeKey("bombardment_" + stance_key + "_desc");
            if (stance_desc != "bombardment_" + stance_key + "_desc") desc += "\n" + stance_desc;
        }
        add("blockaded", "OUTLINER_PLANET_BLOCKADED", name, desc);
    }

    uint32_t owner = 0xFFFFFFFF, controller = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)planet_obj + sdk::ent::CPlanet::owner), &owner);
    SafeReadU32((const void*)((uintptr_t)planet_obj + sdk::ent::CPlanet::controller), &controller);
    if (owner != 0xFFFFFFFF && controller != 0xFFFFFFFF && controller != owner) {
        std::string who = CountryDisplayName(controller);
        add("occupied", "OUTLINER_PLANET_OCCUPIED",
            LocalizeWithParam(base_address_, "OUTLINER_PLANET_OCCUPIED", "OCCUPIER", who.empty() ? "?" : who), "");
    }

    // -- outliner status frames -------------------------------------------------------------
    struct Frame { int frame; const char* id; const char* key; };
    static const Frame kFrames[] = {
        { 0, "construction_available", "OUTLINER_PLANET_CONSTRUCTION_AVAILABLE" },
        { 2, "upgrade_available", "OUTLINER_PLANET_UPGRADE_AVAILABLE" },
        { 5, "unemployment", "OUTLINER_PLANET_UNEMPLOYMENT_PRESENT" },
        { 6, "excess_civilians", "OUTLINER_PLANET_AUTOM_MIGRATION_PRESENT" },
        { 7, "overcrowding", "OUTLINER_PLANET_OVERCROWDING_PRESENT" },
        { 10, "low_stability", "OUTLINER_PLANET_LOW_STABILITY" },
    };
    alignas(16) uint8_t dummy_controller[0x80]{};
    StatusFrameCtx ctx{ base_address_ + sdk::fn::COutlinerPlanetStatusController_ShouldShowStatusFrame,
                        dummy_controller, 0, colony_obj, GetPlayerCountryId(), false };
    for (const auto& f : kFrames) {
        ctx.frame = f.frame;
        ctx.result = false;
        if (!CommandBuilder::Get().CallGuarded(&CallShouldShowStatusFrame, &ctx) || !ctx.result) continue;
        std::string desc;
        if (f.frame == 10) {
            double min = LowPlanetStabilityDefine(ctx.fn);
            char buf[32];
            snprintf(buf, sizeof(buf), "%g", min);
            desc = LocalizeWithParam(base_address_, std::string(f.key) + "_DESC", "STABILITY_MIN", min >= 0 ? buf : "?");
        }
        add(f.id, f.key, "", desc);
    }

    auto blockers = GetClearableBlockersJson(planet_id);
    if (blockers.contains("blockers") && blockers["blockers"].is_array()) {
        for (const auto& b : blockers["blockers"]) {
            if (b.value("can_clear", false) && !b.value("is_queued", false)) {
                add("blocker_available", "OUTLINER_PLANET_BLOCKER_AVAILABLE", "", "");
                break;
            }
        }
    }
    return alerts;
}

// -------------------------------------------------------------
void OutlinerManager::BuildSectorGroups(std::vector<SectorGroup>& out_sectors) {
    out_sectors.clear();
    void* country = GetPlayerCountry();
    if (!country || !base_address_) return;

    // 1. Read player owned colony IDs from CCountry::owned_planets (data at +0x8, count at +0x14)
    void* colony_vec = nullptr;
    uint32_t colony_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x8), &colony_vec);
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x14), &colony_cnt);

    std::vector<uint32_t> player_colony_ids;
    if (colony_vec && colony_cnt > 0) {
        for (uint32_t i = 0; i < colony_cnt; ++i) {
            uint32_t cid = 0;
            if (SafeReadU32((const void*)((uintptr_t)colony_vec + i * 4), &cid)) {
                player_colony_ids.push_back(cid);
            }
        }
    }

    if (player_colony_ids.empty()) {
        return;
    }

    uint32_t capital_cid = 0xFFFFFFFF;  // CCountry::capital is the capital colony
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::capital), &capital_cid);

    // 2. Discover colonies and their carrier planets
    std::unordered_map<uint32_t, ColonyCard> colony_card_map; // key: cid
    for (uint32_t cid : player_colony_ids) {
        void* colony_obj = FindColony(cid);
        if (!colony_obj) continue;

        uint32_t planet_id = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xF78), &planet_id);
        if (planet_id == 0xFFFFFFFF) continue;

        void* p_obj = FindPlanet(planet_id);
        if (!p_obj) continue;

        ColonyCard card{};
        card.colony_id = cid;
        card.planet_id = planet_id;  // handle for the planet queries

        // Planet size at +0x150
        SafeReadU32((const void*)((uintptr_t)p_obj + 0x150), &card.size);

        card.name = PlanetName(p_obj);
        card.system_name = SystemName(PlanetSystemId(p_obj));
        card.is_capital = cid == capital_cid;

        // Pops demographics
        void* sp_arr = nullptr;
        uint32_t sp_cnt = 0;
        SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xF68), &sp_arr);
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xF70), &sp_cnt);
        uint32_t total_pops = 0;
        if (sp_arr && sp_cnt > 0 && sp_cnt < 256) {
            for (uint32_t si = 0; si < sp_cnt; ++si) {
                uint32_t cnt = 0;
                SafeReadU32((const void*)((uintptr_t)sp_arr + si * 12), &cnt);
                total_pops += cnt;
            }
        }
        card.pops = total_pops;

        // CColony::IsUnderColonization: the colonizing species is a real species; progress is
        // CColony::CalcColonizationProgressPerc (colony pops / COLONY_POPS_REQUIRED)
        uint32_t colonizing_species = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)colony_obj + sdk::ent::CColony::colonizing_species), &colonizing_species);
        card.is_colonizing = colonizing_species != 0xFFFFFFFF &&
                             SpeciesManager::Get().FindSpeciesPtr(colonizing_species) != nullptr;
        card.status = card.is_colonizing ? "colonizing" : "established";
        card.colonization_progress = card.is_colonizing ? ColonizationProgress(colony_obj) : 1.0;

        card.current_construction = ExtractColonyConstruction(colony_obj);
        if (!card.is_colonizing) card.status_alerts = ReadColonyStatus(colony_obj, p_obj, planet_id);

        colony_card_map[cid] = card;
    }

    if (colony_card_map.empty()) {
        return;
    }

    uint32_t country_id = GetPlayerCountryId();

    // 3. Match with Galaxy Sectors from CSectorManager (base + sdk::db::CSector)
    void* sec_mgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CSector), &sec_mgr);
    void* sec_arr = nullptr;
    uint32_t sec_cap = 0;
    if (sec_mgr) {
        SafeReadPtr((const void*)((uintptr_t)sec_mgr + 0x18), &sec_arr);
        SafeReadU32((const void*)((uintptr_t)sec_mgr + 0x20), &sec_cap);
    }

    std::unordered_set<uint32_t> assigned_cids;

    if (sec_arr && sec_cap > 0) {
        for (uint32_t sid = 0; sid < sec_cap; ++sid) {
            void* sec_ptr = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)sec_arr + sid * 16 + 8), &sec_ptr) || !sec_ptr) continue;

            uint32_t owner = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)sec_ptr + sdk::ent::CSector::owner), &owner);
            if (owner != country_id) continue;

            SectorGroup group{};
            group.sector_id = (int32_t)sid;
            void* sector_type = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)sec_ptr + sdk::ent::CSector::type), &sector_type) && sector_type) {
                SafeReadPdxString((const void*)((uintptr_t)sector_type + 0x20), group.focus_type);
            }
            group.is_core = group.focus_type == "core_sector";
            group.sector_name = PersistentNameText((const void*)((uintptr_t)sec_ptr + sdk::ent::CSector::name));

            uint32_t cap_cid = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)sec_ptr + sdk::ent::CSector::local_capital), &cap_cid);
            group.capital_planet_id = (cap_cid != 0xFFFFFFFF && colony_card_map.count(cap_cid)) ? colony_card_map[cap_cid].planet_id : 0xFFFFFFFF;
            group.capital_planet_name = (cap_cid != 0xFFFFFFFF && colony_card_map.count(cap_cid)) ? colony_card_map[cap_cid].name : "";

            void* c_vec = nullptr;
            uint32_t c_cnt = 0;
            SafeReadPtr((const void*)((uintptr_t)sec_ptr + 0x158), &c_vec);
            SafeReadU32((const void*)((uintptr_t)sec_ptr + 0x164), &c_cnt);

            if (c_vec && c_cnt > 0) {
                for (uint32_t ci = 0; ci < c_cnt; ++ci) {
                    uint32_t cid = 0xFFFFFFFF;
                    if (SafeReadU32((const void*)((uintptr_t)c_vec + ci * 4), &cid) && cid != 0xFFFFFFFF) {
                        auto it = colony_card_map.find(cid);
                        if (it != colony_card_map.end()) {
                            group.colonies.push_back(it->second);
                            group.total_colonies++;
                            group.total_pops += it->second.pops;
                            assigned_cids.insert(cid);
                        }
                    }
                }
            }

            out_sectors.push_back(group);
        }
    }

    // 4. Colonies in no sector (the game labels them NO_SECTOR)
    SectorGroup frontier{};
    frontier.unassigned = true;
    frontier.sector_id = -2;
    frontier.sector_name = LocalizeKey("NO_SECTOR");
    frontier.capital_planet_id = 0xFFFFFFFF;
    frontier.is_core = false;

    for (const auto& [cid, card] : colony_card_map) {
        if (assigned_cids.find(cid) == assigned_cids.end()) {
            frontier.colonies.push_back(card);
            frontier.total_colonies++;
            frontier.total_pops += card.pops;
        }
    }

    if (frontier.total_colonies > 0) {
        out_sectors.push_back(frontier);
    }
}

// -------------------------------------------------------------
// Layer 1: Global Outliner Summary
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetOutlinerSummaryJson() {
    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not available"} };

    // 1. Sectors summary with sector names and KPI
    std::vector<SectorGroup> sector_groups;
    BuildSectorGroups(sector_groups);

    uint32_t total_colonies = 0;
    uint32_t total_pops = 0;
    nlohmann::json sectors_list = nlohmann::json::array();
    for (const auto& s : sector_groups) {
        total_colonies += s.total_colonies;
        total_pops += s.total_pops;

        nlohmann::json col_summary = nlohmann::json::array();
        for (const auto& c : s.colonies) {
            col_summary.push_back({
                {"colony_id", c.colony_id},
                {"planet_id", c.planet_id},
                {"name", c.name},
                {"is_capital", c.is_capital},
                {"is_colonizing", c.is_colonizing},
                {"pops", c.pops},
                {"status", c.status},
                {"has_construction", c.current_construction.has_value()},
                {"status_alerts_count", (uint32_t)c.status_alerts.size()}
            });
        }

        sectors_list.push_back({
            {"sector_id", SectorIdJson(s)},
            {"sector_name", s.sector_name},
            {"capital_planet_id", s.capital_planet_id},
            {"capital_planet_name", s.capital_planet_name},
            {"is_core", s.is_core},
            {"colonies_count", s.total_colonies},
            {"total_pops", s.total_pops},
            {"colonies_summary", col_summary}
        });
    }

    // 2. Military fleets summary
    void* vec_ptr = nullptr;
    uint32_t template_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + sdk::ent::CCountry::fleet_template_manager + 0x8 + 8), &vec_ptr);
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::fleet_template_manager + 0x8 + 0x14), &template_cnt);

    double total_military_power = 0.0;
    std::vector<uint32_t> military_fleet_ids;

    if (vec_ptr && template_cnt > 0) {
        void* ft_mgr = nullptr;
        SafeReadPtr((const void*)(base_address_ + sdk::db::CFleetTemplate), &ft_mgr);
        void* ft_arr = nullptr;
        uint32_t ft_cap = 0;
        if (ft_mgr) {
            SafeReadPtr((const void*)((uintptr_t)ft_mgr + 0x18), &ft_arr);
            SafeReadU32((const void*)((uintptr_t)ft_mgr + 0x20), &ft_cap);
        }

        for (uint32_t i = 0; i < template_cnt; ++i) {
            uint32_t tid = 0;
            if (SafeReadU32((const void*)((uintptr_t)vec_ptr + i * 4), &tid) && ft_arr) {
                uint32_t slot = tid & 0xFFFFFF;
                if (slot < ft_cap) {
                    void* ft_obj = nullptr;
                    SafeReadPtr((const void*)((uintptr_t)ft_arr + slot * 16 + 8), &ft_obj);
                    if (ft_obj) {
                        uint32_t fid = 0;
                        SafeReadU32((const void*)((uintptr_t)ft_obj + 0x88), &fid);
                        if (fid != 0 && fid != 0xFFFFFFFF) {
                            military_fleet_ids.push_back(fid);
                            total_military_power += fleets::MilitaryPower(FindFleet(fid));
                        }
                    }
                }
            }
        }
    }

    // Armies: stationed on a colony vs embarked on transports
    uint32_t garrison_army_cnt = 0, transport_army_cnt = 0;
    for (void* army_obj : armies::Owned(base_address_, GetPlayerCountryId())) {
        uint32_t ship = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)army_obj + sdk::ent::CArmy::ship), &ship);
        (ship != 0xFFFFFFFF ? transport_army_cnt : garrison_army_cnt)++;
    }

    // 3. Civilian fleets summary: owned fleets of civilian ship classes
    uint32_t civilian_cnt = 0;
    for (uint32_t fid : fleets::Owned(country)) {
        if (fleets::IsCivilianShip(fleets::ClassOf(FindFleet(fid)))) {
            civilian_cnt++;
        }
    }

    // the capital colony's planet (CColony::carrier) and its system
    std::string capital_system;
    uint32_t capital_cid = 0xFFFFFFFF, capital_pid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::capital), &capital_cid);
    if (void* cap_colony = capital_cid != 0xFFFFFFFF ? FindColony(capital_cid) : nullptr) {
        SafeReadU32((const void*)((uintptr_t)cap_colony + sdk::ent::CColony::carrier), &capital_pid);
        capital_system = SystemName(PlanetSystemId(FindPlanet(capital_pid)));
    }

    return {
        {"sectors_summary", {
            {"total_sectors", (uint32_t)sector_groups.size()},
            {"total_colonies", total_colonies},
            {"total_pops", total_pops},
            {"capital_system", capital_system},
            {"sectors", sectors_list}
        }},
        {"military_fleets_summary", {
            {"military_fleets_count", (uint32_t)military_fleet_ids.size()},
            {"total_military_power", std::round(total_military_power * 10.0) / 10.0}
        }},
        {"civilian_fleets_summary", {
            {"civilian_fleets_count", civilian_cnt}
        }},
        {"armies_summary", {
            {"garrison_armies_count", garrison_army_cnt},
            {"transport_armies_count", transport_army_cnt}
        }}
    };
}

// -------------------------------------------------------------
// Layer 2: Category Details - Specific Sector Expansion
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetSectorsJson(int32_t sector_id) {
    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not available"} };

    std::vector<SectorGroup> sector_groups;
    BuildSectorGroups(sector_groups);

    if (sector_groups.empty()) {
        return { {"error", "No sectors available"} };
    }

    auto build_colony_json = [](const ColonyCard& c) -> nlohmann::json {
        nlohmann::json j = {
            {"colony_id", c.colony_id},
            {"id", c.planet_id},
            {"planet_id", c.planet_id},
            {"name", c.name},
            {"system_name", c.system_name},
            {"pops", c.pops},
            {"size", c.size},
            {"is_capital", c.is_capital},
            {"is_colonizing", c.is_colonizing},
            {"status", c.status}
        };
        if (c.is_colonizing) j["colonization_progress"] = c.colonization_progress;
        if (c.current_construction.has_value()) {
            const auto& cc = c.current_construction.value();
            j["current_construction"] = {
                {"type", cc.type},
                {"name", cc.name},
                {"key", cc.key},
                {"progress", cc.progress},
                {"remaining_days", cc.remaining_days}
            };
        } else {
            j["current_construction"] = nullptr;
        }

        nlohmann::json alerts_arr = nlohmann::json::array();
        for (const auto& a : c.status_alerts) {
            alerts_arr.push_back({
                {"id", a.id},
                {"name", a.name},
                {"desc", a.desc}
            });
        }
        j["status_alerts"] = alerts_arr;

        return j;
    };

    if (sector_id == -1) {
        nlohmann::json res_arr = nlohmann::json::array();
        for (const auto& s : sector_groups) {
            nlohmann::json colonies_arr = nlohmann::json::array();
            for (const auto& c : s.colonies) {
                colonies_arr.push_back(build_colony_json(c));
            }
            res_arr.push_back({
                {"sector_id", SectorIdJson(s)},
                {"sector_name", s.sector_name},
                {"capital_planet_id", s.capital_planet_id},
                {"capital_planet_name", s.capital_planet_name},
                {"is_core", s.is_core},
                {"sector_type", s.focus_type},
                {"total_colonies", s.total_colonies},
                {"total_pops", s.total_pops},
                {"colonies", colonies_arr}
            });
        }
        return {
            {"total_sectors", (uint32_t)sector_groups.size()},
            {"sectors", res_arr}
        };
    }

    const SectorGroup* target = nullptr;
    for (const auto& s : sector_groups) {
        if (s.sector_id == sector_id) {
            target = &s;
            break;
        }
    }

    if (!target) {
        nlohmann::json avail = nlohmann::json::array();
        for (const auto& s : sector_groups) {
            avail.push_back({ {"sector_id", SectorIdJson(s)}, {"sector_name", s.sector_name} });
        }
        return {
            {"error", "Sector not found. Please provide a valid sector_id."},
            {"available_sectors", avail}
        };
    }

    nlohmann::json colonies_arr = nlohmann::json::array();
    for (const auto& c : target->colonies) {
        colonies_arr.push_back(build_colony_json(c));
    }

    return {
        {"sector_id", SectorIdJson(*target)},
        {"sector_name", target->sector_name},
        {"capital_planet_id", target->capital_planet_id},
        {"capital_planet_name", target->capital_planet_name},
        {"is_core", target->is_core},
        {"sector_type", target->focus_type},
        {"total_colonies", target->total_colonies},
        {"total_pops", target->total_pops},
        {"colonies", colonies_arr}
    };
}

// -------------------------------------------------------------
// Layer 2: Category Details - Military Fleets
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetMilitaryFleetsJson() {
    auto fleets = FleetManager::Get().GetFleets(false);
    void* me = GetPlayerCountry();
    uint32_t my_id = 0xFFFFFFFF;
    if (me) SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id);

    /* owned_fleets 这条数组**也包含借入/借出的条目**（SOwnedFleetEntry 里有 ownership_status /
       lease_period / debtor / previous_owner）。借来的舰队没有本国模板，所以模板驱动的那份列表里
       看不到它们 —— 这里按 id 建索引补上，并给每条舰队带上 owner / controller / relation。
       2026-11-21：借出期间 owner 不变、controller 换成借用国（玩家在界面上确认）。 */
    std::unordered_map<uint32_t, bridge::fleets::OwnedFleetEntry> lease;
    if (me) {
        for (const auto& e : bridge::fleets::OwnedEntries(me)) lease[e.fleet_id] = e;
    }

    auto relation_of = [&](uint32_t owner, uint32_t controller) -> const char* {
        if (owner == my_id && (controller == my_id || controller == 0xFFFFFFFF)) return "own";
        if (owner == my_id) return "lent";
        if (controller == my_id) return "borrowed";
        return "other";
    };
    auto ship_count = [](void* flt) {
        uint32_t n = 0;
        if (flt) SafeReadU32((const void*)((uintptr_t)flt + sdk::ent::CFleet::ships + 0xC), &n);
        return n;
    };

    nlohmann::json arr = nlohmann::json::array();
    std::unordered_set<uint32_t> seen;

    for (const auto& f : fleets) {
        void* flt = FindFleet(f.fleet_id);
        const uint32_t owner = flt ? bridge::fleets::Owner(flt) : 0xFFFFFFFF;
        const uint32_t controller = flt ? bridge::fleets::Controller(flt) : 0xFFFFFFFF;
        nlohmann::json j = {
            {"fleet_id", f.fleet_id},
            {"template_id", f.template_id},
            {"name", f.name},
            {"military_power", f.military_power},
            {"total_ships", f.total_ships},
            {"total_quota", f.total_quota},
            {"can_reinforce", f.can_reinforce},
            {"status", bridge::fleets::OrdersText(flt)},
            {"bombardment_stance", BombardmentStanceOf(base_address_, flt)},
            {"owner", owner},
            {"controller", controller},
            {"owner_name", CountryDisplayName(owner)},
            {"controller_name", CountryDisplayName(controller)},
            {"relation", relation_of(owner, controller)}
        };
        auto it = lease.find(f.fleet_id);
        if (it != lease.end()) {
            j["ownership_status"] = it->second.ownership_status;
            j["lease_period"] = it->second.lease_period;
            if (it->second.debtor != 0xFFFFFFFF) {
                j["debtor"] = it->second.debtor;
                j["debtor_name"] = CountryDisplayName(it->second.debtor);
            }
            if (it->second.previous_owner != 0xFFFFFFFF) {
                j["previous_owner"] = it->second.previous_owner;
                j["previous_owner_name"] = CountryDisplayName(it->second.previous_owner);
            }
        }
        arr.push_back(j);
        seen.insert(f.fleet_id);
    }

    // 借来的舰队：本国没有它们的模板，模板驱动的那份列表里不会出现
    for (const auto& kv : lease) {
        if (seen.count(kv.first)) continue;
        void* flt = FindFleet(kv.first);
        if (!flt) continue;
        const auto cls = bridge::fleets::ClassOf(flt);
        if (cls != bridge::fleets::ShipClass::Military && cls != bridge::fleets::ShipClass::MilitarySpecial) continue;
        const uint32_t owner = bridge::fleets::Owner(flt);
        const uint32_t controller = bridge::fleets::Controller(flt);
        nlohmann::json j = {
            {"fleet_id", kv.first},
            {"template_id", bridge::fleets::TemplateId(flt)},
            {"name", bridge::fleets::Name(flt)},
            {"military_power", bridge::fleets::MilitaryPower(flt)},
            {"total_ships", ship_count(flt)},
            {"total_quota", ship_count(flt)},
            {"can_reinforce", false},
            {"status", bridge::fleets::OrdersText(flt)},
            {"owner", owner},
            {"controller", controller},
            {"owner_name", CountryDisplayName(owner)},
            {"controller_name", CountryDisplayName(controller)},
            {"relation", relation_of(owner, controller)},
            {"ownership_status", kv.second.ownership_status},
            {"lease_period", kv.second.lease_period}
        };
        if (kv.second.debtor != 0xFFFFFFFF) {
            j["debtor"] = kv.second.debtor;
            j["debtor_name"] = CountryDisplayName(kv.second.debtor);
        }
        if (kv.second.previous_owner != 0xFFFFFFFF) {
            j["previous_owner"] = kv.second.previous_owner;
            j["previous_owner_name"] = CountryDisplayName(kv.second.previous_owner);
        }
        arr.push_back(j);
        seen.insert(kv.first);
    }
    // 借入的舰队：借入国没有它们的 owned_fleets 条目，模板驱动的那份列表里也没有 —— 扫舰队库，
    // 找「controller 是本国、owner 不是」的。2026-11-21 实测：破浪者/飓风 在借用国这边就是这个状况。
    for (uint32_t id : bridge::fleets::AllIds(base_address_)) {
        if (seen.count(id)) continue;
        void* flt = FindFleet(id);
        if (!flt) continue;
        const uint32_t owner = bridge::fleets::Owner(flt);
        const uint32_t controller = bridge::fleets::Controller(flt);
        if (controller != my_id || owner == my_id) continue;
        const auto cls = bridge::fleets::ClassOf(flt);
        if (cls != bridge::fleets::ShipClass::Military && cls != bridge::fleets::ShipClass::MilitarySpecial) continue;
        arr.push_back({
            {"fleet_id", id},
            {"template_id", bridge::fleets::TemplateId(flt)},
            {"name", bridge::fleets::Name(flt)},
            {"military_power", bridge::fleets::MilitaryPower(flt)},
            {"total_ships", ship_count(flt)},
            {"total_quota", ship_count(flt)},
            {"can_reinforce", false},
            {"status", bridge::fleets::OrdersText(flt)},
            {"owner", owner},
            {"controller", controller},
            {"owner_name", CountryDisplayName(owner)},
            {"controller_name", CountryDisplayName(controller)},
            {"relation", relation_of(owner, controller)}
        });
        seen.insert(id);
    }

    /* 方舟（Nomads：殖民地载体舰）。它既不是军事也不是民用，模板里也没有它，所以上面三条路都看不到；
       但它的战力是真的（本档 4.3k / 5.8k）、能移动、能设姿态与轰炸姿态 —— 玩家 2026-11-23 定：
       **按军用舰队口径把它们列出来**。
       判定：class = Starbase，且舰队里有一艘船的 CShip+0x100（CColony 链）非空 —— 那就是殖民地载体。 */
    {
        auto ship_of = [&](uint32_t sid) -> void* {
            void* mgr = nullptr;
            if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CShip), &mgr) || !mgr) return nullptr;
            void* sarr = nullptr;
            uint32_t scap = 0;
            SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &sarr);
            SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &scap);
            const uint32_t slot = sid & 0xFFFFFF;
            if (!sarr || slot >= scap) return nullptr;
            void* ship = nullptr;
            SafeReadPtr((const void*)((uintptr_t)sarr + slot * 16 + 8), &ship);
            return ship;
        };
        for (uint32_t id : bridge::fleets::AllIds(base_address_)) {
            if (seen.count(id)) continue;
            void* flt = FindFleet(id);
            if (!flt) continue;
            if (bridge::fleets::Owner(flt) != my_id) continue;
            if (bridge::fleets::ClassOf(flt) != bridge::fleets::ShipClass::Starbase) continue;
            void* ships = nullptr;
            uint32_t n = 0;
            SafeReadPtr((const void*)((uintptr_t)flt + sdk::ent::CFleet::ships), &ships);
            SafeReadU32((const void*)((uintptr_t)flt + sdk::ent::CFleet::ships + 0xC), &n);
            if (!ships || n == 0 || n > 64) continue;
            uint32_t ark_ship = 0xFFFFFFFF;
            uint32_t ark_colony = 0xFFFFFFFF;
            for (uint32_t k = 0; k < n; ++k) {
                uint32_t sid = 0;
                if (!SafeReadU32((const void*)((uintptr_t)ships + k * 4), &sid)) continue;
                void* ship = ship_of(sid);
                if (!ship) continue;
                // CShip+0x100 是 **colony 引用**（低 4 字节 = colony id、高 4 字节 = type），不是指针
                // —— 驿站那艘船这一格也有值，所以单看非空会把 13 条驿港都算成方舟（踩过）。
                // 还要回链核对：那个 CColony 的 carrier 必须指回这艘船（id = 船 id、type = 1）。
                uint32_t col_id = 0xFFFFFFFF, col_type = 0xFFFFFFFF;
                if (!SafeReadU32((const void*)((uintptr_t)ship + 0x100), &col_id)) continue;
                SafeReadU32((const void*)((uintptr_t)ship + 0x104), &col_type);
                if (col_id == 0xFFFFFFFF) continue;
                void* cmgr = nullptr;
                if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CColony), &cmgr) || !cmgr) continue;
                void* carr = nullptr;
                uint32_t ccap = 0;
                SafeReadPtr((const void*)((uintptr_t)cmgr + 0x18), &carr);
                SafeReadU32((const void*)((uintptr_t)cmgr + 0x20), &ccap);
                if (!carr || (col_id & 0xFFFFFF) >= ccap) continue;
                void* col = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)carr + (col_id & 0xFFFFFF) * 16 + 8), &col) || !col) continue;
                uint64_t carrier = 0;
                if (!SafeReadU64((const void*)((uintptr_t)col + sdk::ent::CColony::carrier), &carrier)) continue;
                const uint32_t cid = (uint32_t)(carrier & 0xFFFFFFull);
                const uint32_t ctype = (uint32_t)(carrier >> 32);
                if (ctype != 1 || cid != (sid & 0xFFFFFF)) continue;
                ark_ship = sid;
                ark_colony = col_id;
                break;
            }
            if (ark_ship == 0xFFFFFFFF) continue;
            uint32_t stance = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)flt + sdk::ent::CFleet::fleet_stance), &stance);
            arr.push_back({
                {"fleet_id", id},
                {"template_id", bridge::fleets::TemplateId(flt)},
                {"name", bridge::fleets::Name(flt)},
                {"military_power", bridge::fleets::MilitaryPower(flt)},
                {"total_ships", ship_count(flt)},
                {"total_quota", ship_count(flt)},
                {"can_reinforce", false},
                {"status", bridge::fleets::OrdersText(flt)},
                {"bombardment_stance", BombardmentStanceOf(base_address_, flt)},
                {"owner", my_id},
                {"controller", my_id},
                {"owner_name", CountryDisplayName(my_id)},
                {"controller_name", CountryDisplayName(my_id)},
                {"relation", "ark"},
                {"ark", true},
                {"station_ship_id", ark_ship},
                {"colony_id", ark_colony},
                {"stance", stance}
            });
            seen.insert(id);
        }
    }

    return {
        {"total_military_fleets", arr.size()},
        {"fleets", arr}
    };
}

// 可选的轰炸姿态（档位键 + 对象地址），诊断用
nlohmann::json OutlinerManager::GetBombardmentStancesJson() {
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kv : BombardmentStances(base_address_)) {
        arr.push_back({ {"key", kv.first}, {"ptr", (unsigned long long)(uintptr_t)kv.second} });
    }
    return { {"success", true}, {"total", arr.size()}, {"stances", arr} };
}

// 给一支舰队设轰炸姿态：走引擎自己的 CSetFleetGroundSupportStanceCommand
nlohmann::json OutlinerManager::SetBombardmentStanceJson(uint32_t fleet_id, const std::string& stance_key,
                                                        bool dispatch) {
    namespace q = sdk::cmd::set_fleet_ground_support_stance_command;
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    void* flt = FindFleet(fleet_id);
    if (!flt) return { {"success", false}, {"error", "fleet " + std::to_string(fleet_id) + " not found"} };

    void* target = nullptr;
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& kv : BombardmentStances(base_address_)) {
        keys.push_back(kv.first);
        if (kv.first == stance_key) target = kv.second;
    }
    if (!target) {
        return { {"success", false}, {"error", "unknown bombardment stance: " + stance_key}, {"stance_keys", keys} };
    }

    const std::string before = BombardmentStanceOf(base_address_, flt);
    auto cmd = CommandBuilder::Get().Create(q::kSpec);
    if (!cmd) return { {"success", false}, {"error", cmd.error()} };
    cmd.Set<uint32_t>(q::fleet, fleet_id).Set<void*>(q::ground_support_stance, target);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "the engine refused the bombardment stance" : why} };
    }
    if (!dispatch) {
        return { {"success", true}, {"dry_run", true}, {"fleet_id", fleet_id}, {"stance", stance_key}, {"current", before} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"stance", stance_key}, {"previous", before},
             {"current", BombardmentStanceOf(base_address_, flt)} };
}

// -------------------------------------------------------------
// Layer 2: Category Details - Civilian Fleets
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetCivilianFleetsJson() {
    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not available"} };

    // Read leaders to map scientist assignments
    void* l_arr = nullptr;
    uint32_t l_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_leaders + 0x8), &l_arr);
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_leaders + 0x14), &l_cnt);

    // 领袖的 assignment_target 是**舰船 id**（CShip），不是舰队 id —— 单舰舰队两者并不相等
    // （实测 fleet 776 的船是 2665，领袖「前桅帆奥迭戈」也挂在 2665 上）。老代码直接拿 fleet id 去查，
    // 于是 8 艘科研船里只有 fleet 1 显示得出领袖。这里先按船建表，再用「舰船 → 舰队」换回来。
    std::unordered_map<uint32_t, HiredLeaderDetail> ship_leader_map;
    if (l_arr && l_cnt > 0) {
        for (uint32_t i = 0; i < l_cnt; ++i) {
            uint32_t lid = 0;
            if (SafeReadU32((const void*)((uintptr_t)l_arr + i * 4), &lid)) {
                auto leader = LeaderManager::Get().ReadLeader(lid);
                if (leader.assignment_type == 2) { // Fleet：target 是领袖所在的那艘船
                    ship_leader_map[leader.assignment_target] = leader;
                    ship_leader_map[leader.assignment_target & 0xFFFFFF] = leader;
                }
            }
        }
    }

    std::unordered_map<uint32_t, uint32_t> ship_to_fleet;
    for (uint32_t fid : fleets::Owned(country)) {
        void* flt = FindFleet(fid);
        if (!flt) continue;
        void* sarr = nullptr;
        uint32_t scnt = 0;
        if (!SafeReadPtr((const void*)((uintptr_t)flt + sdk::ent::CFleet::ships), &sarr) ||
            !SafeReadU32((const void*)((uintptr_t)flt + sdk::ent::CFleet::ships + 0xC), &scnt)) continue;
        if (!sarr || scnt > 5000) continue;
        for (uint32_t i = 0; i < scnt; ++i) {
            uint32_t sid = 0xFFFFFFFF;
            if (!SafeReadU32((const void*)((uintptr_t)sarr + i * 4), &sid)) break;
            ship_to_fleet[sid] = fid;
            ship_to_fleet[sid & 0xFFFFFF] = fid;
        }
    }

    std::unordered_map<uint32_t, HiredLeaderDetail> fleet_leader_map;
    for (const auto& kv : ship_leader_map) {
        auto it = ship_to_fleet.find(kv.first);
        if (it != ship_to_fleet.end()) fleet_leader_map[it->second] = kv.second;
    }

    nlohmann::json science_ships = nlohmann::json::array();
    nlohmann::json construction_ships = nlohmann::json::array();
    nlohmann::json colony_ships = nlohmann::json::array();

    nlohmann::json transport_fleets = nlohmann::json::array();
    for (uint32_t fid : fleets::Owned(country)) {
        void* flt = FindFleet(fid);
        const fleets::ShipClass cls = fleets::ClassOf(flt);
        if (!fleets::IsCivilianShip(cls)) continue;

        nlohmann::json entry = {
            {"fleet_id", fid},
            {"name", fleets::Name(flt)},
            {"ship_class", fleets::ShipClassKey(cls)},
            {"status", fleets::OrdersText(flt)}
        };
        auto leader = fleet_leader_map.find(fid);
        if (leader != fleet_leader_map.end()) {
            entry["leader_name"] = leader->second.name;
            entry["leader_level"] = leader->second.level;
        }
        switch (cls) {
            case fleets::ShipClass::ScienceShip: science_ships.push_back(entry); break;
            case fleets::ShipClass::Constructor: construction_ships.push_back(entry); break;
            case fleets::ShipClass::Colonizer: colony_ships.push_back(entry); break;
            default: transport_fleets.push_back(entry); break;
        }
    }

    return {
        {"science_ships_count", science_ships.size()},
        {"science_ships", science_ships},
        {"construction_ships_count", construction_ships.size()},
        {"construction_ships", construction_ships},
        {"colony_ships_count", colony_ships.size()},
        {"colony_ships", colony_ships},
        {"transport_fleets_count", transport_fleets.size()},
        {"transport_fleets", transport_fleets}
    };
}

// -------------------------------------------------------------
// Layer 2: Category Details - Armies
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetArmiesJson() {
    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not available"} };

    nlohmann::json garrison_armies = nlohmann::json::array();
    nlohmann::json transport_armies = nlohmann::json::array();
    for (void* army_obj : armies::Owned(base_address_, GetPlayerCountryId())) {
        armies::ArmyInfo army;
        if (!armies::Read(army_obj, army)) continue;
        nlohmann::json j = armies::ToJson(army);
        j["species_name"] = SpeciesName(army.species);
        if (army.ship != 0xFFFFFFFF) {
            transport_armies.push_back(j);
            continue;
        }
        // stationed: the colony's planet (CColony +0xF78)
        uint32_t planet_id = 0xFFFFFFFF;
        if (void* colony = FindColony(army.colony)) {
            SafeReadU32((const void*)((uintptr_t)colony + 0xF78), &planet_id);
        }
        j["colony_id"] = army.colony;
        j["planet_id"] = planet_id;
        j["planet_name"] = PlanetName(FindPlanet(planet_id));
        garrison_armies.push_back(j);
    }

    return {
        {"total_armies", garrison_armies.size() + transport_armies.size()},
        {"garrison_armies_count", garrison_armies.size()},
        {"garrison_armies", garrison_armies},
        {"transport_armies_count", transport_armies.size()},
        {"transport_armies", transport_armies}
    };
}

// -------------------------------------------------------------
// Layer 3: Entity Deep Inspection - Planet Details
// -------------------------------------------------------------
nlohmann::json OutlinerManager::GetPlanetDetailsJson(uint32_t planet_id) {
    if (!base_address_) {
        return { {"success", false}, {"error", "Base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return {
            {"success", false},
            {"error", "Planet with ID " + std::to_string(planet_id) + " not found"}
        };
    }

    // 1. Resolve Colony ID & Colony Object
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    void* colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;

    // 2. Resolve Capital & Basic Identification (CCountry::capital is the capital colony)
    void* country = GetPlayerCountry();
    uint32_t capital_cid = 0xFFFFFFFF;
    if (country) {
        SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::capital), &capital_cid);
    }
    uint32_t sys_id = PlanetSystemId(p_obj);
    bool is_capital = cid != 0xFFFFFFFF && cid == capital_cid;
    std::string p_name = PlanetName(p_obj);
    std::string sys_name = SystemName(sys_id);

    // 2.1 Designation & Designation / Ascension Tier
    std::string desig_key;
    std::string desig_name;
    uint32_t desig_tier = 0;
    uint32_t gov_id = 0xFFFFFFFF;

    if (colony_obj) {
        void* desig_def = nullptr;
        SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x100), &desig_def);
        if (desig_def) {
            SafeReadPdxString((const void*)((uintptr_t)desig_def + 0x20), desig_key);
            desig_name = LocalizeKey(desig_key);
        }
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1020), &desig_tier);
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xFC), &gov_id);
    }
    // no colony (or no designation): nothing to show

    // 2.2 Governor & Leader Info
    nlohmann::json governor_json = nullptr;
    if (gov_id != 0xFFFFFFFF) {
        HiredLeaderDetail gov = LeaderManager::Get().ReadLeader(gov_id);
        if (gov.id != 0) {
            nlohmann::json traits_arr = nlohmann::json::array();
            for (const auto& t : gov.traits) {
                traits_arr.push_back({
                    {"key", t.key},
                    {"name", t.name},
                    {"tier", t.tier}
                });
            }
            governor_json = {
                {"id", gov.id},
                {"name", gov.name},
                {"title", gov.title},
                {"class_key", gov.class_key},
                {"class_name", gov.class_name},
                {"background_job", gov.background_job_key},
                {"background_job_name", gov.background_job_name},
                {"level", gov.level},
                {"experience", gov.experience},
                {"age", gov.age},
                {"ethic_key", gov.ethic_key},
                {"ethic_name", gov.ethic_name},
                {"assignment_type", gov.assignment_type},
                {"assignment_type_name", gov.assignment_type_name},
                {"assignment_target", gov.assignment_target},
                {"traits", traits_arr},
                {"has_unspent_trait_points", gov.has_unspent_trait_points},
                {"is_councilor", gov.is_councilor}
            };
        }
    }

    // 3. Demographics & Pop Groups (Direct read from CColony)
    uint32_t pop_capacity = 0;
    uint32_t pop_groups = 0;
    if (colony_obj) {
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xfc8), &pop_capacity);
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xfe8), &pop_groups);
        if (pop_groups == 0) {
            pop_groups = pop_capacity;
        }
    }

    std::string pop_display;
    if (pop_groups >= 1000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fK", (double)pop_groups / 1000.0);
        pop_display = buf;
    } else {
        pop_display = std::to_string(pop_groups);
    }

    // 4. Welfare KPI Metrics (Direct fixed-point normalization from CColony)
    int64_t housing_raw = 0;
    int64_t amenities_raw = 0;
    int64_t stability_raw = 0;
    int64_t crime_raw = 0;
    uint32_t unemployed_val = 0;
    uint32_t growth_val = 0;

    if (colony_obj) {
        SafeReadI64((const void*)((uintptr_t)colony_obj + 0xf98), &housing_raw);
        SafeReadI64((const void*)((uintptr_t)colony_obj + 0xfa0), &amenities_raw);
        SafeReadI64((const void*)((uintptr_t)colony_obj + 0xfb8), &stability_raw);
        SafeReadI64((const void*)((uintptr_t)colony_obj + 0xfc0), &crime_raw);
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xfd0), &unemployed_val);
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0xfd4), &growth_val);
    }

    double housing_val = std::round((double)housing_raw / 1000.0) / 100.0;
    double amenities_val = std::round((double)amenities_raw / 1000.0) / 100.0;
    uint32_t stability_percent = (uint32_t)std::round((double)stability_raw / 100000.0);
    uint32_t crime_percent = (uint32_t)std::round((double)crime_raw / 100000.0);

    std::string amenities_display;
    if (std::abs(amenities_val) >= 1000.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fK", amenities_val / 1000.0);
        amenities_display = buf;
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", amenities_val);
        amenities_display = buf;
    }

    // 5. Planet Overview
    std::string planet_type;
    void* p_class_def = nullptr;
    if (SafeReadPtr((const void*)((uintptr_t)p_obj + 0x148), &p_class_def) && p_class_def) {
        std::string class_key;
        if (SafeReadPdxString((const void*)((uintptr_t)p_class_def + 0x28), class_key)) {
            planet_type = LocalizePlanetClass(class_key);
        }
    }

    // habitability for the player's founder species (NHabitability::CalcHabitability)
    uint32_t founder = 0xFFFFFFFF;
    if (country) SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::founder_species_ref), &founder);
    void* founder_sp = SpeciesManager::Get().FindSpeciesPtr(founder);
    double habitability = founder_sp ? Habitability(base_address_, founder_sp, p_obj, country) : -1;
    nlohmann::json habitability_percent = habitability >= 0 ? nlohmann::json(std::round(habitability * 1000.0) / 10.0)
                                                            : nlohmann::json(nullptr);
    uint32_t planet_size = 0;
    SafeReadU32((const void*)((uintptr_t)p_obj + sdk::ent::CPlanet::planet_size), &planet_size);

    std::string colony_date = "2016.05.09";
    if (colony_obj) {
        uint32_t raw_hours = 0;
        if (SafeReadU32((const void*)((uintptr_t)colony_obj + 0x114), &raw_hours) && raw_hours > 0x29C55C0) {
            uint32_t total_days = (raw_hours - 0x29C55C0) / 24;
            uint32_t y = total_days / 360;
            uint32_t m = (total_days % 360) / 30 + 1;
            uint32_t d = (total_days % 360) % 30 + 1;
            char buf[32];
            snprintf(buf, sizeof(buf), "%04u.%02u.%02u", y, m, d);
            colony_date = buf;
        }
    }

    // 6. Real dynamic districts & 4.5.0 specialization zones
    nlohmann::json districts = nlohmann::json::array();
    if (colony_obj) {
        void* d_mgr = nullptr;
        void* z_mgr = nullptr;
        void* b_mgr = nullptr;
        SafeReadPtr((const void*)(base_address_ + sdk::db::CDistrict), &d_mgr);
        SafeReadPtr((const void*)(base_address_ + sdk::db::CZone), &z_mgr);
        SafeReadPtr((const void*)(base_address_ + sdk::db::CBuilding), &b_mgr);

        if (d_mgr && z_mgr && b_mgr && (uintptr_t)d_mgr >= 0x10000 && (uintptr_t)z_mgr >= 0x10000 && (uintptr_t)b_mgr >= 0x10000) {
            void* d_arr = nullptr;
            uint32_t d_cap = 0;
            SafeReadPtr((const void*)((uintptr_t)d_mgr + 0x18), &d_arr);
            SafeReadU32((const void*)((uintptr_t)d_mgr + 0x20), &d_cap);

            void* z_arr = nullptr;
            uint32_t z_cap = 0;
            SafeReadPtr((const void*)((uintptr_t)z_mgr + 0x18), &z_arr);
            SafeReadU32((const void*)((uintptr_t)z_mgr + 0x20), &z_cap);

            void* b_arr = nullptr;
            uint32_t b_cap = 0;
            SafeReadPtr((const void*)((uintptr_t)b_mgr + 0x18), &b_arr);
            SafeReadU32((const void*)((uintptr_t)b_mgr + 0x20), &b_cap);


            /* 本殖民地的建筑 id 表：+0x58 指针，+0x60 高 32 位容量 / 低 32 位数量 */
            std::vector<uint32_t> colony_building_ids;
            {
                void* bid_arr = nullptr;
                uint32_t bid_pack = 0;
                if (SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x58), &bid_arr) && bid_arr &&
                    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x60), &bid_pack)) {
                    const uint32_t bid_count = bid_pack & 0xFFFF;
                    if (bid_count <= 4096) {
                        for (uint32_t i = 0; i < bid_count; ++i) {
                            uint32_t bid = 0;
                            if (SafeReadU32((const void*)((uintptr_t)bid_arr + i * 4), &bid)) {
                                colony_building_ids.push_back(bid);
                            }
                        }
                    }
                }
            }

            /* 有些 zone 不在数据库遍历结果里（升级/新建后对象被替换），
               但建筑自己的 +0x18 指向它。把建筑引用的 zone 补进来。 */
            std::vector<void*> zone_objs;
            {
                std::set<void*> seen;
                for (uint32_t zs = 0; zs < z_cap; ++zs) {
                    void* z_obj = nullptr;
                    if (SafeReadPtr((const void*)((uintptr_t)z_arr + zs * 16 + 8), &z_obj) && z_obj) {
                        if (seen.insert(z_obj).second) zone_objs.push_back(z_obj);
                    }
                }
                for (uint32_t bid : colony_building_ids) {
                    void* b_obj = nullptr;
                    const uint32_t slot = bid & 0xFFFFFFu;
                    if (slot >= b_cap) continue;
                    if (!SafeReadPtr((const void*)((uintptr_t)b_arr + slot * 16 + 8), &b_obj) || !b_obj) continue;
                    void* p_zone = nullptr;
                    if (SafeReadPtr((const void*)((uintptr_t)b_obj + 0x18), &p_zone) && p_zone) {
                        if (seen.insert(p_zone).second) zone_objs.push_back(p_zone);
                    }
                }
            }

            for (uint32_t ds = 0; ds < d_cap; ++ds) {
                void* d_obj = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)d_arr + ds * 16 + 8), &d_obj) || !d_obj) continue;
                void* col_ptr = nullptr;
                SafeReadPtr((const void*)((uintptr_t)d_obj + 0x18), &col_ptr);
                if (col_ptr != colony_obj) continue;

                void* d_def = nullptr;
                SafeReadPtr((const void*)((uintptr_t)d_obj + sdk::ent::CDistrict::type), &d_def);
                std::string d_key;
                if (d_def) SafeReadPdxString((const void*)((uintptr_t)d_def + 0x20), d_key);
                // the district count (save token "level") and its zone slots (CDistrict::zones size)
                uint32_t d_built = 0;
                uint32_t d_zone_slots = 0;
                SafeReadU32((const void*)((uintptr_t)d_obj + sdk::ent::CDistrict::level), &d_built);
                SafeReadU32((const void*)((uintptr_t)d_obj + sdk::ent::CDistrict::zones), &d_zone_slots);

                nlohmann::json zones = nlohmann::json::array();
                auto district_type_key = [&](void* dobj) -> std::string {
                    void* def = nullptr;
                    std::string key;
                    if (dobj && SafeReadPtr((const void*)((uintptr_t)dobj + sdk::ent::CDistrict::type), &def) && def) {
                        SafeReadPdxString((const void*)((uintptr_t)def + 0x20), key);
                    }
                    return key;
                };
                const std::string this_district_key = district_type_key(d_obj);

                for (void* z_obj : zone_objs) {
                    if (!z_obj) continue;
                    void* p_dist = nullptr;
                    SafeReadPtr((const void*)((uintptr_t)z_obj + 0x18), &p_dist);
                    if (p_dist != d_obj) {
                        /* 升级/新建后可能出现"另一个同类型区划对象"：
                           要求类型相同，且区划必须属于本殖民地 */
                        const std::string pd_key = district_type_key(p_dist);
                        if (pd_key.empty() || pd_key != this_district_key) continue;
                        void* pd_colony = nullptr;
                        SafeReadPtr((const void*)((uintptr_t)p_dist + 0x18), &pd_colony);
                        if (pd_colony != colony_obj) continue;
                    }

                    void* z_def = nullptr;
                    SafeReadPtr((const void*)((uintptr_t)z_obj + 0x20), &z_def);
                    std::string z_key;
                    if (z_def) SafeReadPdxString((const void*)((uintptr_t)z_def + 0x20), z_key);
                    uint32_t z_slot_idx = 0;
                    SafeReadU32((const void*)((uintptr_t)z_obj + 8), &z_slot_idx);

                    /* 建筑按 **zone 自己的数组**（CZone::buildings, +0x1A0）枚举。
                       旧做法走 colony+0x58 的 id 表 + 建筑 +0x18 的 zone 指针：新建的建筑在本会话里
                       根本没写进那份 id 表（读不到），而表里残留的 0 被当成 id 0（正是首府），
                       于是同一个首府被重复打印。zone 的数组由引擎自己维护，新建筑当场就在里面
                       （get_buildable_buildings 的「已用 N/M」也数这条）。
                       实测 2026-11-18：档案馆里新造的研究实验室，只有这条路径读得到。 */
                    nlohmann::json bldgs = nlohmann::json::array();
                    {
                        const uintptr_t zb_base = (uintptr_t)z_obj + sdk::ent::CZone::buildings;
                        void* zb_data = nullptr;
                        uint32_t zb_count = 0;
                        if (SafeReadPtr((const void*)(zb_base + 0x8), &zb_data) && zb_data &&
                            SafeReadU32((const void*)(zb_base + 0x14), &zb_count) && zb_count <= 64) {
                            for (uint32_t i = 0; i < zb_count; ++i) {
                                // ref_array<TPdxRef<CBuilding>>：元素是 4 字节的建筑 id，再经数据库解析对象
                                uint32_t bid = 0xFFFFFFFF;
                                if (!SafeReadU32((const void*)((uintptr_t)zb_data + i * 4), &bid)) continue;
                                const uint32_t slot = bid & 0xFFFFFFu;
                                if (bid == 0xFFFFFFFF || slot >= b_cap) continue;
                                void* b_obj = nullptr;
                                if (!SafeReadPtr((const void*)((uintptr_t)b_arr + slot * 16 + 8), &b_obj) || !b_obj) continue;
                                void* b_def = nullptr;
                                std::string b_key;
                                if (SafeReadPtr((const void*)((uintptr_t)b_obj + 0x20), &b_def) && b_def) {
                                    SafeReadPdxString((const void*)((uintptr_t)b_def + 0x20), b_key);
                                }
                                bool b_is_capital = (b_key.rfind("building_capital", 0) == 0 || b_key == "building_colony_shelter");
                                // 格子号读建筑自己的 position（CBuilding+0x1A4），与 GUI 一致；
                                // 读不到就退回数组下标
                                uint32_t b_pos = (uint32_t)bldgs.size();
                                if (!SafeReadU32((const void*)((uintptr_t)b_obj + sdk::ent::CBuilding::position), &b_pos)) {
                                    b_pos = (uint32_t)bldgs.size();
                                }
                                bldgs.push_back({
                                    {"slot_index", b_pos},
                                    {"id", bid},
                                    {"key", b_key},
                                    {"name", LocalizeKey(b_key)},
                                    {"status", "built"},
                                    {"is_capital", b_is_capital}
                                });
                            }
                        }
                    }

                    MaxBuildingsCtx mctx{ base_address_ + sdk::fn::CColony_CalcMaxBuildings, colony_obj, z_obj, GetPlayerCountry(), 0 };
                    int max_buildings = CommandBuilder::Get().CallGuarded(&CallMaxBuildings, &mctx) ? mctx.result : -1;
                    zones.push_back({
                        {"slot_index", z_slot_idx},  // the zone id (build_building slot_index)
                        {"key", z_key},
                        {"name", LocalizeKey(z_key)},
                        {"max_buildings", max_buildings},
                        {"buildings", bldgs}
                    });
                }

                districts.push_back({
                    {"type", d_key},
                    {"name", LocalizeKey(d_key)},
                    {"built", d_built},
                    {"zone_slots", d_zone_slots},
                    {"zones", zones}
                });
            }
        }
    }

    // 7. Dynamic Construction Queue
    nlohmann::json queue = ExtractPlanetConstructionQueue(planet_id);

    // 8. Monthly Production: the colony's cached economy tables (CColony +0xE80 produced,
    // +0xEA0 upkeep, +0xEC0 profits = produced - upkeep, as CColony::GetResourceProfits reads).
    auto& gs = GameState::Get();
    nlohmann::json monthly_production = {
        {"produced", gs.ResourceTableJson((const void*)((uintptr_t)colony_obj + 0xE80))},
        {"upkeep", gs.ResourceTableJson((const void*)((uintptr_t)colony_obj + 0xEA0))},
        {"net", gs.ResourceTableJson((const void*)((uintptr_t)colony_obj + 0xEC0))}
    };

    // 8. Dynamic Blockers
    nlohmann::json blockers = nlohmann::json::array();
    auto blockers_data = GetClearableBlockersJson(planet_id);
    if (blockers_data.contains("blockers") && blockers_data["blockers"].is_array()) {
        blockers = blockers_data["blockers"];
    }

    // 9. Status Alerts: what the game's outliner shows for this colony
    nlohmann::json alerts_json = nlohmann::json::array();
    for (const auto& al : ReadColonyStatus(colony_obj, p_obj, planet_id)) {
        alerts_json.push_back({ {"id", al.id}, {"name", al.name}, {"desc", al.desc} });
    }

    // 10. Planetary Features (Deposits & Blockers details)
    uint32_t queue_id = GetPlanetQueueId(planet_id);
    uint32_t country_id = GetPlayerCountryId();
    nlohmann::json planetary_features = ExtractPlanetaryFeatures(p_obj, cid, queue_id, country_id);

    // 11. Population Breakdown (现有人口)
    nlohmann::json population_breakdown = ExtractPopulationBreakdown(colony_obj);

    // 12. Monthly Population Summary (每月人口概要)
    nlohmann::json monthly_population_summary = ExtractMonthlyPopulationSummary(colony_obj);

    // 13. Colony Ascension (殖民地飞升)
    nlohmann::json colony_ascension = ExtractColonyAscension(colony_obj, cid);

    // 14. Workforce Summary (Layer 1 宏观岗位与阶层劳动力摘要)
    nlohmann::json workforce_summary = ExtractWorkforceSummary(colony_obj);

    // 15. Armies Summary (Layer 1 宏观陆军摘要)
    nlohmann::json armies_summary = ExtractArmiesSummary(p_obj, colony_obj);

    return {
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"name", p_name},
        {"system_id", sys_id},
        {"system_name", sys_name},
        {"is_capital", is_capital},
        {"designation", desig_name},
        {"designation_key", desig_key},
        {"designation_tier", desig_tier},
        {"governor", governor_json},
        {"overview", {
            {"planet_type", planet_type},
            {"habitability_percent", habitability_percent},
            {"habitability_species_id", founder},
            {"colony_date", colony_date},
            {"planet_size", planet_size}
        }},
        {"kpi", {
            {"stability_percent", stability_percent},
            {"pop_groups", pop_groups},
            {"pop_display", pop_display},
            {"pop_capacity", pop_capacity},
            {"crime_percent", crime_percent},
            {"housing", housing_val},
            {"amenities", amenities_val},
            {"amenities_display", amenities_display},
            {"unemployed", unemployed_val},
            {"growth", growth_val}
        }},
        {"districts", districts},
        {"monthly_production", monthly_production},
        {"construction_queue", queue},
        {"blockers", blockers},
        {"planetary_features", planetary_features},
        {"population_breakdown", population_breakdown},
        {"monthly_population_summary", monthly_population_summary},
        {"colony_ascension", colony_ascension},
        {"workforce_summary", workforce_summary},
        {"armies_summary", armies_summary},
        {"status_alerts", alerts_json}
    };
}

nlohmann::json OutlinerManager::ExtractPlanetConstructionQueue(uint32_t planet_id) {
    return ExtractConstructionQueue(GetPlanetQueueId(planet_id));
}

// A ship design by id (TPdxRef<CShipDesign>; the design's own id must match)
void* OutlinerManager::FindShipDesignObj(uint32_t design_id) {
    void* mgr = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (design_id == 0xFFFFFFFF || !SafeReadPtr((const void*)(base_address_ + sdk::db::CShipDesign), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || (design_id & 0xFFFFFF) >= cap) {
        return nullptr;
    }
    void* obj = nullptr;
    uint32_t id = 0xFFFFFFFF;
    return SafeReadPtr((const void*)((uintptr_t)slots + (design_id & 0xFFFFFF) * 16 + 8), &obj) && obj &&
           SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CShipDesign_id), &id) && id == design_id ? obj : nullptr;
}

// The items of one construction queue (a colony's, or a starbase's shipyard queue)
nlohmann::json OutlinerManager::ExtractConstructionQueue(uint32_t queue_id) {
    nlohmann::json queue = nlohmann::json::array();
    if (queue_id == 0xFFFFFFFF) return queue;

    void* mgr_eb8 = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8) || !mgr_eb8) return queue;
    void* arr_eb8 = nullptr;
    uint32_t cap_eb8 = 0;
    uint32_t q_slot = queue_id & 0xFFFFFF;
    if (!SafeReadPtr((const void*)((uintptr_t)mgr_eb8 + 0x18), &arr_eb8) || !arr_eb8 ||
        !SafeReadU32((const void*)((uintptr_t)mgr_eb8 + 0x20), &cap_eb8) || q_slot >= cap_eb8) return queue;
    void* queue_obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr_eb8 + q_slot * 16 + 8), &queue_obj) || !queue_obj) return queue;

    void* q_items = nullptr;
    uint32_t q_cnt = 0;
    uint32_t q_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)queue_obj + 0x20), &q_items);
    // +0x28 = **容量**、+0x2C = **条目数**（2026-11-18 01:50 由"UI 只有 1 条、我们读出 4 条"定案：
    // 船坞队列 +0x28=[4][1] → 容量 4、条数 1；空的建造队列 +0x28=[1][0] → 容量 1、条数 0）。
    // 早先把 +0x28 当条数，于是把容量里残留的旧 id 也列了出来——那三条同 id 的"黑鱼"就是这么来的。
    SafeReadU32((const void*)((uintptr_t)queue_obj + 0x28), &q_cap);
    SafeReadU32((const void*)((uintptr_t)queue_obj + 0x2C), &q_cnt);
    if (q_cnt > q_cap && q_cap > 0) q_cnt = q_cap;  // 兜底：别越界
    if (!q_items || q_cnt == 0 || q_cnt > 200) return queue;

    void* mgr_ea8 = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8) || !mgr_ea8) return queue;
    void* arr_ea8 = nullptr;
    uint32_t cap_ea8 = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr_ea8 + 0x18), &arr_ea8);
    SafeReadU32((const void*)((uintptr_t)mgr_ea8 + 0x20), &cap_ea8);
    if (!arr_ea8 || cap_ea8 == 0) return queue;

    for (uint32_t i = 0; i < q_cnt; ++i) {
        uint32_t item_id = 0;
        if (!SafeReadU32((const void*)((uintptr_t)q_items + i * sizeof(uint32_t)), &item_id)) continue;
        uint32_t i_slot = item_id & 0xFFFFFF;
        if (i_slot >= cap_ea8) continue;
        void* item_obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr_ea8 + i_slot * 16 + 8), &item_obj) || !item_obj) continue;

        // 条目自带它所属的队列（CConstructionQueueItem + 0x20）。取消后的条目要过一个 tick 才从
        // 队列的 id 列表里消失，而条目对象会立刻被回收给别的队列——不核对归属就会读到别的队列
        // 的造船条目，看起来像"这座基地多了一条船"（2026-11-18 01:10 踩到）。
        uint32_t item_queue = 0xFFFFFFFF;
        if (SafeReadU32((const void*)((uintptr_t)item_obj + sdk::ent::CConstructionQueueItem::queue), &item_queue) &&
            item_queue != 0xFFFFFFFF && item_queue != queue_id) {
            continue;
        }

        uint32_t prog = 0;
        uint32_t tot = 0;
        SafeReadU32((const void*)((uintptr_t)item_obj + 0x28), &prog);
        SafeReadU32((const void*)((uintptr_t)item_obj + 0x30), &tot);

        void* action_obj = nullptr;
        SafeReadPtr((const void*)((uintptr_t)item_obj + 0x18), &action_obj);
        std::string key;
        std::string item_type = "construction";
        std::string ship_name;
        if (action_obj) {
            void* act_vt = nullptr;
            SafeReadPtr(action_obj, &act_vt);
            // the buildable's token: its GetToken slot is `mov eax, TOKEN; ret`
            uint32_t token = 0;
            void* get_token = nullptr;
            uint8_t op = 0, tail = 0;
            if (act_vt && SafeReadPtr((const void*)((uintptr_t)act_vt + sdk::vt::CBuildableBase_GetToken * sizeof(void*)), &get_token) &&
                get_token && SafeReadU8(get_token, &op) && op == 0xB8 && SafeReadU8((const void*)((uintptr_t)get_token + 5), &tail) &&
                tail == 0xC3) {
                SafeReadU32((const void*)((uintptr_t)get_token + 1), &token);
            }
            const bool colony_ship = token == sdk::rt::Token_buildable_colony_ship;
            const bool ship = colony_ship || token == sdk::rt::Token_buildable_ship ||
                              token == sdk::rt::Token_buildable_federation_ship ||
                              token == sdk::rt::Token_buildable_galactic_community_ship;
            // 防御平台（0x3E03）内嵌同样的 CShipDesignImplementation：+8 起是 impl，impl 的 +0x530 是设计 id。
            // 2026-11-18 01:20 实测：监牢队列里那条的 +0x538 = 16778971，可用与舰船相同的方式查名。
            const bool defense_platform = token == 0x3E03;
            if (ship || defense_platform) {
                // a ship buildable embeds the design's implementation; its base design names the ship
                item_type = defense_platform ? "defense_platform" : (colony_ship ? "colony_ship" : "ship");
                uint32_t design_id = 0xFFFFFFFF;
                SafeReadU32((const void*)((uintptr_t)action_obj + sdk::rt::CBuildableShip_implementation +
                                          sdk::rt::CShipDesignImplementation_design), &design_id);
                key = std::to_string(design_id);
                if (void* design = FindShipDesignObj(design_id)) {
                    ship_name = PersistentNameText((const void*)((uintptr_t)design + sdk::ent::CShipDesign::name));
                    if (ship_name.empty()) {
                        // 没有持久名时退回设计模板名（名字键 + 本地化）。防御平台常走这条：
                        // 2026-11-18 01:25 建成的那座面板名是「自重之物II」，模板名「自重之物」。
                        std::string name_key;
                        if (SafeReadPdxString((const void*)((uintptr_t)design + sdk::ent::CShipDesign::name + 0x20),
                                              name_key) &&
                            !name_key.empty()) {
                            std::string localized = LocalizeKey(name_key);
                            if (!localized.empty() && localized != name_key) ship_name = localized;
                        }
                    }
                }
            } else if (act_vt == (void*)(base_address_ + kBuildableClearDepositBlockerVt)) {
                item_type = "clear_blocker";
                uint32_t dep_id = 0;
                SafeReadU32((const void*)((uintptr_t)action_obj + 8), &dep_id);
                void* d_mgr = nullptr;
                if (SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &d_mgr) && d_mgr) {
                    void* d_arr = nullptr;
                    uint32_t d_cap = 0;
                    SafeReadPtr((const void*)((uintptr_t)d_mgr + 0x18), &d_arr);
                    SafeReadU32((const void*)((uintptr_t)d_mgr + 0x20), &d_cap);
                    if (d_arr && dep_id < d_cap) {
                        void* d_obj = nullptr;
                        SafeReadPtr((const void*)((uintptr_t)d_arr + dep_id * 16 + 8), &d_obj);
                        if (d_obj) {
                            void* t_obj = nullptr;
                            SafeReadPtr((const void*)((uintptr_t)d_obj + 0x18), &t_obj);
                            if (t_obj) {
                                SafeReadPdxString((const void*)((uintptr_t)t_obj + 0x20), key);
                            }
                        }
                    }
                }
            } else {
                void* def_obj = nullptr;
                SafeReadPtr((const void*)((uintptr_t)action_obj + 8), &def_obj);
                if (def_obj) {
                    SafeReadPdxString((const void*)((uintptr_t)def_obj + 0x20), key);
                }
                if (key.rfind("district_", 0) == 0) item_type = "district";
                else if (key.rfind("building_", 0) == 0) item_type = "building";
                else if (key.rfind("zone_", 0) == 0) item_type = "zone_specialization";
                // 恒星基地的三类 buildable（token 见 CBuildableStarbase* 的 GetToken）
                else if (token == 0x3E01) item_type = "starbase_module";
                else if (token == 0x3E00) item_type = "starbase_building";
                else if (token == 0x3E02) item_type = "starbase_upgrade";
            }
        }

        uint32_t remaining = tot > prog ? tot - prog : 0;
        double percent = tot > 0 ? (double)prog / (double)tot * 100.0 : 0.0;

        uint32_t display_tot = (tot >= 100000) ? (tot / 100000) : tot;
        uint32_t display_prog = (tot >= 100000) ? (prog / 100000) : prog;
        uint32_t display_rem = (tot >= 100000) ? ((remaining + 99999) / 100000) : remaining;

        queue.push_back({
            {"item_id", item_id},
            {"key", key},
            {"item_name", !ship_name.empty() ? ship_name
                              : (item_type == "starbase_upgrade"
                                     ? key  // 升级的目标是等级键，交给插件翻成中文
                                     : (item_type.rfind("starbase_", 0) == 0 ? LocalizeStarbaseSm(key) : LocalizeKey(key)))},
            {"type", item_type},
            {"progress", display_prog},
            {"total_days", display_tot},
            {"remaining_days", display_rem},
            {"progress_percent", percent}
        });
    }

    return queue;
}

// CBuildableZone {vtable, CZoneType* +8, colony +0x10, district +0x14, zone slot +0x18}
// (CBuildableZone::CSerializer); CDistrict {id +8, CColony* +0x18, CDistrictType* +0x20, zones:
// CPdxArray of CZone ids, data +0x30, size +0x3C (CBuildableZone::CanBuild only builds into
// slot < size)}.
void OutlinerManager::FillZoneBuildable(uint8_t (&obj)[0x20], void* zone_type, uint32_t colony_id, uint32_t district_id,
                                        int32_t slot) {
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + sdk::vt::CBuildableZone);
    *(void**)(obj + 0x08) = zone_type;
    *(uint32_t*)(obj + 0x10) = colony_id;
    *(uint32_t*)(obj + 0x14) = district_id;
    *(int32_t*)(obj + 0x18) = slot;
}

std::vector<OutlinerManager::DistrictSlots> OutlinerManager::ColonyDistrictSlots(void* colony_obj) {
    constexpr std::ptrdiff_t kDistrictId = 0x8, kDistrictColony = 0x18, kDistrictType = 0x20;
    constexpr std::ptrdiff_t kDistrictZones = 0x30, kDistrictZonesSize = 0x3C;
    std::vector<DistrictSlots> out;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!colony_obj || !SafeReadPtr((const void*)(base_address_ + sdk::db::CDistrict), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap)) {
        return out;
    }
    for (uint32_t i = 0; i < cap; ++i) {
        void* d = nullptr;
        void* col = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &d) || !d ||
            !SafeReadPtr((const void*)((uintptr_t)d + kDistrictColony), &col) || col != colony_obj) {
            continue;
        }
        DistrictSlots ds{};
        ds.obj = d;
        SafeReadU32((const void*)((uintptr_t)d + kDistrictId), &ds.id);
        SafeReadU32((const void*)((uintptr_t)d + sdk::ent::CDistrict::level), &ds.level);
        void* type = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)d + kDistrictType), &type) && type) {
            SafeReadPdxString((const void*)((uintptr_t)type + 0x20), ds.type_key);
        }
        void* zones = nullptr;
        int32_t n = 0;
        if (SafeReadPtr((const void*)((uintptr_t)d + kDistrictZones), &zones) && zones &&
            SafeReadU32((const void*)((uintptr_t)d + kDistrictZonesSize), (uint32_t*)&n) && n > 0 && n < 32) {
            for (int32_t k = 0; k < n; ++k) {
                uint32_t zid = 0xFFFFFFFF;
                SafeReadU32((const void*)((uintptr_t)zones + k * 4), &zid);
                ds.zone_ids.push_back(zid);
            }
        }
        out.push_back(ds);
    }
    return out;
}

/* 按持有者列 deposit（行星特征）。关键：特征不是持有者对象上的字段 —— 它待在 CDeposit 表里，
   用 deposit_holder{type, id}（+0x28，8 字节）指回来。type 0/1 是行星一类，type 2 是舰 ——
   方舟（Nomads）的「珍宝馆」就是 d_treasure_planet，holder 指向方舟舰（2026-11-22 实测）。 */
nlohmann::json OutlinerManager::ProbeBuildableJson(uint32_t item_id, uint32_t words) {
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    if (words == 0 || words > 32) words = 12;
    void* mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || (item_id & 0xFFFFFF) >= cap) {
        return { {"success", false}, {"error", "queue item db not readable or item out of range"} };
    }
    void* item = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr + (item_id & 0xFFFFFF) * 16 + 8), &item) || !item) {
        return { {"success", false}, {"error", "queue item not found"} };
    }
    void* buildable = nullptr;
    SafeReadPtr((const void*)((uintptr_t)item + 0x18), &buildable);
    nlohmann::json out = { {"success", true}, {"item_id", item_id},
                           {"item_ptr", (unsigned long long)(uintptr_t)item},
                           {"buildable_ptr", (unsigned long long)(uintptr_t)buildable} };
    if (buildable) {
        nlohmann::json w = nlohmann::json::array();
        for (uint32_t k = 0; k < words; ++k) {
            uint32_t v = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)buildable + k * 4), &v);
            w.push_back(v);
        }
        out["words"] = w;
        uint64_t vt = 0;
        SafeReadI64((const void*)buildable, (int64_t*)&vt);
        out["vtable_rva"] = (unsigned long long)(vt - base_address_);
    }
    return out;
}

nlohmann::json OutlinerManager::ProbeDepositsJson(const std::string& key_filter, uint32_t holder_type,
                                                      uint32_t holder_id, uint32_t limit, uint32_t dump_words) {
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    if (limit == 0 || limit > 400) limit = 50;
    if (dump_words > 64) dump_words = 64;
    void* mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || cap > 200000) {
        return { {"success", false}, {"error", "CDeposit database not readable"} };
    }
    // 持有者实为 CDeposit 的 **+0x30**：低 4 字节是 type，高 4 字节是 id
    // （存档里的 deposit_holder={type, id} 就是这个；SDK 标的 +0x28 是个共享指针，不是它）。
    constexpr std::ptrdiff_t kHolderOff = 0x30;
    nlohmann::json out = nlohmann::json::array();
    uint32_t scanned = 0, matched = 0;
    for (uint32_t slot = 0; slot < cap && matched < limit; ++slot) {
        void* d = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &d) || !d) continue;
        ++scanned;
        void* t = nullptr;
        std::string key;
        if (SafeReadPtr((const void*)((uintptr_t)d + sdk::ent::CDeposit::type), &t) && t) {
            SafeReadPdxString((const void*)((uintptr_t)t + 0x20), key);
        }
        if (!key_filter.empty() && key.find(key_filter) == std::string::npos) continue;
        uint64_t holder = 0;
        SafeReadI64((const void*)((uintptr_t)d + kHolderOff), (int64_t*)&holder);
        uint32_t htype = (uint32_t)(holder & 0xFFFFFFFFull);
        uint32_t hid = (uint32_t)(holder >> 32);
        if (holder_id != 0xFFFFFFFF && (htype != holder_type || hid != holder_id)) continue;
        uint32_t did = 0;
        SafeReadU32((const void*)((uintptr_t)d + 0x8), &did);
        nlohmann::json j = { {"slot", slot}, {"id", did}, {"key", key}, {"name", LocalizeKey(key)},
                             {"holder_type", htype}, {"holder_id", hid},
                             {"ptr", (unsigned long long)(uintptr_t)d} };
        if (dump_words > 0) {
            nlohmann::json w = nlohmann::json::array();
            for (uint32_t k = 0; k < dump_words; ++k) {
                uint32_t v = 0xFFFFFFFF;
                SafeReadU32((const void*)((uintptr_t)d + k * 4), &v);
                w.push_back(v);
            }
            j["words"] = w;
        }
        out.push_back(j);
        ++matched;
    }
    return { {"success", true}, {"key_filter", key_filter}, {"holder_type", holder_type}, {"holder_id", holder_id},
             {"table_cap", cap}, {"scanned", scanned}, {"matched", matched}, {"deposits", out} };
}

/* 定时修正：CTimedModifierCollection 在载体上的 +0x280（数据 +0x288 / 容量 +0x290 / 条数 +0x294），
   条目步长 0x28：{ +0x08 modifier 指针 · +0x10 days · +0x20 clear_on_owner_change }。
   键名不在条目上，而在 modifier 类型对象 + 0x178 的 CPdxString 里（长度在 +0x188，<=15 是内联 SSO）。
   2026-11-23 由 CTimedModifier serializer 的 hook + 存档对账定下来的。 */
std::string OutlinerManager::TimedModifierKey(void* type_obj) {
    std::string out;
    if (!type_obj) return out;
    uint64_t size = 0;
    char buf[17] = { 0 };
    // 长度在 +0x188
    if (!SafeReadI64((const void*)((uintptr_t)type_obj + 0x188), (int64_t*)&size)) return out;
    if (size == 0 || size > 200) return out;
    if (size <= 15) {
        // 内联 SSO：16 字节直接是字符
        for (int i = 0; i < 16; ++i) {
            char ch = 0;
            if (!SafeReadU8((const void*)((uintptr_t)type_obj + 0x178 + i), (uint8_t*)&ch)) return out;
            buf[i] = ch;
        }
        out.assign(buf, (size_t)size);
        return out;
    }
    void* ptr = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)type_obj + 0x178), &ptr) || !ptr) return out;
    out.resize((size_t)size);
    if (!SafeCopyChars((char*)out.data(), (const char*)ptr, (size_t)size)) {
        out.clear();
        return out;
    }
    return out;
}

nlohmann::json OutlinerManager::ReadTimedModifiers(void* carrier, std::ptrdiff_t base) {
    nlohmann::json arr = nlohmann::json::array();
    if (!carrier) return arr;
    void* data = nullptr;
    uint32_t cap = 0, count = 0;
    SafeReadPtr((const void*)((uintptr_t)carrier + base + 0x08), &data);
    SafeReadU32((const void*)((uintptr_t)carrier + base + 0x10), &cap);
    SafeReadU32((const void*)((uintptr_t)carrier + base + 0x14), &count);
    if (!data || count == 0 || count > 200) return arr;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t item = (uintptr_t)data + (uintptr_t)i * 0x28;
        uint64_t type_ptr = 0;
        int32_t days = 0;
        SafeReadI64((const void*)(item + 0x08), (int64_t*)&type_ptr);
        SafeReadI32((const void*)(item + 0x10), &days);
        std::string key = TimedModifierKey((void*)(uintptr_t)type_ptr);
        arr.push_back({ {"key", key}, {"name", LocalizeKey(key)}, {"days", days},
                        {"clear_on_owner_change", [&] {
                             uint8_t v = 0;
                             SafeReadU8((const void*)(item + 0x20), &v);
                             return v != 0;
                         }()} });
    }
    return arr;
}

/* 宇宙风暴的读法：CCosmicStorm（+0x340 类型 · +0x2C 路径索引 · +0x3C activated · +0x348 path），
   CCosmicStormInfluenceField（+0x30 center · +0x38 radius · +0x58 自带 timed_modifier 容器）。
   「电力风暴」这类按“殖民地所在星系是否在风暴范围内”出现的修正，属于后者。 */
nlohmann::json OutlinerManager::StormFieldsAtSystem(uint32_t system_id) {
    nlohmann::json out = nlohmann::json::array();
    if (!base_address_ || system_id == 0xFFFFFFFF) return out;
    void* mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCosmicStormInfluenceField), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || cap >= 100000) {
        return out;
    }
    for (uint32_t slot = 0; slot < cap; ++slot) {
        void* f = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &f) || !f) continue;
        uint32_t center = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::center), &center);
        if (center != system_id) continue;
        uint32_t id = 0;
        uint64_t radius = 0;
        SafeReadU32((const void*)((uintptr_t)f + 0x10), &id);
        SafeReadI64((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::radius), (int64_t*)&radius);
        // 场自带的 timed_modifier（SDK 标 +0x58）：先按“解引用一层”读，同时把原始 8 个字带出去对照
        void* mod_owner = nullptr;
        SafeReadPtr((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::timed_modifier), &mod_owner);
        nlohmann::json mods = ReadTimedModifiers(mod_owner, 0x08);
        nlohmann::json raw = nlohmann::json::array();
        for (uint32_t k = 0; k < 8; ++k) {
            uint32_t v = 0;
            SafeReadU32((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::timed_modifier + k * 4), &v);
            raw.push_back(v);
        }
        out.push_back({ {"id", id}, {"radius_fixed", radius}, {"radius", (double)radius / 16777216.0},
                        {"mod_owner", (unsigned long long)(uintptr_t)mod_owner}, {"modifiers", mods}, {"raw", raw} });
    }
    return out;
}

nlohmann::json OutlinerManager::StormsInSystem(uint32_t system_id) {
    nlohmann::json out = nlohmann::json::array();
    if (!base_address_ || system_id == 0xFFFFFFFF) return out;
    void* mgr = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCosmicStorm), &mgr) || !mgr ||
        !SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) || cap >= 100000) {
        return out;
    }
    for (uint32_t slot = 0; slot < cap; ++slot) {
        void* st = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &st) || !st) continue;
        uint8_t activated = 0;
        SafeReadU8((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::activated), &activated);
        if (!activated) continue;
        int32_t path_index = 0;
        SafeReadI32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::storm_current_path_index), &path_index);
        void* path_data = nullptr;
        uint32_t path_count = 0;
        SafeReadPtr((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::path + 0x08), &path_data);
        SafeReadU32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::path + 0x14), &path_count);
        uint32_t cur = 0xFFFFFFFF;
        if (path_data && path_count > 0 && path_index >= 0 && (uint32_t)path_index < path_count) {
            SafeReadU32((const void*)((uintptr_t)path_data + (uint32_t)path_index * 4), &cur);
        }
        if (cur != system_id) continue;
        std::string type_key;
        void* type = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::type), &type) && type) {
            SafeReadPdxString((const void*)((uintptr_t)type + 0x20), type_key);
        }
        int32_t days_next = 0;
        SafeReadI32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::storm_days_till_next_system), &days_next);
        out.push_back({ {"type", type_key}, {"name", PersistentNameText((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::name))},
                        {"days_till_next", days_next} });
    }
    return out;
}

nlohmann::json OutlinerManager::ProbeStormsJson() {
    nlohmann::json out = { {"success", true} };
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };

    // 1) 风暴本体
    nlohmann::json storms = nlohmann::json::array();
    {
        void* mgr = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CCosmicStorm), &mgr) && mgr &&
            SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) && cap < 100000) {
            for (uint32_t slot = 0; slot < cap; ++slot) {
                void* st = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &st) || !st) continue;
                uint32_t id = 0;
                SafeReadU32((const void*)((uintptr_t)st + 0x20), &id);
                int32_t path_index = 0, age = 0, days_next = 0;
                uint8_t activated = 0;
                SafeReadI32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::storm_current_path_index), &path_index);
                SafeReadI32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::storm_age), &age);
                SafeReadI32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::storm_days_till_next_system), &days_next);
                SafeReadU8((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::activated), &activated);
                std::string type_key;
                void* type = nullptr;
                if (SafeReadPtr((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::type), &type) && type) {
                    SafeReadPdxString((const void*)((uintptr_t)type + 0x20), type_key);
                }
                std::string nm = PersistentNameText((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::name));
                uint32_t cur_sys = 0xFFFFFFFF;
                void* path_data = nullptr;
                uint32_t path_count = 0;
                SafeReadPtr((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::path + 0x08), &path_data);
                SafeReadU32((const void*)((uintptr_t)st + sdk::ent::CCosmicStorm::path + 0x14), &path_count);
                if (path_data && path_count > 0 && path_index >= 0 && (uint32_t)path_index < path_count) {
                    SafeReadU32((const void*)((uintptr_t)path_data + (uint32_t)path_index * 4), &cur_sys);
                }
                storms.push_back({ {"id", id}, {"type", type_key}, {"name", nm},
                                   {"activated", activated != 0}, {"path_index", path_index},
                                   {"path_count", path_count}, {"current_system", cur_sys},
                                   {"current_system_name", cur_sys != 0xFFFFFFFF ? SystemName(cur_sys) : std::string()},
                                   {"age", age}, {"days_till_next", days_next} });
            }
        }
    }
    out["storms"] = storms;

    // 2) 影响场
    nlohmann::json fields = nlohmann::json::array();
    {
        void* mgr = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CCosmicStormInfluenceField), &mgr) && mgr &&
            SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap) && cap < 100000) {
            for (uint32_t slot = 0; slot < cap; ++slot) {
                void* f = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &f) || !f) continue;
                uint32_t id = 0, center = 0xFFFFFFFF;
                uint64_t radius = 0;
                SafeReadU32((const void*)((uintptr_t)f + 0x10), &id);
                SafeReadU32((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::center), &center);
                SafeReadI64((const void*)((uintptr_t)f + sdk::ent::CCosmicStormInfluenceField::radius), (int64_t*)&radius);
                nlohmann::json mods = ReadTimedModifiers(f, sdk::ent::CCosmicStormInfluenceField::timed_modifier);
                fields.push_back({ {"id", id}, {"center", center},
                                   {"center_name", center != 0xFFFFFFFF ? SystemName(center) : std::string()},
                                   {"radius", radius}, {"modifiers", mods} });
            }
        }
    }
    out["fields"] = fields;
    return out;
}

/* 一个殖民地的建筑树，入口是 CColony —— 方舟（Nomads）没有 CPlanet，但 colony 是同一个对象
   （行星：CPlanet::colony +0xE0；方舟：CShip::colony +0x100）。所以下面这条链两边通用：

     colony.districts(0x80) → CDistrict{ type 0x20, zones 0x30/0x3C, level 0x1B0 }
       → CZone{ type 0x20, buildings 0x1A0 } → CBuilding{ type 0x20 }

   注意 SDK 里 CDistrict::zones = 0x3C 与 CColony::districts = 0x94 标的都是**条数**的位置，
   结构起点要减 0x14（2026-11-22 实测，见桥 README「方舟」一节）。 */
nlohmann::json OutlinerManager::ProbeColonyStructuresJson(uint32_t colony_id) {
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    void* colony = FindColony(colony_id);
    if (!colony) return { {"success", false}, {"error", "Unknown colony id: " + std::to_string(colony_id)} };

    auto read_u32 = [&](std::ptrdiff_t off) {
        uint32_t v = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)colony + off), &v);
        return v;
    };

    uint64_t carrier = 0;
    SafeReadI64((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), (int64_t*)&carrier);
    uint32_t carrier_full = (uint32_t)(carrier & 0xFFFFFFFFull);   // 完整 id（船 id 会超过 24 位）
    uint32_t carrier_id = (uint32_t)(carrier & 0xFFFFFFull);       // 查表用的槽号
    uint32_t carrier_type = (uint32_t)(carrier >> 32);

    nlohmann::json j = { {"success", true}, {"colony_id", colony_id},
                         {"carrier_id", carrier_id}, {"carrier_type", carrier_type},
                         {"pop_groups", read_u32(0x18 + 0x14)},
                         {"pop_jobs", read_u32(0x30 + 0x14)},
                         {"district_array", read_u32(0x80 + 0x14)},
                         {"num_sapient_pops", read_u32(0xFC8)},
                         {"stability_fixed", read_u32(0xFB8)},
                         {"ascension_tier", read_u32(0x110)} };

    // 区划类型库：只为了把 type 指针换成键名（CDistrictType 的键在 +0x20）
    void* zmgr = nullptr;
    void* zarr = nullptr;
    uint32_t zcap = 0;
    const bool zones_ok = SafeReadPtr((const void*)(base_address_ + sdk::db::CZone), &zmgr) && zmgr &&
                          SafeReadPtr((const void*)((uintptr_t)zmgr + 0x18), &zarr) && zarr &&
                          SafeReadU32((const void*)((uintptr_t)zmgr + 0x20), &zcap) && zcap < 100000;
    void* bmgr = nullptr;
    void* barr = nullptr;
    uint32_t bcap = 0;
    const bool blds_ok = SafeReadPtr((const void*)(base_address_ + sdk::db::CBuilding), &bmgr) && bmgr &&
                         SafeReadPtr((const void*)((uintptr_t)bmgr + 0x18), &barr) && barr &&
                         SafeReadU32((const void*)((uintptr_t)bmgr + 0x20), &bcap) && bcap < 200000;

    nlohmann::json districts = nlohmann::json::array();
    for (const auto& d : ColonyDistrictSlots(colony)) {
        nlohmann::json dj = { {"district_id", d.id}, {"type", d.type_key},
                              {"name", LocalizeKey(d.type_key)}, {"level", d.level} };
        nlohmann::json slots = nlohmann::json::array();
        for (size_t k = 0; k < d.zone_ids.size(); ++k) {
            uint32_t zid = d.zone_ids[k];
            nlohmann::json sj = { {"slot", (uint32_t)k}, {"zone_id", zid} };
            void* zone = nullptr;
            if (zones_ok && zid != 0xFFFFFFFF && (zid & 0xFFFFFF) < zcap) {
                SafeReadPtr((const void*)((uintptr_t)zarr + (zid & 0xFFFFFF) * 16 + 8), &zone);
            }
            if (zone) {
                void* ztype = nullptr;
                std::string zkey;
                if (SafeReadPtr((const void*)((uintptr_t)zone + sdk::ent::CZone::type), &ztype) && ztype) {
                    SafeReadPdxString((const void*)((uintptr_t)ztype + 0x20), zkey);
                }
                sj["zone"] = zkey;
                sj["zone_name"] = LocalizeKey(zkey);
                // CZone::buildings +0x1A0：数据 +0x1A8、条数 +0x1B4、元素 4 字节
                void* bdata = nullptr;
                uint32_t bcount = 0;
                if (SafeReadPtr((const void*)((uintptr_t)zone + sdk::ent::CZone::buildings + 0x8), &bdata) && bdata &&
                    SafeReadU32((const void*)((uintptr_t)zone + sdk::ent::CZone::buildings + 0x14), &bcount) &&
                    bcount < 64) {
                    nlohmann::json bl = nlohmann::json::array();
                    for (uint32_t bi = 0; bi < bcount; ++bi) {
                        uint32_t bid = 0xFFFFFFFF;
                        if (!SafeReadU32((const void*)((uintptr_t)bdata + bi * 4), &bid) || bid == 0xFFFFFFFF) continue;
                        nlohmann::json bj = { {"building_id", bid} };
                        void* bobj = nullptr;
                        if (blds_ok && (bid & 0xFFFFFF) < bcap) {
                            SafeReadPtr((const void*)((uintptr_t)barr + (bid & 0xFFFFFF) * 16 + 8), &bobj);
                        }
                        if (bobj) {
                            void* btype = nullptr;
                            std::string bkey;
                            if (SafeReadPtr((const void*)((uintptr_t)bobj + sdk::ent::CBuilding::type), &btype) && btype) {
                                SafeReadPdxString((const void*)((uintptr_t)btype + 0x20), bkey);
                            }
                            bj["key"] = bkey;
                            bj["name"] = LocalizeKey(bkey);
                        }
                        bl.push_back(bj);
                    }
                    sj["buildings"] = bl;
                }
            }
            slots.push_back(sj);
        }
        dj["slots"] = slots;
        districts.push_back(dj);
    }
    j["districts"] = districts;

    // 建造队列：方舟与行星共用 CConstructionQueue，通过 carrier 取队列 id（CShip +0x104 / CPlanet +0xE4）。
    // 明细用现成的 ExtractConstructionQueue —— 与行星详情、恒星基地队列同一套（含 item_id，取消要用）。
    {
        uint32_t qid = GetColonyQueueId(colony_id);
        j["queue_id"] = qid;
        nlohmann::json qitems = (qid == 0xFFFFFFFF) ? nlohmann::json::array() : ExtractConstructionQueue(qid);
        j["queue_items"] = (uint32_t)qitems.size();
        j["queue"] = qitems;
    }

    // 定时修正：行星走 CPlanet、方舟走 CShip，两者容器都在载体 +0x280
    {
        void* carrier_obj = nullptr;
        if (carrier_type == 0) {
            void* pmgr = nullptr;
            void* parr = nullptr;
            uint32_t pcap = 0;
            if (SafeReadPtr((const void*)(base_address_ + sdk::db::CPlanet), &pmgr) && pmgr &&
                SafeReadPtr((const void*)((uintptr_t)pmgr + 0x18), &parr) && parr &&
                SafeReadU32((const void*)((uintptr_t)pmgr + 0x20), &pcap) && (carrier_id & 0xFFFFFF) < pcap) {
                SafeReadPtr((const void*)((uintptr_t)parr + (carrier_id & 0xFFFFFF) * 16 + 8), &carrier_obj);
            }
        } else {
            void* smgr = nullptr;
            void* sarr = nullptr;
            uint32_t scap = 0;
            if (SafeReadPtr((const void*)(base_address_ + sdk::db::CShip), &smgr) && smgr &&
                SafeReadPtr((const void*)((uintptr_t)smgr + 0x18), &sarr) && sarr &&
                SafeReadU32((const void*)((uintptr_t)smgr + 0x20), &scap) && (carrier_id & 0xFFFFFF) < scap) {
                SafeReadPtr((const void*)((uintptr_t)sarr + (carrier_id & 0xFFFFFF) * 16 + 8), &carrier_obj);
            }
        }
        // 容器位置分两类：行星 CPlanet +0x280、方舟 CShip +0x2A0（2026-11-23 实测）
        nlohmann::json mods = ReadTimedModifiers(carrier_obj, carrier_type == 0 ? 0x280 : 0x2A0);
        j["modifiers"] = mods;
        j["modifier_count"] = (uint32_t)mods.size();

        // 所在星系 + 该星系正在刮的风暴（「电力风暴」这类修正由风暴决定，不存在 timed_modifier 里）
        uint32_t sys_id = 0xFFFFFFFF;
        if (carrier_obj) {
            if (carrier_type == 0) {
                void* planet_obj = nullptr;
                void* pmgr = nullptr;
                void* parr = nullptr;
                uint32_t pcap = 0;
                if (SafeReadPtr((const void*)(base_address_ + sdk::db::CPlanet), &pmgr) && pmgr &&
                    SafeReadPtr((const void*)((uintptr_t)pmgr + 0x18), &parr) && parr &&
                    SafeReadU32((const void*)((uintptr_t)pmgr + 0x20), &pcap) && (carrier_id & 0xFFFFFF) < pcap) {
                    SafeReadPtr((const void*)((uintptr_t)parr + (carrier_id & 0xFFFFFF) * 16 + 8), &planet_obj);
                }
                if (planet_obj) sys_id = PlanetSystemId(planet_obj);
            } else {
                SafeReadU32((const void*)((uintptr_t)carrier_obj + 0x9C0 + sdk::ent::CCelestialCoordinate::origin), &sys_id);
            }
        }
        j["system_id"] = sys_id;
        if (sys_id != 0xFFFFFFFF) {
            j["system_name"] = SystemName(sys_id);
            nlohmann::json st = StormsInSystem(sys_id);
            j["storms"] = st;
            j["storm_count"] = (uint32_t)st.size();
            nlohmann::json fld = StormFieldsAtSystem(sys_id);
            j["storm_fields"] = fld;
            j["storm_field_count"] = (uint32_t)fld.size();
        } else {
            j["storms"] = nlohmann::json::array();
            j["storm_count"] = 0;
        }
    }

    // 特征（deposit）：特征不在持有者对象上，而在 CDeposit 表里用 +0x30 的 {type, id} 指回来。
    // 方舟的持有者是舰（type 2），行星的按 planet id —— 这里一律只按 id 比，两类都覆盖。
    {
        void* dmgr = nullptr;
        void* darr = nullptr;
        uint32_t dcap = 0;
        nlohmann::json features = nlohmann::json::array();
        uint32_t holder_type_seen = 0xFFFFFFFF;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &dmgr) && dmgr &&
            SafeReadPtr((const void*)((uintptr_t)dmgr + 0x18), &darr) && darr &&
            SafeReadU32((const void*)((uintptr_t)dmgr + 0x20), &dcap) && dcap <= 200000) {
            for (uint32_t slot = 0; slot < dcap; ++slot) {
                void* dep = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)darr + slot * 16 + 8), &dep) || !dep) continue;
                uint64_t holder = 0;
                if (!SafeReadI64((const void*)((uintptr_t)dep + 0x30), (int64_t*)&holder)) continue;
                uint32_t htype = (uint32_t)(holder & 0xFFFFFFFFull);
                uint32_t hid = (uint32_t)(holder >> 32);
                if (hid != carrier_full) continue;
                // 还要比类型：方舟是船（type 2），行星那类是 0/1。只比 id 会撞 —— 方舟 0 的 id
                // 就是 0，会把行星 0 的 deposit 也算进来（实测多出一处）。
                if (carrier_type == 1 && htype != 2) continue;
                if (carrier_type == 0 && htype == 2) continue;
                holder_type_seen = htype;
                void* t = nullptr;
                std::string key;
                if (SafeReadPtr((const void*)((uintptr_t)dep + sdk::ent::CDeposit::type), &t) && t) {
                    SafeReadPdxString((const void*)((uintptr_t)t + 0x20), key);
                }
                uint32_t flags = 0;
                SafeReadU32((const void*)((uintptr_t)dep + sdk::ent::CDeposit::flags), &flags);
                features.push_back({ {"slot", slot}, {"key", key}, {"name", LocalizeKey(key)}, {"flags", flags} });
            }
        }
        j["features"] = features;
        j["features_count"] = (uint32_t)features.size();
        if (holder_type_seen != 0xFFFFFFFF) j["feature_holder_type"] = holder_type_seen;
    }

    return j;
}

/* 扫 CShip 表找船（防御平台就是 CShip）。只读两个字段：id@+8、设计号@+0x988、名字@+0x110。
   设计号过滤：design_id >= 0 时只返回该设计；-1 表示全列。扫描在桥内做（SafeRead），
   避免脚本裸扫把 IPC 管道搞挂（2026-11-18 01:30 的事故）。 */
nlohmann::json OutlinerManager::FindShipsJson(int design_id, uint32_t limit, int system_id_filter) {
    if (!base_address_) return { {"success", false}, {"error", "Engine functions not initialized"} };
    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CShip), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CShip database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);
    if (!arr) return { {"success", false}, {"error", "CShip table unreadable"} };
    if (limit == 0 || limit > 2000) limit = 2000;

    nlohmann::json ships = nlohmann::json::array();
    for (uint32_t i = 0; i < cap && ships.size() < limit; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;

        uint32_t ship_id = 0;
        uint32_t ship_design = 0;
        // 2026-11-22 修正：CShip 的 id 在 +0x18（sdk::rt::CShip_id），旧代码读 +8 一律得 0
        SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CShip_id), &ship_id);
        SafeReadU32((const void*)((uintptr_t)obj + 0x988), &ship_design);
        if (design_id >= 0 && (int)ship_design != design_id) continue;

        std::string name = PersistentNameText((const void*)((uintptr_t)obj + sdk::ent::CShip::name));
        // 所属星系：坐标结构在 CShip +0x9C0，origin 在 +0x20（= CPlanetSystemId 用的同一条公式）。
        // 2026-11-18 02:20 由「监牢两座=106、格索拉那座=91」反查确认。
        uint32_t system_id = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)obj + 0x9C0 + sdk::ent::CCelestialCoordinate::origin), &system_id);
        if (system_id_filter >= 0 && (int)system_id != system_id_filter) continue;
        // 站内编号（防御平台在所属空间站上的序号，1 起）
        uint32_t station_ordinal = 0;
        SafeReadU32((const void*)((uintptr_t)obj + 0x9A0), &station_ordinal);
        // 可能挂靠的字段先原样带出来，等对比时再认：+0x550 是 orbitable 相关的那个位置（序列化器注释）
        void* p550 = nullptr;
        void* p458 = nullptr;
        SafeReadPtr((const void*)((uintptr_t)obj + 0x550), &p550);
        SafeReadPtr((const void*)((uintptr_t)obj + sdk::ent::CShip::ship_design_implementation), &p458);

        ships.push_back({ {"slot", i},
                          {"ship_id", ship_id},
                          {"design_id", ship_design},
                          {"system_id", system_id},
                          {"station_ordinal", station_ordinal},
                          {"name", name},
                          {"ptr", "0x" + [&] { std::ostringstream o; o << std::hex << (uintptr_t)obj; return o.str(); }()},
                          {"f550", (uintptr_t)p550},
                          {"impl_ptr", (uintptr_t)p458} });
    }
    return { {"success", true}, {"count", ships.size()}, {"ships", ships} };
}

/* 恒星基地的建造队列。队列 id 在 CStarbase +0x880（与 QueueBuildable 用的同一个），
   队列本体的解码交给 ExtractConstructionQueue——它只认 queue_id，殖民地/基地/船坞通用。 */
nlohmann::json OutlinerManager::GetStarbaseQueueJson(uint32_t starbase_id) {
    if (!base_address_) return { {"success", false}, {"error", "Engine functions not initialized"} };

    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    // 两条队列：建造（+0x880）与船坞（+0x884）。造船订单在后者里，以前完全读不到。
    // 方舟是例外：等级键里带 arkship，它只有一条队列，在载体舰 CShip+0x104 上（与殖民地、陆军共用）。
    uint32_t build_queue = 0xFFFFFFFF;
    uint32_t shipyard_queue = 0xFFFFFFFF;
    uint32_t ark_queue = 0xFFFFFFFF;
    uint32_t ark_module_queue = 0xFFFFFFFF;
    const std::string level_key = StarbaseLevelKey(base_address_, starbase);
    const bool is_ark = IsArkshipLevel(level_key);
    if (is_ark) {
        // 方舟上其实是**两条**：载体舰 CShip+0x104（殖民地建筑 / 陆军 / 方舟升级）与
        // CStarbase+0x880（桩基模块，实测投模块落在这里）。2026-11-23 用 get_queue 分别读出：
        // 6829 里有升级与建筑、6828 里有刚投的 arkship_docking_bay。
        ark_queue = ArkBuildQueue(base_address_, starbase);
        SafeReadU32((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::build_queue), &ark_module_queue);
        if (ark_queue == 0xFFFFFFFF && ark_module_queue == 0xFFFFFFFF) {
            return { {"success", false}, {"error", "arkship has no build queue"} };
        }
    } else {
        SafeReadU32((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::build_queue), &build_queue);
        SafeReadU32((const void*)((uintptr_t)starbase + 0x884), &shipyard_queue);
        if (build_queue == 0xFFFFFFFF && shipyard_queue == 0xFFFFFFFF) {
            return { {"success", false}, {"error", "starbase has no build queue"} };
        }
    }

    // 并行建造数在队列对象的 +0x48（CConstructionQueue::simultaneous）。船坞模块越多它越大：
    // 2026-11-18 01:55 实测，监牢从 1 个船坞加到 3 个，这个字段从 1 变 3。
    auto simultaneousOf = [&](uint32_t qid) -> int {
        void* mgr = nullptr;
        if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr) || !mgr) return -1;
        void* qarr = nullptr;
        uint32_t qcap = 0;
        SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &qarr);
        SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &qcap);
        const uint32_t slot = qid & 0xFFFFFF;
        if (!qarr || slot >= qcap) return -1;
        void* qobj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)qarr + slot * 16 + 8), &qobj) || !qobj) return -1;
        uint32_t n = 0;
        SafeReadU32((const void*)((uintptr_t)qobj + 0x48), &n);
        return (int)n;
    };

    auto block = [&](uint32_t qid) -> nlohmann::json {
        nlohmann::json items = ExtractConstructionQueue(qid);
        return { {"queue_id", qid}, {"simultaneous", simultaneousOf(qid)},
                 {"count", items.size()}, {"items", items} };
    };

    nlohmann::json build = build_queue == 0xFFFFFFFF ? nlohmann::json::object() : block(build_queue);
    nlohmann::json shipyard = shipyard_queue == 0xFFFFFFFF ? nlohmann::json::object() : block(shipyard_queue);
    nlohmann::json ark = ark_queue == 0xFFFFFFFF ? nlohmann::json::object() : block(ark_queue);
    nlohmann::json ark_module =
        ark_module_queue == 0xFFFFFFFF ? nlohmann::json::object() : block(ark_module_queue);

    nlohmann::json out = { {"success", true}, {"starbase_id", starbase_id}, {"level", level_key},
                           {"is_arkship", is_ark}, {"ark", ark}, {"ark_module", ark_module},
                           {"build", build}, {"shipyard", shipyard} };
    // 兼容旧字段：方舟指载体那条（殖民地/陆军/升级），模块单列在 ark_module
    const nlohmann::json prim = is_ark ? ark : build;
    out["queue_id"] = is_ark ? ark_queue : build_queue;
    if (prim.contains("count")) {
        out["count"] = prim["count"];
        out["items"] = prim["items"];
    } else {
        out["count"] = 0;
        out["items"] = nlohmann::json::array();
    }
    return out;
}

/* 诊断用：按 id 直读一条建造队列（方舟上 module 与 colony 的队列不是同一条，排查时靠它）。 */
nlohmann::json OutlinerManager::GetQueueJson(uint32_t queue_id) {
    if (!base_address_) return { {"success", false}, {"error", "Engine functions not initialized"} };
    if (queue_id == 0xFFFFFFFF) return { {"success", false}, {"error", "queue_id is required"} };
    nlohmann::json items = ExtractConstructionQueue(queue_id);
    int sim = -1;
    void* mgr = nullptr;
    if (SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr) && mgr) {
        void* qarr = nullptr;
        uint32_t qcap = 0;
        SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &qarr);
        SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &qcap);
        const uint32_t slot = queue_id & 0xFFFFFF;
        if (qarr && slot < qcap) {
            void* qobj = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)qarr + slot * 16 + 8), &qobj) && qobj) {
                uint32_t n = 0;
                SafeReadU32((const void*)((uintptr_t)qobj + 0x48), &n);
                sim = (int)n;
            }
        }
    }
    return { {"success", true}, {"queue_id", queue_id}, {"simultaneous", sim},
             {"count", items.size()}, {"items", items} };
}

/* 模块/建筑的名字：sm_ 前缀 + 格式塔变体（_machine_intelligence/_hive_mind → _gestalt → 通用） */
std::string OutlinerManager::LocalizeStarbaseSm(const std::string& key) {
    std::string variant;
    {
        std::string authority = GovernmentManager::Get().GetGovernmentState().authority;
        if (authority == "auth_machine_intelligence") variant = "_machine_intelligence";
        else if (authority == "auth_hive_mind") variant = "_hive_mind";
    }
    if (!variant.empty()) {
        const std::string vk = "sm_" + key + variant;
        std::string text = LocalizeKey(vk);
        if (!text.empty() && text != vk) return text;
        const std::string gk = "sm_" + key + "_gestalt";
        std::string gestalt = LocalizeKey(gk);
        if (!gestalt.empty() && gestalt != gk) return gestalt;
    }
    return LocalizeKey("sm_" + key);
}

nlohmann::json OutlinerManager::GetAvailableDistrictZonesJson(uint32_t planet_id, const std::string& district_type,
                                                              bool include_blocked) {
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "Planet not found: " + std::to_string(planet_id)} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + sdk::ent::CPlanet::colony), &cid);
    void* colony_obj = cid != 0xFFFFFFFF ? FindColony(cid) : nullptr;
    if (!colony_obj) return { {"success", false}, {"error", "Planet has no colony"} };
    uint32_t queue_id = GetPlanetQueueId(planet_id);
    uint32_t country_id = GetPlayerCountryId();

    void* zdb = nullptr;
    void* zarr = nullptr;
    uint32_t zn = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CZoneTypeDatabase_pInstance), &zdb) || !zdb ||
        !SafeReadPtr((const void*)((uintptr_t)zdb + 0x50), &zarr) || !zarr ||
        !SafeReadU32((const void*)((uintptr_t)zdb + 0x5C), &zn) || zn > 5000) {
        return { {"success", false}, {"error", "Zone type database not readable"} };
    }

    nlohmann::json districts = nlohmann::json::array();
    for (const auto& ds : ColonyDistrictSlots(colony_obj)) {
        if (ds.zone_ids.empty()) continue;  // districts without zone slots
        if (!district_type.empty() && ds.type_key != district_type && ds.type_key != "district_" + district_type) continue;
        nlohmann::json slots = nlohmann::json::array();
        for (int32_t slot = 0; slot < (int32_t)ds.zone_ids.size(); ++slot) {
            nlohmann::json sj = { {"slot", slot} };
            uint32_t zid = ds.zone_ids[slot];
            void* zone = nullptr;
            if (zid != 0xFFFFFFFF) {
                void* zmgr = nullptr;
                void* zarr2 = nullptr;
                if (SafeReadPtr((const void*)(base_address_ + sdk::db::CZone), &zmgr) && zmgr &&
                    SafeReadPtr((const void*)((uintptr_t)zmgr + 0x18), &zarr2) && zarr2) {
                    SafeReadPtr((const void*)((uintptr_t)zarr2 + (zid & 0xFFFFFF) * 16 + 8), &zone);
                }
            }
            if (zone) {
                std::string cur;
                void* ztype = nullptr;
                if (SafeReadPtr((const void*)((uintptr_t)zone + sdk::ent::CZone::type), &ztype) && ztype) {
                    SafeReadPdxString((const void*)((uintptr_t)ztype + 0x20), cur);
                }
                sj["zone_id"] = zid;
                sj["zone"] = cur;
                sj["zone_name"] = LocalizeKey(cur);
            } else {
                sj["zone"] = nullptr;
            }
            nlohmann::json buildable = nlohmann::json::array();
            nlohmann::json blocked = nlohmann::json::array();
            for (uint32_t i = 0; i < zn; ++i) {
                void* t = nullptr;
                std::string key;
                if (!SafeReadPtr((const void*)((uintptr_t)zarr + i * 8), &t) || !t ||
                    !SafeReadPdxString((const void*)((uintptr_t)t + 0x20), key) || key.empty()) {
                    continue;
                }
                uint8_t obj[0x20];
                FillZoneBuildable(obj, t, cid, ds.id, slot);
                std::string why;
                if (QueueBuildable(obj, sizeof(obj), country_id, queue_id, false, &why)) {
                    nlohmann::json bj = { {"zone", key}, {"name", LocalizeKey(key)} };
                    bj.update(BuildableCostJson(obj));
                    buildable.push_back(bj);
                } else if (include_blocked) {
                    blocked.push_back({ {"zone", key}, {"reason", why} });
                }
            }
            sj["buildable_zones"] = buildable;
            if (include_blocked) sj["blocked_zones"] = blocked;
            slots.push_back(sj);
        }
        districts.push_back({ {"district_id", ds.id}, {"district_type", ds.type_key},
                              {"district_name", LocalizeKey(ds.type_key)}, {"slots", slots} });
    }
    return { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid}, {"districts", districts} };
}


/* 限时扫描全堆，找 CColonyType 对象（共同 vtable，键名内联在 +0x20）。
   返回 {key, object} 列表；vtable 从殖民地当前设计指针读。 */
nlohmann::json OutlinerManager::ScanDesignationObjects(uint32_t planet_id, int budget_ms, const std::string& only_key) {
    if (!base_address_) return { {"error", "not initialized"} };

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"error", "planet not found"} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"error", "planet has no colony"} };
    void* colony_obj = FindColony(cid);
    if (!colony_obj) return { {"error", "colony not found"} };

    void* current = nullptr;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x100), &current);
    if (!current) return { {"error", "no designation pointer"} };

    uint64_t vtable = 0;
    if (!SafeReadI64((const void*)current, (int64_t*)&vtable) || (vtable >> 32) != 0x7ff6ull) {
        return { {"error", "cannot read vtable"} };
    }

    auto valid_key = [](const std::string& t) -> bool {
        if (t.size() < 3 || t.size() > 48) return false;
        for (char c : t) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        }
        return true;
    };
    auto read_cstr = [&](uintptr_t addr, std::string& out) -> bool {
        char buf[64] = {0};
        for (int k = 0; k < 7; ++k) {
            int64_t chunk = 0;
            if (!SafeReadI64((const void*)(addr + k * 8), &chunk)) return false;
            std::memcpy(buf + k * 8, &chunk, 8);
        }
        buf[63] = 0;
        std::string t(buf);
        if (!valid_key(t)) return false;
        out = t;
        return true;
    };
    auto read_key = [&](uintptr_t obj, std::string& out) -> bool {
        if (read_cstr(obj + 0x20, out)) return true;
        int64_t ptr = 0;
        if (!SafeReadI64((const void*)(obj + 0x20), &ptr)) return false;
        if (ptr < 0x10000 || (uint64_t)ptr > 0x7fffffffffffull) return false;
        return read_cstr((uintptr_t)ptr, out);
    };

    nlohmann::json list = nlohmann::json::array();
    std::set<std::string> seen;
    std::set<uintptr_t> seen_obj;
    const DWORD started = GetTickCount();

    /* 先扫锚点附近（对象池通常和当前设计对象在同一片堆区） */
    {
        const uintptr_t a = (uintptr_t)current;
        const uintptr_t win = 16ull << 20;   // ±16 MB
        const uintptr_t from = a > win ? a - win : 0x10000;
        const size_t cs = 0x1000;
        std::vector<uint8_t> chunkbuf(cs);
        for (uintptr_t base = from; base < a + win; base += cs) {
            if (GetTickCount() - started > (DWORD)(budget_ms * 2 / 3)) break;
            size_t got = 0;
            while (got + 8 <= cs) {
                int64_t q = 0;
                if (!SafeReadI64((const void*)(base + got), &q)) break;
                std::memcpy(chunkbuf.data() + got, &q, 8);
                got += 8;
            }
            for (size_t i = 0; i + 8 <= got; i += 8) {
                uint64_t v = 0;
                std::memcpy(&v, chunkbuf.data() + i, 8);
                if (v != vtable) continue;
                const uintptr_t obj = base + i;
                if (!seen_obj.insert(obj).second) continue;
                std::string key;
                if (!read_key(obj, key)) continue;
                if (!seen.insert(key).second) continue;
                char hex[24];
                std::snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)obj);
                list.push_back({ {"key", key}, {"object", hex} });
                if (!only_key.empty() && key == only_key) {
                    return { {"success", true}, {"found", list.size()}, {"designations", list}, {"source", "window"} };
                }
            }
        }
        if (only_key.empty() && list.size() > 0) {
            return { {"success", true}, {"found", list.size()}, {"designations", list}, {"source", "window"} };
        }
    }
    const size_t chunk = 1u << 20;   // 1 MB
    static std::vector<uint8_t> buf(chunk + 8);

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0x14000000000ull;   // 从游戏堆起始区开始
    while (VirtualQuery((const void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (GetTickCount() - started > (DWORD)budget_ms) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool bad = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
        if (mbi.State == MEM_COMMIT && readable && !bad && size >= 8) {
            for (size_t off = 0; off < size; off += chunk) {
                if (GetTickCount() - started > (DWORD)budget_ms) break;
                const size_t want = std::min<size_t>(chunk, size - off);
                size_t got = 0;
                while (got + 8 <= want) {
                    int64_t q = 0;
                    if (!SafeReadI64((const void*)(base + off + got), &q)) break;
                    std::memcpy(buf.data() + got, &q, 8);
                    got += 8;
                }
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uint64_t v = 0;
                    std::memcpy(&v, buf.data() + i, 8);
                    if (v != vtable) continue;
                    const uintptr_t obj = base + off + i;
                    if (!seen_obj.insert(obj).second) continue;
                    std::string key;
                    if (!read_key(obj, key)) continue;
                    if (!seen.insert(key).second) continue;
                    char hex[24];
                    std::snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)obj);
                    list.push_back({ {"key", key}, {"object", hex} });
                    if (!only_key.empty() && key == only_key) {
                        return { {"success", true}, {"found", list.size()}, {"designations", list} };
                    }
                }
            }
        }
        if (size == 0) break;
        addr = base + size;
    }

    return { {"success", true}, {"found", list.size()}, {"designations", list},
             {"elapsed_ms", (int)(GetTickCount() - started)} };
}

nlohmann::json OutlinerManager::ListColonyDesignationsJson(uint32_t planet_id) {
    auto scanned = ScanDesignationObjects(planet_id, 1800, "");
    if (scanned.contains("error")) return scanned;
    nlohmann::json out = nlohmann::json::array();
    for (auto& item : scanned["designations"]) {
        const std::string key = item.value("key", std::string());
        out.push_back({ {"key", key}, {"object", item.value("object", std::string())}, {"name", LocalizeKey(key)} });
    }
    return { {"success", true}, {"planet_id", planet_id}, {"count", out.size()}, {"designations", out} };
}

nlohmann::json OutlinerManager::SetColonyDesignationJson(uint32_t planet_id, const std::string& key, uint64_t designation_ptr) {
    /* 通过引擎的 CSetColonyTypeCommand 切换殖民地设计（designation）。只读解析 + 一条命令。 */
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "engine functions not initialized"} };
    }
    if (key.empty()) {
        return { {"error", "designation key is empty"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"error", "planet not found: " + std::to_string(planet_id)} };

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"error", "planet has no colony"} };

    void* colony_obj = FindColony(cid);
    if (!colony_obj) return { {"error", "colony object not found: " + std::to_string(cid)} };

    void* found = nullptr;
    std::string found_key = key;
    int64_t found_offset = 0;
    int found_count = 0;

    if (designation_ptr != 0) {
        found = (void*)designation_ptr;  // 调用方已解析好
    } else {
        /* 设计对象：CColony 在 +0x108 存着当前设计的对象指针 */
        void* current_desig = nullptr;
        SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x100), &current_desig);
        if (!current_desig) return { {"error", "colony has no designation pointer"} };

        uint64_t vtable = 0;
        if (!SafeReadI64((const void*)current_desig, (int64_t*)&vtable) || (vtable >> 32) != 0x7ff6ull) {
            return { {"error", "cannot read designation vtable"} };
        }

        auto valid_key = [](const std::string& t) -> bool {
            if (t.size() < 3 || t.size() > 48) return false;
            for (char c : t) {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
            }
            return true;
        };

        auto read_cstr = [&](uintptr_t addr, std::string& out) -> bool {
            char buf[64] = {0};
            for (int k = 0; k < 7; ++k) {
                int64_t chunk = 0;
                if (!SafeReadI64((const void*)(addr + k * 8), &chunk)) return false;
                std::memcpy(buf + k * 8, &chunk, 8);
            }
            buf[63] = 0;
            std::string t(buf);
            if (!valid_key(t)) return false;
            out = t;
            return true;
        };

        auto read_key = [&](uintptr_t obj, std::string& out) -> bool {
            // 短键名内联在 +0x20；长键名在 +0x20 存指针，正文在别处
            if (read_cstr(obj + 0x20, out)) return true;
            int64_t ptr = 0;
            if (!SafeReadI64((const void*)(obj + 0x20), &ptr)) return false;
            if (ptr < 0x10000 || (uint64_t)ptr > 0x7fffffffffffull) return false;
            return read_cstr((uintptr_t)ptr, out);
        };

        const uintptr_t anchor = (uintptr_t)current_desig;
        const uintptr_t window = 4ull << 20;
        const uintptr_t from = anchor > window ? anchor - window : 0x10000;
        const uintptr_t to = anchor + window;
        const size_t chunk_size = 0x1000;
        uint8_t buf[0x1000];

        for (uintptr_t base = from; base < to && !found; base += chunk_size) {
            size_t got = 0;
            while (got + 8 <= chunk_size) {
                int64_t q = 0;
                if (!SafeReadI64((const void*)(base + got), &q)) break;
                std::memcpy(buf + got, &q, 8);
                got += 8;
            }
            if (got < 8) continue;
            for (size_t off = 0; off + 8 <= got; off += 8) {
                uint64_t v = 0;
                std::memcpy(&v, buf + off, 8);
                if (v != vtable) continue;
                std::string k;
                if (!read_key(base + off, k)) continue;
                ++found_count;
                if (k == key) { found = (void*)(base + off); found_offset = (int64_t)off; break; }
            }
        }

        if (!found) {
            /* 窗口扫描找不到（对象池可能被分配到很远的地方）→ 全堆扫描兜底 */
            auto scanned = ScanDesignationObjects(planet_id, 1800, key);
            if (scanned.contains("designations")) {
                for (auto& item : scanned["designations"]) {
                    if (item.value("key", std::string()) == key) {
                        const std::string hex = item.value("object", std::string());
                        found = (void*)(uintptr_t)std::strtoull(hex.c_str(), nullptr, 16);
                        break;
                    }
                }
            }
            if (!found) {
                return { {"error", "designation not found: " + key}, {"objects_seen", found_count} };
            }
        }
    }

    namespace desig = sdk::cmd::set_colony_type_command;
    auto cmd = CommandBuilder::Get().Create(desig::kSpec);
    cmd.Set<uint32_t>(desig::colony, cid);
    cmd.Set<uint32_t>(desig::country, GetPlayerCountryId());
    cmd.Set<void*>(desig::designation, found);

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"is_valid", false}, {"planet_id", planet_id},
                 {"colony_id", cid}, {"key", key},
                 {"error", why.empty() ? "designation change refused" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"planet_id", planet_id}, {"colony_id", cid},
                 {"key", key}, {"error", cmd.error()} };
    }

    char obj_hex[24];
    std::snprintf(obj_hex, sizeof(obj_hex), "0x%llX", (unsigned long long)(uintptr_t)found);
    return { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid},
             {"key", key}, {"designation_object", obj_hex}, {"offset_from_current", found_offset} };
}

nlohmann::json OutlinerManager::SetDistrictZoneJson(uint32_t planet_id, uint32_t district_id, int32_t slot,
                                                    const std::string& zone_key) {
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "Planet not found: " + std::to_string(planet_id)} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + sdk::ent::CPlanet::colony), &cid);
    void* colony_obj = cid != 0xFFFFFFFF ? FindColony(cid) : nullptr;
    if (!colony_obj) return { {"success", false}, {"error", "Planet has no colony"} };
    auto districts = ColonyDistrictSlots(colony_obj);
    auto it = std::find_if(districts.begin(), districts.end(), [&](const DistrictSlots& d) { return d.id == district_id; });
    if (it == districts.end()) return { {"success", false}, {"error", "District " + std::to_string(district_id) + " is not on this planet"} };
    if (slot < 0 || slot >= (int32_t)it->zone_ids.size()) {
        return { {"success", false}, {"error", "The district has " + std::to_string(it->zone_ids.size()) + " zone slots"} };
    }
    void* zone_type = FindDbElementByKey(base_address_, sdk::glob::TGameDatabase_CZoneTypeDatabase_pInstance, zone_key);
    if (!zone_type) return { {"success", false}, {"error", "Unknown zone type: " + zone_key} };
    uint8_t obj[0x20];
    FillZoneBuildable(obj, zone_type, cid, district_id, slot);
    std::string why;
    if (!QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), GetPlanetQueueId(planet_id), true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused the zone" : why} };
    }
    return { {"success", true}, {"planet_id", planet_id}, {"district_id", district_id}, {"slot", slot},
             {"zone", zone_key}, {"zone_name", LocalizeKey(zone_key)}, {"message", "Zone construction queued"} };
}

// ---- building construction -------------------------------------------------------------------
// CBuildableBuilding (0x20 bytes): vtable, CBuildingType*, colony id, zone id, building id
// (0xFFFFFFFF: new). Its vtable slot 9 is CBuildableBuildingBase::CalcCost(CResourceTable& cost,
// CResourceTable&, CString*) and slot 11 CalcProgressionTimeNeeded(CString*) -> CFixedPoint days
// (CBuildingType::CalcBuildTime with the colony's modifiers), as the build queue uses them.

void OutlinerManager::FillBuildable(uint8_t (&obj)[0x20], void* building_type, uint32_t colony_id, uint32_t zone_id) {
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableBuildingVt);
    *(void**)(obj + 0x08) = building_type;
    *(uint32_t*)(obj + 0x10) = colony_id;
    *(uint32_t*)(obj + 0x14) = zone_id;
    *(uint32_t*)(obj + 0x18) = 0xFFFFFFFF;  // a new building
}


// ---- starbase module construction -------------------------------------------------------------
// CBuildableStarbaseModule (0x20 bytes): vtable, CStarbaseModuleType*, starbase id, slot.
// The vtable was recovered from its token getter: token 0x3E01 ("buildable_starbase_module",
// the exe's renumbered id) sits in slot 16 of the vtable at 0x2349600, and the producer
// (0xAD2EF0) stores exactly these four fields. Verified against the game's own
// "create starbase module command" function, which allocates 0x20 and writes that vtable.
constexpr uintptr_t kBuildableStarbaseModuleVt = 0x2349600;
// CBuildableStarbaseBuilding: token 0x3E00（buildable_starbase_building）的 getter 在 0x208060，
// .rdata 里指向它的指针落在 0x2349730（= vtable + 0x80，即槽 16）→ vtable = 0x23496B0。
// 生产者 0x227D00：分配 0x20，写 {vtable, 类型@+8, id@+0x10, slot@+0x14}，与模块同构。
constexpr uintptr_t kBuildableStarbaseBuildingVt = 0x23496B0;
// CBuildableStarbaseUpgrade：token 0x3E02（buildable_starbase_upgrade）的 getter 在 0x208170，
// .rdata 里指向它的指针落在 0x2349890（= vtable + 0x80）→ vtable = 0x2349810。
// 序列化器只有 {starbase_upgrade(目标等级), starbase} 两个字段——没有 slot，所以形态是
// {vtable, 目标等级类型@+8, starbase id@+0x10}。
constexpr uintptr_t kBuildableStarbaseUpgradeVt = 0x2349810;

/* 查一座恒星基地的**下一级**能不能升：不花钱，只让引擎校验，并报出合金与工期。 */
nlohmann::json OutlinerManager::GetStarbaseUpgradeJson(uint32_t starbase_id, bool dispatch) {
    if (!base_address_ || !fn_post_command_) return { {"success", false}, {"error", "Engine functions not initialized"} };

    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    std::string level;
    void* level_type = nullptr;
    if (SafeReadPtr((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::level), &level_type) && level_type) {
        SafeReadPdxString((const void*)((uintptr_t)level_type + 0x20), level);
    }

    // 升级链（2026-11-23 补方舟 / 驿站 / 轨道环 / 深空要塞）：
    // 常规五级、驿港 1→3、方舟按舰种 1→3（champions_forge 无下一级）。
    // 桥没有 CStarbaseLevelType 的 next_level 字段偏移，所以照 common/starbase_levels/*.txt 硬编码；
    // 脚本里 next_level 写的正是下面这些目标。mod 改链时要跟着改这里。
    static const std::map<std::string, std::string> kNext = {
        {"starbase_level_outpost", "starbase_level_starport"},
        {"starbase_level_starport", "starbase_level_starhold"},
        {"starbase_level_starhold", "starbase_level_starfortress"},
        {"starbase_level_starfortress", "starbase_level_citadel"},
        {"starbase_level_waystation_1", "starbase_level_waystation_2"},
        {"starbase_level_waystation_2", "starbase_level_waystation_3"},
        {"starbase_level_civilian_arkship_1", "starbase_level_civilian_arkship_2"},
        {"starbase_level_civilian_arkship_2", "starbase_level_civilian_arkship_3"},
        {"starbase_level_science_arkship_1", "starbase_level_science_arkship_2"},
        {"starbase_level_science_arkship_2", "starbase_level_science_arkship_3"},
        {"starbase_level_military_arkship_1", "starbase_level_military_arkship_2"},
        {"starbase_level_military_arkship_2", "starbase_level_military_arkship_3"},
        {"starbase_level_orbital_ring_tier_1", "starbase_level_orbital_ring_tier_2"},
        {"starbase_level_orbital_ring_tier_2", "starbase_level_orbital_ring_tier_3"},
        {"starbase_level_deep_space_citadel_1", "starbase_level_deep_space_citadel_2"},
        {"starbase_level_deep_space_citadel_2", "starbase_level_deep_space_citadel_3"},
    };
    auto it = kNext.find(level);
    if (it == kNext.end()) {
        const bool maxed = level == "starbase_level_citadel" || level.find("champions_forge") != std::string::npos ||
                           (level.rfind("starbase_level_", 0) == 0 &&
                            (level.find("arkship_3") != std::string::npos || level.find("waystation_3") != std::string::npos ||
                             level.find("orbital_ring_tier_3") != std::string::npos ||
                             level.find("deep_space_citadel_3") != std::string::npos));
        return { {"success", true}, {"starbase_id", starbase_id}, {"level", level},
                 {"current_level", level}, {"can_upgrade", false},
                 {"reason", maxed ? "已是该类型的最高级" : "这一级没有下一级（不在已知升级链里）"} };
    }

    uint32_t queue_id = BuildQueueForStarbase(base_address_, starbase, nullptr);
    if (queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "starbase has no build queue"} };
    }

    void* target = FindDbElementByKey(base_address_, sdk::glob::TGameDatabase_CStarbaseLevelTypeDatabase_pInstance, it->second);
    if (!target) return { {"success", false}, {"error", "Unknown starbase level: " + it->second} };

    uint8_t obj[0x20];
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableStarbaseUpgradeVt);
    *(void**)(obj + 0x08) = target;
    *(uint32_t*)(obj + 0x10) = starbase_id;

    std::string why;
    const bool ok = QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, dispatch, &why);

    nlohmann::json out = { {"success", true},
                           {"starbase_id", starbase_id},
                           {"current_level", level},
                           {"next_level", it->second},
                           {"can_upgrade", ok} };
    if (ok) out.update(BuildableCostJson(obj));
    else out["reason"] = why.empty() ? "The game refused the upgrade" : why;
    return out;
}

nlohmann::json OutlinerManager::QueueStarbaseModuleJson(uint32_t starbase_id, int32_t slot,
                                                        const std::string& key) {
    if (key.empty()) return { {"success", false}, {"error", "module key is empty"} };
    if (slot < 0) return { {"success", false}, {"error", "slot must be >= 0"} };

    // find the starbase object by id
    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    // 与界面同一条队列。2026-11-23 用 PostCommand 旁听抓到界面投的 CAddBuildableToQueueCommand：
    // +0x2C 是 6829（**殖民地队列**），模块就落在 6829 里。CStarbase+0x880（6828）那条是隐藏队列，
    // 引擎认、界面从不使用——照它投，东西会造出来但玩家在界面上看不到。
    uint32_t queue_id = BuildQueueForStarbase(base_address_, starbase, nullptr);
    if (queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "starbase has no build queue"} };
    }

    void* module_type = FindDbElementByKey(base_address_,
                                           sdk::glob::TGameDatabase_CStarbaseModuleDatabase_pInstance, key);
    if (!module_type) return { {"success", false}, {"error", "Unknown starbase module: " + key} };

    uint8_t obj[0x20];
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableStarbaseModuleVt);
    *(void**)(obj + 0x08) = module_type;
    *(uint32_t*)(obj + 0x10) = starbase_id;
    *(uint32_t*)(obj + 0x14) = (uint32_t)slot;

    std::string why;
    if (!QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused the module" : why} };
    }
    return { {"success", true}, {"starbase_id", starbase_id}, {"slot", slot}, {"module", key},
             {"module_name", LocalizeKey(key)}, {"message", "Module construction queued"} };
}

/* 恒星基地**建筑槽**入队：同上面的模块路径，换成 CBuildableStarbaseBuilding（vtable 0x23496B0）。
   字段布局与模块同构：{vtable, 建筑类型@+8, starbase id@+0x10, 建筑槽@+0x14}，命令也是
   CAddBuildableToQueueCommand，走同一个建造队列。 */
nlohmann::json OutlinerManager::QueueStarbaseBuildingJson(uint32_t starbase_id, int32_t slot,
                                                          const std::string& key) {
    if (key.empty()) return { {"success", false}, {"error", "building key is empty"} };
    if (slot < 0) return { {"success", false}, {"error", "slot must be >= 0"} };

    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    uint32_t queue_id = BuildQueueForStarbase(base_address_, starbase, nullptr);
    if (queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "starbase has no build queue"} };
    }

    void* building_type = FindDbElementByKey(base_address_,
                                             sdk::glob::TGameDatabase_CStarbaseBuildingDatabase_pInstance, key);
    if (!building_type) return { {"success", false}, {"error", "Unknown starbase building: " + key} };

    uint8_t obj[0x20];
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableStarbaseBuildingVt);
    *(void**)(obj + 0x08) = building_type;
    *(uint32_t*)(obj + 0x10) = starbase_id;
    *(uint32_t*)(obj + 0x14) = (uint32_t)slot;

    std::string why;
    if (!QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused the building" : why} };
    }

    // 建筑有格式塔变体（crew_quarters → 服务电缆），名字按权威类型挑
    std::string variant;
    {
        std::string authority = GovernmentManager::Get().GetGovernmentState().authority;
        if (authority == "auth_machine_intelligence") variant = "_machine_intelligence";
        else if (authority == "auth_hive_mind") variant = "_hive_mind";
    }
    std::string name;
    if (!variant.empty()) {
        const std::string vk = "sm_" + key + variant;
        name = LocalizeKey(vk);
        if (name.empty() || name == vk) {
            const std::string gk = "sm_" + key + "_gestalt";
            name = LocalizeKey(gk);
            if (name == gk) name.clear();
        }
    }
    if (name.empty()) name = LocalizeKey("sm_" + key);

    return { {"success", true}, {"starbase_id", starbase_id}, {"slot", slot}, {"building", key},
             {"building_name", name}, {"message", "Building construction queued"} };
}

/* 某座恒星基地**每个空槽位能建哪些模块**（对应殖民地的 get_buildable_buildings）。

   做法：逐个（槽 × 模块）填一个 CBuildableStarbaseModule，调 QueueBuildable(dispatch=false)
   只让引擎校验、不下单，拿 can_build 与引擎给的原因；能建时再用 BuildableCostJson 取花费与工期。
   模块目录来自 CStarbaseModuleDatabase（db+0x50 数组、+0x5C 个数，键在类型对象 +0x20）。
   已装模块从实例的 +0x5E0 数组读（按槽位索引、可稀疏），占用的槽直接标出来、不再探。 */
nlohmann::json OutlinerManager::GetBuildableStarbaseModulesJson(uint32_t starbase_id, int32_t slot,
                                                               const std::string& module_key,
                                                               bool include_blocked) {
    if (!base_address_ || !fn_post_command_) return { {"success", false}, {"error", "Engine functions not initialized"} };

    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    uint32_t queue_id = BuildQueueForStarbase(base_address_, starbase, nullptr);
    if (queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "starbase has no build queue"} };
    }

    std::string level;
    uint32_t module_slots = 0;
    void* level_type = nullptr;
    if (SafeReadPtr((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::level), &level_type) && level_type) {
        SafeReadPdxString((const void*)((uintptr_t)level_type + 0x20), level);
        SafeReadU32((const void*)((uintptr_t)level_type + 0xDC), &module_slots);  // 条数（+0xD8 是容量）
    }
    if (module_slots > 64) module_slots = 64;

    void* installed = nullptr;
    uint32_t installed_n = 0;
    SafeReadPtr((const void*)((uintptr_t)starbase + 0x5E0), &installed);
    SafeReadU32((const void*)((uintptr_t)starbase + 0x5E8), &installed_n);

    int32_t first = slot >= 0 ? slot : 0;
    int32_t last = slot >= 0 ? slot : (int32_t)module_slots - 1;
    if (slot >= 0 && (uint32_t)slot >= module_slots) {
        return { {"success", false}, {"error", "slot " + std::to_string(slot) + " is beyond this starbase's " +
                                                 std::to_string(module_slots) + " module slots"} };
    }

    // 模块目录：要么只问一个键，要么全表
    bool single = !module_key.empty();
    void* only_type = nullptr;
    if (single) {
        only_type = FindDbElementByKey(base_address_, sdk::glob::TGameDatabase_CStarbaseModuleDatabase_pInstance,
                                       module_key);
        if (!only_type) return { {"success", false}, {"error", "Unknown starbase module: " + module_key} };
    }
    void* db = nullptr;
    void* mod_arr = nullptr;
    uint32_t mod_n = 0;
    if (!single) {
        if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CStarbaseModuleDatabase_pInstance),
                         &db) || !db ||
            !SafeReadPtr((const void*)((uintptr_t)db + 0x50), &mod_arr) || !mod_arr ||
            !SafeReadU32((const void*)((uintptr_t)db + 0x5C), &mod_n) || mod_n > 5000) {
            return { {"success", false}, {"error", "Starbase module database not readable"} };
        }
    }

    // 格式塔有两套变体后缀：`_machine_intelligence` / `_hive_mind`（crew_quarters → 服务电缆），
    // 以及 `_gestalt`（trading_hub → 资源再处理器）。先试权威对应的那套，再试 `_gestalt`，最后通用键。
    std::string variant;
    {
        std::string authority = GovernmentManager::Get().GetGovernmentState().authority;
        if (authority == "auth_machine_intelligence") variant = "_machine_intelligence";
        else if (authority == "auth_hive_mind") variant = "_hive_mind";
    }
    auto localize_sm = [&](const std::string& key) {
        if (!variant.empty()) {
            const std::string vk = "sm_" + key + variant;
            std::string text = LocalizeKey(vk);
            if (!text.empty() && text != vk) return text;
            const std::string gk = "sm_" + key + "_gestalt";
            std::string gtext = LocalizeKey(gk);
            if (!gtext.empty() && gtext != gk) return gtext;
        }
        return LocalizeKey("sm_" + key);
    };

    auto probe = [&](void* module_type, uint32_t slot_index, std::string* why, uint8_t (&obj)[0x20]) {
        memset(obj, 0, sizeof(obj));
        *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableStarbaseModuleVt);
        *(void**)(obj + 0x08) = module_type;
        *(uint32_t*)(obj + 0x10) = starbase_id;
        *(uint32_t*)(obj + 0x14) = slot_index;
        return QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, false, why);
    };

    nlohmann::json slots = nlohmann::json::array();
    for (int32_t s = first; s <= last; ++s) {
        nlohmann::json entry = { {"slot", s}, {"occupied", false} };
        void* current = nullptr;
        std::string cur_key;
        if (installed && (uint32_t)s < installed_n) {
            SafeReadPtr((const void*)((uintptr_t)installed + (uint32_t)s * sizeof(void*)), &current);
            // 空位不是 nullptr，而是个读不出键的占位对象（TPdxNullObject 一类）——按「键为空」判空
            if (current) SafeReadPdxString((const void*)((uintptr_t)current + 0x20), cur_key);
        }
        if (!cur_key.empty()) {
            entry["occupied"] = true;
            entry["current_key"] = cur_key;
            entry["current_name"] = localize_sm(cur_key);
        }

        // 已装的槽**照样能换别的模块**（2026-11-17 23:30 实测：往装着 anchorage 的槽 0 下单
        // trading_hub，一 tick 后真的被替换）。所以这里不再跳过已装槽，只把「与已装同名的那个」
        // 排除掉——同一个模块不能在一槽里装两份，而引擎对此的回应是静默成功、什么都不做。
        nlohmann::json buildable = nlohmann::json::array();
        nlohmann::json blocked = nlohmann::json::array();
        for (uint32_t i = 0; i < (single ? 1u : mod_n); ++i) {
            void* type = only_type;
            std::string key = module_key;
            if (!single) {
                type = nullptr;
                key.clear();
                if (!SafeReadPtr((const void*)((uintptr_t)mod_arr + i * 8), &type) || !type) continue;
                if (!SafeReadPdxString((const void*)((uintptr_t)type + 0x20), key) || key.empty()) continue;
            }
            if (!cur_key.empty() && key == cur_key) {
                if (include_blocked || single) {
                    blocked.push_back({ {"key", key}, {"name", localize_sm(key)},
                                        {"reason", "该槽已装同名模块，换一个种类才会替换"} });
                }
                continue;
            }
            uint8_t obj[0x20];
            std::string why;
            if (probe(type, (uint32_t)s, &why, obj)) {
                nlohmann::json item = { {"key", key}, {"name", localize_sm(key)} };
                item.update(BuildableCostJson(obj));
                buildable.push_back(item);
            } else if (include_blocked) {
                blocked.push_back({ {"key", key}, {"name", localize_sm(key)},
                                    {"reason", why.empty() ? "The game refused it" : why} });
            }
        }
        entry["buildable"] = buildable;
        if (include_blocked || single) entry["blocked"] = blocked;
        slots.push_back(entry);
    }

    return { {"success", true},
             {"starbase_id", starbase_id},
             {"level", level},
             {"module_slots", module_slots},
             {"slots", slots} };
}

/* 建筑槽版本：同上面的模块探针，换成 CBuildableStarbaseBuilding + CStarbaseBuildingDatabase。
   已装建筑在实例 +0x5F8（size +0x600），槽位上限在等级对象 +0xF0。
   vtable 0x23496B0 是拿 token 0x3E00（buildable_starbase_building）的 getter 反查出来的：
   getter 在 0x208060，.rdata 里指向它的指针落在 0x2349730（= vtable+0x80，即槽 16）。 */
nlohmann::json OutlinerManager::GetBuildableStarbaseBuildingsJson(uint32_t starbase_id, int32_t slot,
                                                               const std::string& building_key,
                                                               bool include_blocked) {
    if (!base_address_ || !fn_post_command_) return { {"success", false}, {"error", "Engine functions not initialized"} };

    void* mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &mgr) || !mgr) {
        return { {"success", false}, {"error", "CStarbase database not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)mgr + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)mgr + 0x20), &cap);

    void* starbase = nullptr;
    for (uint32_t i = 0; i < cap && arr; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + 8), &id)) continue;
        if ((id & 0xFFFFFF) == (starbase_id & 0xFFFFFF)) { starbase = obj; break; }  // 表里有些 id 带 0x01000000 标记
    }
    if (!starbase) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    uint32_t queue_id = BuildQueueForStarbase(base_address_, starbase, nullptr);
    if (queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "starbase has no build queue"} };
    }

    std::string level;
    uint32_t module_slots = 0;
    void* level_type = nullptr;
    if (SafeReadPtr((const void*)((uintptr_t)starbase + sdk::ent::CStarbase::level), &level_type) && level_type) {
        SafeReadPdxString((const void*)((uintptr_t)level_type + 0x20), level);
        SafeReadU32((const void*)((uintptr_t)level_type + 0xF4), &module_slots);  // 条数（+0xF0 是容量）
    }
    if (module_slots > 64) module_slots = 64;

    void* installed = nullptr;
    uint32_t installed_n = 0;
    SafeReadPtr((const void*)((uintptr_t)starbase + 0x5F8), &installed);
    SafeReadU32((const void*)((uintptr_t)starbase + 0x600), &installed_n);

    int32_t first = slot >= 0 ? slot : 0;
    int32_t last = slot >= 0 ? slot : (int32_t)module_slots - 1;
    if (slot >= 0 && (uint32_t)slot >= module_slots) {
        return { {"success", false}, {"error", "slot " + std::to_string(slot) + " is beyond this starbase's " +
                                                 std::to_string(module_slots) + " building slots"} };
    }

    // 模块目录：要么只问一个键，要么全表
    bool single = !building_key.empty();
    void* only_type = nullptr;
    if (single) {
        only_type = FindDbElementByKey(base_address_, sdk::glob::TGameDatabase_CStarbaseBuildingDatabase_pInstance,
                                       building_key);
        if (!only_type) return { {"success", false}, {"error", "Unknown starbase building: " + building_key} };
    }
    void* db = nullptr;
    void* mod_arr = nullptr;
    uint32_t mod_n = 0;
    if (!single) {
        if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CStarbaseBuildingDatabase_pInstance),
                         &db) || !db ||
            !SafeReadPtr((const void*)((uintptr_t)db + 0x50), &mod_arr) || !mod_arr ||
            !SafeReadU32((const void*)((uintptr_t)db + 0x5C), &mod_n) || mod_n > 5000) {
            return { {"success", false}, {"error", "Starbase building database not readable"} };
        }
    }

    // 格式塔有两套变体后缀：`_machine_intelligence` / `_hive_mind`（crew_quarters → 服务电缆），
    // 以及 `_gestalt`（trading_hub → 资源再处理器）。先试权威对应的那套，再试 `_gestalt`，最后通用键。
    std::string variant;
    {
        std::string authority = GovernmentManager::Get().GetGovernmentState().authority;
        if (authority == "auth_machine_intelligence") variant = "_machine_intelligence";
        else if (authority == "auth_hive_mind") variant = "_hive_mind";
    }
    auto localize_sm = [&](const std::string& key) {
        if (!variant.empty()) {
            const std::string vk = "sm_" + key + variant;
            std::string text = LocalizeKey(vk);
            if (!text.empty() && text != vk) return text;
            const std::string gk = "sm_" + key + "_gestalt";
            std::string gtext = LocalizeKey(gk);
            if (!gtext.empty() && gtext != gk) return gtext;
        }
        return LocalizeKey("sm_" + key);
    };

    auto probe = [&](void* module_type, uint32_t slot_index, std::string* why, uint8_t (&obj)[0x20]) {
        memset(obj, 0, sizeof(obj));
        *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableStarbaseBuildingVt);
        *(void**)(obj + 0x08) = module_type;
        *(uint32_t*)(obj + 0x10) = starbase_id;
        *(uint32_t*)(obj + 0x14) = slot_index;
        return QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, false, why);
    };

    nlohmann::json slots = nlohmann::json::array();
    for (int32_t s = first; s <= last; ++s) {
        nlohmann::json entry = { {"slot", s}, {"occupied", false} };
        void* current = nullptr;
        std::string cur_key;
        if (installed && (uint32_t)s < installed_n) {
            SafeReadPtr((const void*)((uintptr_t)installed + (uint32_t)s * sizeof(void*)), &current);
            // 空位不是 nullptr，而是个读不出键的占位对象（TPdxNullObject 一类）——按「键为空」判空
            if (current) SafeReadPdxString((const void*)((uintptr_t)current + 0x20), cur_key);
        }
        if (!cur_key.empty()) {
            entry["occupied"] = true;
            entry["current_key"] = cur_key;
            entry["current_name"] = localize_sm(cur_key);
        }

        // 已装的槽**照样能换别的模块**（2026-11-17 23:30 实测：往装着 anchorage 的槽 0 下单
        // trading_hub，一 tick 后真的被替换）。所以这里不再跳过已装槽，只把「与已装同名的那个」
        // 排除掉——同一个模块不能在一槽里装两份，而引擎对此的回应是静默成功、什么都不做。
        nlohmann::json buildable = nlohmann::json::array();
        nlohmann::json blocked = nlohmann::json::array();
        for (uint32_t i = 0; i < (single ? 1u : mod_n); ++i) {
            void* type = only_type;
            std::string key = building_key;
            if (!single) {
                type = nullptr;
                key.clear();
                if (!SafeReadPtr((const void*)((uintptr_t)mod_arr + i * 8), &type) || !type) continue;
                if (!SafeReadPdxString((const void*)((uintptr_t)type + 0x20), key) || key.empty()) continue;
            }
            if (!cur_key.empty() && key == cur_key) {
                if (include_blocked || single) {
                    blocked.push_back({ {"key", key}, {"name", localize_sm(key)},
                                        {"reason", "该槽已装同名建筑，换一个种类才会替换"} });
                }
                continue;
            }
            uint8_t obj[0x20];
            std::string why;
            if (probe(type, (uint32_t)s, &why, obj)) {
                nlohmann::json item = { {"key", key}, {"name", localize_sm(key)} };
                item.update(BuildableCostJson(obj));
                buildable.push_back(item);
            } else if (include_blocked) {
                blocked.push_back({ {"key", key}, {"name", localize_sm(key)},
                                    {"reason", why.empty() ? "The game refused it" : why} });
            }
        }
        entry["buildable"] = buildable;
        if (include_blocked || single) entry["blocked"] = blocked;
        slots.push_back(entry);
    }

    return { {"success", true},
             {"starbase_id", starbase_id},
             {"level", level},
             {"building_slots", module_slots},
             {"slots", slots} };
}

nlohmann::json OutlinerManager::BuildableCostJson(uint8_t (&obj)[0x20]) {
    return BuildableCostJsonPtr(obj);
}

nlohmann::json OutlinerManager::BuildableCostJsonPtr(void* obj) {
    const auto& names = GameState::Get().ResourceNames();
    StackResourceTable cost{}, other{};
    for (StackResourceTable* t : { &cost, &other }) {
        t->holder = &t->data;
        t->data = t->values;
        t->capacity = t->size = (int32_t)std::min<size_t>(names.size(), 256);
    }
    BuildableCostCtx ctx{ obj, &cost, &other, 0 };
    nlohmann::json out = { {"cost", nlohmann::json::object()} };
    if (!CommandBuilder::Get().CallGuarded(&CallBuildableCost, &ctx)) return out;
    for (int32_t i = 0; i < cost.size; ++i) {
        if (cost.values[i] > 0 && !names[i].empty()) out["cost"][names[i]] = std::round(cost.values[i] / 1000.0) / 100.0;
    }
    out["build_days"] = std::round(ctx.days / 1000.0) / 100.0;
    return out;
}

std::vector<OutlinerManager::ZoneRef> OutlinerManager::ColonyZones(void* colony_obj) {
    // Zones of the colony's districts: CZone +0x8 id, +0x18 CDistrict*, +0x20 CZoneType* (key +0x20),
    // +0x1A0 buildings CPdxArray (size +0x1B4); CDistrict +0x18 CColony*, +0x20 CDistrictType*.
    std::vector<ZoneRef> out;
    void* z_mgr = nullptr;
    void* z_arr = nullptr;
    uint32_t z_cap = 0;
    if (!colony_obj || !SafeReadPtr((const void*)(base_address_ + sdk::db::CZone), &z_mgr) || !z_mgr ||
        !SafeReadPtr((const void*)((uintptr_t)z_mgr + 0x18), &z_arr) || !z_arr ||
        !SafeReadU32((const void*)((uintptr_t)z_mgr + 0x20), &z_cap)) {
        return out;
    }
    for (uint32_t i = 0; i < z_cap; ++i) {
        void* zone = nullptr;
        void* district = nullptr;
        void* colony = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)z_arr + i * 16 + 8), &zone) || !zone ||
            !SafeReadPtr((const void*)((uintptr_t)zone + 0x18), &district) || !district ||
            !SafeReadPtr((const void*)((uintptr_t)district + 0x18), &colony) || colony != colony_obj) {
            continue;
        }
        ZoneRef z;
        z.zone = zone;
        SafeReadU32((const void*)((uintptr_t)zone + 8), &z.id);
        void* ztype = nullptr;
        void* dtype = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)zone + 0x20), &ztype) && ztype) {
            SafeReadPdxString((const void*)((uintptr_t)ztype + 0x20), z.key);
        }
        if (SafeReadPtr((const void*)((uintptr_t)district + 0x20), &dtype) && dtype) {
            SafeReadPdxString((const void*)((uintptr_t)dtype + 0x20), z.district_key);
        }
        SafeReadU32((const void*)((uintptr_t)zone + 0x1B4), &z.buildings);
        MaxBuildingsCtx mctx{ base_address_ + sdk::fn::CColony_CalcMaxBuildings, colony_obj, zone, GetPlayerCountry(), 0 };
        if (CommandBuilder::Get().CallGuarded(&CallMaxBuildings, &mctx)) z.max_buildings = mctx.result;
        out.push_back(z);
    }
    return out;
}

nlohmann::json OutlinerManager::GetBuildableBuildingsJson(uint32_t planet_id, const std::string& building_key, int32_t zone_id, uint32_t colony_id) {
    uint32_t cid = 0xFFFFFFFF, queue_id = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
        queue_id = GetColonyQueueId(cid);
        if (queue_id == 0xFFFFFFFF)
            return { {"success", false}, {"error", "Colony " + std::to_string(cid) + " has no construction queue"} };
    } else {
        void* p_obj = FindPlanet(planet_id);
        if (!p_obj || !SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid) || cid == 0xFFFFFFFF) {
            return { {"success", false}, {"error", "Planet " + std::to_string(planet_id) + " has no colony"} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe4), &queue_id);
    }
    void* colony_obj = FindColony(cid);
    const uint32_t country_id = GetPlayerCountryId();

    std::vector<ZoneRef> zones = ColonyZones(colony_obj);
    if (zone_id >= 0) {
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const ZoneRef& z) { return z.id != (uint32_t)zone_id; }),
                    zones.end());
        if (zones.empty()) return { {"success", false}, {"error", "Zone " + std::to_string(zone_id) + " is not on this planet"} };
    }

    // One building: whether and where it can be built, with the game's reason.
    if (!building_key.empty()) {
        void* type = FindDbElementByKey(base_address_, kBuildingTypeDb, building_key);
        if (!type) return { {"success", false}, {"error", "Building type '" + building_key + "' not found"} };
        nlohmann::json per_zone = nlohmann::json::array();
        for (const auto& z : zones) {
            uint8_t obj[0x20];
            FillBuildable(obj, type, cid, z.id);
            std::string why;
            bool ok = QueueBuildable(obj, sizeof(obj), country_id, queue_id, false, &why);
            nlohmann::json j = { {"zone_id", z.id}, {"zone", z.key}, {"can_build", ok} };
            if (ok) j.update(BuildableCostJson(obj));
            else j["reason"] = why;
            per_zone.push_back(j);
        }
        return { {"success", true}, {"planet_id", planet_id}, {"building_key", building_key},
                 {"building_name", LocalizeKey(building_key)}, {"zones", per_zone} };
    }

    // Every building type the game would accept queued in each zone now.
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t n = 0;
    if (!SafeReadPtr((const void*)(base_address_ + kBuildingTypeDb), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x5C), &n) || n > 10000) {
        return { {"success", false}, {"error", "Building database not readable"} };
    }
    nlohmann::json zones_json = nlohmann::json::array();
    for (const auto& z : zones) {
        nlohmann::json buildable = nlohmann::json::array();
        for (uint32_t i = 0; i < n; ++i) {
            void* type = nullptr;
            std::string key;
            if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &type) || !type ||
                !SafeReadPdxString((const void*)((uintptr_t)type + 0x20), key) || key.empty()) {
                continue;
            }
            uint8_t obj[0x20];
            FillBuildable(obj, type, cid, z.id);
            if (!QueueBuildable(obj, sizeof(obj), country_id, queue_id, false, nullptr)) continue;
            nlohmann::json j = { {"key", key}, {"name", LocalizeKey(key)} };
            j.update(BuildableCostJson(obj));
            buildable.push_back(j);
        }
        zones_json.push_back({ {"zone_id", z.id}, {"zone", z.key}, {"zone_name", LocalizeKey(z.key)},
                               {"district", z.district_key}, {"buildings", z.buildings},
                               {"max_buildings", z.max_buildings}, {"buildable", buildable} });
    }
    return { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid}, {"zones", zones_json} };
}

// CBuildableDistrict {vtable, CDistrictType* +0x08, colony +0x10}: one more district of the type
// (what the colony automation builds; CBuildableDistrict::CSerializer writes type and colony)
void OutlinerManager::FillDistrictBuildable(uint8_t (&obj)[0x20], void* district_type, uint32_t colony_id) {
    memset(obj, 0, sizeof(obj));
    *(void**)(obj + 0x00) = (void*)(base_address_ + kBuildableDistrictVt);
    *(void**)(obj + 0x08) = district_type;
    *(uint32_t*)(obj + 0x10) = colony_id;
}

// The planet's colony and construction queue, or an error
bool OutlinerManager::PlanetColonyQueue(uint32_t planet_id, uint32_t* cid, uint32_t* queue_id, std::string* error,
                                        uint32_t colony_id) {
    // 方舟（Nomads）没有行星：给了 colony_id 就直接按 carrier 找队列
    if (colony_id != 0xFFFFFFFF) {
        *cid = colony_id;
        *queue_id = GetColonyQueueId(colony_id);
        if (*queue_id == 0xFFFFFFFF) {
            *error = "Colony " + std::to_string(colony_id) + " has no construction queue";
            return false;
        }
        return true;
    }
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        *error = "Planet with ID " + std::to_string(planet_id) + " not found";
        return false;
    }
    if (!SafeReadU32((const void*)((uintptr_t)p_obj + sdk::ent::CPlanet::colony), cid) || *cid == 0xFFFFFFFF) {
        *error = "Planet has no active colony";
        return false;
    }
    if (!SafeReadU32((const void*)((uintptr_t)p_obj + sdk::ent::CPlanet::build_queue), queue_id) || *queue_id == 0xFFFFFFFF) {
        *error = "Planet has no active construction queue";
        return false;
    }
    return true;
}

// District types the game would queue on the planet now (CAddBuildableToQueueCommand::IsValid),
// with the colony's current count of each; one type with its reason when district_key is given.
nlohmann::json OutlinerManager::GetBuildableDistrictsJson(uint32_t planet_id, const std::string& district_key, uint32_t colony_id) {
    uint32_t cid = 0xFFFFFFFF, queue_id = 0xFFFFFFFF;
    std::string error;
    if (!PlanetColonyQueue(planet_id, &cid, &queue_id, &error, colony_id)) return { {"success", false}, {"error", error} };
    const uint32_t country_id = GetPlayerCountryId();

    std::unordered_map<std::string, int> built;
    for (const auto& d : ColonyDistrictCounts(FindColony(cid))) built[d.first] = d.second;

    auto row = [&](void* type, const std::string& key, bool* ok) {
        uint8_t obj[0x20];
        FillDistrictBuildable(obj, type, cid);
        std::string why;
        *ok = QueueBuildable(obj, sizeof(obj), country_id, queue_id, false, &why);
        nlohmann::json j = { {"key", key}, {"name", LocalizeKey(key)}, {"built", built.count(key) ? built[key] : 0},
                             {"can_build", *ok} };
        if (*ok) j.update(BuildableCostJson(obj));
        else j["reason"] = why;
        return j;
    };

    if (!district_key.empty()) {
        void* type = FindDbElementByKey(base_address_, kDistrictTypeDb, district_key);
        if (!type) return { {"success", false}, {"error", "District type '" + district_key + "' not found"} };
        bool ok = false;
        return { {"success", true}, {"planet_id", planet_id}, {"district", row(type, district_key, &ok)} };
    }

    void* db = nullptr;
    void* arr = nullptr;
    uint32_t n = 0;
    if (!SafeReadPtr((const void*)(base_address_ + kDistrictTypeDb), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x5C), &n) || n > 2000) {
        return { {"success", false}, {"error", "District database not readable"} };
    }
    nlohmann::json buildable = nlohmann::json::array();
    for (uint32_t i = 0; i < n; ++i) {
        void* type = nullptr;
        std::string key;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &type) || !type ||
            !SafeReadPdxString((const void*)((uintptr_t)type + 0x20), key) || key.empty()) {
            continue;
        }
        bool ok = false;
        nlohmann::json j = row(type, key, &ok);
        if (ok) buildable.push_back(j);
    }
    nlohmann::json current = nlohmann::json::array();
    for (const auto& [key, count] : built) current.push_back({ {"key", key}, {"name", LocalizeKey(key)}, {"built", count} });
    return { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid}, {"current", current},
             {"buildable", buildable} };
}

nlohmann::json OutlinerManager::BuildDistrictJson(uint32_t planet_id, const std::string& district_key, uint32_t colony_id) {
    if (!base_address_ || !fn_post_command_) return { {"success", false}, {"error", "Engine functions not initialized"} };
    void* type = FindDbElementByKey(base_address_, kDistrictTypeDb, district_key);
    if (!type) return { {"success", false}, {"error", "District type '" + district_key + "' not found (see stellaris_get_buildable_districts)"} };
    uint32_t cid = 0xFFFFFFFF, queue_id = 0xFFFFFFFF;
    std::string error;
    if (!PlanetColonyQueue(planet_id, &cid, &queue_id, &error, colony_id)) return { {"success", false}, {"error", error} };

    uint8_t obj[0x20];
    FillDistrictBuildable(obj, type, cid);
    nlohmann::json cost = BuildableCostJson(obj);
    std::string why;
    if (!QueueBuildable(obj, sizeof(obj), GetPlayerCountryId(), queue_id, true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "Cannot be queued" : "Cannot be queued: " + why},
                 {"planet_id", planet_id}, {"district_key", district_key} };
    }
    nlohmann::json out = { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid}, {"queue_id", queue_id},
                           {"district_key", district_key}, {"district_name", LocalizeKey(district_key)},
                           {"message", "District queued"} };
    out.update(cost);
    return out;
}

// CRemoveBuildableFromQueueCommand: takes one item out of a construction queue (a colony's or a
// shipyard's; item_id as the queues list it), as the queue's cancel button
nlohmann::json OutlinerManager::CancelConstructionJson(uint32_t item_id) {
    // the token has two vtables; this one has a real factory (the other's is a clone)
    namespace rq = sdk::cmd::remove_buildable_from_queue_command_2;
    auto cmd = CommandBuilder::Get().Create(rq::kSpec);
    cmd.Set<uint32_t>(rq::country, GetPlayerCountryId()).Set<uint32_t>(rq::item, item_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused to cancel item " + std::to_string(item_id) : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"item_id", item_id}, {"message", "Construction item removed from its queue"} };
}

// CDestroyDistrictCommand: removes one district of the type from the colony
nlohmann::json OutlinerManager::DemolishDistrictJson(uint32_t planet_id, const std::string& district_key,
                                                     uint32_t colony_id) {
    void* type = FindDbElementByKey(base_address_, kDistrictTypeDb, district_key);
    if (!type) return { {"success", false}, {"error", "District type '" + district_key + "' not found"} };
    uint32_t cid = 0xFFFFFFFF, queue_id = 0xFFFFFFFF;
    std::string error;
    if (!PlanetColonyQueue(planet_id, &cid, &queue_id, &error, colony_id)) return { {"success", false}, {"error", error} };
    namespace dd = sdk::cmd::destroy_district_command;
    auto cmd = CommandBuilder::Get().Create(dd::kSpec);
    cmd.Set<uint32_t>(dd::colony, cid).Set<void*>(dd::district, type);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused to demolish it" : why},
                 {"planet_id", planet_id}, {"district_key", district_key} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"planet_id", planet_id}, {"colony_id", cid}, {"district_key", district_key},
             {"message", "District demolished"} };
}

// District type key -> count on the colony (one CDistrict per type; its count is the save's "level")
std::vector<std::pair<std::string, int>> OutlinerManager::ColonyDistrictCounts(void* colony_obj) {
    std::vector<std::pair<std::string, int>> out;
    void* d_mgr = nullptr;
    void* d_arr = nullptr;
    uint32_t d_cap = 0;
    if (!colony_obj || !SafeReadPtr((const void*)(base_address_ + sdk::db::CDistrict), &d_mgr) || !d_mgr ||
        !SafeReadPtr((const void*)((uintptr_t)d_mgr + 0x18), &d_arr) || !d_arr ||
        !SafeReadU32((const void*)((uintptr_t)d_mgr + 0x20), &d_cap)) {
        return out;
    }
    for (uint32_t i = 0; i < d_cap && i < 100000; ++i) {
        void* d = nullptr;
        void* col = nullptr;
        void* def = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)d_arr + i * 16 + 8), &d) || !d) continue;
        if (!SafeReadPtr((const void*)((uintptr_t)d + 0x18), &col) || col != colony_obj) continue;
        std::string key;
        uint32_t count = 0;
        if (SafeReadPtr((const void*)((uintptr_t)d + sdk::ent::CDistrict::type), &def) && def) {
            SafeReadPdxString((const void*)((uintptr_t)def + 0x20), key);
        }
        SafeReadU32((const void*)((uintptr_t)d + sdk::ent::CDistrict::level), &count);
        if (!key.empty()) out.push_back({ key, (int)count });
    }
    return out;
}

nlohmann::json OutlinerManager::BuildBuildingJson(uint32_t planet_id, const std::string& building_key, const std::string& district_type, int32_t slot_index, uint32_t colony_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    void* bldg_def = FindDbElementByKey(base_address_, kBuildingTypeDb, building_key);
    if (!bldg_def) {
        return { {"error", "Building definition not found: " + building_key} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return { {"error", "Player country not found"} };
    }
    uint32_t country_id = GetPlayerCountryId();

    // Planet ID (planet_id) -> Colony ID (cid) & Queue ID from Planet DB (sdk::db::CPlanet)
    uint32_t cid = 0xFFFFFFFF;
    uint32_t queue_id = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
        queue_id = GetColonyQueueId(cid);
        if (queue_id == 0xFFFFFFFF) {
            return { {"error", "Colony " + std::to_string(cid) + " has no construction queue"} };
        }
    } else {
        void* p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"error", "Planet with ID " + std::to_string(planet_id) + " not found"} };
        }
        if (!SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid) || cid == 0xFFFFFFFF) {
            return { {"error", "Planet has no active colony"} };
        }
        if (!SafeReadU32((const void*)((uintptr_t)p_obj + 0xe4), &queue_id) || queue_id == 0xFFFFFFFF) {
            return { {"error", "Planet has no active construction queue"} };
        }
    }

    // Target zone: the zone_id given (see get_planet_details districts[].zones[].slot_index or
    // get_buildable_buildings), else the first zone of the colony where the game accepts it.
    uint8_t action_obj[0x20];
    uint32_t target_zone_id = 0xFFFFFFFF;
    std::string zone_why;
    if (slot_index >= 0) {
        target_zone_id = (uint32_t)slot_index;
    } else {
        for (const auto& z : ColonyZones(FindColony(cid))) {
            FillBuildable(action_obj, bldg_def, cid, z.id);
            if (QueueBuildable(action_obj, sizeof(action_obj), country_id, queue_id, false, &zone_why)) {
                target_zone_id = z.id;
                break;
            }
        }
        if (target_zone_id == 0xFFFFFFFF) {
            return { {"success", false},
                     {"error", zone_why.empty() ? "No zone on this planet accepts " + building_key
                                                : "No zone on this planet accepts " + building_key + ": " + zone_why},
                     {"planet_id", planet_id}, {"building_key", building_key} };
        }
    }
    FillBuildable(action_obj, bldg_def, cid, target_zone_id);

    // CAddBuildableToQueueCommand takes ownership of the buildable (engine heap copy)
    std::string why;
    if (!QueueBuildable(action_obj, sizeof(action_obj), country_id, queue_id, true, &why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Cannot be queued (prerequisites not met)" : "Cannot be queued: " + why},
            {"planet_id", planet_id},
            {"queue_id", queue_id},
            {"building_key", building_key}
        };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"zone_id", target_zone_id},
        {"building_key", building_key},
        {"building_name", LocalizeKey(building_key)},
        {"country_id", country_id},
        {"queue_id", queue_id},
        {"message", "Building successfully queued"}
    };
}

nlohmann::json OutlinerManager::UpgradeBuildingJson(uint32_t planet_id, uint32_t building_id,
                                                   const std::string& upgrade_to_key, uint32_t colony_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return { {"error", "Player country not found"} };
    }
    uint32_t country_id = GetPlayerCountryId();

    // Planet ID -> Colony ID & Queue ID（方舟走 colony_id：队列在方舟舰 CShip::build_queue +0x104）
    uint32_t cid = 0xFFFFFFFF;
    uint32_t queue_id = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
        queue_id = GetColonyQueueId(cid);
        if (queue_id == 0xFFFFFFFF) {
            return { {"error", "Colony " + std::to_string(cid) + " has no construction queue"} };
        }
    } else {
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet with ID " + std::to_string(planet_id) + " not found"} };
    }

    if (!SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid) || cid == 0xFFFFFFFF) {
        return { {"error", "Planet has no active colony"} };
    }

    if (!SafeReadU32((const void*)((uintptr_t)p_obj + 0xe4), &queue_id) || queue_id == 0xFFFFFFFF) {
        return { {"error", "Planet has no active construction queue"} };
    }
    }

    // Locate the existing building in CBuilding database (sdk::db::CBuilding)
    void* b_mgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CBuilding), &b_mgr);
    if (!b_mgr) {
        return { {"error", "Building manager database (sdk::db::CBuilding) not found"} };
    }

    void* b_arr = nullptr;
    uint32_t b_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)b_mgr + 0x18), &b_arr);
    SafeReadU32((const void*)((uintptr_t)b_mgr + 0x20), &b_cap);



    void* target_b_obj = nullptr;
    uint32_t target_zone_id = 0;
    std::string current_building_key = "";

    for (uint32_t bs = 0; bs < b_cap; ++bs) {
        void* b_obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)b_arr + bs * 16 + 8), &b_obj) || !b_obj) continue;
        uint32_t b_id = 0;
        SafeReadU32((const void*)((uintptr_t)b_obj + 8), &b_id);
        if (b_id == building_id) {
            target_b_obj = b_obj;
            void* p_zone = nullptr;
            SafeReadPtr((const void*)((uintptr_t)b_obj + 0x18), &p_zone);
            if (p_zone) {
                SafeReadU32((const void*)((uintptr_t)p_zone + 8), &target_zone_id);
            }
            void* b_def = nullptr;
            SafeReadPtr((const void*)((uintptr_t)b_obj + 0x20), &b_def);
            if (b_def) {
                SafeReadPdxString((const void*)((uintptr_t)b_def + 0x20), current_building_key);
            }
            break;
        }
    }

    if (!target_b_obj) {
        return { {"error", "Building ID " + std::to_string(building_id) + " not found on planet"} };
    }

    // Determine target upgrade building key
    std::string resolved_upgrade_key = upgrade_to_key;
    if (resolved_upgrade_key.empty()) {
        static const std::unordered_map<std::string, std::string> kUpgradeMap = {
            {"building_holo_theatres", "building_hyper_entertainment_forum"},
            {"building_commercial_zone", "building_commercial_megaplex"},
            {"building_physics_lab_1", "building_physics_lab_2"},
            {"building_factory_1", "building_factory_2"},
            {"building_foundry_1", "building_foundry_2"},
            {"building_biolab_1", "building_biolab_2"},
            {"building_engineering_facility_1", "building_engineering_facility_2"},
            {"building_energy_grid", "building_energy_nexus"},
            {"building_mineral_purification_plant", "building_mineral_purification_hub"},
            {"building_food_processing_facility", "building_food_processing_center"},
            {"building_autochthon_monument", "building_heritage_site"},
            {"building_capital", "building_capital_2"}
        };
        auto it = kUpgradeMap.find(current_building_key);
        if (it != kUpgradeMap.end()) {
            resolved_upgrade_key = it->second;
        } else {
            return { {"error", "No known upgrade path for building: " + current_building_key + ". Please specify upgrade_to_key explicitly."} };
        }
    }

    void* target_bldg_def = FindDbElementByKey(base_address_, kBuildingTypeDb, resolved_upgrade_key);
    if (!target_bldg_def) {
        return { {"error", "Target upgrade building definition not found: " + resolved_upgrade_key} };
    }

    // 1. Construct CBuildableUpgradeBuilding (0x20 bytes) on stack
    uint8_t action_obj[0x20];
    memset(action_obj, 0, sizeof(action_obj));
    *(void**)(action_obj + 0x00) = (void*)(base_address_ + kBuildableUpgradeBuildingVt); // CBuildableUpgradeBuilding concrete vtable (4.5.1 Cygnus)
    *(void**)(action_obj + 0x08) = target_bldg_def;                   // target CBuildingType*
    *(uint32_t*)(action_obj + 0x10) = cid;                            // colony_id
    *(uint32_t*)(action_obj + 0x14) = target_zone_id;                 // zone_id
    *(uint32_t*)(action_obj + 0x18) = building_id;                    // existing building_id (bid)
    *(uint32_t*)(action_obj + 0x1C) = 0;

    // CAddBuildableToQueueCommand takes ownership of the buildable (engine heap copy)
    std::string why;
    if (!QueueBuildable(action_obj, sizeof(action_obj), country_id, queue_id, true, &why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Cannot be queued (prerequisites not met)" : "Cannot be queued: " + why},
            {"planet_id", planet_id},
            {"queue_id", queue_id}
        };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"zone_id", target_zone_id},
        {"building_id", building_id},
        {"current_building_key", current_building_key},
        {"upgrade_to_key", resolved_upgrade_key},
        {"upgrade_to_name", LocalizeKey(resolved_upgrade_key)},
        {"country_id", country_id},
        {"queue_id", queue_id},
        {"message", "Building upgrade successfully queued"}
    };
}

std::string OutlinerManager::LocalizeModifierType(uint32_t mod_type_id) {
    if (!base_address_) return "";
    void* defs_arr = nullptr;
    uint32_t defs_cnt = 0;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::CModifier_Definitions), &defs_arr);
    SafeReadU32((const void*)(base_address_ + sdk::glob::CModifier_Definitions + 0xC), &defs_cnt);
    if (!defs_arr || mod_type_id >= defs_cnt) return "";

    void* entry = (void*)((uintptr_t)defs_arr + mod_type_id * 0xB0);
    std::string key;
    SafeReadPdxString((const void*)((uintptr_t)entry + 0x10), key);
    if (key.empty()) return "";

    std::string loc = LocalizeKey(key);
    return (loc.empty() || loc == key) ? key : loc;
}

static nlohmann::json ReadDepositModifiers(uintptr_t base_address, void* tptr, OutlinerManager* mgr) {
    nlohmann::json mods_json = nlohmann::json::array();
    if (!tptr) return mods_json;

    void* p_types = nullptr;
    uint32_t cnt1 = 0;
    void* p_vals = nullptr;
    uint32_t cnt2 = 0;

    SafeReadPtr((const void*)((uintptr_t)tptr + 0x858), &p_types);
    SafeReadU32((const void*)((uintptr_t)tptr + 0x864), &cnt1);
    SafeReadPtr((const void*)((uintptr_t)tptr + 0x880), &p_vals);
    SafeReadU32((const void*)((uintptr_t)tptr + 0x88C), &cnt2);

    if (p_types && p_vals && cnt1 > 0 && cnt1 <= 32) {
        uint32_t limit = (std::min)(cnt1, cnt2);
        for (uint32_t m = 0; m < limit; ++m) {
            uint32_t mtype = 0;
            int64_t mval = 0;
            if (SafeReadU32((const void*)((uintptr_t)p_types + m * 4), &mtype) &&
                SafeReadI64((const void*)((uintptr_t)p_vals + m * 8), &mval)) {
                double val = (double)mval / 100000.0;

                std::string mod_key;
                void* defs_arr = nullptr;
                uint32_t defs_cnt = 0;
                SafeReadPtr((const void*)(base_address + sdk::glob::CModifier_Definitions), &defs_arr);
                SafeReadU32((const void*)(base_address + sdk::glob::CModifier_Definitions + 0xC), &defs_cnt);
                if (defs_arr && mtype < defs_cnt) {
                    void* entry = (void*)((uintptr_t)defs_arr + mtype * 0xB0);
                    SafeReadPdxString((const void*)((uintptr_t)entry + 0x10), mod_key);
                }

                std::string mod_name = mgr->LocalizeModifierType(mtype);
                if (mod_name.empty()) {
                    mod_name = mod_key.empty() ? ("Modifier 0x" + std::to_string(mtype)) : mod_key;
                }

                char val_buf[32];
                if (std::abs(val - std::round(val)) < 0.0001) {
                    snprintf(val_buf, sizeof(val_buf), "%+d", (int)std::round(val));
                } else {
                    snprintf(val_buf, sizeof(val_buf), "%+.2f", val);
                }

                mods_json.push_back({
                    {"type_id", mtype},
                    {"key", mod_key},
                    {"name", mod_name},
                    {"value", val},
                    {"formatted", mod_name + ": " + val_buf}
                });
            }
        }
    }
    return mods_json;
}

static nlohmann::json ReadClearCost(void* tptr) {
    // CDepositType +0xA8: the clearing cost, a per-resource CFixedPoint array indexed by the
    // engine's resource index (vtable +0, data +8, size +0x14).
    nlohmann::json cost = nlohmann::json::object();
    void* p_econ = nullptr;
    if (!tptr || !SafeReadPtr((const void*)((uintptr_t)tptr + 0xA8), &p_econ) || !p_econ) return cost;

    void* p_res = nullptr;
    uint32_t res_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)p_econ + 0x8), &p_res);
    SafeReadU32((const void*)((uintptr_t)p_econ + 0x14), &res_cnt);
    const auto& names = GameState::Get().ResourceNames();
    if (!p_res || res_cnt == 0) return cost;
    for (uint32_t r = 0; r < res_cnt && r < names.size(); ++r) {
        int64_t raw_val = 0;
        if (!names[r].empty() && SafeReadI64((const void*)((uintptr_t)p_res + r * 8), &raw_val) && raw_val > 0) {
            cost[names[r]] = raw_val / 100000.0;
        }
    }
    return cost;
}

nlohmann::json OutlinerManager::ExtractPlanetaryFeatures(void* p_obj, uint32_t cid, uint32_t queue_id, uint32_t country_id) {
    nlohmann::json features = nlohmann::json::array();
    if (!p_obj || !base_address_) {
        return {
            {"summary", {
                {"total_features", 0},
                {"blockers_count", 0},
                {"natural_features_count", 0},
                {"clearable_blockers_count", 0},
                {"queued_blockers_count", 0}
            }},
            {"features", features}
        };
    }

    // 1. Gather all deposit blockers currently queued on this planet
    std::vector<uint32_t> queued_blocker_ids;
    if (queue_id != 0xFFFFFFFF) {
        void* mgr_eb8 = nullptr;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8) && mgr_eb8) {
            void* arr_eb8 = nullptr;
            uint32_t cap_eb8 = 0;
            uint32_t q_slot = queue_id & 0xFFFFFF;
            if (SafeReadPtr((const void*)((uintptr_t)mgr_eb8 + 0x18), &arr_eb8) && arr_eb8 &&
                SafeReadU32((const void*)((uintptr_t)mgr_eb8 + 0x20), &cap_eb8) && q_slot < cap_eb8) {
                void* queue_obj = nullptr;
                if (SafeReadPtr((const void*)((uintptr_t)arr_eb8 + q_slot * 16 + 8), &queue_obj) && queue_obj) {
                    void* q_items = nullptr;
                    uint32_t q_cnt = 0;
                    SafeReadPtr((const void*)((uintptr_t)queue_obj + 0x20), &q_items);
                    SafeReadU32((const void*)((uintptr_t)queue_obj + 0x2C), &q_cnt);
                    if (q_items && q_cnt > 0 && q_cnt <= 200) {
                        void* mgr_ea8 = nullptr;
                        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8) && mgr_ea8) {
                            void* arr_ea8 = nullptr;
                            uint32_t cap_ea8 = 0;
                            SafeReadPtr((const void*)((uintptr_t)mgr_ea8 + 0x18), &arr_ea8);
                            SafeReadU32((const void*)((uintptr_t)mgr_ea8 + 0x20), &cap_ea8);
                            if (arr_ea8 && cap_ea8 > 0) {
                                for (uint32_t i = 0; i < q_cnt; ++i) {
                                    uint32_t item_id = 0;
                                    if (!SafeReadU32((const void*)((uintptr_t)q_items + i * sizeof(uint32_t)), &item_id)) continue;
                                    uint32_t i_slot = item_id & 0xFFFFFF;
                                    if (i_slot >= cap_ea8) continue;
                                    void* item_obj = nullptr;
                                    if (!SafeReadPtr((const void*)((uintptr_t)arr_ea8 + i_slot * 16 + 8), &item_obj) || !item_obj) continue;
                                    void* action_obj = nullptr;
                                    SafeReadPtr((const void*)((uintptr_t)item_obj + 0x18), &action_obj);
                                    if (action_obj) {
                                        void* act_vt = nullptr;
                                        SafeReadPtr(action_obj, &act_vt);
                                        if (act_vt == (void*)(base_address_ + kBuildableClearDepositBlockerVt)) {
                                            uint32_t dep_id = 0;
                                            SafeReadU32((const void*)((uintptr_t)action_obj + 8), &dep_id);
                                            queued_blocker_ids.push_back(dep_id);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 2. Read planet deposits
    void* dep_arr = nullptr;
    uint32_t dep_count = 0;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0x60), &dep_arr);
    SafeReadU32((const void*)((uintptr_t)p_obj + 0x6C), &dep_count);
    if (!dep_arr || dep_count == 0) {
        return {
            {"summary", {
                {"total_features", 0},
                {"blockers_count", 0},
                {"natural_features_count", 0},
                {"clearable_blockers_count", 0},
                {"queued_blockers_count", 0}
            }},
            {"features", features}
        };
    }

    void* dep_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &dep_mgr) || !dep_mgr || (uintptr_t)dep_mgr < 0x10000) {
        SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &dep_mgr);
    }
    if (!dep_mgr) {
        return {
            {"summary", {
                {"total_features", 0},
                {"blockers_count", 0},
                {"natural_features_count", 0},
                {"clearable_blockers_count", 0},
                {"queued_blockers_count", 0}
            }},
            {"features", features}
        };
    }

    void* dep_db_arr = nullptr;
    uint32_t dep_db_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)dep_mgr + 0x18), &dep_db_arr);
    SafeReadU32((const void*)((uintptr_t)dep_mgr + 0x20), &dep_db_cap);
    if (!dep_db_arr || dep_db_cap == 0) {
        return {
            {"summary", {
                {"total_features", 0},
                {"blockers_count", 0},
                {"natural_features_count", 0},
                {"clearable_blockers_count", 0},
                {"queued_blockers_count", 0}
            }},
            {"features", features}
        };
    }

    uint32_t blockers_count = 0;
    uint32_t natural_features_count = 0;
    uint32_t clearable_blockers_count = 0;
    uint32_t queued_blockers_count = 0;

    for (uint32_t i = 0; i < dep_count; ++i) {
        uint32_t dep_id = 0;
        if (!SafeReadU32((const void*)((uintptr_t)dep_arr + i * sizeof(uint32_t)), &dep_id)) continue;
        if (dep_id >= dep_db_cap) continue;

        void* dep_obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dep_db_arr + dep_id * 16 + 8), &dep_obj) || !dep_obj) continue;

        void* type_ptr = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dep_obj + 0x18), &type_ptr) || !type_ptr) continue;

        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)type_ptr + 0x20), key);

        void* cat_ptr = nullptr;
        SafeReadPtr((const void*)((uintptr_t)type_ptr + 0x80), &cat_ptr);
        std::string cat_key;
        if (cat_ptr) {
            SafeReadPdxString((const void*)((uintptr_t)cat_ptr + 0x20), cat_key);
        }

        bool is_blocker = (cat_key.rfind("deposit_blockers", 0) == 0 || cat_key.find("blocker") != std::string::npos);
        if (is_blocker) blockers_count++;
        else natural_features_count++;

        bool is_queued = false;
        bool can_clear = false;
        std::string status_text;

        if (is_blocker) {
            is_queued = (std::find(queued_blocker_ids.begin(), queued_blocker_ids.end(), dep_id) != queued_blocker_ids.end());
            if (is_queued) {
                queued_blockers_count++;
                status_text = "这个障碍已经在清除中了。";
            } else if (queue_id != 0xFFFFFFFF) {
                uint8_t action_obj[0x20];
                memset(action_obj, 0, sizeof(action_obj));
                *(void**)(action_obj + 0x00) = (void*)(base_address_ + kBuildableClearDepositBlockerVt);
                *(uint32_t*)(action_obj + 0x08) = dep_id;
                *(uint32_t*)(action_obj + 0x0C) = cid;

                can_clear = QueueBuildable(action_obj, sizeof(action_obj), country_id, queue_id, false, nullptr);
                if (can_clear) {
                    clearable_blockers_count++;
                    status_text = "可清理";
                } else {
                    status_text = "未满足清理前提条件";
                }
            }
        }

        uint32_t clear_time_val = 0;
        SafeReadU32((const void*)((uintptr_t)type_ptr + 0xA64), &clear_time_val);
        int32_t clear_time = (clear_time_val == 0xFFFFFFFF || clear_time_val > 10000) ? 0 : (int32_t)clear_time_val;

        nlohmann::json clear_cost = ReadClearCost(type_ptr);
        nlohmann::json modifiers = ReadDepositModifiers(base_address_, type_ptr, this);

        // Blocker swap type
        nlohmann::json swap_json = nullptr;
        void* swap_type_ptr = nullptr;
        SafeReadPtr((const void*)((uintptr_t)dep_obj + 0x20), &swap_type_ptr);
        if (swap_type_ptr) {
            std::string swap_key;
            SafeReadPdxString((const void*)((uintptr_t)swap_type_ptr + 0x20), swap_key);
            if (!swap_key.empty()) {
                nlohmann::json swap_mods = ReadDepositModifiers(base_address_, swap_type_ptr, this);
                swap_json = {
                    {"key", swap_key},
                    {"name", LocalizeKey(swap_key)},
                    {"description", LocalizeKey(swap_key + "_desc")},
                    {"modifiers", swap_mods}
                };
            }
        }

        std::string loc_name = LocalizeKey(key);
        if (loc_name.empty()) loc_name = key;
        std::string loc_desc = LocalizeKey(key + "_desc");

        features.push_back({
            {"deposit_id", dep_id},
            {"key", key},
            {"name", loc_name},
            {"description", loc_desc},
            {"category", cat_key},
            {"is_blocker", is_blocker},
            {"is_queued", is_queued},
            {"can_clear", can_clear},
            {"clear_time", clear_time},
            {"clear_cost", clear_cost},
            {"modifiers", modifiers},
            {"swap_type", swap_json},
            {"status_text", status_text}
        });
    }

    return {
        {"summary", {
            {"total_features", (uint32_t)features.size()},
            {"blockers_count", blockers_count},
            {"natural_features_count", natural_features_count},
            {"clearable_blockers_count", clearable_blockers_count},
            {"queued_blockers_count", queued_blockers_count}
        }},
        {"features", features}
    };
}

nlohmann::json OutlinerManager::GetPlanetaryFeaturesJson(uint32_t planet_id) {
    if (!base_address_) {
        return { {"error", "Base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    uint32_t queue_id = GetPlanetQueueId(planet_id);
    uint32_t country_id = GetPlayerCountryId();

    return ExtractPlanetaryFeatures(p_obj, cid, queue_id, country_id);
}

nlohmann::json OutlinerManager::GetClearableBlockersJson(uint32_t planet_id) {
    if (!base_address_) {
        return { {"error", "Base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) {
        return { {"error", "Planet has no active colony: " + std::to_string(planet_id)} };
    }

    uint32_t queue_id = GetPlanetQueueId(planet_id);
    uint32_t country_id = GetPlayerCountryId();

    auto features_data = ExtractPlanetaryFeatures(p_obj, cid, queue_id, country_id);
    nlohmann::json blockers = nlohmann::json::array();
    if (features_data.contains("features") && features_data["features"].is_array()) {
        for (const auto& feat : features_data["features"]) {
            if (feat.value("is_blocker", false)) {
                blockers.push_back(feat);
            }
        }
    }

    return {
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"blockers", blockers}
    };
}

nlohmann::json OutlinerManager::ClearBlockerJson(uint32_t planet_id, uint32_t deposit_id, const std::string& deposit_key) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) {
        return { {"error", "Planet has no active colony: " + std::to_string(planet_id)} };
    }

    uint32_t queue_id = GetPlanetQueueId(planet_id);
    if (queue_id == 0xFFFFFFFF) {
        return { {"error", "Planet has no valid construction queue"} };
    }

    uint32_t country_id = GetPlayerCountryId();

    // Read planet deposits
    void* dep_arr = nullptr;
    uint32_t dep_count = 0;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0x60), &dep_arr);
    SafeReadU32((const void*)((uintptr_t)p_obj + 0x6C), &dep_count);
    if (!dep_arr || dep_count == 0) {
        return { {"error", "No deposits found on planet"} };
    }

    // Deposit database at sdk::db::CDeposit
    void* dep_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &dep_mgr) || !dep_mgr || (uintptr_t)dep_mgr < 0x10000) {
        SafeReadPtr((const void*)(base_address_ + sdk::db::CDeposit), &dep_mgr);
    }
    if (!dep_mgr) return { {"error", "Deposit manager not found"} };
    void* dep_db_arr = nullptr;
    uint32_t dep_db_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)dep_mgr + 0x18), &dep_db_arr);
    SafeReadU32((const void*)((uintptr_t)dep_mgr + 0x20), &dep_db_cap);
    if (!dep_db_arr || dep_db_cap == 0) return { {"error", "Deposit database empty"} };

    uint32_t target_deposit_id = deposit_id;
    std::string target_deposit_key = deposit_key;
    if (target_deposit_key == "slums" || target_deposit_key == "d_slums" || target_deposit_key == "贫民窟" || target_deposit_key == "破旧民居") {
        target_deposit_key = "d_decrepit_dwellings";
    } else if (target_deposit_key == "failing_infrastructure" || target_deposit_key == "工业废土" || target_deposit_key == "衰退的基础设施") {
        target_deposit_key = "d_failing_infrastructure_earth";
    } else if (target_deposit_key == "garbage_patch" || target_deposit_key == "太平洋垃圾带") {
        target_deposit_key = "d_great_pacific_garbage_patch";
    }

    // Resolve target deposit
    bool found = false;
    for (uint32_t i = 0; i < dep_count; ++i) {
        uint32_t did = 0;
        if (!SafeReadU32((const void*)((uintptr_t)dep_arr + i * sizeof(uint32_t)), &did)) continue;
        if (did >= dep_db_cap) continue;

        void* dep_obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dep_db_arr + did * 16 + 8), &dep_obj) || !dep_obj) continue;
        void* type_ptr = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dep_obj + 0x18), &type_ptr) || !type_ptr) continue;

        std::string k;
        SafeReadPdxString((const void*)((uintptr_t)type_ptr + 0x20), k);

        if (target_deposit_id != 0 && did == target_deposit_id) {
            target_deposit_key = k;
            found = true;
            break;
        } else if (target_deposit_id == 0 && !target_deposit_key.empty() && k == target_deposit_key) {
            target_deposit_id = did;
            found = true;
            break;
        }
    }

    if (!found || target_deposit_id == 0) {
        return {
            {"success", false},
            {"error", "Blocker deposit not found on this planet (deposit_id: " + std::to_string(deposit_id) + ", deposit_key: '" + deposit_key + "')"}
        };
    }

    // 1. Construct CBuildableClearDepositBlocker (0x20 bytes) on stack
    uint8_t action_obj[0x20];
    memset(action_obj, 0, sizeof(action_obj));
    *(void**)(action_obj + 0x00) = (void*)(base_address_ + kBuildableClearDepositBlockerVt); // CBuildableClearDepositBlocker vtable
    *(uint32_t*)(action_obj + 0x08) = target_deposit_id;              // deposit_id
    *(uint32_t*)(action_obj + 0x0C) = cid;                            // colony_id
    *(uint64_t*)(action_obj + 0x10) = 0;
    *(uint64_t*)(action_obj + 0x18) = 0;

    // CAddBuildableToQueueCommand takes ownership of the buildable (engine heap copy)
    std::string why;
    if (!QueueBuildable(action_obj, sizeof(action_obj), country_id, queue_id, true, &why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Cannot be queued (prerequisites not met)" : "Cannot be queued: " + why},
            {"planet_id", planet_id},
            {"queue_id", queue_id}
        };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"deposit_id", target_deposit_id},
        {"deposit_key", target_deposit_key},
        {"deposit_name", LocalizeKey(target_deposit_key)},
        {"country_id", country_id},
        {"queue_id", queue_id},
        {"message", "Deposit blocker clearance order successfully queued"}
    };
}

nlohmann::json OutlinerManager::ExtractMonthlyPopulationSummary(void* colony_obj) {
    if (!colony_obj) {
        return {
            {"net_change", 0},
            {"growth", 0},
            {"migration", 0},
            {"assembly", 0},
            {"categories", nlohmann::json::object()},
            {"demographics", nlohmann::json::array()}
        };
    }

    int32_t net_change = 0;
    SafeReadI32((const void*)((uintptr_t)colony_obj + 0xFEC), &net_change);

    // Robin Hood table at colony_obj + 0x1010
    void* table_entries = nullptr;
    uint32_t mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x1018), &table_entries);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1024), &mask);

    int32_t growth_raw = 0;
    int32_t decline_raw = 0;
    int32_t immigration_raw = 0;
    int32_t emigration_raw = 0;
    int32_t assembly_raw = 0;
    nlohmann::json categories = nlohmann::json::object();

    if (table_entries && mask > 0 && mask < 1024) {
        for (uint32_t i = 0; i <= mask; ++i) {
            void* entry = (void*)((uintptr_t)table_entries + i * 0x40);
            uint32_t dist_raw = 0;
            SafeReadU32((const void*)((uintptr_t)entry + 4), &dist_raw);
            uint8_t dist = (uint8_t)(dist_raw & 0xFF);
            if (dist != 0xFF && dist != 0) {
                std::string cat_k;
                SafeReadPdxString((const void*)((uintptr_t)entry + 0x18), cat_k);
                int32_t val = 0;
                SafeReadI32((const void*)((uintptr_t)entry + 0x38), &val);
                if (!cat_k.empty()) {
                    categories[cat_k] = val;
                    if (cat_k == "GROWTH_CAT_GROWTH") growth_raw = val;
                    else if (cat_k == "GROWTH_CAT_DECLINE") decline_raw = val;
                    else if (cat_k == "GROWTH_CAT_IMMIGRATION") immigration_raw = val;
                    else if (cat_k == "GROWTH_CAT_EMIGRATION") emigration_raw = val;
                    else if (cat_k == "GROWTH_CAT_ASSEMBLY") assembly_raw = val;
                }
            }
        }
    }

    int32_t migration = immigration_raw - emigration_raw;
    int32_t growth = growth_raw - decline_raw;
    int32_t assembly = assembly_raw;

    // Demographics for pie chart from species array
    nlohmann::json demographics = nlohmann::json::array();
    void* sp_arr = nullptr;
    uint32_t sp_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xF68), &sp_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xF70), &sp_cnt);

    uint64_t total_pops = 0;
    std::vector<std::pair<uint32_t, uint32_t>> species_pops; // (species_id, count)
    if (sp_arr && sp_cnt > 0 && sp_cnt < 256) {
        for (uint32_t i = 0; i < sp_cnt; ++i) {
            uint32_t count = 0;
            uint32_t sp_handle = 0;
            SafeReadU32((const void*)((uintptr_t)sp_arr + i * 12), &count);
            SafeReadU32((const void*)((uintptr_t)sp_arr + i * 12 + 8), &sp_handle);
            uint32_t sid = sp_handle;  // full species id, as get_species and the species commands use it
            species_pops.push_back({ sid, count });
            total_pops += count;
        }
    }

    for (const auto& sp : species_pops) {
        double share = total_pops > 0 ? ((double)sp.second * 100.0 / (double)total_pops) : 100.0;
        std::string sp_name = "Species " + std::to_string(sp.first);
        void* sp_ptr = SpeciesManager::Get().FindSpeciesPtr(sp.first);
        if (sp_ptr) {
            std::string raw_name;
            SafeReadPdxString((const void*)((uintptr_t)sp_ptr + 0x60), raw_name);
            std::string loc_name = LocalizeKey(raw_name);
            if (!loc_name.empty()) sp_name = loc_name;
        }
        demographics.push_back({
            {"species_id", sp.first},
            {"species_name", sp_name},
            {"count", sp.second},
            {"share_percent", std::round(share * 10.0) / 10.0}
        });
    }

    return {
        {"net_change", net_change},
        {"growth", growth},
        {"migration", migration},
        {"assembly", assembly},
        {"categories", categories},
        {"demographics", demographics}
    };
}

nlohmann::json OutlinerManager::ExtractPopulationBreakdown(void* colony_obj) {
    nlohmann::json breakdown = nlohmann::json::array();
    if (!colony_obj) return breakdown;

    void* sp_arr = nullptr;
    uint32_t sp_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xF68), &sp_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xF70), &sp_cnt);
    if (!sp_arr || sp_cnt == 0 || sp_cnt > 256) return breakdown;

    // Per-species net change table at colony_obj + 0xFF8
    std::unordered_map<uint32_t, int32_t> sp_net_map;
    void* table_sp_entries = nullptr;
    uint32_t sp_mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xFF8), &table_sp_entries);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1004), &sp_mask);
    if (table_sp_entries && sp_mask > 0 && sp_mask < 1024) {
        for (uint32_t i = 0; i <= sp_mask; ++i) {
            void* entry = (void*)((uintptr_t)table_sp_entries + i * 0x40);
            uint32_t dist_raw = 0;
            SafeReadU32((const void*)((uintptr_t)entry + 4), &dist_raw);
            uint8_t dist = (uint8_t)(dist_raw & 0xFF);
            if (dist != 0xFF && dist != 0) {
                uint32_t sp_handle = 0;
                int32_t net = 0;
                SafeReadU32((const void*)((uintptr_t)entry + 8), &sp_handle);
                SafeReadI32((const void*)((uintptr_t)entry + 0x24), &net);
                uint32_t sid = sp_handle;  // full species id, as get_species and the species commands use it
                if (sid != 0) sp_net_map[sid] = net;
            }
        }
    }

    int32_t colony_net = 0;
    SafeReadI32((const void*)((uintptr_t)colony_obj + 0xFEC), &colony_net);

    uint64_t total_pops = 0;
    for (uint32_t i = 0; i < sp_cnt; ++i) {
        uint32_t count = 0;
        SafeReadU32((const void*)((uintptr_t)sp_arr + i * 12), &count);
        total_pops += count;
    }

    for (uint32_t i = 0; i < sp_cnt; ++i) {
        uint32_t count = 0;
        uint32_t sp_handle = 0;
        SafeReadU32((const void*)((uintptr_t)sp_arr + i * 12), &count);
        SafeReadU32((const void*)((uintptr_t)sp_arr + i * 12 + 8), &sp_handle);
        uint32_t sid = sp_handle;  // full species id, as get_species and the species commands use it

        std::string sp_name = "Species " + std::to_string(sid);
        std::string portrait = "";
        void* sp_ptr = SpeciesManager::Get().FindSpeciesPtr(sid);
        if (sp_ptr) {
            std::string raw_name;
            SafeReadPdxString((const void*)((uintptr_t)sp_ptr + 0x60), raw_name);
            std::string loc_name = LocalizeKey(raw_name);
            if (!loc_name.empty()) sp_name = loc_name;
            SafeReadPdxString((const void*)((uintptr_t)sp_ptr + 0x190), portrait);
        }

        std::string pop_disp;
        if (count >= 1000) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1fK", (double)count / 1000.0);
            pop_disp = buf;
        } else {
            pop_disp = std::to_string(count);
        }

        double share = total_pops > 0 ? ((double)count * 100.0 / (double)total_pops) : 100.0;
        int32_t net = 0;
        auto it = sp_net_map.find(sid);
        if (it != sp_net_map.end()) {
            net = it->second;
        } else if (sp_cnt == 1) {
            net = colony_net;
        }

        breakdown.push_back({
            {"species_id", sid},
            {"species_name", sp_name},
            {"pop_count", count},
            {"pop_count_display", pop_disp},
            {"share_percent", std::round(share * 10.0) / 10.0},
            {"net_change", net},
            {"portrait", portrait}
        });
    }

    return breakdown;
}

nlohmann::json OutlinerManager::ExtractColonyAscension(void* colony_obj, uint32_t cid) {
    if (!colony_obj) {
        return {
            {"tier", 0},
            {"tier_name", "行星尚未飞升。"},
            {"designation_multiplier_percent", 0.0},
            {"can_ascend", false},
            {"description", "提升该殖民地的潜力，为子孙后代铸造一个更加繁荣、稳定、高效的星球。"},
            {"status_desc", "行星尚未飞升。"}
        };
    }

    int32_t tier = 0;
    SafeReadI32((const void*)((uintptr_t)colony_obj + 0x110), &tier);
    double mult = (double)tier * 25.0;

    bool can_ascend = false;
    if (base_address_ && cid != 0xFFFFFFFF) {
        namespace ascend = sdk::cmd::increase_planetary_ascension_tier;
        auto probe = CommandBuilder::Get().Create(ascend::kSpec);
        probe.Set<uint32_t>(ascend::colony, cid);
        can_ascend = probe.IsValid();
    }

    std::string tier_name = (tier <= 0) ? "行星尚未飞升。" : ("飞升等级: " + std::to_string(tier));
    std::string status_desc = (tier <= 0) ? "行星尚未飞升。" : ("行星已飞升至等级 " + std::to_string(tier) + "，特化效果提升 " + std::to_string((int)mult) + "%。");

    return {
        {"tier", tier},
        {"tier_name", tier_name},
        {"designation_multiplier_percent", mult},
        {"can_ascend", can_ascend},
        {"description", "提升该殖民地的潜力，为子孙后代铸造一个更加繁荣、稳定、高效的星球。"},
        {"status_desc", status_desc}
    };
}

nlohmann::json OutlinerManager::AscendColonyJson(uint32_t planet_id, uint32_t colony_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    // 方舟没有行星：给了 colony_id 就直接用（命令拿的就是 colony，不是 planet）
    uint32_t cid = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
    } else {
        void* p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"error", "Planet not found: " + std::to_string(planet_id)} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
        if (cid == 0xFFFFFFFF) {
            return { {"error", "Planet has no active colony: " + std::to_string(planet_id)} };
        }
    }

    void* colony_obj = FindColony(cid);
    if (!colony_obj) {
        return { {"error", "Colony object not found: " + std::to_string(cid)} };
    }

    namespace ascend = sdk::cmd::increase_planetary_ascension_tier;
    auto cmd = CommandBuilder::Get().Create(ascend::kSpec);
    cmd.Set<uint32_t>(ascend::colony, cid);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"is_valid", false},
            {"planet_id", planet_id},
            {"colony_id", cid},
            {"error", why.empty() ? "Ascension requirement not met" : "Ascension not possible: " + why}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    int32_t current_tier = 0;
    SafeReadI32((const void*)((uintptr_t)colony_obj + 0x110), &current_tier);

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"previous_tier", current_tier},
        {"target_tier", current_tier + 1},
        {"message", "Planetary ascension order successfully posted"}
    };
}

std::string OutlinerManager::LocalizeDecisionKey(const std::string& key) {
    return LocalizeKey(key);
}

double OutlinerManager::ColonizationProgress(void* colony) {
    struct Ctx {
        uintptr_t fn;
        void* colony;
        int64_t out;
    } ctx{ base_address_ + sdk::fn::CColony_CalcColonizationProgressPerc, colony, 0 };
    auto call = [](void* c, void*) {
        auto* x = (Ctx*)c;
        ((int64_t * (*)(void*, int64_t*)) x->fn)(x->colony, &x->out);
    };
    if (!CommandBuilder::Get().CallGuarded(call, &ctx)) return 0.0;
    return std::round(ctx.out / 100000.0 * 1000.0) / 1000.0;  // fixed point fraction
}

nlohmann::json OutlinerManager::SectorIdJson(const SectorGroup& s) {
    return s.unassigned ? nlohmann::json(nullptr) : nlohmann::json(s.sector_id);
}

std::string OutlinerManager::PlanetName(void* planet) {
    return planet ? PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name)) : "";
}

uint32_t OutlinerManager::PlanetSystemId(void* planet) {
    uint32_t sys_id = 0xFFFFFFFF;
    if (planet) {
        SafeReadU32((const void*)((uintptr_t)planet + sdk::ent::CPlanet::coordinate + sdk::ent::CCelestialCoordinate::origin),
                    &sys_id);
    }
    return sys_id;
}

std::string OutlinerManager::SystemName(uint32_t system_id) {
    void* sys = FindSystem(system_id);
    return sys ? PersistentNameText((const void*)((uintptr_t)sys + sdk::ent::CGalacticObject::name)) : "";
}

std::string OutlinerManager::LocalizePlanetClass(const std::string& key) {
    return LocalizeKey(key);
}

nlohmann::json OutlinerManager::GetPlanetaryDecisionsJson(uint32_t planet_id, uint32_t colony_id) {
    if (!base_address_) {
        return { {"error", "Base address not initialized"} };
    }

    // 方舟没有行星：给了 colony_id 就直接用（决议命令吃的本来就是 carrier）
    void* p_obj = nullptr;
    uint32_t cid = 0xFFFFFFFF;
    std::string planet_name;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
        planet_name = "(方舟)";
    } else {
        p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"error", "Planet not found: " + std::to_string(planet_id)} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
        planet_name = PlanetName(p_obj);
    }

    uint32_t country_id = GetPlayerCountryId();

    void* dec_mgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CDecisionsDatabase_pInstance), &dec_mgr);
    if (!dec_mgr) {
        return { {"error", "Decisions database not found"} };
    }

    uint32_t dec_count = 0;
    SafeReadU32((const void*)((uintptr_t)dec_mgr + 0x5C), &dec_count);
    void* dec_arr = nullptr;
    SafeReadPtr((const void*)((uintptr_t)dec_mgr + 0x50), &dec_arr);
    if (!dec_arr || dec_count == 0) {
        return {
            {"planet_id", planet_id},
            {"colony_id", cid},
            {"planet_name", planet_name},
            {"decisions", nlohmann::json::array()}
        };
    }

    // 探测用的 carrier：行星 {planet_id, 0}，方舟抄 colony 自己的 {ship_id, 1}
    uint64_t probe_carrier = (uint64_t)planet_id;
    if (colony_id != 0xFFFFFFFF) {
        void* colony = FindColony(colony_id);
        if (colony) SafeReadI64((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), (int64_t*)&probe_carrier);
    }
    nlohmann::json decisions = nlohmann::json::array();
    for (uint32_t i = 0; i < dec_count; ++i) {
        void* dec = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dec_arr + i * sizeof(void*)), &dec) || !dec) {
            continue;
        }

        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)dec + 0x20), key);
        if (key.empty()) continue;

        int32_t raw_days = 0;
        SafeReadI32((const void*)((uintptr_t)dec + 0xb58), &raw_days);
        uint32_t days = raw_days > 0 ? (uint32_t)raw_days : 0;

        // CEnactDecisionCommand::IsValid decides whether the decision can be enacted here
        namespace enact = sdk::cmd::enact_decision_planet_command;
        auto probe = CommandBuilder::Get().Create(enact::kSpec);
        probe.Set<void*>(enact::decision, dec)
             .Set<uint64_t>(enact::carrier, probe_carrier)
             .Set<uint32_t>(enact::country, country_id);
        bool can_enact = probe.IsValid();

        decisions.push_back({
            {"key", key},
            {"name", LocalizeDecisionKey(key)},
            {"days", days},
            {"can_enact", can_enact}
        });
    }

    return {
        {"planet_id", planet_id},
       
        {"colony_id", cid},
        {"planet_name", planet_name},
        {"decisions_count", decisions.size()},
        {"decisions", decisions}
    };
}

nlohmann::json OutlinerManager::EnactDecisionJson(uint32_t planet_id, const std::string& decision_key,
                                                   uint32_t colony_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    uint32_t cid = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
    } else {
        void* p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"error", "Planet not found: " + std::to_string(planet_id)} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    }

    uint32_t country_id = GetPlayerCountryId();

    void* dec_mgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CDecisionsDatabase_pInstance), &dec_mgr);
    if (!dec_mgr) return { {"error", "Decisions database not found"} };

    uint32_t dec_count = 0;
    SafeReadU32((const void*)((uintptr_t)dec_mgr + 0x5C), &dec_count);
    void* dec_arr = nullptr;
    SafeReadPtr((const void*)((uintptr_t)dec_mgr + 0x50), &dec_arr);
    if (!dec_arr || dec_count == 0) return { {"error", "Decisions database empty"} };

    std::string norm_key = decision_key;
    if (norm_key == "luxuries" || norm_key == "consumer_goods" || norm_key == "发放奢侈品" || norm_key == "奢侈品") {
        norm_key = "decision_planet_luxuries_boost";
    } else if (norm_key == "food" || norm_key == "food_boost" || norm_key == "发放食物补贴" || norm_key == "食物补贴") {
        norm_key = "decision_planet_food_boost";
    } else if (norm_key == "population_control" || norm_key == "鼓励生育" || norm_key == "控制人口") {
        norm_key = "decision_enact_population_control";
    } else if (norm_key == "martial_law" || norm_key == "戒严令") {
        norm_key = "decision_declare_martial_law";
    } else if (norm_key == "mastery_of_nature" || norm_key == "掌握自然") {
        norm_key = "decision_mastery_of_nature";
    } else if (norm_key == "consecrate_world" || norm_key == "祝圣世界") {
        norm_key = "decision_consecrated_worlds";
    }

    void* target_dec = nullptr;
    std::string matched_key;
    for (uint32_t i = 0; i < dec_count; ++i) {
        void* dec = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)dec_arr + i * sizeof(void*)), &dec) || !dec) continue;
        std::string k;
        SafeReadPdxString((const void*)((uintptr_t)dec + 0x20), k);
        if (k == norm_key || k == decision_key) {
            target_dec = dec;
            matched_key = k;
            break;
        }
    }

    if (!target_dec) {
        return { {"error", "Decision not found in database: " + decision_key} };
    }

    namespace enact = sdk::cmd::enact_decision_planet_command;
    auto cmd = CommandBuilder::Get().Create(enact::kSpec);
    // carrier 8 字节：低 4 位 id、高 4 位类型（0 行星 / 1 舰）。方舟直接抄 colony 自己存的那个值。
    uint64_t carrier_val = (uint64_t)planet_id;
    if (colony_id != 0xFFFFFFFF) {
        void* colony = FindColony(colony_id);
        if (colony) SafeReadI64((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), (int64_t*)&carrier_val);
    }
    cmd.Set<void*>(enact::decision, target_dec)
       .Set<uint64_t>(enact::carrier, carrier_val)
       .Set<uint32_t>(enact::country, country_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Decision cannot be enacted (conditions not met, already active, or insufficient resources)"
                                  : "Decision cannot be enacted: " + why},
            {"planet_id", planet_id},
            {"colony_id", cid},
            {"decision_key", matched_key},
            {"country_id", country_id}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"decision_key", matched_key},
        {"decision_name", LocalizeDecisionKey(matched_key)},
        {"country_id", country_id},
        {"message", "Planetary decision enacted successfully"}
    };
}

nlohmann::json OutlinerManager::GetTerraformingOptionsJson(uint32_t planet_id) {
    if (!base_address_) {
        return { {"error", "Base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    std::string planet_name = PlanetName(p_obj);

    void* cur_class = nullptr;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0x148), &cur_class);
    std::string cur_class_key;
    if (cur_class) {
        SafeReadPdxString((const void*)((uintptr_t)cur_class + 0x28), cur_class_key);
    }
    std::string cur_class_name = LocalizePlanetClass(cur_class_key);

    uint32_t country_id = GetPlayerCountryId();

    // Check active terraforming process at planet + 0xB70
    void* proc = nullptr;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0xB70), &proc);
    bool is_terraforming = false;
    if (proc) {
        void** proc_vt = nullptr;
        SafeReadPtr(proc, (void**)&proc_vt);
        if (proc_vt && proc_vt[8]) {
            typedef bool (*FnIsTerraforming)(void*);
            is_terraforming = ((FnIsTerraforming)proc_vt[8])(proc);
        }
    }

    nlohmann::json current_process = nullptr;
    if (is_terraforming) {
        uint64_t raw_prog = 0, raw_total = 0;
        SafeReadU64((const void*)((uintptr_t)proc + 0x10), &raw_prog);
        SafeReadU64((const void*)((uintptr_t)proc + 0x18), &raw_total);
        double progress_days = (double)raw_prog / 100000.0;
        double total_days = (double)raw_total / 100000.0;
        double pct = total_days > 0.0 ? (progress_days / total_days) * 100.0 : 0.0;
        int32_t remaining = total_days > progress_days ? (int32_t)(total_days - progress_days) : 0;

        void* link = nullptr;
        SafeReadPtr((const void*)((uintptr_t)proc + 0x68), &link);
        std::string to_class_key = "unknown";
        if (link) {
            void* to_cls = nullptr;
            SafeReadPtr((const void*)((uintptr_t)link + 0x268), &to_cls);
            if (to_cls) {
                SafeReadPdxString((const void*)((uintptr_t)to_cls + 0x28), to_class_key);
            }
        }
        current_process = {
            {"target_planet_class", to_class_key},
            {"target_planet_class_name", LocalizePlanetClass(to_class_key)},
            {"progress_days", (int32_t)progress_days},
            {"total_days", (int32_t)total_days},
            {"remaining_days", remaining},
            {"progress_percent", std::round(pct * 10.0) / 10.0}
        };
    }

    // Read terraform links database at base + sdk::glob::CTerraformDatabase_pInstance
    void* db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::CTerraformDatabase_pInstance), &db);
    nlohmann::json options = nlohmann::json::array();
    if (db && cur_class) {
        uint32_t link_cnt = 0;
        SafeReadU32((const void*)((uintptr_t)db + 0x1C), &link_cnt);
        void* link_arr = nullptr;
        SafeReadPtr((const void*)((uintptr_t)db + 0x10), &link_arr);

        if (link_arr && link_cnt > 0) {
            for (uint32_t i = 0; i < link_cnt; ++i) {
                void* link = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)link_arr + i * sizeof(void*)), &link) || !link) {
                    continue;
                }

                void* from_cls = nullptr;
                SafeReadPtr((const void*)((uintptr_t)link + 0x260), &from_cls);
                if (from_cls != cur_class) continue;

                void* to_cls = nullptr;
                SafeReadPtr((const void*)((uintptr_t)link + 0x268), &to_cls);
                if (!to_cls) continue;

                std::string to_class_key;
                SafeReadPdxString((const void*)((uintptr_t)to_cls + 0x28), to_class_key);

                uint32_t duration_days = 0;
                SafeReadU32((const void*)((uintptr_t)link + 0x258), &duration_days);

                uint32_t link_index = 0;
                SafeReadU32((const void*)((uintptr_t)link + 0x3D0), &link_index);

                // Test validation via CStartTerraformationCommand
                bool can_terraform = false;
                if (!is_terraforming) {
                    namespace tf = sdk::cmd::start_terraformation;
                    auto probe = CommandBuilder::Get().Create(tf::kSpec);
                    probe.Set<uint32_t>(tf::planet, planet_id)
                         .Set<uint32_t>(tf::terraform_link, link_index)
                         .Set<uint32_t>(tf::who, country_id);
                    can_terraform = probe.IsValid();
                }

                options.push_back({
                    {"link_index", link_index},
                    {"target_planet_class", to_class_key},
                    {"target_planet_class_name", LocalizePlanetClass(to_class_key)},
                    {"duration_days", duration_days},
                    {"can_terraform", can_terraform}
                });
            }
        }
    }

    return {
        {"planet_id", planet_id},
        {"planet_name", planet_name},
        {"current_planet_class", cur_class_key},
        {"current_planet_class_name", cur_class_name},
        {"is_terraforming", is_terraforming},
        {"current_process", current_process},
        {"available_options_count", options.size()},
        {"options", options}
    };
}

nlohmann::json OutlinerManager::StartTerraformingJson(uint32_t planet_id, const std::string& target_class, int32_t link_index) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    void* proc = nullptr;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0xB70), &proc);
    bool is_terraforming = false;
    if (proc) {
        void** proc_vt = nullptr;
        SafeReadPtr(proc, (void**)&proc_vt);
        if (proc_vt && proc_vt[8]) {
            typedef bool (*FnIsTerraforming)(void*);
            is_terraforming = ((FnIsTerraforming)proc_vt[8])(proc);
        }
    }
    if (is_terraforming) {
        return { {"error", "Planet is already being terraformed"} };
    }

    void* cur_class = nullptr;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0x148), &cur_class);
    if (!cur_class) return { {"error", "Failed to get planet class"} };

    uint32_t country_id = GetPlayerCountryId();

    void* db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::CTerraformDatabase_pInstance), &db);
    if (!db) return { {"error", "Terraform database not found"} };

    uint32_t link_cnt = 0;
    SafeReadU32((const void*)((uintptr_t)db + 0x1C), &link_cnt);
    void* link_arr = nullptr;
    SafeReadPtr((const void*)((uintptr_t)db + 0x10), &link_arr);
    if (!link_arr || link_cnt == 0) return { {"error", "Terraform database empty"} };

    std::string norm_target = target_class;
    if (!norm_target.empty() && norm_target.find("pc_") != 0) {
        if (norm_target == "ocean" || norm_target == "海洋") norm_target = "pc_ocean";
        else if (norm_target == "tropical" || norm_target == "热带") norm_target = "pc_tropical";
        else if (norm_target == "continental" || norm_target == "大陆") norm_target = "pc_continental";
        else if (norm_target == "desert" || norm_target == "沙漠") norm_target = "pc_desert";
        else if (norm_target == "arid" || norm_target == "干旱") norm_target = "pc_arid";
        else if (norm_target == "savannah" || norm_target == "热带草原") norm_target = "pc_savannah";
        else if (norm_target == "arctic" || norm_target == "极地") norm_target = "pc_arctic";
        else if (norm_target == "alpine" || norm_target == "高山") norm_target = "pc_alpine";
        else if (norm_target == "tundra" || norm_target == "苔原") norm_target = "pc_tundra";
        else if (norm_target == "gaia" || norm_target == "盖亚") norm_target = "pc_gaia";
        else if (norm_target == "machine" || norm_target == "机魂") norm_target = "pc_machine";
        else if (norm_target == "hive" || norm_target == "蜂巢") norm_target = "pc_hive";
        else norm_target = "pc_" + norm_target;
    }

    void* target_link = nullptr;
    uint32_t target_link_idx = 0;
    uint32_t duration_days = 0;
    std::string matched_target_key;

    for (uint32_t i = 0; i < link_cnt; ++i) {
        void* link = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)link_arr + i * sizeof(void*)), &link) || !link) continue;

        void* from_cls = nullptr;
        SafeReadPtr((const void*)((uintptr_t)link + 0x260), &from_cls);
        if (from_cls != cur_class) continue;

        uint32_t l_idx = 0;
        SafeReadU32((const void*)((uintptr_t)link + 0x3D0), &l_idx);

        void* to_cls = nullptr;
        SafeReadPtr((const void*)((uintptr_t)link + 0x268), &to_cls);
        std::string to_k;
        if (to_cls) {
            SafeReadPdxString((const void*)((uintptr_t)to_cls + 0x28), to_k);
        }

        if (link_index >= 0 && (uint32_t)link_index == l_idx) {
            target_link = link;
            target_link_idx = l_idx;
            matched_target_key = to_k;
            SafeReadU32((const void*)((uintptr_t)link + 0x258), &duration_days);
            break;
        } else if (!norm_target.empty() && to_k == norm_target) {
            target_link = link;
            target_link_idx = l_idx;
            matched_target_key = to_k;
            SafeReadU32((const void*)((uintptr_t)link + 0x258), &duration_days);
            break;
        }
    }

    if (!target_link) {
        return { {"error", "No valid terraforming link found for target class: " + target_class} };
    }

    namespace tf = sdk::cmd::start_terraformation;
    auto cmd = CommandBuilder::Get().Create(tf::kSpec);
    cmd.Set<uint32_t>(tf::planet, planet_id)
       .Set<uint32_t>(tf::terraform_link, target_link_idx)
       .Set<uint32_t>(tf::who, country_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Terraforming not possible (tech prerequisite, energy credits, or invalid target)"
                                  : "Terraforming not possible: " + why},
            {"planet_id", planet_id},
            {"target_planet_class", matched_target_key},
            {"link_index", target_link_idx},
            {"country_id", country_id}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"target_planet_class", matched_target_key},
        {"target_planet_class_name", LocalizePlanetClass(matched_target_key)},
        {"duration_days", duration_days},
        {"link_index", target_link_idx},
        {"country_id", country_id},
        {"message", "Terraforming project started successfully"}
    };
}

nlohmann::json OutlinerManager::CancelTerraformingJson(uint32_t planet_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"error", "Engine functions or base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    void* proc = nullptr;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0xB70), &proc);
    bool is_terraforming = false;
    if (proc) {
        void** proc_vt = nullptr;
        SafeReadPtr(proc, (void**)&proc_vt);
        if (proc_vt && proc_vt[8]) {
            typedef bool (*FnIsTerraforming)(void*);
            is_terraforming = ((FnIsTerraforming)proc_vt[8])(proc);
        }
    }
    if (!is_terraforming) {
        return { {"error", "Planet is not currently being terraformed"} };
    }

    uint32_t country_id = GetPlayerCountryId();

    namespace tfc = sdk::cmd::cancel_terraformation;
    auto cmd = CommandBuilder::Get().Create(tfc::kSpec);
    cmd.Set<uint32_t>(tfc::planet, planet_id).Set<uint32_t>(tfc::who, country_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Terraforming cannot be cancelled" : "Terraforming cannot be cancelled: " + why},
            {"planet_id", planet_id},
            {"country_id", country_id}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"country_id", country_id},
        {"message", "Terraforming project cancelled successfully"}
    };
}

// -------------------------------------------------------------
// Economy & Jobs Management (4.5.0 Cygnus Workforce Model)
// -------------------------------------------------------------

nlohmann::json OutlinerManager::ExtractWorkforceSummary(void* colony_obj) {
    if (!colony_obj) {
        return {
            {"total_employed_workforce", 0},
            {"total_effective_workforce", 0},
            {"unemployed", 0},
            {"active_jobs_count", 0},
            {"prioritized_job", nullptr},
            {"strata", nlohmann::json::array()}
        };
    }

    uint32_t unemployed = 0;
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xfd0), &unemployed);

    void* jobs_arr = nullptr;
    uint32_t jobs_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x38), &jobs_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x44), &jobs_cnt);

    void* job_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &job_db);

    void* job_db_arr = nullptr;
    uint32_t job_db_cap = 0;
    if (job_db && (uintptr_t)job_db >= 0x10000) {
        SafeReadPtr((const void*)((uintptr_t)job_db + 0x18), &job_db_arr);
        SafeReadU32((const void*)((uintptr_t)job_db + 0x20), &job_db_cap);
    }

    int64_t total_employed_wf = 0;
    int64_t total_effective_wf = 0;
    uint32_t active_jobs_count = 0;
    std::string prioritized_job_key = "";

    struct StratumSummary {
        std::string key;
        std::string name;
        int64_t workforce{ 0 };
        int64_t effective_workforce{ 0 };
        uint32_t active_jobs{ 0 };
    };
    std::vector<std::string> strata_order = { "ruler", "specialist", "worker", "civilian" };
    std::unordered_map<std::string, StratumSummary> strata_map;
    strata_map["ruler"] = { "ruler", LocalizeKey("pop_cat_ruler"), 0, 0, 0 };
    strata_map["specialist"] = { "specialist", LocalizeKey("pop_cat_specialist"), 0, 0, 0 };
    strata_map["worker"] = { "worker", LocalizeKey("pop_cat_worker"), 0, 0, 0 };
    strata_map["civilian"] = { "civilian", LocalizeKey("pop_cat_civilian"), 0, 0, 0 };

    void* fav_map_entries = nullptr;
    uint32_t fav_map_mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xee8), &fav_map_entries);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xef4), &fav_map_mask);
    if (fav_map_entries && (uintptr_t)fav_map_entries > 0x10000 && fav_map_mask < 1024) {
        for (uint32_t mi = 0; mi <= fav_map_mask; ++mi) {
            uint8_t occupied = 0;
            SafeReadU8((const void*)((uintptr_t)fav_map_entries + mi * 24 + 4), &occupied);
            if (!occupied) continue;

            void* j = nullptr;
            SafeReadPtr((const void*)((uintptr_t)fav_map_entries + mi * 24 + 16), &j);
            if (j && (uintptr_t)j > 0x10000) {
                std::string pj_key;
                SafeReadPdxString((const void*)((uintptr_t)j + 0x20), pj_key);
                if (!pj_key.empty() && pj_key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") == std::string::npos) {
                    prioritized_job_key = pj_key;
                    break;
                }
            }
        }
    }

    if (jobs_arr && job_db_arr && jobs_cnt > 0 && jobs_cnt < 256) {
        for (uint32_t idx = 0; idx < jobs_cnt; ++idx) {
            uint32_t handle = 0;
            SafeReadU32((const void*)((uintptr_t)jobs_arr + idx * 4), &handle);
            uint32_t slot = handle & 0xFFFFFF;
            if (slot >= job_db_cap) continue;

            void* p_job = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)job_db_arr + slot * 16 + 8), &p_job) || !p_job) continue;

            int64_t raw_wf = 0, raw_max = 0, raw_bonus = 0;
            SafeReadI64((const void*)((uintptr_t)p_job + 0x90), &raw_wf);
            SafeReadI64((const void*)((uintptr_t)p_job + 0x98), &raw_max);
            SafeReadI64((const void*)((uintptr_t)p_job + 0xa0), &raw_bonus);

            if (raw_wf <= 0 && raw_max <= 0) continue;

            void* p_job_type = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job + 0x18), &p_job_type);
            if (!p_job_type) continue;

            std::string job_key;
            SafeReadPdxString((const void*)((uintptr_t)p_job_type + 0x20), job_key);
            if (job_key.empty()) continue;

            void* p_cat = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job_type + 0xaa0), &p_cat);
            std::string stratum_key = "worker";
            if (p_cat) {
                SafeReadPdxString((const void*)((uintptr_t)p_cat + 0x20), stratum_key);
            }
            if (stratum_key.empty()) stratum_key = "worker";

            int64_t wf = raw_wf / 100000;
            int64_t bonus_wf = raw_bonus / 100000;

            active_jobs_count++;
            total_employed_wf += wf;
            total_effective_wf += (wf + bonus_wf);

            auto& entry = strata_map[stratum_key];
            entry.key = stratum_key;
            if (entry.name.empty()) {
                entry.name = LocalizeKey("pop_cat_" + stratum_key);
            }
            entry.workforce += wf;
            entry.effective_workforce += (wf + bonus_wf);
            entry.active_jobs++;
        }
    }

    nlohmann::json strata_arr = nlohmann::json::array();
    for (const auto& skey : strata_order) {
        auto it = strata_map.find(skey);
        if (it != strata_map.end() && it->second.active_jobs > 0) {
            strata_arr.push_back({
                {"stratum_key", it->second.key},
                {"stratum_name", it->second.name},
                {"workforce", it->second.workforce},
                {"effective_workforce", it->second.effective_workforce},
                {"active_jobs_count", it->second.active_jobs}
            });
        }
    }
    for (const auto& [k, v] : strata_map) {
        if (std::find(strata_order.begin(), strata_order.end(), k) == strata_order.end() && v.active_jobs > 0) {
            strata_arr.push_back({
                {"stratum_key", v.key},
                {"stratum_name", v.name},
                {"workforce", v.workforce},
                {"effective_workforce", v.effective_workforce},
                {"active_jobs_count", v.active_jobs}
            });
        }
    }

    return {
        {"total_employed_workforce", total_employed_wf},
        {"total_effective_workforce", total_effective_wf},
        {"unemployed", unemployed},
        {"active_jobs_count", active_jobs_count},
        {"prioritized_job", prioritized_job_key.empty() ? nlohmann::json(nullptr) : nlohmann::json(prioritized_job_key)},
        {"strata", strata_arr}
    };
}

nlohmann::json OutlinerManager::ProbeColonyJobsRaw(uint32_t planet_id) {
    /* 只读探针：殖民地岗位数组里每条记录，把 +0x80..+0xC0 的原始 qword 全打出来。
       用来确定格式塔岗位的 workforce 到底存在哪（存档里是 100/100 这种整数）。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "planet not found"} };

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"success", false}, {"error", "planet has no colony"} };

    void* colony = FindColony(cid);
    if (!colony) return { {"success", false}, {"error", "colony not found"} };

    void* jobs_arr = nullptr;
    uint32_t jobs_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony + 0x38), &jobs_arr);
    SafeReadU32((const void*)((uintptr_t)colony + 0x44), &jobs_cnt);

    void* job_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &job_db);
    void* job_db_arr = nullptr;
    uint32_t job_db_cap = 0;
    if (job_db && (uintptr_t)job_db >= 0x10000) {
        SafeReadPtr((const void*)((uintptr_t)job_db + 0x18), &job_db_arr);
        SafeReadU32((const void*)((uintptr_t)job_db + 0x20), &job_db_cap);
    }

    nlohmann::json rows = nlohmann::json::array();
    for (uint32_t idx = 0; idx < jobs_cnt && idx < 40; ++idx) {
        uint32_t handle = 0;
        SafeReadU32((const void*)((uintptr_t)jobs_arr + idx * 4), &handle);
        uint32_t slot = handle & 0xFFFFFF;
        if (slot >= job_db_cap) continue;

        void* p_job = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)job_db_arr + slot * 16 + 8), &p_job) || !p_job) continue;

        void* p_type = nullptr;
        SafeReadPtr((const void*)((uintptr_t)p_job + 0x18), &p_type);
        std::string key;
        if (p_type) SafeReadPdxString((const void*)((uintptr_t)p_type + 0x20), key);

        nlohmann::json words = nlohmann::json::array();
        for (int off = 0x80; off <= 0xC0; off += 8) {
            int64_t v = 0;
            SafeReadI64((const void*)((uintptr_t)p_job + off), &v);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%+lld(0x%llX)", (long long)v, (unsigned long long)v);
            words.push_back(nlohmann::json::object({ { "off", off }, { "v", buf } }));
        }

        rows.push_back({ {"key", key}, {"job_ptr", (unsigned long long)(uintptr_t)p_job}, {"words", words} });
    }

    return { {"success", true}, {"colony_id", cid}, {"jobs_count", jobs_cnt},
             {"jobs_array", (unsigned long long)(uintptr_t)jobs_arr}, {"rows", rows} };
}

nlohmann::json OutlinerManager::ScanColonyForInt(uint32_t planet_id, int64_t value, uint32_t radius) {
    /* 只读探针：在 CColony 本体与它浅指针指向的区域里，找等于 value 的 qword。
       用于定位“岗位分配数”（存档里 workforce 100 → 内存可能是 100*100000）。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    if (radius == 0 || radius > 0x4000) radius = 0x400;

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "planet not found"} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"success", false}, {"error", "planet has no colony"} };
    void* colony = FindColony(cid);
    if (!colony) return { {"success", false}, {"error", "colony not found"} };

    nlohmann::json hits = nlohmann::json::array();

    auto scan = [&](uintptr_t region, size_t len, const char* where) {
        for (size_t off = 0; off + 8 <= len; off += 8) {
            int64_t v = 0;
            if (!SafeReadI64((const void*)(region + off), &v)) break;
            if (v != value) continue;
            nlohmann::json neighbours = nlohmann::json::array();
            for (int d = -3; d <= 3; ++d) {
                long long at = (long long)off + d * 8;
                if (at < 0 || (size_t)at + 8 > len) continue;
                int64_t n = 0;
                SafeReadI64((const void*)(region + at), &n);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%+lld", (long long)n);
                neighbours.push_back(buf);
            }
            char item[96];
            std::snprintf(item, sizeof(item), "%s+0x%zX", where, off);
            hits.push_back({ {"at", item}, {"region", (unsigned long long)region}, {"context", neighbours} });
            if (hits.size() >= 40) return;
        }
    };

    scan((uintptr_t)colony, 0x2000, "colony");
    if (hits.size() < 40) {
        for (int slot = 0; slot < 0x1000; slot += 8) {
            void* pv = nullptr;
            if (!SafeReadPtr((const uint8_t*)colony + slot, &pv)) break;
            uint64_t p = (uint64_t)(uintptr_t)pv;
            if (p < 0x10000ull || p > 0x7FFFFFFFFFFFull) continue;
            if (slot % 0x100 == 0) {
                char where[48];
                std::snprintf(where, sizeof(where), "*(colony+0x%X)", slot);
                scan((uintptr_t)p, radius, where);
                if (hits.size() >= 40) break;
            }
        }
    }

    return { {"success", true}, {"colony_id", cid}, {"value", (long long)value},
             {"colony", (unsigned long long)(uintptr_t)colony}, {"hits", hits} };
}

nlohmann::json OutlinerManager::DumpColonyMap(uint32_t planet_id) {
    /* 只读：把 colony+0xee8 那张哈希表的条目全打出来（含两处 key 的字符串）。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "planet not found"} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"success", false}, {"error", "planet has no colony"} };
    void* colony = FindColony(cid);
    if (!colony) return { {"success", false}, {"error", "colony not found"} };

    void* entries = nullptr;
    uint32_t mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony + 0xee8), &entries);
    SafeReadU32((const void*)((uintptr_t)colony + 0xef4), &mask);

    nlohmann::json rows = nlohmann::json::array();
    if (entries && mask < 4096) {
        for (uint32_t i = 0; i <= mask; ++i) {
            uint8_t occupied = 0;
            SafeReadU8((const void*)((uintptr_t)entries + i * 24 + 4), &occupied);
            if (!occupied) continue;

            uint64_t q0 = 0, q1 = 0, q2 = 0;
            SafeReadU64((const void*)((uintptr_t)entries + i * 24 + 0), &q0);
            SafeReadU64((const void*)((uintptr_t)entries + i * 24 + 8), &q1);
            SafeReadU64((const void*)((uintptr_t)entries + i * 24 + 16), &q2);

            std::string k1, k2;
            if (q1 > 0x10000) SafeReadPdxString((const void*)(uintptr_t)(q1 + 0x20), k1);
            if (q2 > 0x10000) SafeReadPdxString((const void*)(uintptr_t)(q2 + 0x20), k2);

            char item[64];
            std::snprintf(item, sizeof(item), "idx%u", i);
            rows.push_back({ {"at", item}, {"q0", (unsigned long long)q0},
                             {"q1", (unsigned long long)q1}, {"q2", (unsigned long long)q2},
                             {"key1", k1}, {"key2", k2} });
        }
    }
    return { {"success", true}, {"colony_id", cid}, {"mask", mask},
             {"entries", (unsigned long long)(uintptr_t)entries}, {"rows", rows} };
}

nlohmann::json OutlinerManager::ScanColonyForIntDeep(uint32_t planet_id, int64_t value) {
    /* 只读：colony[0,0x4000) 直接扫 + 每个 qword 当指针、扫其前 0x200 字节。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "planet not found"} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"success", false}, {"error", "planet has no colony"} };
    void* colony = FindColony(cid);
    if (!colony) return { {"success", false}, {"error", "colony not found"} };

    nlohmann::json hits = nlohmann::json::array();
    auto scan = [&](uintptr_t region, size_t len, const char* where) {
        for (size_t off = 0; off + 8 <= len; off += 8) {
            int64_t v = 0;
            if (!SafeReadI64((const void*)(region + off), &v)) break;
            if (v != value) continue;
            char item[96];
            std::snprintf(item, sizeof(item), "%s+0x%zX", where, off);
            hits.push_back({ {"at", item}, {"region", (unsigned long long)region} });
            if (hits.size() >= 60) return;
        }
    };

    scan((uintptr_t)colony, 0x4000, "colony");
    for (int slot = 0; slot < 0x2000 && hits.size() < 60; slot += 8) {
        void* pv = nullptr;
        if (!SafeReadPtr((const uint8_t*)colony + slot, &pv)) break;
        uint64_t p = (uint64_t)(uintptr_t)pv;
        if (p < 0x10000ull || p > 0x7FFFFFFFFFFFull) continue;
        char where[48];
        std::snprintf(where, sizeof(where), "*(c+0x%X)", slot);
        scan((uintptr_t)p, 0x200, where);
    }
    return { {"success", true}, {"colony_id", cid}, {"value", (long long)value}, {"hits", hits} };
}

nlohmann::json OutlinerManager::FindPopGroupSlots(uint32_t planet_id, const std::vector<int64_t>& sizes, uint32_t radius) {
    /* 只读探针：殖民地本体与每个指针槽指向的区域里，搜一组 int64（人口组规模）。
       命中会记下槽位、目标地址、偏移与匹配到的值 —— 用来定位 colony → 人口组 的指针。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    if (radius == 0 || radius > 0x2000) radius = 0x400;

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "planet not found"} };
    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) return { {"success", false}, {"error", "planet has no colony"} };
    void* colony = FindColony(cid);
    if (!colony) return { {"success", false}, {"error", "colony not found"} };

    nlohmann::json hits = nlohmann::json::array();

    auto scanRegion = [&](uintptr_t region, size_t len, const char* label, int slot) {
        for (size_t off = 0; off + 8 <= len; off += 8) {
            int64_t v = 0;
            if (!SafeReadI64((const void*)(region + off), &v)) break;
            for (const int64_t want : sizes) {
                if (v != want) continue;
                char at[80];
                std::snprintf(at, sizeof(at), "%s+0x%zX", label, off);
                hits.push_back({ {"at", at}, {"slot", slot}, {"region", (unsigned long long)region},
                                 {"value", (long long)want} });
                if (hits.size() >= 80) return;
            }
        }
    };

    scanRegion((uintptr_t)colony, 0x2000, "colony", -1);

    for (int slot = 0; slot < 0x2000 && hits.size() < 80; slot += 8) {
        void* pv = nullptr;
        if (!SafeReadPtr((const uint8_t*)colony + slot, &pv)) break;
        const uint64_t p = (uint64_t)(uintptr_t)pv;
        if (p < 0x10000ull || p > 0x7FFFFFFFFFFFull) continue;
        char label[40];
        std::snprintf(label, sizeof(label), "*(c+0x%X)", slot);
        scanRegion((uintptr_t)p, radius, label, slot);
    }

    return { {"success", true}, {"colony_id", cid},
             {"colony", (unsigned long long)(uintptr_t)colony}, {"hits", hits} };
}

nlohmann::json OutlinerManager::DumpPopGroup(uint32_t group_id) {
    /* 只读：TPdxRef<CPopGroup>::_pDatabase → 数组 → 对象，dump 前 0x140 字节 + 可疑的岗位三连。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };

    void* db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CPopGroup_pDatabase), &db);
    if (!db) return { {"success", false}, {"error", "pop group database is null"} };

    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap);
    if (!arr || cap == 0) return { {"success", false}, {"error", "pop group database is empty"} };

    const uint32_t slot = group_id & 0xFFFFFF;
    if (slot >= cap) return { {"success", false}, {"error", "slot out of range"}, {"cap", cap} };

    void* obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &obj) || !obj) {
        return { {"success", false}, {"error", "pop group object is null"}, {"slot", slot} };
    }

    nlohmann::json fields = nlohmann::json::array();
    for (int off = 0; off < 0x140; off += 8) {
        int64_t v = 0;
        if (!SafeReadI64((const void*)((uintptr_t)obj + off), &v)) break;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+lld", (long long)v);
        fields.push_back({ {"off", off}, {"v", buf} });
    }

    /* 可疑的岗位三连：相邻 8 字节里出现的 100000 / 3200000 之类 */
    nlohmann::json suspects = nlohmann::json::array();
    for (int off = 0; off < 0x140; off += 8) {
        int64_t v = 0;
        if (!SafeReadI64((const void*)((uintptr_t)obj + off), &v)) break;
        if (v > 0 && v < 100000000LL && (v % 10000 == 0) && v >= 10000) {
            char item[48];
            std::snprintf(item, sizeof(item), "+0x%X = %lld (%.2f)", off, (long long)v, (double)v / 100000.0);
            suspects.push_back(item);
        }
    }

    return { {"success", true}, {"group_id", group_id}, {"slot", slot},
             {"object", (unsigned long long)(uintptr_t)obj},
             {"fields", fields}, {"suspects", suspects} };
}

nlohmann::json OutlinerManager::PlanetPointer(uint32_t planet_id) {
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    void* p = FindPlanet(planet_id);
    if (!p) return { {"success", false}, {"error", "planet not found"} };
    uint32_t colony = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p + 0xe0), &colony);
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)(uintptr_t)p);
    return { {"success", true}, {"planet_id", planet_id}, {"planet", hex}, {"colony_id", colony} };
}

nlohmann::json OutlinerManager::ResolvePopJob(uint32_t handle) {
    /* 只读：把岗位句柄按 {index, generation} 拆开，试几种位段，看哪种能读出键名。 */
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };

    void* db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &db);
    if (!db) return { {"success", false}, {"error", "CPopJob db is null"} };

    void* arr = nullptr;
    uint32_t cap = 0;
    SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr);
    SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap);
    if (!arr || cap == 0) return { {"success", false}, {"error", "db empty"} };

    nlohmann::json tries = nlohmann::json::array();
    const uint32_t masks[] = { 0xFFFu, 0xFFFFu, 0xFFFFFFu, 0x1FFFu, 0x3FFFu };
    for (uint32_t mask : masks) {
        const uint32_t slot = handle & mask;
        const uint32_t gen = mask == 0xFFFFFFu ? 0u : (handle >> 12);
        if (slot >= cap) {
            tries.push_back({ {"mask", mask}, {"slot", slot}, {"error", "slot out of range"} });
            continue;
        }
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + slot * 16 + 8), &obj) || !obj) {
            tries.push_back({ {"mask", mask}, {"slot", slot}, {"error", "null object"} });
            continue;
        }
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)obj + 0x20), key);
        std::string name = key.empty() ? std::string() : LocalizeKey("job_" + key);
        char obj_hex[24];
        std::snprintf(obj_hex, sizeof(obj_hex), "0x%llX", (unsigned long long)(uintptr_t)obj);
        tries.push_back({ {"mask", mask}, {"slot", slot}, {"gen", gen},
                          {"object", obj_hex}, {"key", key}, {"name", name} });
    }

    return { {"success", true}, {"handle", handle}, {"cap", cap}, {"tries", tries} };
}


// ---- 岗位编制（planet+0x788） --------------------------------------------------------------
// 行星对象 +0x788 是一个指针，+0x790 是条目数；每条 16 字节：
//   { 编制 × 100000 (i64), 岗位引用 id (u32) }
// 编制含修正，可以为负。实测与存档一致（活体陈设 4650、采矿 1200、农业 650、技工 450…）。
// 注意：岗位 id 的位段还没解开（>>8 落在 162–166，对不上键名），所以这里**只返回原始 id**。
namespace {
// 岗位类型 id（类型对象 +0x48）→ 键名/阶层。CPopJob 库里每个对象都指向一个类型对象，
// 类型对象 +0x48 正是编制表里存的那个 id（实测 bio_trophy = 41562）。
// 编制表里还有一半条目属于别的 id 空间（修正/贡献项），用这张映射就能把它们筛掉。
struct JobTypeInfo {
    std::string key;
    std::string name;
    std::string stratum;
};
}  // namespace

nlohmann::json OutlinerManager::GetPlanetJobCapacityJson(uint32_t planet_id) {
    if (!base_address_) return { {"success", false}, {"error", "Base address not initialized"} };

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) return { {"success", false}, {"error", "Planet " + std::to_string(planet_id) + " not found"} };

    void* table = nullptr;
    uint32_t count = 0;
    SafeReadPtr((const void*)((uintptr_t)p_obj + 0x788), &table);
    SafeReadU32((const void*)((uintptr_t)p_obj + 0x790), &count);
    if (!table || (uintptr_t)table < 0x10000 || count == 0 || count > 4096) {
        return { {"success", false}, {"error", "This planet has no job capacity table"} };
    }

    // 岗位类型 id → 信息（进程内缓存；库里约 2048 个槽，首次调用建表）
    static std::unordered_map<uint32_t, JobTypeInfo> type_map;
    if (type_map.empty()) {
        void* jdb = nullptr;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &jdb) && jdb) {
            void* jarr = nullptr;
            uint32_t jcap = 0;
            SafeReadPtr((const void*)((uintptr_t)jdb + 0x18), &jarr);
            SafeReadU32((const void*)((uintptr_t)jdb + 0x20), &jcap);
            for (uint32_t slot = 0; jarr && slot < jcap; ++slot) {
                void* job = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)jarr + slot * 16 + 8), &job) || !job) continue;
                void* type = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)job + 0x18), &type) || !type) continue;
                uint32_t id = 0;
                if (!SafeReadU32((const void*)((uintptr_t)type + 0x48), &id) || type_map.count(id)) continue;
                JobTypeInfo info;
                SafeReadPdxString((const void*)((uintptr_t)type + 0x20), info.key);
                if (info.key.empty()) continue;
                void* cat = nullptr;
                SafeReadPtr((const void*)((uintptr_t)type + 0xaa0), &cat);
                if (cat) SafeReadPdxString((const void*)((uintptr_t)cat + 0x20), info.stratum);
                info.name = LocalizeKey("job_" + info.key);
                if (info.name.empty()) info.name = info.key;
                type_map[id] = info;
            }
        }
    }

    nlohmann::json jobs = nlohmann::json::array();
    double total = 0.0;
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int64_t fixed = 0;
        uint32_t ref = 0;
        if (!SafeReadI64((const void*)((uintptr_t)table + i * 16), &fixed)) continue;
        if (!SafeReadU32((const void*)((uintptr_t)table + i * 16 + 8), &ref)) continue;
        auto it = type_map.find(ref);
        if (it == type_map.end()) { ++skipped; continue; }   // 不是岗位，丢掉
        const double wf = (double)fixed / 100000.0;
        total += wf;
        jobs.push_back({ {"job_key", it->second.key}, {"job_name", it->second.name},
                         {"stratum", it->second.stratum}, {"workforce", wf} });
    }

    return { {"success", true}, {"planet_id", planet_id}, {"count", (uint32_t)jobs.size()},
             {"skipped_non_job", skipped}, {"total_workforce", total}, {"jobs", jobs} };
}

nlohmann::json OutlinerManager::GetPlanetJobsJson(uint32_t planet_id) {
    if (!base_address_) {
        return { {"success", false}, {"error", "Base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet with ID " + std::to_string(planet_id) + " not found"} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "Planet has no active colony"} };
    }

    void* colony_obj = FindColony(cid);
    if (!colony_obj) {
        return { {"success", false}, {"error", "Colony object not found"} };
    }

    void* jobs_arr = nullptr;
    uint32_t jobs_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x38), &jobs_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x44), &jobs_cnt);

    void* job_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &job_db);

    void* job_db_arr = nullptr;
    uint32_t job_db_cap = 0;
    if (job_db && (uintptr_t)job_db >= 0x10000) {
        SafeReadPtr((const void*)((uintptr_t)job_db + 0x18), &job_db_arr);
        SafeReadU32((const void*)((uintptr_t)job_db + 0x20), &job_db_cap);
    }

    struct JobCard {
        std::string job_key;
        std::string job_name;
        int64_t current_workforce{ 0 };
        int64_t max_workforce{ 0 };
        int64_t bonus_workforce{ 0 };
        int64_t effective_workforce{ 0 };
        int64_t workforce_limit{ 0 };
        bool is_prioritized{ false };
        bool can_prioritize{ false };
    };

    struct StratumGroup {
        std::string stratum_key;
        std::string stratum_name;
        int64_t total_workforce{ 0 };
        int64_t total_effective_workforce{ 0 };
        std::vector<JobCard> jobs;
    };

    std::vector<std::string> strata_order = { "ruler", "specialist", "worker", "civilian" };
    std::unordered_map<std::string, StratumGroup> strata_map;
    strata_map["ruler"] = { "ruler", LocalizeKey("pop_cat_ruler"), 0, 0, {} };
    strata_map["specialist"] = { "specialist", LocalizeKey("pop_cat_specialist"), 0, 0, {} };
    strata_map["worker"] = { "worker", LocalizeKey("pop_cat_worker"), 0, 0, {} };
    strata_map["civilian"] = { "civilian", LocalizeKey("pop_cat_civilian"), 0, 0, {} };

    int64_t total_workforce = 0;

    void* map_entries = nullptr;
    uint32_t map_mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xee8), &map_entries);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xef4), &map_mask);

    if (jobs_arr && job_db_arr && jobs_cnt > 0 && jobs_cnt < 256) {
        for (uint32_t idx = 0; idx < jobs_cnt; ++idx) {
            uint32_t handle = 0;
            SafeReadU32((const void*)((uintptr_t)jobs_arr + idx * 4), &handle);
            uint32_t slot = handle & 0xFFFFFF;
            if (slot >= job_db_cap) continue;

            void* p_job = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)job_db_arr + slot * 16 + 8), &p_job) || !p_job) continue;

            int64_t raw_wf = 0, raw_max = 0, raw_bonus = 0, raw_limit = 0;
            // 2026-11-18 实测更正：在岗/编制在 +0xF8 / +0x100（原来读的 0x90/0x98 是错的，
            // 差了 0x68，所以过去一直返回 0）。+0x108 / +0x110 是加成与上限修正。
            SafeReadI64((const void*)((uintptr_t)p_job + 0xf8), &raw_wf);
            SafeReadI64((const void*)((uintptr_t)p_job + 0x100), &raw_max);
            SafeReadI64((const void*)((uintptr_t)p_job + 0x108), &raw_bonus);
            SafeReadI64((const void*)((uintptr_t)p_job + 0x110), &raw_limit);

            if (raw_wf <= 0 && raw_max <= 0) continue;

            void* p_job_type = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job + 0x18), &p_job_type);
            if (!p_job_type) continue;

            std::string job_key;
            SafeReadPdxString((const void*)((uintptr_t)p_job_type + 0x20), job_key);
            if (job_key.empty()) continue;

            void* p_cat = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job_type + 0xaa0), &p_cat);
            std::string stratum_key = "worker";
            if (p_cat) {
                SafeReadPdxString((const void*)((uintptr_t)p_cat + 0x20), stratum_key);
            }
            if (stratum_key.empty()) stratum_key = "worker";

            bool is_prioritized = false;
            if (map_entries && (uintptr_t)map_entries > 0x10000 && map_mask < 1024) {
                for (uint32_t mi = 0; mi <= map_mask; ++mi) {
                    uint8_t occupied = 0;
                    SafeReadU8((const void*)((uintptr_t)map_entries + mi * 24 + 4), &occupied);
                    if (!occupied) continue;

                    void* c = nullptr;
                    void* j = nullptr;
                    SafeReadPtr((const void*)((uintptr_t)map_entries + mi * 24 + 8), &c);
                    SafeReadPtr((const void*)((uintptr_t)map_entries + mi * 24 + 16), &j);
                    if (c == p_cat && j == p_job_type) {
                        is_prioritized = true;
                        break;
                    }
                }
            }

            uint32_t jt_flags = 0;
            SafeReadU32((const void*)((uintptr_t)p_job_type + 0x928), &jt_flags);
            bool can_prio = (jt_flags & 1) != 0 && (jt_flags & 2) != 0;

            int64_t cur_wf = raw_wf / 100000;
            int64_t max_wf = (raw_max > 0 && raw_max < 100000000000000LL) ? (raw_max / 100000) : cur_wf;
            int64_t bonus_wf = raw_bonus / 100000;
            int64_t eff_wf = cur_wf + bonus_wf;
            int64_t limit_wf = (raw_limit > 0 && raw_limit < 100000000000000LL) ? (raw_limit / 100000) : max_wf;

            std::string job_name = LocalizeKey("job_" + job_key);
            if (job_name.empty()) job_name = job_key;

            JobCard card;
            card.job_key = job_key;
            card.job_name = job_name;
            card.current_workforce = cur_wf;
            card.max_workforce = max_wf;
            card.bonus_workforce = bonus_wf;
            card.effective_workforce = eff_wf;
            card.workforce_limit = limit_wf;
            card.is_prioritized = is_prioritized;
            card.can_prioritize = can_prio;

            total_workforce += cur_wf;

            auto& group = strata_map[stratum_key];
            group.stratum_key = stratum_key;
            if (group.stratum_name.empty()) {
                group.stratum_name = LocalizeKey("pop_cat_" + stratum_key);
            }
            group.total_workforce += cur_wf;
            group.total_effective_workforce += eff_wf;
            group.jobs.push_back(card);
        }
    }

    nlohmann::json strata_arr = nlohmann::json::array();
    // 固定四阶层之外还要输出其余阶层：蜂群/机械的岗位不属于 ruler/specialist/worker/civilian，
    // 过去只遍历 strata_order，于是格式塔殖民地的 strata 永远是空数组（总数却是对的）。
    std::vector<std::string> emit_order = strata_order;
    for (const auto& kv : strata_map) {
        if (std::find(strata_order.begin(), strata_order.end(), kv.first) == strata_order.end()) {
            emit_order.push_back(kv.first);
        }
    }
    std::sort(emit_order.begin() + (long)strata_order.size(), emit_order.end());
    for (const auto& skey : emit_order) {
        auto it = strata_map.find(skey);
        if (it != strata_map.end() && !it->second.jobs.empty()) {
            nlohmann::json j_list = nlohmann::json::array();
            for (const auto& j : it->second.jobs) {
                j_list.push_back({
                    {"job_key", j.job_key},
                    {"job_name", j.job_name},
                    {"current_workforce", j.current_workforce},
                    {"max_workforce", j.max_workforce},
                    {"bonus_workforce", j.bonus_workforce},
                    {"effective_workforce", j.effective_workforce},
                    {"workforce_limit", j.workforce_limit},
                    {"is_prioritized", j.is_prioritized},
                    {"can_prioritize", j.can_prioritize}
                });
            }
            strata_arr.push_back({
                {"stratum_key", it->second.stratum_key},
                {"stratum_name", it->second.stratum_name},
                {"total_workforce", it->second.total_workforce},
                {"total_effective_workforce", it->second.total_effective_workforce},
                {"jobs", j_list}
            });
        }
    }

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"total_workforce", total_workforce},
        {"strata", strata_arr}
    };
}

nlohmann::json OutlinerManager::SetJobPriorityJson(uint32_t planet_id, const std::string& job_key) {
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions or base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "Planet has no active colony: " + std::to_string(planet_id)} };
    }

    void* colony_obj = FindColony(cid);
    if (!colony_obj) {
        return { {"success", false}, {"error", "Colony object not found: " + std::to_string(cid)} };
    }

    void* jobs_arr = nullptr;
    uint32_t jobs_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x38), &jobs_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x44), &jobs_cnt);

    void* job_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &job_db);
    if (!job_db || (uintptr_t)job_db < 0x10000) {
        return { {"success", false}, {"error", "PopJob database not found"} };
    }

    void* job_db_arr = nullptr;
    uint32_t job_db_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)job_db + 0x18), &job_db_arr);
    SafeReadU32((const void*)((uintptr_t)job_db + 0x20), &job_db_cap);

    void* target_job_type = nullptr;
    void* target_p_job = nullptr;
    uint32_t target_handle = 0;

    std::string q_key = job_key;
    std::transform(q_key.begin(), q_key.end(), q_key.begin(), ::tolower);
    if (q_key.rfind("job_", 0) == 0) {
        q_key = q_key.substr(4);
    }

    if (jobs_arr && job_db_arr && jobs_cnt > 0) {
        for (uint32_t idx = 0; idx < jobs_cnt; ++idx) {
            uint32_t h = 0;
            SafeReadU32((const void*)((uintptr_t)jobs_arr + idx * 4), &h);
            uint32_t slot = h & 0xFFFFFF;
            if (slot >= job_db_cap) continue;

            void* p_job = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)job_db_arr + slot * 16 + 8), &p_job) || !p_job) continue;

            void* jt = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job + 0x18), &jt);
            if (!jt) continue;

            std::string cur_key;
            SafeReadPdxString((const void*)((uintptr_t)jt + 0x20), cur_key);
            std::string cur_key_lower = cur_key;
            std::transform(cur_key_lower.begin(), cur_key_lower.end(), cur_key_lower.begin(), ::tolower);

            if (cur_key_lower == q_key) {
                target_job_type = jt;
                target_p_job = p_job;
                target_handle = h;
                break;
            }
        }
    }

    if (!target_job_type) {
        return {
            {"success", false},
            {"error", "Job type '" + job_key + "' not found on colony " + std::to_string(cid)}
        };
    }

    uint32_t country_id = GetPlayerCountryId();

    namespace fav = sdk::cmd::set_favorite_job_command;
    auto cmd = CommandBuilder::Get().Create(fav::kSpec);
    cmd.Set<uint32_t>(fav::country, country_id)
       .Set<uint32_t>(fav::colony, cid)
       .Set<void*>(fav::job, target_job_type);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Job cannot be prioritized" : "Job cannot be prioritized: " + why},
            {"planet_id", planet_id},
            {"job_key", job_key}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    bool currently_prio = false;
    void* map_entries = nullptr;
    uint32_t map_mask = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xee8), &map_entries);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xef4), &map_mask);
    if (map_entries && (uintptr_t)map_entries > 0x10000 && map_mask < 1024) {
        for (uint32_t mi = 0; mi <= map_mask; ++mi) {
            uint8_t occupied = 0;
            SafeReadU8((const void*)((uintptr_t)map_entries + mi * 24 + 4), &occupied);
            if (!occupied) continue;

            void* j = nullptr;
            SafeReadPtr((const void*)((uintptr_t)map_entries + mi * 24 + 16), &j);
            if (j == target_job_type) {
                currently_prio = true;
                break;
            }
        }
    }
    bool will_be_prioritized = !currently_prio;

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"job_key", q_key},
        {"is_prioritized", will_be_prioritized},
        {"message", will_be_prioritized ? "Job priority enabled" : "Job priority cleared"}
    };
}

nlohmann::json OutlinerManager::SetJobWorkforceLimitJson(uint32_t planet_id, const std::string& job_key, int32_t limit) {
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions or base address not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet not found: " + std::to_string(planet_id)} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    if (cid == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "Planet has no active colony: " + std::to_string(planet_id)} };
    }

    void* colony_obj = FindColony(cid);
    if (!colony_obj) {
        return { {"success", false}, {"error", "Colony object not found: " + std::to_string(cid)} };
    }

    void* jobs_arr = nullptr;
    uint32_t jobs_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0x38), &jobs_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x44), &jobs_cnt);

    void* job_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CPopJob), &job_db);
    if (!job_db || (uintptr_t)job_db < 0x10000) {
        return { {"success", false}, {"error", "PopJob database not found"} };
    }

    void* job_db_arr = nullptr;
    uint32_t job_db_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)job_db + 0x18), &job_db_arr);
    SafeReadU32((const void*)((uintptr_t)job_db + 0x20), &job_db_cap);

    void* target_p_job = nullptr;
    uint32_t target_handle = 0;
    int64_t max_wf = 0;

    std::string q_key = job_key;
    std::transform(q_key.begin(), q_key.end(), q_key.begin(), ::tolower);
    if (q_key.rfind("job_", 0) == 0) {
        q_key = q_key.substr(4);
    }

    if (jobs_arr && job_db_arr && jobs_cnt > 0) {
        for (uint32_t idx = 0; idx < jobs_cnt; ++idx) {
            uint32_t h = 0;
            SafeReadU32((const void*)((uintptr_t)jobs_arr + idx * 4), &h);
            uint32_t slot = h & 0xFFFFFF;
            if (slot >= job_db_cap) continue;

            void* p_job = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)job_db_arr + slot * 16 + 8), &p_job) || !p_job) continue;

            void* jt = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_job + 0x18), &jt);
            if (!jt) continue;

            std::string cur_key;
            SafeReadPdxString((const void*)((uintptr_t)jt + 0x20), cur_key);
            std::string cur_key_lower = cur_key;
            std::transform(cur_key_lower.begin(), cur_key_lower.end(), cur_key_lower.begin(), ::tolower);

            if (cur_key_lower == q_key) {
                target_p_job = p_job;
                target_handle = h;
                int64_t raw_max = 0;
                SafeReadI64((const void*)((uintptr_t)p_job + 0x100), &raw_max);  // 2026-11-18 更正：编制在 +0x100（旧的 0x98 是错的）
                max_wf = (raw_max > 0 && raw_max < 100000000000000LL) ? (raw_max / 100000) : 10000;
                break;
            }
        }
    }

    if (!target_p_job) {
        return {
            {"success", false},
            {"error", "Job type '" + job_key + "' not found on colony " + std::to_string(cid)}
        };
    }

    int32_t target_limit = limit;
    if (target_limit < 0) target_limit = (int32_t)max_wf;
    if (max_wf > 0 && target_limit > max_wf) target_limit = (int32_t)max_wf;

    uint32_t country_id = GetPlayerCountryId();

    // Engine token name: change_job_priority_command (CChangeJobWorkforceLimitCommand).
    // The CFixedPoint limit at +0x28 (token 0x1a1 "amount") is written from a register by the
    // serializer, which the SDK dumper does not pick up yet.
    namespace wf = sdk::cmd::change_job_priority_command;
    constexpr std::ptrdiff_t kWorkforceAmount = 0x28;
    auto cmd = CommandBuilder::Get().Create(wf::kSpec);
    cmd.Set<uint32_t>(wf::country, country_id)
       .Set<uint32_t>(wf::job, target_handle)
       .Set<int64_t>(kWorkforceAmount, (int64_t)target_limit * 100000);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Workforce limit cannot be changed" : "Workforce limit cannot be changed: " + why},
            {"planet_id", planet_id},
            {"job_key", job_key},
            {"target_limit", target_limit}
        };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"is_valid", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"job_key", q_key},
        {"workforce_limit", target_limit},
        {"max_workforce", max_wf},
        {"message", "Workforce limit updated successfully"}
    };
}

// -------------------------------------------------------------
// Planet Armies Subpage (Layer 1 Macro Summary & Layer 2 Deep Dive)
// -------------------------------------------------------------

std::string OutlinerManager::SpeciesName(uint32_t species_id) {
    std::string raw;
    if (void* sp = SpeciesManager::Get().FindSpeciesPtr(species_id)) {
        SafeReadPdxString((const void*)((uintptr_t)sp + 0x60), raw);
    }
    return raw.empty() ? "" : LocalizeKey(raw);
}

std::vector<armies::ArmyInfo> OutlinerManager::ReadColonyArmies(void* colony_obj) {
    std::vector<armies::ArmyInfo> out;
    void* list = nullptr;
    uint32_t count = 0;
    if (!colony_obj || !SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xD0), &list) || !list ||
        !SafeReadU32((const void*)((uintptr_t)colony_obj + 0xDC), &count) || count > 1000) {
        return out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0xFFFFFFFF;
        armies::ArmyInfo info;
        if (SafeReadU32((const void*)((uintptr_t)list + i * 4), &id) &&
            armies::Read(armies::Find(base_address_, id), info)) {
            out.push_back(info);
        }
    }
    return out;
}

nlohmann::json OutlinerManager::ExtractArmiesSummary(void* p_obj, void* colony_obj) {
    if (!colony_obj) {
        return {
            {"total_stationed_armies", 0},
            {"garrison_power", 0.0},
            {"assault_power", 0.0},
            {"total_power", 0.0},
            {"defense_armies_count", 0},
            {"assault_armies_count", 0},
            {"deploy_in_orbit", false},
            {"include_in_builder", false},
            {"recruitment_queue_count", 0}
        };
    }

    uint32_t defense_cnt = 0;
    uint32_t assault_cnt = 0;
    double garrison_power = 0.0;
    double assault_power = 0.0;
    for (const auto& army : ReadColonyArmies(colony_obj)) {
        if (army.defensive) {
            defense_cnt++;
            garrison_power += army.power;
        } else {
            assault_cnt++;
            assault_power += army.power;
        }
    }

    bool deploy_in_orbit = false;
    if (p_obj) {
        uint32_t c8c = 0;
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xC8C), &c8c);
        deploy_in_orbit = (c8c & 0x40) != 0;
    }

    bool include_in_builder = false;
    uint32_t v1088 = 0;
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1008), &v1088);
    include_in_builder = (v1088 & 8) != 0;

    uint32_t army_queue_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xC0), &army_queue_id);
    uint32_t queue_cnt = 0;
    if (army_queue_id != 0xFFFFFFFF) {
        void* mgr_eb8 = nullptr;
        if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8) || !mgr_eb8 || (uintptr_t)mgr_eb8 < 0x10000) {
            SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8);
        }
        if (mgr_eb8) {
            void* arr_eb8 = nullptr;
            uint32_t cap_eb8 = 0;
            SafeReadPtr((const void*)((uintptr_t)mgr_eb8 + 0x18), &arr_eb8);
            SafeReadU32((const void*)((uintptr_t)mgr_eb8 + 0x20), &cap_eb8);
            uint32_t q_slot = army_queue_id & 0xFFFFFF;
            if (arr_eb8 && q_slot < cap_eb8) {
                void* queue_obj = nullptr;
                SafeReadPtr((const void*)((uintptr_t)arr_eb8 + q_slot * 16 + 8), &queue_obj);
                if (queue_obj) {
                    SafeReadU32((const void*)((uintptr_t)queue_obj + 0x2C), &queue_cnt);
                }
            }
        }
    }

    double total_power = garrison_power + assault_power;
    return {
        {"total_stationed_armies", defense_cnt + assault_cnt},
        {"garrison_power", std::round(garrison_power * 10.0) / 10.0},
        {"assault_power", std::round(assault_power * 10.0) / 10.0},
        {"total_power", std::round(total_power * 10.0) / 10.0},
        {"defense_armies_count", defense_cnt},
        {"assault_armies_count", assault_cnt},
        {"deploy_in_orbit", deploy_in_orbit},
        {"include_in_builder", include_in_builder},
        {"recruitment_queue_count", queue_cnt}
    };
}

nlohmann::json OutlinerManager::GetPlanetArmiesJson(uint32_t planet_id, uint32_t colony_id){
    if (!base_address_) {
        return { {"success", false}, {"error", "Base address not initialized"} };
    }

    // 方舟没有行星：给了 colony_id 就跳过 FindPlanet（p_obj 留空，行星专属的那几段自动为空）
    void* p_obj = nullptr;
    uint32_t cid = 0xFFFFFFFF;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
    } else {
        p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"success", false}, {"error", "Planet ID " + std::to_string(planet_id) + " not found"} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    }
    void* colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;
    if (!colony_obj) {
        return { {"success", false}, {"error", "Planet/colony has no active colony object"} };
    }

    std::string p_name = p_obj ? PlanetName(p_obj) : std::string("(方舟)");

    nlohmann::json overview = ExtractArmiesSummary(p_obj, colony_obj);

    // 1. Stationed Armies
    nlohmann::json stationed_armies = nlohmann::json::array();
    for (const auto& army : ReadColonyArmies(colony_obj)) {
        nlohmann::json j = armies::ToJson(army);
        j["species_name"] = SpeciesName(army.species);
        stationed_armies.push_back(j);
    }

    // 2. Recruitable Armies Catalog
    nlohmann::json recruitable_armies = nlohmann::json::array();
    void* army_type_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CArmyTypeDatabase_pInstance), &army_type_db);
    if (army_type_db && (uintptr_t)army_type_db > 0x10000) {
        void* type_arr = nullptr;
        uint32_t type_cnt = 0;
        SafeReadPtr((const void*)((uintptr_t)army_type_db + 0x50), &type_arr);
        SafeReadU32((const void*)((uintptr_t)army_type_db + 0x5C), &type_cnt);
        if (type_arr && type_cnt > 0 && type_cnt < 200) {
            for (uint32_t ti = 0; ti < type_cnt; ++ti) {
                void* p_type = nullptr;
                SafeReadPtr((const void*)((uintptr_t)type_arr + ti * 8), &p_type);
                if (!p_type) continue;

                // CArmyType (ReadMember): +0x352 defensive, +0x358 build time, +0x360 health,
                // +0x368 morale, +0x370 damage, +0x378 morale damage multipliers.
                uint8_t defensive = 0;
                SafeReadU8((const void*)((uintptr_t)p_type + 0x352), &defensive);
                if (defensive) continue;  // defense armies are not recruited

                int64_t build_time_raw = 0;
                SafeReadI64((const void*)((uintptr_t)p_type + 0x358), &build_time_raw);
                if (build_time_raw <= 0) continue; // Skip non-buildable armies
                uint32_t build_days = (uint32_t)(build_time_raw / 100000);

                std::string a_key;
                SafeReadPdxString((const void*)((uintptr_t)p_type + 0x20), a_key);
                if (a_key.empty()) continue;

                std::string loc_name = LocalizeKey(a_key);
                if (loc_name.empty()) loc_name = a_key;

                auto mult = [&](std::ptrdiff_t off) {
                    int64_t raw = 0;
                    SafeReadI64((const void*)((uintptr_t)p_type + off), &raw);
                    return std::round(raw / 1000.0) / 100.0;
                };

                recruitable_armies.push_back({
                    {"key", a_key},
                    {"name", loc_name},
                    {"build_time_days", build_days},
                    {"health_mult", mult(0x360)},
                    {"damage_mult", mult(0x370)},
                    {"morale_mult", mult(0x368)},
                    {"morale_damage_mult", mult(0x378)}
                });
            }
        }
    }

    // 3. Army Construction Queue
    nlohmann::json construction_queue = nlohmann::json::array();
    uint32_t army_queue_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xC0), &army_queue_id);
    if (army_queue_id != 0xFFFFFFFF) {
        void* mgr_eb8 = nullptr;
        if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8) || !mgr_eb8 || (uintptr_t)mgr_eb8 < 0x10000) {
            SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueue), &mgr_eb8);
        }
        if (mgr_eb8) {
            void* arr_eb8 = nullptr;
            uint32_t cap_eb8 = 0;
            SafeReadPtr((const void*)((uintptr_t)mgr_eb8 + 0x18), &arr_eb8);
            SafeReadU32((const void*)((uintptr_t)mgr_eb8 + 0x20), &cap_eb8);
            uint32_t q_slot = army_queue_id & 0xFFFFFF;
            if (arr_eb8 && q_slot < cap_eb8) {
                void* queue_obj = nullptr;
                SafeReadPtr((const void*)((uintptr_t)arr_eb8 + q_slot * 16 + 8), &queue_obj);
                if (queue_obj) {
                    void* q_items = nullptr;
                    uint32_t q_cnt = 0;
                    SafeReadPtr((const void*)((uintptr_t)queue_obj + 0x20), &q_items);
                    SafeReadU32((const void*)((uintptr_t)queue_obj + 0x2C), &q_cnt);

                    void* mgr_ea8 = nullptr;
                    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8) || !mgr_ea8 || (uintptr_t)mgr_ea8 < 0x10000) {
                        SafeReadPtr((const void*)(base_address_ + sdk::db::CConstructionQueueItem), &mgr_ea8);
                    }
                    if (mgr_ea8) {
                        void* arr_ea8 = nullptr;
                        uint32_t cap_ea8 = 0;
                        SafeReadPtr((const void*)((uintptr_t)mgr_ea8 + 0x18), &arr_ea8);
                        SafeReadU32((const void*)((uintptr_t)mgr_ea8 + 0x20), &cap_ea8);

                        for (uint32_t qi = 0; qi < q_cnt; ++qi) {
                            uint32_t item_id = 0;
                            if (!SafeReadU32((const void*)((uintptr_t)q_items + qi * sizeof(uint32_t)), &item_id)) continue;
                            uint32_t i_slot = item_id & 0xFFFFFF;
                            if (i_slot >= cap_ea8) continue;
                            void* item_obj = nullptr;
                            if (!SafeReadPtr((const void*)((uintptr_t)arr_ea8 + i_slot * 16 + 8), &item_obj) || !item_obj) continue;

                            uint32_t prog = 0, tot = 0;
                            SafeReadU32((const void*)((uintptr_t)item_obj + 0x28), &prog);
                            SafeReadU32((const void*)((uintptr_t)item_obj + 0x30), &tot);
                            void* action_obj = nullptr;
                            SafeReadPtr((const void*)((uintptr_t)item_obj + 0x18), &action_obj);

                            std::string item_key;
                            uint32_t sp_id = 0xFFFFFFFF;
                            if (action_obj) {
                                void* p_t = nullptr;
                                SafeReadPtr((const void*)((uintptr_t)action_obj + 8), &p_t);
                                if (p_t) SafeReadPdxString((const void*)((uintptr_t)p_t + 0x20), item_key);
                                SafeReadU32((const void*)((uintptr_t)action_obj + 0x10), &sp_id);
                            }

                            std::string sp_name;
                            if (sp_id != 0 && sp_id != 0xFFFFFFFF) {
                                void* sp_ptr = SpeciesManager::Get().FindSpeciesPtr(sp_id & 0xFFFFFF);
                                if (sp_ptr) {
                                    std::string raw_sn;
                                    SafeReadPdxString((const void*)((uintptr_t)sp_ptr + 0x60), raw_sn);
                                    sp_name = LocalizeKey(raw_sn);
                                }
                            }

                            std::string item_name = LocalizeKey(item_key);
                            if (item_name.empty()) item_name = item_key;
                            if (!sp_name.empty()) item_name += " (" + sp_name + ")";

                            uint32_t norm_prog = prog / 100000;
                            uint32_t norm_tot = tot / 100000;
                            uint32_t norm_rem = (tot > prog) ? ((tot - prog) / 100000) : 0;

                            construction_queue.push_back({
                                {"slot_index", qi},
                                {"item_id", item_id},
                                {"key", item_key},
                                {"name", item_name},
                                {"progress_days", norm_prog},
                                {"total_days", norm_tot},
                                {"progress_percent", tot > 0 ? std::round(((double)prog * 100.0 / (double)tot) * 10.0) / 10.0 : 0.0},
                                {"remaining_days", norm_rem},
                                {"species_id", sp_id},
                                {"species_name", sp_name}
                            });
                        }
                    }
                }
            }
        }
    }

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"planet_name", p_name},
        {"overview", overview},
        {"stationed_armies", stationed_armies},
        {"recruitable_armies", recruitable_armies},
        {"construction_queue", construction_queue}
    };
}

nlohmann::json OutlinerManager::SetPlanetArmySettingsJson(uint32_t planet_id, std::optional<bool> deploy_in_orbit, std::optional<bool> include_in_builder) {
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet ID " + std::to_string(planet_id) + " not found"} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    void* colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;
    if (!colony_obj) {
        return { {"success", false}, {"error", "Planet has no active colony"} };
    }

    uint32_t country_id = GetPlayerCountryId();
    typedef void* (*FnCloneCmd)(void*);

    // 1. Checkbox 1: deploy_in_orbit
    if (deploy_in_orbit.has_value()) {
        uint32_t c8c = 0;
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xC8C), &c8c);
        bool cur_deploy = (c8c & 0x40) != 0;
        if (cur_deploy != *deploy_in_orbit) {
            uint32_t planet_handle = 0;
            SafeReadU32((const void*)((uintptr_t)p_obj + 0x18), &planet_handle);
            if (planet_handle == 0) planet_handle = planet_id;

            namespace dep = sdk::cmd::toggle_deploy_in_orbit_command;
            auto cmd = CommandBuilder::Get().Create(dep::kSpec);
            cmd.Set<uint32_t>(dep::country, country_id).Set<uint32_t>(dep::planet, planet_handle);
            cmd.Post();
        }
    }

    // 2. Checkbox 2: include_in_builder
    if (include_in_builder.has_value()) {
        uint32_t v1088 = 0;
        SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1008), &v1088);
        bool cur_builder = (v1088 & 8) != 0;
        if (cur_builder != *include_in_builder) {
            uint32_t colony_handle = 0;
            SafeReadU32((const void*)((uintptr_t)colony_obj + 0x10), &colony_handle);
            if (colony_handle == 0) colony_handle = cid;

            namespace inc = sdk::cmd::toggle_include_in_army_builder_command;
            auto cmd = CommandBuilder::Get().Create(inc::kSpec);
            cmd.Set<uint32_t>(inc::country, country_id).Set<uint32_t>(inc::colony, colony_handle);
            cmd.Post();
        }
    }

    // Re-read current states
    uint32_t final_c8c = 0, final_1088 = 0;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xC8C), &final_c8c);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0x1008), &final_1088);

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"deploy_in_orbit", deploy_in_orbit.value_or((final_c8c & 0x40) != 0)},
        {"include_in_builder", include_in_builder.value_or((final_1088 & 8) != 0)},
        {"message", "Planet army settings updated successfully"}
    };
}

nlohmann::json OutlinerManager::EmbarkAllArmiesJson(uint32_t planet_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet ID " + std::to_string(planet_id) + " not found"} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    void* colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;
    if (!colony_obj) {
        return { {"success", false}, {"error", "Planet has no active colony"} };
    }

    void* armies_arr = nullptr;
    uint32_t armies_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xD0), &armies_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xDC), &armies_cnt);

    void* army_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CArmy), &army_db);
    void* army_db_arr = nullptr;
    uint32_t army_db_cap = 0;
    if (army_db && (uintptr_t)army_db > 0x10000) {
        SafeReadPtr((const void*)((uintptr_t)army_db + 0x18), &army_db_arr);
        SafeReadU32((const void*)((uintptr_t)army_db + 0x20), &army_db_cap);
    }

    std::vector<uint32_t> assault_handles;
    if (armies_arr && army_db_arr && armies_cnt > 0) {
        for (uint32_t i = 0; i < armies_cnt; ++i) {
            uint32_t h = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)armies_arr + i * 4), &h);
            if (h == 0xFFFFFFFF) continue;
            uint32_t slot = h & 0xFFFFFF;
            if (slot >= army_db_cap) continue;

            void* p_army = nullptr;
            SafeReadPtr((const void*)((uintptr_t)army_db_arr + slot * 16 + 8), &p_army);
            if (!p_army) continue;

            void* p_type = nullptr;
            SafeReadPtr((const void*)((uintptr_t)p_army + 0x138), &p_type);
            uint8_t defensive = 0;  // CArmyType +0x352 "defensive"
            if (p_type) SafeReadU8((const void*)((uintptr_t)p_type + 0x352), &defensive);
            if (!defensive) {
                assault_handles.push_back(h);
            }
        }
    }

    if (assault_handles.empty()) {
        return {
            {"success", false},
            {"error", "No assault armies stationed on planet " + std::to_string(planet_id) + " to embark (defense armies cannot embark)"}
        };
    }

    namespace mv = sdk::cmd::move_army_to_orbit_command;
    auto cmd = CommandBuilder::Get().Create(mv::kSpec);
    std::string why;
    if (!SetArmyRefs(cmd, mv::army, assault_handles, &why)) {
        return { {"success", false}, {"error", why} };
    }
    cmd.Set<uint8_t>(mv::retreat, 0);
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "Armies cannot embark" : "Armies cannot embark: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"embarked_count", assault_handles.size()},
        {"message", "All assault armies successfully embarked to orbit"}
    };
}

nlohmann::json OutlinerManager::DisbandArmyJson(uint32_t planet_id, uint32_t army_id) {
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* p_obj = FindPlanet(planet_id);
    if (!p_obj) {
        return { {"success", false}, {"error", "Planet ID " + std::to_string(planet_id) + " not found"} };
    }

    uint32_t cid = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
    void* colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;
    if (!colony_obj) {
        return { {"success", false}, {"error", "Planet has no active colony"} };
    }

    void* armies_arr = nullptr;
    uint32_t armies_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xD0), &armies_arr);
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xDC), &armies_cnt);

    uint32_t target_handle = 0xFFFFFFFF;
    if (armies_arr && armies_cnt > 0) {
        for (uint32_t i = 0; i < armies_cnt; ++i) {
            uint32_t h = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)armies_arr + i * 4), &h);
            if ((h & 0xFFFFFF) == army_id || h == army_id) {
                target_handle = h;
                break;
            }
        }
    }

    if (target_handle == 0xFFFFFFFF) {
        return {
            {"success", false},
            {"error", "Army ID " + std::to_string(army_id) + " is not stationed on planet " + std::to_string(planet_id)}
        };
    }

    uint32_t disband_h = target_handle;
    // CDisbandArmyCommand carries the same CPdxArray<TPdxRef<CArmy>> at +0x20 as
    // CMoveArmyToOrbitCommand (its serializer reads the array there; the SDK has no field for it).
    namespace db = sdk::cmd::disband_army_command;
    constexpr std::ptrdiff_t kArmies = 0x20;
    auto cmd = CommandBuilder::Get().Create(db::kSpec);
    std::string why;
    if (!SetArmyRefs(cmd, kArmies, { disband_h }, &why)) {
        return { {"success", false}, {"error", why} };
    }
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "Army cannot be disbanded" : "Army cannot be disbanded: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"army_id", army_id},
        {"message", "Army disbanded successfully"}
    };
}

nlohmann::json OutlinerManager::RecruitArmyJson(uint32_t planet_id, const std::string& army_key, std::optional<uint32_t> species_id, uint32_t colony_id){
    if (!base_address_ || !fn_post_command_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    // 方舟：没有行星，直接给 colony_id（陆军的建造队列在 colony +0xC0，两类殖民地同一个字段）
    uint32_t cid = 0xFFFFFFFF;
    void* colony_obj = nullptr;
    if (colony_id != 0xFFFFFFFF) {
        cid = colony_id;
        colony_obj = FindColony(cid);
    } else {
        void* p_obj = FindPlanet(planet_id);
        if (!p_obj) {
            return { {"success", false}, {"error", "Planet ID " + std::to_string(planet_id) + " not found"} };
        }
        SafeReadU32((const void*)((uintptr_t)p_obj + 0xe0), &cid);
        colony_obj = (cid != 0xFFFFFFFF) ? FindColony(cid) : nullptr;
    }
    if (!colony_obj) {
        return { {"success", false}, {"error", "Planet/colony has no active colony object"} };
    }

    uint32_t army_queue_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)colony_obj + 0xC0), &army_queue_id);
    if (army_queue_id == 0xFFFFFFFF) {
        // 方舟没有独立的陆军队列（CColony::army_build_queue 为空）：陆军与建筑共用载体上的建造队列
        // （行星 CPlanet +0xE4 / 方舟 CShip +0x104），这也是玩家在界面上看到的那条。
        army_queue_id = GetColonyQueueId(cid);
    }
    if (army_queue_id == 0xFFFFFFFF) {
        return { {"success", false}, {"error", "Colony has no active army recruitment queue"} };
    }

    // Locate CArmyType in database
    void* army_type_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CArmyTypeDatabase_pInstance), &army_type_db);
    if (!army_type_db || (uintptr_t)army_type_db < 0x10000) {
        return { {"success", false}, {"error", "ArmyType database not found"} };
    }

    void* type_arr = nullptr;
    uint32_t type_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)army_type_db + 0x50), &type_arr);
    SafeReadU32((const void*)((uintptr_t)army_type_db + 0x5C), &type_cnt);

    void* target_p_type = nullptr;
    std::string q_key = army_key;
    std::transform(q_key.begin(), q_key.end(), q_key.begin(), ::tolower);

    if (type_arr && type_cnt > 0) {
        for (uint32_t ti = 0; ti < type_cnt; ++ti) {
            void* p_type = nullptr;
            SafeReadPtr((const void*)((uintptr_t)type_arr + ti * 8), &p_type);
            if (!p_type) continue;
            std::string cur_k;
            SafeReadPdxString((const void*)((uintptr_t)p_type + 0x20), cur_k);
            std::string cur_k_low = cur_k;
            std::transform(cur_k_low.begin(), cur_k_low.end(), cur_k_low.begin(), ::tolower);
            if (cur_k_low == q_key) {
                target_p_type = p_type;
                break;
            }
        }
    }

    if (!target_p_type) {
        return { {"success", false}, {"error", "Army type '" + army_key + "' not found in catalog"} };
    }

    uint8_t defensive = 0;  // CArmyType +0x352 "defensive"
    SafeReadU8((const void*)((uintptr_t)target_p_type + 0x352), &defensive);
    if (defensive) {
        return { {"success", false}, {"error", "Defense armies cannot be recruited directly"} };
    }

    // 物种按调用方给的来：**不给就是不给**（0xFFFFFFFF）。
    // 特殊陆军（如 machine_assault_3 巨型战体）根本不带物种 —— 引擎自己那条的 +0x10 正是
    // 0xFFFFFFFF（2026-11-22 probe_buildable 实测），硬填主族反而被拒。
    uint32_t target_species = species_id.value_or(0xFFFFFFFF);

    // 1. Construct CBuildableArmy (0x28 bytes)
    alignas(16) uint8_t action_obj[0x28]{ 0 };
    *(void**)(action_obj + 0x00) = (void*)(base_address_ + kBuildableArmyVt); // CBuildableArmy true vtable
    *(void**)(action_obj + 0x08) = target_p_type;
    *(uint32_t*)(action_obj + 0x10) = target_species;
    *(uint32_t*)(action_obj + 0x14) = 0;
    *(uint32_t*)(action_obj + 0x18) = cid; // Colony ID from [colony_obj + 0x10]
    *(uint32_t*)(action_obj + 0x1C) = 0;
    // 方舟的陆军是**基地**造的：CBuildableArmy 的 starbase 字段（+0x20）必须填方舟基地的 id。
    // 行星上这个字段留 null 就行（2026-11-22 界面投「巨型战体」后对照出来的）。
    uint32_t army_starbase = 0xFFFFFFFF;
    {
        uint64_t carrier = 0;
        SafeReadI64((const void*)((uintptr_t)colony_obj + sdk::ent::CColony::carrier), (int64_t*)&carrier);
        uint32_t carrier_type = (uint32_t)(carrier >> 32);
        uint32_t carrier_id_full = (uint32_t)(carrier & 0xFFFFFFFFull);
        if (carrier_type == 1) {
            void* smgr = nullptr;
            void* sarr = nullptr;
            uint32_t scap = 0;
            if (SafeReadPtr((const void*)(base_address_ + sdk::db::CStarbase), &smgr) && smgr &&
                SafeReadPtr((const void*)((uintptr_t)smgr + 0x18), &sarr) && sarr &&
                SafeReadU32((const void*)((uintptr_t)smgr + 0x20), &scap) && scap < 100000) {
                for (uint32_t slot = 0; slot < scap; ++slot) {
                    void* sb = nullptr;
                    if (!SafeReadPtr((const void*)((uintptr_t)sarr + slot * 16 + 8), &sb) || !sb) continue;
                    uint32_t station = 0xFFFFFFFF;
                    if (!SafeReadU32((const void*)((uintptr_t)sb + sdk::ent::CStarbase::station), &station)) continue;
                    if (station != carrier_id_full) continue;
                    SafeReadU32((const void*)((uintptr_t)sb + 0x8), &army_starbase);
                    break;
                }
            }
        }
    }
    // 2026-11-22 23:40 用 probe_buildable 抓了引擎自己造的那三个（machine_assault_1/2/3）：
    //   vtable_rva 0x237C8E8（CBuildableArmy，同一个类）· +0x08 army_type 指针 · +0x10 species 193
    //   +0x18 colony 0 · **+0x20 starbase = 0xFFFFFFFF（null，不是基地 id）**
    // 所以 starbase 一律留 null —— 之前「补上基地 id」的想法是错的。
    (void)army_starbase;
    *(uint32_t*)(action_obj + 0x20) = 0xFFFFFFFF;
    *(uint32_t*)(action_obj + 0x24) = 0;

    // 2. Construct CAddBuildableToQueueCommand (0x30 bytes)
    uint32_t country_id = GetPlayerCountryId();
    // CAddBuildableToQueueCommand takes ownership of the buildable (engine heap copy)
    std::string why;
    if (!QueueBuildable(action_obj, sizeof(action_obj), country_id, army_queue_id, true, &why)) {
        return {
            {"success", false},
            {"error", why.empty() ? "Cannot be queued (prerequisites not met)" : "Cannot be queued: " + why},
            {"planet_id", planet_id},
            {"queue_id", army_queue_id}
        };
    }

    return {
        {"success", true},
        {"planet_id", planet_id},
        {"colony_id", cid},
        {"army_key", q_key},
        {"queue_id", army_queue_id},
        {"species_id", target_species},
        {"message", "Army recruitment queued successfully"}
    };
}

bool OutlinerManager::QueueBuildable(const void* buildable, size_t size, uint32_t country_id, uint32_t queue_id,
                                     bool dispatch, std::string* why) {
    namespace q = sdk::cmd::add_buildable_to_queue_command;
    // +0x20: the owned CBuildable* (the serializer writes it through the buildable's own
    // CSerializer, which the SDK dumper does not expand).
    constexpr std::ptrdiff_t kBuildable = 0x20;

    // Reject anything that is not shaped like a CBuildable vtable (slots 2/3 = base class
    // implementation, same as the army buildable's) before the engine calls into it.
    uintptr_t vt = *(const uintptr_t*)buildable;
    uintptr_t ref = base_address_ + kBuildableArmyVt;
    uint64_t vt2 = 0, vt3 = 0, ref2 = 0;
    // A concrete buildable may override slot 2 (CBuildableStarbaseModule does), but slot 3 is the
    // base implementation in every buildable vtable seen so far; accept either slot matching it.
    if (!SafeReadU64((const void*)(vt + 16), &vt2) || !SafeReadU64((const void*)(vt + 24), &vt3) ||
        !SafeReadU64((const void*)(ref + 16), &ref2) ||
        (vt2 != ref2 && vt3 != ref2) || vt3 != ref2) {
        if (why) *why = "internal error: buildable vtable does not look like a CBuildable";
        LOGF("[OUTLINER] refusing buildable with vtable 0x%llX", (unsigned long long)(vt - base_address_));
        return false;
    }
    auto cmd = CommandBuilder::Get().Create(q::kSpec);
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    // The command deletes its buildable, so it must live on the engine heap even for a probe.
    void* heap = CommandBuilder::Get().EngineAlloc(size);
    if (!heap) {
        if (why) *why = "engine allocation for the buildable failed";
        return false;
    }
    memcpy(heap, buildable, size);
    return QueueCommandWithBuildable(cmd, heap, country_id, queue_id, dispatch, why);
}

// A buildable the engine built on its own heap (NConstruction::CreateBuildable): the command takes
// it over and frees it with itself.
bool OutlinerManager::QueueOwnedBuildable(void* heap_buildable, uint32_t country_id, uint32_t queue_id,
                                          bool dispatch, std::string* why) {
    namespace q = sdk::cmd::add_buildable_to_queue_command;
    auto cmd = CommandBuilder::Get().Create(q::kSpec);
    if (!cmd) {
        if (why) *why = cmd.error();
        LOGF("[OUTLINER] queue command not created; buildable %p not freed", heap_buildable);
        return false;
    }
    return QueueCommandWithBuildable(cmd, heap_buildable, country_id, queue_id, dispatch, why);
}

bool OutlinerManager::QueueCommandWithBuildable(NativeCommand& cmd, void* heap_buildable, uint32_t country_id,
                                                uint32_t queue_id, bool dispatch, std::string* why) {
    namespace q = sdk::cmd::add_buildable_to_queue_command;
    constexpr std::ptrdiff_t kBuildable = 0x20;  // the owned CBuildable* (see QueueBuildable)
    cmd.Set<void*>(kBuildable, heap_buildable)
       .Set<uint32_t>(q::country, country_id)
       .Set<uint32_t>(q::queue, queue_id);
    if (!cmd.IsValid(why)) {
        return false;  // destroying the command frees the buildable
    }
    if (!dispatch) {
        return true;
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        if (why) *why = cmd.error();
        return false;
    }
    return true;
}

bool OutlinerManager::SetArmyRefs(NativeCommand& cmd, std::ptrdiff_t array_off, const std::vector<uint32_t>& ids,
                                  std::string* why) {
    // CPdxArray<TPdxRef<CArmy>> embedded in the command: vtable +0 (set by the factory), data +8,
    // capacity +0x10, size +0x14. The command frees the data, so it comes from the engine heap.
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    void* data = CommandBuilder::Get().EngineAlloc(ids.size() * sizeof(uint32_t));
    if (!data) {
        if (why) *why = "engine allocation for the army list failed";
        return false;
    }
    memcpy(data, ids.data(), ids.size() * sizeof(uint32_t));
    cmd.Set<void*>(array_off + 0x08, data)
       .Set<uint32_t>(array_off + 0x10, (uint32_t)ids.size())
       .Set<uint32_t>(array_off + 0x14, (uint32_t)ids.size());
    return true;
}

} // namespace bridge


