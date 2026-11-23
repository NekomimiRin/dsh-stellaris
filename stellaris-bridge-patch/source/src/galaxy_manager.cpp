#include "galaxy_manager.hpp"
#include "command_builder.hpp"
#include "fleet_access.hpp"
#include "game_state.hpp"
#include "government_manager.hpp"
#include "outliner_manager.hpp"
#include "ship_designer.hpp"
#include "situation_log_manager.hpp"
#include "species_manager.hpp"
#include "sdk/stellaris_sdk.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <cstring>
#include <deque>
#include <queue>
#include <set>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <MinHook.h>

namespace bridge {
namespace {

constexpr uint32_t kInvalidId = 0xFFFFFFFF;
constexpr double kFixed = 100000.0;  // CFixedPoint
constexpr int kMaxJumps = 12;

// CPdxArray object {vtable, data @ +8, capacity @ +0x10, size @ +0x14} (ref_array fields such as
// CGalacticObject::starbases / fleet_presence); CGalacticObject::hyperlane is a plain
// {data, size @ +8} array of CHyperlane (0x20 bytes each).
constexpr std::ptrdiff_t kArrData = 0x8;
constexpr std::ptrdiff_t kArrCapacity = 0x10;
constexpr std::ptrdiff_t kArrSize = 0x14;
constexpr std::ptrdiff_t kPlainSize = 0x8;
constexpr size_t kHyperlaneSize = 0x20;
// A planet's CDepositHolder base (holder type at +0xB4 is 0 for planets; the id before it at +0x18);
// what CCountry::_HasSurveyedDepositHolder and the survey command take.
constexpr std::ptrdiff_t kPlanetDepositHolder = 0x20;
// CMetaRef<CDepositHolder> {vtable, type @ +8, id @ +0xC}; type 0 = planet, 3 = none
constexpr std::ptrdiff_t kMetaRefType = 0x8;
constexpr std::ptrdiff_t kMetaRefId = 0xC;
constexpr uint32_t kHolderPlanet = 0;
constexpr uint32_t kHolderNone = 3;
// key strings of script types: planet class (+0x28), deposit type and starbase level (+0x20)
constexpr std::ptrdiff_t kPlanetClassKey = 0x28;
constexpr std::ptrdiff_t kTypeKey = 0x20;
// CBypass::type -> CBypassType, key ("gateway", "wormhole", "relay_bypass", ...) at +0x28
constexpr std::ptrdiff_t kBypassTypeKey = 0x28;
// ESpatialObjectType (CSpatialObjectRefCaster::PointerFromTypeAndID): the kinds a spatial
// reference {vtable, type +8, id +0xC} (CArchaeologicalSite::location) points at
constexpr uint32_t kSpatialPlanet = 2;
constexpr uint32_t kSpatialSystem = 4;
constexpr uint32_t kSpatialMegastructure = 6;
constexpr uint32_t kSpatialFleet = 3;
constexpr uint32_t kSpatialDebris = 5;
constexpr uint32_t kSpatialNaturalWormhole = 7;
constexpr uint32_t kSpatialAstralRift = 9;
// CRefObjectOrbitableRef<CFleetOrbitableEnumType> (fleet_orbit_planet::orbitable): id +0, kind
// byte +4 (1 = planet, as CFleetOrbitPlanetCommand(fleet, TPdxRef<CPlanet>, ...) builds it)
constexpr std::ptrdiff_t kOrbitableId = 0x0;
constexpr std::ptrdiff_t kOrbitableKind = 0x4;
constexpr uint8_t kOrbitablePlanet = 1;
// CColony::carrier {planet id, carrier type (0 = planet)}
constexpr std::ptrdiff_t kCarrierId = 0x0;

template <typename T>
bool Read(const void* addr, T* out) {
    __try {
        *out = *(const T*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
T ReadOr(uintptr_t addr, T fallback) {
    T v{};
    return Read((const void*)addr, &v) ? v : fallback;
}

bool CopyChars(char* dst, const char* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// MSVC std::string: inline buffer / heap pointer [16], size, capacity
std::string ReadString(uintptr_t addr) {
    struct Raw {
        union {
            char buf[16];
            const char* ptr;
        };
        uint64_t size;
        uint64_t cap;
    } raw{};
    if (!Read((const void*)addr, &raw) || raw.size == 0 || raw.size > 512) return "";
    std::string out(raw.size, '\0');
    const char* src = raw.cap >= 16 ? raw.ptr : (const char*)addr;
    return CopyChars(&out[0], src, raw.size) ? out : "";
}

std::string KeyOf(uintptr_t type_ptr_addr, std::ptrdiff_t key_off) {
    void* t = nullptr;
    return Read((const void*)type_ptr_addr, &t) && t ? ReadString((uintptr_t)t + key_off) : "";
}

// TPdxRefDatabase<T>: arr at +0x18 (16-byte slots, object at +8), capacity at +0x20
void* RefLookup(uintptr_t base, uintptr_t db_rva, uint32_t id) {
    if (id == kInvalidId) return nullptr;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!Read((const void*)(base + db_rva), &db) || !db || !Read((const void*)((uintptr_t)db + 0x18), &arr) ||
        !arr || !Read((const void*)((uintptr_t)db + 0x20), &cap) || (id & 0xFFFFFF) >= cap) {
        return nullptr;
    }
    void* obj = nullptr;
    return Read((const void*)((uintptr_t)arr + (id & 0xFFFFFF) * 16 + 8), &obj) ? obj : nullptr;
}

template <typename F>
void ForEachRef(uintptr_t base, uintptr_t db_rva, F&& f) {
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!Read((const void*)(base + db_rva), &db) || !db || !Read((const void*)((uintptr_t)db + 0x18), &arr) ||
        !arr || !Read((const void*)((uintptr_t)db + 0x20), &cap) || cap > 1000000) {
        return;
    }
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (Read((const void*)((uintptr_t)arr + i * 16 + 8), &obj) && obj) f(i, obj);
    }
}

std::vector<uint32_t> RefArray(uintptr_t arr_obj) {
    std::vector<uint32_t> out;
    void* data = nullptr;
    int32_t n = 0;
    if (!Read((const void*)(arr_obj + kArrData), &data) || !data || !Read((const void*)(arr_obj + kArrSize), &n) ||
        n <= 0 || n > 100000) {
        return out;
    }
    for (int32_t i = 0; i < n; ++i) {
        uint32_t v = kInvalidId;
        // empty slots hold 0xFFFFFFFF (a system without a starbase keeps one)
        if (Read((const void*)((uintptr_t)data + i * 4), &v) && v != kInvalidId) out.push_back(v);
    }
    return out;
}

// bool/uint8 engine calls: fn(a, b)
struct Call2Ctx {
    uintptr_t fn;
    const void* a;
    const void* b;
    uint8_t result;
};

void CallByte2(void* c, void*) {
    auto* x = (Call2Ctx*)c;
    x->result = ((uint8_t(*)(const void*, const void*))x->fn)(x->a, x->b);
}

bool EngineByte(uintptr_t fn, const void* a, const void* b, uint8_t* out) {
    Call2Ctx ctx{ fn, a, b, 0 };
    if (!CommandBuilder::Get().CallGuarded(&CallByte2, &ctx)) return false;
    *out = ctx.result;
    return true;
}

// CCelestialCoordinate {vtable, x, y, .., origin system, .., randomized}: the engine copies it
// member-wise up to `randomized`
struct Coord {
    alignas(8) uint8_t raw[(sdk::ent::CCelestialCoordinate::randomized + 1 + 7) & ~7];
};

// A system's centre as CCelestialCoordinate(system, 0, 0) builds it: x = y = 0, origin = system
Coord SystemCentre(uintptr_t base, uint32_t system_id) {
    Coord c{};
    *(uintptr_t*)c.raw = base + sdk::vt::CCelestialCoordinate;
    *(uint32_t*)(c.raw + sdk::ent::CCelestialCoordinate::origin) = system_id;
    return c;
}

// CFleetPath as DrawMovementDebugLines builds it on the stack: vtable, CPdxArray<SNode> (vtable at
// +8, nodes / count per sdk::rt), CGameDate; Create fills the nodes, CalcEstimatedDays reads them
struct PathCtx {
    uintptr_t base;
    void* fleet;
    Coord from, to;
    alignas(16) uint8_t path[0x40];
    void* nodes;
    int32_t count;
    int64_t* per_node;
    int64_t days;
};

void CallFleetCoordinate(void* c, void*) {
    auto* x = (PathCtx*)c;
    // the fleet's position: a virtual on its coordinate interface (secondary base)
    auto iface = (uintptr_t)x->fleet + sdk::rt::CFleet_coordinate_base;
    auto fn = (*(const void* (***)(uintptr_t))iface)[sdk::vt::CFleet_GetCoordinate];
    x->nodes = (void*)fn(iface);
}

const void* FleetCoordinate(uintptr_t base, void* fleet) {
    PathCtx ctx{};
    ctx.base = base;
    ctx.fleet = fleet;
    if (!CommandBuilder::Get().CallGuarded(&CallFleetCoordinate, &ctx)) return nullptr;
    return ctx.nodes;
}

void CallCreatePath(void* c, void*) {
    auto* x = (PathCtx*)c;
    // CFleet::CalcMovementPathFindSettings, as the engine inlines it before Create
    bool flag = ((bool (*)(const void*))(x->base + sdk::fn::CFleet_PathFindSettingsFlag))(x->fleet);
    const void* avoid = *(const void* const*)(x->base + sdk::glob::TPdxNullObject_CGalacticObject_pInstance);
    ((void (*)(void*, const void*, const void*, const void*, const void*, uint32_t))(x->base + sdk::fn::CFleetPath_Create))(
        x->path, x->from.raw, x->to.raw, avoid, x->fleet, flag ? 3u : 2u);
}

bool EnginePath(PathCtx* ctx) {
    memset(ctx->path, 0, sizeof(ctx->path));
    *(uintptr_t*)ctx->path = ctx->base + sdk::vt::CFleetPath;
    *(uintptr_t*)(ctx->path + 8) = ctx->base + sdk::vt::CPdxArray_CFleetPath_SNode;
    if (!CommandBuilder::Get().CallGuarded(&CallCreatePath, ctx)) return false;
    ctx->nodes = *(void**)(ctx->path + sdk::rt::CFleetPath_nodes);
    ctx->count = *(int32_t*)(ctx->path + sdk::rt::CFleetPath_node_count);
    if (!ctx->nodes || ctx->count < 0 || ctx->count > 100000) ctx->count = 0;
    return true;
}

void CallEstimatedDays(void* c, void*) {
    auto* x = (PathCtx*)c;
    int64_t out = 0;
    ((int64_t * (*)(const void*, int64_t*, const void*, int64_t*))(x->base + sdk::fn::CFleetPath_CalcEstimatedDays))(
        x->path, &out, x->fleet, x->per_node);
    x->days = out;
}

// total and per-node days (CFixedPoint); per_node has one slot per node
bool EngineDays(PathCtx* ctx, int64_t* per_node, int64_t* total) {
    ctx->per_node = per_node;
    if (!CommandBuilder::Get().CallGuarded(&CallEstimatedDays, ctx)) return false;
    *total = ctx->days;
    return true;
}

void CallFreeNodes(void* c, void*) {
    auto* x = (PathCtx*)c;
    ((void (*)(void*))(x->base + sdk::fn::CRT_operator_delete))(x->nodes);
}

// the path's destructor: the nodes are plain data, the engine just frees the array
void FreePath(PathCtx* ctx) {
    ctx->nodes = *(void**)(ctx->path + sdk::rt::CFleetPath_nodes);
    if (ctx->nodes) CommandBuilder::Get().CallGuarded(&CallFreeNodes, ctx);
    *(void**)(ctx->path + sdk::rt::CFleetPath_nodes) = nullptr;
}

struct CanColonizeCtx {
    uintptr_t fn;
    const void* planet;
    const void* country;
    bool result;
};

void CallCanColonize(void* c, void* out) {
    auto* x = (CanColonizeCtx*)c;
    x->result = ((bool (*)(const void*, const void*, void*))x->fn)(x->planet, x->country, out);
}

// CPlanet::CanColonize, the check CFleetColonizePlanetCommand::IsValid makes (with no reason);
// `why` gets the game's reason when it says no
bool CanColonize(uintptr_t base, const void* planet, const void* country, std::string* why) {
    CanColonizeCtx ctx{ base + sdk::fn::CPlanet_CanColonize, planet, country, false };
    std::string text;
    if (!CommandBuilder::Get().CallForText(&CallCanColonize, &ctx, &text)) return false;
    if (!ctx.result && why) *why = text;
    return ctx.result;
}

// CPdxArray<TPdxRef<T>> embedded in a command {vtable (factory's), data +8, capacity +0x10,
// size +0x14}; the command frees the data, so it comes from the engine heap
bool SetRefArray(NativeCommand& cmd, std::ptrdiff_t arr, const std::vector<uint32_t>& ids, std::string* why) {
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    void* data = CommandBuilder::Get().EngineAlloc(ids.size() * sizeof(uint32_t));
    if (!data) {
        if (why) *why = "engine allocation for the fleet list failed";
        return false;
    }
    memcpy(data, ids.data(), ids.size() * sizeof(uint32_t));
    cmd.Set<void*>(arr + kArrData, data)
        .Set<uint32_t>(arr + kArrCapacity, (uint32_t)ids.size())
        .Set<uint32_t>(arr + kArrSize, (uint32_t)ids.size());
    return true;
}

struct ClaimCtx {
    uintptr_t fn;
    const void* system;
    const void* country;
    alignas(8) uint8_t out[0x18];  // CClaim {vtable, owner +8, date +0xC, claims +0x10}
};

void CallGetClaims(void* c, void*) {
    auto* x = (ClaimCtx*)c;
    ((void* (*)(const void*, void*, const void*))x->fn)(x->system, x->out, x->country);
}

// CGalacticObject::GetClaimsBy(country).claims; 0 when none (or the call failed)
int ClaimsBy(uintptr_t base, const void* system, const void* country) {
    ClaimCtx ctx{ base + sdk::fn::CGalacticObject_GetClaimsBy, system, country, {} };
    if (!CommandBuilder::Get().CallGuarded(&CallGetClaims, &ctx)) return 0;
    return *(const int32_t*)(ctx.out + sdk::ent::CClaim::claims);
}

double Fixed(uintptr_t addr) { return std::round(ReadOr<int64_t>(addr, 0) / kFixed * 100.0) / 100.0; }

const char* kIntelNames[] = { "none", "low", "medium", "high", "full" };
// EPathJumpMethod (save tokens jump_hyperlane, jump_bypass): a hyperlane, or a gateway / wormhole / L-gate
const char* kJumpMethods[] = { "hyperlane", "bypass" };
// EFleetStance (save tokens passive, aggressive, evasive; names FLEET_STANCE_*)
const char* kStances[] = { "passive", "aggressive", "evasive" };
const char* kStanceNames[] = { "FLEET_STANCE_PASSIVE", "FLEET_STANCE_AGGRESSIVE", "FLEET_STANCE_EVASIVE" };
// EMiaType: mia_emergency_ftl 0, mia_return_home 1 (the ones a player orders)
constexpr uint32_t kMiaEmergencyFtl = 0;
constexpr uint32_t kMiaReturnHome = 1;

}  // namespace

GalaxyManager& GalaxyManager::Get() {
    static GalaxyManager instance;
    return instance;
}

bool GalaxyManager::Init(uintptr_t base_address) {
    base_address_ = base_address;
    LOGF("[GALAXY] Initialized (Base: 0x%llX)", (unsigned long long)base_address_);
    return true;
}

GalaxyManager::Snapshot GalaxyManager::Take() {
    Snapshot s;
    s.player = GameState::Get().GetPlayerCountry();
    if (s.player) s.player_id = ReadOr<uint32_t>((uintptr_t)s.player + sdk::rt::CCountry_id, kInvalidId);

    namespace go = sdk::ent::CGalacticObject;
    namespace cc = sdk::ent::CCelestialCoordinate;
    namespace hl = sdk::ent::CHyperlane;
    ForEachRef(base_address_, sdk::db::CGalacticObject, [&](uint32_t, void* obj) {
        System sys{};
        sys.id = ReadOr<uint32_t>((uintptr_t)obj + sdk::rt::CGalacticObject_id, kInvalidId);
        if (sys.id == kInvalidId) return;
        sys.obj = obj;
        sys.x = Fixed((uintptr_t)obj + go::coordinate + cc::x);
        sys.y = Fixed((uintptr_t)obj + go::coordinate + cc::y);
        void* lanes = nullptr;
        int32_t n = 0;
        if (Read((const void*)((uintptr_t)obj + go::hyperlane), &lanes) && lanes &&
            Read((const void*)((uintptr_t)obj + go::hyperlane + kPlainSize), &n) && n > 0 && n < 64) {
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t e = (uintptr_t)lanes + i * kHyperlaneSize;
                uint32_t to = ReadOr<uint32_t>(e + hl::to, kInvalidId);
                if (to != kInvalidId) sys.lanes.push_back({ to, Fixed(e + hl::length) });
            }
        }
        s.index[sys.id] = s.systems.size();
        s.systems.push_back(std::move(sys));
    });

    // planets by system (the planet's coordinate origin)
    ForEachRef(base_address_, sdk::db::CPlanet, [&](uint32_t, void* obj) {
        uint32_t pid = ReadOr<uint32_t>((uintptr_t)obj + sdk::rt::CPlanet_id, kInvalidId);
        uint32_t sid = ReadOr<uint32_t>((uintptr_t)obj + sdk::ent::CPlanet::coordinate + cc::origin, kInvalidId);
        if (pid != kInvalidId && sid != kInvalidId) s.planets[sid].push_back({ pid, obj });
    });
    return s;
}

int GalaxyManager::Intel(const Snapshot& s, void* system) {
    uint8_t level = 0;
    if (!s.player || !EngineByte(base_address_ + sdk::fn::CCountry_GetIntelLevel, s.player, system, &level)) return 0;
    return level > 4 ? 4 : level;
}

bool GalaxyManager::PlanetSurveyed(const Snapshot& s, void* system, void* planet) {
    uint8_t v = 0;
    if (!s.player) return false;
    if (EngineByte(base_address_ + sdk::fn::CCountry_HasAutoSurveyedSystem, s.player, system, &v) && v) return true;
    return EngineByte(base_address_ + sdk::fn::CCountry_HasSurveyedDepositHolder, s.player,
                      (const void*)((uintptr_t)planet + kPlanetDepositHolder), &v) && v;
}

bool GalaxyManager::SystemSurveyed(const Snapshot& s, uint32_t system_id, void* system) {
    auto it = s.planets.find(system_id);
    if (it == s.planets.end()) return true;
    for (const auto& p : it->second) {
        if (!PlanetSurveyed(s, system, p.obj)) return false;
    }
    return true;
}

uint32_t GalaxyManager::Owner(void* system) {
    return ReadOr<uint32_t>((uintptr_t)system + sdk::rt::CGalacticObject_owner, kInvalidId);
}

std::string GalaxyManager::SystemName(uint32_t id, void* system) {
    void* db = nullptr;
    Read((const void*)(base_address_ + sdk::db::CGalacticObject), &db);
    if (db != names_db_) {  // another save: drop the cache
        names_.clear();
        names_db_ = db;
    }
    auto it = names_.find(id);
    if (it != names_.end()) return it->second;
    std::string name = PersistentNameText((const void*)((uintptr_t)system + sdk::ent::CGalacticObject::name));
    names_[id] = name;
    return name;
}

uint32_t GalaxyManager::CapitalSystem(const Snapshot& s) {
    if (!s.player) return kInvalidId;
    uint32_t colony_id = ReadOr<uint32_t>((uintptr_t)s.player + sdk::ent::CCountry::capital, kInvalidId);
    void* colony = RefLookup(base_address_, sdk::db::CColony, colony_id);
    if (!colony) return kInvalidId;
    uint32_t planet_id = ReadOr<uint32_t>((uintptr_t)colony + sdk::ent::CColony::carrier + kCarrierId, kInvalidId);
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return kInvalidId;
    return ReadOr<uint32_t>((uintptr_t)planet + sdk::ent::CPlanet::coordinate + sdk::ent::CCelestialCoordinate::origin,
                            kInvalidId);
}

std::vector<uint32_t> GalaxyManager::StarbaseIds(void* system) {
    return RefArray((uintptr_t)system + sdk::ent::CGalacticObject::starbases);
}

std::vector<uint32_t> GalaxyManager::FleetIds(void* system) {
    return RefArray((uintptr_t)system + sdk::ent::CGalacticObject::fleet_presence);
}

// CStarbaseLevelType is script data (no serializer, so no SDK layout): its ship_size is the one
// pointer field that, in every starbase level type, points at an entry of the ship size database.
std::ptrdiff_t GalaxyManager::LevelShipSizeField() {
    if (level_ship_size_ != -2) return level_ship_size_;
    auto items = [&](uintptr_t glob, std::vector<void*>& out) {
        void* db = nullptr;
        void* arr = nullptr;
        int32_t n = 0;
        if (!Read((const void*)(base_address_ + glob), &db) || !db || !Read((const void*)((uintptr_t)db + 0x50), &arr) ||
            !arr || !Read((const void*)((uintptr_t)db + 0x5C), &n) || n <= 0 || n > 5000) {
            return;
        }
        for (int32_t i = 0; i < n; ++i) {
            void* p = nullptr;
            if (Read((const void*)((uintptr_t)arr + i * 8), &p) && p) out.push_back(p);
        }
    };
    std::vector<void*> levels, sizes;
    items(sdk::glob::TGameDatabase_CStarbaseLevelTypeDatabase_pInstance, levels);
    items(sdk::glob::TGameDatabase_CShipSizeDatabase_pInstance, sizes);
    std::unordered_set<void*> size_set(sizes.begin(), sizes.end());
    std::map<std::ptrdiff_t, size_t> hits;
    for (void* lv : levels) {
        for (std::ptrdiff_t off = 0x28; off < 0x800; off += 8) {
            void* p = nullptr;
            if (Read((const void*)((uintptr_t)lv + off), &p) && size_set.count(p)) hits[off]++;
        }
    }
    level_ship_size_ = -1;
    for (const auto& [off, n] : hits) {
        if (!levels.empty() && n == levels.size()) {
            level_ship_size_ = level_ship_size_ == -1 ? off : -1;  // must be unique
            if (level_ship_size_ == -1) break;
        }
    }
    LOGF("[GALAXY] starbase level ship_size field: %lld (%zu levels)", (long long)level_ship_size_, levels.size());
    return level_ship_size_;
}

std::string GalaxyManager::CountryName(uint32_t country_id) {
    return country_id == kInvalidId ? "" : OutlinerManager::Get().CountryDisplayName(country_id);
}

// 玩家自己的国家：id + 名字。名字走 CountryName（与遗址/星系的 owner_name 同一条路），
// 传的是 Snapshot 里的 player_id（带标记的引用），不是裸 id。
nlohmann::json GalaxyManager::GetPlayerCountryJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    return { {"success", true},
             {"country_id", s.player_id & 0xFFFFFF},
             {"raw_ref", s.player_id},
             {"name", CountryName(s.player_id)} };
}

nlohmann::json GalaxyManager::GetOverviewJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };

    int intel_counts[5] = { 0, 0, 0, 0, 0 };
    int surveyed = 0, owned = 0, lanes = 0, frontier = 0;
    std::map<uint32_t, int> empires;
    std::unordered_set<uint32_t> player_systems;
    for (const auto& sys : s.systems) {
        int intel = Intel(s, sys.obj);
        intel_counts[intel]++;
        lanes += (int)sys.lanes.size();
        uint32_t owner = Owner(sys.obj);
        if (owner == s.player_id) {
            owned++;
            player_systems.insert(sys.id);
        } else if (owner != kInvalidId && intel >= 1) {
            empires[owner]++;
        }
        if (SystemSurveyed(s, sys.id, sys.obj)) surveyed++;
    }
    for (const auto& sys : s.systems) {
        if (Owner(sys.obj) != kInvalidId) continue;
        for (const auto& l : sys.lanes) {
            if (player_systems.count(l.first)) {
                frontier++;
                break;
            }
        }
    }
    std::vector<std::pair<uint32_t, int>> known(empires.begin(), empires.end());
    std::sort(known.begin(), known.end(), [](auto& a, auto& b) { return a.second > b.second; });
    nlohmann::json empire_arr = nlohmann::json::array();
    for (const auto& [id, n] : known) {
        empire_arr.push_back({ {"country_id", id}, {"name", CountryName(id)}, {"known_systems", n} });
    }
    uint32_t capital = CapitalSystem(s);
    auto cap_it = s.index.find(capital);
    nlohmann::json intel = nlohmann::json::object();
    for (int i = 0; i < 5; ++i) intel[kIntelNames[i]] = intel_counts[i];

    return {
        {"systems_total", s.systems.size()},
        {"hyperlanes_total", lanes / 2},
        // changes only when systems or hyperlanes do: a cached get_galaxy_map topology stays valid
        {"topology_version", std::to_string(s.systems.size()) + "-" + std::to_string(lanes)},
        {"player", {
            {"country_id", s.player_id},
            {"capital_system_id", capital},
            {"capital_system", cap_it != s.index.end() ? SystemName(capital, s.systems[cap_it->second].obj) : ""},
            {"owned_systems", owned}
        }},
        {"intel", intel},
        {"surveyed_systems", surveyed},
        {"unsurveyed_systems", (int)s.systems.size() - surveyed},
        {"unclaimed_frontier_systems", frontier},
        {"known_empires", empire_arr},
        {"unresearched_anomalies", SituationLogManager::Get().ReadAnomalies(s.player).size()}
    };
}

nlohmann::json GalaxyManager::GetMapJson(uint32_t center, int jumps) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (center == kInvalidId) center = CapitalSystem(s);
    if (!s.index.count(center)) return { {"error", "Unknown system id: " + std::to_string(center)} };
    // 2026-11-22：jumps < 0 = 全银河（原来 clamp 到 kMaxJumps=12，只能看邻域）。
    // kHardJumpCap 只是防呆上限，实际星系数远小于它。
    constexpr int kHardJumpCap = 1 << 20;
    const bool whole_galaxy = jumps < 0;
    jumps = whole_galaxy ? kHardJumpCap : std::clamp(jumps, 0, kHardJumpCap);

    // breadth-first over hyperlanes
    std::unordered_map<uint32_t, int> dist{ {center, 0} };
    std::deque<uint32_t> q{ center };
    std::vector<uint32_t> order;
    while (!q.empty()) {
        uint32_t id = q.front();
        q.pop_front();
        order.push_back(id);
        int d = dist[id];
        if (d >= jumps) continue;
        for (const auto& l : s.systems[s.index[id]].lanes) {
            if (s.index.count(l.first) && !dist.count(l.first)) {
                dist[l.first] = d + 1;
                q.push_back(l.first);
            }
        }
    }

    std::unordered_set<uint32_t> anomaly_planets;
    for (const auto& a : SituationLogManager::Get().ReadAnomalies(s.player)) anomaly_planets.insert(a.planet_id);
    std::unordered_set<uint32_t> own_fleets;
    for (uint32_t f : fleets::Owned(s.player)) own_fleets.insert(f);

    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json owners = nlohmann::json::object();
    for (uint32_t id : order) {
        const System& sys = s.systems[s.index[id]];
        int intel = Intel(s, sys.obj);
        uint32_t owner = Owner(sys.obj);
        bool owner_known = owner != kInvalidId && (owner == s.player_id || intel >= 1);
        std::string flags;
        if (SystemSurveyed(s, id, sys.obj)) flags += 'S';
        auto pit = s.planets.find(id);
        bool colony = false, anomaly = false;
        if (pit != s.planets.end()) {
            for (const auto& p : pit->second) {
                uint32_t col = ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::colony, kInvalidId);
                uint32_t pown = ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::owner, kInvalidId);
                if (col != kInvalidId && (intel >= 2 || pown == s.player_id)) colony = true;
                if (anomaly_planets.count(p.id)) anomaly = true;
            }
        }
        if (colony) flags += 'C';
        bool starbase = false;
        for (uint32_t sb : StarbaseIds(sys.obj)) starbase = starbase || RefLookup(base_address_, sdk::db::CStarbase, sb);
        if (starbase && (intel >= 1 || owner == s.player_id)) flags += 'B';
        for (uint32_t f : FleetIds(sys.obj)) {
            if (own_fleets.count(f)) {
                flags += 'F';
                break;
            }
        }
        if (anomaly) flags += 'A';
        if (ClaimsBy(base_address_, sys.obj, s.player) > 0) flags += 'K';
        if (intel >= 2 || owner == s.player_id) {
            if (!RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::megastructures).empty()) flags += 'M';
            if (!RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::bypasses).empty()) flags += 'G';
        }
        if (owner_known && !owners.contains(std::to_string(owner))) owners[std::to_string(owner)] = CountryName(owner);
        rows.push_back({ id, SystemName(id, sys.obj), sys.x, sys.y,
                         owner_known ? nlohmann::json(owner) : nlohmann::json(nullptr), intel, dist[id], flags });
    }
    nlohmann::json lanes = nlohmann::json::array();
    for (uint32_t id : order) {
        for (const auto& l : s.systems[s.index[id]].lanes) {
            if (id < l.first && dist.count(l.first)) lanes.push_back({ id, l.first, l.second });
        }
    }
    return {
        {"center_system_id", center},
        {"jumps", whole_galaxy ? -1 : jumps},
        {"columns", {"id", "name", "x", "y", "owner_id", "intel", "jumps", "flags"}},
        {"rows", rows},
        {"hyperlanes", lanes},  // [a, b, length]
        {"owners", owners},
        {"legend", {
            {"intel", "0 none, 1 low, 2 medium, 3 high, 4 full"},
            {"flags", "S surveyed, C colonized, B starbase, F own fleet present, A unresearched anomaly, "
                      "K claimed by the player, M megastructure, G gateway / wormhole / relay (bypass)"},
            {"survey", "S means every planet is surveyed by the player (CCountry::HasFullySurveyedSystem); "
                       "planets inside another empire's borders cannot be surveyed without access"},
            {"owner_id", "null: unowned or not known to the player"}
        }}
    };
}

nlohmann::json GalaxyManager::GetSystemJson(uint32_t system_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    auto it = s.index.find(system_id);
    if (it == s.index.end()) return { {"error", "Unknown system id: " + std::to_string(system_id)} };
    const System& sys = s.systems[it->second];
    int intel = Intel(s, sys.obj);
    uint32_t owner = Owner(sys.obj);
    bool owner_known = owner != kInvalidId && (owner == s.player_id || intel >= 1);

    nlohmann::json lanes = nlohmann::json::array();
    for (const auto& l : sys.lanes) {
        auto lt = s.index.find(l.first);
        lanes.push_back({ {"to", l.first},
                          {"name", lt != s.index.end() ? SystemName(l.first, s.systems[lt->second].obj) : ""},
                          {"length", l.second} });
    }

    // starbase (visible with the owner)
    nlohmann::json starbase = nullptr;
    if (owner_known) {
        for (uint32_t sb_id : StarbaseIds(sys.obj)) {
            void* sb = RefLookup(base_address_, sdk::db::CStarbase, sb_id);
            if (!sb) continue;
            std::string level = KeyOf((uintptr_t)sb + sdk::ent::CStarbase::level, kTypeKey);
            // the level's display name is its ship size's (starbase_level_outpost -> starbase_outpost)
            std::string size_key;
            void* level_type = nullptr;
            std::ptrdiff_t ship_size = LevelShipSizeField();
            if (ship_size >= 0 && Read((const void*)((uintptr_t)sb + sdk::ent::CStarbase::level), &level_type) && level_type) {
                size_key = KeyOf((uintptr_t)level_type + ship_size, kTypeKey);
            }
            starbase = { {"id", sb_id}, {"level", level},
                         {"level_name", size_key.empty() ? "" : SafeLocalize(base_address_, size_key)} };
            break;
        }
    }

    // planets: intel >= 2, a survey, or our own
    std::unordered_map<uint32_t, std::string> anomalies;
    for (const auto& a : SituationLogManager::Get().ReadAnomalies(s.player)) anomalies[a.planet_id] = a.key;
    std::unordered_map<uint32_t, std::vector<std::string>> deposits;  // planet id -> deposit keys
    ForEachRef(base_address_, sdk::db::CDeposit, [&](uint32_t, void* d) {
        uintptr_t holder = (uintptr_t)d + sdk::ent::CDeposit::deposit_holder;
        if (ReadOr<uint32_t>(holder + kMetaRefType, kHolderNone) != kHolderPlanet) return;
        uint32_t pid = ReadOr<uint32_t>(holder + kMetaRefId, kInvalidId);
        std::string key = KeyOf((uintptr_t)d + sdk::ent::CDeposit::type, kTypeKey);
        if (pid != kInvalidId && !key.empty()) deposits[pid].push_back(key);
    });
    nlohmann::json planets = nlohmann::json::array();
    bool all_surveyed = true;
    int hidden = 0;
    auto pit = s.planets.find(system_id);
    if (pit != s.planets.end()) {
        for (const auto& p : pit->second) {
            bool surveyed = PlanetSurveyed(s, sys.obj, p.obj);
            all_surveyed = all_surveyed && surveyed;
            uint32_t pown = ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::owner, kInvalidId);
            if (intel < 2 && !surveyed && pown != s.player_id) {
                hidden++;
                continue;
            }
            std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
            nlohmann::json pj = {
                {"id", p.id},
                {"name", PersistentNameText((const void*)((uintptr_t)p.obj + sdk::ent::CPlanet::name))},
                {"class", cls},
                {"class_name", cls.empty() ? "" : SafeLocalize(base_address_, cls)},
                {"size", ReadOr<int32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::planet_size, 0)},
                {"surveyed", surveyed}
            };
            uint32_t col = ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::colony, kInvalidId);
            if (pown != kInvalidId) pj["owner_id"] = pown;
            if (col != kInvalidId) pj["colony_id"] = col;
            if (surveyed) {
                nlohmann::json dj = nlohmann::json::array();
                for (const auto& k : deposits[p.id]) dj.push_back({ {"key", k}, {"name", SafeLocalize(base_address_, k)} });
                pj["deposits"] = dj;
            }
            auto an = anomalies.find(p.id);
            if (an != anomalies.end()) pj["anomaly"] = { {"key", an->second}, {"name", SafeLocalize(base_address_, an->second)} };
            planets.push_back(pj);
        }
    }

    // fleets: our own always, others from intel >= 3
    std::unordered_set<uint32_t> own_fleets;
    for (uint32_t f : fleets::Owned(s.player)) own_fleets.insert(f);
    nlohmann::json fleet_arr = nlohmann::json::array();
    for (uint32_t fid : FleetIds(sys.obj)) {
        bool own = own_fleets.count(fid) > 0;
        if (!own && intel < 3) continue;
        void* f = fleets::Find(base_address_, fid);
        if (!f) continue;
        nlohmann::json fj = { {"id", fid}, {"name", fleets::Name(f)}, {"own", own},
                              {"ship_class", fleets::ShipClassKey(fleets::ClassOf(f))},
                              {"military_power", fleets::MilitaryPower(f)} };
        if (own) {
            uint32_t st = ReadOr<uint32_t>((uintptr_t)f + sdk::ent::CFleet::fleet_stance, 0);
            if (st < std::size(kStances)) fj["stance"] = kStances[st];
        }
        fleet_arr.push_back(fj);
    }

    // other objects in the system: megastructures, bypasses (gateways, wormholes, relays, L-gates),
    // astral rifts and debris from intel >= 2 or in our own systems; archaeological sites when
    // the site lists the player in visible_to
    bool see_objects = intel >= 2 || owner == s.player_id;
    namespace go = sdk::ent::CGalacticObject;
    nlohmann::json megas = nlohmann::json::array();
    nlohmann::json bypasses = nlohmann::json::array();
    nlohmann::json rifts = nlohmann::json::array();
    nlohmann::json debris = nlohmann::json::array();
    if (see_objects) {
        namespace ms = sdk::ent::CMegaStructure;
        for (uint32_t id : RefArray((uintptr_t)sys.obj + go::megastructures)) {
            void* m = RefLookup(base_address_, sdk::db::CMegaStructure, id);
            if (!m) continue;
            std::string type = KeyOf((uintptr_t)m + ms::type, kTypeKey);
            nlohmann::json mj = { {"id", id}, {"type", type}, {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)} };
            std::string mname = PersistentNameText((const void*)((uintptr_t)m + ms::name));
            if (!mname.empty()) mj["name"] = mname;  // relays and gates carry no name of their own
            uint32_t mown = ReadOr<uint32_t>((uintptr_t)m + ms::owner, kInvalidId);
            if (mown != kInvalidId) mj["owner_id"] = mown;
            uint32_t mplanet = ReadOr<uint32_t>((uintptr_t)m + ms::planet, kInvalidId);
            if (mplanet != kInvalidId) mj["planet_id"] = mplanet;
            if (ReadOr<uint8_t>((uintptr_t)m + ms::is_dismantling, 0)) mj["dismantling"] = true;
            megas.push_back(mj);
        }

        // where each bypass is: every system's bypass list
        std::unordered_map<uint32_t, uint32_t> bypass_system;
        for (const auto& other : s.systems) {
            for (uint32_t b : RefArray((uintptr_t)other.obj + go::bypasses)) bypass_system[b] = other.id;
        }
        namespace bp = sdk::ent::CBypass;
        for (uint32_t id : RefArray((uintptr_t)sys.obj + go::bypasses)) {
            void* b = RefLookup(base_address_, sdk::db::CBypass, id);
            if (!b) continue;
            std::string type = KeyOf((uintptr_t)b + bp::type, kBypassTypeKey);
            std::string type_name = type.empty() ? "" : SafeLocalize(base_address_, type);
            // the systems it leads to: its linked bypass plus its active network connections
            std::vector<uint32_t> to;
            auto add_to = [&](uint32_t other_bypass) {
                auto it2 = bypass_system.find(other_bypass);
                if (it2 != bypass_system.end() && it2->second != system_id &&
                    std::find(to.begin(), to.end(), it2->second) == to.end()) {
                    to.push_back(it2->second);
                }
            };
            add_to(ReadOr<uint32_t>((uintptr_t)b + bp::linked_to, kInvalidId));
            for (uint32_t c : RefArray((uintptr_t)b + bp::active_connections)) add_to(c);
            nlohmann::json leads = nlohmann::json::array();
            for (size_t i = 0; i < to.size() && i < 12; ++i) {
                auto ti = s.index.find(to[i]);
                leads.push_back({ {"id", to[i]}, {"name", ti != s.index.end() ? SystemName(to[i], s.systems[ti->second].obj) : ""} });
            }
            nlohmann::json bj = { {"id", id}, {"type", type}, {"active", ReadOr<uint8_t>((uintptr_t)b + bp::active, 0) != 0},
                                  {"leads_to", leads} };
            if (!type_name.empty() && type_name != type) bj["type_name"] = type_name;
            if (to.size() > 12) bj["leads_to_total"] = to.size();
            uint32_t lock = ReadOr<uint32_t>((uintptr_t)b + bp::lock_country, kInvalidId);
            if (lock != kInvalidId) bj["locked_by"] = lock;
            bypasses.push_back(bj);
        }

        namespace ar = sdk::ent::CAstralRift;
        for (uint32_t id : RefArray((uintptr_t)sys.obj + go::astral_rifts)) {
            void* r = RefLookup(base_address_, sdk::db::CAstralRift, id);
            if (!r) continue;
            std::string type = KeyOf((uintptr_t)r + ar::type, kTypeKey);
            nlohmann::json rj = { {"id", id}, {"type", type}, {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)},
                                  {"name", PersistentNameText((const void*)((uintptr_t)r + ar::name))},
                                  {"clues", ReadOr<int32_t>((uintptr_t)r + ar::clues, 0)},
                                  {"difficulty", ReadOr<int32_t>((uintptr_t)r + ar::difficulty, 0)} };
            uint32_t ex = ReadOr<uint32_t>((uintptr_t)r + ar::explorer_fleet, kInvalidId);
            if (ex != kInvalidId) rj["explorer_fleet_id"] = ex;
            auto explorable = RefArray((uintptr_t)r + ar::explorable_by);
            rj["explorable_by_player"] = std::find(explorable.begin(), explorable.end(), s.player_id) != explorable.end();
            rifts.push_back(rj);
        }

        namespace db_ = sdk::ent::CDebris;
        std::unordered_map<uint32_t, uint32_t> debris_project;
        for (const auto& pr : SituationLogManager::Get().ReadSpecialProjects(s.player)) {
            if (pr.debris_id != kInvalidId) debris_project[pr.debris_id] = pr.id;
        }
        ForEachRef(base_address_, sdk::db::CDebris, [&](uint32_t, void* d) {
            if (ReadOr<uint32_t>((uintptr_t)d + db_::coordinate + sdk::ent::CCelestialCoordinate::origin, kInvalidId) != system_id) return;
            if (ReadOr<uint8_t>((uintptr_t)d + db_::killed, 0)) return;
            uint32_t did = ReadOr<uint32_t>((uintptr_t)d + sdk::rt::CDebris_id, kInvalidId);
            nlohmann::json dj = { {"id", did} };
            auto pr = debris_project.find(did);
            if (pr != debris_project.end()) dj["project_id"] = pr->second;  // research it with collect_data
            uint32_t from = ReadOr<uint32_t>((uintptr_t)d + db_::from_country, kInvalidId);
            if (from != kInvalidId) dj["from_country_id"] = from;
            uint32_t cty = ReadOr<uint32_t>((uintptr_t)d + db_::country, kInvalidId);
            if (cty != kInvalidId) dj["country_id"] = cty;
            debris.push_back(dj);
        });
    }

    nlohmann::json sites = nlohmann::json::array();
    namespace as = sdk::ent::CArchaeologicalSite;
    ForEachRef(base_address_, sdk::db::CArchaeologicalSite, [&](uint32_t, void* a) {
        uintptr_t loc = (uintptr_t)a + as::location;
        uint32_t ltype = ReadOr<uint32_t>(loc + kMetaRefType, kInvalidId), lid = ReadOr<uint32_t>(loc + kMetaRefId, kInvalidId);
        if (SpatialSystem(ltype, lid) != system_id) return;
        auto visible = RefArray((uintptr_t)a + as::visible_to);
        if (std::find(visible.begin(), visible.end(), s.player_id) == visible.end()) return;
        std::string type = KeyOf((uintptr_t)a + as::type, kTypeKey);
        nlohmann::json sj = { {"id", ReadOr<uint32_t>((uintptr_t)a + sdk::rt::CArchaeologicalSite_id, kInvalidId)}, {"type", type},
                              {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)},
                              {"chapter", ReadOr<int32_t>((uintptr_t)a + as::index, 0)},
                              {"clues", ReadOr<int32_t>((uintptr_t)a + as::clues, 0)},
                              {"difficulty", ReadOr<int32_t>((uintptr_t)a + as::difficulty, 0)},
                              {"locked", ReadOr<uint8_t>((uintptr_t)a + as::locked, 0) != 0} };
        if (ltype == kSpatialPlanet) sj["planet_id"] = lid;
        uint32_t ex = ReadOr<uint32_t>((uintptr_t)a + as::excavator_fleet, kInvalidId);
        if (ex != kInvalidId) sj["excavator_fleet_id"] = ex;
        sites.push_back(sj);
    });

    nlohmann::json out = {
        {"id", system_id},
        {"name", SystemName(system_id, sys.obj)},
        {"x", sys.x},
        {"y", sys.y},
        {"intel", intel},
        {"intel_name", kIntelNames[intel]},
        {"owner_id", owner_known ? nlohmann::json(owner) : nlohmann::json(nullptr)},
        {"owner_name", owner_known ? CountryName(owner) : ""},
        {"surveyed", all_surveyed},
        {"hyperlanes", lanes},
        {"starbase", starbase},
        {"planets", planets},
        {"fleets", fleet_arr},
        {"player_claims", ClaimsBy(base_address_, sys.obj, s.player)}
    };
    if (!megas.empty()) out["megastructures"] = megas;
    if (!bypasses.empty()) out["bypasses"] = bypasses;
    if (!rifts.empty()) out["astral_rifts"] = rifts;
    if (!debris.empty()) out["debris"] = debris;
    if (!sites.empty()) out["archaeological_sites"] = sites;
    // the player's special projects researched here (debris analysis, located event projects)
    nlohmann::json projects = nlohmann::json::array();
    for (const auto& pr : SituationLogManager::Get().ReadSpecialProjects(s.player)) {
        if (pr.location_type == kInvalidId || SpatialSystem(pr.location_type, pr.location_id) != system_id) continue;
        nlohmann::json pj = { {"id", pr.id}, {"name", pr.name}, {"kind", pr.kind} };
        if (pr.days_left >= 0) pj["days_left"] = pr.days_left;
        projects.push_back(pj);
    }
    if (!projects.empty()) out["special_projects"] = projects;
    if (hidden) out["unknown_planets"] = hidden;  // present but not known to the player
    return out;
}

nlohmann::json GalaxyManager::GetArchaeologicalSitesJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };

    nlohmann::json out = nlohmann::json::array();
    namespace as = sdk::ent::CArchaeologicalSite;
    ForEachRef(base_address_, sdk::db::CArchaeologicalSite, [&](uint32_t, void* a) {
        uintptr_t loc = (uintptr_t)a + as::location;
        uint32_t ltype = ReadOr<uint32_t>(loc + kMetaRefType, kInvalidId);
        uint32_t lid = ReadOr<uint32_t>(loc + kMetaRefId, kInvalidId);
        std::string type = KeyOf((uintptr_t)a + as::type, kTypeKey);
        nlohmann::json sj = {
            {"id", ReadOr<uint32_t>((uintptr_t)a + sdk::rt::CArchaeologicalSite_id, kInvalidId)},
            {"type", type},
            {"name", type.empty() ? "" : SafeLocalize(base_address_, type)},
            {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)},
            {"chapter", ReadOr<int32_t>((uintptr_t)a + as::index, 0)},
            {"clues", ReadOr<int32_t>((uintptr_t)a + as::clues, 0)},
            {"difficulty", ReadOr<int32_t>((uintptr_t)a + as::difficulty, 0)},
            {"days_left", ReadOr<uint64_t>((uintptr_t)a + as::days_left, 0)},
            {"locked", ReadOr<uint8_t>((uintptr_t)a + as::locked, 0) != 0},
            {"location_type", ltype},
            {"location_id", lid}
        };
        if (ltype == kSpatialPlanet) sj["planet_id"] = lid;
        const uint32_t system_id_here = SpatialSystem(ltype, lid);
        if (system_id_here != kInvalidId) {
            sj["system_id"] = system_id_here;
            auto ixit = s.index.find(system_id_here);
            if (ixit != s.index.end()) {
                sj["system_name"] = SystemName(system_id_here, s.systems[ixit->second].obj);
                // 星系归属直接从星系对象读，不经 intel 过滤：判断「这个遗址在谁的地界上」
                // 靠它（未勘察的星系 get_system 会给 null）。
                const uint32_t sys_owner = Owner(s.systems[ixit->second].obj);
                if (sys_owner != kInvalidId) {
                    sj["owner_id"] = sys_owner;
                    sj["owner_name"] = CountryName(sys_owner);
                    sj["owner_is_player"] = (sys_owner == s.player_id);
                }
            }
        }
        auto visible = RefArray((uintptr_t)a + as::visible_to);
        sj["visible_to_player"] = std::find(visible.begin(), visible.end(), s.player_id) != visible.end();
        uint32_t ex = ReadOr<uint32_t>((uintptr_t)a + as::excavator_fleet, kInvalidId);
        if (ex != kInvalidId) sj["excavator_fleet_id"] = ex;
        out.push_back(sj);
    });

    return { {"success", true}, {"count", out.size()}, {"sites", out} };
}

// 只读：遗址的事件表（CArchaeologicalSite::events = +0xB8）。
// 每条 SSiteEventData：expired +0x8、event_id +0x10、scope +0x18、fleet +0x188、owner +0x18C、
// index +0x1F0、player_event +0x1F4。阶段弹窗就是这里面未过期的那一条。
nlohmann::json GalaxyManager::ProbeSiteEventsJson(uint32_t site_id) {
    void* site = RefLookup(base_address_, sdk::db::CArchaeologicalSite, site_id);
    if (!site) return { {"error", "site not found: " + std::to_string(site_id)} };

    namespace as = sdk::ent::CArchaeologicalSite;
    const uintptr_t arr = (uintptr_t)site + as::events;
    void* data = ReadOr<void*>(arr, nullptr);
    uint32_t cap = ReadOr<uint32_t>(arr + 8, 0);
    uint32_t size = ReadOr<uint32_t>(arr + 0xC, 0);

    nlohmann::json out = nlohmann::json::array();
    if (data && size > 0 && size <= 32) {
        for (uint32_t i = 0; i < size; ++i) {
            const uintptr_t e = (uintptr_t)data + (size_t)i * 0x1F8;
            out.push_back({
                {"i", i},
                {"expired", ReadOr<uint32_t>(e + 0x8, 0)},
                {"event_id", ReadOr<uint32_t>(e + 0x10, 0)},
                {"scope_raw", ReadOr<uint64_t>(e + 0x18, 0)},
                {"fleet_id", ReadOr<uint32_t>(e + 0x188, 0) & 0xFFFFFF},
                {"owner_id", ReadOr<uint32_t>(e + 0x18C, 0) & 0xFFFFFF},
                {"index", ReadOr<int32_t>(e + 0x1F0, 0)},
                {"player_event", ReadOr<int32_t>(e + 0x1F4, 0)}
            });
        }
    }
    return { {"success", true}, {"site_id", site_id}, {"ptr", (uint64_t)(uintptr_t)site},
             {"array_cap", cap}, {"array_size", size}, {"events", out} };
}

// 战争表：枚举 TPdxRef<CWar>（0x311BDE8）。
// 字段：id +0x20、name +0x28、attackers +0x80 / defenders +0x98（SWarParticipant 数组）、
//       end_date +0xCC、attacker/defender_war_goal +0xD0/+0xF0（内嵌 CWarGoal：type +0x8、actor +0x14、target +0x18、win +0x1C）、
//       双方战争疲惫 +0x118/+0x120、end +0x138。元素步长先按 0x10 估，逐条输出 count 供核对。
nlohmann::json GalaxyManager::GetWarsJson() {
    nlohmann::json out = nlohmann::json::array();
    ForEachRef(base_address_, sdk::db::CWar, [&](uint32_t db_key, void* w) {
        const uintptr_t a = (uintptr_t)w;
        nlohmann::json j = {
            {"id", ReadOr<uint32_t>(a + sdk::rt::CWar_id, kInvalidId)},
            {"db_key", db_key},
            {"name", ReadString(a + sdk::ent::CWar::name)},
            {"ended", ReadOr<uint8_t>(a + sdk::ent::CWar::end, 0) != 0},
            {"attacker_exhaustion", (int64_t)ReadOr<uint64_t>(a + sdk::ent::CWar::attacker_war_exhaustion, 0)},
            {"defender_exhaustion", (int64_t)ReadOr<uint64_t>(a + sdk::ent::CWar::defender_war_exhaustion, 0)},
        };
        auto side = [&](uintptr_t off) -> nlohmann::json {
            void* begin = nullptr;
            void* end = nullptr;
            Read((const void*)(a + off), &begin);
            Read((const void*)(a + off + 8), &end);
            const uintptr_t bytes = (begin && end && (uintptr_t)end > (uintptr_t)begin) ? ((uintptr_t)end - (uintptr_t)begin) : 0;
            return { {"bytes", (uint64_t)bytes}, {"per_0x10", (uint64_t)(bytes / 0x10)}, {"head", (unsigned long long)(uintptr_t)begin} };
        };
        j["attackers"] = side(sdk::ent::CWar::attackers);
        j["defenders"] = side(sdk::ent::CWar::defenders);
        auto goal = [&](uintptr_t off) -> nlohmann::json {
            const uintptr_t g = a + off;
            nlohmann::json gj = { {"type", KeyOf(g + sdk::ent::CWarGoal::type, kTypeKey)} };
            const uint32_t actor = ReadOr<uint32_t>(g + sdk::ent::CWarGoal::actor, kInvalidId);
            const uint32_t target = ReadOr<uint32_t>(g + sdk::ent::CWarGoal::target, kInvalidId);
            if (actor != kInvalidId) { gj["actor_id"] = actor & 0xFFFFFF; gj["actor_name"] = CountryName(actor); }
            if (target != kInvalidId) { gj["target_id"] = target & 0xFFFFFF; gj["target_name"] = CountryName(target); }
            gj["win"] = ReadOr<uint8_t>(g + sdk::ent::CWarGoal::win, 0) != 0;
            return gj;
        };
        j["attacker_war_goal"] = goal(sdk::ent::CWar::attacker_war_goal);
        j["defender_war_goal"] = goal(sdk::ent::CWar::defender_war_goal);
        out.push_back(j);
    });
    return { {"success", true}, {"count", out.size()}, {"wars", out} };
}

// 扫全银河里「星界细缝」类天体（planet_class = pc_astral_scar）。
// 玩家 2026-11-20 的判断：裂隙其实是「裂隙单位 + 裂隙天体」的复合体，真正占星系的是那个天体，
// 所以这里先找出所有 pc_astral_scar 的行星，数量应与裂隙数大致相等。
nlohmann::json GalaxyManager::ScanAstralScarJson() {
    Snapshot s = Take();
    nlohmann::json out = nlohmann::json::array();
    for (const auto& sys : s.systems) {
        auto it = s.planets.find(sys.id);
        if (it == s.planets.end()) continue;
        for (const auto& p : it->second) {
            const std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
            // 类名里带 astral / rift 的都算：玩家说星界细缝过一阵会「转化」成裂隙，
            // 那转化后星体类名很可能也换了（pc_astral_scar → 别的），所以别只抓一个键。
            if (cls.find("astral") == std::string::npos && cls.find("rift") == std::string::npos) continue;
            const uint32_t owner = Owner(sys.obj);
            nlohmann::json j = {
                {"planet_id", p.id},
                {"system_id", sys.id},
                {"system_name", SystemName(sys.id, sys.obj)},
                {"name", ReadString((uintptr_t)p.obj + sdk::ent::CPlanet::name)},
                {"class", cls},
            };
            if (owner != kInvalidId) {
                j["system_owner_id"] = owner & 0xFFFFFF;
                j["system_owner_name"] = CountryName(owner);
            }
            out.push_back(j);
        }
    }
    return { {"success", true}, {"count", out.size()}, {"planets", out} };
}

// 扫全银河：哪些星系的 astral_rifts（CGalacticObject+0x540）非空，并把头 32 字节原样吐出来，
// 用来定这个 vector 的元素布局（裂隙 → 星系 的反查一直零命中：大概是元素不是 4 字节 id）。
nlohmann::json GalaxyManager::ScanRiftSystemsJson() {
    Snapshot s = Take();
    nlohmann::json out = nlohmann::json::array();
    for (const auto& sys : s.systems) {
        const uintptr_t a = (uintptr_t)sys.obj + sdk::ent::CGalacticObject::astral_rifts;
        void* data = nullptr;
        void* end = nullptr;
        Read((const void*)a, &data);
        Read((const void*)(a + 8), &end);
        const uintptr_t bytes = (data && end && (uintptr_t)end > (uintptr_t)data) ? ((uintptr_t)end - (uintptr_t)data) : 0;
        if (bytes == 0 || bytes > 0x4000) continue;
        std::string hex;
        const uint32_t n = bytes < 32 ? (uint32_t)bytes : 32u;
        for (uint32_t i = 0; i < n && data; ++i) {
            uint8_t b = 0;
            if (!Read((const void*)((uintptr_t)data + i), &b)) break;
            static const char* kDigits = "0123456789ABCDEF";
            hex.push_back(kDigits[(b >> 4) & 0xF]);
            hex.push_back(kDigits[b & 0xF]);
            hex.push_back(' ');
        }
        out.push_back({ {"system_id", sys.id}, {"name", SystemName(sys.id, sys.obj)},
                        {"bytes", (uint64_t)bytes}, {"head", hex} });
    }
    return { {"success", true}, {"count", out.size()}, {"systems", out} };
}

// 只读探针：把一个星系对象（CGalacticObject）的一段内存按 qword 读出来。
// 用来核对 +0x540 那个 astral_rifts 数组到底怎么写（裂隙 → 星系 的反查一直零命中）。
nlohmann::json GalaxyManager::ProbeSystemJson(uint32_t system_id, uint32_t offset, uint32_t length) {
    Snapshot s = Take();
    auto it = s.index.find(system_id);
    if (it == s.index.end()) return { {"error", "system not found: " + std::to_string(system_id)} };
    void* obj = s.systems[it->second].obj;
    if (offset > 0x800) offset = 0;
    length = length == 0 ? 0x100u : length;
    if (length > 0x400) length = 0x400;
    nlohmann::json words = nlohmann::json::array();
    for (uint32_t o = offset; o < offset + length; o += 8) {
        const uint64_t v = ReadOr<uint64_t>((uintptr_t)obj + o, 0);
        words.push_back({ {"offset", o}, {"value", (int64_t)v}, {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
    }
    return { {"success", true}, {"system_id", system_id}, {"name", SystemName(system_id, obj)},
             {"ptr", (unsigned long long)(uintptr_t)obj}, {"words", words} };
}

// 只读探针：把一条 CAstralRift 的一段内存按 qword 读出来（找「所在星系」这类字段用）。
nlohmann::json GalaxyManager::ProbeAstralRiftJson(uint32_t rift_id, uint32_t offset, uint32_t length) {
    if (offset > 0x800) offset = 0;
    length = length == 0 ? 0x100u : length;
    if (length > 0x400) length = 0x400;
    nlohmann::json words = nlohmann::json::array();
    void* found = nullptr;
    ForEachRef(base_address_, sdk::db::CAstralRift, [&](uint32_t id, void* r) {
        if (found || id != rift_id) return;
        found = r;
    });
    if (!found) return { {"error", "Astral rift " + std::to_string(rift_id) + " not found"} };
    for (uint32_t o = offset; o < offset + length; o += 8) {
        const uint64_t v = ReadOr<uint64_t>((uintptr_t)found + o, 0);
        words.push_back({ {"offset", o}, {"value", (int64_t)v}, {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
    }
    return { {"success", true}, {"rift_id", rift_id}, {"ptr", (unsigned long long)(uintptr_t)found}, {"words", words} };
}

// 星界裂隙：枚举 TPdxRef<CAstralRift> 库（0x311BD80）。
// 字段：id +0x8、name +0x128（PdxString）、type +0x1F0（键在 +0x20）、owner +0x120、
//       explorer_fleet +0x180、leader +0x184、status +0x410、clues +0x414
nlohmann::json GalaxyManager::GetAstralRiftsJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };

    // 裂隙所在的星系：星系对象上有 astral_rifts 数组，裂隙本身没有反向指针 —— 反查一次。
    // 游牧的探索规则按「星系归谁」算（无主，或有研究条约的定居帝国），所以这一列必须给。
    std::unordered_map<uint32_t, uint32_t> rift_system;
    for (const auto& sys : s.systems) {
        // 数组里的值是带标记的引用（高 8 位有 type 位），和 CAstralRift_id 的裸 id 不是一回事
        for (uint32_t rid : RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::astral_rifts)) rift_system[rid & 0xFFFFFF] = sys.id;
    }

    nlohmann::json out = nlohmann::json::array();
    namespace ar = sdk::ent::CAstralRift;
    ForEachRef(base_address_, sdk::db::CAstralRift, [&](uint32_t, void* r) {
        const uintptr_t a = (uintptr_t)r;
        const uint32_t rift_id = ReadOr<uint32_t>(a + sdk::rt::CAstralRift_id, kInvalidId);
        nlohmann::json j = {
            {"id", rift_id},
            {"name", ReadString(a + ar::name)},
            {"type", KeyOf(a + ar::type, kTypeKey)},
            {"status", ReadOr<int32_t>(a + ar::status, 0)},
            {"clues", ReadOr<int32_t>(a + ar::clues, 0)}
        };
        const uint32_t owner = ReadOr<uint32_t>(a + ar::owner, kInvalidId);
        if (owner != kInvalidId) {
            j["owner_id"] = owner & 0xFFFFFF;
            // CountryDisplayName 会拿 country+0x20 跟传进来的值逐位比，所以必须传**带标记的引用**；
            // 之前这里先掩了 0xFFFFFF，于是 AI 国家的名字一律读成空串（2026-11-20 修）。
            j["owner_name"] = CountryName(owner);
        }
        // 星系：`CAstralRift + 0xA0` 的低 32 位**就是 origin 星系 id**
        // （2026-11-20 定：存档里 rift 的 `coordinate { x, y, origin }` 那一段，origin 与这里逐条一致——
        //   rift 1 存档 origin=542、内存 +0xA0 也是 542；rift 43 读出 513 = 富尔巴登，无主，正好是本档唯一可探索的）。
        // 旧的两次尝试都作废：+0x80 的 coordinate 是共享指针，星系的 astral_rifts(+0x540) 全银河为空。
        uint32_t coord_sys = ReadOr<uint32_t>(a + 0xA0, kInvalidId);
        if (coord_sys != kInvalidId && s.index.find(coord_sys) == s.index.end()) coord_sys = kInvalidId;
        uint32_t rift_sys = coord_sys;
        if (rift_sys == kInvalidId) {
            auto rs = rift_system.find(rift_id);
            if (rs != rift_system.end()) rift_sys = rs->second;
        }
        if (rift_sys != kInvalidId) {
            j["system_id"] = rift_sys;
            j["system_id_source"] = coord_sys != kInvalidId ? "rift_origin" : "system_array";
            auto ixit = s.index.find(rift_sys);
            if (ixit != s.index.end()) {
                j["system_name"] = SystemName(rift_sys, s.systems[ixit->second].obj);
                const uint32_t sys_owner = Owner(s.systems[ixit->second].obj);
                if (sys_owner != kInvalidId) {
                    j["system_owner_id"] = sys_owner;
                    j["system_owner_name"] = CountryName(sys_owner);
                    j["system_owner_is_player"] = (sys_owner == s.player_id);
                }
            }
        }
        const uint32_t fleet = ReadOr<uint32_t>(a + ar::explorer_fleet, kInvalidId);
        if (fleet != kInvalidId) j["explorer_fleet_id"] = fleet & 0xFFFFFF;
        const uint32_t lead = ReadOr<uint32_t>(a + ar::leader, kInvalidId);
        if (lead != kInvalidId) j["leader_id"] = lead & 0xFFFFFF;
        nlohmann::json ev = nlohmann::json::array();
        const uintptr_t arr = a + ar::events;
        void* data = ReadOr<void*>(arr, nullptr);
        const uint32_t cnt = ReadOr<uint32_t>(arr + 0xC, 0);
        if (data && cnt > 0 && cnt <= 32) {
            for (uint32_t i = 0; i < cnt; ++i) {
                // SAstralRiftScopedEvent：只取能确定的两项，其余留空
                ev.push_back({ {"i", i} });
            }
        }
        j["events"] = ev;
        out.push_back(j);
    });
    return { {"success", true}, {"count", out.size()}, {"rifts", out} };
}
nlohmann::json GalaxyManager::DisbandFleetsJson(const std::vector<uint32_t>& fleet_ids) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (fleet_ids.empty()) return { {"success", false}, {"error", "No fleet ids given"} };

    auto owned = fleets::Owned(s.player);
    for (uint32_t id : fleet_ids) {
        if (std::find(owned.begin(), owned.end(), id) == owned.end()) {
            return { {"success", false}, {"error", "Fleet " + std::to_string(id) + " is not one of the player's fleets"} };
        }
    }

    // CDisbandFleetsCommand: fleet + ref_array<TPdxRef<CFleet>>, country
    namespace db = sdk::cmd::disband_fleets_command;
    auto cmd = CommandBuilder::Get().Create(db::kSpec);
    if (!cmd) return { {"success", false}, {"error", cmd.error()} };

    std::string why;
    if (!SetRefArray(cmd, db::fleet, fleet_ids, &why)) {
        return { {"success", false}, {"error", why} };
    }
    cmd.Set<uint32_t>(db::country, s.player_id);

    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"is_valid", false},
                 {"error", why.empty() ? "The engine refused to disband these fleets" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    return { {"success", true}, {"fleets", fleet_ids}, {"message", "Disband command posted"} };
}

/* 降一级恒星基地：走原生 CDowngradeStarbaseCommand（token 0x3AE4，vtable 0x23D05E0）。
   载荷两个 4 字节 id：starbase@+0x20、actor@+0x24（都是 ref，用 id 填）。 */
nlohmann::json GalaxyManager::DowngradeStarbaseJson(uint32_t starbase_id) {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country (not in game?)"} };

    namespace dc = sdk::cmd::downgrade_starbase_command;
    auto cmd = CommandBuilder::Get().Create(dc::kSpec);
    if (!cmd) return { {"success", false}, {"error", cmd.error()} };

    cmd.Set<uint32_t>(dc::starbase, starbase_id);
    cmd.Set<uint32_t>(dc::actor, s.player_id);

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"is_valid", false},
                 {"error", why.empty() ? "The game refused to downgrade this starbase" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }
    return { {"success", true}, {"starbase_id", starbase_id}, {"message", "Downgrade command posted"} };
}

/* 拆毁恒星基地：原生 CDismantleStarbaseCommand（token 0x3A55，vtable 0x2595148）。
   载荷与降级同构：starbase@+0x20、actor@+0x24。游戏只允许拆**哨站**，其余等级由 IsValid 拒。 */
nlohmann::json GalaxyManager::DismantleStarbaseJson(uint32_t starbase_id) {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country (not in game?)"} };

    namespace dc = sdk::cmd::dismantle_starbase_command;
    auto cmd = CommandBuilder::Get().Create(dc::kSpec);
    if (!cmd) return { {"success", false}, {"error", cmd.error()} };

    cmd.Set<uint32_t>(dc::starbase, starbase_id);
    cmd.Set<uint32_t>(dc::actor, s.player_id);

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"is_valid", false},
                 {"error", why.empty() ? "The game refused to dismantle this starbase" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }
    return { {"success", true}, {"starbase_id", starbase_id}, {"message", "Dismantle command posted"} };
}

nlohmann::json GalaxyManager::MergeFleetsJson(const std::vector<uint32_t>& fleet_ids) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (fleet_ids.size() < 2) {
        return { {"success", false}, {"error", "Need at least two fleets"} };
    }

    auto owned = fleets::Owned(s.player);
    for (uint32_t id : fleet_ids) {
        if (std::find(owned.begin(), owned.end(), id) == owned.end()) {
            return { {"success", false}, {"error", "Fleet " + std::to_string(id) + " is not one of the player's fleets"} };
        }
    }

    // CMergeFleetsCommand: country + ref_array<TPdxRef<CFleet>> + two bools
    namespace mf = sdk::cmd::merge_fleets_command;
    auto cmd = CommandBuilder::Get().Create(mf::kSpec);
    if (!cmd) return { {"success", false}, {"error", cmd.error()} };

    cmd.Set<uint32_t>(mf::country, s.player_id);

    std::string why;
    if (!SetRefArray(cmd, mf::fleets, fleet_ids, &why)) {
        return { {"success", false}, {"error", why} };
    }
    cmd.Set<uint8_t>(mf::mia_reinforce, 0).Set<uint8_t>(mf::cancel_orders, 0);

    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"is_valid", false},
                 {"error", why.empty() ? "The engine refused to merge these fleets" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    return { {"success", true}, {"fleets", fleet_ids}, {"message", "Merge command posted"} };
}

nlohmann::json GalaxyManager::MoveFleet(uint32_t fleet_id, uint32_t system_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    auto owned = fleets::Owned(s.player);
    if (std::find(owned.begin(), owned.end(), fleet_id) == owned.end()) {
        return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    }
    if (!s.index.count(system_id)) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(system_id)} };

    // CSendFleetToLocationCommand: a CMoveToSystemPointFleetOrder to the system's centre
    // (a coordinate with origin = the system, local x = y = 0)
    namespace cmd_ns = sdk::cmd::fleet_send_to_location;
    namespace cc = sdk::ent::CCelestialCoordinate;
    auto cmd = CommandBuilder::Get().Create(cmd_ns::kSpec);
    cmd.Set<uint32_t>(cmd_ns::fleet, fleet_id)
        .Set<int64_t>(cmd_ns::coordinate + cc::x, 0)
        .Set<int64_t>(cmd_ns::coordinate + cc::y, 0)
        .Set<uint32_t>(cmd_ns::coordinate + cc::origin, system_id)
        .Set<uint8_t>(cmd_ns::queue, queue ? 1 : 0)
        .Set<uint8_t>(cmd_ns::queue_to_front, 0);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "The fleet cannot move (a station or immobile)" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"target_system_id", system_id},
             {"target_system", SystemName(system_id, s.systems[s.index[system_id]].obj)}, {"queued", queue},
             {"message", "Move order posted; read the fleet's orders on a later call"} };
}

bool GalaxyManager::SurveyCommand(uint32_t fleet_id, uint32_t system_id, uint32_t planet_id, bool queue, bool post,
                                  std::string* why) {
    // CFleetSurveyDepositHolderCommand: galactic_object -1 surveys the one holder, else the system
    namespace sv = sdk::cmd::survey_planet_order;
    auto cmd = CommandBuilder::Get().Create(sv::kSpec);
    cmd.Set<uint32_t>(sv::fleet, fleet_id)
        .Set<uint32_t>(sv::deposit_holder + kMetaRefType, planet_id != kInvalidId ? kHolderPlanet : kHolderNone)
        .Set<uint32_t>(sv::deposit_holder + kMetaRefId, planet_id)
        .Set<uint32_t>(sv::galactic_object, planet_id != kInvalidId ? kInvalidId : system_id)
        .Set<uint8_t>(sv::queue, queue ? 1 : 0)
        .Set<uint8_t>(sv::queue_to_front, 0);
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    if (!cmd.IsValid(why)) return false;
    if (post && !cmd.Post(NativeCommand::Check::EngineGate)) {
        if (why) *why = cmd.error();
        return false;
    }
    return true;
}

nlohmann::json GalaxyManager::Survey(uint32_t fleet_id, uint32_t system_id, uint32_t planet_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    auto owned = fleets::Owned(s.player);
    if (std::find(owned.begin(), owned.end(), fleet_id) == owned.end()) {
        return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    }
    if (planet_id != kInvalidId) {
        void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
        if (!planet) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };
        system_id = ReadOr<uint32_t>((uintptr_t)planet + sdk::ent::CPlanet::coordinate + sdk::ent::CCelestialCoordinate::origin,
                                     kInvalidId);
    }
    if (!s.index.count(system_id)) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(system_id)} };

    std::string why;
    if (!SurveyCommand(fleet_id, system_id, planet_id, queue, true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "Survey order rejected by the engine" : why} };
    }
    nlohmann::json out = { {"success", true}, {"fleet_id", fleet_id}, {"system_id", system_id},
                           {"system", SystemName(system_id, s.systems[s.index[system_id]].obj)}, {"queued", queue},
                           {"message", "Survey order posted"} };
    if (planet_id != kInvalidId) out["planet_id"] = planet_id;
    return out;
}

std::unordered_map<uint32_t, int> GalaxyManager::JumpsFrom(const Snapshot& s, uint32_t from) {
    std::unordered_map<uint32_t, int> dist;
    if (!s.index.count(from)) return dist;
    std::deque<uint32_t> q{ from };
    dist[from] = 0;
    while (!q.empty()) {
        uint32_t id = q.front();
        q.pop_front();
        for (const auto& l : s.systems[s.index.at(id)].lanes) {
            if (s.index.count(l.first) && !dist.count(l.first)) {
                dist[l.first] = dist[id] + 1;
                q.push_back(l.first);
            }
        }
    }
    return dist;
}

bool GalaxyManager::OwnFleet(const Snapshot& s, uint32_t fleet_id) {
    auto owned = fleets::Owned(s.player);
    return std::find(owned.begin(), owned.end(), fleet_id) != owned.end();
}

nlohmann::json GalaxyManager::FindPath(uint32_t fleet_id, uint32_t to) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) {
        return { {"error", "fleet_id must be one of the player's fleets: the route and travel time depend on its "
                           "speed, FTL and border access"} };
    }
    void* fleet = RefLookup(base_address_, sdk::db::CFleet, fleet_id);
    if (!fleet) return { {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };
    if (!s.index.count(to)) return { {"error", "Unknown system id: " + std::to_string(to)} };

    // from the fleet's own position: CalcEstimatedDays times the first leg from there
    PathCtx ctx{};
    ctx.base = base_address_;
    ctx.fleet = fleet;
    const void* here = FleetCoordinate(base_address_, fleet);
    if (!here || !Read(here, &ctx.from)) return { {"error", "Could not read the fleet's position"} };
    uint32_t from = ReadOr<uint32_t>((uintptr_t)here + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
    ctx.to = SystemCentre(base_address_, to);
    if (!EnginePath(&ctx)) return { {"error", "The engine path finder raised"} };

    int n = ctx.count;
    std::vector<uint32_t> node_system(n, kInvalidId), node_jump(n, 0), node_bypass(n, kInvalidId);
    for (int i = 0; i < n; ++i) {
        uintptr_t node = (uintptr_t)ctx.nodes + i * sdk::rt::CFleetPath_node_size;
        node_system[i] = ReadOr<uint32_t>(node + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
        node_jump[i] = ReadOr<uint32_t>(node + sdk::rt::CFleetPath_node_jump_method, 0);
        node_bypass[i] = ReadOr<uint32_t>(node + sdk::rt::CFleetPath_node_bypass, kInvalidId);
    }
    std::vector<int64_t> per_node(n > 0 ? n : 1, 0);
    int64_t total = 0;
    bool timed = n > 0 && EngineDays(&ctx, per_node.data(), &total);
    FreePath(&ctx);
    if (n == 0) {
        return { {"fleet_id", fleet_id}, {"from_system_id", from}, {"to_system_id", to}, {"reachable", false},
                 {"note", "The game finds no route for this fleet (no hyperlane connection it may use, or it cannot move)"} };
    }

    // nodes are the leave / enter points of each system (the last one is the destination's
    // centre); per_node[i] is the time spent before the leg into node i, so the arrival at
    // node i is per_node[i + 1] (the total for the last)
    auto days = [](int64_t v) { return std::round(v / kFixed * 10.0) / 10.0; };
    nlohmann::json systems = nlohmann::json::array();
    auto add_system = [&](uint32_t id, nlohmann::json arrival, const char* via, uint32_t bypass) {
        void* obj = s.index.count(id) ? s.systems[s.index[id]].obj : nullptr;
        uint32_t owner = obj ? Owner(obj) : kInvalidId;
        bool known = owner != kInvalidId && (owner == s.player_id || Intel(s, obj) >= 1);
        nlohmann::json row = { {"id", id}, {"name", obj ? SystemName(id, obj) : ""},
                               {"owner_id", known ? nlohmann::json(owner) : nlohmann::json(nullptr)},
                               {"arrival_days", arrival} };
        if (via) row["via"] = via;
        // an entry node's bypass is the one arrived at (a gateway, wormhole, relay, L-gate)
        if (void* b = bypass != kInvalidId ? RefLookup(base_address_, sdk::db::CBypass, bypass) : nullptr) {
            std::string key = KeyOf((uintptr_t)b + sdk::ent::CBypass::type, kBypassTypeKey);
            if (!key.empty()) {
                row["bypass"] = { {"key", key} };
                std::string name = SafeLocalize(base_address_, key);  // "lgate" is a loc key, "relay_bypass" not
                if (!name.empty() && name != key) row["bypass"]["name"] = name;
            }
        }
        systems.push_back(row);
    };
    add_system(from, 0.0, nullptr, kInvalidId);
    for (int i = 0; i < n; ++i) {
        uint32_t id = node_system[i];
        if (id == kInvalidId || id == systems.back()["id"].get<uint32_t>()) continue;
        nlohmann::json arrival = nullptr;
        if (timed) arrival = days(i + 1 < n ? per_node[i + 1] : total);
        // the entry node carries how the fleet got here (EPathJumpMethod)
        bool by_bypass = node_jump[i] == 1;
        add_system(id, arrival, node_jump[i] < std::size(kJumpMethods) ? kJumpMethods[node_jump[i]] : "unknown",
                   by_bypass ? node_bypass[i] : kInvalidId);
    }
    nlohmann::json out = { {"fleet_id", fleet_id}, {"from_system_id", from}, {"to_system_id", to}, {"reachable", true},
                           {"jumps", (int)systems.size() - 1}, {"systems", systems} };
    out["estimated_days"] = timed ? nlohmann::json(days(total)) : nlohmann::json(nullptr);
    return out;
}

nlohmann::json GalaxyManager::CancelFleetOrders(const std::vector<uint32_t>& fleet_ids) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (fleet_ids.empty()) return { {"success", false}, {"error", "fleet_ids is empty"} };
    for (uint32_t f : fleet_ids) {
        if (!OwnFleet(s, f)) return { {"success", false}, {"error", "Fleet " + std::to_string(f) + " is not one of the player's fleets"} };
    }
    // CFleetCancelOrdersCommand: ClearOrders + CancelMovement + ClearAutoMoveTarget for each
    // fleet the country controls that has an order
    namespace co = sdk::cmd::fleet_cancel_orders;
    auto cmd = CommandBuilder::Get().Create(co::kSpec);
    std::string why;
    cmd.Set<uint32_t>(co::country, s.player_id);
    if (!SetRefArray(cmd, co::fleets, fleet_ids, &why)) return { {"success", false}, {"error", why} };
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "None of these fleets has an order to cancel" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_ids", fleet_ids}, {"message", "Orders cancelled; the fleets stop where they are"} };
}

nlohmann::json GalaxyManager::FollowFleet(uint32_t fleet_id, uint32_t target_fleet_id, bool attack, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* target = fleets::Find(base_address_, target_fleet_id);
    if (!target) return { {"success", false}, {"error", "Unknown fleet id: " + std::to_string(target_fleet_id)} };
    // CFollowFleetCommand: IsValid asks the CFollowFleetOrder it would add (CanDo)
    namespace fo = sdk::cmd::follow_command;
    auto cmd = CommandBuilder::Get().Create(fo::kSpec);
    cmd.Set<uint32_t>(fo::fleet, fleet_id)
        .Set<uint32_t>(fo::target_fleet, target_fleet_id)
        .Set<uint8_t>(fo::attack, attack ? 1 : 0)
        .Set<uint8_t>(fo::cancelled, 0)
        .Set<uint8_t>(fo::queue, queue ? 1 : 0)
        .Set<uint8_t>(fo::queue_to_front, 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The fleet cannot follow that fleet" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"target_fleet_id", target_fleet_id},
             {"target_fleet", fleets::Name(target)}, {"attack", attack}, {"queued", queue},
             {"message", "Follow order posted"} };
}

nlohmann::json GalaxyManager::SetFleetStance(uint32_t fleet_id, const std::string& stance) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    auto it = std::find(std::begin(kStances), std::end(kStances), stance);
    if (it == std::end(kStances)) return { {"success", false}, {"error", "stance must be one of: passive, aggressive, evasive"} };
    uint32_t value = (uint32_t)(it - std::begin(kStances));
    void* fleet = fleets::Find(base_address_, fleet_id);
    uint32_t before = fleet ? ReadOr<uint32_t>((uintptr_t)fleet + sdk::ent::CFleet::fleet_stance, 0) : 0;
    // CSwitchFleetStanceCommand -> CFleet::SetFleetStance; IsValid: the fleet supports stances
    namespace st = sdk::cmd::switch_fleet_stance_command;
    auto cmd = CommandBuilder::Get().Create(st::kSpec);
    cmd.Set<uint32_t>(st::fleet, fleet_id).Set<uint32_t>(st::stance, value);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "This fleet has no stances (civilian or a station)" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id},
             {"previous_stance", before < std::size(kStances) ? kStances[before] : "unknown"},
             {"stance", stance}, {"stance_name", SafeLocalize(base_address_, kStanceNames[value])} };
}

nlohmann::json GalaxyManager::SetCloaking(uint32_t fleet_id, bool active, bool dry_run) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };

    // TFleetFlags: ~20 bool_conds share one offset (SDK marks them positional), so this is only
    // reported as a raw word for now — the bit meanings come from the save's `properties` block.
    uint32_t flags = ReadOr<uint32_t>((uintptr_t)fleet + sdk::ent::TFleetFlags::is_cloaked, 0);

    std::string why;
    if (active) {
        // CActivateFleetCloakingCommand: fleet +0x20
        namespace c = sdk::cmd::activate_cloaking;
        auto cmd = CommandBuilder::Get().Create(c::kSpec);
        cmd.Set<uint32_t>(c::fleet, fleet_id);
        if (!cmd.IsValid(&why)) {
            return { {"success", false}, {"direction", "activate"}, {"flags", flags},
                     {"error", why.empty() ? "activate_cloaking rejected without a reason" : why} };
        }
        if (dry_run) return { {"success", true}, {"dry_run", true}, {"direction", "activate"}, {"flags", flags} };
        if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
        return { {"success", true}, {"direction", "activate"}, {"fleet_id", fleet_id}, {"flags_before", flags} };
    }

    // CDeactivateFleetCloakingCommand: fleet +0x20, forced +0x28 (ref<CCountry>, empty = voluntary)
    namespace c = sdk::cmd::deactivate_cloaking;
    auto cmd = CommandBuilder::Get().Create(c::kSpec);
    cmd.Set<uint32_t>(c::fleet, fleet_id).Set<uint32_t>(c::forced, 0xFFFFFFFFu);
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"direction", "deactivate"}, {"flags", flags},
                 {"error", why.empty() ? "deactivate_cloaking rejected without a reason" : why} };
    }
    if (dry_run) return { {"success", true}, {"dry_run", true}, {"direction", "deactivate"}, {"flags", flags} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"direction", "deactivate"}, {"fleet_id", fleet_id}, {"flags_before", flags} };
}

nlohmann::json GalaxyManager::ProbeFleetFlags(uint32_t fleet_id, uint32_t offset, uint32_t length) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };
    if (length == 0 || length > 0x400) length = 0x100;
    if (offset > 0x2000) return { {"success", false}, {"error", "offset out of range (<=0x2000)"} };
    nlohmann::json words = nlohmann::json::array();
    for (uint32_t i = 0; i + 4 <= length; i += 4) {
        words.push_back(ReadOr<uint32_t>((uintptr_t)fleet + offset + i, 0xFFFFFFFFu));
    }
    return { {"success", true}, {"fleet_id", fleet_id}, {"ptr", (unsigned long long)(uintptr_t)fleet},
             {"offset", offset}, {"length", length}, {"words", words} };
}

nlohmann::json GalaxyManager::GetColoniesJson() {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country"} };
    void* country = s.player;

    void* data = nullptr;
    uint32_t cnt = 0;
    Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x8), &data);
    Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x14), &cnt);

    nlohmann::json out = nlohmann::json::array();
    uint32_t planet_like = 0, ship_like = 0;
    for (uint32_t i = 0; i < cnt && i < 256; ++i) {
        uint32_t cid = 0;
        if (!Read((const void*)((uintptr_t)data + i * 4), &cid)) continue;
        void* colony = RefLookup(base_address_, sdk::db::CColony, cid);
        if (!colony) continue;

        uint64_t carrier = 0;
        Read((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), &carrier);
        uint32_t carrier_id = (uint32_t)(carrier & 0xFFFFFFull);
        uint32_t carrier_type = (uint32_t)(carrier >> 32);

        auto u32_at = [&](std::ptrdiff_t off) {
            uint32_t v = 0xFFFFFFFF;
            Read((const void*)((uintptr_t)colony + off), &v);
            return v;
        };

        nlohmann::json j = { {"colony_id", cid}, {"carrier_type", carrier_type}, {"carrier_id", carrier_id},
                             {"pop_groups", u32_at(0x18 + 0x14)}, {"pop_jobs", u32_at(0x30 + 0x14)},
                             {"districts", u32_at(0x80 + 0x14)},
                             {"pops", u32_at(0xFC8)}, {"stability_fixed", u32_at(0xFB8)},
                             {"ascension_tier", u32_at(0x110)} };
        if (carrier_type == 0) {
            ++planet_like;
            void* p = RefLookup(base_address_, sdk::db::CPlanet, carrier_id);
            if (p) {
                j["name"] = PersistentNameText((const void*)((uintptr_t)p + sdk::ent::CPlanet::name));
                uint32_t size = 0;
                Read((const void*)((uintptr_t)p + sdk::ent::CPlanet::planet_size), &size);
                j["planet_size"] = size;
            }
            j["kind"] = "planet";
        } else {
            ++ship_like;
            void* ship = RefLookup(base_address_, sdk::db::CShip, carrier_id);
            if (ship) {
                j["name"] = PersistentNameText((const void*)((uintptr_t)ship + sdk::ent::CShip::name));
                uint32_t size = 0;
                Read((const void*)((uintptr_t)ship + sdk::ent::CShip::planet_size), &size);
                j["planet_size"] = size;
            }
            j["kind"] = "ark";
        }
        out.push_back(j);
    }

    uint32_t cid0 = 0;
    Read((const void*)((uintptr_t)country + 0x20), &cid0);
    return { {"success", true}, {"country_id", cid0}, {"total", cnt},
             {"planet_carriers", planet_like}, {"ship_carriers", ship_like}, {"colonies", out} };
}

nlohmann::json GalaxyManager::GetModuleBase() {
    return { {"success", true}, {"base", (unsigned long long)base_address_} };
}

nlohmann::json GalaxyManager::ProbeBuildQueue(uint32_t colony_id) {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country"} };
    void* colony = RefLookup(base_address_, sdk::db::CColony, colony_id);
    if (!colony) return { {"success", false}, {"error", "Unknown colony id: " + std::to_string(colony_id)} };

    uint64_t carrier = 0;
    Read((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), &carrier);
    uint32_t carrier_id = (uint32_t)(carrier & 0xFFFFFFull);
    uint32_t carrier_type = (uint32_t)(carrier >> 32);

    uint32_t queue_id = 0xFFFFFFFF;
    if (carrier_type == 0) {
        void* planet = RefLookup(base_address_, sdk::db::CPlanet, carrier_id);
        if (planet) Read((const void*)((uintptr_t)planet + sdk::ent::CPlanet::build_queue), &queue_id);
    } else {
        void* ship = RefLookup(base_address_, sdk::db::CShip, carrier_id);
        if (ship) Read((const void*)((uintptr_t)ship + sdk::ent::CShip::build_queue), &queue_id);
    }

    nlohmann::json j = { {"success", true}, {"colony_id", colony_id},
                         {"carrier_type", carrier_type}, {"carrier_id", carrier_id},
                         {"queue_id", queue_id} };
    void* q = queue_id == 0xFFFFFFFF ? nullptr : RefLookup(base_address_, sdk::db::CConstructionQueue, queue_id);
    if (!q) {
        j["queue_found"] = false;
        return j;
    }
    j["queue_found"] = true;
    j["queue_ptr"] = (unsigned long long)(uintptr_t)q;
    uint32_t items = 0, owner = 0xFFFFFFFF, simultaneous = 0;
    uint64_t location = 0;
    if (Read((const void*)((uintptr_t)q + sdk::ent::CConstructionQueue::items + 0x14), &items)) j["items"] = items;
    if (Read((const void*)((uintptr_t)q + sdk::ent::CConstructionQueue::owner), &owner)) j["owner"] = owner;
    if (Read((const void*)((uintptr_t)q + sdk::ent::CConstructionQueue::location), &location)) {
        j["location_raw"] = location;
        j["location_low"] = (uint32_t)(location & 0xFFFFFFull);
        j["location_high"] = (uint32_t)(location >> 32);
    }
    if (Read((const void*)((uintptr_t)q + sdk::ent::CConstructionQueue::simultaneous), &simultaneous)) j["simultaneous"] = simultaneous;
    // type 那个字段是个键字符串，galaxy_manager 里没有读字符串的 helper，先不取
    return j;
}

nlohmann::json GalaxyManager::ProbeColony(uint32_t colony_id, uint32_t offset, uint32_t length) {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country"} };
    void* colony = RefLookup(base_address_, sdk::db::CColony, colony_id);
    if (!colony) return { {"success", false}, {"error", "Unknown colony id: " + std::to_string(colony_id)} };
    if (length == 0 || length > 0x400) length = 0x100;
    if (offset > 0x2000) return { {"success", false}, {"error", "offset out of range (<=0x2000)"} };

    // CColony::carrier: 8 bytes, low 4 = id, high 4 = type (0 planet / 1 ship) — same field
    // probe_colony_carriers reads. Everything else about a colony is the same object either way.
    uint64_t carrier = 0;
    Read((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), &carrier);
    uint32_t carrier_id = (uint32_t)(carrier & 0xFFFFFFull);
    uint32_t carrier_type = (uint32_t)(carrier >> 32);

    nlohmann::json j = { {"success", true}, {"colony_id", colony_id},
                         {"ptr", (unsigned long long)(uintptr_t)colony},
                         {"carrier_id", carrier_id}, {"carrier_type", carrier_type},
                         {"offset", offset}, {"length", length} };
    if (carrier_type == 0) {
        void* planet = RefLookup(base_address_, sdk::db::CPlanet, carrier_id);
        if (planet) {
            j["name"] = PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name));
            j["planet_ptr"] = (unsigned long long)(uintptr_t)planet;
        }
    } else {
        void* ship = RefLookup(base_address_, sdk::db::CShip, carrier_id);
        if (!ship) {
            j["error"] = "carrier ship not found";
        } else {
            j["name"] = PersistentNameText((const void*)((uintptr_t)ship + sdk::ent::CShip::name));
            j["ship_ptr"] = (unsigned long long)(uintptr_t)ship;
            uint32_t colony_ref = 0xFFFFFFFF, size = 0;
            Read((const void*)((uintptr_t)ship + sdk::ent::CShip::colony), &colony_ref);
            Read((const void*)((uintptr_t)ship + sdk::ent::CShip::planet_size), &size);
            j["ship_colony_ref"] = colony_ref;
            j["planet_size"] = size;
        }
    }

    nlohmann::json words = nlohmann::json::array();
    for (uint32_t i = 0; i + 4 <= length; i += 4) {
        words.push_back(ReadOr<uint32_t>((uintptr_t)colony + offset + i, 0xFFFFFFFFu));
    }
    j["words"] = words;
    return j;
}

nlohmann::json GalaxyManager::SetFleetHomeBase(uint32_t fleet_id, uint32_t starbase_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };

    uint32_t template_id = fleets::TemplateId(fleet);
    if (template_id == 0xFFFFFFFFu) return { {"success", false}, {"error", "This fleet has no fleet template"} };

    /* 在 db::CStarbase 里按 id 找对象，顺便拿到指针（命令的 orbitable 字段要的就是它） */
    if (starbase_id > 0xFFFFFF) return { {"success", false}, {"error", "starbase_id out of range"} };

    uint64_t before = 0;
    void* tmpl = nullptr;
    {
        void* db = nullptr; void* arr = nullptr; uint32_t cap = 0; uint32_t check = 0xFFFFFFFF;
        if (Read((const void*)(base_address_ + sdk::db::CFleetTemplate), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            Read((const void*)((uintptr_t)db + 0x20), &cap) && (template_id & 0xFFFFFF) < cap &&
            Read((const void*)((uintptr_t)arr + (template_id & 0xFFFFFF) * 16 + 8), &tmpl) && tmpl &&
            Read((const void*)((uintptr_t)tmpl + sdk::rt::CFleetTemplate_id), &check) && check == template_id) {
            Read((const void*)((uintptr_t)tmpl + sdk::ent::CFleetTemplate::home_base), &before);
        } else {
            tmpl = nullptr;
        }
    }

    namespace hb = sdk::cmd::set_home_base_command;
    auto cmd = CommandBuilder::Get().Create(hb::kSpec);
    cmd.Set<uint32_t>(hb::owner, s.player_id)
       .Set<uint32_t>(hb::template_, template_id)
       .Set<uint64_t>(hb::orbitable, ((uint64_t)2u << 32) | (uint64_t)starbase_id)
       .Set<uint8_t>(hb::is_human_request, 1);

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "The engine refused this home base" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };

    char before_hex[32];
    std::snprintf(before_hex, sizeof(before_hex), "0x%llX", (unsigned long long)before);
    return { {"success", true}, {"fleet_id", fleet_id}, {"fleet", fleets::Name(fleet)},
             {"template_id", template_id}, {"starbase_id", starbase_id},
             {"previous_home", before_hex}, {"message", "Home base command posted"} };
}

// 只读：全银河的 bypass（虫洞 / 星辉虫洞 / 星门）。
// 位置靠「每个星系的 bypass 数组」（CGalacticObject+0x5A8）反推，与 GetSystemJson 同一套；
// 「已探明 / 可用」读玩家国家的 usable_bypasses（CCountry+0x34A0）与 seen_bypasses（+0x3430）。
// 虫洞是成对的（one_to_one，靠 CBypass+0x18 linked_to 找对端），星门是网状（active_connections +0x38）。
nlohmann::json GalaxyManager::GetBypassesJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };

    std::unordered_map<uint32_t, uint32_t> bypass_system;
    for (const auto& sys : s.systems) {
        for (uint32_t b : RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::bypasses)) bypass_system[b] = sys.id;
    }
    auto id_set = [&](std::ptrdiff_t off) {
        std::unordered_set<uint32_t> out;
        for (uint32_t v : RefArray((uintptr_t)s.player + off)) out.insert(v & 0xFFFFFFu);
        return out;
    };
    const auto usable = id_set(sdk::ent::CCountry::usable_bypasses);
    const auto seen = id_set(sdk::ent::CCountry::seen_bypasses);

    namespace bp = sdk::ent::CBypass;
    auto sys_row = [&](uint32_t sys_id) {
        auto it = s.index.find(sys_id);
        return nlohmann::json{ {"id", sys_id},
                               {"name", it != s.index.end() ? SystemName(sys_id, s.systems[it->second].obj) : ""} };
    };

    nlohmann::json rows = nlohmann::json::array();
    uint32_t usable_cnt = 0, wormhole_cnt = 0, gateway_cnt = 0;
    for (const auto& [id, sid] : bypass_system) {
        void* b = RefLookup(base_address_, sdk::db::CBypass, id);
        if (!b) continue;
        std::string type = KeyOf((uintptr_t)b + bp::type, kBypassTypeKey);
        std::string type_name = type.empty() ? "" : SafeLocalize(base_address_, type);
        nlohmann::json row = { {"id", id}, {"type", type}, {"system", sys_row(sid)} };
        if (!type_name.empty() && type_name != type) row["type_name"] = type_name;
        row["active"] = ReadOr<uint8_t>((uintptr_t)b + bp::active, 0) != 0;
        uint32_t linked = ReadOr<uint32_t>((uintptr_t)b + bp::linked_to, kInvalidId);
        if (linked != kInvalidId) {
            row["linked_to"] = linked;
            auto it2 = bypass_system.find(linked);
            if (it2 != bypass_system.end()) row["linked_system"] = sys_row(it2->second);
        }
        nlohmann::json conns = nlohmann::json::array();
        for (uint32_t conn : RefArray((uintptr_t)b + bp::active_connections)) conns.push_back(conn);
        if (!conns.empty()) row["active_connections"] = conns;
        // 未启用的星门：网络形状写在 connections（+0x20）里，active_connections 是空的
        nlohmann::json all_conns = nlohmann::json::array();
        for (uint32_t conn : RefArray((uintptr_t)b + bp::connections)) all_conns.push_back(conn);
        if (!all_conns.empty()) row["connections"] = all_conns;
        const bool is_usable = usable.count(id) != 0;
        row["usable"] = is_usable;
        row["seen"] = seen.count(id) != 0;
        uint32_t lock = ReadOr<uint32_t>((uintptr_t)b + bp::lock_country, kInvalidId);
        if (lock != kInvalidId) {
            row["locked_by"] = lock;
            row["lock_days"] = ReadOr<int32_t>((uintptr_t)b + bp::lock_remaining_days, 0);
        }
        if (is_usable) usable_cnt++;
        if (type.find("gateway") != std::string::npos) gateway_cnt++;
        else wormhole_cnt++;
        rows.push_back(row);
    }
    return { {"count", rows.size()}, {"usable_count", usable_cnt},
             {"wormhole_count", wormhole_cnt}, {"gateway_count", gateway_cnt},
             {"bypasses", rows} };
}

// 只读：全银河的舰队（含太空兽群 / 中立势力 / 海盗 / 别国）。
// 2026-11-22 加：太空兽群在存档里就是 CFleet（owner 是个特殊 country_type），
// 光靠 get_military_fleets（只给本国的）看不到它们。
//   fleet db    TPdxRef<CFleet>::_pDatabase → {slots +0x18, cap +0x20}，对象在 slot*16+8
//   id          CFleet+0x30 ｜ owner +0x44C（本项目的 2026-11-21 定案）
//   name        CFleet+0x3F0 ｜ ship_class +0x1280 ｜ ships ref_array +0x320
//   所在星系   扫每个星系的 fleet_presence（go::fleet_presence）反推
//   owner 类型 CCountry+0x1D8 的 PdxString（default / neutral_faction / space_fauna …）
// 只读：本国的「舰队编制模板」库（2026-11-22 加）。
// 模板挂在舰队上（CFleetTemplate.fleet），但模板对象本身是独立数据库：
//   TPdxRef<CFleetTemplate>::_pDatabase（0x311BE00）→ {slots +0x18, cap +0x20}，对象在 slot*16+8
//   舰队的 id：CFleetTemplate::fleet（+0x88）
//   条目数组：+0x28（指针）/ +0x34（个数），每项 CFleetTemplateDesign，步长 0x560
//     设计 id：条目 + ship_design_implementation(+0x10?) + CShipDesignImplementation_design
//     编制数量：条目 +0x558
// 太空兽群（晶态实体 / 虚空虫…）会自动生成设计，所以在模板里能直接看见。
nlohmann::json GalaxyManager::GetFleetTemplatesJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };

    std::unordered_map<uint32_t, std::pair<std::string, std::string>> designs;  // id -> {name, ship_size}
    for (const auto& d : ShipDesigner::Get().GetShipDesigns()) designs[d.design_id] = { d.name, d.ship_size };

    void* db = nullptr;
    if (!Read((const void*)(base_address_ + sdk::db::CFleetTemplate), &db) || !db) {
        return { {"error", "fleet template database not readable"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    Read((const void*)((uintptr_t)db + 0x18), &slots);
    Read((const void*)((uintptr_t)db + 0x20), &cap);
    if (!slots || cap == 0 || cap > 100000) return { {"error", "fleet template table unreadable"} };

    nlohmann::json out = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* t = nullptr;
        if (!Read((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &t) || !t) continue;
        const uint32_t fleet_raw = ReadOr<uint32_t>((uintptr_t)t + sdk::ent::CFleetTemplate::fleet, 0xFFFFFFFFu);
        const uint32_t fleet_id = fleet_raw & 0xFFFFFFu;
        // 只报我们自己的模板：模板的舰队属于我们，或舰队已不存在（模板还在）
        std::string fleet_name;
        uint32_t fleet_owner = 0xFFFFFFFFu;
        if (fleet_id != 0xFFFFFFu) {
            if (void* flt = fleets::Find(base_address_, fleet_id)) {
                fleet_owner = fleets::Owner(flt);
                fleet_name = fleets::Name(flt);
            }
        }
        // 只要本国模板：舰队必须存在且 owner 是我国（之前放宽成"没有 owner 也收"，把别国的一起卷进来了）
        // 用 GameState 的同一个取法（outliner 判 relation 也用它），别自己猜 CCountry 的 id 偏移
        const uint32_t my_id = GameState::Get().GetPlayerCountryId();
        // owner 有时是带标记的引用（0x01000000|id），两边都按低 24 位比
        if (fleet_id == 0xFFFFFFu || (fleet_owner & 0xFFFFFFu) != (my_id & 0xFFFFFFu)) continue;

        void* arr = nullptr;
        uint32_t cnt = 0;
        Read((const void*)((uintptr_t)t + 0x28), &arr);
        Read((const void*)((uintptr_t)t + 0x34), &cnt);
        nlohmann::json entries = nlohmann::json::array();
        uint32_t total_quota = 0;
        if (arr && cnt > 0 && cnt <= 64) {
            for (uint32_t d = 0; d < cnt; ++d) {
                const uintptr_t entry = (uintptr_t)arr + (size_t)d * 0x560;
                // 设计 id 就在条目 +0x550（= CShipDesignImplementation_design 0x530 + ship_design_implementation 0x20）
                const uint32_t design_id = ReadOr<uint32_t>(entry + 0x550, 0xFFFFFFFFu);
                const uint32_t quota = ReadOr<uint32_t>(entry + 0x558, 0);
                nlohmann::json e = { {"design_id", design_id == 0xFFFFFFFFu ? nlohmann::json(nullptr) : nlohmann::json(design_id)},
                                     {"quota", quota} };
                auto it = designs.find(design_id);
                if (it != designs.end()) {
                    e["design_name"] = it->second.first;
                    e["ship_size"] = it->second.second;
                }
                total_quota += quota;
                entries.push_back(e);
            }
        }
        out.push_back({ {"template_index", i},
                        {"fleet_id", fleet_id == 0xFFFFFFu ? nlohmann::json(nullptr) : nlohmann::json(fleet_id)},
                        {"fleet_name", fleet_name},
                        {"total_quota", total_quota},
                        {"entries", entries} });
    }
    return { {"count", out.size()}, {"templates", out} };
}

nlohmann::json GalaxyManager::GetFleetsAllJson(const std::string& type_filter, bool foreign_only, uint32_t limit) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (limit == 0 || limit > 2000) limit = 500;

    // 所在星系：扫每个星系的 fleet_presence 反推（与 GetSystemJson 同一套）
    std::unordered_map<uint32_t, uint32_t> fleet_system;
    for (const auto& sys : s.systems) {
        for (uint32_t f : RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::fleet_presence)) {
            fleet_system[f & 0xFFFFFFu] = sys.id;
        }
    }

    // 走 fleet_access 的枚举器：cap < 1e6 且要求 (id & 0xFFFFFF) == 槽位号，脏 cap 直接放弃
    const std::vector<uint32_t> ids = fleets::AllIds(base_address_);
    std::string needle = type_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    nlohmann::json rows = nlohmann::json::array();
    for (uint32_t id : ids) {
        if (rows.size() >= limit) break;
        void* flt = fleets::Find(base_address_, id);
        if (!flt) continue;
        const uint32_t owner = fleets::Owner(flt);
        const uint32_t controller = fleets::Controller(flt);
        if (foreign_only && owner == s.player_id) continue;
        const fleets::ShipClass cls = fleets::ClassOf(flt);
        const std::string class_key = fleets::ShipClassKey(cls);
        if (!needle.empty()) {
            std::string low = class_key;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find(needle) == std::string::npos) continue;
        }
        nlohmann::json row = { {"id", id},
                               {"name", fleets::Name(flt)},
                               {"class", class_key},
                               {"military_power", fleets::MilitaryPower(flt)},
                               {"ship_count", ReadOr<uint32_t>((uintptr_t)flt + sdk::ent::CFleet::ships + 0xC, 0)},
                               {"owner", owner == 0xFFFFFFu ? nlohmann::json(nullptr) : nlohmann::json(owner & 0xFFFFFFu)},
                               {"controller", controller == 0xFFFFFFu ? nlohmann::json(nullptr) : nlohmann::json(controller & 0xFFFFFFu)},
                               {"status", fleets::OrdersText(flt)} };
        auto sit = fleet_system.find(id);
        if (sit != fleet_system.end()) {
            auto it2 = s.index.find(sit->second);
            row["system"] = { {"id", sit->second},
                              {"name", it2 != s.index.end() ? SystemName(sit->second, s.systems[it2->second].obj) : ""} };
        }
        rows.push_back(row);
    }
    return { {"count", rows.size()}, {"fleet_total", ids.size()}, {"fleets", rows} };
}

nlohmann::json GalaxyManager::ProbeFleetShips(uint32_t fleet_id) {
    /* 只读探针：舰队真正的舰船列表在 CFleet::ships(0x320)，条目是 CShip 的 ref(u32)，
       个数在 0x320+0xC。每艘船的设计 id 不知道在哪个偏移，这里把几个候选都读出来，
       用已知真值（海啸 4 艘、飓风 3 艘，设计 1552）反推。 */
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet"} };

    void* arr = nullptr;
    uint32_t count = 0;
    if (!Read((const void*)((uintptr_t)fleet + sdk::ent::CFleet::ships), &arr) ||
        !Read((const void*)((uintptr_t)fleet + sdk::ent::CFleet::ships + 0xC), &count)) {
        return { {"success", false}, {"error", "Could not read the ship list"} };
    }

    nlohmann::json ships = nlohmann::json::array();
    if (arr && count < 5000) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t ship_id = 0xFFFFFFFF;
            if (!Read((const void*)((uintptr_t)arr + i * 4), &ship_id)) break;
            nlohmann::json item = { {"index", i}, {"ship_id", ship_id} };
            void* ship = nullptr;
            {
                void* db = nullptr; void* sarr = nullptr; uint32_t scap = 0; uint32_t sid = 0xFFFFFFFF; void* cand = nullptr;
                if (Read((const void*)(base_address_ + sdk::db::CShip), &db) && db &&
                    Read((const void*)((uintptr_t)db + 0x18), &sarr) && sarr &&
                    Read((const void*)((uintptr_t)db + 0x20), &scap) && (ship_id & 0xFFFFFF) < scap &&
                    Read((const void*)((uintptr_t)sarr + (ship_id & 0xFFFFFF) * 16 + 8), &cand) && cand &&
                    Read((const void*)((uintptr_t)cand + sdk::rt::CShip_id), &sid) && sid == ship_id) {
                    ship = cand;
                }
            }
            if (ship) {
                nlohmann::json words = nlohmann::json::object();
                for (int off = 0x400; off <= 0x4F8; off += 8) {
                    uint64_t v = 0;
                    if (Read((const void*)((uintptr_t)ship + off), &v)) {
                        char key[16];
                        std::snprintf(key, sizeof(key), "0x%03X", off);
                        words[key] = v;
                    }
                }
                item["words"] = words;
                /* 顺着 0x478 / 0x4D8 两个指针各读 8 个字，看设计号是否在它们指向的对象里 */
                for (const char* name : {"ptr478", "ptr4d8"}) {
                    const int off = (std::string(name) == "ptr478") ? 0x478 : 0x4D8;
                    uint64_t ptr = 0;
                    nlohmann::json target = nlohmann::json::array();
                    if (Read((const void*)((uintptr_t)ship + off), &ptr) && ptr > 0x10000) {
                        for (int k = 0; k < 8; ++k) {
                            uint64_t v = 0;
                            if (Read((const void*)((uintptr_t)ptr + k * 8), &v)) target.push_back(v);
                        }
                    }
                    item[name] = { {"pointer", ptr}, {"words", target} };
                }
            }
            ships.push_back(item);
        }
    }
    return { {"success", true}, {"fleet_id", fleet_id}, {"ships_offset", 0x320},
             {"count", count}, {"ships", ships} };
}

namespace {
bool WriteU32Guarded(void* addr, uint32_t value) {
    if (!addr) return false;
    __try {
        *(uint32_t*)addr = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

nlohmann::json GalaxyManager::SetFleetTemplateDesign(uint32_t fleet_id, uint32_t from_design, uint32_t to_design) {
    /* 把舰队模板里"旧设计"那条改成"新设计"。设计号在条目 +0x20 的
       CShipDesignImplementation_design(0x530) 处，即条目 +0x550。
       只写一个 u32，不动数组；随后 upgrade_fleet 会让现有舰船按新设计改装。 */
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id"} };
    uint32_t template_id = fleets::TemplateId(fleet);
    if (template_id == 0xFFFFFFFFu) return { {"success", false}, {"error", "This fleet has no template"} };

    void* tmpl = nullptr;
    {
        void* db = nullptr; void* arr = nullptr; uint32_t cap = 0; uint32_t check = 0xFFFFFFFF;
        if (Read((const void*)(base_address_ + sdk::db::CFleetTemplate), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            Read((const void*)((uintptr_t)db + 0x20), &cap) && (template_id & 0xFFFFFF) < cap &&
            Read((const void*)((uintptr_t)arr + (template_id & 0xFFFFFF) * 16 + 8), &tmpl) && tmpl &&
            Read((const void*)((uintptr_t)tmpl + sdk::rt::CFleetTemplate_id), &check) && check == template_id) {
            /* 命中，保留 tmpl */
        } else {
            tmpl = nullptr;
        }
    }
    if (!tmpl) return { {"success", false}, {"error", "Template not found"} };

    void* designs_arr = nullptr;
    uint32_t designs_cnt = 0;
    if (!(Read((const void*)((uintptr_t)tmpl + 0x28), &designs_arr) && designs_arr &&
          Read((const void*)((uintptr_t)tmpl + 0x34), &designs_cnt) && designs_cnt > 0)) {
        return { {"success", false}, {"error", "Template has no design entries"} };
    }

    uintptr_t target = 0;
    for (uint32_t d = 0; d < designs_cnt && d < 64; ++d) {
        uintptr_t entry = (uintptr_t)designs_arr + d * 0x560;
        uint32_t id = 0xFFFFFFFF;
        if (!Read((const void*)(entry + 0x20 + sdk::rt::CShipDesignImplementation_design), &id)) continue;
        if (id == from_design) { target = entry; break; }
    }
    if (!target) return { {"success", false}, {"error", "No entry with that design id"} };

    uint32_t before = 0;
    Read((const void*)(target + 0x20 + sdk::rt::CShipDesignImplementation_design), &before);
    if (!WriteU32Guarded((void*)(target + 0x20 + sdk::rt::CShipDesignImplementation_design), to_design)) {
        return { {"success", false}, {"error", "write failed"} };
    }
    uint32_t after = 0;
    Read((const void*)(target + 0x20 + sdk::rt::CShipDesignImplementation_design), &after);
    return { {"success", after == to_design}, {"fleet_id", fleet_id}, {"template_id", template_id},
             {"from", before}, {"to", after} };
}

nlohmann::json GalaxyManager::SetShipUpgradeDesign(uint32_t fleet_id, uint32_t to_design) {
    /* 把舰队里每艘船的"待升级目标设计"（CShip+0x98C）写成指定设计，
       之后 upgrade_fleet 就会按这个目标改装。只写 u32，不动容器。 */
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id"} };

    void* ship_arr = nullptr;
    uint32_t count = 0;
    if (!(Read((const void*)((uintptr_t)fleet + sdk::ent::CFleet::ships), &ship_arr) && ship_arr &&
          Read((const void*)((uintptr_t)fleet + sdk::ent::CFleet::ships + 0xC), &count))) {
        return { {"success", false}, {"error", "Could not read the ship list"} };
    }

    nlohmann::json ships = nlohmann::json::array();
    uint32_t written = 0;
    for (uint32_t i = 0; i < count && i < 5000; ++i) {
        uint32_t sid = 0xFFFFFFFF;
        if (!Read((const void*)((uintptr_t)ship_arr + i * 4), &sid)) break;
        void* db = nullptr; void* arr = nullptr; uint32_t cap = 0; void* so = nullptr; uint32_t check = 0xFFFFFFFF;
        if (!(Read((const void*)(base_address_ + sdk::db::CShip), &db) && db &&
              Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
              Read((const void*)((uintptr_t)db + 0x20), &cap) && (sid & 0xFFFFFF) < cap &&
              Read((const void*)((uintptr_t)arr + (sid & 0xFFFFFF) * 16 + 8), &so) && so &&
              Read((const void*)((uintptr_t)so + sdk::rt::CShip_id), &check) && check == sid)) continue;
        uint32_t design = 0xFFFFFFFF, before = 0xFFFFFFFF;
        Read((const void*)((uintptr_t)so + 0x988), &design);
        Read((const void*)((uintptr_t)so + 0x98C), &before);
        bool ok = WriteU32Guarded((void*)((uintptr_t)so + 0x98C), to_design);
        uint32_t after = 0xFFFFFFFF;
        Read((const void*)((uintptr_t)so + 0x98C), &after);
        if (ok && after == to_design) ++written;
        ships.push_back({ {"ship_id", sid}, {"design", design}, {"upgrade_before", before}, {"upgrade_after", after} });
    }
    return { {"success", written > 0}, {"fleet_id", fleet_id}, {"written", written}, {"ships", ships} };
}

nlohmann::json GalaxyManager::ScanShipForValues(uint32_t ship_id, std::vector<uint32_t> values) {
    /* 在船的头部 0x2000 字节里搜给定的 u32，报告出现的位置。
       设计号（+0x988）就是这么定位的：搜 1552 命中 0x98C、搜 49 命中 0x988。 */
    void* ship = nullptr;
    {
        void* db = nullptr; void* arr = nullptr; uint32_t cap = 0; uint32_t sid = 0xFFFFFFFF; void* cand = nullptr;
        if (Read((const void*)(base_address_ + sdk::db::CShip), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            Read((const void*)((uintptr_t)db + 0x20), &cap) && (ship_id & 0xFFFFFF) < cap &&
            Read((const void*)((uintptr_t)arr + (ship_id & 0xFFFFFF) * 16 + 8), &cand) && cand &&
            Read((const void*)((uintptr_t)cand + sdk::rt::CShip_id), &sid) && sid == ship_id) {
            ship = cand;
        }
    }
    if (!ship) return { {"success", false}, {"error", "Unknown ship"} };

    nlohmann::json hits = nlohmann::json::array();
    for (int off = 0; off < 0x2000; off += 4) {
        uint32_t v = 0;
        if (!Read((const void*)((uintptr_t)ship + off), &v)) break;
        for (uint32_t want : values) {
            if (v == want) {
                char key[16];
                std::snprintf(key, sizeof(key), "0x%04X", off);
                hits.push_back({ {"offset", key}, {"value", want} });
            }
        }
    }
    return { {"success", true}, {"ship_id", ship_id}, {"hits", hits} };
}

namespace {
bool ReadBytes(const void* addr, void* out, size_t n) {
    __try {
        std::memcpy(out, addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

nlohmann::json GalaxyManager::ScanConsoleForText(const std::string& needle) {
    /* 只读探针：控制台把输出打到哪儿？
       1) 先在 CConsole 对象自身前 0x1000 字节里找明文
       2) 再把其中像指针的字逐个解引用，在它们指向的 0x600 字节里找
       找到就报告偏移/指针，用来定位“输出缓冲”。 */
    void* console = nullptr;
    if (!Read((const void*)(base_address_ + sdk::glob::CConsole_pInstance), &console) || !console) {
        return { {"success", false}, {"error", "CConsole::_pInstance is null"} };
    }
    if (needle.empty()) return { {"success", false}, {"error", "needle is empty"} };

    char console_hex[32];
    std::snprintf(console_hex, sizeof(console_hex), "0x%llX", (unsigned long long)(uintptr_t)console);

    nlohmann::json self_hits = nlohmann::json::array();
    {
        std::vector<uint8_t> buf(0x1000, 0);
        size_t got = 0;
        for (size_t off = 0; off < buf.size(); off += 0x100) {
            if (!ReadBytes((const uint8_t*)console + off, buf.data() + off, 0x100)) break;
            got = off + 0x100;
        }
        if (got >= needle.size()) {
            for (size_t i = 0; i + needle.size() <= got; ++i) {
                if (std::memcmp(buf.data() + i, needle.data(), needle.size()) == 0) {
                    char key[16];
                    std::snprintf(key, sizeof(key), "0x%03zX", i);
                    self_hits.push_back(key);
                }
            }
        }
    }

    nlohmann::json ptr_hits = nlohmann::json::array();
    for (int off = 0; off < 0x1000; off += 8) {
        uint64_t value = 0;
        if (!Read((const uint8_t*)console + off, &value)) break;
        if (value < 0x10000000000ull || value > 0x7FFFFFFFFFFFull) continue;

        std::vector<uint8_t> buf(0x600, 0);
        size_t got = 0;
        for (size_t i = 0; i < buf.size(); i += 0x100) {
            if (!ReadBytes((const uint8_t*)(uintptr_t)value + i, buf.data() + i, 0x100)) break;
            got = i + 0x100;
        }
        if (got < needle.size()) continue;
        for (size_t i = 0; i + needle.size() <= got; ++i) {
            if (std::memcmp(buf.data() + i, needle.data(), needle.size()) == 0) {
                char item[64];
                std::snprintf(item, sizeof(item), "slot0x%03X->0x%llX+0x%zX",
                              off, (unsigned long long)value, i);
                ptr_hits.push_back(item);
            }
        }
    }

    return { {"success", true}, {"console", console_hex}, {"needle", needle},
             {"self_hits", self_hits}, {"ptr_hits", ptr_hits} };
}

nlohmann::json GalaxyManager::ProbeConsoleSlot(uint32_t slot_offset, uint32_t length) {
    /* 读 CConsole+slot 处的指针，并把指向的内容按可打印 ASCII 输出；
       同时尝试把它当 MSVC std::string（buf[16], size, cap）解释。 */
    void* console = nullptr;
    if (!Read((const void*)(base_address_ + sdk::glob::CConsole_pInstance), &console) || !console) {
        return { {"success", false}, {"error", "CConsole::_pInstance is null"} };
    }
    if (length == 0 || length > 0x1000) length = 0x200;

    uint64_t ptr = 0;
    if (!Read((const uint8_t*)console + slot_offset, &ptr) || !ptr) {
        return { {"success", false}, {"error", "slot is not a usable pointer"} };
    }

    std::vector<uint8_t> buf(length, 0);
    size_t got = 0;
    for (size_t i = 0; i < length; i += 0x100) {
        size_t chunk = std::min<size_t>(0x100, length - i);
        if (!ReadBytes((const uint8_t*)(uintptr_t)ptr + i, buf.data() + i, chunk)) break;
        got = i + chunk;
    }

    std::string printable;
    printable.reserve(got);
    for (size_t i = 0; i < got; ++i) {
        uint8_t c = buf[i];
        printable.push_back((c >= 32 && c < 127) ? (char)c : '.');
    }

    /* std::string 语义：指针 + 0x10 起是数据，size 在 +0x10，cap 在 +0x18 */
    uint64_t str_size = 0, str_cap = 0;
    Read((const uint8_t*)(uintptr_t)ptr + 0x10, &str_size);
    Read((const uint8_t*)(uintptr_t)ptr + 0x18, &str_cap);

    char ptr_hex[32];
    std::snprintf(ptr_hex, sizeof(ptr_hex), "0x%llX", (unsigned long long)ptr);
    return { {"success", true}, {"slot", slot_offset}, {"pointer", ptr_hex},
             {"got", got}, {"string_size", str_size}, {"string_cap", str_cap},
             {"ascii", printable} };
}



nlohmann::json GalaxyManager::ProbeColonyCarriersJson() {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country"} };
    void* country = s.player;

    void* data = nullptr;
    uint32_t cnt = 0;
    Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x8), &data);
    Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x14), &cnt);

    nlohmann::json out = nlohmann::json::array();
    uint32_t planet_like = 0, ship_like = 0;
    for (uint32_t i = 0; i < cnt && i < 256; ++i) {
        uint32_t cid = 0;
        if (!Read((const void*)((uintptr_t)data + i * 4), &cid)) continue;
        void* colony = RefLookup(base_address_, sdk::db::CColony, cid);
        nlohmann::json j = { {"colony_id", cid} };
        if (!colony) { j["error"] = "colony not found"; out.push_back(j); continue; }
        uint64_t carrier = 0;
        Read((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), &carrier);
        uint32_t id = (uint32_t)(carrier & 0xFFFFFFFFull);
        uint32_t type = (uint32_t)(carrier >> 32);
        j["carrier_id"] = id;
        j["carrier_type"] = type;
        if (type == 0) {
            ++planet_like;
            void* p = RefLookup(base_address_, sdk::db::CPlanet, id);
            if (p) j["planet_name"] = PersistentNameText((const void*)((uintptr_t)p + sdk::ent::CPlanet::name));
        } else {
            ++ship_like;
        }
        out.push_back(j);
    }

    uint32_t cid0 = 0;
    Read((const void*)((uintptr_t)country + 0x20), &cid0);
    return { {"success", true}, {"country_id", cid0}, {"total", cnt},
             {"planet_carriers", planet_like}, {"ship_carriers", ship_like}, {"colonies", out} };
}
// Where do the researched technologies sit? CCountry::tech_status = +0x1620 is a CTechnologyStatus
// (SDK: level +0x20, technology +0x2C, stored_techpoints +0x90). Walking it on guessed offsets is how
// the market manager's "is this resource unlocked" check came to use a 0x28 stride with the pointer at
// +0x10, which the SDK fields do not support. So: resolve every CTechnology of the database first
// (array +0x50 / count +0x5C, key at +0x20, the same shape as the edicts db), then report every qword
// inside the status object that is one of them — the offsets give the real layout, no guessing.
nlohmann::json GalaxyManager::ProbeTechStatusJson(uint32_t offset, uint32_t length, bool dump_hex) {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "No player country"} };
    void* country = s.player;
    const uintptr_t ts = (uintptr_t)country + offset;
    if (length < 0x40) length = 0x40;
    if (length > 0x4000) length = 0x4000;

    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cnt = 0;
    std::map<uintptr_t, std::string> tech_keys;
    if (Read((const void*)(base_address_ + sdk::glob::TGameDatabase_CTechnologyDatabase_pInstance), &db) && db &&
        Read((const void*)((uintptr_t)db + 0x50), &arr) && arr &&
        Read((const void*)((uintptr_t)db + 0x5C), &cnt) && cnt < 100000) {
        for (uint32_t i = 0; i < cnt; ++i) {
            void* p = nullptr;
            if (!Read((const void*)((uintptr_t)arr + i * 8), &p) || !p) continue;
            tech_keys[(uintptr_t)p] = ReadString((uintptr_t)p + 0x20);
        }
    }

    nlohmann::json hits = nlohmann::json::array();
    for (uint32_t off = 0; off + 8 <= length; off += 8) {
        void* p = nullptr;
        if (!Read((const void*)(ts + off), &p) || !p) continue;
        auto it = tech_keys.find((uintptr_t)p);
        if (it == tech_keys.end()) continue;
        nlohmann::json h;
        h["offset"] = off;
        h["key"] = it->second;
        uint32_t id = 0;
        if (Read((const void*)((uintptr_t)it->first + 0x8), &id)) h["tech_id"] = id;
        int32_t v = 0;
        if (off >= 8 && Read((const void*)(ts + off - 8), &v)) h["i32_before8"] = v;
        if (off >= 4 && Read((const void*)(ts + off - 4), &v)) h["i32_before4"] = v;
        if (Read((const void*)(ts + off + 8), &v)) h["i32_after8"] = v;
        if (Read((const void*)(ts + off + 12), &v)) h["i32_after12"] = v;
        hits.push_back(h);
    }

    // a researched set may be stored as TPdxRef ids (u32) instead of pointers — report those too
    std::map<uint32_t, std::string> tech_ids;
    for (const auto& kv : tech_keys) {
        uint32_t id = 0;
        if (Read((const void*)(kv.first + 0x8), &id) && id) tech_ids[id] = kv.second;
    }
    nlohmann::json id_hits = nlohmann::json::array();
    for (uint32_t off = 0; off + 4 <= length && id_hits.size() < 64; off += 4) {
        uint32_t id = 0;
        if (!Read((const void*)(ts + off), &id) || !id) continue;
        auto it = tech_ids.find(id);
        if (it == tech_ids.end()) continue;
        id_hits.push_back({ {"offset", off}, {"key", it->second}, {"id", id} });
    }

    std::string hex;
    char line[128];
    for (uint32_t off = 0; dump_hex && off < length; off += 16) {
        unsigned char b[16]{};
        const uint32_t n = (length - off < 16) ? (length - off) : 16;
        if (!CopyChars((char*)b, (const char*)(ts + off), n)) break;
        int w = snprintf(line, sizeof(line), "%04X:", off);
        for (uint32_t i = 0; i < n; ++i) {
            w += snprintf(line + w, sizeof(line) - (size_t)w, " %02X", b[i]);
        }
        hex += line;
        hex += "\n";
    }

    // The object at +0x150 is a 679-entry array (one entry per technology in the database, so it is
    // indexed by the technology id). Decode it as u32 to see whether it holds a level per technology.
    nlohmann::json arr_probe = nlohmann::json::array();
    {
        void* data = nullptr;
        uint32_t cap = 0, sz = 0;
        if (Read((const void*)(ts + 0x150 + 0x08), &data) && Read((const void*)(ts + 0x150 + 0x10), &cap) &&
            Read((const void*)(ts + 0x150 + 0x14), &sz) && data && sz && sz < 100000) {
            nlohmann::json nz = nlohmann::json::array();
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < sz; ++i) {
                uint32_t v = 0;
                if (!Read((const void*)((uintptr_t)data + i * 4), &v)) break;
                if (!v) continue;
                ++nonzero;
                if (nz.size() < 200) {
                    nlohmann::json e = { {"i", i}, {"v", v} };
                    auto id_it = tech_ids.find(i);
                    if (id_it != tech_ids.end()) e["key"] = id_it->second;
                    nz.push_back(e);
                }
            }
            arr_probe.push_back({ {"offset", 0x150}, {"data", (uint64_t)(uintptr_t)data}, {"cap", cap},
                                  {"size", sz}, {"nonzero", nonzero}, {"entries", nz} });
        }
    }

    return { {"success", true}, {"country_ptr", (uint64_t)(uintptr_t)country},
             {"base_offset", offset}, {"base_ptr", (uint64_t)ts},
             {"tech_db_count", (uint64_t)tech_keys.size()},
             {"hit_count", (uint64_t)hits.size()}, {"hits", hits},
             {"id_hit_count", (uint64_t)id_hits.size()}, {"id_hits", id_hits},
             {"array_probe", arr_probe}, {"dump", hex} };
}

nlohmann::json GalaxyManager::ScanEdictArraysJson(int max_seconds) {
    if (max_seconds <= 0 || max_seconds > 20) max_seconds = 8;

    void* db = nullptr;
    if (!Read((const void*)(base_address_ + sdk::glob::TGameDatabase_CEdictsDatabase_pInstance), &db) || !db) {
        return { {"success", false}, {"error", "EdictsDatabase not found"} };
    }
    void* arr = nullptr;
    uint32_t cnt = 0;
    if (!Read((const void*)((uintptr_t)db + 0x50), &arr) || !arr ||
        !Read((const void*)((uintptr_t)db + 0x5C), &cnt) || cnt == 0) {
        return { {"success", false}, {"error", "edict array not readable"} };
    }

    std::unordered_set<uintptr_t> edict_set;
    std::unordered_map<uintptr_t, std::string> key_of;
    for (uint32_t i = 0; i < cnt && i < 4096; ++i) {
        void* p = nullptr;
        if (!Read((const void*)((uintptr_t)arr + i * 8), &p) || !p) continue;
        edict_set.insert((uintptr_t)p);
        key_of[(uintptr_t)p] = KeyOf((uintptr_t)p + 0x20, kTypeKey);
    }
    if (edict_set.empty()) return { {"success", false}, {"error", "no edict objects"} };

    nlohmann::json hits = nlohmann::json::array();
    const DWORD start = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0x10000;
    size_t regions = 0;
    bool stop = false;
    const size_t strides[7] = {8, 16, 24, 32, 40, 48, 64};

    while (!stop && VirtualQuery((const void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool writable = (prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool bad = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
        if (mbi.State == MEM_COMMIT && writable && !bad && size >= 64 && size <= (1024ull << 20)) {
            const size_t n = size / 8;
            if (n <= (1u << 21)) {
                std::vector<uint64_t> words(n);
                if (Read((const void*)base, words.data())) {
                    for (int si = 0; si < 7 && !stop; ++si) {
                        const size_t step = strides[si] / 8;
                        if (step < 1) continue;
                        size_t i = 0;
                        while (!stop && i + 2 * step < n) {
                            if (!edict_set.count((uintptr_t)words[i])) { ++i; continue; }
                            size_t run = 1;
                            while (i + run * step < n && edict_set.count((uintptr_t)words[i + run * step])) ++run;
                            if (run >= 2) {
                                nlohmann::json keys = nlohmann::json::array();
                                for (size_t k = 0; k < run && k < 16; ++k) keys.push_back(key_of[(uintptr_t)words[i + k * step]]);
                                hits.push_back({
                                    {"addr", (unsigned long long)(base + i * 8)},
                                    {"stride", (uint32_t)strides[si]},
                                    {"count", (uint32_t)run},
                                    {"keys", keys}
                                });
                                if (hits.size() >= 60) { stop = true; break; }
                                i += run * step;
                            } else {
                                ++i;
                            }
                        }
                    }
                }
            }
            regions++;
        }
        addr = base + size;
        if (addr == 0) break;
        if (GetTickCount() - start > (DWORD)max_seconds * 1000u) break;
    }
    return { {"success", true}, {"regions", (uint32_t)regions}, {"hits", hits} };
}
// 只读：在可写内存里找「哪个位置存着这个 8 字节值」（通常是对象指针）。
// 用途：反向找持有者——比如谁引用了某个遗址对象，从而定位那个没有挂在视图上的剧情窗。
nlohmann::json GalaxyManager::ScanMemoryForQword(uint64_t value, int max_seconds, uint64_t start_addr, uint64_t max_bytes) {
    if (max_seconds <= 0 || max_seconds > 30) max_seconds = 6;
    if (!value) return { {"success", false}, {"error", "value is 0"} };

    nlohmann::json hits = nlohmann::json::array();
    const size_t chunk = 1u << 20;
    std::vector<uint8_t> buf(chunk + 8);
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = start_addr ? (uintptr_t)start_addr : 0x10000;
    const uint64_t budget = max_bytes ? max_bytes : (400ull << 20);
    uint64_t scanned = 0;
    const DWORD start = GetTickCount();

    while (VirtualQuery((const void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool writable = (prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool bad = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
        if (mbi.State == MEM_COMMIT && writable && !bad && size >= 16 && base > 0x10000) {
            for (size_t off = 0; off < size; off += chunk) {
                const size_t len = (size - off < chunk) ? (size - off) : chunk;
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(base + off), buf.data(), len, &got) || got < 8) continue;
                const size_t real = (size_t)got;
                for (size_t i = 0; i + 8 <= real; i += 8) {
                    uint64_t v = 0;
                    memcpy(&v, buf.data() + i, 8);
                    if (v == value) {
                        hits.push_back((uint64_t)(base + off + i));
                        if (hits.size() >= 24) goto done_q;
                    }
                }
                scanned += len;
                if (scanned >= budget) goto done_q;
                if (GetTickCount() - start > (DWORD)max_seconds * 1000u) goto done_q;
            }
        }
        addr = base + size;
    }
done_q:
    return { {"success", true}, {"value", value}, {"scanned", scanned}, {"count", hits.size()}, {"hits", hits} };
}
nlohmann::json GalaxyManager::ScanMemoryForText(const std::string& needle, bool include_code, int max_seconds, uint64_t start_addr, uint64_t max_bytes) {
    /* 只读扫描：在已提交的可读内存区里找 needle 的 UTF-8 / UTF-16LE / UTF-32LE 副本。
       默认只扫可写区（堆），避免动到镜像；带时间上限，超时就停。 */
    if (needle.empty() || needle.size() > 64) {
        return { {"success", false}, {"error", "needle must be 1..64 bytes"} };
    }
    if (max_seconds <= 0 || max_seconds > 30) max_seconds = 6;

    std::vector<std::vector<uint8_t>> patterns;
    {
        std::vector<uint8_t> u8(needle.begin(), needle.end());
        std::vector<uint8_t> u16, u32;
        for (unsigned char c : u8) {
            u16.push_back(c); u16.push_back(0);
            u32.push_back(c); u32.push_back(0); u32.push_back(0); u32.push_back(0);
        }
        patterns.push_back(u8);
        patterns.push_back(u16);
        patterns.push_back(u32);
    }
    const char* kinds[3] = {"utf8", "utf16", "utf32"};

    const uintptr_t self_lo = (uintptr_t)&patterns;
    nlohmann::json hits = nlohmann::json::array();
    size_t scanned = 0;
    const DWORD start = GetTickCount();

    const size_t chunk = 1u << 20;  // 1 MB
    std::vector<uint8_t> buf(chunk + 64);

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = start_addr ? (uintptr_t)start_addr : 0x10000;
    const uint64_t budget = max_bytes ? max_bytes : (400ull << 20);
    uintptr_t last_addr = addr;
    while (VirtualQuery((const void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool writable = (prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool bad = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
        const bool take = mbi.State == MEM_COMMIT && readable && !bad && (include_code || writable);

        if (take && size >= 64) {
            for (size_t off = 0; off < size; off += chunk) {
                if (scanned >= budget) { last_addr = base + off; addr = last_addr; goto done_scan; }
                if (GetTickCount() - start > (DWORD)max_seconds * 1000u) {
                    addr = base + size;
                    goto done_scan;
                }
                const size_t want = std::min<size_t>(chunk, size - off);
                if (!ReadBytes((const uint8_t*)(base + off), buf.data(), want)) continue;
                scanned += want;
                for (int p = 0; p < 3; ++p) {
                    const auto& pat = patterns[p];
                    if (pat.size() > want) continue;
                    for (size_t i = 0; i + pat.size() <= want; ++i) {
                        if (buf[i] != pat[0]) continue;
                        if (std::memcmp(buf.data() + i, pat.data(), pat.size()) != 0) continue;
                        char item[128];
                        std::snprintf(item, sizeof(item), "0x%llX %s",
                                      (unsigned long long)(base + off + i), kinds[p]);
                        hits.push_back(item);
                        if (hits.size() >= 40) { last_addr = base + off; goto done_scan; }
                    }
                }
            }
        }
        if (size == 0) break;
        addr = base + size;
        last_addr = addr;
    }

done_scan:
    (void)self_lo;
    return { {"success", true}, {"needle", needle}, {"hits", hits}, {"next_addr", (unsigned long long)last_addr},
             {"scanned_mb", scanned / (1024 * 1024)},
             {"elapsed_ms", (int)(GetTickCount() - start)} };
}

nlohmann::json GalaxyManager::ReadMemoryAt(uint64_t address, uint32_t length) {
    /* 只读：把指定地址的一段内存按可打印 ASCII 输出，顺带给出十六进制。 */
    if (address < 0x10000ull) return { {"success", false}, {"error", "address too low"} };
    if (length == 0 || length > 0x2000) length = 0x200;

    std::vector<uint8_t> buf(length, 0);
    if (!ReadBytes((const void*)(uintptr_t)address, buf.data(), length)) {
        return { {"success", false}, {"error", "unreadable"} };
    }
    std::string printable;
    printable.reserve(length);
    for (uint8_t c : buf) printable.push_back((c >= 32 && c < 127) ? (char)c : '.');
    std::string hex;
    hex.reserve(length * 3);
    char pair[4];
    for (uint8_t c : buf) { std::snprintf(pair, sizeof(pair), "%02X ", c); hex += pair; }
    char addr_hex[32];
    std::snprintf(addr_hex, sizeof(addr_hex), "0x%llX", (unsigned long long)address);
    return { {"success", true}, {"address", addr_hex}, {"length", length},
             {"ascii", printable}, {"hex", hex} };
}

namespace {
/* ---- 岗位分配数据抓取（一次性 hook） ---- */
using JobUpdateFn = void (*)(void* data, const void* colonies);

std::atomic<bool> g_job_hook_on{ false };
std::atomic<int> g_job_calls{ 0 };
std::mutex g_job_mu;
void* g_job_target = nullptr;
JobUpdateFn g_job_orig = nullptr;

struct JobSample {
    uint64_t data = 0;
    uint64_t colonies = 0;
    std::string data_hex;
    std::string colonies_hex;
};
std::vector<JobSample> g_job_samples;

void DumpHexRange(uint64_t addr, size_t len, std::string* out) {
    std::vector<uint8_t> buf(len, 0);
    if (addr < 0x10000ull || !ReadBytes((const void*)(uintptr_t)addr, buf.data(), len)) {
        *out = "unreadable";
        return;
    }
    char pair[4];
    out->clear();
    out->reserve(len * 3);
    for (uint8_t c : buf) {
        std::snprintf(pair, sizeof(pair), "%02X ", c);
        out->append(pair);
    }
}

void JobUpdateDetour(void* data, const void* colonies) {
    if (g_job_hook_on.load(std::memory_order_relaxed) && g_job_calls.load(std::memory_order_relaxed) < 3) {
        g_job_calls.fetch_add(1);
        JobSample s;
        s.data = (uint64_t)(uintptr_t)data;
        s.colonies = (uint64_t)(uintptr_t)colonies;
        DumpHexRange(s.data, 0x200, &s.data_hex);
        DumpHexRange(s.colonies, 0x60, &s.colonies_hex);
        std::lock_guard<std::mutex> lock(g_job_mu);
        g_job_samples.push_back(std::move(s));
    }
    if (g_job_orig) g_job_orig(data, colonies);
}
}  // namespace

namespace {
bool SafeReadBytes(const void* addr, void* out, size_t n) {
    __try {
        std::memcpy(out, addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

nlohmann::json GalaxyManager::HookJobUpdate(bool enable) {
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };

    if (!enable) {
        g_job_hook_on = false;
        return { {"success", true}, {"on", false}, {"calls", g_job_calls.load()} };
    }

    if (!g_job_target) {
        g_job_target = (void*)(base_address_ + sdk::fn::NPlanetJobs_UpdatePopJobs);
        if (MH_CreateHook(g_job_target, (void*)&JobUpdateDetour, (void**)&g_job_orig) != MH_OK) {
            g_job_target = nullptr;
            return { {"success", false}, {"error", "MH_CreateHook failed"} };
        }
        if (MH_EnableHook(g_job_target) != MH_OK) {
            return { {"success", false}, {"error", "MH_EnableHook failed"} };
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        g_job_samples.clear();
    }
    g_job_calls = 0;
    g_job_hook_on = true;

    char target[32];
    std::snprintf(target, sizeof(target), "0x%llX", (unsigned long long)(uintptr_t)g_job_target);
    return { {"success", true}, {"on", true}, {"target", target},
             {"hint", "让时间走几天，然后 read_job_hook 取样本"} };
}

nlohmann::json GalaxyManager::ReadJobHook() {
    std::lock_guard<std::mutex> lock(g_job_mu);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& s : g_job_samples) {
        char d[32], c[32];
        std::snprintf(d, sizeof(d), "0x%llX", (unsigned long long)s.data);
        std::snprintf(c, sizeof(c), "0x%llX", (unsigned long long)s.colonies);
        rows.push_back({ {"data", d}, {"colonies", c},
                         {"data_hex", s.data_hex}, {"colonies_hex", s.colonies_hex} });
    }
    return { {"success", true}, {"count", g_job_samples.size()},
             {"calls", g_job_calls.load()}, {"samples", rows} };
}

namespace {
using ColonyUpdateFn = void (*)(void* data, void* colony);

std::atomic<bool> g_col_hook_on{ false };
std::atomic<int> g_col_calls{ 0 };
std::mutex g_col_mu;
void* g_col_target = nullptr;
ColonyUpdateFn g_col_orig = nullptr;

struct ColSample {
    uint64_t colony = 0;
    uint64_t data = 0;
    std::vector<std::string> hits;
};
std::vector<ColSample> g_col_samples;

void ScanFor(ColSample* s, uintptr_t region, size_t len, const char* label, int64_t value) {
    for (size_t off = 0; off + 8 <= len; off += 8) {
        int64_t v = 0;
        if (!ReadBytes((const void*)(region + off), &v, 8)) break;
        if (v != value) continue;
        char item[96];
        std::snprintf(item, sizeof(item), "%s+0x%zX", label, off);
        s->hits.push_back(item);
        if (s->hits.size() >= 60) return;
    }
}

void ColonyUpdateDetour(void* data, void* colony) {
    if (g_col_hook_on.load(std::memory_order_relaxed) && g_col_calls.load(std::memory_order_relaxed) < 6) {
        g_col_calls.fetch_add(1);
        ColSample s;
        s.colony = (uint64_t)(uintptr_t)colony;
        s.data = (uint64_t)(uintptr_t)data;
        if (colony) {
            ScanFor(&s, (uintptr_t)colony, 0x4000, "colony", 10000000);
            ScanFor(&s, (uintptr_t)colony, 0x4000, "colony", 100000);
        }
        std::lock_guard<std::mutex> lock(g_col_mu);
        g_col_samples.push_back(std::move(s));
    }
    if (g_col_orig) g_col_orig(data, colony);
}
}  // namespace

nlohmann::json GalaxyManager::HookColonyUpdate(bool enable) {
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    if (!enable) {
        g_col_hook_on = false;
        return { {"success", true}, {"on", false}, {"calls", g_col_calls.load()} };
    }
    if (!g_col_target) {
        g_col_target = (void*)(base_address_ + sdk::fn::NColonyUpdate_MonthlyOrDailyUpdatePopJobsInColony);
        if (MH_CreateHook(g_col_target, (void*)&ColonyUpdateDetour, (void**)&g_col_orig) != MH_OK) {
            g_col_target = nullptr;
            return { {"success", false}, {"error", "MH_CreateHook failed"} };
        }
        if (MH_EnableHook(g_col_target) != MH_OK) {
            return { {"success", false}, {"error", "MH_EnableHook failed"} };
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_col_mu);
        g_col_samples.clear();
    }
    g_col_calls = 0;
    g_col_hook_on = true;
    char target[32];
    std::snprintf(target, sizeof(target), "0x%llX", (unsigned long long)(uintptr_t)g_col_target);
    return { {"success", true}, {"on", true}, {"target", target} };
}

nlohmann::json GalaxyManager::ReadColonyHook() {
    std::lock_guard<std::mutex> lock(g_col_mu);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& s : g_col_samples) {
        char c[32], d[32];
        std::snprintf(c, sizeof(c), "0x%llX", (unsigned long long)s.colony);
        std::snprintf(d, sizeof(d), "0x%llX", (unsigned long long)s.data);
        rows.push_back({ {"colony", c}, {"data", d}, {"hits", s.hits} });
    }
    return { {"success", true}, {"count", g_col_samples.size()}, {"calls", g_col_calls.load()}, {"samples", rows} };
}

namespace {
using MoveSizeFn = void* (*)(void* colony, const void* key, void* src, int amount, void* extra);
using AddSizeFn = void* (*)(void* colony, const void* key, int amount, void* extra);

std::atomic<bool> g_move_on{ false };
std::atomic<int> g_move_calls{ 0 };
std::mutex g_move_mu;
void* g_move_target = nullptr;
void* g_add_target = nullptr;
MoveSizeFn g_move_orig = nullptr;
AddSizeFn g_add_orig = nullptr;

struct MoveSample {
    std::string which;
    uint64_t colony = 0;
    uint64_t key = 0;
    int amount = 0;
    std::string key_hex;
};
std::vector<MoveSample> g_move_samples;

void CaptureKey(MoveSample* s, const void* key) {
    uint64_t k = (uint64_t)(uintptr_t)key;
    s->key = k;
    if (k < 0x10000ull) return;
    std::vector<uint8_t> buf(0x80, 0);
    for (size_t i = 0; i < buf.size(); i += 0x40) {
        SafeReadBytes((const void*)(k + i), buf.data() + i, 0x40);
    }
    char pair[4];
    s->key_hex.reserve(buf.size() * 3);
    for (uint8_t c : buf) {
        std::snprintf(pair, sizeof(pair), "%02X ", c);
        s->key_hex.append(pair);
    }
}

void* MoveSizeDetour(void* colony, const void* key, void* src, int amount, void* extra) {
    if (g_move_on.load(std::memory_order_relaxed) && g_move_calls.load(std::memory_order_relaxed) < 20) {
        g_move_calls.fetch_add(1);
        MoveSample s;
        s.which = "move";
        s.colony = (uint64_t)(uintptr_t)colony;
        s.amount = amount;
        CaptureKey(&s, key);
        std::lock_guard<std::mutex> lock(g_move_mu);
        g_move_samples.push_back(std::move(s));
    }
    return g_move_orig ? g_move_orig(colony, key, src, amount, extra) : nullptr;
}

void* AddSizeDetour(void* colony, const void* key, int amount, void* extra) {
    if (g_move_on.load(std::memory_order_relaxed) && g_move_calls.load(std::memory_order_relaxed) < 20) {
        g_move_calls.fetch_add(1);
        MoveSample s;
        s.which = "add";
        s.colony = (uint64_t)(uintptr_t)colony;
        s.amount = amount;
        CaptureKey(&s, key);
        std::lock_guard<std::mutex> lock(g_move_mu);
        g_move_samples.push_back(std::move(s));
    }
    return g_add_orig ? g_add_orig(colony, key, amount, extra) : nullptr;
}
}  // namespace

nlohmann::json GalaxyManager::HookPopGroupSize(bool enable) {
    if (!base_address_) return { {"success", false}, {"error", "base not initialized"} };
    if (!enable) {
        g_move_on = false;
        return { {"success", true}, {"on", false}, {"calls", g_move_calls.load()} };
    }
    if (!g_move_target) {
        g_move_target = (void*)(base_address_ + sdk::fn::CColony_MoveSizeToPopGroup);
        g_add_target = (void*)(base_address_ + sdk::fn::CColony_AddSizeToPopGroup);
        if (MH_CreateHook(g_move_target, (void*)&MoveSizeDetour, (void**)&g_move_orig) != MH_OK) {
            return { {"success", false}, {"error", "MH_CreateHook(move) failed"} };
        }
        if (MH_CreateHook(g_add_target, (void*)&AddSizeDetour, (void**)&g_add_orig) != MH_OK) {
            return { {"success", false}, {"error", "MH_CreateHook(add) failed"} };
        }
        if (MH_EnableHook(g_move_target) != MH_OK || MH_EnableHook(g_add_target) != MH_OK) {
            return { {"success", false}, {"error", "MH_EnableHook failed"} };
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_move_mu);
        g_move_samples.clear();
    }
    g_move_calls = 0;
    g_move_on = true;
    return { {"success", true}, {"on", true} };
}

nlohmann::json GalaxyManager::ReadPopGroupSizeHook() {
    std::lock_guard<std::mutex> lock(g_move_mu);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& s : g_move_samples) {
        char c[32], k[32];
        std::snprintf(c, sizeof(c), "0x%llX", (unsigned long long)s.colony);
        std::snprintf(k, sizeof(k), "0x%llX", (unsigned long long)s.key);
        rows.push_back({ {"which", s.which}, {"colony", c}, {"key", k},
                         {"amount", s.amount}, {"key_hex", s.key_hex} });
    }
    return { {"success", true}, {"count", g_move_samples.size()},
             {"calls", g_move_calls.load()}, {"samples", rows} };
}

namespace {
std::unordered_map<uint64_t, uint32_t> g_snapshot;   // value -> count

}  // namespace

namespace {
std::unordered_map<uint64_t, uint32_t> g_snap_a;   // 基准
std::unordered_map<uint64_t, uint32_t> g_snap_b;   // 对比

// 从 start 起扫一段（受时间预算限制），把指向静态区的 qword 计数累加进 out
}  // namespace

nlohmann::json GalaxyManager::ScanMemoryForU64Range(uint64_t lo, uint64_t hi, int max_seconds,
                                                   uint64_t start_addr, uint64_t max_bytes) {
    /* 只读：在已提交可写区里找落在 [lo, hi) 的 8 字节值。分页返回 next_addr。 */
    if (max_seconds <= 0 || max_seconds > 30) max_seconds = 2;

    nlohmann::json hits = nlohmann::json::array();
    size_t scanned = 0;
    const DWORD started = GetTickCount();

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = start_addr ? (uintptr_t)start_addr : 0x10000;
    const uint64_t budget = max_bytes ? max_bytes : (300ull << 20);
    uintptr_t last_addr = addr;

    const size_t chunk = 1u << 20;
    std::vector<uint8_t> buf(chunk + 8);

    while (VirtualQuery((const void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
        const bool writable = (prot & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
        const bool bad = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
        const bool take = mbi.State == MEM_COMMIT && readable && !bad && writable;

        if (take && size >= 8) {
            for (size_t off = 0; off < size; off += chunk) {
                if (scanned >= budget || GetTickCount() - started > (DWORD)max_seconds * 1000u) {
                    last_addr = base + off;
                    addr = last_addr;
                    goto done_scan;
                }
                const size_t want = std::min<size_t>(chunk, size - off);
                if (!ReadBytes((const uint8_t*)(base + off), buf.data(), want)) continue;
                scanned += want;
                for (size_t i = 0; i + 8 <= want; i += 8) {
                    uint64_t v = 0;
                    std::memcpy(&v, buf.data() + i, 8);
                    if (v < lo || v >= hi) continue;
                    char item[96];
                    std::snprintf(item, sizeof(item), "0x%llX:0x%llX",
                                  (unsigned long long)(base + off + i), (unsigned long long)v);
                    hits.push_back(item);
                    if (hits.size() >= 4000) {
                        last_addr = base + off + i + 8;
                        addr = last_addr;
                        goto done_scan;
                    }
                }
            }
        }
        addr = base + size;
        last_addr = addr;
    }

done_scan:
    return { {"hits", hits}, {"scanned_bytes", (uint64_t)scanned}, {"next_addr", (uint64_t)last_addr} };
}

nlohmann::json GalaxyManager::GetFleetHomeBase(uint32_t fleet_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"success", false}, {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };

    uint32_t template_id = fleets::TemplateId(fleet);
    if (template_id == 0xFFFFFFFFu) return { {"success", false}, {"error", "This fleet has no fleet template"} };

    void* tmpl = nullptr;
    {
        void* db = nullptr; void* arr = nullptr; uint32_t cap = 0; uint32_t check = 0xFFFFFFFF;
        if (!(Read((const void*)(base_address_ + sdk::db::CFleetTemplate), &db) && db &&
              Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
              Read((const void*)((uintptr_t)db + 0x20), &cap) && (template_id & 0xFFFFFF) < cap &&
              Read((const void*)((uintptr_t)arr + (template_id & 0xFFFFFF) * 16 + 8), &tmpl) && tmpl &&
              Read((const void*)((uintptr_t)tmpl + sdk::rt::CFleetTemplate_id), &check) && check == template_id)) {
            return { {"success", false}, {"error", "Template " + std::to_string(template_id) + " not found"} };
        }
    }

    /* home_base 是 ORBITABLE 联合体：高 32 位 kind（1=行星 2=星港），低 32 位 id。
       实测位置在模板 +0x70（SDK 标的 0x68 是错的，那里是个共享常量）。 */
    uint64_t home_raw = 0;
    if (!Read((const void*)((uintptr_t)tmpl + 0x70), &home_raw)) {
        return { {"success", false}, {"error", "Could not read home_base"} };
    }

    /* 探针：把 0x60..0xA0 之间的 8 字节字都读出来，看哪个字随母港变化 */
    nlohmann::json words = nlohmann::json::object();
    for (int off = 0x60; off <= 0xA0; off += 8) {
        uint64_t v = 0;
        if (Read((const void*)((uintptr_t)tmpl + off), &v)) {
            char key[16];
            std::snprintf(key, sizeof(key), "0x%02X", off);
            words[key] = v;
        }
    }
    char hexbuf[32];
    std::snprintf(hexbuf, sizeof(hexbuf), "0x%llX", (unsigned long long)home_raw);

    nlohmann::json out = { {"success", true}, {"fleet_id", fleet_id}, {"fleet", fleets::Name(fleet)},
                           {"template_id", template_id}, {"home_pointer", hexbuf}, {"words", words} };

    const uint32_t kind = (uint32_t)(home_raw >> 32);
    const uint32_t home_id = (uint32_t)(home_raw & 0xFFFFFFFFu);
    out["kind"] = kind;                       /* 1 = 行星, 2 = 星港 */
    out["home_id"] = home_id;
    out["home_kind_name"] = kind == 1 ? "planet" : (kind == 2 ? "starbase" : "none");
    return out;
}

nlohmann::json GalaxyManager::GetStarbasePointers() {
    /* 只读探针：把 db::CStarbase 里的每一项按 id 列出来，附带对象地址与 station 字段。
       用来确认“星港 id ↔ CStarbase 指针”的对应关系，再决定要不要写命令。 */
    nlohmann::json list = nlohmann::json::array();
    void* db = nullptr; void* arr = nullptr; uint32_t cap = 0;
    if (!(Read((const void*)(base_address_ + sdk::db::CStarbase), &db) && db &&
          Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
          Read((const void*)((uintptr_t)db + 0x20), &cap))) {
        return { {"success", false}, {"error", "Could not open db::CStarbase"} };
    }
    if (cap > 20000) cap = 20000;
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        uint32_t id = 0xFFFFFFFF;
        if (!Read((const void*)((uintptr_t)arr + i * 16 + 8), &obj) || !obj) continue;
        if (!Read((const void*)((uintptr_t)obj + sdk::rt::CStarbase_id), &id)) continue;
        char hexbuf[32];
        std::snprintf(hexbuf, sizeof(hexbuf), "0x%llX", (unsigned long long)(uintptr_t)obj);
        nlohmann::json item = { {"index", i}, {"id", id}, {"pointer", hexbuf} };
        uint32_t station = 0xFFFFFFFF;
        if (Read((const void*)((uintptr_t)obj + sdk::ent::CStarbase::station), &station)) {
            item["station_raw"] = station;
        }
        list.push_back(item);
    }
    return { {"success", true}, {"capacity", cap}, {"count", list.size()}, {"starbases", list} };
}

/* 一座恒星基地的完整状态：等级 + 已装模块 + 已装建筑（键名 + 本地化名 + 槽号）。

   模块/建筑是两个 {ptr array, size, capacity} 向量，挂在 CStarbase 对象上
   （2026-11-17 用“建一个模块前后各 dump 一次”对比出来的）：
     +0x5E0 modules   数据 / +0x5E8 size、+0x5EC capacity   （CStarbaseModuleType*）
     +0x5F8 buildings 数据 / +0x600 size、+0x604 capacity   （CStarbaseBuildingType*）
   数组只装**已建成**的，正在队列里的不在里面；槽号就是数组下标。
   类型对象的键是 +0x20 处的 PdxString；本地化要加 "sm_" 前缀
   （sm_anchorage → 锚地、sm_solar_panel_network → 太阳能电池板、sm_crew_quarters → 船员居住舱）。
   槽位上限来自 common/starbase_levels（星港 module_slots={1 2}、building_slots={1}），
   不读内存——等级对象里那几个向量的布局还没吃透。 */
/* 玩家帝国的恒星基地一览（对应殖民地的 get_colonies 概览）。
   判归属的方式：站体 CShip → 坐标 origin = 星系 id → 星系对象的 owner = 玩家。
   2026-11-18 03:15 实测，监牢/格索拉都靠这条链判出来。 */
nlohmann::json GalaxyManager::GetStarbaseOverviewJson() {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "not in game"} };

    void* mgr = nullptr;
    Read((const void*)(base_address_ + sdk::db::CStarbase), &mgr);
    if (!mgr) return { {"success", false}, {"error", "CStarbase database not found"} };
    void* arr = ReadOr<void*>((uintptr_t)mgr + 0x18, nullptr);
    uint32_t cap = ReadOr<uint32_t>((uintptr_t)mgr + 0x20, 0);
    if (!arr || cap == 0) return { {"success", false}, {"error", "CStarbase table unreadable"} };

    void* ship_mgr = nullptr;
    void* ship_arr = nullptr;
    uint32_t ship_cap = 0;
    if (Read((const void*)(base_address_ + sdk::db::CShip), &ship_mgr) && ship_mgr) {
        ship_arr = ReadOr<void*>((uintptr_t)ship_mgr + 0x18, nullptr);
        ship_cap = ReadOr<uint32_t>((uintptr_t)ship_mgr + 0x20, 0);
    }

    nlohmann::json out = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* sb = ReadOr<void*>((uintptr_t)arr + i * 16 + 8, nullptr);
        if (!sb) continue;
        const uint32_t station = ReadOr<uint32_t>((uintptr_t)sb + sdk::ent::CStarbase::station, 0xFFFFFFFF);
        if (station == 0xFFFFFFFF || !ship_arr) continue;
        const uint32_t slot = station & 0xFFFFFF;
        if (slot >= ship_cap) continue;
        void* ship = ReadOr<void*>((uintptr_t)ship_arr + slot * 16 + 8, nullptr);
        if (!ship) continue;

        const uint32_t system_id = ReadOr<uint32_t>((uintptr_t)ship + 0x9C0 + 0x20, 0xFFFFFFFF);
        if (system_id == 0xFFFFFFFF) continue;
        void* sys = RefLookup(base_address_, sdk::db::CGalacticObject, system_id);
        if (!sys) continue;
        const uint32_t owner = ReadOr<uint32_t>((uintptr_t)sys + sdk::rt::CGalacticObject_owner, 0xFFFFFFFF);
        if (owner != s.player_id) continue;

        const uint32_t id = ReadOr<uint32_t>((uintptr_t)sb + 8, 0xFFFFFFFF);
        const uint32_t design = ReadOr<uint32_t>((uintptr_t)ship + 0x988, 0xFFFFFFFF);
        const std::string level = KeyOf((uintptr_t)sb + sdk::ent::CStarbase::level, kTypeKey);
        std::string level_name;
        void* level_type = nullptr;
        if (Read((const void*)((uintptr_t)sb + sdk::ent::CStarbase::level), &level_type) && level_type) {
            std::ptrdiff_t ship_size = LevelShipSizeField();
            if (ship_size >= 0) {
                std::string size_key = KeyOf((uintptr_t)level_type + ship_size, kTypeKey);
                if (!size_key.empty()) level_name = SafeLocalize(base_address_, size_key);
            }
        }

        uint32_t mod_n = ReadOr<uint32_t>((uintptr_t)sb + 0x5E8, 0);
        uint32_t bld_n = ReadOr<uint32_t>((uintptr_t)sb + 0x600, 0);
        uint32_t mod_cap = 0, bld_cap = 0;
        if (level_type) {
            // 条数在 +0xDC / +0xF4（+0xD8 / +0xF0 是容量，方舟上两者不等）
            mod_cap = ReadOr<uint32_t>((uintptr_t)level_type + 0xDC, 0);
            bld_cap = ReadOr<uint32_t>((uintptr_t)level_type + 0xF4, 0);
        }
        const std::string sys_name = PersistentNameText((const void*)((uintptr_t)sys + sdk::ent::CGalacticObject::name));

        out.push_back({ {"starbase_id", id},
                        {"level", level},
                        {"level_name", level_name},
                        {"system_id", system_id},
                        {"system_name", sys_name},
                        {"module_count", mod_n},
                        {"module_slots", mod_cap},
                        {"building_count", bld_n},
                        {"building_slots", bld_cap},
                        {"station_design_id", design} });
    }
    return { {"success", true}, {"count", out.size()}, {"starbases", out} };
}

/* 我们自己的恒星基地，分三组（2026-11-23）：方舟 / 驿站 / 常规。
 *
 * 游牧没有「拥有星系」这回事：驿站占着星系却不把 owner 改成我们（只是允许在里面造巨构），
 * 所以 GetStarbaseOverviewJson 那条「星系 owner == 玩家」的判据在游牧档上恒为 0 座。
 * 这里另开两条权威来源：
 *   方舟 —— 本国殖民地的 carrier{type=1, ship} → 在 CStarbase 表里找 station & 0xFFFFFF == 舰 id
 *   驿站 —— CCountry + 0x2E68 的 waystation_networks（ref_array，元素 4 字节 id）
 *           → 全局 CWaystationNetwork 库（base + 0x311BDC8）→ 网络对象 + 0x330 的 starbase 列表
 * 常规基地仍走原来那条判据（定居帝国用得上）。
 */
nlohmann::json GalaxyManager::GetOurStarbasesJson() {
    Snapshot s = Take();
    if (!s.player) return { {"success", false}, {"error", "not in game"} };
    void* country = s.player;

    // 一行 = 一个 CStarbase 对象；槽位口径与 get_starbase_details 一致（条数在 +0xDC / +0xF4）
    auto row = [&](uint32_t sb_id, void* sb) -> nlohmann::json {
        nlohmann::json j = { {"starbase_id", sb_id} };
        void* level_type = nullptr;
        Read((const void*)((uintptr_t)sb + sdk::ent::CStarbase::level), &level_type);
        const std::string level = KeyOf((uintptr_t)sb + sdk::ent::CStarbase::level, kTypeKey);
        j["level"] = level;
        if (level_type) {
            std::ptrdiff_t ship_size = LevelShipSizeField();
            if (ship_size >= 0) {
                std::string size_key = KeyOf((uintptr_t)level_type + ship_size, kTypeKey);
                if (!size_key.empty()) j["level_name"] = SafeLocalize(base_address_, size_key);
            }
            j["module_slots"] = ReadOr<uint32_t>((uintptr_t)level_type + 0xDC, 0);
            j["building_slots"] = ReadOr<uint32_t>((uintptr_t)level_type + 0xF4, 0);
        }
        // 已装数量要数「键不为空」的条目：已装数组可能带着空占位（方舟上 size 9 / 槽 7），
        // 直接印 size 会多出幽灵槽。
        auto countInstalled = [&](std::ptrdiff_t data_off, std::ptrdiff_t size_off, uint32_t slots) {
            void* data = ReadOr<void*>((uintptr_t)sb + data_off, nullptr);
            uint32_t size = ReadOr<uint32_t>((uintptr_t)sb + size_off, 0);
            uint32_t n = 0;
            for (uint32_t i = 0; i < size && data && (slots == 0 || i < slots); ++i) {
                void* type = ReadOr<void*>((uintptr_t)data + i * sizeof(void*), nullptr);
                if (!type || ReadString((uintptr_t)type + kTypeKey).empty()) continue;
                ++n;
            }
            return n;
        };
        j["module_count"] = countInstalled(0x5E0, 0x5E8, ReadOr<uint32_t>((uintptr_t)level_type + 0xDC, 0));
        j["building_count"] = countInstalled(0x5F8, 0x600, ReadOr<uint32_t>((uintptr_t)level_type + 0xF4, 0));
        const uint32_t station = ReadOr<uint32_t>((uintptr_t)sb + sdk::ent::CStarbase::station, 0xFFFFFFFF);
        if (station != 0xFFFFFFFF) {
            void* ship = RefLookup(base_address_, sdk::db::CShip, station & 0xFFFFFF);
            if (ship) {
                j["station_ship_id"] = station & 0xFFFFFF;
                j["name"] = PersistentNameText((const void*)((uintptr_t)ship + sdk::ent::CShip::name));
                j["design_id"] = ReadOr<uint32_t>((uintptr_t)ship + 0x988, 0xFFFFFFFF);
                const uint32_t sys_id = ReadOr<uint32_t>((uintptr_t)ship + 0x9C0 + 0x20, 0xFFFFFFFF);
                if (sys_id != 0xFFFFFFFF) {
                    j["system_id"] = sys_id;
                    void* sys = RefLookup(base_address_, sdk::db::CGalacticObject, sys_id);
                    if (sys) j["system_name"] = SystemName(sys_id, sys);
                }
            }
        }
        return j;
    };

    nlohmann::json arks = nlohmann::json::array();
    nlohmann::json waystations = nlohmann::json::array();

    // 1) 方舟：殖民地 carrier 是舰的那些（carrier 高 32 位 0 = 行星，1 = 舰）
    std::map<uint32_t, uint32_t> ark_ships;  // ship id → colony id
    {
        void* data = nullptr;
        uint32_t cnt = 0;
        Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x8), &data);
        Read((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + 0x14), &cnt);
        for (uint32_t i = 0; i < cnt && i < 256; ++i) {
            uint32_t cid = 0;
            if (!Read((const void*)((uintptr_t)data + i * 4), &cid)) continue;
            void* colony = RefLookup(base_address_, sdk::db::CColony, cid);
            if (!colony) continue;
            uint64_t carrier = 0;
            Read((const void*)((uintptr_t)colony + sdk::ent::CColony::carrier), &carrier);
            if ((uint32_t)(carrier >> 32) != 1) continue;
            ark_ships[(uint32_t)(carrier & 0xFFFFFFull)] = cid;
        }
    }
    // station 可能带 0x04000000 标记（tier 2 的方舟见过），所以按低 24 位比
    {
        void* mgr = nullptr;
        if (Read((const void*)(base_address_ + sdk::db::CStarbase), &mgr) && mgr) {
            void* arr = ReadOr<void*>((uintptr_t)mgr + 0x18, nullptr);
            uint32_t cap = ReadOr<uint32_t>((uintptr_t)mgr + 0x20, 0);
            for (uint32_t i = 0; i < cap && arr; ++i) {
                void* sb = ReadOr<void*>((uintptr_t)arr + i * 16 + 8, nullptr);
                if (!sb) continue;
                const uint32_t station = ReadOr<uint32_t>((uintptr_t)sb + sdk::ent::CStarbase::station, 0xFFFFFFFF);
                if (station == 0xFFFFFFFF) continue;
                auto hit = ark_ships.find(station & 0xFFFFFF);
                if (hit == ark_ships.end()) continue;
                nlohmann::json j = row(ReadOr<uint32_t>((uintptr_t)sb + 8, 0xFFFFFFFF), sb);
                j["group"] = "ark";
                j["colony_id"] = hit->second;
                arks.push_back(j);
            }
        }
    }

    // 2) 驿站：country + 0x2E68 的 waystation_networks → 网络 +0x330 的 starbase 列表
    {
        void* nets = ReadOr<void*>((uintptr_t)country + 0x2E68 + 0x08, nullptr);
        uint32_t net_cnt = ReadOr<uint32_t>((uintptr_t)country + 0x2E68 + 0x14, 0);
        for (uint32_t n = 0; n < net_cnt && n < 256 && nets; ++n) {
            uint32_t net_raw = 0;
            if (!Read((const void*)((uintptr_t)nets + n * 4), &net_raw)) continue;
            const uint32_t net_id = net_raw & 0xFFFFFF;
            void* net = RefLookup(base_address_, sdk::db::CWaystationNetwork, net_id);
            if (!net) continue;
            void* wdata = ReadOr<void*>((uintptr_t)net + 0x330 + 0x08, nullptr);
            uint32_t wcnt = ReadOr<uint32_t>((uintptr_t)net + 0x330 + 0x14, 0);
            for (uint32_t w = 0; w < wcnt && w < 512 && wdata; ++w) {
                uint32_t raw = 0;
                if (!Read((const void*)((uintptr_t)wdata + w * 4), &raw)) continue;
                const uint32_t sb_id = raw & 0xFFFFFF;
                void* sb = RefLookup(base_address_, sdk::db::CStarbase, sb_id);
                if (!sb) continue;
                // 报**对象自己的 id**（+0x08）：表里有些基地的 id 带 0x01000000 标记（144 → 16777360），
                // 报掩码后的 144 会让按 id 扫描的方法（升级/队列）找不到它。
                nlohmann::json j = row(ReadOr<uint32_t>((uintptr_t)sb + 8, sb_id), sb);
                j["group"] = "waystation";
                j["network_id"] = net_id;
                waystations.push_back(j);
            }
        }
    }

    // 3) 常规基地（定居帝国）：沿用星系归属判据
    nlohmann::json bases = nlohmann::json::array();
    {
        nlohmann::json overview = GetStarbaseOverviewJson();
        if (overview.contains("starbases") && overview["starbases"].is_array()) {
            for (auto& b : overview["starbases"]) {
                // 第 3 组按「星系归属」列，会把停在我方星系里的别国方舟 / 驿港 / 主宰也带进来，
                // 所以按等级键把它们剔掉：方舟与驿港各有自己的分组（第 1、2 组），主宰是舰不是基地。
                const std::string lv = b.value("level", std::string());
                if (lv.find("arkship") != std::string::npos || lv.find("waystation") != std::string::npos ||
                    lv.find("juggernaut") != std::string::npos)
                    continue;
                b["group"] = "starbase";
                bases.push_back(b);
            }
        }
    }

    return { {"success", true},
             {"country_id", s.player_id},
             {"ark_count", arks.size()},
             {"waystation_count", waystations.size()},
             {"starbase_count", bases.size()},
             {"arks", arks},
             {"waystations", waystations},
             {"starbases", bases} };
}

nlohmann::json GalaxyManager::GetStarbaseDetails(uint32_t starbase_id) {
    if (!base_address_) return { {"success", false}, {"error", "not initialized"} };
    void* sb = RefLookup(base_address_, sdk::db::CStarbase, starbase_id);
    if (!sb) return { {"success", false}, {"error", "starbase " + std::to_string(starbase_id) + " not found"} };

    std::string level = KeyOf((uintptr_t)sb + sdk::ent::CStarbase::level, kTypeKey);
    std::string size_key;
    void* level_type = nullptr;
    std::ptrdiff_t ship_size = LevelShipSizeField();
    if (ship_size >= 0 &&
        Read((const void*)((uintptr_t)sb + sdk::ent::CStarbase::level), &level_type) && level_type) {
        size_key = KeyOf((uintptr_t)level_type + ship_size, kTypeKey);
    }

    // 格式塔/蜂群用的是同名变体键（sm_crew_quarters_machine_intelligence → 服务电缆、
    // sm_crew_quarters_hive_mind → 船员产育舱）。权威类型决定用哪套后缀，取不到就退回通用键。
    std::string variant;
    {
        std::string authority = GovernmentManager::Get().GetGovernmentState().authority;
        if (authority == "auth_machine_intelligence") variant = "_machine_intelligence";
        else if (authority == "auth_hive_mind") variant = "_hive_mind";
    }
    auto localize_sm = [&](const std::string& key) {
        const std::string base_key = "sm_" + key;
        if (!variant.empty()) {
            const std::string variant_key = base_key + variant;
            std::string text = SafeLocalize(base_address_, variant_key);
            if (!text.empty() && text != variant_key) return text;
            // 另一套后缀：_gestalt（trading_hub → 资源再处理器）
            const std::string gestalt_key = base_key + "_gestalt";
            std::string gestalt = SafeLocalize(base_address_, gestalt_key);
            if (!gestalt.empty() && gestalt != gestalt_key) return gestalt;
        }
        return SafeLocalize(base_address_, base_key);
    };

    /* 槽位上限从**等级对象**读（2026-11-17 22:45 实测；2026-11-23 修正偏移）：
         等级 + 0xC8 模块槽向量 {vtable, data +0xD0, capacity +0xD8, size +0xDC}
         等级 + 0xE0 建筑槽向量 {vtable, data +0xE8, capacity +0xF0, size +0xF4}
       **条数在 +0xDC / +0xF4，不是 +0xD8 / +0xF0（那是容量）**。
       常规基地两者相等，所以以前没露；方舟/驿站上容量大于条数，照容量读会多出幽灵槽
       （科研方舟 III：模块 9/7、建筑 13/12；军事方舟 II：建筑 9/8）。 */
    int module_slots = -1;
    int building_slots = -1;
    if (level_type) {
        uint32_t n = 0;
        if (Read((const void*)((uintptr_t)level_type + 0xDC), &n) && n <= 64) module_slots = (int)n;
        if (Read((const void*)((uintptr_t)level_type + 0xF4), &n) && n <= 64) building_slots = (int)n;
    }
    if (module_slots < 0 || building_slots < 0) {  // 等级对象读不到时退回游戏数据里的值
        if (level == "starbase_level_outpost") { module_slots = 0; building_slots = 0; }
        else if (level == "starbase_level_starport") { module_slots = 2; building_slots = 1; }
        else if (level == "starbase_level_starhold") { module_slots = 4; building_slots = 2; }
        else if (level == "starbase_level_starfortress") { module_slots = 6; building_slots = 3; }
        else if (level == "starbase_level_citadel") { module_slots = 6; building_slots = 4; }  // 数据里是 6/4，不是 8/4
        else if (level == "starbase_level_waystation_1") { module_slots = 2; building_slots = 0; }
        else if (level == "starbase_level_waystation_2") { module_slots = 4; building_slots = 0; }
        else if (level == "starbase_level_waystation_3") { module_slots = 6; building_slots = 0; }
        else if (level.rfind("starbase_level_", 0) == 0 && level.find("arkship_1") != std::string::npos) { module_slots = 2; building_slots = 4; }
        else if (level.rfind("starbase_level_", 0) == 0 && level.find("arkship_2") != std::string::npos) { module_slots = 4; building_slots = 8; }
        else if (level.rfind("starbase_level_", 0) == 0 && level.find("arkship_3") != std::string::npos) { module_slots = 7; building_slots = 12; }
    }

    /* 数组是**按槽位索引**的，可以稀疏：2026-11-17 22:35 实测，监牢（星垒 4 槽）
       装了 0/1 两槽后直接装第 4 格（slot 3），数组 size 变成 4，下标 2 是空指针。
       所以这里按「槽位上限」逐个输出，空位 key 为空字符串，不能跳过。 */
    auto list = [&](std::ptrdiff_t data_off, std::ptrdiff_t size_off, int limit) {
        nlohmann::json out = nlohmann::json::array();
        void* data = nullptr;
        uint32_t size = 0;
        Read((const void*)((uintptr_t)sb + data_off), &data);
        Read((const void*)((uintptr_t)sb + size_off), &size);
        uint32_t count = size;
        // 槽位上限就是显示上限：已装数组里可能残留比槽位更多的条目（方舟上见过 size 9 / 槽 7），
        // 照 size 印会多出幽灵槽。
        if (limit > 0) count = (uint32_t)limit;
        if (count > 64) count = 64;
        for (uint32_t i = 0; i < count; ++i) {
            void* type = nullptr;
            std::string key;
            if (data && i < size) {
                Read((const void*)((uintptr_t)data + i * sizeof(void*)), &type);
                if (type) key = ReadString((uintptr_t)type + kTypeKey);
            }
            nlohmann::json item = { {"slot", i}, {"key", key} };
            if (!key.empty()) item["name"] = localize_sm(key);
            out.push_back(item);
        }
        return out;
    };

    nlohmann::json modules = list(0x5E0, 0x5E8, module_slots);
    nlohmann::json buildings = list(0x5F8, 0x600, building_slots);

    auto filled = [](const nlohmann::json& arr) {
        int n = 0;
        for (const auto& it : arr) {
            if (!it.value("key", std::string()).empty()) ++n;
        }
        return n;
    };

    // 站体（CShip）的设计号：CStarbase::station(+0x98) 存的是站体 id，低 24 位就是 CShip 表下标。
    // 2026-11-18 03:05 实测：监牢站体 id=0（表下标 0）、格索拉 id=1188（下标 1188），与 find_ships 的 slot 一致。
    uint32_t station_ref = ReadOr<uint32_t>((uintptr_t)sb + sdk::ent::CStarbase::station, 0xFFFFFFFF);
    std::string station_name;  // 站体持久名（“监牢-A空间站”那种，CShip +0x110）
    uint32_t station_design = 0xFFFFFFFF;
    if (station_ref != 0xFFFFFFFF) {
        void* ship_mgr = nullptr;
        if (Read((const void*)(base_address_ + sdk::db::CShip), &ship_mgr) && ship_mgr) {
            void* ship_arr = ReadOr<void*>((uintptr_t)ship_mgr + 0x18, nullptr);
            uint32_t ship_cap = ReadOr<uint32_t>((uintptr_t)ship_mgr + 0x20, 0);
            const uint32_t slot = station_ref & 0xFFFFFF;
            if (ship_arr && slot < ship_cap) {
                void* ship = ReadOr<void*>((uintptr_t)ship_arr + slot * 16 + 8, nullptr);
                if (ship) {
                    station_design = ReadOr<uint32_t>((uintptr_t)ship + 0x988, 0xFFFFFFFF);
                    station_name = PersistentNameText((const void*)((uintptr_t)ship + sdk::ent::CShip::name));
                }
            }
        }
    }

    return { {"success", true},
             {"starbase_id", starbase_id},
             {"level", level},
             {"level_name", size_key.empty() ? std::string() : SafeLocalize(base_address_, size_key)},
             {"station_name", station_name},
             {"station_design_id", station_design},
             {"module_slots", module_slots},
             {"building_slots", building_slots},
             {"module_count", filled(modules)},
             {"building_count", filled(buildings)},
             {"modules", modules},
             {"buildings", buildings} };
}

nlohmann::json GalaxyManager::FleetMia(const std::vector<uint32_t>& fleet_ids, const std::string& type) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (fleet_ids.empty()) return { {"success", false}, {"error", "fleet_ids is empty"} };
    for (uint32_t f : fleet_ids) {
        if (!OwnFleet(s, f)) return { {"success", false}, {"error", "Fleet " + std::to_string(f) + " is not one of the player's fleets"} };
    }
    uint32_t mia;
    if (type == "return_home") mia = kMiaReturnHome;
    else if (type == "emergency_ftl") mia = kMiaEmergencyFtl;
    else return { {"success", false}, {"error", "type must be return_home or emergency_ftl"} };
    // CGoMIACommand: CFleet::GoMIA(type) for every fleet with CFleet::CanGoMIA
    namespace mi = sdk::cmd::mia_command;
    auto cmd = CommandBuilder::Get().Create(mi::kSpec);
    std::string why;
    if (!SetRefArray(cmd, mi::fleets, fleet_ids, &why)) return { {"success", false}, {"error", why} };
    cmd.Set<uint32_t>(sdk::rt::CGoMIACommand_mia_type, mia);
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "None of these fleets can go missing in action now" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_ids", fleet_ids}, {"type", type},
             {"message", type == "return_home" ? "The fleets jump out and return home (MIA until they arrive)"
                                               : "Emergency FTL: the fleets retreat and are MIA for a while"} };
}

nlohmann::json GalaxyManager::ClaimSystem(uint32_t system_id, bool remove, int count) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    auto it = s.index.find(system_id);
    if (it == s.index.end()) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(system_id)} };
    void* sys = s.systems[it->second].obj;
    int before = ClaimsBy(base_address_, sys, s.player);
    std::string why;
    if (remove) {
        // CRemoveSystemClaimCommand: at most the claims we hold; not while at war with the owner
        if (count <= 0) count = before;
        namespace rc = sdk::cmd::remove_system_claim_command;
        auto cmd = CommandBuilder::Get().Create(rc::kSpec);
        cmd.Set<uint32_t>(rc::country, s.player_id).Set<uint32_t>(rc::system, system_id).Set<int32_t>(rc::claims, count);
        if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "No claims to remove here" : why}, {"player_claims", before} };
        if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    } else {
        // CAddSystemClaimCommand, as the UI builds it: `count` claims dated today; IsValid is
        // CGalacticObject::IsClaimableBy plus the influence cost (CCountry::CalcClaimCost)
        if (count <= 0) count = 1;
        void* state = nullptr;
        Read((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &state);
        uint32_t today = state ? ReadOr<uint32_t>((uintptr_t)state + sdk::rt::CGameState_date_hours, 0) : 0;
        namespace ac = sdk::cmd::add_system_claim_command;
        auto cmd = CommandBuilder::Get().Create(ac::kSpec);
        cmd.Set<uint32_t>(ac::country, s.player_id)
            .Set<uint32_t>(ac::system, system_id)
            .Set<int32_t>(ac::claims, count)
            .Set<uint32_t>(ac::date, today);
        if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The system cannot be claimed" : why}, {"player_claims", before} };
        if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    }
    return { {"success", true}, {"system_id", system_id}, {"system", SystemName(system_id, sys)},
             {"action", remove ? "remove" : "add"}, {"count", count}, {"player_claims_before", before},
             {"message", "Claim order posted; read player_claims from get_system on a later call"} };
}

uint32_t GalaxyManager::SpatialSystem(uint32_t type, uint32_t id) {
    namespace cc = sdk::ent::CCelestialCoordinate;
    auto origin = [](void* obj, std::ptrdiff_t coord) {
        return obj ? ReadOr<uint32_t>((uintptr_t)obj + coord + cc::origin, kInvalidId) : kInvalidId;
    };
    switch (type) {
    case kSpatialSystem:
        return id;
    case kSpatialPlanet:
        return origin(RefLookup(base_address_, sdk::db::CPlanet, id), sdk::ent::CPlanet::coordinate);
    case kSpatialDebris:
        return origin(RefLookup(base_address_, sdk::db::CDebris, id), sdk::ent::CDebris::coordinate);
    case kSpatialMegastructure:
        return origin(RefLookup(base_address_, sdk::db::CMegaStructure, id), sdk::ent::CMegaStructure::coordinate);
    case kSpatialNaturalWormhole:
        return origin(RefLookup(base_address_, sdk::db::CNaturalWormhole, id), sdk::ent::CNaturalWormhole::coordinate);
    case kSpatialAstralRift:
        return origin(RefLookup(base_address_, sdk::db::CAstralRift, id), sdk::ent::CAstralRift::coordinate);
    case kSpatialFleet:
        if (void* f = fleets::Find(base_address_, id)) {
            const void* c = FleetCoordinate(base_address_, f);
            return c ? ReadOr<uint32_t>((uintptr_t)c + cc::origin, kInvalidId) : kInvalidId;
        }
        return kInvalidId;
    default:
        return kInvalidId;
    }
}

nlohmann::json GalaxyManager::CollectData(uint32_t fleet_id, uint32_t project_id, uint32_t system_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (project_id == kInvalidId && !s.index.count(system_id)) {
        return { {"success", false}, {"error", "Give project_id, or system_id of a system with a located special project"} };
    }
    // CCollectDataFleetOrderCommand: galactic_object set = every located project of the country in
    // that system; galactic_object -1 = the project with this id (as the two engine ctors build it)
    namespace cd = sdk::cmd::collect_data_fleet_order_command;
    auto cmd = CommandBuilder::Get().Create(cd::kSpec);
    cmd.Set<uint32_t>(cd::fleet, fleet_id)
        .Set<uint32_t>(cd::galactic_object, project_id != kInvalidId ? kInvalidId : system_id)
        .Set<uint32_t>(cd::id, project_id != kInvalidId ? project_id : 0)
        .Set<uint8_t>(cd::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused: no special project there that this fleet can research" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    nlohmann::json out = { {"success", true}, {"fleet_id", fleet_id}, {"queued", queue}, {"message", "Research order posted"} };
    if (project_id != kInvalidId) out["project_id"] = project_id;
    else out["system_id"] = system_id;
    return out;
}

nlohmann::json GalaxyManager::LandArmies(uint32_t fleet_id, uint32_t planet_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };
    uint32_t colony = ReadOr<uint32_t>((uintptr_t)planet + sdk::ent::CPlanet::colony, kInvalidId);
    if (colony == kInvalidId) return { {"success", false}, {"error", "The planet has no colony to invade"} };
    // CFleetLandArmiesCommand: IsValid is CLandArmiesFleetOrder::IsPossible (war, armies aboard,
    // planetary shields and defences ...), with the game's reason
    namespace la = sdk::cmd::fleet_land_armies_command;
    auto cmd = CommandBuilder::Get().Create(la::kSpec);
    cmd.Set<uint32_t>(la::fleet, fleet_id).Set<uint32_t>(la::colony, colony)
        .Set<uint8_t>(la::queue, queue ? 1 : 0).Set<uint8_t>(la::queue_to_front, 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the landing" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id}, {"colony_id", colony},
             {"planet", PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name))}, {"queued", queue},
             {"message", "Landing order posted: the armies fly there and invade"} };
}

nlohmann::json GalaxyManager::OrbitPlanet(uint32_t fleet_id, uint32_t planet_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };
    namespace ob = sdk::cmd::fleet_orbit_planet;
    auto cmd = CommandBuilder::Get().Create(ob::kSpec);
    cmd.Set<uint32_t>(ob::fleet, fleet_id)
        .Set<uint32_t>(ob::orbitable + kOrbitableId, planet_id)
        .Set<uint8_t>(ob::orbitable + kOrbitableKind, kOrbitablePlanet)
        .Set<uint8_t>(ob::queue, queue ? 1 : 0)
        .Set<uint8_t>(ob::queue_to_front, 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The fleet cannot move (a station or immobile)" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id},
             {"planet", PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name))}, {"queued", queue},
             {"message", "Orbit order posted"} };
}

nlohmann::json GalaxyManager::ResearchAnomalies(uint32_t fleet_id, uint32_t system_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!s.index.count(system_id)) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(system_id)} };
    // CFleetResearchAnomaliesCommand: a science ship researches the system's discovered anomalies
    namespace ra = sdk::cmd::research_anomalies;
    auto cmd = CommandBuilder::Get().Create(ra::kSpec);
    cmd.Set<uint32_t>(ra::fleet, fleet_id).Set<uint32_t>(ra::system, system_id).Set<uint8_t>(ra::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the anomaly research" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"system_id", system_id},
             {"system", SystemName(system_id, s.systems[s.index[system_id]].obj)}, {"queued", queue},
             {"message", "Anomaly research order posted"} };
}

// 按天体研究异常：CResearchAnomalyFleetOrderCommand（token 0x2CF0，size 0x30）
//   fleet +0x20 / planet +0x24 / queue +0x28
// 与星系版的 CFleetResearchAnomaliesCommand 不同，这条直接指定天体，星系里有多个异常时能准确选。
// 只读：拿一个已知的 planet_id 当路标，从舰队对象出发找「+0x30 == planet_id」的对象，
// 目的就是定位 CFleet 的指令容器（CResearchAnomalyFleetOrder::planet 在 +0x30）。
// 只读：直接读舰队的指令数组。偏移来自 CFleet::BuildOrdersString（RVA 0xC7B960）：
//   +0x2D8 数组 data、+0x2E4 size、+0x2C8 指令管理器对象。
// 每条指令 dump：vtable RVA、order_id(+0x12)、planet(+0x30 指针与低 32 位)
nlohmann::json GalaxyManager::ProbeFleetOrdersJson(uint32_t fleet_id) {
    void* fleet = RefLookup(base_address_, sdk::db::CFleet, fleet_id);
    if (!fleet) return { {"error", "fleet not found: " + std::to_string(fleet_id)} };

    void* data = nullptr;
    uint32_t size = 0;
    Read((const void*)((uintptr_t)fleet + 0x2D8), &data);
    Read((const void*)((uintptr_t)fleet + 0x2E4), &size);

    nlohmann::json out = nlohmann::json::array();
    if (data && size > 0 && size <= 64) {
        for (uint32_t i = 0; i < size; ++i) {
            void* o = nullptr;
            if (!Read((const void*)((uintptr_t)data + (size_t)i * 8), &o) || !o) continue;
            void* vt = nullptr;
            Read((const void*)o, &vt);
            const uint64_t q30 = ReadOr<uint64_t>((uintptr_t)o + 0x30, 0);
            out.push_back({
                {"i", i},
                {"obj", (uint64_t)(uintptr_t)o},
                {"vtable_rva", vt ? (uint64_t)((uintptr_t)vt - base_address_) : 0},
                {"order_id", ReadOr<int32_t>((uintptr_t)o + 0x12, 0)},
                {"q30", q30},
                {"q30_lo", (uint32_t)(q30 & 0xFFFFFFFF)},
                {"q30_hi", (uint32_t)(q30 >> 32)},
                {"q8", ReadOr<uint64_t>((uintptr_t)o + 0x08, 0)},
                {"q10", ReadOr<uint64_t>((uintptr_t)o + 0x10, 0)},
                {"q18", ReadOr<uint64_t>((uintptr_t)o + 0x18, 0)},
                {"q20", ReadOr<uint64_t>((uintptr_t)o + 0x20, 0)},
                {"q28", ReadOr<uint64_t>((uintptr_t)o + 0x28, 0)},
                {"q38", ReadOr<uint64_t>((uintptr_t)o + 0x38, 0)}
            });
        }
    }
    // 当前指令：fleet+0x2C8 指向它；无指令时指向一个共享的空占位对象（token 0）。
    // 实测（fleet 1 正在调查 4038）：+0x18 = 0x2CF0（research_anomaly_order 的 token）、+0x28 = 4038。
    void* cur = nullptr;
    Read((const void*)((uintptr_t)fleet + 0x2C8), &cur);
    nlohmann::json current = {
        {"ptr", (uint64_t)(uintptr_t)cur},
        {"token", cur ? ReadOr<uint32_t>((uintptr_t)cur + 0x18, 0) : 0u},
        {"target", cur ? ReadOr<uint32_t>((uintptr_t)cur + 0x28, 0) : 0u},
        {"has_order", cur != nullptr && ReadOr<uint32_t>((uintptr_t)cur + 0x18, 0) != 0}
    };
    return { {"success", true}, {"fleet_id", fleet_id}, {"fleet", (uint64_t)(uintptr_t)fleet},
             {"name", PersistentNameText((const void*)((uintptr_t)fleet + sdk::ent::CFleet::name))},
             {"orders_ptr", (uint64_t)(uintptr_t)data}, {"orders_count", size},
             {"current_order", current}, {"orders", out} };
}
nlohmann::json GalaxyManager::ScanFleetOrdersJson(uint32_t fleet_id, uint32_t planet_id, int depth) {
    if (depth < 1) depth = 1;
    if (depth > 2) depth = 2;
    void* fleet = RefLookup(base_address_, sdk::db::CFleet, fleet_id);
    if (!fleet) return { {"error", "fleet not found: " + std::to_string(fleet_id)} };

    void* planet_obj = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    const uintptr_t planet_ptr = (uintptr_t)planet_obj;
    // 句柄的低 24 位是「库里第几槽」，不是对象 id —— 两者不同，得单独算
    uint32_t planet_slot = 0xFFFFFFFF;
    {
        void* db = nullptr; void* slots = nullptr; uint32_t cap = 0;
        if (Read((const void*)(base_address_ + sdk::db::CPlanet), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &slots) && slots &&
            Read((const void*)((uintptr_t)db + 0x20), &cap)) {
            for (uint32_t i = 0; i < cap && i < 65536; ++i) {
                void* o = nullptr;
                if (!Read((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &o) || !o) continue;
                if ((uintptr_t)o == planet_ptr) { planet_slot = i; break; }
            }
        }
    }
    struct Item { uintptr_t obj; int depth; const char* via; };
    std::vector<Item> queue{ { (uintptr_t)fleet, 0, "fleet" } };
    std::vector<uintptr_t> seen;
    nlohmann::json hits = nlohmann::json::array();
    int guard = 0;
    uint32_t visited = 0;
    while (!queue.empty() && guard < 3000) {
        const Item it = queue.back();
        queue.pop_back();
        ++guard;
        if (std::find(seen.begin(), seen.end(), it.obj) != seen.end()) continue;
        seen.push_back(it.obj);
        ++visited;

        // 命中判据：+0x30 是「指向 CPlanet 的指针」（SDK: u32 (ptr; value at +0x0)），
        // 所以既比指针，也比裸 id（兼容不同序列化写法）
        const uint32_t at30 = ReadOr<uint32_t>(it.obj + 0x30, 0xFFFFFFFF);
        const uint64_t q30 = ReadOr<uint64_t>(it.obj + 0x30, 0);
        const bool hit_ptr = planet_ptr != 0 && q30 == (uint64_t)planet_ptr;
        const bool hit_id = at30 != 0xFFFFFFFF && (at30 == planet_id || (at30 & 0xFFFFFF) == (planet_id & 0xFFFFFF));
        const bool hit_slot = planet_slot != 0xFFFFFFFF && (at30 & 0xFFFFFF) == planet_slot;
        if (hit_ptr || hit_id || hit_slot) {
            void* vt = nullptr;
            Read((const void*)it.obj, &vt);
            hits.push_back({
                {"obj", (uint64_t)it.obj},
                {"via", it.via},
                {"depth", it.depth},
                {"at30", at30},
                {"hit_ptr", hit_ptr}, {"hit_slot", hit_slot}, {"planet_slot", planet_slot},
                {"vtable_rva", vt ? (uint64_t)((uintptr_t)vt - base_address_) : 0},
                {"q0", ReadOr<uint64_t>(it.obj + 0x00, 0)},
                {"q08", ReadOr<uint64_t>(it.obj + 0x08, 0)},
                {"q10", ReadOr<uint64_t>(it.obj + 0x10, 0)},
                {"q18", ReadOr<uint64_t>(it.obj + 0x18, 0)},
                {"q20", ReadOr<uint64_t>(it.obj + 0x20, 0)},
                {"q28", ReadOr<uint64_t>(it.obj + 0x28, 0)},
                {"q38", ReadOr<uint64_t>(it.obj + 0x38, 0)}
            });
            if (hits.size() >= 12) break;
        }

        if (it.depth < depth) {
            for (int off = 0; off <= 0x2000; off += 8) {
                void* p = nullptr;
                if (!Read((const void*)(it.obj + off), &p) || !p) continue;
                const uintptr_t up = (uintptr_t)p;
                if (up < 0x10000) continue;                       // 跳过 null / 小整数
                queue.push_back({ up, it.depth + 1, "ptr" });
            }
        }
    }
    return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id},
             {"visited", visited}, {"hits", hits} };
}
nlohmann::json GalaxyManager::ResearchAnomaly(uint32_t fleet_id, uint32_t planet_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };

    namespace ra = sdk::cmd::research_anomaly_order;
    auto cmd = CommandBuilder::Get().Create(ra::kSpec);
    cmd.Set<uint32_t>(ra::fleet, fleet_id).Set<uint32_t>(ra::planet, planet_id).Set<uint8_t>(ra::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the anomaly research" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };

    std::string planet_name = PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name));
    return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id}, {"planet", planet_name},
             {"queued", queue}, {"message", "Anomaly research order posted (by body)"} };
}
nlohmann::json GalaxyManager::ExcavateSite(uint32_t fleet_id, uint32_t site_id, bool queue, bool dry_run) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!RefLookup(base_address_, sdk::db::CArchaeologicalSite, site_id)) {
        return { {"success", false}, {"error", "Unknown archaeological site id: " + std::to_string(site_id)} };
    }
    // CExcavateArchaeologicalSiteFleetOrderCommand: a science ship with a scientist digs the site
    namespace ex = sdk::cmd::excavate_archaeological_site_fleet_order_command;
    auto cmd = CommandBuilder::Get().Create(ex::kSpec);
    cmd.Set<uint32_t>(ex::fleet, fleet_id).Set<uint32_t>(ex::archaeological_site, site_id).Set<uint8_t>(ex::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) {
        // CArchaeologicalSite::IsPotentialExcavator: chapters left and the site type's potential
        // trigger for this fleet; neither gives a reason text
        const char* generic = "The game refused the excavation: the site has no chapters left, "
                              "or its potential trigger rejects this fleet";
        if (dry_run) {
            return { {"success", true}, {"valid", false}, {"dry_run", true}, {"fleet_id", fleet_id},
                     {"site_id", site_id}, {"reason", why.empty() ? generic : why} };
        }
        return { {"success", false}, {"error", why.empty() ? generic : why} };
    }
    if (dry_run) {
        return { {"success", true}, {"valid", true}, {"dry_run", true}, {"fleet_id", fleet_id}, {"site_id", site_id} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"site_id", site_id}, {"queued", queue},
             {"message", "Excavation order posted"} };
}

// 只读：对每个遗址建一条 CExcavateArchaeologicalSiteFleetOrderCommand，只跑 IsValid、不 Post。
// 用一次快照 + 一个循环（不能拿 ExcavateSite 逐个问：它每次都 Take()，34 次会很慢）。
// IsValid 走 CArchaeologicalSite::IsPotentialExcavator（章节是否还有、遗址类型的 potential），
// 拒绝时引擎**不给原因**，只能回一句通用说明——所以这里把空 reason 标出来，别当成真因。
nlohmann::json GalaxyManager::ProbeExcavatability(uint32_t fleet_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };

    namespace ex = sdk::cmd::excavate_archaeological_site_fleet_order_command;
    nlohmann::json rows = nlohmann::json::array();
    int valid_count = 0;
    ForEachRef(base_address_, sdk::db::CArchaeologicalSite, [&](uint32_t, void* a) {
        const uint32_t site_id = ReadOr<uint32_t>((uintptr_t)a + sdk::rt::CArchaeologicalSite_id, kInvalidId);
        if (site_id == kInvalidId) return;
        auto cmd = CommandBuilder::Get().Create(ex::kSpec);
        cmd.Set<uint32_t>(ex::fleet, fleet_id).Set<uint32_t>(ex::archaeological_site, site_id).Set<uint8_t>(ex::queue, 0);
        std::string why;
        const bool ok = cmd.IsValid(&why);
        if (ok) ++valid_count;
        nlohmann::json r = { {"site_id", site_id}, {"valid", ok} };
        if (!ok) r["reason"] = why;  // 引擎经常给空串，空就是「没说」
        rows.push_back(r);
    });
    return { {"success", true}, {"fleet_id", fleet_id}, {"count", rows.size()}, {"valid_count", valid_count}, {"rows", rows} };
}

// 星界裂隙：投递探索（CExploreAstralRiftFleetOrderCommand：fleet +0x20 / astral_rift +0x24 / queue +0x28）。
// 注意它和考古不同：科学官一旦进去就撤不回来，所以调用方要先挑好船。
nlohmann::json GalaxyManager::ExploreAstralRift(uint32_t fleet_id, uint32_t rift_id, bool queue, bool dry_run) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!RefLookup(base_address_, sdk::db::CAstralRift, rift_id)) {
        return { {"success", false}, {"error", "Unknown astral rift id: " + std::to_string(rift_id)} };
    }
    namespace ex = sdk::cmd::explore_astral_rift_fleet_order_command;
    auto cmd = CommandBuilder::Get().Create(ex::kSpec);
    cmd.Set<uint32_t>(ex::fleet, fleet_id).Set<uint32_t>(ex::astral_rift, rift_id).Set<uint8_t>(ex::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) {
        const char* generic = "The game refused the rift exploration (no reason text)";
        if (dry_run) {
            return { {"success", true}, {"valid", false}, {"dry_run", true}, {"fleet_id", fleet_id},
                     {"rift_id", rift_id}, {"reason", why.empty() ? generic : why} };
        }
        return { {"success", false}, {"error", why.empty() ? generic : why} };
    }
    if (dry_run) {
        return { {"success", true}, {"valid", true}, {"dry_run", true}, {"fleet_id", fleet_id}, {"rift_id", rift_id} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"rift_id", rift_id}, {"queued", queue},
             {"message", "Astral rift exploration order posted"} };
}

// 只读：对每个裂隙各建一条 CExploreAstralRiftFleetOrderCommand，只跑 IsValid、不投递。
// 和 probe_excavatability 同一个套路（一次快照 + 一个循环）。
nlohmann::json GalaxyManager::ProbeRiftExplorability(uint32_t fleet_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };

    namespace ex = sdk::cmd::explore_astral_rift_fleet_order_command;
    nlohmann::json rows = nlohmann::json::array();
    int valid_count = 0;
    ForEachRef(base_address_, sdk::db::CAstralRift, [&](uint32_t, void* r) {
        const uint32_t rift_id = ReadOr<uint32_t>((uintptr_t)r + sdk::rt::CAstralRift_id, kInvalidId);
        if (rift_id == kInvalidId) return;
        auto cmd = CommandBuilder::Get().Create(ex::kSpec);
        cmd.Set<uint32_t>(ex::fleet, fleet_id).Set<uint32_t>(ex::astral_rift, rift_id).Set<uint8_t>(ex::queue, 0);
        std::string why;
        const bool ok = cmd.IsValid(&why);
        if (ok) ++valid_count;
        nlohmann::json row = { {"rift_id", rift_id}, {"valid", ok} };
        if (!ok) row["reason"] = why;
        rows.push_back(row);
    });
    return { {"success", true}, {"fleet_id", fleet_id}, {"count", rows.size()}, {"valid_count", valid_count}, {"rows", rows} };
}

nlohmann::json GalaxyManager::UseBypass(uint32_t fleet_id, uint32_t bypass_id, uint32_t to_system, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* b = RefLookup(base_address_, sdk::db::CBypass, bypass_id);
    if (!b) return { {"success", false}, {"error", "Unknown bypass id: " + std::to_string(bypass_id)} };
    // the destination is the bypass in to_system that this one connects to (its linked bypass or
    // one of its active network connections)
    namespace bp = sdk::ent::CBypass;
    std::vector<uint32_t> links = RefArray((uintptr_t)b + bp::active_connections);
    links.push_back(ReadOr<uint32_t>((uintptr_t)b + bp::linked_to, kInvalidId));

    uint32_t dest = kInvalidId;
    uint32_t target_system = to_system;
    if (to_system == kInvalidId) {
        // 没给 to_system：自己找对端——扫所有星系，看哪条的 bypass 列表里有与本条相连的那条。
        // （2026-11-23 修：以前直接拿 0xFFFFFFFF 去查，必然报 Unknown system id）
        for (const auto& sys : s.systems) {
            auto there = RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::bypasses);
            for (uint32_t l : links) {
                if (l == kInvalidId) continue;
                if (std::find(there.begin(), there.end(), l) != there.end()) { dest = l; target_system = sys.id; break; }
            }
            if (dest != kInvalidId) break;
        }
        if (dest == kInvalidId)
            return { {"success", false}, {"error", "This bypass has no counterpart in any known system"} };
    } else {
        auto it = s.index.find(to_system);
        if (it == s.index.end()) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(to_system)} };
        auto there = RefArray((uintptr_t)s.systems[it->second].obj + sdk::ent::CGalacticObject::bypasses);
        for (uint32_t l : links) {
            if (l != kInvalidId && std::find(there.begin(), there.end(), l) != there.end()) { dest = l; break; }
        }
        if (dest == kInvalidId) {
            return { {"success", false}, {"error", "This bypass does not lead to that system (see leads_to in stellaris_get_system)"} };
        }
    }
    namespace ub = sdk::cmd::use_bypass_command;
    auto cmd = CommandBuilder::Get().Create(ub::kSpec);
    cmd.Set<uint32_t>(ub::fleet, fleet_id).Set<uint32_t>(ub::bypass, bypass_id).Set<uint32_t>(ub::destination, dest)
        .Set<uint8_t>(ub::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the jump" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"bypass_id", bypass_id}, {"destination_bypass_id", dest},
             {"to_system_id", target_system}, {"to_system", SystemName(target_system, s.systems[s.index[target_system]].obj)}, {"queued", queue},
             {"message", "Bypass jump order posted (the fleet flies to the bypass first)"} };
}

nlohmann::json GalaxyManager::ExploreBypass(uint32_t fleet_id, uint32_t bypass_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!RefLookup(base_address_, sdk::db::CBypass, bypass_id)) return { {"success", false}, {"error", "Unknown bypass id: " + std::to_string(bypass_id)} };
    // CExploreBypassCommand: a science ship explores an unexplored wormhole / gateway
    namespace eb = sdk::cmd::explore_bypass_command;
    auto cmd = CommandBuilder::Get().Create(eb::kSpec);
    cmd.Set<uint32_t>(eb::fleet, fleet_id).Set<uint32_t>(eb::bypass, bypass_id).Set<uint8_t>(eb::queue, queue ? 1 : 0);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the exploration" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"bypass_id", bypass_id}, {"queued", queue},
             {"message", "Bypass exploration order posted"} };
}

bool GalaxyManager::OutpostCommand(uint32_t fleet_id, uint32_t system_id, bool queue, bool post, std::string* why) {
    // CFleetBuildOrbitalStationCommand: an outpost is a starbase (EShipClass 10) built on the
    // system (galactic_object set; the deposit holder stays the factory's "none")
    namespace ob = sdk::cmd::build_orbital_station_order;
    auto cmd = CommandBuilder::Get().Create(ob::kSpec);
    cmd.Set<uint32_t>(ob::fleet, fleet_id)
        .Set<uint32_t>(ob::galactic_object, system_id)
        .Set<uint8_t>(ob::class_, (uint8_t)fleets::ShipClass::Starbase)
        .Set<uint8_t>(ob::queue, queue ? 1 : 0)
        .Set<uint8_t>(ob::queue_to_front, 0);
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    if (!cmd.IsValid(why)) return false;
    if (post && !cmd.Post(NativeCommand::Check::EngineGate)) {
        if (why) *why = cmd.error();
        return false;
    }
    return true;
}

bool GalaxyManager::StationCommand(uint32_t fleet_id, uint32_t planet_id, uint8_t ship_class, bool queue, bool post,
                                   std::string* why) {
    // CFleetBuildOrbitalStationCommand on a deposit holder (no galactic object): the holder is a
    // CMetaRef persistent (vtable, then EDepositHolderType 0 = planet and the id), the class the
    // station's EShipClass, checked by CanBuildOnDepositHolder
    namespace ob = sdk::cmd::build_orbital_station_order;
    constexpr std::ptrdiff_t kHolderType = ob::deposit_holder + 0x8, kHolderId = ob::deposit_holder + 0xC;
    constexpr uint32_t kHolderPlanet = 0;
    auto cmd = CommandBuilder::Get().Create(ob::kSpec);
    cmd.Set<uint32_t>(ob::fleet, fleet_id)
        .Set<uint32_t>(kHolderType, kHolderPlanet)
        .Set<uint32_t>(kHolderId, planet_id)
        .Set<uint32_t>(ob::galactic_object, 0xFFFFFFFF)
        .Set<uint8_t>(ob::class_, ship_class)
        .Set<uint8_t>(ob::queue, queue ? 1 : 0)
        .Set<uint8_t>(ob::queue_to_front, 0);
    if (!cmd) {
        if (why) *why = cmd.error();
        return false;
    }
    if (!cmd.IsValid(why)) return false;
    if (post && !cmd.Post(NativeCommand::Check::EngineGate)) {
        if (why) *why = cmd.error();
        return false;
    }
    return true;
}

nlohmann::json GalaxyManager::BuildStation(uint32_t fleet_id, uint32_t planet_id, const std::string& station, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!RefLookup(base_address_, sdk::db::CPlanet, planet_id)) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };
    const std::pair<const char*, fleets::ShipClass> kinds[] = {
        { "mining", fleets::ShipClass::MiningStation }, { "research", fleets::ShipClass::ResearchStation },
        { "observation", fleets::ShipClass::ObservationStation } };
    std::vector<std::pair<const char*, fleets::ShipClass>> tries;
    for (const auto& k : kinds) {
        if (station == k.first || (station.empty() && k.second != fleets::ShipClass::ObservationStation)) tries.push_back(k);
    }
    if (tries.empty()) return { {"success", false}, {"error", "station is mining, research or observation"} };
    nlohmann::json refused = nlohmann::json::object();
    for (const auto& [name, cls] : tries) {
        std::string why;
        if (StationCommand(fleet_id, planet_id, (uint8_t)cls, queue, true, &why)) {
            return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id}, {"station", name},
                     {"queued", queue}, {"message", std::string(name) + " station order posted"} };
        }
        refused[name] = why.empty() ? "The game refused it" : why;
    }
    return { {"success", false}, {"error", "The game refuses the station"}, {"reasons", refused} };
}

nlohmann::json GalaxyManager::BuildOutpost(uint32_t fleet_id, uint32_t system_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    if (!s.index.count(system_id)) return { {"success", false}, {"error", "Unknown system id: " + std::to_string(system_id)} };
    std::string why;
    if (!OutpostCommand(fleet_id, system_id, queue, true, &why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused the outpost" : why} };
    }
    return { {"success", true}, {"fleet_id", fleet_id}, {"system_id", system_id},
             {"system", SystemName(system_id, s.systems[s.index[system_id]].obj)}, {"queued", queue},
             {"message", "Outpost order posted"} };
}

nlohmann::json GalaxyManager::Colonize(uint32_t fleet_id, uint32_t planet_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return { {"success", false}, {"error", "Unknown planet id: " + std::to_string(planet_id)} };
    // IsValid asks CanColonize without a reason; ask it first so a refusal says why
    std::string why;
    if (!CanColonize(base_address_, planet, s.player, &why)) {
        return { {"success", false}, {"error", why.empty() ? "The game refused the colonization" : why} };
    }
    namespace co = sdk::cmd::colonize_planet_order;
    auto cmd = CommandBuilder::Get().Create(co::kSpec);
    cmd.Set<uint32_t>(co::fleet, fleet_id)
        .Set<uint32_t>(co::planet, planet_id)
        .Set<uint8_t>(co::queue, queue ? 1 : 0)
        .Set<uint8_t>(co::queue_to_front, 0);
    if (!cmd.IsValid(&why)) {
        // CanColonize passed: CColonizePlanetFleetOrder::CanQueue refused this fleet
        return { {"success", false}, {"error", why.empty() ? "This fleet cannot take the colonization order (not a colony ship?)" : why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"fleet_id", fleet_id}, {"planet_id", planet_id},
             {"planet", PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name))},
             {"queued", queue}, {"message", "Colonization order posted"} };
}

nlohmann::json GalaxyManager::FindSystems(const std::string& purpose, uint32_t from, int limit, uint32_t fleet_id,
                                          const std::string& resource, uint32_t species_id, const std::string& name) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (from == kInvalidId) from = CapitalSystem(s);
    if (!s.index.count(from)) return { {"error", "Unknown system id: " + std::to_string(from)} };
    limit = std::clamp(limit, 1, 50);
    auto jumps = JumpsFrom(s, from);
    std::vector<std::pair<int, uint32_t>> order;
    for (const auto& [id, d] : jumps) order.push_back({ d, id });
    std::sort(order.begin(), order.end());

    std::unordered_set<uint32_t> own_systems;
    for (const auto& sys : s.systems) {
        if (Owner(sys.obj) == s.player_id) own_systems.insert(sys.id);
    }
    auto base_row = [&](uint32_t id, int d) {
        const System& sys = s.systems[s.index[id]];
        int intel = Intel(s, sys.obj);
        uint32_t owner = Owner(sys.obj);
        bool known = owner != kInvalidId && (owner == s.player_id || intel >= 1);
        return nlohmann::json{ {"id", id}, {"name", SystemName(id, sys.obj)}, {"jumps", d}, {"intel", intel},
                               {"owner_id", known ? nlohmann::json(owner) : nlohmann::json(nullptr)} };
    };

    nlohmann::json out = nlohmann::json::array();
    std::map<std::string, int> skipped;  // unsurveyed systems the fleet cannot survey, by the game's reason
    // 2026-11-22 新增：按名字找星系（插件 stellaris_find_systems 的 name 模式）。
    // 名称搜索是**全银河**的（不是 get_galaxy_map 那种 3 跳邻域），跳数仍从 from 算。
    if (!name.empty()) {
        std::string needle = name;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        std::vector<std::pair<int, uint32_t>> hits;
        for (const auto& sys : s.systems) {
            std::string nm = SystemName(sys.id, sys.obj);
            std::string low = nm;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char ch) { return (char)std::tolower(ch); });
            if (low.find(needle) == std::string::npos) continue;
            auto it = jumps.find(sys.id);
            hits.push_back({ it == jumps.end() ? 9999 : it->second, sys.id });
        }
        std::sort(hits.begin(), hits.end());
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [d, id] : hits) {
            if ((int)list.size() >= limit) break;
            list.push_back(base_row(id, d));
        }
        return { {"systems", list}, {"count", list.size()}, {"mode", "name"}, {"query", name}, {"from_system_id", from} };
    }
    if (purpose == "unsurveyed") {
        // the game decides what a science ship may survey (route, border access, nothing left to
        // survey there for it): the survey order's IsValid, as the fleet's order menu checks it
        if (fleet_id == kInvalidId) {
            for (uint32_t f : fleets::Owned(s.player)) {
                if (fleets::ClassOf(fleets::Find(base_address_, f)) == fleets::ShipClass::ScienceShip) {
                    fleet_id = f;
                    break;
                }
            }
        }
        for (const auto& [d, id] : order) {
            if ((int)out.size() >= limit) break;
            const System& sys = s.systems[s.index[id]];
            if (SystemSurveyed(s, id, sys.obj)) continue;
            if (fleet_id != kInvalidId) {
                std::string why;
                if (!SurveyCommand(fleet_id, id, kInvalidId, false, false, &why)) {
                    skipped[why.empty() ? "rejected by the game" : RenderPdxMarkup(why)]++;
                    continue;
                }
            }
            out.push_back(base_row(id, d));
        }
    } else if (purpose == "outpost") {
        // an outpost goes into an unowned system; the engine's own check (the build order's
        // IsValid with a construction ship) decides, else only the basic facts are listed
        if (fleet_id == kInvalidId) {
            for (uint32_t f : fleets::Owned(s.player)) {
                if (fleets::ClassOf(fleets::Find(base_address_, f)) == fleets::ShipClass::Constructor) {
                    fleet_id = f;
                    break;
                }
            }
        }
        for (const auto& [d, id] : order) {
            if ((int)out.size() >= limit) break;
            const System& sys = s.systems[s.index[id]];
            if (Owner(sys.obj) != kInvalidId) continue;
            bool borders = false;
            for (const auto& l : sys.lanes) borders = borders || own_systems.count(l.first);
            nlohmann::json row = base_row(id, d);
            row["surveyed"] = SystemSurveyed(s, id, sys.obj);
            row["borders_own_space"] = borders;
            if (fleet_id != kInvalidId) {
                std::string why;
                bool ok = OutpostCommand(fleet_id, id, false, false, &why);
                row["can_build"] = ok;
                if (!ok) row["reason"] = why;
            }
            out.push_back(row);
        }
    } else if (purpose == "deposit") {
        // deposits on planets the player has surveyed, matching `resource` in the deposit key
        std::unordered_map<uint32_t, std::vector<std::string>> deposits;
        ForEachRef(base_address_, sdk::db::CDeposit, [&](uint32_t, void* dp) {
            uintptr_t holder = (uintptr_t)dp + sdk::ent::CDeposit::deposit_holder;
            if (ReadOr<uint32_t>(holder + kMetaRefType, kHolderNone) != kHolderPlanet) return;
            std::string key = KeyOf((uintptr_t)dp + sdk::ent::CDeposit::type, kTypeKey);
            if (key.empty() || (!resource.empty() && key.find(resource) == std::string::npos)) return;
            deposits[ReadOr<uint32_t>(holder + kMetaRefId, kInvalidId)].push_back(key);
        });
        for (const auto& [d, id] : order) {
            if ((int)out.size() >= limit) break;
            const System& sys = s.systems[s.index[id]];
            auto pit = s.planets.find(id);
            if (pit == s.planets.end()) continue;
            nlohmann::json found = nlohmann::json::array();
            for (const auto& p : pit->second) {
                auto dit = deposits.find(p.id);
                if (dit == deposits.end() || !PlanetSurveyed(s, sys.obj, p.obj)) continue;
                for (const auto& k : dit->second) {
                    found.push_back({ {"planet_id", p.id}, {"key", k}, {"name", SafeLocalize(base_address_, k)} });
                }
            }
            if (found.empty()) continue;
            nlohmann::json row = base_row(id, d);
            row["deposits"] = found;
            out.push_back(row);
        }
    } else if (purpose == "colonizable") {
        // the expansion planner's rule (ValidatePlanetFilters): systems with intel above low, planets
        // the player surveyed, not owned, with habitability for the species from
        // NHabitability::CalcHabitability(species, planet, player) above zero; best first. The
        // planner hides systems of empires the player has communications with; a system another
        // country owns cannot be colonized at all, so every foreign-owned system is left out
        if (species_id == kInvalidId) {
            species_id = ReadOr<uint32_t>((uintptr_t)s.player + sdk::ent::CCountry::founder_species_ref, kInvalidId);
        }
        void* species = SpeciesManager::Get().FindSpeciesPtr(species_id);
        if (!species) return { {"error", "Unknown species id: " + std::to_string(species_id)} };
        struct Cand {
            double hab;
            int jumps;
            nlohmann::json row;
        };
        std::vector<Cand> cands;
        for (const auto& [d, id] : order) {
            const System& sys = s.systems[s.index[id]];
            if (Intel(s, sys.obj) < 2) continue;
            uint32_t sys_owner = Owner(sys.obj);
            if (sys_owner != kInvalidId && sys_owner != s.player_id) continue;
            auto pit = s.planets.find(id);
            if (pit == s.planets.end()) continue;
            for (const auto& p : pit->second) {
                if (ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::owner, kInvalidId) != kInvalidId) continue;
                if (ReadOr<uint32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::colony, kInvalidId) != kInvalidId) continue;
                if (!PlanetSurveyed(s, sys.obj, p.obj)) continue;
                double hab = Habitability(base_address_, species, p.obj, s.player);
                if (hab <= 0) continue;
                std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
                cands.push_back({ hab, d, {
                    {"planet_id", p.id},
                    {"planet", PersistentNameText((const void*)((uintptr_t)p.obj + sdk::ent::CPlanet::name))},
                    {"class", cls}, {"class_name", SafeLocalize(base_address_, cls)},
                    {"size", ReadOr<int32_t>((uintptr_t)p.obj + sdk::ent::CPlanet::planet_size, 0)},
                    {"habitability_percent", std::round(hab * 1000.0) / 10.0},
                    {"system_id", id}, {"system", SystemName(id, sys.obj)}, {"jumps", d},
                    {"system_owned", sys_owner == s.player_id} } });
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.hab != b.hab ? a.hab > b.hab : a.jumps < b.jumps;
        });
        nlohmann::json planets = nlohmann::json::array();
        for (size_t i = 0; i < cands.size() && (int)i < limit; ++i) {
            // the game's own verdict for the player now (borders, hostile fleets, blockers ...)
            nlohmann::json row = cands[i].row;
            void* planet = RefLookup(base_address_, sdk::db::CPlanet, row["planet_id"].get<uint32_t>());
            std::string why;
            bool can = planet && CanColonize(base_address_, planet, s.player, &why);
            row["can_colonize"] = can;
            if (!can && !why.empty()) row["reason"] = why;
            planets.push_back(row);
        }
        return { {"purpose", purpose}, {"from_system_id", from}, {"species_id", species_id}, {"planets", planets},
                 {"note", "Habitability for the species with the player's modifiers; systems owned by other empires are "
                          "left out. can_colonize is the game's CPlanet::CanColonize for the player now (reason when not); "
                          "colonizing also needs a colony ship."} };
    } else {
        return { {"error", "purpose must be one of: unsurveyed, outpost, deposit, colonizable"} };
    }
    nlohmann::json res = { {"purpose", purpose}, {"from_system_id", from}, {"systems", out} };
    if (purpose == "outpost" || purpose == "unsurveyed") {
        res["checked_with_fleet_id"] = fleet_id == kInvalidId ? nlohmann::json(nullptr) : nlohmann::json(fleet_id);
    }
    if (purpose == "unsurveyed" && !skipped.empty()) {
        nlohmann::json sk = nlohmann::json::object();
        for (const auto& [why, n] : skipped) sk[why] = n;
        res["not_surveyable"] = sk;  // unsurveyed but refused for this fleet, counted by the game's reason
    }
    return res;
}

// ---- megastructures ----------------------------------------------------------------------------
// Upgrades as the megastructure view offers them (CMegaStructureType::IsPotentiallyUpgradableFrom,
// then CCountryUpgradeMegaStructureCommand whose IsValid is CMegaStructureType::CanUpgrade) and
// construction as a construction ship's build menu (IsPotentialInBuildMenu, then
// CBuildMegaStructureCommand whose IsValid is CBuildMegaStructureFleetOrder::CanQueue). Costs are
// the engine's economic unit table {country, -1, scope, type}: scoped to the country for a new
// megastructure (CMegaStructureType::CalcCost) and to the megastructure for an upgrade (what the
// upgrade command charges).
namespace {

constexpr uint32_t kQueueFirst = 0;  // EQueueMode: first, last, clear (the build window's)
constexpr uint32_t kQueueLast = 1;
constexpr uint32_t kQueueClear = 2;

struct Call3Ctx {
    uintptr_t fn;
    const void* a;
    const void* b;
    const void* c;
    uint8_t result;
};

void CallByte3(void* p, void*) {
    auto* x = (Call3Ctx*)p;
    x->result = ((uint8_t(*)(const void*, const void*, const void*))x->fn)(x->a, x->b, x->c);
}

bool EngineByte3(uintptr_t fn, const void* a, const void* b, const void* c) {
    Call3Ctx ctx{ fn, a, b, c, 0 };
    return CommandBuilder::Get().CallGuarded(&CallByte3, &ctx) && ctx.result;
}

// CFixedResourceTable pre-sized to every resource: +0 points at the {data, capacity, size} holder
struct CostTable {
    void* holder;
    int64_t* data;
    int32_t capacity;
    int32_t size;
    void* allocator;
    int64_t values[256];
};

// the economic unit CalcTable reads: {country id, -1, scope, type}
struct CostCtx {
    uintptr_t fn;
    uint32_t country;
    uint32_t none;
    const void* scope;
    const void* type;
    CostTable* table;
};

void CallCostTable(void* p, void*) {
    auto* x = (CostCtx*)p;
    ((void (*)(const void*, void*, int64_t, bool))x->fn)(&x->country, x->table, (int64_t)kFixed, false);
}

nlohmann::json MegaCost(uintptr_t base, uint32_t country_id, const void* scope, const void* type) {
    const auto& names = GameState::Get().ResourceNames();
    CostTable t{};
    t.holder = &t.data;
    t.data = t.values;
    t.capacity = t.size = (int32_t)std::min<size_t>(names.size(), 256);
    CostCtx ctx{ base + sdk::fn::CMegaStructureType_CalcCostTable, country_id, kInvalidId, scope, type, &t };
    nlohmann::json out = nlohmann::json::object();
    if (!CommandBuilder::Get().CallGuarded(&CallCostTable, &ctx)) return out;
    for (int32_t i = 0; i < t.size; ++i) {
        if (t.values[i] > 0 && !names[i].empty()) out[names[i]] = std::round(t.values[i] / 1000.0) / 100.0;
    }
    return out;
}

// TGameDatabase<CMegaStructureTypeDatabase>: the types at +0x50, count at +0x5C
std::vector<void*> MegaTypes(uintptr_t base) {
    std::vector<void*> out;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t n = 0;
    if (!Read((const void*)(base + sdk::glob::TGameDatabase_CMegaStructureTypeDatabase_pInstance), &db) || !db ||
        !Read((const void*)((uintptr_t)db + 0x50), &arr) || !arr || !Read((const void*)((uintptr_t)db + 0x5C), &n) ||
        n > 4096) {
        return out;
    }
    for (uint32_t i = 0; i < n; ++i) {
        void* t = nullptr;
        if (Read((const void*)((uintptr_t)arr + i * 8), &t) && t) out.push_back(t);
    }
    return out;
}

void* MegaTypeByKey(uintptr_t base, const std::string& key) {
    for (void* t : MegaTypes(base)) {
        if (ReadString((uintptr_t)t + kTypeKey) == key) return t;
    }
    return nullptr;
}

// the upgrades the megastructure's view offers the country
std::vector<void*> MegaUpgrades(uintptr_t base, void* country, void* m) {
    std::vector<void*> out;
    for (void* t : MegaTypes(base)) {
        if (EngineByte3(base + sdk::fn::CMegaStructureType_IsPotentiallyUpgradableFrom, t, country, m)) out.push_back(t);
    }
    return out;
}

// CMegaStructureType_build_type: where a type is placed (the script's build_type)
constexpr int kBuildAtPlanet = 0;
constexpr int kBuildInsideGravityWell = 1;
constexpr int kBuildOutsideGravityWell = 2;

const char* BuildTypeName(int bt) {
    return bt == kBuildAtPlanet ? "planet" : bt == kBuildInsideGravityWell ? "inside_gravity_well"
         : bt == kBuildOutsideGravityWell ? "outside_gravity_well" : "unknown";
}

struct PlacementCtx {
    uintptr_t fn;
    const void* rules;
    const void* planet;
    const void* country;
    uint8_t result;
};

void CallPlacementPossible(void* p, void*) {
    auto* x = (PlacementCtx*)p;
    x->result = ((uint8_t(*)(const void*, const void*, const void*, void*))x->fn)(x->rules, x->planet, x->country, nullptr);
}

// the type's placement rules accept the planet (CPlacementRules::IsPossible, as the build menu)
bool PlacementPossible(uintptr_t base, void* type, const void* planet, const void* country) {
    PlacementCtx ctx{ base + sdk::fn::CPlacementRules_IsPossible,
                      (const void*)((uintptr_t)type + sdk::rt::CMegaStructureType_placement_rules), planet, country, 0 };
    return CommandBuilder::Get().CallGuarded(&CallPlacementPossible, &ctx) && ctx.result;
}

// A CCelestialCoordinate's members up to `randomized` (the engine copies it member-wise)
constexpr size_t kCoordinateSize = sdk::ent::CCelestialCoordinate::randomized + 1;

struct FtlPointCtx {
    uintptr_t fn;
    const void* system;
    uint8_t* out;
    const void* toward;
};

void CallFtlPoint(void* p, void*) {
    auto* x = (FtlPointCtx*)p;
    ((void* (*)(const void*, void*, const void*))x->fn)(x->system, x->out, x->toward);
}

// A point in `system` for a type built in open space, the way the AI places one: the system's
// FTL point toward a neighbouring system (CGalacticObject::CalcFTLPointWith) for outside the
// gravity well, scaled by DEEPSPACE_CITADEL_INNER_RADIUS_PERCENTAGE inside it. Local x, y.
bool OpenSpacePoint(uintptr_t base, void* system, void* toward_system, int build_type, int64_t* x, int64_t* y) {
    namespace cc = sdk::ent::CCelestialCoordinate;
    uint8_t out[0x40]{};
    if (!CopyChars((char*)out, (const char*)((uintptr_t)system + sdk::ent::CGalacticObject::coordinate), kCoordinateSize)) {
        return false;
    }
    FtlPointCtx ctx{ base + sdk::fn::CGalacticObject_CalcFTLPointWith, system, out,
                     (const void*)((uintptr_t)toward_system + sdk::ent::CGalacticObject::coordinate) };
    if (!CommandBuilder::Get().CallGuarded(&CallFtlPoint, &ctx)) return false;
    *x = *(int64_t*)(out + cc::x);
    *y = *(int64_t*)(out + cc::y);
    if (build_type == kBuildInsideGravityWell) {
        int64_t pct = ReadOr<int64_t>(base + sdk::glob::NDefines_DEEPSPACE_CITADEL_INNER_RADIUS_PERCENTAGE, 0);
        if (pct <= 0) return false;
        *x = *x * pct / (int64_t)kFixed;
        *y = *y * pct / (int64_t)kFixed;
    }
    return true;
}

NativeCommand UpgradeCommand(uint32_t country_id, uint32_t mega_id, void* type) {
    namespace um = sdk::cmd::upgrade_megastructure_command;
    auto cmd = CommandBuilder::Get().Create(um::kSpec);
    cmd.Set<uint32_t>(um::paying_country, country_id).Set<uint32_t>(um::object, mega_id).Set<void*>(um::type, type);
    return cmd;
}

NativeCommand BuildMegaCommand(uint32_t fleet_id, void* type, uint32_t planet_id, uint32_t mode) {
    // the factory leaves the coordinate empty (origin -1): the planet form of the command
    namespace bm = sdk::cmd::build_megastructure_command;
    auto cmd = CommandBuilder::Get().Create(bm::kSpec);
    cmd.Set<uint32_t>(bm::fleet, fleet_id).Set<void*>(bm::type, type).Set<uint32_t>(bm::planet, planet_id)
        .Set<uint32_t>(bm::queue, mode);
    return cmd;
}

// the coordinate form: a point in the system (local x, y; origin = the system), no planet
NativeCommand BuildMegaAtPointCommand(uint32_t fleet_id, void* type, uint32_t system_id, int64_t x, int64_t y, uint32_t mode) {
    namespace bm = sdk::cmd::build_megastructure_command;
    namespace cc = sdk::ent::CCelestialCoordinate;
    auto cmd = CommandBuilder::Get().Create(bm::kSpec);
    cmd.Set<uint32_t>(bm::fleet, fleet_id).Set<void*>(bm::type, type).Set<uint32_t>(bm::planet, kInvalidId)
        .Set<uint32_t>(bm::queue, mode)
        .Set<int64_t>(bm::coordinate + cc::x, x)
        .Set<int64_t>(bm::coordinate + cc::y, y)
        .Set<uint32_t>(bm::coordinate + cc::origin, system_id);
    return cmd;
}

}  // namespace

// 只读：列全部巨型结构（2026-11-22）。CMegaStructure 库：TPdxRef<CMegaStructure>::_pDatabase
// 0x311BD40 → {slots +0x18, cap +0x20}，对象在 slot*16+8；id 在 CMegaStructure_id 且要与槽位自洽。
// 可见性沿用 GetMegastructureJson：自己的，或所在星系 intel >= 2 的。
// 只读：帮电弧熔炉挑星系（2026-11-22）。它的收益看**星系内天体数量**
//（游戏文案：揭露星系内每颗行星、卫星和小行星上规模 1 的矿物矿藏，开采站产出 +25%），
// 所以按天体数从多到少排；同时套上脚本里的两条前置：
//   1) system_has_arc_furnace = no  → 该星系还没有电弧熔炉
//   2) any_system_planet = { is_planet_class = pc_molten } → 至少一颗熔融星球
// 只读：戴森云候选星系（2026-11-22）。戴森云只吃**恒星自身产出**，且只能建在**单星星系**：
//   脚本前置：system_has_dyson_swarm = no、is_multiple_star = no、
//            NOT { is_star_class = sc_black_hole / sc_neutron_star / sc_pulsar }
//   效果：station_gatherers_produces_mult / station_researchers_produces_mult（@dyson_swarm_N_mod_value）
// 所以排序按「恒星产出折算成能量币」：1 稀有文物 = 1 战略资源 = 2 科研点 = 8 能量币（玩家定的等价）。
// 只读：戴森云候选星系（2026-11-22）。戴森云只吃**恒星自身产出**，且只能建在**单星星系**：
//   脚本前置：system_has_dyson_swarm = no、is_multiple_star = no、
//            NOT { is_star_class = sc_black_hole / sc_neutron_star / sc_pulsar }
//   效果：station_gatherers_produces_mult / station_researchers_produces_mult（@dyson_swarm_N_mod_value）
// 所以按「恒星产出折算成能量币」排序：1 稀有文物 = 1 战略资源 = 2 科研点 = 8 能量币。
// 恒星的判定用**天体类键**：pc_f_star / pc_m_star 这种以 _star 结尾的，加上黑洞 / 中子星 / 脉冲星。
nlohmann::json GalaxyManager::FindDysonSwarmSystems(uint32_t limit, uint32_t fleet_id, uint32_t from_system_id,
                                                    bool sort_by_value) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (limit == 0 || limit > 200) limit = 30;

    auto is_star = [](const std::string& cls) {
        if (cls.rfind("pc_", 0) != 0) return false;
        return cls.find("_star") != std::string::npos || cls.find("black_hole") != std::string::npos ||
               cls.find("neutron") != std::string::npos || cls.find("pulsar") != std::string::npos;
    };
    auto is_excluded_star = [](const std::string& cls) {
        return cls.find("black_hole") != std::string::npos || cls.find("neutron") != std::string::npos ||
               cls.find("pulsar") != std::string::npos;
    };

    // 起点：显式给的 > 工程船所在星系 > 首都
    uint32_t origin = from_system_id;
    if (origin == kInvalidId && fleet_id != kInvalidId) {
        for (const auto& sys : s.systems) {
            for (uint32_t f : RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::fleet_presence)) {
                if ((f & 0xFFFFFFu) == (fleet_id & 0xFFFFFFu)) origin = sys.id;
            }
            if (origin != kInvalidId) break;
        }
    }
    if (origin == kInvalidId) origin = CapitalSystem(s);
    std::unordered_map<uint32_t, int> dist;
    if (s.index.count(origin)) {
        dist[origin] = 0;
        std::deque<uint32_t> q{ origin };
        while (!q.empty()) {
            const uint32_t id = q.front();
            q.pop_front();
            const int d = dist[id];
            for (const auto& l : s.systems[s.index[id]].lanes) {
                if (s.index.count(l.first) && !dist.count(l.first)) {
                    dist[l.first] = d + 1;
                    q.push_back(l.first);
                }
            }
        }
    }

    // 已经有戴森云的星系
    std::unordered_set<uint32_t> has_swarm;
    {
        void* db = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (Read((const void*)(base_address_ + sdk::db::CMegaStructure), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            Read((const void*)((uintptr_t)db + 0x20), &cap) && cap > 0 && cap < 100000) {
            for (uint32_t i = 0; i < cap; ++i) {
                void* m = nullptr;
                if (!Read((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &m) || !m) continue;
                const std::string type = KeyOf((uintptr_t)m + sdk::ent::CMegaStructure::type, kTypeKey);
                if (type.find("dyson_swarm") == std::string::npos) continue;
                const uint32_t sys = ReadOr<uint32_t>(
                    (uintptr_t)m + sdk::ent::CMegaStructure::coordinate + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
                if (sys != kInvalidId) has_swarm.insert(sys);
            }
        }
    }

    // 挂在天体上的 deposit，按天体 id 归堆
    std::unordered_map<uint32_t, std::vector<std::string>> deposits;
    ForEachRef(base_address_, sdk::db::CDeposit, [&](uint32_t, void* d) {
        uintptr_t holder = (uintptr_t)d + sdk::ent::CDeposit::deposit_holder;
        if (ReadOr<uint32_t>(holder + kMetaRefType, kHolderNone) != kHolderPlanet) return;
        const uint32_t pid = ReadOr<uint32_t>(holder + kMetaRefId, kInvalidId);
        std::string key = KeyOf((uintptr_t)d + sdk::ent::CDeposit::type, kTypeKey);
        if (pid != kInvalidId && !key.empty()) deposits[pid].push_back(key);
    });
    auto parse_deposit = [](const std::string& key, std::string* kind, uint32_t* amount) {
        const size_t us = key.rfind('_');
        if (us == std::string::npos) return false;
        const int n = std::atoi(key.c_str() + us + 1);
        if (n <= 0) return false;
        std::string k = key.substr(0, us);
        if (k.rfind("d_", 0) == 0) k = k.substr(2);
        *kind = k;
        *amount = (uint32_t)n;
        return true;
    };

    nlohmann::json rows = nlohmann::json::array();
    for (const auto& sys : s.systems) {
        auto pit = s.planets.find(sys.id);
        if (pit == s.planets.end() || pit->second.empty()) continue;
        if (has_swarm.count(sys.id)) continue;
        int stars = 0;
        uint32_t star_id = kInvalidId;
        std::string star_cls;
        uint32_t bodies = 0;
        for (const auto& p : pit->second) {
            bodies++;
            const std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
            if (!is_star(cls)) continue;
            stars++;
            star_id = p.id;
            star_cls = cls;
        }
        if (stars != 1 || star_id == kInvalidId) continue;
        if (is_excluded_star(star_cls)) continue;

        double value = 0;
        uint32_t energy = 0, research = 0, strategic = 0, artifacts = 0, minerals = 0;
        auto dit = deposits.find(star_id);
        if (dit != deposits.end()) {
            for (const auto& key : dit->second) {
                std::string kind;
                uint32_t amount = 0;
                if (!parse_deposit(key, &kind, &amount)) continue;
                if (kind == "energy") {
                    energy += amount;
                    value += amount * 1.0;
                } else if (kind == "physics" || kind == "society" || kind == "engineering") {
                    research += amount;
                    value += amount * 4.0;  // 2 科研点 = 8 能量币
                } else if (kind == "minerals") {
                    minerals += amount;
                    value += amount * 1.0;
                } else if (kind == "minor_artifacts" || kind == "rare_crystals" || kind == "exotic_gases" ||
                           kind == "volatile_motes" || kind == "living_metal" || kind == "zro" ||
                           kind == "dark_matter" || kind == "nanites") {
                    if (kind == "minor_artifacts") artifacts += amount;
                    else strategic += amount;
                    value += amount * 8.0;  // 1 稀有文物 = 1 战略资源 = 8 能量币
                }
            }
        }
        if (value <= 0) continue;
        auto dit2 = dist.find(sys.id);
        const uint32_t owner = Owner(sys.obj);
        nlohmann::json row = { {"system_id", sys.id},
                               {"system", SystemName(sys.id, sys.obj)},
                               {"star_class", star_cls},
                               {"star_class_name", SafeLocalize(base_address_, star_cls)},
                               {"star_planet_id", star_id},
                               {"bodies", bodies},
                               {"energy", energy},
                               {"research", research},
                               {"strategic", strategic},
                               {"artifacts", artifacts},
                               {"minerals", minerals},
                               {"value", value},
                               {"jumps", dit2 == dist.end() ? nlohmann::json(nullptr) : nlohmann::json(dit2->second)},
                               {"owner_id", owner == kInvalidId ? nlohmann::json(nullptr) : nlohmann::json(owner)},
                               {"owner", owner == kInvalidId ? "" : CountryName(owner)} };
        if (fleet_id != kInvalidId) {
            void* type = MegaTypeByKey(base_address_, "dyson_swarm_1");
            std::string why;
            const bool ok = type && BuildMegaCommand(fleet_id, type, star_id, kQueueClear).IsValid(&why);
            row["buildable"] = ok;
            if (ok) row["site_planet_id"] = star_id;
            else if (!why.empty()) row["reason"] = why;
        }
        rows.push_back(row);
    }

    if (sort_by_value) {
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("value", 0.0) > b.value("value", 0.0);
        });
    } else {
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            const bool ja = !a["jumps"].is_null();
            const bool jb = !b["jumps"].is_null();
            if (ja != jb) return ja;
            if (ja && jb && a["jumps"].get<int>() != b["jumps"].get<int>()) return a["jumps"].get<int>() < b["jumps"].get<int>();
            return a.value("value", 0.0) > b.value("value", 0.0);
        });
    }
    if (rows.size() > limit) {
        nlohmann::json cut = nlohmann::json::array();
        for (size_t i = 0; i < limit; ++i) cut.push_back(rows[i]);
        rows = cut;
    }
    return { {"count", rows.size()}, {"from_system_id", origin}, {"sort", sort_by_value ? "value" : "jumps"}, {"systems", rows} };
}

nlohmann::json GalaxyManager::FindArcFurnaceSystems(uint32_t min_bodies, uint32_t limit, uint32_t fleet_id,
                                                   uint32_t from_system_id, bool sort_by_jumps) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (limit == 0 || limit > 200) limit = 30;

    // 起点：显式给的 > 工程船所在星系 > 首都
    uint32_t origin = from_system_id;
    if (origin == kInvalidId && fleet_id != kInvalidId) {
        for (const auto& sys : s.systems) {
            for (uint32_t f : RefArray((uintptr_t)sys.obj + sdk::ent::CGalacticObject::fleet_presence)) {
                if ((f & 0xFFFFFFu) == (fleet_id & 0xFFFFFFu)) origin = sys.id;
            }
            if (origin != kInvalidId) break;
        }
    }
    if (origin == kInvalidId) origin = CapitalSystem(s);
    // 从起点做一次 BFS，得到每个星系的跳数
    std::unordered_map<uint32_t, int> dist;
    if (s.index.count(origin)) {
        dist[origin] = 0;
        std::deque<uint32_t> q{ origin };
        while (!q.empty()) {
            const uint32_t id = q.front();
            q.pop_front();
            const int d = dist[id];
            for (const auto& l : s.systems[s.index[id]].lanes) {
                if (s.index.count(l.first) && !dist.count(l.first)) {
                    dist[l.first] = d + 1;
                    q.push_back(l.first);
                }
            }
        }
    }

    // 已有电弧熔炉的星系
    std::unordered_set<uint32_t> has_furnace;
    {
        void* db = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (Read((const void*)(base_address_ + sdk::db::CMegaStructure), &db) && db &&
            Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            Read((const void*)((uintptr_t)db + 0x20), &cap) && cap > 0 && cap < 100000) {
            for (uint32_t i = 0; i < cap; ++i) {
                void* m = nullptr;
                if (!Read((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &m) || !m) continue;
                const std::string type = KeyOf((uintptr_t)m + sdk::ent::CMegaStructure::type, kTypeKey);
                if (type.find("arc_furnace") == std::string::npos) continue;
                const uint32_t sys = ReadOr<uint32_t>(
                    (uintptr_t)m + sdk::ent::CMegaStructure::coordinate + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
                if (sys != kInvalidId) has_furnace.insert(sys);
            }
        }
    }

    nlohmann::json rows = nlohmann::json::array();
    for (const auto& sys : s.systems) {
        auto pit = s.planets.find(sys.id);
        if (pit == s.planets.end() || pit->second.empty()) continue;
        if (has_furnace.count(sys.id)) continue;
        const bool known = Intel(s, sys.obj) >= 1;
        if (!known) continue;
        uint32_t bodies = 0, molten = 0;
        for (const auto& p : pit->second) {
            bodies++;
            const std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
            if (cls == "pc_molten") molten++;
        }
        if (molten == 0 || bodies < min_bodies) continue;
        const uint32_t owner = Owner(sys.obj);
        auto dit = dist.find(sys.id);
        rows.push_back({ {"system_id", sys.id},
                         {"system", SystemName(sys.id, sys.obj)},
                         {"jumps", dit == dist.end() ? nlohmann::json(nullptr) : nlohmann::json(dit->second)},
                         {"owner_id", owner == kInvalidId ? nlohmann::json(nullptr) : nlohmann::json(owner)},
                         {"owner", owner == kInvalidId ? "" : CountryName(owner)},
                         {"bodies", bodies},
                         {"molten", molten} });
    }
    // 给了工程船就顺带验一次：这星系现在到底投不投得出去（引擎 IsValid 才算数）
    if (fleet_id != kInvalidId) {
        void* type = MegaTypeByKey(base_address_, "orbital_arc_furnace_1");
        for (auto& row : rows) {
            if (!type) break;
            const uint32_t sys_id = row.value("system_id", kInvalidId);
            auto pit = s.planets.find(sys_id);
            if (pit == s.planets.end()) continue;
            for (const auto& p : pit->second) {
                const std::string cls = KeyOf((uintptr_t)p.obj + sdk::ent::CPlanet::planet_class, kPlanetClassKey);
                if (cls != "pc_molten") continue;
                std::string why;
                if (BuildMegaCommand(fleet_id, type, p.id, kQueueClear).IsValid(&why)) {
                    row["site_planet_id"] = p.id;
                    row["site_name"] = PersistentNameText((const void*)((uintptr_t)p.obj + sdk::ent::CPlanet::name));
                    break;
                }
                if (!why.empty() && !row.contains("reason")) row["reason"] = why;
            }
            row["buildable"] = row.contains("site_planet_id");
        }
    }

    if (sort_by_jumps) {
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            const bool ja = !a["jumps"].is_null();
            const bool jb = !b["jumps"].is_null();
            if (ja != jb) return ja;
            if (ja && jb && a["jumps"].get<int>() != b["jumps"].get<int>()) return a["jumps"].get<int>() < b["jumps"].get<int>();
            return a.value("bodies", 0) > b.value("bodies", 0);
        });
    } else {
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("bodies", 0) > b.value("bodies", 0);
        });
    }
    if (rows.size() > limit) {
        nlohmann::json cut = nlohmann::json::array();
        for (size_t i = 0; i < limit; ++i) cut.push_back(rows[i]);
        rows = cut;
    }
    return { {"count", rows.size()}, {"from_system_id", origin}, {"systems", rows} };
}

nlohmann::json GalaxyManager::GetMegastructuresJson() {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    namespace ms = sdk::ent::CMegaStructure;
    namespace up = sdk::ent::SMegaStructureUpgrade;

    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!(Read((const void*)(base_address_ + sdk::db::CMegaStructure), &db) && db &&
          Read((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
          Read((const void*)((uintptr_t)db + 0x20), &cap) && cap > 0 && cap < 100000)) {
        return { {"error", "megastructure table not readable"} };
    }

    nlohmann::json rows = nlohmann::json::array();
    uint32_t total = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        void* m = nullptr;
        if (!Read((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &m) || !m) continue;
        const uint32_t id = ReadOr<uint32_t>((uintptr_t)m + sdk::rt::CMegaStructure_id, kInvalidId);
        if (id == kInvalidId || (id & 0xFFFFFFu) != i) continue;
        total++;
        const uint32_t owner = ReadOr<uint32_t>((uintptr_t)m + ms::owner, kInvalidId);
        const uint32_t system_id = ReadOr<uint32_t>((uintptr_t)m + ms::coordinate + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
        auto sit = s.index.find(system_id);
        const bool known = sit != s.index.end() && Intel(s, s.systems[sit->second].obj) >= 2;
        if (owner != s.player_id && !known) continue;

        std::string type = KeyOf((uintptr_t)m + ms::type, kTypeKey);
        nlohmann::json row = { {"id", id},
                               {"type", type},
                               {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)},
                               {"owner_id", owner == kInvalidId ? nlohmann::json(nullptr) : nlohmann::json(owner)},
                               {"owner", owner == kInvalidId ? "" : CountryName(owner)} };
        if (system_id != kInvalidId) {
            row["system_id"] = system_id;
            if (sit != s.index.end()) row["system"] = SystemName(system_id, s.systems[sit->second].obj);
        }
        const uint32_t planet = ReadOr<uint32_t>((uintptr_t)m + ms::planet, kInvalidId);
        if (planet != kInvalidId) row["planet_id"] = planet;
        if (ReadOr<uint8_t>((uintptr_t)m + ms::is_dismantling, 0)) row["dismantling"] = true;
        const std::string to = KeyOf((uintptr_t)m + up::upgrade_to, kTypeKey);
        if (!to.empty()) {
            row["upgrading"] = { {"to", to},
                                 {"to_name", SafeLocalize(base_address_, to)},
                                 {"progress_days", std::round(ReadOr<int64_t>((uintptr_t)m + up::progress, 0) / kFixed)} };
        }
        rows.push_back(row);
    }
    return { {"count", rows.size()}, {"table_total", total}, {"megastructures", rows} };
}

nlohmann::json GalaxyManager::GetMegastructureJson(uint32_t mega_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    namespace ms = sdk::ent::CMegaStructure;
    namespace up = sdk::ent::SMegaStructureUpgrade;
    void* m = RefLookup(base_address_, sdk::db::CMegaStructure, mega_id);
    if (!m || ReadOr<uint32_t>((uintptr_t)m + sdk::rt::CMegaStructure_id, kInvalidId) != mega_id) {
        return { {"error", "Unknown megastructure id: " + std::to_string(mega_id)} };
    }
    const uint32_t owner = ReadOr<uint32_t>((uintptr_t)m + ms::owner, kInvalidId);
    const uint32_t system_id = ReadOr<uint32_t>((uintptr_t)m + ms::coordinate + sdk::ent::CCelestialCoordinate::origin, kInvalidId);
    // only what the player can see: own megastructures, or ones in a system the player knows
    auto sit = s.index.find(system_id);
    if (owner != s.player_id && (sit == s.index.end() || Intel(s, s.systems[sit->second].obj) < 2)) {
        return { {"error", "Megastructure " + std::to_string(mega_id) + " is not visible to the player"} };
    }
    std::string type = KeyOf((uintptr_t)m + ms::type, kTypeKey);
    nlohmann::json out = { {"id", mega_id}, {"type", type}, {"type_name", type.empty() ? "" : SafeLocalize(base_address_, type)} };
    std::string name = PersistentNameText((const void*)((uintptr_t)m + ms::name));
    if (!name.empty()) out["name"] = name;
    if (system_id != kInvalidId) {
        out["system_id"] = system_id;
        if (sit != s.index.end()) out["system"] = SystemName(system_id, s.systems[sit->second].obj);
    }
    if (owner != kInvalidId) {
        out["owner_id"] = owner;
        out["owner"] = CountryName(owner);
    }
    uint32_t planet = ReadOr<uint32_t>((uintptr_t)m + ms::planet, kInvalidId);
    if (planet != kInvalidId) out["planet_id"] = planet;
    if (ReadOr<uint8_t>((uintptr_t)m + ms::is_dismantling, 0)) out["dismantling"] = true;
    // an upgrade under way (SMegaStructureUpgrade embedded at CMegaStructure::upgrade; the SDK's
    // offsets for it are CMegaStructure's); progress counts the days done
    std::string to = KeyOf((uintptr_t)m + up::upgrade_to, kTypeKey);
    if (!to.empty()) {
        out["upgrading"] = { {"to", to}, {"to_name", SafeLocalize(base_address_, to)},
                             {"progress_days", std::round(ReadOr<int64_t>((uintptr_t)m + up::progress, 0) / kFixed)},
                             {"halted", ReadOr<int32_t>((uintptr_t)m + up::halted, 0) != 0} };
    }
    if (owner != s.player_id) return out;
    nlohmann::json ups = nlohmann::json::array();
    const void* scope = (const void*)((uintptr_t)m + sdk::rt::CMegaStructure_economic_scope);
    for (void* t : MegaUpgrades(base_address_, s.player, m)) {
        std::string key = ReadString((uintptr_t)t + kTypeKey);
        nlohmann::json u = { {"type", key}, {"name", SafeLocalize(base_address_, key)},
                             {"cost", MegaCost(base_address_, s.player_id, scope, t)} };
        std::string why;
        auto cmd = UpgradeCommand(s.player_id, mega_id, t);
        u["possible"] = cmd.IsValid(&why);
        if (!why.empty()) u["reason"] = why;
        ups.push_back(u);
    }
    out["upgrades"] = ups;
    return out;
}

nlohmann::json GalaxyManager::UpgradeMegastructure(uint32_t mega_id, const std::string& type_key) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    void* m = RefLookup(base_address_, sdk::db::CMegaStructure, mega_id);
    if (!m || ReadOr<uint32_t>((uintptr_t)m + sdk::rt::CMegaStructure_id, kInvalidId) != mega_id ||
        ReadOr<uint32_t>((uintptr_t)m + sdk::ent::CMegaStructure::owner, kInvalidId) != s.player_id) {
        return { {"success", false}, {"error", "Megastructure " + std::to_string(mega_id) + " is not one of the player's"} };
    }
    auto ups = MegaUpgrades(base_address_, s.player, m);
    void* type = nullptr;
    if (type_key.empty()) {
        if (ups.size() != 1) {
            return { {"success", false}, {"error", ups.empty() ? "This megastructure offers no upgrade"
                                                                : "Several upgrades are offered: pass type (stellaris_get_megastructure)"} };
        }
        type = ups[0];
    } else {
        for (void* t : ups) {
            if (ReadString((uintptr_t)t + kTypeKey) == type_key) type = t;
        }
        if (!type) return { {"success", false}, {"error", "Not an upgrade this megastructure offers: " + type_key} };
    }
    std::string key = ReadString((uintptr_t)type + kTypeKey);
    nlohmann::json cost = MegaCost(base_address_, s.player_id,
                                   (const void*)((uintptr_t)m + sdk::rt::CMegaStructure_economic_scope), type);
    auto cmd = UpgradeCommand(s.player_id, mega_id, type);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the upgrade" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    return { {"success", true}, {"megastructure_id", mega_id}, {"upgrade_to", key},
             {"upgrade_name", SafeLocalize(base_address_, key)}, {"cost", cost},
             {"message", "Upgrade started; stellaris_get_megastructure shows its progress"} };
}

nlohmann::json GalaxyManager::GetBuildableMegastructures(uint32_t fleet_id, uint32_t system_id) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* fleet = fleets::Find(base_address_, fleet_id);
    if (!fleet) return { {"error", "Unknown fleet id: " + std::to_string(fleet_id)} };
    if (system_id != kInvalidId && !s.index.count(system_id)) return { {"error", "Unknown system id: " + std::to_string(system_id)} };
    const void* scope = (const void*)((uintptr_t)s.player + sdk::rt::CCountry_economic_scope);
    nlohmann::json list = nlohmann::json::array();
    void* system = system_id != kInvalidId ? s.systems[s.index[system_id]].obj : nullptr;
    for (void* t : MegaTypes(base_address_)) {
        // the menu (CCountry::ListBuildableMegaStructures): potential in the menu and buildable by
        // this fleet; upgrade stages and repairs fail the second
        if (!EngineByte3(base_address_ + sdk::fn::CMegaStructureType_IsPotentialInBuildMenu, t, s.player, fleet) ||
            !EngineByte3(base_address_ + sdk::fn::CMegaStructureType_IsPotentiallyBuildable, t, s.player, fleet)) {
            continue;
        }
        std::string key = ReadString((uintptr_t)t + kTypeKey);
        const int bt = ReadOr<int32_t>((uintptr_t)t + sdk::rt::CMegaStructureType_build_type, -1);
        nlohmann::json row = { {"type", key}, {"name", SafeLocalize(base_address_, key)}, {"placement", BuildTypeName(bt)},
                               {"cost", MegaCost(base_address_, s.player_id, scope, t)} };
        if (system) {
            nlohmann::json sites = nlohmann::json::array();
            std::map<std::string, int> refused;
            bool any_place = false;
            if (bt == kBuildAtPlanet) {
                // the system's planets and stars that meet the placement rules, then the order
                auto pit = s.planets.find(system_id);
                for (const auto& p : pit != s.planets.end() ? pit->second : std::vector<PlanetRef>{}) {
                    if (!PlacementPossible(base_address_, t, p.obj, s.player)) continue;
                    any_place = true;
                    std::string why;
                    if (BuildMegaCommand(fleet_id, t, p.id, kQueueClear).IsValid(&why)) {
                        sites.push_back({ {"planet_id", p.id},
                                          {"name", PersistentNameText((const void*)((uintptr_t)p.obj + sdk::ent::CPlanet::name))} });
                    } else {
                        ++refused[why.empty() ? "refused" : why];
                    }
                }
            } else if (bt == kBuildInsideGravityWell || bt == kBuildOutsideGravityWell) {
                // open space: a point toward each neighbouring system
                for (const auto& lane : s.systems[s.index[system_id]].lanes) {
                    auto nit = s.index.find(lane.first);
                    int64_t x = 0, y = 0;
                    if (nit == s.index.end() || !OpenSpacePoint(base_address_, system, s.systems[nit->second].obj, bt, &x, &y)) continue;
                    any_place = true;
                    std::string why;
                    if (BuildMegaAtPointCommand(fleet_id, t, system_id, x, y, kQueueClear).IsValid(&why)) {
                        sites.push_back({ {"toward_system_id", lane.first},
                                          {"toward", SystemName(lane.first, s.systems[nit->second].obj)} });
                    } else {
                        ++refused[why.empty() ? "refused" : why];
                    }
                }
            }
            row["sites"] = sites;
            if (sites.empty()) {
                if (!refused.empty()) {
                    // the most common reason (cost, a construction already under way, a limit ...)
                    auto best = std::max_element(refused.begin(), refused.end(),
                                                 [](const auto& a, const auto& b) { return a.second < b.second; });
                    row["reason"] = best->first;
                } else if (!any_place) {
                    row["reason"] = bt == kBuildAtPlanet ? "No planet or star in this system meets its placement rules"
                                                         : "No place in this system to build it";
                }
            }
        }
        list.push_back(row);
    }
    nlohmann::json out = { {"fleet_id", fleet_id}, {"megastructures", list} };
    if (system_id != kInvalidId) out["system_id"] = system_id;
    if (list.empty()) out["note"] = "This fleet's build menu lists no megastructures (not a construction ship, or none unlocked)";
    return out;
}

nlohmann::json GalaxyManager::BuildMegastructure(uint32_t fleet_id, const std::string& type_key, uint32_t planet_id,
                                                 uint32_t system_id, uint32_t toward_system_id, bool queue) {
    Snapshot s = Take();
    if (!s.player) return { {"error", "No player country (not in game?)"} };
    if (!OwnFleet(s, fleet_id)) return { {"success", false}, {"error", "Fleet " + std::to_string(fleet_id) + " is not one of the player's fleets"} };
    void* type = MegaTypeByKey(base_address_, type_key);
    if (!type) return { {"success", false}, {"error", "Unknown megastructure type: " + type_key} };
    const int bt = ReadOr<int32_t>((uintptr_t)type + sdk::rt::CMegaStructureType_build_type, -1);
    const uint32_t mode = queue ? kQueueLast : kQueueClear;
    nlohmann::json out = { {"fleet_id", fleet_id}, {"type", type_key}, {"name", SafeLocalize(base_address_, type_key)},
                           {"placement", BuildTypeName(bt)}, {"queued", queue} };
    NativeCommand cmd = [&]() {
        if (bt == kBuildAtPlanet) {
            void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
            if (planet) {
                out["planet_id"] = planet_id;
                out["planet"] = PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name));
            }
            return BuildMegaCommand(fleet_id, type, planet ? planet_id : kInvalidId, mode);
        }
        // open space: the point toward the neighbouring system, as the menu listed it
        int64_t x = 0, y = 0;
        auto sit = s.index.find(system_id);
        auto tit = s.index.find(toward_system_id);
        bool lane = sit != s.index.end() && tit != s.index.end() &&
                    std::any_of(s.systems[sit->second].lanes.begin(), s.systems[sit->second].lanes.end(),
                                [&](const auto& l) { return l.first == toward_system_id; });
        if (lane && OpenSpacePoint(base_address_, s.systems[sit->second].obj, s.systems[tit->second].obj, bt, &x, &y)) {
            out["system_id"] = system_id;
            out["toward_system_id"] = toward_system_id;
        }
        return BuildMegaAtPointCommand(fleet_id, type, out.contains("system_id") ? system_id : kInvalidId, x, y, mode);
    }();
    if (bt == kBuildAtPlanet && !out.contains("planet_id")) {
        return { {"success", false}, {"error", "This type is built at a planet or star: pass planet_id (stellaris_get_buildable_megastructures sites)"} };
    }
    if (bt != kBuildAtPlanet && !out.contains("system_id")) {
        return { {"success", false}, {"error", "This type is built in open space: pass system_id and toward_system_id, a hyperlane neighbour "
                                               "(stellaris_get_buildable_megastructures sites)"} };
    }
    out["cost"] = MegaCost(base_address_, s.player_id, (const void*)((uintptr_t)s.player + sdk::rt::CCountry_economic_scope), type);
    std::string why;
    if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused the construction" : why} };
    if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
    out["success"] = true;
    out["message"] = "Build order posted: the ship travels there and starts the construction site";
    return out;
}

}  // namespace bridge








