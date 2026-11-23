#include "situation_log_manager.hpp"
#include "game_state.hpp"
#include "sdk/stellaris_sdk.hpp"
#include "command_builder.hpp"
#include "fleet_access.hpp"
#include "species_manager.hpp"
#include <cstring>

namespace bridge {

// Safe memory access helpers
static bool SafeReadPtr(const void* addr, void** out) {
    __try {
        *out = *(void**)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU32(const void* addr, uint32_t* out) {
    __try {
        *out = *(const uint32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadI32(const void* addr, int32_t* out) {
    __try {
        *out = *(const int32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU64(const void* addr, uint64_t* out) {
    __try {
        *out = *(const uint64_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCopyChars(char* dest, const char* src, size_t count) {
    __try {
        memcpy(dest, src, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RawPdxString {
    union {
        char buf[16];
        char* heap_ptr;
    };
    uint64_t size;
    uint64_t capacity;
};

static bool SafeReadPdxString(const void* pdx_str_addr, std::string& out) {
    out.clear();
    if (!pdx_str_addr) return false;

    RawPdxString raw{};
    if (!SafeCopyChars((char*)&raw, (const char*)pdx_str_addr, sizeof(RawPdxString))) {
        return false;
    }

    if (raw.size == 0 || raw.size > 1024) return false;

    if (raw.capacity < 16) {
        size_t len = raw.size < 16 ? (size_t)raw.size : 15;
        char temp[16]{ 0 };
        if (SafeCopyChars(temp, raw.buf, len)) {
            out = std::string(temp, len);
            return true;
        }
    } else if (raw.heap_ptr) {
        uintptr_t addr = (uintptr_t)raw.heap_ptr;
        if (addr > 0x10000 && addr < 0x7FFFFFFFFFFF) {
            size_t len = raw.size < 512 ? (size_t)raw.size : 512;
            std::string temp(len, '\0');
            if (SafeCopyChars(&temp[0], raw.heap_ptr, len)) {
                out = temp;
                return true;
            }
        }
    }
    return false;
}

static bool SafeReadI64(const void* addr, int64_t* out) {
    __try {
        *out = *(const int64_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

// CPdxArray layouts in CCountryEventManager (serializer 0x802E10): special_project is read as
// {data, ..., size at +0xC}; anomalies (ref_array<TPdxRef<CPlanet>>) is an array object
// {vtable, data at +8, size at +0x14}.
constexpr std::ptrdiff_t kProjectsSize = 0xC;
constexpr std::ptrdiff_t kRefArrayData = 0x8;
constexpr std::ptrdiff_t kRefArraySize = 0x14;
// The special project type's key (the CSpecialProjectInstance serializer writes [type] + 0x18) and
// the anomaly category's key (the CPlanet serializer writes [anomaly] + 0x20).
constexpr std::ptrdiff_t kProjectTypeKey = 0x18;
constexpr std::ptrdiff_t kAnomalyKey = 0x20;
// CDebris compares its own id at +0x20 in TPdxRef lookups (CSpecialProjectInstance::ShouldAbort).
constexpr uint32_t kInvalidId = 0xFFFFFFFF;

// TPdxRefDatabase<T>: arr at +0x18 (16-byte slots, object at +8), capacity at +0x20.
void* RefLookup(uintptr_t base, uintptr_t db_rva, uint32_t id, std::ptrdiff_t id_off = -1) {
    if (id == kInvalidId) return nullptr;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base + db_rva), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap)) {
        return nullptr;
    }
    uint32_t idx = id & 0xFFFFFF;
    void* obj = nullptr;
    if (idx >= cap || !SafeReadPtr((const void*)((uintptr_t)arr + idx * 16 + 8), &obj) || !obj) return nullptr;
    uint32_t own = 0;
    if (id_off >= 0 && (!SafeReadU32((const void*)((uintptr_t)obj + id_off), &own) || own != id)) return nullptr;
    return obj;
}

struct PredicateCtx {
    uintptr_t fn;
    const void* obj;
    bool result;
};

void CallPredicate0(void* c, void*) {
    auto* x = (PredicateCtx*)c;
    x->result = ((bool (*)(const void*))x->fn)(x->obj);
}

bool EnginePredicate(uintptr_t fn, const void* obj) {
    PredicateCtx ctx{ fn, obj, false };
    return CommandBuilder::Get().CallGuarded(&CallPredicate0, &ctx) && ctx.result;
}

void* EventManager(void* country) {
    return country ? (void*)((uintptr_t)country + sdk::ent::CCountry::events) : nullptr;
}

}  // namespace

std::vector<SpecialProjectItem> SituationLogManager::ReadSpecialProjects(void* country) {
    std::vector<SpecialProjectItem> out;
    void* em = EventManager(country);
    void* data = nullptr;
    uint32_t count = 0;
    const uintptr_t list = (uintptr_t)em + sdk::ent::CCountryEventManager::special_project;
    if (!em || !SafeReadPtr((const void*)list, &data) || !data ||
        !SafeReadU32((const void*)(list + kProjectsSize), &count) || count > 1000) {
        return out;
    }
    namespace sp = sdk::ent::CSpecialProjectInstance;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)data + i * 8), &p) || !p) continue;
        SpecialProjectItem item{};
        SafeReadU32((const void*)((uintptr_t)p + sp::id), &item.id);
        SafeReadI32((const void*)((uintptr_t)p + sp::status), &item.status);
        SafeReadI32((const void*)((uintptr_t)p + sp::days_left), &item.days_left);
        void* type = nullptr;
        // species modification projects hold the null type, whose key is empty
        if (SafeReadPtr((const void*)((uintptr_t)p + sp::special_project), &type) && type) {
            SafeReadPdxString((const void*)((uintptr_t)type + kProjectTypeKey), item.key);
        }
        // CSpecialProjectInstance::GetName: species modification and uplift projects are named
        // after the species, debris projects after the system of the debris, the rest by type.
        uint32_t convert_to = kInvalidId;
        SafeReadU32((const void*)((uintptr_t)p + sp::convert_to), &convert_to);
        uint32_t debris_id = kInvalidId;
        SafeReadU32((const void*)((uintptr_t)p + sp::debris), &debris_id);
        // CSpecialProjectInstance::coordinate is a spatial reference {vtable, type +8, id +0xC}
        SafeReadU32((const void*)((uintptr_t)p + sp::coordinate + 0x8), &item.location_type);
        SafeReadU32((const void*)((uintptr_t)p + sp::coordinate + 0xC), &item.location_id);
        // 位置名：按类型去对应的库取（id 带 tag 高位，先掩掉）
        {
            const uint32_t lid = item.location_id & 0xFFFFFF;
            if (item.location_type == 2) {
                if (void* o = RefLookup(base_address_, sdk::db::CPlanet, lid))
                    item.location_name = PersistentNameText((const void*)((uintptr_t)o + sdk::ent::CPlanet::name));
            } else if (item.location_type == 3) {
                if (void* o = RefLookup(base_address_, sdk::db::CFleet, lid))
                    item.location_name = PersistentNameText((const void*)((uintptr_t)o + sdk::ent::CFleet::name));
            } else if (item.location_type == 4) {
                if (void* o = RefLookup(base_address_, sdk::db::CGalacticObject, lid))
                    item.location_name = PersistentNameText((const void*)((uintptr_t)o + sdk::ent::CGalacticObject::name));
            } else if (item.location_type == 5) {
                if (void* o = RefLookup(base_address_, sdk::db::CDebris, lid)) {
                    uint32_t sys = kInvalidId;
                    SafeReadU32((const void*)((uintptr_t)o + sdk::ent::CDebris::coordinate +
                                              sdk::ent::CCelestialCoordinate::origin), &sys);
                    if (void* s2 = RefLookup(base_address_, sdk::db::CGalacticObject, sys))
                        item.location_name = PersistentNameText((const void*)((uintptr_t)s2 + sdk::ent::CGalacticObject::name));
                }
            } else if (item.location_type == 9) {
                if (void* o = RefLookup(base_address_, sdk::db::CAstralRift, lid))
                    SafeReadPdxString((const void*)((uintptr_t)o + sdk::ent::CAstralRift::name), item.location_name);
            }
        }
        const bool species_mod = EnginePredicate(base_address_ + sdk::fn::CSpecialProjectInstance_IsSpeciesModification, p);
        const bool uplift = !species_mod && EnginePredicate(base_address_ + sdk::fn::CSpecialProjectInstance_IsUplift, p);
        if (species_mod || uplift) {
            item.kind = species_mod ? "species_modification" : "uplift";
            item.species_id = convert_to;
            std::string species;
            if (void* sp_obj = SpeciesManager::Get().FindSpeciesPtr(convert_to)) {
                species = PersistentNameText((const void*)((uintptr_t)sp_obj + sdk::ent::CSpecies::name));
            }
            item.key = species_mod ? "MOD_TRAIT_PROJECT" : "UPLIFT_PROJECT";
            item.name = LocalizeWithParam(base_address_, item.key, species_mod ? "TEMPLATE" : "SPECIES", species);
        } else if (void* debris = RefLookup(base_address_, sdk::db::CDebris, debris_id, sdk::rt::CDebris_id)) {
            item.kind = "debris";
            item.debris_id = debris_id;
            uint32_t system_id = kInvalidId;
            SafeReadU32((const void*)((uintptr_t)debris + sdk::ent::CDebris::coordinate +
                                      sdk::ent::CCelestialCoordinate::origin), &system_id);
            std::string system;
            if (void* sys = RefLookup(base_address_, sdk::db::CGalacticObject, system_id)) {
                system = PersistentNameText((const void*)((uintptr_t)sys + sdk::ent::CGalacticObject::name));
            }
            item.key = "SPECIAL_PROJECT_DEBRIS";
            item.name = LocalizeWithParam(base_address_, item.key, "SYSTEM", system);
        } else {
            item.kind = "project";
            item.name = item.key.empty() ? "" : LocalizeKey(item.key);
        }
        out.push_back(item);
    }
    return out;
}

std::vector<AnomalyItem> SituationLogManager::ReadAnomalies(void* country) {
    std::vector<AnomalyItem> out;
    void* em = EventManager(country);
    if (!em) return out;
    const uintptr_t arr = (uintptr_t)em + sdk::ent::CCountryEventManager::anomalies;
    void* data = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)(arr + kRefArrayData), &data) || !data ||
        !SafeReadU32((const void*)(arr + kRefArraySize), &count) || count > 4096) {
        return out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t planet_id = kInvalidId;
        if (!SafeReadU32((const void*)((uintptr_t)data + i * 4), &planet_id)) continue;
        void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
        void* anomaly = nullptr;
        // the planet keeps its anomaly (category) until it is researched, then holds a null object
        if (!planet || !SafeReadPtr((const void*)((uintptr_t)planet + sdk::ent::CPlanet::anomaly), &anomaly) ||
            !IsRealObject(anomaly)) {
            continue;
        }
        AnomalyItem item{};
        item.planet_id = planet_id;
        SafeReadPdxString((const void*)((uintptr_t)anomaly + kAnomalyKey), item.key);
        if (item.key.empty()) continue;
        item.name = LocalizeKey(item.key);
        item.planet_name = PersistentNameText((const void*)((uintptr_t)planet + sdk::ent::CPlanet::name));
        // 难度：anomaly 对象 +0x60（1–10 档；进度不在这里，进度在科研船上）
        SafeReadI32((const void*)((uintptr_t)anomaly + 0x60), &item.difficulty);
        // 星系：CPlanet::coordinate 的 origin 就是星系 id，再经 CGalacticObject 取名
        uint32_t sys_id = kInvalidId;
        SafeReadU32((const void*)((uintptr_t)planet + sdk::ent::CPlanet::coordinate +
                                  sdk::ent::CCelestialCoordinate::origin), &sys_id);
        if (sys_id != kInvalidId) {
            item.system_id = sys_id;
            if (void* sys = RefLookup(base_address_, sdk::db::CGalacticObject, sys_id)) {
                item.system_name = PersistentNameText((const void*)((uintptr_t)sys + sdk::ent::CGalacticObject::name));
            }
        }
        out.push_back(item);
    }
    return out;
}

// 只读：某颗行星上的 anomaly 对象（CPlanet::anomaly = +0x4B0）。用于对比「正在被调查」与「未调查」
// 的差异——列表只给名字，状态得从对象里找。
nlohmann::json SituationLogManager::ProbeAnomalyJson(uint32_t planet_id, int32_t offset, int32_t length) {
    if (offset < 0) offset = 0;
    if (length <= 0) length = 0x80;
    if (length > 0x400) length = 0x400;
    void* planet = RefLookup(base_address_, sdk::db::CPlanet, planet_id);
    if (!planet) return { {"error", "planet not found: " + std::to_string(planet_id)} };
    void* anomaly = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)planet + sdk::ent::CPlanet::anomaly), &anomaly) || !anomaly) {
        return { {"error", "planet has no anomaly object (researched already?)"} };
    }
    std::vector<uint8_t> buf((size_t)length, 0);
    if (!SafeCopyChars((char*)buf.data(), (const char*)((uintptr_t)anomaly + offset), (size_t)length)) {
        return { {"error", "reading that window faulted"} };
    }
    static const char* kHex = "0123456789abcdef";
    std::string hex, ascii;
    for (int i = 0; i < length; ++i) {
        const unsigned char c = buf[(size_t)i];
        if (i % 16 == 0) { if (!hex.empty()) hex += "\n"; char h[16]; snprintf(h, sizeof(h), "%04X  ", offset + i); hex += h; }
        hex.push_back(kHex[c >> 4]); hex.push_back(kHex[c & 0xF]); hex.push_back(' ');
        ascii.push_back(c >= 0x20 && c < 0x7f ? (char)c : '.');
        if (i % 16 == 15) ascii.push_back('\n');
    }
    nlohmann::json qw = nlohmann::json::array();
    for (int i = 0; i + 8 <= length; i += 8) {
        uint64_t v = 0; SafeReadU64((const void*)((uintptr_t)anomaly + offset + i), &v);
        char label[16]; snprintf(label, sizeof(label), "%04X", offset + i);
        qw.push_back({ {"at", label}, {"u64", v}, {"lo32", (uint32_t)(v & 0xFFFFFFFF)}, {"hi32", (uint32_t)(v >> 32)} });
    }
    std::string key; SafeReadPdxString((const void*)((uintptr_t)anomaly + kAnomalyKey), key);
    return { {"success", true}, {"planet_id", planet_id}, {"planet", (uint64_t)(uintptr_t)planet},
             {"anomaly", (uint64_t)(uintptr_t)anomaly}, {"key", key},
             {"offset", offset}, {"length", length}, {"hex", hex}, {"ascii", ascii}, {"qwords", qw} };
}
// 起/停一个「直接研究」类特殊项目：CResearchProjectCommand（token 0x2D8F）
//   country +0x20 / special_project +0x24；取消用 CCancelProjectCommand（0x31AC，字段相同）。
// 需要舰船的那类走的是 collect_data（CCollectDataFleetOrderCommand），不能用这条。
nlohmann::json SituationLogManager::ResearchProjectJson(uint32_t project_id, bool cancel) {
    if (project_id == 0xFFFFFFFF) return { {"success", false}, {"error", "project_id is required"} };
    const uint32_t cid = GetPlayerCountryId();
    nlohmann::json out;
    std::string why;
    if (cancel) {
        namespace cp = sdk::cmd::cancel_project_command;
        auto cmd = CommandBuilder::Get().Create(cp::kSpec);
        cmd.Set<uint32_t>(cp::country, cid).Set<uint32_t>(cp::special_project, project_id);
        if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused to cancel it" : why} };
        if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
        out = { {"success", true}, {"project_id", project_id}, {"cancelled", true} };
    } else {
        namespace rp = sdk::cmd::research_project_command;
        auto cmd = CommandBuilder::Get().Create(rp::kSpec);
        cmd.Set<uint32_t>(rp::country, cid).Set<uint32_t>(rp::special_project, project_id);
        if (!cmd.IsValid(&why)) return { {"success", false}, {"error", why.empty() ? "The game refused to start it" : why} };
        if (!cmd.Post(NativeCommand::Check::EngineGate)) return { {"success", false}, {"error", cmd.error()} };
        out = { {"success", true}, {"project_id", project_id}, {"started", true} };
    }
    return out;
}
SituationLogManager& SituationLogManager::Get() {
    static SituationLogManager instance;
    return instance;
}

bool SituationLogManager::Init(uintptr_t base_address) {
    base_address_ = base_address;


    LOGF("[SITUATION_LOG] Initialized (Base: 0x%llX)", (unsigned long long)base_address_);

    return true;
}

void* SituationLogManager::GetPlayerCountry() {
    return GameState::Get().GetPlayerCountry();  // the local player's country (not country 0)
}

uint32_t SituationLogManager::GetPlayerCountryId() {
    void* country = GetPlayerCountry();
    if (!country) return 0;

    uint32_t cid = 0;
    SafeReadU32((const void*)((uintptr_t)country + 0x20), &cid);
    return cid;
}

std::string SituationLogManager::LocalizeKey(const std::string& key) {
    return SafeLocalize(base_address_, key);
}

SituationLogSummary SituationLogManager::GetSummary() {
    SituationLogSummary summary{};
    if (!base_address_) return summary;

    uint32_t player_id = GetPlayerCountryId();

    // 1. Situations count for player empire from global entity manager
    void* sit_mgr = nullptr;
    if (SafeReadPtr((const void*)(base_address_ + sdk::db::CSituation), &sit_mgr) && sit_mgr) {
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) && cap > 0) {
            for (uint32_t i = 0; i < cap && i < 2048; ++i) {
                void* sit = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &sit) || !sit) {
                    continue;
                }
                uint32_t owner_id = 0;
                SafeReadU32((const void*)((uintptr_t)sit + 0x204), &owner_id);
                if (owner_id == player_id) {
                    summary.situations_count++;
                }
            }
        }
    }

    // special projects and anomalies of the player (CCountry::events, CCountryEventManager)
    void* country = GetPlayerCountry();
    summary.special_projects_count = (uint32_t)ReadSpecialProjects(country).size();
    summary.anomalies_count = (uint32_t)ReadAnomalies(country).size();
    return summary;
}

FullSituationLogState SituationLogManager::GetSituationLogState(bool player_only) {
    FullSituationLogState state;
    if (!base_address_) return state;

    uint32_t player_id = GetPlayerCountryId();
    state.summary = GetSummary();

    // 1. Extract Situations from Global Situation EntityManager
    void* sit_mgr = nullptr;
    if (SafeReadPtr((const void*)(base_address_ + sdk::db::CSituation), &sit_mgr) && sit_mgr) {
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) && cap > 0) {
            for (uint32_t i = 0; i < cap && i < 2048; ++i) {
                void* sit = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &sit) || !sit) {
                    continue;
                }

                uint32_t sit_id = 0;
                SafeReadU32((const void*)((uintptr_t)sit + 0x08), &sit_id);

                uint32_t owner_id = 0;
                SafeReadU32((const void*)((uintptr_t)sit + 0x204), &owner_id);

                bool is_player = (owner_id == player_id);
                if (player_only && !is_player) {
                    continue;
                }

                SituationItem item{};
                item.id = sit_id;
                item.owner_country_id = owner_id;
                item.is_player = is_player;

                void* type_ptr = nullptr;
                SafeReadPtr((const void*)((uintptr_t)sit + 0x208), &type_ptr);
                if (type_ptr) {
                    SafeReadPdxString((const void*)((uintptr_t)type_ptr + 0x20), item.key);
                    item.name = LocalizeKey(item.key);
                }

                void* approach_ptr = nullptr;
                SafeReadPtr((const void*)((uintptr_t)sit + 0x210), &approach_ptr);
                if (approach_ptr) {
                    SafeReadPdxString((const void*)((uintptr_t)approach_ptr + 0x18), item.current_approach);
                    item.current_approach_name = LocalizeKey(item.current_approach);
                }

                // CFixedPoint (x100000): a stage with `end = 1920` is cached as 192000000
                int64_t raw_prog = 0;
                SafeReadI64((const void*)((uintptr_t)sit + sdk::ent::CSituation::progress), &raw_prog);
                item.progress = (double)raw_prog / 100000.0;

                int64_t raw_rate = 0;
                SafeReadI64((const void*)((uintptr_t)sit + sdk::ent::CSituation::last_month_progress), &raw_rate);
                item.monthly_change = (double)raw_rate / 100000.0;

                // stage index (0-based) — CSituation::stage = +0x578, SDK tok 0x3d27
                SafeReadI32((const void*)((uintptr_t)sit + sdk::ent::CSituation::stage), &item.stage);

                // The per-stage absolute ranges are cached inline on the situation object:
                //   +0x590  capacity (10 here)   +0x594  count (4 here)
                //   +0x598  array of {begin, end} int64 fixed points, stride 0x10, one per stage
                // (deficit: (0,25) (25,50) (50,75) (75,100) — exactly what the UI prints as
                //  "this stage is in effect between x and y")
                uint32_t range_cnt = 0;
                SafeReadU32((const void*)((uintptr_t)sit + 0x594), &range_cnt);
                if (range_cnt > 0 && range_cnt <= 10) {
                    const uint32_t idx = (item.stage >= 0 && (uint32_t)item.stage < range_cnt) ? (uint32_t)item.stage : 0;
                    int64_t raw_min = 0, raw_max = 0;
                    SafeReadI64((const void*)((uintptr_t)sit + 0x598 + idx * 0x10), &raw_min);
                    SafeReadI64((const void*)((uintptr_t)sit + 0x598 + idx * 0x10 + 0x08), &raw_max);
                    item.stage_min = (double)raw_min / 100000.0;
                    item.stage_max = (double)raw_max / 100000.0;
                }

                state.situations.push_back(item);
            }
        }
    }

    void* country = GetPlayerCountry();
    state.special_projects = ReadSpecialProjects(country);
    state.anomalies = ReadAnomalies(country);
    return state;
}

nlohmann::json SituationLogManager::GetSituationLogJson(bool player_only) {
    FullSituationLogState state = GetSituationLogState(player_only);

    nlohmann::json sit_arr = nlohmann::json::array();
    for (const auto& item : state.situations) {
        sit_arr.push_back({
            {"id", item.id},
            {"key", item.key},
            {"name", item.name},
            {"current_approach", item.current_approach},
            {"current_approach_name", item.current_approach_name},
            {"progress", item.progress},
            {"monthly_change", item.monthly_change},
            {"stage", item.stage},
            {"stage_min", item.stage_min},
            {"stage_max", item.stage_max},
            {"owner_country_id", item.owner_country_id},
            {"is_player", item.is_player}
        });
    }

    nlohmann::json sp_arr = nlohmann::json::array();
    for (const auto& item : state.special_projects) {
        nlohmann::json j = {
            {"id", item.id},
            {"key", item.key},
            {"name", item.name},
            {"kind", item.kind}
        };
        j["status"] = item.status;
        if (item.days_left >= 0) j["days_left"] = item.days_left;  // -1: no deadline
        if (item.species_id != 0xFFFFFFFF) j["species_id"] = item.species_id;
        if (item.debris_id != 0xFFFFFFFF) j["debris_id"] = item.debris_id & 0xFFFFFF;  // 去掉 tag 高位
        // 位置：有位置的（location_type/id 不是 0xFFFFFFFF）说明要派舰船过去
        if (item.location_type != 0xFFFFFFFF) {
            j["location_type"] = item.location_type;
            j["location_id"] = item.location_id & 0xFFFFFF;  // 去掉 tag 高位
            if (!item.location_name.empty()) j["location_name"] = item.location_name;
            j["needs_ship"] = (item.location_type != 0 && item.location_id != 0xFFFFFFFF);
        } else {
            j["needs_ship"] = false;
        }
        sp_arr.push_back(j);
    }

    nlohmann::json anom_arr = nlohmann::json::array();
    for (const auto& item : state.anomalies) {
        nlohmann::json aj = {
            {"planet_id", item.planet_id},
            {"planet_name", item.planet_name},
            {"key", item.key},
            {"name", item.name}
        };
        if (item.system_id != 0xFFFFFFFF) aj["system_id"] = item.system_id;
        if (!item.system_name.empty()) aj["system_name"] = item.system_name;
        if (item.difficulty > 0) aj["difficulty"] = item.difficulty;
        anom_arr.push_back(aj);
    }

    return {
        {"summary", {
            {"situations_count", state.summary.situations_count},
            {"special_projects_count", state.special_projects.size()},
            {"anomalies_count", state.anomalies.size()}
        }},
        {"situations", sit_arr},
        {"special_projects", sp_arr},
        {"anomalies", anom_arr},
        {"player_only_filter", player_only}
    };
}

static nlohmann::json ProbeSituationTypeBody(uintptr_t base_address, uint32_t situation_id, uint32_t offset,
                                             uint32_t length, bool follow, uint64_t match_vtable,
                                             uint64_t vec_vtable) {
    void* sit_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address + sdk::db::CSituation), &sit_mgr) || !sit_mgr) {
        return { {"success", false}, {"error", "situation manager not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) || cap == 0) {
        return { {"success", false}, {"error", "situation array not readable"} };
    }

    void* sit = nullptr;
    for (uint32_t i = 0; i < cap && i < 2048; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &p) || !p) continue;
        uint32_t id = 0;
        SafeReadU32((const void*)((uintptr_t)p + 0x08), &id);
        if (id == situation_id) { sit = p; break; }
    }
    if (!sit) return { {"success", false}, {"error", "situation not found"} };

    void* type = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)sit + 0x208), &type) || !type) {
        return { {"success", false}, {"error", "situation type not readable"} };
    }

    if (length < 0x10) length = 0x10;
    if (length > 0x800) length = 0x800;
    const uintptr_t base = (uintptr_t)type + offset;

    std::string hex;
    char line[128];
    for (uint32_t off = 0; off < length; off += 16) {
        unsigned char b[16]{};
        const uint32_t n = (length - off < 16) ? (length - off) : 16;
        if (!SafeCopyChars((char*)b, (const char*)(base + off), n)) break;
        int w = snprintf(line, sizeof(line), "%04X:", offset + off);
        for (uint32_t i = 0; i < n; ++i) {
            w += snprintf(line + w, sizeof(line) - (size_t)w, " %02X", b[i]);
        }
        hex += line;
        hex += "\n";
    }

    // Follow every qword in the range and report the ones that land on an object carrying a key
    // string at +0x18 (approaches) or +0x20 (types). OFF BY DEFAULT: this is the part that can
    // dereference junk. Two guards matter here:
    //   1) a junk PdxString header can ask for an allocation (SEH would not catch that)
    //   2) garbage bytes are not valid UTF-8, and nlohmann::json::dump() THROWS on invalid UTF-8 —
    //      that throw happens on the IPC thread, outside this function, and kills the process with
    //      no crash dump. So only short printable-ASCII keys are ever put into the JSON.
    auto plausible_key = [](const std::string& s) {
        if (s.size() < 2 || s.size() > 64) return false;
        for (unsigned char c : s) {
            if (c < 0x20 || c > 0x7E) return false;
        }
        return true;
    };
    nlohmann::json strings = nlohmann::json::array();
    for (uint32_t off = 0; follow && off + 8 <= length && strings.size() < 60; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)type + offset + off), &p) || !p) continue;
        const uintptr_t pp = (uintptr_t)p;
        if (pp < 0x10000 || pp > 0x7FFFFFFFFFFF) continue;
        // vtable filter: when match_vtable is given, only report pointers to objects of that class
        // (cheap, and it is how the second approach definition is told apart from noise)
        if (match_vtable) {
            uint64_t vt = 0;
            if (!SafeReadPtr((const void*)pp, (void**)&vt)) continue;
            if (vt != match_vtable) continue;
        }
        std::string k18, k20;
        SafeReadPdxString((const void*)(pp + 0x18), k18);
        SafeReadPdxString((const void*)(pp + 0x20), k20);
        if (!plausible_key(k18)) k18.clear();
        if (!plausible_key(k20)) k20.clear();
        if (k18.empty() && k20.empty() && !match_vtable) continue;
        nlohmann::json j = { {"offset", offset + off}, {"ptr", (uint64_t)pp} };
        if (!k18.empty()) j["at_0x18"] = k18;
        if (!k20.empty()) j["at_0x20"] = k20;
        strings.push_back(j);
    }

    // vec_vtable: treat every qword as a possible vector data pointer and report the ones whose
    // first 8 elements contain objects of that class. This is how a `vector<CApproach*>` is told
    // apart from the surrounding noise: the vector itself lives on the heap, the type only holds
    // {data, cap, size}, so the pointer scan above can never see the elements.
    nlohmann::json vec_hits = nlohmann::json::array();
    for (uint32_t off = 0; vec_vtable && off + 8 <= length && vec_hits.size() < 20; off += 8) {
        void* q = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)type + offset + off), &q) || !q) continue;
        const uintptr_t arr = (uintptr_t)q;
        if (arr < 0x10000 || arr > 0x7FFFFFFFFFFF || (arr & 7)) continue;
        int cnt = 0;
        nlohmann::json ptrs = nlohmann::json::array();
        for (int k = 0; k < 8; ++k) {
            void* p = nullptr;
            if (!SafeReadPtr((const void*)(arr + (uintptr_t)k * 8), &p) || !p) continue;
            uint64_t vt = 0;
            if (!SafeReadPtr((const void*)p, (void**)&vt)) continue;
            if (vt == vec_vtable) {
                ++cnt;
                ptrs.push_back({ {"k", k}, {"ptr", (uint64_t)(uintptr_t)p} });
            }
        }
        if (cnt) {
            nlohmann::json h;
            h["offset"] = offset + off;
            h["data"] = (uint64_t)arr;
            h["count"] = cnt;
            h["ptrs"] = ptrs;
            vec_hits.push_back(h);
        }
    }

    // the live approach object (CSituation+0x210): its vtable is the class signature to match on
    void* cur_approach = nullptr;
    uint64_t cur_approach_vt = 0;
    if (SafeReadPtr((const void*)((uintptr_t)sit + 0x210), &cur_approach) && cur_approach) {
        SafeReadPtr((const void*)cur_approach, (void**)&cur_approach_vt);
    }

    return { {"success", true}, {"situation_id", situation_id}, {"situation_ptr", (uint64_t)(uintptr_t)sit},
             {"type_ptr", (uint64_t)(uintptr_t)type}, {"offset", offset}, {"length", length},
             {"current_approach_ptr", (uint64_t)(uintptr_t)cur_approach},
             {"current_approach_vtable", (uint64_t)cur_approach_vt},
             {"ptr_strings", strings}, {"vec_hits", vec_hits}, {"dump", hex} };
}

// Never let a C++ exception escape into the game's main thread: __try/__except only catches access
// violations, while a std::bad_alloc from a junk length would call std::terminate (no crash dump).
nlohmann::json SituationLogManager::ProbeSituationTypeJson(uint32_t situation_id, uint32_t offset, uint32_t length,
                                                            bool follow, uint64_t match_vtable, uint64_t vec_vtable) {
    try {
        return ProbeSituationTypeBody(base_address_, situation_id, offset, length, follow, match_vtable, vec_vtable);
    } catch (const std::exception& e) {
        return { {"success", false}, {"error", std::string("probe threw: ") + e.what()} };
    } catch (...) {
        return { {"success", false}, {"error", "probe threw an unknown exception"} };
    }
}

nlohmann::json SituationLogManager::ProbeSituationJson(uint32_t situation_id, uint32_t offset, uint32_t length,
                                                       uint64_t vec_vtable) {
    void* sit_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CSituation), &sit_mgr) || !sit_mgr) {
        return { {"success", false}, {"error", "situation manager not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) || cap == 0) {
        return { {"success", false}, {"error", "situation array not readable"} };
    }

    void* sit = nullptr;
    for (uint32_t i = 0; i < cap && i < 2048; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &p) || !p) continue;
        uint32_t id = 0;
        SafeReadU32((const void*)((uintptr_t)p + 0x08), &id);
        if (id == situation_id) { sit = p; break; }
    }
    if (!sit) return { {"success", false}, {"error", "situation not found"} };

    if (length < 0x10) length = 0x10;
    if (length > 0x800) length = 0x800;
    const uintptr_t base = (uintptr_t)sit + offset;

    std::string hex;
    char line[128];
    for (uint32_t off = 0; off < length; off += 16) {
        unsigned char b[16]{};
        const uint32_t n = (length - off < 16) ? (length - off) : 16;
        if (!SafeCopyChars((char*)b, (const char*)(base + off), n)) break;
        int w = snprintf(line, sizeof(line), "%04X:", offset + off);
        for (uint32_t i = 0; i < n; ++i) {
            w += snprintf(line + w, sizeof(line) - (size_t)w, " %02X", b[i]);
        }
        hex += line;
        hex += "\n";
    }
    // vec_vtable (or the live approach's vtable when 0): treat each qword as a possible vector
    // data pointer and report the ones whose first elements are objects of that class.
    uint64_t want_vt = vec_vtable;
    if (!want_vt) {
        void* cur = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)sit + 0x210), &cur) && cur) {
            SafeReadPtr((const void*)cur, (void**)&want_vt);
        }
    }
    nlohmann::json vec_hits = nlohmann::json::array();
    for (uint32_t off = 0; want_vt && off + 8 <= length && vec_hits.size() < 20; off += 8) {
        void* q = nullptr;
        if (!SafeReadPtr((const void*)(base + off), &q) || !q) continue;
        const uintptr_t arr = (uintptr_t)q;
        if (arr < 0x10000 || arr > 0x7FFFFFFFFFFF || (arr & 7)) continue;
        int cnt = 0;
        nlohmann::json ptrs = nlohmann::json::array();
        for (int k = 0; k < 8; ++k) {
            void* p = nullptr;
            if (!SafeReadPtr((const void*)(arr + (uintptr_t)k * 8), &p) || !p) continue;
            uint64_t vt = 0;
            if (!SafeReadPtr((const void*)p, (void**)&vt)) continue;
            if (vt == want_vt) {
                ++cnt;
                ptrs.push_back({ {"k", k}, {"ptr", (uint64_t)(uintptr_t)p} });
            }
        }
        if (cnt) {
            nlohmann::json h;
            h["offset"] = offset + off;
            h["data"] = (uint64_t)arr;
            h["count"] = cnt;
            h["ptrs"] = ptrs;
            vec_hits.push_back(h);
        }
    }

    return { {"success", true}, {"situation_id", situation_id}, {"ptr", (uint64_t)(uintptr_t)sit},
             {"offset", offset}, {"length", length}, {"vec_hits", vec_hits}, {"dump", hex} };
}

nlohmann::json SituationLogManager::GetSituationApproachesJson(uint32_t situation_id) {
    void* sit_mgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CSituation), &sit_mgr) || !sit_mgr) {
        return { {"success", false}, {"error", "situation manager not found"} };
    }
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) || cap == 0) {
        return { {"success", false}, {"error", "situation array not readable"} };
    }
    void* sit = nullptr;
    for (uint32_t i = 0; i < cap && i < 2048; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &p) || !p) continue;
        uint32_t id = 0;
        SafeReadU32((const void*)((uintptr_t)p + 0x08), &id);
        if (id == situation_id) { sit = p; break; }
    }
    if (!sit) return { {"success", false}, {"error", "situation not found"} };

    void* type = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)sit + 0x208), &type) || !type) {
        return { {"success", false}, {"error", "situation type not readable"} };
    }
    void* block = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)type + 0x68), &block) || !block) {
        return { {"success", false}, {"error", "approach block not readable"} };
    }

    // class signature + the current selection, both taken from CSituation+0x210
    void* cur = nullptr;
    uint64_t vt = 0;
    if (SafeReadPtr((const void*)((uintptr_t)sit + 0x210), &cur) && cur) {
        SafeReadPtr((const void*)cur, (void**)&vt);
    }
    std::string cur_key;
    if (cur) SafeReadPdxString((const void*)((uintptr_t)cur + 0x18), cur_key);

    auto plausible = [](const std::string& s) {
        if (s.size() < 2 || s.size() > 96) return false;
        for (unsigned char c : s) {
            if (c < 0x20 || c > 0x7E) return false;
        }
        return true;
    };

    nlohmann::json list = nlohmann::json::array();
    const uintptr_t block_addr = (uintptr_t)block;
    for (uint32_t k = 0; k < 64 && list.size() < 16; ++k) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)(block_addr + (uintptr_t)k * 8), &p) || !p) continue;
        const uintptr_t pp = (uintptr_t)p;
        if (pp < 0x10000 || pp > 0x7FFFFFFFFFFF) continue;
        if (vt) {
            uint64_t t = 0;
            if (!SafeReadPtr((const void*)pp, (void**)&t) || t != vt) continue;
        }
        std::string key;
        if (!SafeReadPdxString((const void*)(pp + 0x18), key) || !plausible(key)) continue;
        if (!vt && key.find("approach") == std::string::npos) continue;
        bool dup = false;
        for (const auto& e : list) {
            if (e.value("key", std::string()) == key) { dup = true; break; }
        }
        if (dup) continue;
        list.push_back({ {"key", key}, {"name", LocalizeKey(key)}, {"is_active", pp == (uintptr_t)cur} });
    }

    if (list.empty()) {
        return { {"success", false}, {"error", "no approach found for this situation"} };
    }
    return { {"success", true}, {"situation_id", situation_id}, {"current_approach", cur_key},
             {"count", (uint32_t)list.size()}, {"approaches", list} };
}

nlohmann::json SituationLogManager::ValidateSituationApproachJson(uint32_t situation_id, const std::string& approach_key) {
    if (approach_key.empty()) {
        return { {"success", false}, {"valid", false}, {"error", "Approach key cannot be empty"} };
    }
    namespace approach = sdk::cmd::set_situation_approach_command;
    auto cmd = CommandBuilder::Get().Create(approach::kSpec);
    cmd.Set<uint32_t>(approach::situation, situation_id)
       .SetString(approach::key, approach_key);
    // 与 Post 前的检查完全相同，但只问不投——拿到引擎自己的拒绝原因
    std::string why;
    const bool valid = cmd.IsValid(&why);
    nlohmann::json out = { {"success", true}, {"situation_id", situation_id}, {"approach_key", approach_key},
                           {"valid", valid}, {"name", LocalizeKey(approach_key)} };
    if (!why.empty()) out["reason"] = why;
    else if (!valid && !cmd.error().empty()) out["reason"] = cmd.error();
    return out;
}

nlohmann::json SituationLogManager::SetSituationApproach(uint32_t situation_id, const std::string& approach_key) {
    if (approach_key.empty()) {
        return {
            {"error", {
                {"code", -32050},
                {"message", "Approach key cannot be empty"}
            }}
        };
    }

    uint32_t target_country_id = GetPlayerCountryId();

    // If situation_id is specified, find it to verify ownership
    if (situation_id != 0) {
        void* sit_mgr = nullptr;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CSituation), &sit_mgr) && sit_mgr) {
            void* arr = nullptr;
            uint32_t cap = 0;
            if (SafeReadPtr((const void*)((uintptr_t)sit_mgr + 0x18), &arr) && arr &&
                SafeReadU32((const void*)((uintptr_t)sit_mgr + 0x20), &cap) && cap > 0) {
                bool found = false;
                for (uint32_t i = 0; i < cap && i < 2048; ++i) {
                    void* sit = nullptr;
                    if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &sit) || !sit) continue;
                    uint32_t s_id = 0;
                    SafeReadU32((const void*)((uintptr_t)sit + 0x08), &s_id);
                    if (s_id == situation_id) {
                        found = true;
                        uint32_t owner = 0;
                        SafeReadU32((const void*)((uintptr_t)sit + 0x204), &owner);
                        target_country_id = owner;
                        break;
                    }
                }
                if (!found) {
                    LOGF("[SITUATION_LOG] Situation ID %u not found in global manager. Using player country %u...",
                        situation_id, target_country_id);
                }
            }
        }
    }

    // Retrieve tick timestamp from date manager (+0xC0)
    uint32_t tick_timestamp = 0;
    void* date_mgr = nullptr;
    if (SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &date_mgr) && date_mgr && (uintptr_t)date_mgr >= 0x10000) {
        SafeReadU32((const void*)((uintptr_t)date_mgr + 0xC0), &tick_timestamp);
    }

    LOGF("[SITUATION_LOG] Posting CSetSituationApproachCommand for country %u, approach '%s', tick %u...",
        target_country_id, approach_key.c_str(), tick_timestamp);

    // The engine serializes only the situation and the approach key; the owning country is
    // derived from the situation, so it is not part of the payload.
    namespace approach = sdk::cmd::set_situation_approach_command;
    auto cmd = CommandBuilder::Get().Create(approach::kSpec);
    cmd.Set<uint32_t>(approach::situation, situation_id)
       .SetString(approach::key, approach_key);
    if (!cmd.Post()) {
        return {
            {"error", {
                {"code", -32053},
                {"message", cmd.error()}
            }}
        };
    }

    LOGF("[SITUATION_LOG] CSetSituationApproachCommand successfully posted.");
    return {
        {"success", true},
        {"situation_id", situation_id},
        {"country_id", target_country_id},
        {"approach_key", approach_key},
        {"name", LocalizeKey(approach_key)},
        {"message", "Situation approach command posted successfully"}
    };
}

} // namespace bridge
