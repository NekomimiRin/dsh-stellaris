#include "species_manager.hpp"
#include "game_state.hpp"
#include "sdk/stellaris_sdk.hpp"
#include "command_builder.hpp"
#include "common.hpp"
#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace bridge {

// Engine container layouts (templates, the same for every element type):
//   TPdxRefDatabase<T> (sdk::db): +0x18 slots of 0x10 bytes {.., T* at +8}, +0x20 slot count,
//                                 +0x24 live objects
//   TGameDatabase<T> (script types): +0x50 T* array, +0x5C count
//   CPdxArray<T>: +0x8 data, +0x10 capacity, +0x14 size
//   CString: a 16-byte header, then its std::string text at +0x10; CString_Assign / CString_Free
//            take the CString itself
//   CPdxUnorderedMap (Robin Hood): +0 entries, +0xC index mask, +0x10 overflow slots; an entry is
//                                  {+4 probe distance (0 = empty), +8 key, +0x10 value}
constexpr std::ptrdiff_t kRefDbSlots = 0x18, kRefDbSlotCount = 0x20, kRefDbLive = 0x24;
constexpr std::ptrdiff_t kGameDbItems = 0x50, kGameDbCount = 0x5C;
constexpr std::ptrdiff_t kArrayData = 0x8, kArrayCapacity = 0x10, kArraySize = 0x14;
constexpr std::ptrdiff_t kCStringText = 0x10;
constexpr std::ptrdiff_t kMapMask = 0xC, kMapOverflow = 0x10, kMapProbe = 4, kMapKey = 8, kMapValue = 0x10;

namespace species = sdk::ent::CSpecies;
namespace module = sdk::ent::CSpeciesRightsModule;
namespace rights = sdk::ent::CSpeciesRightsCountryConfiguration;
// One species' rights (CSpeciesRightsCountryConfiguration) as the module embeds them back to back
constexpr size_t kRightsSize = module::built_species - module::primary;

// The rights categories: the configuration's pointer to the chosen right type, and the date the
// category may change again (stamped by CopySettingsFrom; integration has none)
struct RightCategory {
    const char* name;
    std::ptrdiff_t field;
    std::ptrdiff_t changed;
};
static const RightCategory kRightCategories[] = {
    {"citizenship", rights::citizenship, sdk::rt::CSpeciesRightsCountryConfiguration_changed_citizenship},
    {"living_standards", rights::living_standard, sdk::rt::CSpeciesRightsCountryConfiguration_changed_living_standard},
    {"military_service", rights::military_service, sdk::rt::CSpeciesRightsCountryConfiguration_changed_military_service},
    {"slavery_type", rights::slavery, sdk::rt::CSpeciesRightsCountryConfiguration_changed_slavery},
    {"purge_type", rights::purge, sdk::rt::CSpeciesRightsCountryConfiguration_changed_purge},
    {"population_controls", rights::population_control, sdk::rt::CSpeciesRightsCountryConfiguration_changed_population_control},
    {"colonization_controls", rights::colonization_control, sdk::rt::CSpeciesRightsCountryConfiguration_changed_colonization_control},
    {"migration_controls", rights::migration_control, sdk::rt::CSpeciesRightsCountryConfiguration_changed_migration_control},
    {"subspecies_integration", rights::subspecies_integration, 0},
};

// A CPersistentName's text (its key string) and its "literal" flag (text is not a loc key)
constexpr std::ptrdiff_t NameText(std::ptrdiff_t persistent_name) { return persistent_name + sdk::ent::CPersistentName::key; }
constexpr std::ptrdiff_t NameLiteral(std::ptrdiff_t persistent_name) { return persistent_name + sdk::ent::CPersistentName::literal; }

// Safe memory read helpers
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

static bool SafeCopyChars(char* dest, const char* src, size_t count) {
    __try {
        memcpy(dest, src, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallCopySpecies(SpeciesManager::FnSpeciesCopyCtor fn_copy, void* dest, const void* src) {
    if (!fn_copy || !dest || !src) return false;
    __try {
        fn_copy(dest, src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallDtorSpecies(SpeciesManager::FnSpeciesDtor fn_dtor, void* species) {
    if (!fn_dtor || !species) return false;
    __try {
        fn_dtor(species);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetTraits(SpeciesManager::FnTraitSetSetTraits fn_set_traits, void* trait_set, void* pdx_array) {
    if (!fn_set_traits || !trait_set || !pdx_array) return false;
    __try {
        fn_set_traits(trait_set, pdx_array);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallAssign(SpeciesManager::FnCStringAssign fn_assign, void* cstring, const std::string& text) {
    if (!fn_assign || !cstring) return false;
    __try {
        fn_assign(cstring, text.c_str(), text.size());
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

static uint32_t ReadId(const void* obj, std::ptrdiff_t off) {
    uint32_t id = 0xFFFFFFFF;
    if (obj) SafeReadU32((const void*)((uintptr_t)obj + off), &id);
    return id;
}

// Engine calls (main thread, SEH-guarded by CommandBuilder, skipped when the SDK does not match)
struct TraitCostCall {
    uintptr_t fn;
    const void* trait;
    const void* country;
    int result;
};
static void CallTraitCost(void* p, void*) {
    auto* x = (TraitCostCall*)p;
    x->result = ((int (*)(const void*, const void*))x->fn)(x->trait, x->country);
}

static int TraitCost(uintptr_t base, const void* trait, const void* country) {
    TraitCostCall c{ base + sdk::fn::CTrait_GetCost, trait, country, 0 };
    return CommandBuilder::Get().CallGuarded(&CallTraitCost, &c) ? c.result : 0;
}

struct FreePointsCall {
    uintptr_t fn;
    const void* country;
    const void* species;
    int points;
    int picks;
};
static void CallFreePoints(void* p, void*) {
    auto* x = (FreePointsCall*)p;
    ((bool (*)(const void*, const void*, int*, int*))x->fn)(x->country, x->species, &x->points, &x->picks);
}

struct CountryIntCall {
    uintptr_t fn;
    const void* country;
    int result;
};
static void CallCountryInt(void* p, void*) {
    auto* x = (CountryIntCall*)p;
    x->result = ((int (*)(const void*))x->fn)(x->country);
}

// CCountry::GetSpeciesRightsModule (inlined on Windows): the module the country keeps per species
static void* RightsModule(void* country) {
    void* m = nullptr;
    if (country) SafeReadPtr((const void*)((uintptr_t)country + sdk::rt::CCountry_species_rights_module), &m);
    return m;
}

// Direct-read port of CSpeciesRightsModule::CalcSpeciesRights: the founder and built species have
// their own configurations, others an entry in the per-species map, and the rest the default.
static void* LookupSpeciesRights(void* module_obj, void* country, void* species_obj, uint8_t* out_is_specific, int depth = 0) {
    *out_is_specific = 0;
    if (!module_obj || !species_obj || !country) return nullptr;
    uint32_t sid = ReadId(species_obj, sdk::rt::CSpecies_id);
    uint32_t base_id = ReadId(species_obj, species::base_ref);
    if (sid == 0xFFFFFFFF) return nullptr;
    if (sid == ReadId(country, sdk::ent::CCountry::founder_species_ref)) {
        *out_is_specific = 1;
        return (void*)((uintptr_t)module_obj + module::primary);
    }
    if (sid == ReadId(country, sdk::ent::CCountry::built_species_ref)) {
        *out_is_specific = 1;
        return (void*)((uintptr_t)module_obj + module::built_species);
    }
    const uintptr_t map = (uintptr_t)module_obj + module::species_rights;
    void* entries = nullptr;
    uint32_t mask = 0;
    uint8_t overflow = 0;
    if (SafeReadPtr((const void*)map, &entries) && entries &&
        SafeReadU32((const void*)(map + kMapMask), &mask) && mask < 0x10000) {
        SafeCopyChars((char*)&overflow, (const char*)(map + kMapOverflow), 1);
        const size_t stride = kMapValue + kRightsSize;
        for (uint32_t i = 0; i <= mask + overflow; ++i) {
            uintptr_t e = (uintptr_t)entries + (uintptr_t)i * stride;
            uint8_t probe = 0;
            uint32_t key = 0;
            if (SafeCopyChars((char*)&probe, (const char*)(e + kMapProbe), 1) && probe != 0 && probe != 0xFF &&
                SafeReadU32((const void*)(e + kMapKey), &key) && key == sid) {
                *out_is_specific = 1;
                return (void*)(e + kMapValue);
            }
        }
    }
    // Templates and subspecies without their own configuration follow their base species.
    if (base_id != 0xFFFFFFFF && base_id != sid && depth < 8) {
        void* base = SpeciesManager::Get().FindSpeciesPtr(base_id);
        if (base) {
            uint8_t dummy = 0;
            if (void* r = LookupSpeciesRights(module_obj, country, base, &dummy, depth + 1)) {
                return r;
            }
        }
    }
    return (void*)((uintptr_t)module_obj + module::default_);
}

SpeciesManager& SpeciesManager::Get() {
    static SpeciesManager instance;
    return instance;
}

bool SpeciesManager::Init(uintptr_t base_address) {
    base_address_ = base_address;

    fn_species_copy_ctor_ = (FnSpeciesCopyCtor)(base_address_ + sdk::fn::CSpecies_CopyCtor);
    fn_species_dtor_ = (FnSpeciesDtor)(base_address_ + sdk::fn::CSpecies_Dtor);
    fn_trait_set_set_traits_ = (FnTraitSetSetTraits)(base_address_ + sdk::fn::CTraitSet_SetTraits);
    fn_cstring_assign_ = (FnCStringAssign)(base_address_ + sdk::fn::CString_Assign);

    rights_cache_.clear();
    rights_catalog_.clear();
    traits_catalog_.clear();
    trait_objects_.clear();

    EnsureDatabasesLoaded();
    EnsureTraitsLoaded();

    LOGF("[SPECIES_MGR] Initialized with base address: 0x%llX", (unsigned long long)base_address_);
    return true;
}

void* SpeciesManager::GetPlayerCountry() {
    // The local player's country, not country 0: after `play N` the species screen follows the
    // country being played, and so must the species/rights/template commands. (This used to read
    // slot 0 of the country database, which is only the player while playing country 0.)
    return GameState::Get().GetPlayerCountry();
}

uint32_t SpeciesManager::GetCurrentGameHours() {
    if (!base_address_) return 0;

    void* state = nullptr;
    if (SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &state) && state && (uintptr_t)state >= 0x10000) {
        uint32_t hours = 0;
        if (SafeReadU32((const void*)((uintptr_t)state + sdk::rt::CGameState_date_hours), &hours)) {
            return hours;
        }
    }
    return 0;
}

std::string SpeciesManager::LocalizeKey(const std::string& key) {
    return SafeLocalize(base_address_, key);
}

void SpeciesManager::LoadRightDatabase(const std::string& category, uintptr_t db_rva) {
    if (!base_address_) return;

    void* db_ptr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + db_rva), &db_ptr) || !db_ptr) {
        return;
    }

    void* arr_ptr = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db_ptr + kGameDbItems), &arr_ptr) || !arr_ptr ||
        !SafeReadU32((const void*)((uintptr_t)db_ptr + kGameDbCount), &count) || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count && i < 100; ++i) {
        void* elem = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr_ptr + i * 8), &elem) || !elem) {
            continue;
        }

        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)elem + sdk::rt::CSpeciesRightType_key), key) && !key.empty()) {
            rights_cache_[category][key] = elem;
            rights_catalog_[category].push_back({ key, LocalizeKey(key) });
        }
    }
}

void SpeciesManager::EnsureDatabasesLoaded() {
    if (!rights_cache_.empty()) return;

    LoadRightDatabase("citizenship", sdk::glob::TGameDatabase_CCitizenshipTypeDatabase_pInstance);
    LoadRightDatabase("living_standards", sdk::glob::TGameDatabase_CLivingStandardDatabase_pInstance);
    LoadRightDatabase("military_service", sdk::glob::TGameDatabase_CMilitaryServiceTypeDatabase_pInstance);
    LoadRightDatabase("slavery_type", sdk::glob::TGameDatabase_CSlaveryTypeDatabase_pInstance);
    LoadRightDatabase("purge_type", sdk::glob::TGameDatabase_CPurgeTypeDatabase_pInstance);
    LoadRightDatabase("population_controls", sdk::glob::TGameDatabase_CPopulationControlDatabase_pInstance);
    LoadRightDatabase("colonization_controls", sdk::glob::TGameDatabase_CColonizationControlDatabase_pInstance);
    LoadRightDatabase("migration_controls", sdk::glob::TGameDatabase_CMigrationControlDatabase_pInstance);
    LoadRightDatabase("subspecies_integration", sdk::glob::TGameDatabase_CSubSpeciesIntegrationTypeDatabase_pInstance);

    LOGF("[SPECIES_MGR] Loaded %zu rights categories into catalog.", rights_cache_.size());
}

void SpeciesManager::EnsureTraitsLoaded() {
    if (!traits_catalog_.empty() || !base_address_) return;

    void* db_ptr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::CTraitDatabase_pInstance), &db_ptr) || !db_ptr) {
        return;
    }

    // the database's trait array {data, capacity, size}
    void* arr_ptr = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db_ptr + sdk::rt::CTraitDatabase_traits), &arr_ptr) || !arr_ptr ||
        !SafeReadU32((const void*)((uintptr_t)db_ptr + sdk::rt::CTraitDatabase_traits + (kArraySize - kArrayData)), &count) || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count && i < 1000; ++i) {
        void* elem = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr_ptr + i * 8), &elem) || !elem) {
            continue;
        }

        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)elem + sdk::rt::CTrait_key), key) && !key.empty()) {
            traits_catalog_[key] = { key, LocalizeKey(key) };
            trait_objects_[key] = elem;
        }
    }

    LOGF("[SPECIES_MGR] Loaded %zu traits from CTraitDatabase", traits_catalog_.size());
}

void* SpeciesManager::GetSpeciesRightType(const std::string& category, const std::string& key) {
    EnsureDatabasesLoaded();
    auto cat_it = rights_cache_.find(category);
    if (cat_it != rights_cache_.end()) {
        auto key_it = cat_it->second.find(key);
        if (key_it != cat_it->second.end()) {
            return key_it->second;
        }
    }
    return nullptr;
}

void* SpeciesManager::FindSpeciesPtr(uint32_t species_id) {
    if (!base_address_ || species_id == 0xFFFFFFFF) return nullptr;

    void* smgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CSpecies), &smgr) || !smgr || (uintptr_t)smgr < 0x10000) {
        return nullptr;
    }

    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)smgr + kRefDbSlots), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)smgr + kRefDbSlotCount), &cap) || cap == 0) {
        return nullptr;
    }

    auto matches = [&](void* ptr) {
        return ptr && (ReadId(ptr, sdk::rt::CSpecies_id) & 0xFFFFFF) == (species_id & 0xFFFFFF);
    };
    uint32_t direct_slot = species_id & 0xFFFFFF;
    if (direct_slot < cap) {
        void* ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)arr + direct_slot * 16 + 8), &ptr) && matches(ptr)) {
            return ptr;
        }
    }

    for (uint32_t i = 0; i < cap && i < 1024; ++i) {
        void* ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &ptr) && matches(ptr)) {
            return ptr;
        }
    }

    return nullptr;
}

// The species' traits: CSpecies::traits is a CTraitSet holding a CPdxArray<CTrait const*>
std::vector<void*> SpeciesManager::TraitObjects(void* pSpecies) {
    std::vector<void*> out;
    if (!pSpecies) return out;
    const uintptr_t set = (uintptr_t)pSpecies + species::traits;
    void* data = nullptr;
    uint32_t count = 0;
    if (SafeReadPtr((const void*)(set + sdk::rt::CTraitSet_traits_data), &data) && data &&
        SafeReadU32((const void*)(set + sdk::rt::CTraitSet_traits_count), &count) && count < 64) {
        for (uint32_t i = 0; i < count; ++i) {
            void* trait = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)data + i * 8), &trait) && trait) {
                out.push_back(trait);
            }
        }
    }
    return out;
}

std::vector<TraitInfo> SpeciesManager::ReadTraits(void* pSpecies) {
    std::vector<TraitInfo> traits;
    for (void* trait : TraitObjects(pSpecies)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)trait + sdk::rt::CTrait_key), key) && !key.empty()) {
            traits.push_back({ key, LocalizeKey(key) });
        }
    }
    return traits;
}

uint32_t SpeciesManager::CalculateEmpirePops(uint32_t* out_colony_count) {
    void* country = GetPlayerCountry();
    if (!country) return 0;

    uint32_t colony_cnt = 0;
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::owned_planets + kArraySize), &colony_cnt);
    if (out_colony_count) {
        *out_colony_count = colony_cnt;
    }

    CountryIntCall c{ base_address_ + sdk::fn::CCountry_CalcAllPops, country, 0 };
    if (!CommandBuilder::Get().CallGuarded(&CallCountryInt, &c) || c.result < 0) return 0;
    return (uint32_t)c.result;
}

SpeciesRights SpeciesManager::ReadRights(void* pRightsMgr, void* pSpecies) {
    SpeciesRights r;
    if (!pRightsMgr || !pSpecies) {
        return r;
    }

    uint8_t is_specific = 0;
    void* pRights = LookupSpeciesRights(pRightsMgr, GetPlayerCountry(), pSpecies, &is_specific);
    if (!pRights) {
        return r;
    }
    r.is_specific = is_specific != 0;

    std::string* keys[] = { &r.citizenship, &r.living_standards, &r.military_service, &r.slavery_type, &r.purge_type,
                            &r.population_controls, &r.colonization_controls, &r.migration_controls,
                            &r.subspecies_integration };
    std::string* names[] = { &r.citizenship_localized, &r.living_standards_localized, &r.military_service_localized,
                             &r.slavery_type_localized, &r.purge_type_localized, &r.population_controls_localized,
                             &r.colonization_controls_localized, &r.migration_controls_localized,
                             &r.subspecies_integration_localized };
    uint32_t max_change_hour = 0;  // latest "may change again" date
    uint32_t cur_hour = GetCurrentGameHours();
    for (size_t k = 0; k < std::size(kRightCategories); ++k) {
        const auto& cat = kRightCategories[k];
        void* type_obj = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)pRights + cat.field), &type_obj) && type_obj) {
            SafeReadPdxString((const void*)((uintptr_t)type_obj + sdk::rt::CSpeciesRightType_key), *keys[k]);
        }
        *names[k] = LocalizeKey(*keys[k]);
        uint32_t h = 0;
        if (cat.changed && SafeReadU32((const void*)((uintptr_t)pRights + cat.changed), &h)) {
            if (h > max_change_hour) max_change_hour = h;
            if (h > 0 && cur_hour > 0 && cur_hour < h) {
                r.cooldown_days[cat.name] = (h - cur_hour) / 24;
            }
        }
    }

    // Each date is when that category may next change (now + SPECIES_POLICY_YEARS at the change).
    r.cooldown_remaining_days = (max_change_hour > 0 && cur_hour > 0 && cur_hour < max_change_hour)
                                    ? (max_change_hour - cur_hour) / 24 : 0;

    return r;
}

SpeciesSummary SpeciesManager::ReadSummary() {
    SpeciesSummary s;
    void* country = GetPlayerCountry();
    if (!country) return s;

    uint32_t fid = ReadId(country, sdk::ent::CCountry::founder_species_ref);
    if (fid != 0xFFFFFFFF) {
        s.founder_species_id = fid;
        void* pSpecies = FindSpeciesPtr(fid);
        if (pSpecies) {
            std::string name_key;
            if (SafeReadPdxString((const void*)((uintptr_t)pSpecies + NameText(species::name)), name_key)) {
                s.founder_species_name = LocalizeKey(name_key);
            }
        }
    }

    void* smgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CSpecies), &smgr);
    if (smgr && (uintptr_t)smgr >= 0x10000) {
        uint32_t total = 0;
        if (SafeReadU32((const void*)((uintptr_t)smgr + kRefDbLive), &total)) {
            s.total_species_in_galaxy = total;
        }
    }

    s.total_empire_pops = CalculateEmpirePops(&s.empire_colonies_count);
    return s;
}

nlohmann::json SpeciesManager::GetSummaryJson() {
    SpeciesSummary s = ReadSummary();
    return {
        {"founder_species_id", s.founder_species_id},
        {"founder_species_name", s.founder_species_name},
        {"total_species_in_galaxy", s.total_species_in_galaxy},
        {"empire_colonies_count", s.empire_colonies_count},
        {"total_empire_pops", s.total_empire_pops}
    };
}

nlohmann::json SpeciesManager::GetSpeciesJson(const nlohmann::json& req) {
    EnsureDatabasesLoaded();

    void* country = GetPlayerCountry();
    if (!country) {
        return {
            {"success", false},
            {"error", "Player country not available or game not loaded"}
        };
    }

    void* rights_mgr = RightsModule(country);
    uint32_t founder_id = ReadId(country, sdk::ent::CCountry::founder_species_ref);

    bool galaxy_mode = false;
    if (req.contains("mode") && req["mode"].is_string()) {
        galaxy_mode = (req["mode"] == "galaxy");
    }

    uint32_t target_species_id = 0;
    if (req.contains("species_id") && req["species_id"].is_number_unsigned()) {
        target_species_id = req["species_id"].get<uint32_t>();
    }

    // Build list of species to return
    std::vector<void*> species_to_process;

    void* smgr = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CSpecies), &smgr);
    void* arr = nullptr;
    uint32_t cap = 0;
    if (smgr) {
        SafeReadPtr((const void*)((uintptr_t)smgr + kRefDbSlots), &arr);
        SafeReadU32((const void*)((uintptr_t)smgr + kRefDbSlotCount), &cap);
    }

    if (target_species_id != 0) {
        void* sp = FindSpeciesPtr(target_species_id);
        if (sp) species_to_process.push_back(sp);
    } else if (galaxy_mode && arr && cap > 0) {
        for (uint32_t i = 0; i < cap && i < 1024; ++i) {
            void* sp = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)arr + i * 16 + 8), &sp) && sp) {
                species_to_process.push_back(sp);
            }
        }
    } else {
        // Empire mode: the country's own species list — CCountry::owned_species_refs (+0x3470),
        // the same array the species screen shows. It holds the founder, every species with pops
        // in the empire and the modification templates, and it includes pre-sapients (a species
        // with default rights too, which the old founder + rights-specific reconstruction missed).
        // Verified against a save: country 0 = {23 27 81 82 83 115 143 169 176 193 206 218 279 283}.
        std::vector<uint32_t> owned_ids;
        const uintptr_t owned = (uintptr_t)country + sdk::ent::CCountry::owned_species_refs;
        void* owned_arr = nullptr;
        uint32_t owned_cnt = 0;
        if (SafeReadPtr((const void*)(owned + kArrayData), &owned_arr) && owned_arr &&
            SafeReadU32((const void*)(owned + kArraySize), &owned_cnt) && owned_cnt > 0 && owned_cnt < 256) {
            for (uint32_t i = 0; i < owned_cnt; ++i) {
                uint32_t sid = 0;
                if (SafeReadU32((const void*)((uintptr_t)owned_arr + i * 4), &sid) && sid != 0xFFFFFFFF) {
                    owned_ids.push_back(sid);
                }
            }
        }
        if (owned_ids.empty() && founder_id != 0xFFFFFFFF) {
            // Fallback for a country whose array could not be read
            owned_ids.push_back(founder_id);
        }
        for (uint32_t sid : owned_ids) {
            void* sp = FindSpeciesPtr(sid);
            if (sp && std::find(species_to_process.begin(), species_to_process.end(), sp) == species_to_process.end()) {
                species_to_process.push_back(sp);
            }
        }
    }

    // The modification-template pool: ids in CCountry::species_templates_ref are templates that have
    // not been applied yet; a species whose base_ref is outside this pool is a real subspecies.
    std::unordered_set<uint32_t> template_pool;
    {
        const uintptr_t tmpl = (uintptr_t)country + sdk::ent::CCountry::species_templates_ref;
        void* pool_arr = nullptr;
        uint32_t pool_cnt = 0;
        if (SafeReadPtr((const void*)(tmpl + kArrayData), &pool_arr) && pool_arr &&
            SafeReadU32((const void*)(tmpl + kArraySize), &pool_cnt) && pool_cnt > 0 && pool_cnt < 256) {
            for (uint32_t i = 0; i < pool_cnt; ++i) {
                uint32_t id = 0;
                if (SafeReadU32((const void*)((uintptr_t)pool_arr + i * 4), &id) && id != 0xFFFFFFFF) {
                    template_pool.insert(id);
                }
            }
        }
    }

    nlohmann::json species_list = nlohmann::json::array();
    uint32_t pre_sapient_count = 0;
    uint32_t points_available_count = 0;
    for (void* sp : species_to_process) {
        uint32_t sid = ReadId(sp, sdk::rt::CSpecies_id);
        uint32_t base_id = ReadId(sp, species::base_ref);
        bool is_template = (base_id != 0xFFFFFFFF && base_id != sid);
        const bool in_template_pool = template_pool.count(sid) != 0;
        // This species' own trait budget (cheap native call — not the full modification payload)
        int32_t free_points = 0, free_picks = 0;
        {
            FreePointsCall fp{ base_address_ + sdk::fn::NSpeciesModification_HasFreeSpeciesTraitPoints, country, sp, 0, 0 };
            if (CommandBuilder::Get().CallGuarded(&CallFreePoints, &fp)) {
                free_points = fp.points;
                free_picks = fp.picks;
            }
        }
        if (free_points > 0) points_available_count++;
        // The base species' name, so variants of one species can be grouped together
        std::string base_name;
        if (is_template) {
            void* base_sp = FindSpeciesPtr(base_id);
            std::string raw_base;
            if (base_sp && SafeReadPdxString((const void*)((uintptr_t)base_sp + NameText(species::name)), raw_base)) {
                base_name = LocalizeKey(raw_base);
            }
        }

        std::string raw_name, raw_plural, raw_adj, portrait_class;
        SafeReadPdxString((const void*)((uintptr_t)sp + NameText(species::name)), raw_name);
        SafeReadPdxString((const void*)((uintptr_t)sp + NameText(species::plural)), raw_plural);
        SafeReadPdxString((const void*)((uintptr_t)sp + NameText(species::adjective)), raw_adj);
        SafeReadPdxString((const void*)((uintptr_t)sp + species::portrait), portrait_class);

        // Pre-sapients are defined by their script traits (every trait_presapient_* carries
        // `sapient = no`); an uplifted species keeps the presapient traits and gains trait_uplifted.
        const std::vector<TraitInfo> trait_list = ReadTraits(sp);
        nlohmann::json traits_json = nlohmann::json::array();
        bool is_pre_sapient = false, is_uplifted = false;
        for (const auto& t : trait_list) {
            if (t.key.find("presapient") != std::string::npos) is_pre_sapient = true;
            if (t.key == "trait_uplifted") is_uplifted = true;
            traits_json.push_back({
                {"key", t.key},
                {"name", t.localized_name}
            });
        }
        // The planet the species came from and, for an uplifted species, who uplifted it.
        // Both are refs: 0xFFFFFFFF when absent. Planet and country ids are < 2^24.
        const uint32_t home_planet_raw = ReadId(sp, species::home_planet);
        const uint32_t uplifter_raw = ReadId(sp, species::uplifter);
        // An uplifted species keeps its pre-sapient traits (and gains trait_uplifted), so
        // is_pre_sapient means "still pre-sapient" — has the trait and has not been uplifted.
        is_pre_sapient = is_pre_sapient && !is_uplifted;
        if (is_pre_sapient) pre_sapient_count++;

        SpeciesRights r = ReadRights(rights_mgr, sp);
        nlohmann::json cooldowns_json = nlohmann::json::object();
        for (const auto& cat : kRightCategories) {
            auto it = r.cooldown_days.find(cat.name);
            cooldowns_json[cat.name] = (it == r.cooldown_days.end()) ? 0u : it->second;
        }

        nlohmann::json sp_json = {
            {"species_id", sid},
            {"base_species_id", base_id},
            {"base_species_name", base_name},
            {"is_template", is_template},
            {"in_template_pool", in_template_pool},
            {"points_free", free_points},
            {"picks_free", free_picks},
            {"key", raw_name},
            {"name", LocalizeKey(raw_name)},
            {"plural", LocalizeKey(raw_plural)},
            {"adjective", LocalizeKey(raw_adj)},
            {"class", portrait_class},
            {"is_founder", (sid == founder_id)},
            {"is_pre_sapient", is_pre_sapient},
            {"is_uplifted", is_uplifted},
            {"home_planet_id", home_planet_raw == 0xFFFFFFFF ? nlohmann::json(nullptr)
                                                             : nlohmann::json(home_planet_raw & 0xFFFFFF)},
            {"uplifter_country_id", uplifter_raw == 0xFFFFFFFF ? nlohmann::json(nullptr)
                                                               : nlohmann::json(uplifter_raw & 0xFFFFFF)},
            {"traits", traits_json},
            {"rights", {
                {"citizenship", {{"key", r.citizenship}, {"name", r.citizenship_localized}}},
                {"living_standards", {{"key", r.living_standards}, {"name", r.living_standards_localized}}},
                {"military_service", {{"key", r.military_service}, {"name", r.military_service_localized}}},
                {"slavery_type", {{"key", r.slavery_type}, {"name", r.slavery_type_localized}}},
                {"purge_type", {{"key", r.purge_type}, {"name", r.purge_type_localized}}},
                {"population_controls", {{"key", r.population_controls}, {"name", r.population_controls_localized}}},
                {"colonization_controls", {{"key", r.colonization_controls}, {"name", r.colonization_controls_localized}}},
                {"migration_controls", {{"key", r.migration_controls}, {"name", r.migration_controls_localized}}},
                {"subspecies_integration", {{"key", r.subspecies_integration}, {"name", r.subspecies_integration_localized}}},
                {"is_specific", r.is_specific},
                {"cooldowns", cooldowns_json},
                {"cooldown_remaining_days", r.cooldown_remaining_days}
            }}
        };

        species_list.push_back(sp_json);
    }

    nlohmann::json catalog_json = nlohmann::json::object();
    for (const auto& [cat, opts] : rights_catalog_) {
        nlohmann::json arr_json = nlohmann::json::array();
        for (const auto& opt : opts) {
            arr_json.push_back({
                {"key", opt.key},
                {"name", opt.localized_name}
            });
        }
        catalog_json[cat] = arr_json;
    }

    SpeciesSummary summary = ReadSummary();

    return {
        {"success", true},
        {"mode", galaxy_mode ? "galaxy" : "empire"},
        {"founder_species_id", founder_id},
        {"founder_species_name", summary.founder_species_name},
        {"total_empire_pops", summary.total_empire_pops},
        {"empire_colonies_count", summary.empire_colonies_count},
        {"total_species_in_galaxy", summary.total_species_in_galaxy},
        {"species_count", species_list.size()},
        {"pre_sapient_count", pre_sapient_count},
        {"points_available_count", points_available_count},
        {"species", species_list},
        {"available_rights_catalog", catalog_json}
    };
}

nlohmann::json SpeciesManager::SetSpeciesRight(uint32_t species_id, const std::string& category, const std::string& right_value) {
    void* country = GetPlayerCountry();
    if (!country) {
        return {
            {"success", false},
            {"error", "Player country not found"}
        };
    }

    void* rights_mgr = RightsModule(country);
    if (!rights_mgr) {
        return {
            {"success", false},
            {"error", "Species rights manager not found on country"}
        };
    }

    void* species_obj = FindSpeciesPtr(species_id);
    if (!species_obj) {
        return {
            {"success", false},
            {"error", "Species not found with ID " + std::to_string(species_id)}
        };
    }

    const RightCategory* cat = nullptr;
    for (const auto& c : kRightCategories) {
        if (category == c.name) cat = &c;
    }
    if (!cat) {
        return {
            {"success", false},
            {"error", "Invalid rights category: '" + category + "'. Valid categories: citizenship, living_standards, military_service, slavery_type, purge_type, population_controls, colonization_controls, migration_controls, subspecies_integration"}
        };
    }

    void* new_right_type = GetSpeciesRightType(category, right_value);
    if (!new_right_type) {
        return {
            {"success", false},
            {"error", "Right value '" + right_value + "' not found in category '" + category + "' catalog"}
        };
    }

    uint8_t is_specific = 0;
    void* current_rights = LookupSpeciesRights(rights_mgr, country, species_obj, &is_specific);
    if (!current_rights) {
        return {
            {"success", false},
            {"error", "Failed to retrieve current species rights from engine"}
        };
    }

    // Check cooldown
    if (cat->changed) {
        uint32_t next_change_hour = 0;
        SafeReadU32((const void*)((uintptr_t)current_rights + cat->changed), &next_change_hour);
        uint32_t cur_hour = GetCurrentGameHours();
        // The engine stores the date the category may next change (set to now +
        // SPECIES_POLICY_YEARS by CopySettingsFrom when the category changes).
        if (next_change_hour > 0 && cur_hour > 0 && cur_hour < next_change_hour) {
            uint32_t rem_days = (next_change_hour - cur_hour) / 24;
            return {
                {"success", false},
                {"error", "Rights category '" + category + "' is on cooldown for " + std::to_string(rem_days) + " more days"}
            };
        }
    }

    // The engine applies any choice and then silently falls back to the best allowed right
    // (CheckIsValidForCountry) while still starting the category's cooldown, so check the
    // right's potential/allow triggers first, as the species rights screen does.
    void* right_base = (void*)((uintptr_t)new_right_type + sdk::rt::CSpeciesRightType_right_base);
    if (!CommandBuilder::Get().CallPredicate(sdk::fn::CSpeciesRightBase_IsPotential, right_base, country, species_obj, nullptr)) {
        return { {"success", false},
                 {"error", "Right '" + right_value + "' is not available for this species in this empire"} };
    }
    std::string not_allowed;
    if (!CommandBuilder::Get().CallPredicate(sdk::fn::CSpeciesRightBase_IsAllowed, right_base, country, species_obj, &not_allowed)) {
        return { {"success", false},
                 {"error", "Right '" + right_value + "' is not allowed" + (not_allowed.empty() ? "" : ": " + not_allowed)} };
    }

    // The current rights with this one category changed
    uint8_t modified_rights[kRightsSize];
    if (!SafeCopyChars((char*)modified_rights, (const char*)current_rights, kRightsSize)) {
        return {
            {"success", false},
            {"error", "Failed to copy current rights buffer"}
        };
    }
    *(void**)(modified_rights + cat->field) = new_right_type;

    // CCountrySetSpeciesRightsCommand: the factory default-constructs the rights payload (a vtable,
    // right-type pointers and dates, no owned memory), so copying the current rights over it with
    // one category changed matches what the engine's copy does.
    namespace cmd_rights = sdk::cmd::set_species_right_command;
    auto cmd = CommandBuilder::Get().Create(cmd_rights::kSpec);
    cmd.Set<uint32_t>(cmd_rights::country, ReadId(country, sdk::rt::CCountry_id))
       .Set<uint32_t>(cmd_rights::species, species_id)
       .SetBytes(cmd_rights::species_rights, modified_rights, sizeof(modified_rights));
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"error", why.empty() ? "Rights change rejected" : "Rights change rejected: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CSetSpeciesRightCommand for species_id %u, %s -> %s",
         species_id, category.c_str(), right_value.c_str());

    return {
        {"success", true},
        {"species_id", species_id},
        {"category", category},
        {"right_value", right_value},
        {"message", "Species right modification command dispatched successfully"}
    };
}

nlohmann::json SpeciesManager::GetSpeciesModificationInfoJson(uint32_t species_id) {
    EnsureDatabasesLoaded();
    EnsureTraitsLoaded();

    void* species_obj = FindSpeciesPtr(species_id);
    if (!species_obj) {
        return {
            {"success", false},
            {"error", "Species with ID " + std::to_string(species_id) + " not found"}
        };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return {
            {"success", false},
            {"error", "Player country not found"}
        };
    }
    if (!CommandBuilder::Get().SdkMatchesExe()) {
        return { {"success", false}, {"error", "SDK does not match this stellaris.exe; regenerate it with tools/sdk_dumper/dump.py"} };
    }

    std::string raw_name;
    SafeReadPdxString((const void*)((uintptr_t)species_obj + NameText(species::name)), raw_name);

    std::string portrait_class;
    SafeReadPdxString((const void*)((uintptr_t)species_obj + species::portrait), portrait_class);

    uint32_t base_species_id = ReadId(species_obj, species::base_ref);
    bool is_template = (base_species_id != 0xFFFFFFFF && base_species_id != species_id);

    // Trait costs for this country (CTrait::GetCost); a trait with a cost takes a pick, as
    // NSpeciesModification::CalcFreeTraitPoints counts them
    nlohmann::json current_traits_json = nlohmann::json::array();
    int32_t points_used = 0;
    int32_t picks_used = 0;
    for (void* trait : TraitObjects(species_obj)) {
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)trait + sdk::rt::CTrait_key), key);
        int32_t cost = TraitCost(base_address_, trait, country);
        points_used += cost;
        if (cost != 0) {
            picks_used++;
        }
        current_traits_json.push_back({
            {"key", key},
            {"name", LocalizeKey(key)},
            {"cost", cost}
        });
    }

    // Free points and picks as the species view shows them (class archetype, country modifiers,
    // the species' extra points, minus the traits taken)
    FreePointsCall fp{ base_address_ + sdk::fn::NSpeciesModification_HasFreeSpeciesTraitPoints, country, species_obj, 0, 0 };
    if (!CommandBuilder::Get().CallGuarded(&CallFreePoints, &fp)) {
        return { {"success", false}, {"error", "NSpeciesModification::HasFreeSpeciesTraitPoints failed"} };
    }
    int32_t points_free = fp.points;
    int32_t picks_free = fp.picks;

    nlohmann::json available_traits_json = nlohmann::json::array();
    for (const auto& [key, info] : traits_catalog_) {
        available_traits_json.push_back({
            {"key", key},
            {"name", info.localized_name},
            {"cost", TraitCost(base_address_, trait_objects_[key], country)}
        });
    }

    return {
        {"success", true},
        {"species_id", species_id},
        {"name", LocalizeKey(raw_name)},
        {"key", raw_name},
        {"class", portrait_class},
        {"base_species_id", base_species_id},
        {"is_template", is_template},
        {"points_total", points_free + points_used},
        {"points_used", points_used},
        {"points_free", points_free},
        {"picks_total", picks_free + picks_used},
        {"picks_used", picks_used},
        {"picks_free", picks_free},
        {"current_traits", current_traits_json},
        {"available_traits_catalog_count", available_traits_json.size()},
        {"available_traits_catalog", available_traits_json}
    };
}

// The trait list for a template: the requested traits, plus the source species' planet
// preference when none is given (a species always has one)
static bool BuildTraitList(SpeciesManager& mgr, void* source_species, const std::vector<std::string>& trait_keys,
                           const std::unordered_map<std::string, void*>& trait_objects,
                           std::vector<void*>& out, std::string& error) {
    bool has_preference = std::any_of(trait_keys.begin(), trait_keys.end(),
                                      [](const std::string& k) { return k.find("_preference") != std::string::npos; });
    if (!has_preference) {
        for (const auto& bt : mgr.ReadTraits(source_species)) {
            if (bt.key.find("_preference") != std::string::npos) {
                auto it = trait_objects.find(bt.key);
                if (it != trait_objects.end() && it->second) out.push_back(it->second);
                break;
            }
        }
    }
    for (const auto& k : trait_keys) {
        auto it = trait_objects.find(k);
        if (it == trait_objects.end() || !it->second) {
            error = "Unknown trait key: " + k;
            return false;
        }
        if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
    }
    return true;
}

// The species carried by CCountryCreateSpeciesModTemplate / CCountryUpdateSpeciesModTemplate:
// replace the factory's default species with a copy of the source, then apply the name and traits.
// Returns an error message, empty on success. On a fault after the default species was destroyed
// the command is abandoned (its embedded species can no longer be destroyed safely).
std::string SpeciesManager::FillTemplateSpecies(NativeCommand& cmd, std::ptrdiff_t species_off, void* source,
                                                uint32_t base_species_id, const std::string& name,
                                                const std::vector<std::string>& trait_keys) {
    std::vector<void*> trait_ptrs;
    std::string error;
    if (!trait_keys.empty() && !BuildTraitList(*this, source, trait_keys, trait_objects_, trait_ptrs, error)) {
        return error;
    }
    void* embedded = (void*)((uintptr_t)cmd.get() + species_off);
    if (!SafeCallDtorSpecies(fn_species_dtor_, embedded)) {
        cmd.Abandon("CSpecies destructor raised an exception");
        return cmd.error();
    }
    if (!SafeCallCopySpecies(fn_species_copy_ctor_, embedded, source)) {
        cmd.Abandon("CSpecies copy constructor raised an exception");
        return cmd.error();
    }
    *(uint32_t*)((uintptr_t)embedded + species::base_ref) = base_species_id;

    if (!name.empty()) {
        for (std::ptrdiff_t field : { species::name, species::plural, species::adjective }) {
            if (!SafeCallAssign(fn_cstring_assign_, (void*)((uintptr_t)embedded + NameText(field) - kCStringText), name)) {
                return "Failed to set the species name";
            }
            *(uint8_t*)((uintptr_t)embedded + NameLiteral(field)) = 1;
        }
    }

    if (!trait_ptrs.empty()) {
        struct PdxTraitArray {
            void* vtable{ nullptr };
            void** data{ nullptr };
            uint32_t capacity{ 0 };
            uint32_t size{ 0 };
        } pdx_traits;
        pdx_traits.data = trait_ptrs.data();
        pdx_traits.capacity = (uint32_t)trait_ptrs.size();
        pdx_traits.size = (uint32_t)trait_ptrs.size();
        if (!SafeCallSetTraits(fn_trait_set_set_traits_, (void*)((uintptr_t)embedded + species::traits), &pdx_traits)) {
            return "Failed to set traits on the species template";
        }
    }
    return {};
}

nlohmann::json SpeciesManager::CreateSpeciesTemplateJson(uint32_t base_species_id, const std::string& name,
                                                        const std::vector<std::string>& trait_keys, bool no_base,
                                                        uint32_t root_base_id) {
    if (!base_address_ || !CommandBuilder::Get().SdkMatchesExe()) {
        return { {"success", false}, {"error", "SDK does not match this stellaris.exe; regenerate it with tools/sdk_dumper/dump.py"} };
    }

    EnsureDatabasesLoaded();
    EnsureTraitsLoaded();

    void* base_species = FindSpeciesPtr(base_species_id);
    if (!base_species) {
        return { {"success", false}, {"error", "Base species with ID " + std::to_string(base_species_id) + " not found"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return { {"success", false}, {"error", "Player country not found"} };
    }

    namespace create = sdk::cmd::create_species_mod_template_command;
    auto cmd = CommandBuilder::Get().Create(create::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", cmd.error()} };
    }
    // no_base: the species is only used as the seed (class / portrait / traits); the new record
    // gets a null base_ref, i.e. it is a standalone species rather than a subspecies.
    // The command's own `species` field stays the real seed — naming 0xFFFFFFFF there is refused
    // by the engine ("Species template creation rejected", no reason).
    // root_base_id: record the new species under that base instead (the game does this for the
    // founder: a template of 115 is filed under 3791650817).
    uint32_t base_for_new = base_species_id;
    if (no_base) {
        base_for_new = 0xFFFFFFFFu;
    } else if (root_base_id != 0xFFFFFFFFu) {
        base_for_new = root_base_id;
    }
    cmd.Set<uint32_t>(create::country, ReadId(country, sdk::rt::CCountry_id))
       .Set<uint32_t>(create::species, base_species_id)
       .Set<uint8_t>(create::apply_template, 0);
    std::string error = FillTemplateSpecies(cmd, create::template_, base_species, base_for_new, name, trait_keys);
    if (!error.empty()) {
        return { {"success", false}, {"error", error} };
    }

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false},
                 {"error", why.empty() ? "Species template creation rejected" : "Species template creation rejected: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CCountryCreateSpeciesModTemplate for base_species_id %u, name '%s'",
         base_species_id, name.c_str());

    std::string effective_name;
    if (!name.empty()) {
        effective_name = name;
    } else {
        SafeReadPdxString((const void*)((uintptr_t)base_species + NameText(species::name)), effective_name);
        if (effective_name.empty()) {
            effective_name = "(inherited)";
        }
    }

    return {
        {"success", true},
        {"base_species_id", base_species_id},
        {"name", effective_name},
        {"inherited_name", name.empty()},
        {"traits_count", trait_keys.size()},
        {"message", "Species modification template created successfully"}
    };
}

nlohmann::json SpeciesManager::DeleteSpeciesTemplateJson(uint32_t species_id) {
    LOGF("[SPECIES_MGR] DeleteSpeciesTemplate: start, species_id %u", species_id);
    if (!base_address_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* species_obj = FindSpeciesPtr(species_id);
    if (!species_obj) {
        LOGF("[SPECIES_MGR] DeleteSpeciesTemplate: species %u not found", species_id);
        return { {"success", false}, {"error", "Template species with ID " + std::to_string(species_id) + " not found"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        LOGF("[SPECIES_MGR] DeleteSpeciesTemplate: player country not found");
        return { {"success", false}, {"error", "Player country not found"} };
    }

    namespace del = sdk::cmd::delete_species_mod_template_command;
    auto cmd = CommandBuilder::Get().Create(del::kSpec);
    cmd.Set<uint32_t>(del::country, ReadId(country, sdk::rt::CCountry_id)).Set<uint32_t>(del::species, species_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false},
                 {"error", why.empty() ? "Template cannot be deleted (not owned, or living pops of it exist)"
                                       : "Template cannot be deleted: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CCountryDeleteSpeciesModTemplate for species_id %u", species_id);

    return {
        {"success", true},
        {"species_id", species_id},
        {"message", "Species template deleted successfully"}
    };
}

nlohmann::json SpeciesManager::ModifySpeciesTemplateJson(uint32_t template_species_id, const std::string& name, const std::vector<std::string>& trait_keys) {
    LOGF("[SPECIES_MGR] ModifySpeciesTemplate: start, template_species_id %u, name '%s', traits %zu",
         template_species_id, name.c_str(), trait_keys.size());

    if (!base_address_ || !CommandBuilder::Get().SdkMatchesExe()) {
        return { {"success", false}, {"error", "SDK does not match this stellaris.exe; regenerate it with tools/sdk_dumper/dump.py"} };
    }

    EnsureDatabasesLoaded();
    EnsureTraitsLoaded();

    void* template_species = FindSpeciesPtr(template_species_id);
    if (!template_species) {
        LOGF("[SPECIES_MGR] ModifySpeciesTemplate: template species %u not found", template_species_id);
        return { {"success", false}, {"error", "Template species with ID " + std::to_string(template_species_id) + " not found"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        LOGF("[SPECIES_MGR] ModifySpeciesTemplate: player country not found");
        return { {"success", false}, {"error", "Player country not found"} };
    }

    namespace update = sdk::cmd::update_species_mod_template_command;
    auto cmd = CommandBuilder::Get().Create(update::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", cmd.error()} };
    }
    cmd.Set<uint32_t>(update::country, ReadId(country, sdk::rt::CCountry_id))
       .Set<uint32_t>(update::species, template_species_id);
    std::string error = FillTemplateSpecies(cmd, update::template_, template_species,
                                            ReadId(template_species, species::base_ref), name, trait_keys);
    if (!error.empty()) {
        return { {"success", false}, {"error", error} };
    }

    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false},
                 {"error", why.empty() ? "Species template modification rejected" : "Species template modification rejected: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CCountryUpdateSpeciesModTemplate for template_species_id %u", template_species_id);

    std::string effective_name;
    if (!name.empty()) {
        effective_name = name;
    } else {
        SafeReadPdxString((const void*)((uintptr_t)template_species + NameText(species::name)), effective_name);
        if (effective_name.empty()) {
            effective_name = "(unchanged)";
        }
    }

    return {
        {"success", true},
        {"template_species_id", template_species_id},
        {"name", effective_name},
        {"traits_count", trait_keys.size()},
        {"message", "Species template modified; the engine replaces the template with a new species id (re-read get_species)"}
    };
}

nlohmann::json SpeciesManager::ApplySpeciesTemplateJson(uint32_t template_species_id, const std::vector<uint32_t>& colony_ids,
                                                       bool tag_refs) {
    if (!base_address_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* template_species = FindSpeciesPtr(template_species_id);
    if (!template_species) {
        return { {"success", false}, {"error", "Template species with ID " + std::to_string(template_species_id) + " not found"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return { {"success", false}, {"error", "Player country not found"} };
    }

    uint32_t base_species_id = ReadId(template_species, species::base_ref);
    if (base_species_id == 0xFFFFFFFF) {
        base_species_id = template_species_id;
    }

    std::vector<uint32_t> targets;
    if (!colony_ids.empty()) {
        targets = colony_ids;
    } else {
        const uintptr_t owned = (uintptr_t)country + sdk::ent::CCountry::owned_planets;
        void* colony_vec = nullptr;
        uint32_t colony_cnt = 0;
        SafeReadPtr((const void*)(owned + kArrayData), &colony_vec);
        SafeReadU32((const void*)(owned + kArraySize), &colony_cnt);
        if (colony_vec && colony_cnt > 0 && colony_cnt < 1000) {
            for (uint32_t i = 0; i < colony_cnt; ++i) {
                uint32_t cid = 0;
                if (SafeReadU32((const void*)((uintptr_t)colony_vec + i * 4), &cid)) {
                    targets.push_back(cid);
                }
            }
        }
    }

    if (targets.empty()) {
        return { {"success", false}, {"error", "No colonies found to apply template to"} };
    }

    // CCreateSpeciesModSpecialProjectCommand: the colonies as a CPdxArray<SSpeciesColonyPair> (array
    // vtable from the factory) of {vtable, species, colony} elements
    namespace apply = sdk::cmd::create_species_mod_special_project;
    namespace pair = sdk::ent::NSpeciesModification_SSpeciesColonyPair;
    constexpr size_t kPairSize = 0x10;
    static_assert(pair::planet + sizeof(uint32_t) <= kPairSize, "pair element holds species and colony");
    auto cmd = CommandBuilder::Get().Create(apply::kSpec);
    auto* pairs = (uint8_t*)CommandBuilder::Get().EngineAlloc(targets.size() * kPairSize);
    if (!pairs) {
        return { {"success", false}, {"error", "engine allocation for the colony list failed"} };
    }
    const uint32_t kRefTag = 0x01000000u;
    const uint32_t pair_species = tag_refs ? (kRefTag | base_species_id) : base_species_id;
    for (size_t i = 0; i < targets.size(); ++i) {
        uint8_t* e = pairs + i * kPairSize;
        memset(e, 0, kPairSize);
        *(uintptr_t*)e = base_address_ + sdk::vt::NSpeciesModification_SSpeciesColonyPair;
        *(uint32_t*)(e + pair::species) = pair_species;
        *(uint32_t*)(e + pair::planet) = tag_refs ? (kRefTag | targets[i]) : targets[i];
    }
    cmd.Set<uint32_t>(apply::country, ReadId(country, sdk::rt::CCountry_id))
       .Set<uint32_t>(apply::template_, template_species_id)
       .Set<void*>(apply::species + kArrayData, pairs)
       .Set<uint32_t>(apply::species + kArrayCapacity, (uint32_t)targets.size())
       .Set<uint32_t>(apply::species + kArraySize, (uint32_t)targets.size());
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false},
                 {"error", why.empty() ? "Species modification project rejected" : "Species modification project rejected: " + why} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CCreateSpeciesModSpecialProjectCommand for template %u on %zu colonies",
         template_species_id, targets.size());

    return {
        {"success", true},
        {"template_species_id", template_species_id},
        {"base_species_id", base_species_id},
        {"colonies_affected", targets.size()},
        {"message", "Species modification special project initiated successfully"}
    };
}

nlohmann::json SpeciesManager::BeginUpliftingJson(uint32_t species_id, bool dry_run) {
    if (!base_address_) {
        return { {"success", false}, {"error", "Engine functions not initialized"} };
    }

    void* species_obj = FindSpeciesPtr(species_id);
    if (!species_obj) {
        return { {"success", false}, {"error", "Species with ID " + std::to_string(species_id) + " not found"} };
    }

    void* country = GetPlayerCountry();
    if (!country) {
        return { {"success", false}, {"error", "Player country not found"} };
    }

    // Only a species that is still pre-sapient can be uplifted. The engine checks this too, but a
    // plain reason here is friendlier than its generic refusal.
    bool has_presapient_trait = false, already_uplifted = false;
    for (const auto& t : ReadTraits(species_obj)) {
        if (t.key.find("presapient") != std::string::npos) has_presapient_trait = true;
        if (t.key == "trait_uplifted") already_uplifted = true;
    }
    if (!has_presapient_trait || already_uplifted) {
        return { {"success", false},
                 {"error", already_uplifted ? "Species is already uplifted" : "Species is not pre-sapient"} };
    }

    namespace up = sdk::cmd::begin_uplifting_command;
    auto cmd = CommandBuilder::Get().Create(up::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", cmd.error()} };
    }
    cmd.Set<uint32_t>(up::country, ReadId(country, sdk::rt::CCountry_id))
       .Set<uint32_t>(up::species, species_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false},
                 {"error", why.empty() ? "Uplift rejected" : "Uplift rejected: " + why} };
    }
    if (dry_run) {
        return { {"success", true}, {"dry_run", true}, {"species_id", species_id},
                 {"message", "IsValid passed; nothing posted"} };
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        return { {"success", false}, {"error", cmd.error()} };
    }

    LOGF("[SPECIES_MGR] Dispatched CBeginUpliftingCommand for species_id %u", species_id);
    return { {"success", true}, {"species_id", species_id}, {"message", "Uplift special project started"} };
}

} // namespace bridge
