#include "discoveries_manager.hpp"
#include "sdk/stellaris_sdk.hpp"
#include "command_builder.hpp"
#include "game_state.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace bridge {

// Relics owned by a country: CPdxArray<CRelic*> inside CCountry.
// 2026-11-22 corrected offsets. Where they come from: CActivateRelicCommand::IsValid
// (vtable slot 8, RVA 0x6CAAA0) calls CCountry::CanActivateRelic (RVA 0x755860), which
// loops
//     count = [country + 0x303C]   data = [country + 0x3030]   (8-byte CRelic* each)
// The old 0x30F0/0x30FC pair belongs to an older build and reads 0 for every country on
// 4.5.4 (verified 2026-11-22: country 0 holds 7 relics, country 1 holds 1, both read 0).
// Relic keys ARE serialized in the save (`country.<id>.relics = { "r_..." }`), which is the
// truth source for cross-checking; they are just absent from the SDK member table.
constexpr std::ptrdiff_t kCountryOwnedRelics = 0x3030;       // data (8-byte relic pointers)
constexpr std::ptrdiff_t kCountryOwnedRelicsCount = 0x303C;  // element count

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

static bool SafeReadI64(const void* addr, int64_t* out) {
    __try {
        *out = *(const int64_t*)addr;
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

// —— 星界行动的使用状态（2026-11-22 拆开）——
// CCountry + 0x3100 是一条 flat 数组 {哨兵, data +0x08, cap +0x10, size +0x14}，每条行动一项、
// 步长 0x40，索引就是行动对象 +0x08 的库内下标。定案依据：CActivateAstralActionCommand::IsValid
// （vtable 0x2398598 槽 8 → RVA 0x6CE890）第一道闸就是
//     mov rax,[cmd+0x20] / movsxd rcx,[rax+8] / shl rcx,6 / add rcx,[country+0x3108] / cmp byte [rcx],0
// 元素布局（字节）：
//   +0x00 unlockable(u8)       这个行动对本国是否已解锁（潜在条件 + 裂隙数阈值都算在内）
//   +0x01 unlocked(u8)         存档里的同名字段；写入时点未定（本档 16 条全 0，引擎照样放行可用项）
//   +0x04 usages(i32)          剩余可用次数，-1 = 不限次
//   +0x08 next_usable_date(u64) 冷却结束日（游戏日期编码），0 = 从没进过冷却
//   +0x20 key(std::string)     行动键，长度在 +0x30
// 存档里这段是明文：country.<id>.astral_actions_usage_states_array（unlockable/unlocked/usages/
// next_usable_date/is_waiting_for_cooldown_to_start），可拿来对账。已对过账：裂分剩 3/4 且
// 2280.12.15 + 187 天 = 2281.06.22 与引擎回执一字不差；虹吸 2/3、+2802 天 = 2288.09.27 同样吻合。
constexpr std::ptrdiff_t kCountryAstralUsage = 0x3100;
constexpr uint32_t kAstralUsageStride = 0x40;

// 游戏日期：u32 = (年*360 + (月-1)*30 + (日-1)) * 24 + 43800000（四个已知日期对过）。
static std::string DecodeGameDate(uint32_t v) {
    if (v < 43800000u) return std::string();
    const uint32_t ord = (v - 43800000u) / 24u;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%02u.%02u", ord / 360u, (ord % 360u) / 30u + 1u, (ord % 360u) % 30u + 1u);
    return std::string(buf);
}

// 只读探针：在 CCountry 里找「大档案馆模块」的 ref_array（storage_slots / exhibits 三类）。
// 数组头按本引擎的形状 {data(u64), cap, size}；data 指向堆，且第一个元素要能通过
// TPdxRef<CExhibit> 的数据库解析成 CExhibit（对象 +0x10 的 id 与引用 id 相同）才算候选。
static void* ResolveExhibitObj(uintptr_t base, uint32_t ref) {
    const uint32_t id = ref & 0xFFFFFFu;
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base + sdk::glob::TPdxRef_CExhibit_pDatabase), &db) || !db) return nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots) return nullptr;
    if (!SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || id >= cap) return nullptr;
    void* obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)id * 16 + 8), &obj) || !obj) return nullptr;
    uint32_t rid = 0;
    if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CExhibit_id), &rid)) return nullptr;
    if ((rid & 0xFFFFFFu) != id) return nullptr;
    return obj;
}
// 按 id 取 CCountry 对象（与 ProbeCountryJson 同一套解析）
static void* CountryById(uintptr_t base, uint32_t country_id) {
    void* cdb = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0, check = 0;
    void* country = nullptr;
    if (!SafeReadPtr((const void*)(base + sdk::db::CCountry), &cdb) || !cdb) return nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots) return nullptr;
    if (!SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap) return nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &country) || !country) return nullptr;
    if (!SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &check)) return nullptr;
    return (check & 0xFFFFFFu) == (country_id & 0xFFFFFFu) ? country : nullptr;
}

// 固定偏移候选：CCountry+0x2AB8 在 4.5.4 上两次载档都指向大档案馆模块（2026-11-22 实测）
static uint64_t ModuleAt(uintptr_t base, void* country, uint32_t off) {
    uint64_t p = 0;
    if (!SafeReadU64((const void*)((uintptr_t)country + off), &p) || p < 0x10000) return 0;
    uint64_t data = 0;
    uint32_t a = 0, b = 0;
    if (!SafeReadU64((const void*)((uintptr_t)p + 0x40), &data) ||
        !SafeReadU32((const void*)((uintptr_t)p + 0x48), &a) ||
        !SafeReadU32((const void*)((uintptr_t)p + 0x4C), &b)) return 0;
    if (!data || data < 0x10000 || b == 0 || b > 40 || a > 400) return 0;
    uint32_t first = 0;
    if (!SafeReadU32((const void*)data, &first)) return 0;
    first &= 0xFFFFFFu;
    if (!first || !ResolveExhibitObj(base, first)) return 0;
    return p;
}

// 在 CCountry 里找大档案馆模块（判据与探针相同）
static uint64_t FindArchiveModule(uintptr_t base, void* country) {
    uint64_t fixed = ModuleAt(base, country, 0x2AB8);
    if (fixed) return fixed;
    uint64_t best = 0;
    int best_d = 0;
    for (uint32_t off = 0; off + 16 <= 0x3FF0; off += 8) {
        uint64_t p = 0;
        if (!SafeReadU64((const void*)((uintptr_t)country + off), &p) || p < 0x10000) continue;
        uint64_t data = 0;
        uint32_t a = 0, b = 0;
        if (!SafeReadU64((const void*)((uintptr_t)p + 0x40), &data) ||
            !SafeReadU32((const void*)((uintptr_t)p + 0x48), &a) ||
            !SafeReadU32((const void*)((uintptr_t)p + 0x4C), &b)) continue;
        if (!data || data < 0x10000 || b == 0 || b > 40 || a > 400) continue;
        int distinct = 0;
        uint32_t prev = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < b && i < 12; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)(data + (uint64_t)i * 4), &v)) break;
            v &= 0xFFFFFFu;
            if (v > 1000000u) { distinct = 0; break; }
            if (v && v != prev) { ++distinct; prev = v; }
        }
        if (distinct > best_d) { best_d = distinct; best = p; }
    }
    return best;
}

// 在三个展览格里找某个 exhibit，回对象与 (类别, 位置)
static void* FindExhibitInGalleries(uintptr_t base, uint64_t module, uint32_t exhibit_id, int* out_g, int* out_i) {
    uint64_t gp = 0;
    if (!SafeReadU64((const void*)(module + 0x28), &gp) || gp < 0x10000) return nullptr;
    for (int g = 0; g < 3; ++g) {
        uint64_t data = 0;
        uint32_t cap = 0, size = 0;
        if (!SafeReadU64((const void*)(gp + g * 0x18 + 8), &data) ||
            !SafeReadU32((const void*)(gp + g * 0x18 + 0x10), &cap) ||
            !SafeReadU32((const void*)(gp + g * 0x18 + 0x14), &size)) continue;
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)(data + (uint64_t)i * 4), &v)) break;
            if ((v & 0xFFFFFFu) == exhibit_id) {
                if (out_g) *out_g = g;
                if (out_i) *out_i = (int)i;
                return ResolveExhibitObj(base, v);
            }
        }
    }
    return nullptr;
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

DiscoveriesManager& DiscoveriesManager::Get() {
    static DiscoveriesManager instance;
    return instance;
}

bool DiscoveriesManager::Init(uintptr_t base_address) {
    base_address_ = base_address;


    return true;
}

void* DiscoveriesManager::GetPlayerCountry() {
    return GameState::Get().GetPlayerCountry();  // the local player's country (not country 0)
}

std::string DiscoveriesManager::LocalizeKey(const std::string& key) {
    return SafeLocalize(base_address_, key);
}

nlohmann::json DiscoveriesManager::GetDiscoveriesInfo(const std::string& tab, bool probe) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };

    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not found"} };

    std::string q_tab = tab;
    std::transform(q_tab.begin(), q_tab.end(), q_tab.begin(), ::tolower);
    if (q_tab.empty()) q_tab = "all";

    nlohmann::json resp = {
        {"tab", q_tab}
    };

    // 1. Relics
    if (q_tab == "all" || q_tab == "relics") {
        void* r_db = nullptr;
        SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CRelicsDatabase_pInstance), &r_db);

        uint32_t total_relics_cnt = 0;
        void* r_arr = nullptr;
        if (r_db) {
            SafeReadU32((const void*)((uintptr_t)r_db + 0x5C), &total_relics_cnt);
            SafeReadPtr((const void*)((uintptr_t)r_db + 0x50), &r_arr);
        }

        // Player's held relics
        void* player_relics_arr = nullptr;
        uint32_t player_relics_cnt = 0;
        SafeReadPtr((const void*)((uintptr_t)country + kCountryOwnedRelics), &player_relics_arr);
        SafeReadU32((const void*)((uintptr_t)country + kCountryOwnedRelicsCount), &player_relics_cnt);

        nlohmann::json held_list = nlohmann::json::array();
        for (uint32_t i = 0; i < player_relics_cnt; ++i) {
            void* p_relic = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)player_relics_arr + i * 8), &p_relic) || !p_relic) continue;

            std::string r_key;
            SafeReadPdxString((const void*)((uintptr_t)p_relic + 0x20), r_key);
            if (r_key.empty()) continue;

            std::string loc_name = LocalizeKey(r_key);
            std::string desc_key = r_key + "_desc";
            std::string passive_key = r_key + "_passive";
            std::string triumph_key = r_key + "_triumph";

            // Ask the engine: build the activation command and run its IsValid without posting.
            namespace relic_cmd = sdk::cmd::activate_relic_command;
            auto probe = CommandBuilder::Get().Create(relic_cmd::kSpec);
            probe.SetString(relic_cmd::relic, r_key)
                 .Set<uint32_t>(relic_cmd::country, GameState::Get().GetPlayerCountryId());
            std::string why;
            bool can_activate = probe.IsValid(&why);

            nlohmann::json item = {
                {"key", r_key},
                {"name", loc_name},
                {"description", LocalizeKey(desc_key)},
                {"passive_effects", LocalizeKey(passive_key)},
                {"triumph_effects", LocalizeKey(triumph_key)},
                {"can_activate", can_activate}
            };
            if (!can_activate && !why.empty()) item["blocked_reason"] = why;
            held_list.push_back(item);
        }

        resp["relics"] = {
            {"held_count", held_list.size()},
            {"total_galaxy_relics", total_relics_cnt},
            {"can_activate_any", std::any_of(held_list.begin(), held_list.end(),
                                             [](const nlohmann::json& r) { return r.value("can_activate", false); })},
            {"held_relics", held_list}
        };
    }

    // 2. Astral Actions
    if (q_tab == "all" || q_tab == "astral_actions") {
        void* a_db = nullptr;
        SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CAstralActionsDatabase_pInstance), &a_db);

        uint32_t a_cnt = 0;
        void* a_arr = nullptr;
        if (a_db) {
            SafeReadU32((const void*)((uintptr_t)a_db + 0x5C), &a_cnt);
            SafeReadPtr((const void*)((uintptr_t)a_db + 0x50), &a_arr);
        }

        nlohmann::json actions_list = nlohmann::json::array();

        // 使用状态：容器头读一次，逐条按行动的下标取（见文件头 kCountryAstralUsage 的注释）。
        void* u_arr = nullptr;
        uint32_t u_cap = 0, u_size = 0;
        if (void* me = GetPlayerCountry()) {
            SafeReadPtr((const void*)((uintptr_t)me + kCountryAstralUsage + 0x08), &u_arr);
            SafeReadU32((const void*)((uintptr_t)me + kCountryAstralUsage + 0x10), &u_cap);
            SafeReadU32((const void*)((uintptr_t)me + kCountryAstralUsage + 0x14), &u_size);
            if (!u_arr) u_size = 0;
        }
        // 今天（游戏日期）的序数日，用来把 next_usable_date 折成「还剩几天」
        const GameDate today = GameState::Get().ReadDate();
        const uint32_t today_ord = (uint32_t)(today.year * 360u + (today.month ? (today.month - 1u) * 30u : 0u) +
                                              (today.day ? today.day - 1u : 0u));

        for (uint32_t i = 0; i < a_cnt && a_arr; ++i) {
            void* p_act = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)a_arr + i * 8), &p_act) || !p_act) continue;

            std::string a_key;
            SafeReadPdxString((const void*)((uintptr_t)p_act + 0x20), a_key);
            if (a_key.empty()) continue;

            nlohmann::json item = {
                {"key", a_key},
                {"name", LocalizeKey(a_key)},
                {"description", LocalizeKey(a_key + "_desc")}
            };
            // 使用状态（限次 / 冷却 / 已解锁）：一条行动一项，索引取行动对象 +0x08
            uint32_t a_idx = 0;
            if (u_size && SafeReadU32((const void*)((uintptr_t)p_act + 0x08), &a_idx) && a_idx < u_size) {
                const char* el = (const char*)u_arr + (size_t)a_idx * kAstralUsageStride;
                uint32_t unlockable = 0, uses = 0;
                uint64_t next = 0;
                SafeReadU32(el + 0x00, &unlockable);
                SafeReadU32(el + 0x04, &uses);
                SafeReadU64(el + 0x08, &next);
                item["index"] = a_idx;
                item["unlockable"] = unlockable != 0;
                item["remaining_uses"] = (int32_t)uses;
                if (next != 0) {
                    item["next_usable_date"] = DecodeGameDate((uint32_t)next);
                    item["next_usable_date_raw"] = (unsigned long long)next;
                    // 冷却还没走完就是正数；过去的日期（2276 那种）一律算 0，表示现在就能用
                    const uint32_t ord = (uint32_t)((next - 43800000ull) / 24ull);
                    item["cooldown_days_left"] = (today_ord && ord > today_ord) ? (int32_t)(ord - today_ord) : 0;
                }
            }
            // probe: 对每条行动各建一条 CActivateAstralActionCommand、只跑 IsValid（不投递）。
            // 用的是引擎自己的判定：消耗（星界丝缕）、剩余次数、冷却、potential 都会体现在里面。
            if (probe) {
                namespace aac = sdk::cmd::activate_astral_action_command;
                auto cmd = CommandBuilder::Get().Create(aac::kSpec);
                cmd.Set<void*>(aac::astral_action, p_act)
                   .Set<uint32_t>(aac::country, GameState::Get().GetPlayerCountryId());
                std::string why;
                bool can = cmd.IsValid(&why);
                item["can_activate"] = can;
                if (!can && !why.empty()) item["blocked_reason"] = why;
            }

            actions_list.push_back(item);
        }

        resp["astral_actions"] = {
            {"total_actions", actions_list.size()},
            {"usage_states", u_size},
            {"usage_states_capacity", u_cap},
            {"actions", actions_list}
        };
    }

    // 3. Artifact Actions (Minor Relics)
    if (q_tab == "all" || q_tab == "artifact_actions") {
        void* art_db = nullptr;
        SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CArtifactActionsDatabase_pInstance), &art_db);

        uint32_t art_cnt = 0;
        void* art_arr = nullptr;
        if (art_db) {
            SafeReadU32((const void*)((uintptr_t)art_db + 0x5C), &art_cnt);
            SafeReadPtr((const void*)((uintptr_t)art_db + 0x50), &art_arr);
        }

        nlohmann::json art_list = nlohmann::json::array();
        for (uint32_t i = 0; i < art_cnt && art_arr; ++i) {
            void* p_art = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)art_arr + i * 8), &p_art) || !p_art) continue;

            std::string art_key;
            SafeReadPdxString((const void*)((uintptr_t)p_art + 0x20), art_key);
            if (art_key.empty()) continue;

            art_list.push_back({
                {"key", art_key},
                {"name", LocalizeKey(art_key)},
                {"description", LocalizeKey(art_key + "_desc")}
            });
        }

        resp["artifact_actions"] = {
            {"total_actions", art_list.size()},
            {"actions", art_list}
        };
    }

    return resp;
}

nlohmann::json DiscoveriesManager::GetSummaryJson() {
    void* country = GetPlayerCountry();
    uint32_t relics_cnt = 0;
    if (country) {
        SafeReadU32((const void*)((uintptr_t)country + kCountryOwnedRelicsCount), &relics_cnt);
    }
    return {
        {"held_relics_count", relics_cnt},
        {"can_activate_relic", relics_cnt > 0}
    };
}

// 国家 flag 探针：SDK 给 CCountry+0x1A48 = flags（persistent），先吐原始字节看布局
nlohmann::json DiscoveriesManager::ProbeCountryFlags() {
    void* country = CountryById(base_address_, GameState::Get().GetPlayerCountryId());
    if (!country) return {{"error", "player country not found"}};
    const uintptr_t addr = (uintptr_t)country + 0x1A48;
    std::string hex;
    for (int i = 0; i < 0x60; i += 4) {
        uint32_t v = 0;
        if (!SafeReadU32((const void*)(addr + (uint32_t)i), &v)) break;
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%02X %02X %02X %02X ", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
        hex += tmp;
    }
    nlohmann::json out = {{"country_ptr", (unsigned long long)(uintptr_t)country},
                          {"addr", (unsigned long long)addr},
                          {"hex", hex}};
    // 猜测 A：{data, cap, size}（ref_array）
    uint64_t data = 0; uint32_t cap = 0, sz = 0;
    if (SafeReadU64((const void*)addr, &data) && SafeReadU32((const void*)(addr + 8), &cap) && SafeReadU32((const void*)(addr + 12), &sz)) {
        out["guess_array"] = {{"data", data}, {"cap", cap}, {"size", sz}};
        if (data > 0x10000 && sz > 0 && sz < 5000) {
            nlohmann::json sample = nlohmann::json::array();
            for (uint32_t i = 0; i < sz && i < 8; ++i) {
                uint64_t p = 0;
                if (!SafeReadU64((const void*)(data + i * 8), &p)) break;
                std::string s;
                if (p > 0x10000) SafeReadPdxString((const void*)p, s);
                sample.push_back({{"ptr", p}, {"str", s}});
            }
            out["guess_array"]["sample"] = sample;
        }
    }
    return out;
}
// —— 文物行动（Artifact Actions / 稀有文物）：读 + 启动（2026-11-22）——
static void* FindArtifactAction(uintptr_t base, const std::string& key) {
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base + sdk::glob::TGameDatabase_CArtifactActionsDatabase_pInstance), &db) || !db) return nullptr;
    uint32_t cnt = 0;
    void* arr = nullptr;
    SafeReadU32((const void*)((uintptr_t)db + 0x5C), &cnt);
    SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr);
    for (uint32_t i = 0; i < cnt && arr; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &p) || !p) continue;
        std::string k;
        SafeReadPdxString((const void*)((uintptr_t)p + 0x20), k);
        if (k == key) return p;
    }
    return nullptr;
}

nlohmann::json DiscoveriesManager::GetArtifactActions(bool probe) {
    void* db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CArtifactActionsDatabase_pInstance), &db);
    uint32_t cnt = 0;
    void* arr = nullptr;
    if (db) {
        SafeReadU32((const void*)((uintptr_t)db + 0x5C), &cnt);
        SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr);
    }
    nlohmann::json out = nlohmann::json::array();
    for (uint32_t i = 0; i < cnt && arr; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &p) || !p) continue;
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)p + 0x20), key);
        if (key.empty()) continue;
        nlohmann::json row = {{"key", key}, {"name", LocalizeKey(key)}, {"description", LocalizeKey(key + "_desc")}};
        if (probe) {
            namespace aac = sdk::cmd::artifact_action_command;
            auto cmd = CommandBuilder::Get().Create(aac::kSpec);
            cmd.Set<void*>(aac::artifact_action, p)
               .Set<uint32_t>(aac::country, GameState::Get().GetPlayerCountryId());
            std::string why;
            const bool ok = cmd.IsValid(&why);
            row["can_activate"] = ok;
            if (!ok) row["reason"] = why;
        }
        out.push_back(row);
    }
    return {{"total_actions", out.size()}, {"actions", out}};
}

bool DiscoveriesManager::ActivateArtifactAction(const std::string& action_key, std::string& out_message) {
    void* found = FindArtifactAction(base_address_, action_key);
    if (!found) { out_message = "Unknown artifact action key '" + action_key + "'"; return false; }
    namespace aac = sdk::cmd::artifact_action_command;
    auto cmd = CommandBuilder::Get().Create(aac::kSpec);
    cmd.Set<void*>(aac::artifact_action, found)
       .Set<uint32_t>(aac::country, GameState::Get().GetPlayerCountryId());
    std::string why;
    if (!cmd.IsValid(&why)) { out_message = why.empty() ? "The artifact action cannot be activated" : why; return false; }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) { out_message = cmd.error(); return false; }
    out_message = "Artifact action '" + action_key + "' activated";
    return true;
}
bool DiscoveriesManager::ActivateRelic(const std::string& relic_key, std::string& out_message) {
    void* country = GetPlayerCountry();
    if (!country) {
        out_message = "Player country not found";
        return false;
    }

    // Verify player owns the relic
    void* player_relics_arr = nullptr;
    uint32_t player_relics_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + kCountryOwnedRelics), &player_relics_arr);
    SafeReadU32((const void*)((uintptr_t)country + kCountryOwnedRelicsCount), &player_relics_cnt);

    bool has_relic = false;
    for (uint32_t i = 0; i < player_relics_cnt; ++i) {
        void* p_relic = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)player_relics_arr + i * 8), &p_relic) || !p_relic) continue;

        std::string r_key;
        SafeReadPdxString((const void*)((uintptr_t)p_relic + 0x20), r_key);
        if (r_key == relic_key) {
            has_relic = true;
            break;
        }
    }

    if (!has_relic) {
        out_message = "Your empire does not possess the relic '" + relic_key + "'";
        return false;
    }

    namespace relic = sdk::cmd::activate_relic_command;
    auto cmd = CommandBuilder::Get().Create(relic::kSpec);
    cmd.SetString(relic::relic, relic_key)
       .Set<uint32_t>(relic::country, GameState::Get().GetPlayerCountryId());
    if (!cmd.Post()) {
        out_message = cmd.error();
        return false;
    }

    out_message = "Successfully activated triumph effect of relic '" + LocalizeKey(relic_key) + "'";
    return true;
}

// 星界行动：按 key 从 CAstralActionsDatabase 取对象，再投 CActivateAstralActionCommand。
// 字段（SDK）：token 0x4290 / vtable RVA 0x2398598 / 0x30 字节；astral_action +0x20 是对象指针、
// country +0x28。IsValid 里带着引擎自己的判定（星界丝缕消耗、剩余次数、冷却、potential），
// 所以先把它的原因原样回给调用方，再投。
bool DiscoveriesManager::ActivateAstralAction(const std::string& action_key, std::string& out_message) {
    void* a_db = nullptr;
    SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CAstralActionsDatabase_pInstance), &a_db);
    if (!a_db) {
        out_message = "Astral action database not found";
        return false;
    }

    uint32_t cnt = 0;
    void* arr = nullptr;
    SafeReadU32((const void*)((uintptr_t)a_db + 0x5C), &cnt);
    SafeReadPtr((const void*)((uintptr_t)a_db + 0x50), &arr);

    void* found = nullptr;
    for (uint32_t i = 0; i < cnt && arr; ++i) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &p) || !p) continue;
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)p + 0x20), key);
        if (key == action_key) {
            found = p;
            break;
        }
    }
    if (!found) {
        out_message = "Unknown astral action key '" + action_key + "'";
        return false;
    }

    namespace aac = sdk::cmd::activate_astral_action_command;
    auto cmd = CommandBuilder::Get().Create(aac::kSpec);
    cmd.Set<void*>(aac::astral_action, found)
       .Set<uint32_t>(aac::country, GameState::Get().GetPlayerCountryId());
    std::string why;
    if (!cmd.IsValid(&why)) {
        out_message = why.empty() ? "The astral action cannot be activated" : why;
        return false;
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) {
        out_message = cmd.error();
        return false;
    }

    out_message = "Astral action activated: '" + LocalizeKey(action_key) + "'";
    return true;
}

// 只读探针：在 CCountry 里找大档案馆模块的 ref_array（storage_slots / exhibits 三类）。
// decode=true 时再走进模块，把数组里的 CExhibit 逐个解出来（标本键 / 发现者 owner）。
nlohmann::json DiscoveriesManager::ProbeGrandArchive(uint32_t country_id, bool decode) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* cdb = nullptr;
    void* slots = nullptr;
    void* country = nullptr;
    uint32_t cap = 0;
    uint32_t check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &country) || !country ||
        !SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &check) ||
        (check & 0xFFFFFFu) != (country_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(country_id)} };
    }

    // 一条 CExhibit 引用 -> 详情（id / 标本键 / 发现者 owner / 状态）
    auto decode_ref = [&](uint32_t ref) -> nlohmann::json {
        void* obj = ResolveExhibitObj(base_address_, ref);
        if (!obj) return nlohmann::json();
        const uint32_t id = ref & 0xFFFFFFu;
        uint64_t spec_raw = 0;
        SafeReadU64((const void*)((uintptr_t)obj + 0x20), &spec_raw);
        std::string spec_key;
        // CExhibit+0x20 是 persistent，可能是对象指针，也可能是标本库的引用 id
        void* sp = nullptr;
        // 2026-11-22 实测：标本对象在 CExhibit+0x28（+0x20 是模块里的一个静态指针）
        if (SafeReadPtr((const void*)((uintptr_t)obj + 0x28), &sp) && sp) {
            SafeReadPdxString((const void*)((uintptr_t)sp + 0x20), spec_key);
        }
        if (spec_key.empty() && SafeReadPtr((const void*)((uintptr_t)obj + 0x20), &sp) && sp) {
            SafeReadPdxString((const void*)((uintptr_t)sp + 0x20), spec_key);
        }
        if (spec_key.empty()) {
            void* sdb = nullptr;
            SafeReadPtr((const void*)(base_address_ + sdk::glob::TGameDatabase_CSpecimenDatabase_pInstance), &sdb);
            uint32_t scnt = 0;
            void* sarr = nullptr;
            if (sdb) {
                SafeReadU32((const void*)((uintptr_t)sdb + 0x5C), &scnt);
                SafeReadPtr((const void*)((uintptr_t)sdb + 0x50), &sarr);
            }
            const uint32_t sid = (uint32_t)(spec_raw & 0xFFFFFFu);
            if (sarr && sid < scnt) {
                void* ent = nullptr;
                if (SafeReadPtr((const void*)((uintptr_t)sarr + (size_t)sid * 8), &ent) && ent) {
                    SafeReadPdxString((const void*)((uintptr_t)ent + 0x20), spec_key);
                }
            }
        }
        uint32_t owner = 0, pack = 0;
        SafeReadU32((const void*)((uintptr_t)obj + 0x1A0), &owner);
        // +0x18 一个 dword：低字节 = exhibit_state，次字节 = activated（2026-11-22 实测：
        // 取消展览后 state 仍是 2、只有 activated 从 1 变 0，所以「展览中」看的是 activated）
        SafeReadU32((const void*)((uintptr_t)obj + 0x18), &pack);
        const uint32_t state = pack & 0xFFu;
        const uint32_t activated = (pack >> 8) & 0xFFu;
        return nlohmann::json{ {"id", id}, {"ptr", (unsigned long long)(uintptr_t)obj}, {"specimen", spec_key},
                               {"specimen_raw", (unsigned long long)spec_raw},
                               {"owner", owner & 0xFFFFFFu}, {"state", state}, {"activated", activated} };
    };

    auto candidate = [&](const char* kind, uint32_t off, uint64_t module, uint64_t data, uint32_t a, uint32_t b) -> nlohmann::json {
        if (!data || data < 0x10000 || a > 400 || b > 400 || b == 0) return nlohmann::json();
        uint32_t first = 0;
        if (!SafeReadU32((const void*)data, &first)) return nlohmann::json();
        if (!first || !ResolveExhibitObj(base_address_, first)) return nlohmann::json();
        nlohmann::json elems = nlohmann::json::array();
        for (uint32_t i = 0; i < b && i < 12; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)(data + (uint64_t)i * 4), &v)) break;
            elems.push_back(v);
        }
        nlohmann::json out{ {"kind", kind}, {"country_offset", off},
                            {"data", (unsigned long long)data}, {"a", a}, {"b", b}, {"elems", elems} };
        if (module) out["module"] = (unsigned long long)module;
        return out;
    };

    nlohmann::json found = nlohmann::json::array();
    uint64_t module_ptr = ModuleAt(base_address_, country, 0x2AB8);
    int best_distinct = 0;
    for (uint32_t off = 0; off + 16 <= 0x3FF0; off += 8) {
        uint64_t data = 0;
        uint32_t a = 0, b = 0;
        if (SafeReadU64((const void*)((uintptr_t)country + off), &data) &&
            SafeReadU32((const void*)((uintptr_t)country + off + 8), &a) &&
            SafeReadU32((const void*)((uintptr_t)country + off + 12), &b)) {
            auto c = candidate("inline", off, 0, data, a, b);
            if (!c.is_null()) found.push_back(c);
        }
        uint64_t p = 0;
        if (SafeReadU64((const void*)((uintptr_t)country + off), &p) && p >= 0x10000) {
            uint64_t mdata = 0;
            uint32_t ma = 0, mb = 0;
            if (SafeReadU64((const void*)((uintptr_t)p + 0x40), &mdata) &&
                SafeReadU32((const void*)((uintptr_t)p + 0x48), &ma) &&
                SafeReadU32((const void*)((uintptr_t)p + 0x4C), &mb)) {
                auto c = candidate("module+0x40", off, p, mdata, ma, mb);
                if (!c.is_null()) {
                    // 第一个「非零 id 至少两个」的模块候选就是大档案馆
                    if (!module_ptr) {
                        uint32_t distinct = 0, prev = 0xFFFFFFFFu;
                        bool sane = true;
                        for (const auto& e : c["elems"]) {
                            uint32_t v = e.get<uint32_t>();
                            v &= 0xFFFFFFu;
                            if (v > 1000000u) { sane = false; break; }
                            if (v && v != prev) { ++distinct; prev = v; }
                        }
                        if (!module_ptr && sane && distinct > best_distinct) { best_distinct = distinct; module_ptr = p; }
                    }
                    found.push_back(c);
                }
            }
        }
    }

    nlohmann::json result = { {"success", true}, {"country_id", country_id},
                              {"module", (unsigned long long)module_ptr},
                              {"count", found.size()}, {"candidates", found} };
    if (!decode || !module_ptr) return result;

    // 已知布局（2026-11-22 实测）：module+0x40 = storage_slots（flat ref_array）；
    // module+0x28 = 指向 3 个「展览格」包装对象，每个 0x18 字节 {vtable, data, cap, size}。
    nlohmann::json storage = nlohmann::json::array();
    uint64_t sdata = 0;
    uint32_t scap = 0, ssize = 0;
    if (SafeReadU64((const void*)(module_ptr + 0x40), &sdata) &&
        SafeReadU32((const void*)(module_ptr + 0x48), &scap) &&
        SafeReadU32((const void*)(module_ptr + 0x4C), &ssize)) {
        for (uint32_t i = 0; i < ssize && i < 200; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)(sdata + (uint64_t)i * 4), &v)) break;
            auto row = decode_ref(v);
            if (!row.is_null()) { row["slot"] = "storage"; storage.push_back(row); }
        }
    }
    nlohmann::json galleries = nlohmann::json::array();
    uint64_t gp = 0;
    if (SafeReadU64((const void*)(module_ptr + 0x28), &gp) && gp >= 0x10000) {
        for (uint32_t g = 0; g < 3; ++g) {
            uint64_t gdata = 0;
            uint32_t gcap = 0, gsize = 0;
            if (!SafeReadU64((const void*)(gp + g * 0x18 + 8), &gdata) ||
                !SafeReadU32((const void*)(gp + g * 0x18 + 0x10), &gcap) ||
                !SafeReadU32((const void*)(gp + g * 0x18 + 0x14), &gsize)) continue;
            nlohmann::json rows = nlohmann::json::array();
            for (uint32_t i = 0; i < gsize && i < 200; ++i) {
                uint32_t v = 0;
                if (!SafeReadU32((const void*)(gdata + (uint64_t)i * 4), &v)) break;
                auto row = decode_ref(v);
                if (!row.is_null()) { row["slot"] = g; rows.push_back(row); }
            }
            galleries.push_back({ {"index", g}, {"data", (unsigned long long)gdata},
                                  {"a", gcap}, {"b", gsize}, {"rows", rows} });
        }
    }
    result["storage"] = storage;
    result["galleries"] = galleries;
    return result;
}

// 激活/取消展览：CToggleActivateExhibitCommand（token 0x433D，country +0x20、
// exhibition_index +0x24、exhibit_index +0x28）。字段是**下标**，所以要先在展览格里找到位置。
bool DiscoveriesManager::ToggleExhibit(uint32_t exhibit_id, std::string& out_message, bool force_toggle) {
    const uint32_t country_id = GameState::Get().GetPlayerCountryId();
    void* country = CountryById(base_address_, country_id);
    if (!country) { out_message = "player country not found"; return false; }
    const uint64_t module = FindArchiveModule(base_address_, country);
    if (!module) { out_message = "grand archive module not found"; return false; }
    int g = -1, i = -1;
    void* obj = FindExhibitInGalleries(base_address_, module, exhibit_id, &g, &i);
    if (!obj) {
        out_message = "exhibit " + std::to_string(exhibit_id) + " is not in any gallery (move it out of storage first)";
        return false;
    }
    uint32_t pack = 0;
    SafeReadU32((const void*)((uintptr_t)obj + 0x18), &pack);
    const uint32_t activated = (pack >> 8) & 0xFFu;
    const uint32_t state = pack & 0xFFu;
    // 2026-11-22 录制：界面上给「未展览」的标本点「激活展览」发的是 CUnlockExhibitCommand（0x433C），
    // 不是 toggle。所以按当前状态选命令：未展览(1) -> unlock_exhibit；展览中(2) -> toggle_activate_exhibit。
    namespace ue = sdk::cmd::unlock_exhibit;
    namespace tac = sdk::cmd::toggle_activate_exhibit;
    const bool needUnlock = !force_toggle && state == 1;   // state 1 = 还没开槽，要 unlock；2 = 槽已开、用 toggle 切换展览
    auto cmd = CommandBuilder::Get().Create(needUnlock ? ue::kSpec : tac::kSpec);
    const auto country_off = needUnlock ? ue::country : tac::country;
    const auto exhibition_off = needUnlock ? ue::exhibition_index : tac::exhibition_index;
    const auto exhibit_off = needUnlock ? ue::exhibit_index : tac::exhibit_index;
    cmd.Set<uint32_t>(country_off, country_id)
       .Set<int32_t>(exhibition_off, g)
       .Set<int32_t>(exhibit_off, i);
    std::string why;
    if (!cmd.IsValid(&why)) { out_message = why.empty() ? "exhibit cannot be toggled" : why; return false; }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) { out_message = cmd.error(); return false; }
    out_message = "Toggled exhibit " + std::to_string(exhibit_id) + " (gallery " + std::to_string(g) +
                  ", index " + std::to_string(i) + ", activated was " + std::to_string(activated) +
                  "); read stellaris_specimens again to confirm";
    return true;
}

// 发送到库存 / 发送到展览格：CMoveSpecimenToStorageCommand（0x435F）/ CMoveSpecimenFromStorageCommand（0x4360）。
// 两条字段一样：country +0x20、exhibit（ref<CExhibit>）+0x24。
bool DiscoveriesManager::MoveSpecimen(uint32_t exhibit_id, bool to_storage, std::string& out_message, bool force) {
    const uint32_t country_id = GameState::Get().GetPlayerCountryId();
    void* country = CountryById(base_address_, country_id);
    if (!country) { out_message = "player country not found"; return false; }
    const uint64_t module = FindArchiveModule(base_address_, country);
    if (!module) { out_message = "grand archive module not found"; return false; }

    namespace mts = sdk::cmd::move_specimen_to_storage;
    namespace mfs = sdk::cmd::move_specimen_from_storage;
    auto cmd = CommandBuilder::Get().Create(to_storage ? mts::kSpec : mfs::kSpec);
    // exhibit id 0 在命令里会被当成空引用（IsValid 静默失败），加标记位来避开；其他 id 用裸值。
    const uint32_t ref = (exhibit_id == 0) ? 0x01000000u : exhibit_id;
    cmd.Set<uint32_t>(mts::country, country_id)
       .Set<uint32_t>(mts::exhibit, ref);
    std::string why;
    if (!cmd.IsValid(&why)) {
        out_message = why.empty() ? "the specimen cannot be moved" : why;
        return false;
    }
    if (!cmd.Post(NativeCommand::Check::EngineGate)) { out_message = cmd.error(); return false; }
    out_message = std::string("Moved specimen ") + std::to_string(exhibit_id) +
                  (to_storage ? " to storage" : " to its gallery") +
                  " (the game leaves an empty slot in the old array and gives the specimen a new id — re-read)";
    return true;
}
} // namespace bridge





























