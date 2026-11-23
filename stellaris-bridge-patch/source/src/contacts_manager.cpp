#include "contacts_manager.hpp"
#include "game_state.hpp"
#include "fleet_access.hpp"
#include "event_manager.hpp"
#include "sdk/stellaris_sdk.hpp"
#include "command_builder.hpp"
#include "alert_manager.hpp"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace bridge {

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

static bool SafeReadU8(const void* addr, uint8_t* out) {
    __try {
        *out = *(const uint8_t*)addr;
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

ContactsManager& ContactsManager::Get() {
    static ContactsManager instance;
    return instance;
}

bool ContactsManager::Init(uintptr_t base_address) {
    base_address_ = base_address;


    return true;
}

namespace {

// a script database (TGameDatabase<T>): its types (data at +0x50, count at +0x5C), key at +0x20
std::vector<void*> ScriptTypes(uintptr_t db_global) {
    std::vector<void*> out;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t n = 0;
    if (!SafeReadPtr((const void*)db_global, &db) || !db || !SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x5C), &n) || n > 4096) {
        return out;
    }
    for (uint32_t i = 0; i < n; ++i) {
        void* t = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)arr + i * 8), &t) && t) out.push_back(t);
    }
    return out;
}
constexpr std::ptrdiff_t kTypeKey = 0x20;

// SEH 保护的 C 字符串读取（2026-11-21）：固定缓冲、__try 里不出现 C++ 对象，避免 C2712。
// 只认可打印 ASCII；遇到 0 / 不可打印就停。猜错的指针不会把游戏带走。
static bool SafeReadCStr(const void* addr, char* buf, size_t cap) {
    __try {
        const char* p = (const char*)addr;
        for (size_t i = 0; i + 1 < cap; ++i) {
            const char c = p[i];
            buf[i] = c;
            if (!c) return true;
            if ((unsigned char)c < 32 || (unsigned char)c > 126) { buf[i] = 0; return true; }
        }
        buf[cap - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 写引擎内存之前先确认目标在**可写**区间（2026-11-21 09:39 静默崩溃之后加的护栏）。
// 盲写只读段/错偏移的后果是进程直接消失、连 dump 都没有，所以宁可先问 VirtualQuery 再写。
static bool WritableRange(const void* addr, size_t len) {
    if (!addr || len == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t p = (uintptr_t)addr;
    const uintptr_t end = p + len;
    while (p < end) {
        if (!VirtualQuery((const void*)p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect;
        const bool writable = (prot & PAGE_READWRITE) || (prot & PAGE_WRITECOPY) ||
                              (prot & PAGE_EXECUTE_READWRITE) || (prot & PAGE_EXECUTE_WRITECOPY);
        if (!writable || (prot & PAGE_GUARD)) return false;
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// —— 改协议动作里的条款（2026-11-21）——
// 内嵌协议在动作 +0x58、序列化子对象在 +0x68，所以条款数组相对**动作**是：
//   离散 +0xC8（data）/ +0xD0（低 32 容量、高 32 条数），每格 0x10：[key_ptr, value_ptr]
//   资源 +0xE0（data）/ +0xE8（同上），每格 0x10：[key_ptr, i64 定点 ×1e5]
// 键与值都是「对象 +0x20 的 PdxString」；值对象按 key 从 CAgreementTermValueDatabase 取。
static void* FindTermValueObject(uintptr_t base_address, const std::string& key) {
    for (void* t : ScriptTypes(base_address + sdk::glob::TGameDatabase_CAgreementTermValueDatabase_pInstance)) {
        std::string k;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == key) return t;
    }
    return nullptr;
}

static bool PatchAgreementTerms(void* action, uintptr_t base_address, const nlohmann::json& terms,
                                const nlohmann::json& subsidies, nlohmann::json* readback, std::string& err) {
    // 0) 先认类：只有协议类动作（要求臣服 0x23CCCF8 / 请求成为附庸 0x23CC440）才动
    uint64_t vt = 0;
    if (!SafeReadPtr(action, (void**)&vt) || vt < base_address) { err = "动作对象读不出 vtable"; return false; }
    const uint64_t vrva = vt - base_address;
    if (vrva != 0x23CCCF8ULL && vrva != 0x23CC440ULL) {
        err = "这不是协议类动作（vtable RVA 与 CProposeAgreementAction 不符），条款无处可写";
        return false;
    }
    // 1) 条款数组：离散 +0xC8/+0xD0，资源 +0xE0/+0xE8（低 32 容量、高 32 条数）
    uint64_t disc = 0, disc_meta = 0, res = 0, res_meta = 0;
    if (!SafeReadU64((const void*)((uintptr_t)action + 0xC8), &disc) || disc < 0x10000ULL) {
        err = "这份动作没有内嵌协议的条款数组";
        return false;
    }
    SafeReadU64((const void*)((uintptr_t)action + 0xD0), &disc_meta);
    uint32_t dn = (uint32_t)(disc_meta >> 32);
    uint32_t dcap = (uint32_t)(disc_meta & 0xFFFFFFFF);
    if (dn == 0 || dn > 64 || dcap < dn || dcap > 256) { err = "离散条款数组的条数/容量不像真的"; return false; }
    if (!WritableRange((const void*)disc, (size_t)dn * 0x10)) {
        err = "离散条款数组不在可写内存里（可能是共享的预设数据）——不写";
        return false;
    }
    // 2) 离散条款：按 key 找格，把 value 指针换成目标值对象
    if (terms.is_object()) {
        for (auto it = terms.begin(); it != terms.end(); ++it) {
            const std::string key = it.key();
            const std::string want = it.value().is_string() ? it.value().get<std::string>() : std::string();
            if (want.empty()) continue;
            void* vo = FindTermValueObject(base_address, want);
            if (!vo) { err = "未知条款值 " + want + "（CAgreementTermValueDatabase 里没有）"; return false; }
            bool done = false;
            for (uint32_t i = 0; i < dn; ++i) {
                uint64_t kp = 0;
                if (!SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10), &kp) || kp < 0x10000ULL) continue;
                std::string k;
                if (!SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k) || k != key) continue;
                *(void**)(uintptr_t)(disc + i * 0x10 + 8) = vo;
                done = true;
                break;
            }
            if (!done) { err = "这份动作里没有条款 " + key; return false; }
        }
    }
    // 3) 资源条款：直接写 i64 定点（×1e5；宗主给附庸是负数）
    if (subsidies.is_object() && !subsidies.empty()) {
        if (!SafeReadU64((const void*)((uintptr_t)action + 0xE0), &res) || res < 0x10000ULL) {
            err = "这份动作没有资源条款数组";
            return false;
        }
        SafeReadU64((const void*)((uintptr_t)action + 0xE8), &res_meta);
        uint32_t rn = (uint32_t)(res_meta >> 32);
        uint32_t rcap = (uint32_t)(res_meta & 0xFFFFFFFF);
        if (rn == 0 || rn > 16 || rcap < rn || rcap > 64) { err = "资源条款数组的条数/容量不像真的"; return false; }
        if (!WritableRange((const void*)res, (size_t)rn * 0x10)) {
            err = "资源条款数组不在可写内存里——不写";
            return false;
        }
        struct Pair { const char* arg; const char* key; };
        static const Pair kPairs[] = {
            {"basic", "resource_subsidies_basic"},
            {"advanced", "resource_subsidies_advanced"},
            {"research", "resource_subsidies_research"},
            {"strategic", "resource_subsidies_strategic"},
            {"unity", "resource_subsidies_unity"},
        };
        for (const auto& pr : kPairs) {
            if (!subsidies.contains(pr.arg) || !subsidies[pr.arg].is_number()) continue;
            const double pct = subsidies[pr.arg].get<double>();
            int64_t fixed = (int64_t)(pct * 100000.0 + (pct >= 0 ? 0.5 : -0.5));
            if (fixed > 0) fixed = -fixed;
            bool done = false;
            for (uint32_t i = 0; i < rn; ++i) {
                uint64_t kp = 0;
                if (!SafeReadU64((const void*)(uintptr_t)(res + i * 0x10), &kp) || kp < 0x10000ULL) continue;
                std::string k;
                if (!SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k) || k != pr.key) continue;
                *(int64_t*)(uintptr_t)(res + i * 0x10 + 8) = fixed;
                done = true;
                break;
            }
            if (!done) { err = std::string("这份动作里没有资源条款 ") + pr.key; return false; }
        }
    }
    // 4) 回读：dry_run 用，确认写进去的东西真的是我们想要的
    if (readback) {
        nlohmann::json out = nlohmann::json::array();
        for (uint32_t i = 0; i < dn; ++i) {
            uint64_t kp = 0, vp = 0;
            if (!SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10), &kp)) break;
            SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10 + 8), &vp);
            std::string k, v;
            if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
            if (vp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(vp + 0x20), v);
            if (!k.empty()) out.push_back({{"key", k}, {"value", v}});
        }
        nlohmann::json rout = nlohmann::json::array();
        if (res > 0x10000ULL) {
            uint32_t rn2 = (uint32_t)(res_meta >> 32);
            if (rn2 > 16) rn2 = 16;
            for (uint32_t i = 0; i < rn2; ++i) {
                uint64_t kp = 0, v = 0;
                if (!SafeReadU64((const void*)(uintptr_t)(res + i * 0x10), &kp)) break;
                SafeReadU64((const void*)(uintptr_t)(res + i * 0x10 + 8), &v);
                std::string k;
                if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                if (!k.empty()) rout.push_back({{"key", k}, {"value", (double)(int64_t)v / 100000.0}});
            }
        }
        *readback = {{"terms", out}, {"resource_terms", rout}};
    }
    return true;
}

// —— 调引擎自己的「设条款」函数（2026-11-21）——
// 0x7A9740(container, key_ptr, value_ptr)：在 0x10 步长的 [key,value] 数组里按 key 找格，
// 找到就把 value 换成新指针；找不到就调 0x7AAE00 插入。容器布局：data +0x08、条数 +0x14。
// 这是最安全的一条路：内存由引擎自己分配，所有权始终在它手里。
struct SetTermCtx {
    uintptr_t fn;
    void* container;
    void* key;
    void* value;
};

void CallSetTerm(void* p, void*) {
    auto* c = (SetTermCtx*)p;
    ((void (*)(void*, void*, void*))c->fn)(c->container, c->key, c->value);
}

// —— 在动作里把内嵌协议构造出来（2026-11-21）——
// 生产者造的动作只给了空壳：协议区（动作 +0x58）是零，term data 的 vtable 也没写。
// 0x1E1A00 就是 CAgreement 的构造器（我们反汇编看到的那个：写两个 vtable、owner/target=-1、
// 日期置纪元值、再在 +0x68 调 CAgreementTermData 构造器）。
struct Ctor1Ctx {
    uintptr_t fn;
    void* obj;
};

void CallCtor1(void* p, void*) {
    auto* c = (Ctor1Ctx*)p;
    ((void (*)(void*))c->fn)(c->obj);
}

static bool InitAgreementInAction(void* action, uintptr_t base_address, std::string& err) {
    void* agree = (void*)((uintptr_t)action + 0x58);
    uint64_t vt = 0;
    SafeReadPtr((const void*)agree, (void**)&vt);
    if (vt - base_address != 0x23401A8ULL) {   // 还没构造
        Ctor1Ctx c{ base_address + 0x1E1A00ULL, agree };
        if (!CommandBuilder::Get().CallGuarded(&CallCtor1, &c)) { err = "CAgreement 构造器调用失败（SEH）"; return false; }
    }
    uint64_t td = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)action + 0xC0), (void**)&td) || td - base_address != 0x2304418ULL) {
        err = "构造之后 term data 的 vtable 仍不符";
        return false;
    }
    return true;
}

static bool ApplyAgreementTerms(void* action, uintptr_t base_address, const nlohmann::json& terms,
                                const nlohmann::json& subsidies, std::string& err) {
    const uintptr_t termdata = (uintptr_t)action + 0xC0;   // 动作 +0x58 是协议，协议 +0x68 是 term data
    uint64_t vt = 0;
    if (!SafeReadPtr((const void*)termdata, (void**)&vt) || vt - base_address != 0x2304418ULL) {
        err = "动作里没有 term data（vtable 不符）";
        return false;
    }
    for (auto it = terms.begin(); it != terms.end(); ++it) {
        const std::string key = it.key();
        const std::string want = it.value().is_string() ? it.value().get<std::string>() : std::string();
        if (want.empty()) continue;
        void* kobj = nullptr;
        for (void* t : ScriptTypes(base_address + sdk::glob::TGameDatabase_CAgreementTermDatabase_pInstance)) {
            std::string k;
            if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == key) { kobj = t; break; }
        }
        if (!kobj) { err = "条款库里没有 " + key; return false; }
        void* vobj = FindTermValueObject(base_address, want);
        if (!vobj) { err = "条款值库里没有 " + want; return false; }
        SetTermCtx ctx{ base_address + 0x7A9740ULL, (void*)(termdata + 0x00), kobj, vobj };
        if (!CommandBuilder::Get().CallGuarded(&CallSetTerm, &ctx)) { err = "调设条款函数失败（SEH）"; return false; }
    }
    // 资源条款（容器 B 在 term data +0x18）：同一个 setter，r8 直接给 i64 定点值（不是指针）
    if (subsidies.is_object() && !subsidies.empty()) {
        nlohmann::json::object_t pairs = {
            {"basic", "resource_subsidies_basic"}, {"advanced", "resource_subsidies_advanced"},
            {"research", "resource_subsidies_research"}, {"strategic", "resource_subsidies_strategic"},
            {"unity", "resource_subsidies_unity"},
        };
        for (auto& kv : pairs) {
            const std::string arg = kv.first;
            const std::string rkey = kv.second.get<std::string>();
            if (!subsidies.contains(arg) || !subsidies[arg].is_number()) continue;
            const double pct = subsidies[arg].get<double>();
            int64_t fixed = (int64_t)(pct * 100000.0 + (pct >= 0 ? 0.5 : -0.5));
            if (fixed > 0) fixed = -fixed;   // 宗主给附庸：负数
            // 资源键（resource_subsidies_basic 这类）不在条款库里，而在**条款值库**里
            void* kobj = FindTermValueObject(base_address, rkey);
            if (!kobj) { err = "资源键 " + rkey + " 在条款值库里也没有"; return false; }
            SetTermCtx ctx{ base_address + 0x7A9740ULL, (void*)(termdata + 0x18), kobj, (void*)(intptr_t)fixed };
            if (!CommandBuilder::Get().CallGuarded(&CallSetTerm, &ctx)) { err = "调资源条款函数失败（SEH）"; return false; }
        }
    }
    return true;
}

struct NewActionCtx {
    uintptr_t create;
    void* type;
    void* actor;
    void* recipient;
    void* action;
    uint8_t shown;
};

void CallNewAction(void* p, void*) {
    auto* x = (NewActionCtx*)p;
    x->action = ((void* (*)(void*, void*, void*))x->create)(x->type, x->actor, x->recipient);
    if (x->action) {
        void** vt = *(void***)x->action;
        x->shown = ((uint8_t(*)(void*, bool))vt[sdk::vt::CDiplomaticAction_IsPotential])(x->action, false);
    }
}

void CallDeleteAction(void* action, void*) {
    void** vt = *(void***)action;
    ((void (*)(void*, int))vt[sdk::vt::CDiplomaticAction_Delete])(action, 1);
}

// 只造、不筛：隔开 IsPotential，好让 is_listed = no 的动作（求和）也能被看一眼
struct CreateOnlyCtx {
    uintptr_t create;
    void* type;
    void* actor;
    void* recipient;
    void* action;
};

void CallCreateOnly(void* p, void*) {
    auto* x = (CreateOnlyCtx*)p;
    x->action = ((void* (*)(void*, void*, void*))x->create)(x->type, x->actor, x->recipient);
}

struct PotentialCtx {
    void* action;
    uint8_t flag;
    uint8_t out;
    uint8_t ok;
};

void CallIsPotential(void* p, void*) {
    auto* x = (PotentialCtx*)p;
    void** vt = *(void***)x->action;
    x->out = ((uint8_t(*)(void*, bool))vt[sdk::vt::CDiplomaticAction_IsPotential])(x->action, x->flag != 0);
    x->ok = 1;
}

}  // namespace

void* ContactsManager::NewAction(void* type, uint32_t country_id, bool* shown) {
    void* cdb = nullptr;
    void* slots = nullptr;
    void* recipient = nullptr;
    uint32_t cap = 0, check = 0xFFFFFFFF;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots || !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) ||
        (country_id & 0xFFFFFF) >= cap || !SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &recipient) ||
        !recipient || !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) || (check & 0xFFFFFFu) != (country_id & 0xFFFFFFu)) {
        return nullptr;
    }
    NewActionCtx ctx{ base_address_ + sdk::fn::NDiplomacyUtil_CreateDiplomaticAction, type, GetPlayerCountry(), recipient, nullptr, 0 };
    if (!ctx.actor || !CommandBuilder::Get().CallGuarded(&CallNewAction, &ctx)) return nullptr;
    *shown = ctx.shown != 0;
    return ctx.action;
}

nlohmann::json ContactsManager::GetDiplomaticActionsJson(uint32_t country_id) {
    if (!GetPlayerCountry()) return { {"error", "No player country (not in game?)"} };
    nlohmann::json out = nlohmann::json::array();
    namespace pr = sdk::cmd::diplo_action_propose_command;
    constexpr std::ptrdiff_t kAction = 0x20;  // CPdxScopedPtr<CDiplomaticAction>: the command owns it
    bool any = false;
    for (void* type : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        bool shown = false;
        void* action = NewAction(type, country_id, &shown);
        if (!action) continue;
        any = true;
        if (!shown) {
            CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
            continue;
        }
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)type + kTypeKey), key);
        auto cmd = CommandBuilder::Get().Create(pr::kSpec);
        cmd.Set<uint64_t>(kAction, (uint64_t)(uintptr_t)action);  // freed with the unposted command
        std::string why;
        const bool can = cmd.IsValid(&why);
        nlohmann::json row = { {"action", key}, {"can_propose", can} };
        if (!can) row["reason"] = AlertManager::Get().CleanPdxString(why);
        out.push_back(row);
    }
    if (!any && out.empty()) return { {"error", "No empire with country id " + std::to_string(country_id)} };
    return { {"country_id", country_id}, {"actions", out} };
}

nlohmann::json ContactsManager::ProposeDiplomaticAction(uint32_t country_id, const std::string& action_key,
                                                        const std::string& federation_type,
                                                        const nlohmann::json& terms,
                                                        const nlohmann::json& subsidies,
                                                        bool dry_run) {
    if (!GetPlayerCountry()) return { {"error", "No player country (not in game?)"} };
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == action_key) type = t;
    }
    if (!type) return { {"success", false}, {"error", "Unknown diplomatic action " + action_key + " (stellaris_get_diplomatic_actions)"} };
    bool shown = false;
    void* action = NewAction(type, country_id, &shown);
    if (!action) return { {"success", false}, {"error", "No empire with country id " + std::to_string(country_id) + ", or the game makes no such action toward it"} };
    if (!shown) {
        CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
        return { {"success", false}, {"error", action_key + " is not offered toward this empire"} };
    }
    // 2026-11-21：组建新联邦时的类型写在这个指针上（CInviteToFederationAction::federation_type +0xB0）。
    // 类型对象的键字符串在对象 +0x28（实测 default_federation），与外交动作类型那套 +0x20 不同。
    if (!federation_type.empty()) {
        void* ftype = nullptr;
        for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CFederationTypeDatabase_pInstance)) {
            std::string k;
            // 2026-11-21 只读探针实测：类型键在 **+0x20**（+0x28 是共享指针，按它读会把游戏读崩）
            if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == federation_type) { ftype = t; break; }
        }
        if (!ftype) {
            CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
            return { {"success", false},
                     {"error", "unknown federation type " + federation_type +
                                   "（用 stellaris_form_federation { list_types: true } 列可用的）"} };
        }
        // 同样先验 vtable（CFederationType RVA 0x2388990），防止拿错对象写进 +0xB0
        uint64_t ft_vt = 0;
        if (!SafeReadPtr(ftype, (void**)&ft_vt) || (unsigned long long)(ft_vt - base_address_) != 0x2388990ULL) {
            CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
            return { {"success", false}, {"error", "federation type " + federation_type + " 的对象校验失败（vtable 不符）"} };
        }
        *(void**)((uintptr_t)action + 0xB0) = ftype;
    }
    namespace pr = sdk::cmd::diplo_action_propose_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    // 2026-11-21：带条款地发（UI 的发送方式）。生产者造出来的动作已经内嵌一份协议（默认预设条款），
    // 这里按 key 把离散条款的 value 指针换成目标值对象、把资源条款的 i64 改掉。
    bool terms_patched = false;
    nlohmann::json readback;
    if ((terms.is_object() && !terms.empty()) || (subsidies.is_object() && !subsidies.empty())) {
        std::string terr;
        // 协议类动作：先把内嵌协议构造出来（生产者只给空壳），再用引擎自己的「设条款」函数填条款，
        // 最后把 owner/target 写成我们自己与目标国。全程不手动删对象（交给命令/引擎）。
        if (!InitAgreementInAction(action, base_address_, terr)) {
            return { {"success", false}, {"error", terr} };
        }
        if (!ApplyAgreementTerms(action, base_address_, terms, subsidies, terr)) {
            return { {"success", false}, {"error", terr} };
        }
        uint32_t my_id = 0xFFFFFFFFu;
        void* me = GetPlayerCountry();
        if (me) SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id);
        if (my_id != 0xFFFFFFFFu) {
            *(uint32_t*)((uintptr_t)action + 0x70) = my_id;       // agreement.owner
            *(uint32_t*)((uintptr_t)action + 0x74) = country_id;  // agreement.target
        }
        terms_patched = true;
    }
    if (dry_run) {
        // 2026-11-21：dry_run **不再手动删动作**。静默崩溃之后按「交给命令的动作不要再手动删」那条老规矩，
        // 这里让对象泄漏一点内存，也不去碰析构（析构会连带销毁内嵌协议与其条款）。
        return { {"success", true}, {"dry_run", true}, {"terms_patched", terms_patched}, {"patched", readback},
                 {"action_leaked", (unsigned long long)(uintptr_t)action},
                 {"note", "只造动作 + 改条款 + 回读，没有投递，也没有删除（对象故意留着）"} };
    }
    // Post runs CDiploActionProposeCommand::IsValid first: refused with the game's reason
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())} };
    }
    LOGF("[CONTACTS] proposed %s to country %u (terms_patched=%d)", action_key.c_str(), country_id, terms_patched ? 1 : 0);
    return { {"success", true}, {"country_id", country_id}, {"action", action_key}, {"terms_patched", terms_patched},
             {"message", "Proposed; the empire answers on its own time (stellaris_get_contacts)"} };
}

// ——发起一条联邦法律表决（2026-11-21）——
// 引擎没有「提交法律提案」的命令（法律投票通过时也不产生命令），UI 走的是：
//   造 CVoteForFederationLawAction（动作类型 action_vote_for_federation_law）→ 把 law 指针写进 +0x50
//   → 包成 CDiploActionProposeCommand 投递。这里照抄这条路径。
// law_key 从 CFederationLawDatabase（0x3119638）按键取；对象 vtable RVA 0x2388C28（实测）。
nlohmann::json ContactsManager::ProposeFederationLaw(const std::string& law_key, bool dry_run, uint32_t recipient_id) {
    void* me = GetPlayerCountry();
    if (!me) return { {"success", false}, {"error", "not in game"} };
    uint32_t my_id = 0xFFFFFFFFu;
    if (!SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    void* law = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CFederationLawDatabase_pInstance)) {
        std::string k;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == law_key) { law = t; break; }
    }
    if (!law) return { {"success", false}, {"error", "未知法律键 " + law_key + "（用 stellaris_state probe_law_db 列）"} };
    uint64_t lvt = 0;
    if (!SafeReadPtr(law, (void**)&lvt) || (unsigned long long)(lvt - base_address_) != 0x2388C28ULL) {
        return { {"success", false}, {"error", "法律对象 vtable 校验失败（不是 CFederationLaw）"} };
    }
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string k;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == "action_vote_for_federation_law") { type = t; break; }
    }
    if (!type) return { {"success", false}, {"error", "动作类型库里没有 action_vote_for_federation_law"} };
    bool shown = false;
    // 收件方默认是自己（法律表决是联邦内部的事）；给了 country_id 就用那个——引擎的生产者
    // 对某些动作类型要求一个"对方"，所以留个试错口。
    const uint32_t to_id = (recipient_id == 0xFFFFFFFFu) ? my_id : recipient_id;
    void* action = NewAction(type, to_id, &shown);
    bool manual = false;
    if (!action) {
        // 生产者不造（action_vote_for_federation_law 是 auto_accepted = yes，不是"提案"）。
        // 照 propose_capture 实测的形状手工摆一个：vtable / 基类 / +0x20 发起国 / +0x40 类型 / +0x50 法律。
        // 2026-11-21 实测（law_hits offset 80）：law 就在 +0x50；对象比求和动作（0x68）短，先按 0x60 申请。
        action = CommandBuilder::Get().EngineAlloc(0x60);
        if (!action) return { {"success", false}, {"error", "EngineAlloc 失败"} };
        *(void**)((uintptr_t)action + 0x00) = (void*)(base_address_ + 0x23B7050ULL);  // CVoteForFederationLawAction
        *(void**)((uintptr_t)action + 0x08) = (void*)(base_address_ + 0x2388BA0ULL);  // 基类 vtable
        *(uint32_t*)((uintptr_t)action + 0x20) = my_id;
        *(void**)((uintptr_t)action + 0x40) = type;
        *(void**)((uintptr_t)action + 0x50) = law;
        manual = true;
        LOGF("[CONTACTS] hand-built law action for %s", law_key.c_str());
    }
    *(void**)((uintptr_t)action + 0x50) = law;
    namespace pr = sdk::cmd::diplo_action_propose_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    if (dry_run) {
        std::string reason;
        const bool ok = cmd.IsValid(&reason);
        return { {"success", ok}, {"dry_run", true}, {"law_key", law_key}, {"hand_built", manual},
                 {"reason", AlertManager::Get().CleanPdxString(reason)},
                 {"note", "只跑 IsValid，没有投递；动作对象留在命令里随作用域销毁"} };
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())} };
    }
    LOGF("[CONTACTS] proposed federation law %s", law_key.c_str());
    return { {"success", true}, {"law_key", law_key},
             {"message", "已发起；表决落在我们的 proposed_actions 里（stellaris_diplo_vote 可见）"} };
}

// ———— 贸易协定（2026-11-21 抓现场后接线）————
// 布局（两个方向都验过：alloys=100 → 第 10 格、energy=100 → 第 0 格）：
//   CAbstractTradeDealAction +0x20 actor / +0x24 recipient；+0x50 是**内嵌**的 CTradeDeal（SDK 标 ref，实际按值）
//   CTradeDeal: first +0x18 / second +0x130 / killed +0x248 / length +0x249（**年**，不是月）/ date +0x250
//   CTradeDealSide: country +0x8 / recipient +0xC / resources +0xC0 / monthly_resources +0xE0 / reserve +0x106
//   每张表：+0x00 数据指针、+0x08 {size=26, cap=26}；数据是 26 格 i64 定点（×1e5），索引 = 资源声明序
//   （common/strategic_resources/00_strategic_resources.txt 的顶层顺序：energy=0 … alloys=10 …）
// reserve（侧 +0x106）：递交时给出的资源会立即扣除、被拒再退回——给东西的那侧 UI 填 1。
// ———— 换联邦类型表决（2026-11-21 抓 UI 现场后接）————
// UI 投的是 CVoteForFederationTypeAction：vtable RVA 0x23B7548、基类 vtable 0x2388BA0、
// +0x20 发起国、+0x40 动作类型对象（action_vote_for_federation_type）、+0x50 CFederationType*（键在 +0x20）、
// +0x58 名字（UI 传的是模块内共享对象，RVA 0x234A778）。生产者不造这个动作，所以手工摆。
// 实测样本：把研究合作组织换成霸权（hegemony_federation），落成一条「联邦类型表决 · 霸权」。
nlohmann::json ContactsManager::ProposeFederationType(const std::string& type_key, bool dry_run) {
    // ⚠ 2026-11-21 两次静默崩溃（无 crash dump），都出在手搓这个动作对象上：
    //   ① 旧版：0x60 字节 + 把 +0x58（名字）指向模块静态对象 → IsValid 失败路径上就崩
    //   ② 新版：0x80 字节 + +0x58 = 0 → 换「贸易联盟」那次（IsValid 失败）安然无恙；
    //      换「霸权」那次（IsValid **通过**）又崩 —— 引擎在“通过之后”还要用 name 之类
    //      我们没构造完整的字段。
    // 结论：这条动作不能手搓。保持「能读、能投票」，提议一律走界面。
    // 已实测到的形状（留给将来）：vtable RVA 0x23B7548、基类 vtable 0x2388BA0、
    //   +0x20 发起国、+0x40 动作类型对象（action_vote_for_federation_type）、+0x50 CFederationType*、
    //   +0x58 名字（UI 传的是模块内对象 base+0x234A778）。要做就得把 UI 那份动作**连同 name 逐字节复制**并确认尺寸。
    (void)type_key;
    (void)dry_run;
    return { {"success", false},
             {"error", "不手搓 CVoteForFederationTypeAction：2026-11-21 两次静默崩溃（IsValid 通过后仍崩，name 字段不完整）。换联邦类型请在界面上提议，本工具只负责读取与投票"} };
}

// 只读探针（2026-11-21）：列出 CTradeDeal 数据库里的每个对象——用来找「贸易窗口正在编辑的那一份」
// （界面里摆条款时应该有一份草稿 deal），以及配合差分找「接受度」这类界面字段。
// 库指针：TPdxRef<CTradeDeal>::_pDatabase = 0x311BDD8，与舰队库同一套 TPdxRef 结构（数组 +0x18、容量 +0x20）。
// 只读探针（2026-11-21）：列出 idler 里「看起来像窗口/视图对象」的指针（偏移 + 指针 + vtable RVA）。
// 用法：在「窗口关着 / 打开」两种状态下各取一次，差集里多出来的那个就是新开的窗口。
// 判据故意宽：指针落在可读内存、且对象第一个 qword 是指向本模块 .rdata 的 vtable（RVA 0x2000000..0x2600000）。
nlohmann::json ContactsManager::ProbeWindowsJson() {
    void* idler = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentInGameIdler), &idler) || !idler) {
        return { {"success", false}, {"error", "idler 读不到"} };
    }
    nlohmann::json out = nlohmann::json::array();
    for (uintptr_t off = 0; off < 0x4000; off += 8) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)idler + off), &obj) || !obj || (uintptr_t)obj < 0x10000) continue;
        void* vt = nullptr;
        if (!SafeReadPtr((const void*)obj, &vt) || !vt) continue;
        const uintptr_t rva = (uintptr_t)vt - base_address_;
        if (rva < 0x2000000ULL || rva > 0x2600000ULL) continue;   // vtable 必须落在本模块的只读数据里
        out.push_back({ {"offset", (uint32_t)off}, {"ptr", (unsigned long long)(uintptr_t)obj},
                        {"vtable_rva", (unsigned long long)rva} });
    }
    return { {"success", true}, {"idler", (unsigned long long)(uintptr_t)idler}, {"count", out.size()}, {"objects", out} };
}

nlohmann::json ContactsManager::ProbeTradeDealsJson() {
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CTradeDeal_pDatabase), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) || !arr ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 100000) {
        return { {"success", false}, {"error", "CTradeDeal 库读不到"} };
    }
    nlohmann::json list = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* deal = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &deal) || !deal) continue;
        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)deal + 0x10), &id)) continue;
        auto side = [&](uintptr_t off) {
            uint32_t country = 0xFFFFFFFF, recipient = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)deal + off + 0x8), &country);
            SafeReadU32((const void*)((uintptr_t)deal + off + 0xC), &recipient);
            return nlohmann::json{ {"country", country}, {"recipient", recipient} };
        };
        uint8_t killed = 0;
        SafeCopyChars((char*)&killed, (const char*)((uintptr_t)deal + 0x248), 1);
        list.push_back({ {"index", i}, {"id", id}, {"ptr", (unsigned long long)(uintptr_t)deal},
                         {"first", side(0x18)}, {"second", side(0x130)}, {"killed", (int)killed} });
    }
    return { {"success", true}, {"count", list.size()}, {"deals", list} };
}

nlohmann::json ContactsManager::ProposeTradeDeal(uint32_t country_id, const nlohmann::json& give,
                                                 const nlohmann::json& monthly_give, const nlohmann::json& take,
                                                 const nlohmann::json& monthly_take, int32_t years, bool reserve,
                                                 bool sensor_charts, bool sensor_link, const nlohmann::json& specimens,
                                                 const nlohmann::json& fleets, bool dry_run) {
    void* me = GetPlayerCountry();
    if (!me) return { {"success", false}, {"error", "not in game"} };
    uint32_t my_id = 0xFFFFFFFFu;
    if (!SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string k;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && k == "action_offer_trade_deal") { type = t; break; }
    }
    if (!type) return { {"success", false}, {"error", "动作类型库里没有 action_offer_trade_deal"} };
    bool shown = false;
    void* action = NewAction(type, country_id, &shown);
    if (!action) return { {"success", false}, {"error", "生产者没造出 action_offer_trade_deal"} };

    const uintptr_t deal = (uintptr_t)action + 0x50;
    uint64_t deal_vt = 0;
    SafeReadU64((const void*)deal, &deal_vt);

    // 读一侧的两张表，回「非零格」——dry_run 用它看生产者到底有没有把 26 格分配好
    auto side_state = [&](uint32_t which) {
        const uintptr_t side = deal + (which == 0 ? 0x18 : 0x130);
        uint32_t c = 0xFFFFFFFFu, r = 0xFFFFFFFFu;
        SafeReadU32((const void*)(side + 0x8), &c);
        SafeReadU32((const void*)(side + 0xC), &r);
        nlohmann::json j = { {"country", c}, {"recipient", r} };
        for (int pass = 0; pass < 2; ++pass) {
            const uint32_t map_off = pass == 0 ? 0xC0 : 0xE0;
            const char* name = pass == 0 ? "resources" : "monthly";
            uint64_t map = 0, data = 0, meta = 0;
            if (!SafeReadU64((const void*)(side + map_off), &map) || map < 0x10000ULL) {
                j[name] = { {"ptr", (unsigned long long)map}, {"table", "空"} };
                continue;
            }
            SafeReadU64((const void*)(map + 0x00), &data);
            SafeReadU64((const void*)(map + 0x08), &meta);
            nlohmann::json nz = nlohmann::json::array();
            for (uint32_t i = 0; data && i < 26; ++i) {
                long long v = 0;
                std::memcpy(&v, (const void*)(data + (size_t)i * 8), sizeof(v));
                if (v) nz.push_back({ {"idx", i}, {"raw", v}, {"amount", (double)v / 100000.0} });
            }
            j[name] = { {"ptr", (unsigned long long)map}, {"data", (unsigned long long)data},
                        {"size", (uint32_t)(meta >> 32)}, {"cap", (uint32_t)(meta & 0xFFFFFFFF)},
                        {"non_zero", nz} };
        }
        return j;
    };

    nlohmann::json out = { {"success", true}, {"country_id", country_id}, {"my_id", my_id},
                           {"deal_offset", 0x50},
                           {"deal_vtable_rva", deal_vt ? (unsigned long long)(deal_vt - base_address_) : 0ULL},
                           {"first", side_state(0)}, {"second", side_state(1)} };
    if (dry_run) {
        out["dry_run"] = true;
        out["note"] = "只造了动作并回读，没有写内存、没有投递";
        return out;
    }

    // 写一张 26 格表：src 是 [[资源索引, 数量], …]
    auto fill = [&](uintptr_t side, const nlohmann::json& src, uint32_t map_off, const char* label, std::string& err) -> bool {
        if (!src.is_array() || src.empty()) return true;
        uint64_t map = 0, data = 0, meta = 0;
        if (!SafeReadU64((const void*)(side + map_off), &map) || map < 0x10000ULL) { err = std::string(label) + "：表指针为空"; return false; }
        SafeReadU64((const void*)(map + 0x00), &data);
        SafeReadU64((const void*)(map + 0x08), &meta);
        const uint32_t size = (uint32_t)(meta >> 32);
        if (!data || size != 26) { err = std::string(label) + "：不是 26 格（不像贸易协定那份）"; return false; }
        if (!WritableRange((const void*)data, 26 * 8)) { err = std::string(label) + "：表不在可写内存里——不写"; return false; }
        for (const auto& item : src) {
            if (!item.is_array() || item.size() != 2 || !item[0].is_number() || !item[1].is_number()) continue;
            const int idx = item[0].get<int>();
            if (idx < 0 || idx >= 26) { err = std::string(label) + "：资源索引越界"; return false; }
            const long long v = (long long)llround(item[1].get<double>() * 100000.0);
            std::memcpy((void*)(data + (size_t)idx * 8), &v, sizeof(v));
        }
        return true;
    };
    std::string err;
    if (!fill(deal + 0x18, give, 0xC0, "我方一次性", err)) return { {"success", false}, {"error", err} };
    if (!fill(deal + 0x18, monthly_give, 0xE0, "我方月供", err)) return { {"success", false}, {"error", err} };
    if (!fill(deal + 0x130, take, 0xC0, "对方一次性", err)) return { {"success", false}, {"error", err} };
    if (!fill(deal + 0x130, monthly_take, 0xE0, "对方月供", err)) return { {"success", false}, {"error", err} };

    // length 是「年」（实测 +0x249 读到 10 = 10 年）；reserve 给「给出资源的那一侧」
    if (WritableRange((const void*)(deal + 0x249), 4)) {
        *(int32_t*)(deal + 0x249) = years > 0 ? years : 10;
    }
    // reserve 只在「发起方」那一侧（实测界面递交：first.reserve=yes、second 没有这一行）；
    // 含义是「给出的资源先扣住、被拒再退回」，两侧都给东西时也只有发起方那一侧置位。
    if (WritableRange((const void*)(deal + 0x18 + 0x106), 1)) *(uint8_t*)(deal + 0x18 + 0x106) = reserve ? 1 : 0;
    if (WritableRange((const void*)(deal + 0x130 + 0x106), 1)) *(uint8_t*)(deal + 0x130 + 0x106) = 0;
    // 通信同步 / 主动传感链接：发起方那侧 +0x104 / +0x105 的两个字节（2026-11-21 三份样本对位确认）
    if (WritableRange((const void*)(deal + 0x18 + 0x104), 2)) {
        *(uint8_t*)(deal + 0x18 + 0x104) = sensor_charts ? 1 : 0;
        *(uint8_t*)(deal + 0x18 + 0x105) = sensor_link ? 1 : 0;
    }
    // 标本：发起方那侧 +0x60 是 {指针数组, cap, size}，元素是 CSpecimen*（vtable RVA 0x2382708）。
    // 三元素样本实测（2026-11-21）：数组 8 字节一个、顺序与界面一致；键在标本对象 +0x20、库内下标 +0x08。
    // **数组用 EngineAlloc 分配——与容器析构分配器是否一致尚未验证**，所以这条算实验性路径。
    out["specimens_asked"] = nlohmann::json::array();
    if (specimens.is_array() && !specimens.empty()) {
        std::vector<void*> objs;
        for (const auto& kv : specimens) {
            if (!kv.is_string()) continue;
            const std::string key = kv.get<std::string>();
            void* found = nullptr;
            for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CSpecimenDatabase_pInstance)) {
                std::string k;
                if (SafeReadPdxString((const void*)((uintptr_t)t + 0x20), k) && k == key) { found = t; break; }
            }
            if (!found) {
                out["success"] = false;
                out["error"] = "未知标本键 " + key + "（见 common/specimens/specimens.txt）";
                return out;
            }
            objs.push_back(found);
            out["specimens_asked"].push_back(key);
        }
        if (!objs.empty()) {
            void* arr = CommandBuilder::Get().EngineAlloc((uint32_t)(objs.size() * 8));
            if (!arr) return { {"success", false}, {"error", "EngineAlloc 失败（标本数组）"} };
            for (size_t i = 0; i < objs.size(); ++i) *(void**)((uintptr_t)arr + i * 8) = objs[i];
            const uintptr_t side = deal + 0x18;
            if (!WritableRange((const void*)(side + 0x60), 16)) {
                return { {"success", false}, {"error", "标本容器不在可写内存里——不写"} };
            }
            *(void**)(side + 0x60) = arr;
            *(uint32_t*)(side + 0x68) = (uint32_t)objs.size();   // cap
            *(uint32_t*)(side + 0x6C) = (uint32_t)objs.size();   // size
            out["specimens_written"] = objs.size();
        }
    }
    // 借出舰队：发起方那侧 +0x40 是容器（vtable 0x23045F0），+0x48 数据指针、+0x50 = {cap, size}；
    // 元素是 **4 字节的 fleet id**（不是指针，与标本不同）——2026-11-21 用「破浪者/飓风」样本对位确认。
    // 舰队协议是**有期限**的（length 字段就是年限），不是即时交割。
    out["fleets_asked"] = nlohmann::json::array();
    if (fleets.is_array() && !fleets.empty()) {
        std::vector<uint32_t> ids;
        for (const auto& fv : fleets) {
            if (!fv.is_number()) continue;
            ids.push_back((uint32_t)fv.get<uint64_t>());
            out["fleets_asked"].push_back((uint32_t)fv.get<uint64_t>());
        }
        if (!ids.empty()) {
            void* arr = CommandBuilder::Get().EngineAlloc((uint32_t)(ids.size() * 4));
            if (!arr) return { {"success", false}, {"error", "EngineAlloc 失败（舰队数组）"} };
            for (size_t i = 0; i < ids.size(); ++i) *(uint32_t*)((uintptr_t)arr + i * 4) = ids[i];
            const uintptr_t side = deal + 0x18;
            if (!WritableRange((const void*)(side + 0x48), 16)) {
                return { {"success", false}, {"error", "舰队容器不在可写内存里——不写"} };
            }
            *(void**)(side + 0x48) = arr;
            *(uint32_t*)(side + 0x50) = (uint32_t)ids.size();   // cap
            *(uint32_t*)(side + 0x54) = (uint32_t)ids.size();   // size
            out["fleets_written"] = ids.size();
        }
    }

    out["after_write"] = { {"first", side_state(0)}, {"second", side_state(1)} };

    namespace pr = sdk::cmd::diplo_action_propose_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    std::string reason;
    if (!cmd.IsValid(&reason)) {
        out["success"] = false;
        out["posted"] = false;
        out["error"] = AlertManager::Get().CleanPdxString(reason);
        out["note"] = "动作归命令所有，随命令析构释放（不要手动删）";
        return out;
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"posted", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())} };
    }
    LOGF("[CONTACTS] proposed trade deal with country %u (years %d)", country_id, years);
    out["posted"] = true;
    out["message"] = "已递交贸易协定；对方答复前给出的一次性资源已被扣住（被拒会退回）";
    return out;
}

// 只读探针（2026-11-20）：is_listed = no 的动作（求和就是）永远不会出现在外交动作表里，
// 因为 NewAction 会拿 IsPotential(action, false) 当"上不上界面"的筛。这里把造与筛拆开：
// 先只造，再分别问 IsPotential(true) / IsPotential(false)，顺便把对象头 0x80 字节吐出来。
// 造完立刻删除，不投递。
nlohmann::json ContactsManager::ProbeDiplomaticAction(uint32_t country_id, const std::string& action_key) {
    void* me = GetPlayerCountry();
    if (!me) return { {"success", false}, {"error", "No player country"} };
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == action_key) { type = t; break; }
    }
    if (!type) return { {"success", false}, {"created", false}, {"error", "no such action type: " + action_key} };

    void* cdb = nullptr;
    void* slots = nullptr;
    void* recipient = nullptr;
    uint32_t cap = 0, check = 0xFFFFFFFF;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &recipient) || !recipient ||
        !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) || (check & 0xFFFFFFu) != (country_id & 0xFFFFFFu)) {
        return { {"success", false}, {"created", false},
                 {"error", "no country with bare id " + std::to_string(country_id)} };
    }

    CreateOnlyCtx ctx{ base_address_ + sdk::fn::NDiplomacyUtil_CreateDiplomaticAction, type, me, recipient, nullptr };
    void* type_vt = nullptr;
    SafeReadPtr(type, &type_vt);
    const unsigned long long type_addr = (unsigned long long)(uintptr_t)type;
    const unsigned long long type_vt_rva =
        type_vt ? (unsigned long long)((uintptr_t)type_vt - base_address_) : 0ULL;
    if (!CommandBuilder::Get().CallGuarded(&CallCreateOnly, &ctx)) {
        return { {"success", false}, {"created", false}, {"error", "CreateDiplomaticAction faulted"},
                 {"type_ptr", type_addr}, {"type_vtable_rva", type_vt_rva} };
    }
    if (!ctx.action) {
        return { {"success", true}, {"created", false}, {"action", action_key},
                 {"note", "the game's producer returned null for this type"},
                 {"type_ptr", type_addr}, {"type_vtable_rva", type_vt_rva} };
    }
    void* action = ctx.action;
    void* vt = nullptr;
    SafeReadPtr(action, &vt);
    const uintptr_t vt_rva = vt ? (uintptr_t)vt - base_address_ : 0;

    int pot_a_list = -1, pot_listed = -1;
    {
        PotentialCtx pf{ action, 0, 0, 0 };
        if (CommandBuilder::Get().CallGuarded(&CallIsPotential, &pf) && pf.ok) pot_a_list = pf.out;
    }
    {
        PotentialCtx pt{ action, 1, 0, 0 };
        if (CommandBuilder::Get().CallGuarded(&CallIsPotential, &pt) && pt.ok) pot_listed = pt.out;
    }

    static const char* kHexDigits = "0123456789ABCDEF";
    std::string hex;
    for (int i = 0; i < 0x80; ++i) {
        uint8_t b = 0;
        if (!SafeReadU8((const void*)((uintptr_t)action + (uintptr_t)i), &b)) break;
        hex += kHexDigits[b >> 4];
        hex += kHexDigits[b & 0xF];
        hex += ' ';
    }
    CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
    LOGF("[CONTACTS] probe action %s: created at 0x%llX vt 0x%llX", action_key.c_str(),
         (unsigned long long)(uintptr_t)action, (unsigned long long)vt_rva);
    return { {"success", true}, {"created", true}, {"action", action_key},
             {"vtable_rva", vt_rva},
             {"is_potential_hidden", pot_a_list}, {"is_potential_listed", pot_listed},
             {"head_hex", hex} };
}

void* ContactsManager::GetPlayerCountry() {
    return GameState::Get().GetPlayerCountry();  // the local player's country (not country 0)
}

std::string ContactsManager::LocalizeKey(const std::string& key) {
    return SafeLocalize(base_address_, key);
}

nlohmann::json ContactsManager::SetDefenderWarGoal(uint32_t war_id, const std::string& goal_key, bool dry_run, bool swap) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* country = GetPlayerCountry();
    if (!country) return { {"success", false}, {"error", "No player country"} };
    uint32_t my_id = kNoId;
    if (!SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &my_id) || my_id == kNoId) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    // CWar 库查对象：按 id 逐条比对（数组下标不是 id —— id 是带标记的引用，如 0x03000003）
    void* war = nullptr;
    {
        void* db = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CWar), &db) && db &&
            SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) && cap < 1000000) {
            for (uint32_t i = 0; i < cap; ++i) {
                void* obj = nullptr;
                uint32_t id = kNoId;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &obj) || !obj) continue;
                if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CWar_id), &id) || id != war_id) continue;
                war = obj;
                break;
            }
        }
    }
    if (!war) return { {"success", false}, {"error", "war not found: " + std::to_string(war_id)} };
    const uintptr_t w = (uintptr_t)war;

    void* atk_type = nullptr;
    uint32_t war_ref = kNoId, atk_actor = kNoId, atk_target = kNoId;
    SafeReadPtr((const void*)(w + sdk::ent::CWar::attacker_war_goal + sdk::ent::CWarGoal::type), &atk_type);
    SafeReadU32((const void*)(w + sdk::ent::CWar::attacker_war_goal + sdk::ent::CWarGoal::war), &war_ref);
    SafeReadU32((const void*)(w + sdk::ent::CWar::attacker_war_goal + sdk::ent::CWarGoal::actor), &atk_actor);
    SafeReadU32((const void*)(w + sdk::ent::CWar::attacker_war_goal + sdk::ent::CWarGoal::target), &atk_target);

    void* goal_type = atk_type;
    if (!goal_key.empty()) {
        goal_type = nullptr;
        for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CWarGoalTypeDatabase_pInstance)) {
            std::string k;
            if (SafeReadPdxString((const void*)((uintptr_t)t + 0x20), k) && k == goal_key) { goal_type = t; break; }
        }
        if (!goal_type) {
            return { {"success", false}, {"error", "unknown war goal type " + goal_key} };
        }
    }
    if (!goal_type) return { {"success", false}, {"error", "no war goal type on this war and none given"} };
    if (war_ref == kNoId) return { {"success", false}, {"error", "attacker war goal carries no war reference"} };

    const uint32_t my_ref = my_id | 0x01000000u;
    if (atk_actor == kNoId) return { {"success", false}, {"error", "attacker war goal carries no actor"} };

    namespace c = sdk::cmd::diplo_action_set_defender_war_goal_command;
    auto cmd = CommandBuilder::Get().Create(c::kSpec);
    // actor/target 的方向有两种可能：①我方=actor、对方=target；②反过来。
    // 引擎拒「只有防御方盟主才能设置战争目标」时，用 swap 试反方向（2026-11-20 玩家提的）。
    const uint32_t goal_actor = swap ? atk_actor : my_ref;
    const uint32_t goal_target = swap ? my_ref : atk_actor;

    // 别从零拼这个 goal：CWarGoal 的头 8 字节是个**类级静态指针**，四份 goal（两场战争的守方与攻方）
    // 读出来完全一样，且落在 exe 映像内（2026-11-20 实测：0x7FF7989A4130，按 base 0x7FF798000000 算
    // RVA 0x9A4130）。以前从 0 开始写，这 8 字节就是 0，引擎读到的结构不完整。
    // 改成以「本场战争自己的守方 goal 槽」为模板抄一遍再覆盖——空槽也是完整结构，头 8 字节本来就在。
    uint8_t goal[0x20] = {0};
    for (int i = 0; i < 4; ++i) {
        uint64_t v = 0;
        if (!SafeReadU64((const void*)(w + sdk::ent::CWar::defender_war_goal + (uintptr_t)(i * 8)), &v)) {
            return { {"success", false}, {"error", "cannot read the defender goal slot as a template"} };
        }
        std::memcpy(goal + i * 8, &v, sizeof(v));
    }
    const uint64_t template_head = *(uint64_t*)goal;

    *(void**)(goal + sdk::ent::CWarGoal::type) = goal_type;
    *(uint32_t*)(goal + sdk::ent::CWarGoal::war) = war_ref;
    *(uint32_t*)(goal + sdk::ent::CWarGoal::actor) = goal_actor;
    *(uint32_t*)(goal + sdk::ent::CWarGoal::target) = goal_target;
    *(uint8_t*)(goal + sdk::ent::CWarGoal::win) = 0;
    *(uint8_t*)(goal + sdk::ent::CWarGoal::accomplished) = 0;
    // country 字段的编码有两种可能：带标记的 0x01xxxxxx，或裸 id。
    // 证据：CWarGoal::actor 里存的是 0x01000009（带标记），而参战方数组里 country 读出来是裸 54
    // ——同一个对象图里两种编码并存，所以这一条不能靠推。dry_run 时两个都跑，一次拿两条判决。
    auto setup = [&](uint32_t cref) {
        cmd.Set<uint32_t>(c::country, cref)
           .Set<uint32_t>(c::war, war_ref)
           .SetBytes(c::defender_war_goal, goal, sizeof(goal));
    };
    // 2026-11-20 实测定案：country 要**裸 id**。带标记的 0x0100_0036 会被引擎判「你不是防御方盟主」
    // （那条提示其实是 IsValid 失败时的固定要求文本，真正的分歧就在这里）；裸 54 直接通过。
    setup(my_id);
    std::string why;
    const bool ok = cmd.IsValid(&why);
    std::string why_tagged;
    bool ok_tagged = false;
    if (dry_run) {
        setup(my_ref);
        ok_tagged = cmd.IsValid(&why_tagged);
        setup(my_id);
    }
    if (dry_run || !ok) {
        nlohmann::json r = { {"success", ok}, {"dry_run", dry_run}, {"war_id", war_id},
                             {"war_ref", war_ref}, {"war_goal_type", goal_key.empty() ? "(copied from attacker goal)" : goal_key},
                             {"defender_actor", my_ref}, {"defender_target", atk_actor},
                             {"goal_template_head", template_head},
                             {"goal_win_field", (int)*(uint8_t*)(goal + sdk::ent::CWarGoal::win)} };
        if (dry_run) {
            r["variants"] = nlohmann::json::array({
                { {"country_field", "bare"}, {"country_ref", my_id}, {"ok", ok},
                  {"reason", AlertManager::Get().CleanPdxString(why)} },
                { {"country_field", "tagged"}, {"country_ref", my_ref}, {"ok", ok_tagged},
                  {"reason", AlertManager::Get().CleanPdxString(why_tagged)} }
            });
        }
        if (!ok) r["error"] = AlertManager::Get().CleanPdxString(why);
        return r;
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())} };
    }
    return { {"success", true}, {"war_id", war_id}, {"war_ref", war_ref}, {"message", "defender war goal posted"} };
}

// 程序化递交求和提案（2026-11-20 打通尝试）。思路来自两次界面递交的捕获对比：
// status_quo 与 force_demands 两份 COfferPeaceAction 模板只有 +0x64 不同，其余 103 字节全同
// （含 +0x40 那个两次读档都一样的单例指针）。所以：抄模板 → 改 4 个字段 → 交给引擎自己的
// 「模板→新动作」函数 → 包进 CDiploActionProposeCommand，先只跑 IsValid。
nlohmann::json ContactsManager::TryPeaceOffer(uint32_t war_id, uint32_t target_id, int status, bool dry_run,
                                              bool synthetic, uint64_t f40, uint32_t f4c, bool force) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* me = GetPlayerCountry();
    if (!me) return { {"success", false}, {"error", "No player country"} };
    uint32_t my_id = kNoId;
    if (!SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == kNoId) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }

    // 战争：按 id 在 CWar 库里逐条比对（id 本身就是带标记的引用）
    void* war = nullptr;
    uint32_t war_ref = kNoId;
    {
        void* db = nullptr;
        void* arr = nullptr;
        uint32_t cap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CWar), &db) && db &&
            SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
            SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) && cap < 1000000) {
            for (uint32_t i = 0; i < cap; ++i) {
                void* obj = nullptr;
                uint32_t id = kNoId;
                if (!SafeReadPtr((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &obj) || !obj) continue;
                if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CWar_id), &id) || id != war_id) continue;
                war = obj;
                war_ref = id;
                break;
            }
        }
    }
    if (!war) return { {"success", false}, {"error", "war not found: " + std::to_string(war_id)} };

    // 目标国家：查出来顺手把**内存里存的 id 原值**留下（裸或带标记都可能），构造时直接用它
    uint32_t target_ref_raw = kNoId;
    {
        void* cdb = nullptr;
        void* slots = nullptr;
        void* recipient = nullptr;
        uint32_t cap = 0, check = kNoId;
        if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
            !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
            !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (target_id & 0xFFFFFF) >= cap ||
            !SafeReadPtr((const void*)((uintptr_t)slots + (target_id & 0xFFFFFF) * 16 + 8), &recipient) || !recipient ||
            !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) || (check & 0xFFFFFFu) != (target_id & 0xFFFFFFu)) {
            return { {"success", false}, {"error", "no country with id " + std::to_string(target_id)} };
        }
        target_ref_raw = check;
    }

    if (!synthetic && !CommandBuilder::Get().HasPeaceTemplate()) {
        return { {"success", false},
                 {"error", "本会话还没有捕获到模板：先在战争窗口界面递交一次求和提案（Post 旁听会记下模板），再来"},
                 {"hint", "synth:true 走无模板路径（更推荐，2026-11-20 已解）"} };
    }

    // synth 且没显式指定 +0x40 时，自己解析 action_offer_peace 的类型对象 ——
    // 2026-11-20 实测：模板里的 +0x40 就是它（type_ptr == 0x24736D2B960），可以按 key 现取。
    if (synthetic && f40 == 0) {
        for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
            std::string key;
            if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == "action_offer_peace") {
                f40 = (uint64_t)(uintptr_t)t;
                break;
            }
        }
        if (f40 == 0) {
            return { {"success", false}, {"error", "找不到 action_offer_peace 这个动作类型对象（类型库枚举失败）"} };
        }
    }

    void* action = CommandBuilder::Get().BuildPeaceAction(my_id, target_ref_raw, war_ref, status, synthetic, f40, f4c);
    if (!action) {
        return { {"success", false}, {"error", "build action failed（克隆函数没装，或调用时异常，见桥日志）"} };
    }

    namespace pr = sdk::cmd::diplo_action_propose_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", std::string("create propose command failed: ") + cmd.error()} };
    }
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    std::string why;
    const bool ok = cmd.IsValid(&why);
    nlohmann::json r = { {"success", ok}, {"dry_run", dry_run},
                         {"war_id", war_id}, {"war_ref", war_ref},
                         {"actor", my_id}, {"recipient", target_id},
                         {"status", status},
                         {"action_ptr", (unsigned long long)(uintptr_t)action},
                         {"is_valid", ok} };
    if (!ok && !force) {
        r["reason"] = AlertManager::Get().CleanPdxString(why);
        return r;
    }
    if (dry_run) {
        r["note"] = ok ? "IsValid 通过，没有投递（dry_run）" : "IsValid 未通过（dry_run，没投）";
        return r;
    }
    // force 时跳过我们自己的 IsValid，只留 PostCommand 自己的门 —— 界面投递就是不加这道校验的
    const bool posted = force ? cmd.Post(NativeCommand::Check::EngineGate) : cmd.Post();
    if (!posted) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())},
                 {"is_valid", ok}, {"forced", force} };
    }
    LOGF("[CONTACTS] peace offer posted: war 0x%X actor %u recipient %u status %d",
         war_ref, my_id, target_id, status);
    return { {"success", true}, {"posted", true},
             {"war_id", war_id}, {"actor", my_id}, {"recipient", target_id}, {"status", status} };
}

// 只读探针：CCountry 对象的一段内存，按 qword 吐。
// 目的：定位 incoming_actions —— 待回应的外交动作在存档里挂在**收件方**国家身上
// （action_offer_peace / peace_offer{status,actor,recipient,war} / id / waiting_for_vote / timeout），
// 所以「谁收到了提议」要去那个国家的对象里找，不是发起方。
nlohmann::json ContactsManager::ProbeCountryJson(uint32_t country_id, uint32_t offset, uint32_t length) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* db = nullptr;
    void* slots = nullptr;
    void* country = nullptr;
    uint32_t cap = 0, check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &country) || !country ||
        !SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &check) || (check & 0xFFFFFFu) != (country_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(country_id)} };
    }
    if (offset > 0x2000) offset = 0;
    length = length == 0 ? 0x200u : length;
    if (length > 0x2000) length = 0x2000;
    nlohmann::json words = nlohmann::json::array();
    for (uint32_t o = offset; o < offset + length; o += 8) {
        uint64_t v = 0;
        if (!SafeReadU64((const void*)((uintptr_t)country + o), &v)) break;
        words.push_back({ {"offset", o}, {"value", (int64_t)v},
                          {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
    }
    return { {"success", true}, {"country_id", country_id},
             {"ptr", (unsigned long long)(uintptr_t)country}, {"words", words} };
}

// ———— 法律库 / 动作类型库探针（2026-11-21 只读）————
// 目的：确认能不能自己造一条「联邦法律表决」——需要两样东西
//   ① CDiplomaticActionTypeDatabase 里有没有 action_vote_for_federation_law
//   ② CFederationLawDatabase 里按 key 取法律对象（动作 +0x50 要的就是它）
// 只读：枚举 + 读键，不造对象、不写内存。
nlohmann::json ContactsManager::ProbeLawDbJson() {
    nlohmann::json out = { {"success", true} };
    auto dump_db = [&](uintptr_t glob, const char* name, bool only_law) {
        nlohmann::json rows = nlohmann::json::array();
        for (void* t : ScriptTypes(base_address_ + glob)) {
            char kb[96] = {0}, kb28[96] = {0};
            void* kp = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)t + 0x20), &kp) && kp) SafeReadCStr(kp, kb, sizeof kb);
            const std::string key(kb);
            if (only_law && key.find("law") == std::string::npos) continue;
            void* kp28 = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)t + 0x28), &kp28) && kp28) SafeReadCStr(kp28, kb28, sizeof kb28);
            void* vt = nullptr;
            SafeReadPtr(t, &vt);
            rows.push_back({ {"ptr", (unsigned long long)(uintptr_t)t},
                             {"vt_rva", vt ? (unsigned long long)((uintptr_t)vt - base_address_) : 0ULL},
                             {"key", key},
                             {"key28", std::string(kb28)} });
        }
        out[name] = rows;
        out[std::string(name) + "_count"] = rows.size();
    };
    dump_db(sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance, "action_types", true);
    dump_db(sdk::glob::TGameDatabase_CFederationLawDatabase_pInstance, "laws", false);
    dump_db(sdk::glob::TGameDatabase_CFederationLawCategoryDatabase_pInstance, "law_categories", false);
    return out;
}

// ———— 联邦当前法律探针（2026-11-21 只读）————
// 法律对象长这样：vtable RVA 0x2388C28（实测 succession_term_years_40 那个），键字符串指针在对象 +0x20。
// 这里扫联邦对象与 progression 的 qword，凡指向这种对象的都记下来。只读，不写、不造对象。
nlohmann::json ContactsManager::ProbeFederationLawsJson(uint32_t fed_id) {
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFederation_pDatabase), &db) || !db) {
        return { {"success", false}, {"error", "联邦对象表读不出来"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || fed_id >= cap) {
        return { {"success", false}, {"error", "fed_id 越界或表读不出"} };
    }
    void* fed = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)fed_id * 16 + 8), &fed) || !fed) {
        return { {"success", false}, {"error", "该槽位没有联邦对象"} };
    }
    constexpr unsigned long long kLawVtRva = 0x2388C28ULL;
    nlohmann::json rows = nlohmann::json::array();
    auto scan = [&](const void* base, uint32_t limit, const char* where) {
        for (uint32_t off = 0; off + 8 <= limit; off += 8) {
            uint64_t v = 0;
            if (!SafeReadU64((const void*)((uintptr_t)base + off), &v)) break;
            if (v < 0x10000ull || v > 0x7FFF00000000ull || (v & 7)) continue;
            void* obj = nullptr;
            if (!SafeReadPtr((const void*)(uintptr_t)v, &obj) || !obj) continue;
            void* vt = nullptr;
            if (!SafeReadPtr(obj, &vt) || !vt) continue;
            if ((unsigned long long)((uintptr_t)vt - base_address_) != kLawVtRva) continue;
            void* keyp = nullptr;
            char kb[64] = {0};
            if (SafeReadPtr((const void*)((uintptr_t)obj + 0x20), &keyp) && keyp) SafeReadCStr(keyp, kb, sizeof kb);
            rows.push_back({ {"where", std::string(where)}, {"offset", off},
                             {"obj", (unsigned long long)(uintptr_t)obj}, {"key", std::string(kb)} });
        }
    };
    scan(fed, 0x400, "federation");
    void* prog = nullptr;
    nlohmann::json out = { {"success", true}, {"fed_id", fed_id}, {"count", rows.size()}, {"items", rows},
                           {"law_vt_rva", (unsigned long long)kLawVtRva} };
    if (SafeReadPtr((const void*)((uintptr_t)fed + 0x80), &prog) && prog) {
        scan(prog, 0x600, "progression");
        out["count"] = rows.size();
        out["items"] = rows;
        // 「待表决槽」与前后原始 qword：2026-11-21 实测待表决的法律对象挂在 progression+0x2D0。
        // 投票前后各调一次，就能看出票通过那一瞬这个槽怎么变（是不是换成"当前值"）。
        uint64_t slot = 0;
        SafeReadU64((const void*)((uintptr_t)prog + 0x2D0), &slot);
        out["slot_2d0"] = (int64_t)slot;
        nlohmann::json words = nlohmann::json::array();
        for (uint32_t o = 0x2B0; o < 0x300; o += 8) {
            uint64_t v = 0;
            if (!SafeReadU64((const void*)((uintptr_t)prog + o), &v)) break;
            words.push_back({ {"offset", o}, {"value", (int64_t)v},
                              {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
        }
        out["prog_2b0_300"] = words;
        uint64_t cooldowns = 0;
        SafeReadU64((const void*)((uintptr_t)fed + 0xF0), &cooldowns);
        out["law_cooldowns_ptr"] = (unsigned long long)cooldowns;
    }
    return out;
}

// ———— 当前法律（2026-11-21 打通）————
// 布局（两个联邦交叉验证，各 13 条，与存档逐条相同）：
//   CFederation +0x80 → progression；progression +0x2E8 → 容器指针
//   容器：0x10 字节表头，之后每 0x18 一槽；槽 +0x00 = CFederationLaw*，空槽全 0（槽位不连续！）
//   CFederationLaw +0x08 = 本库下标（centralization_high = 3），+0x20 = 键字符串
// 之前两次「内存里读不到」的原因是只按「qword 直接指向 law 对象」扫：
// 容器头那个 qword 不是 law 的 vtable，所以必须再解一层。类别归属不在这里——在插件层用
// common/federation_laws 的脚本表补（值 → 类别）。
nlohmann::json ContactsManager::GetFederationLawsJson(uint32_t fed_id) {
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFederation_pDatabase), &db) || !db) {
        return { {"success", false}, {"error", "联邦对象表读不出来"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || fed_id >= cap) {
        return { {"success", false}, {"error", "fed_id 越界或表读不出"} };
    }
    void* fed = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)fed_id * 16 + 8), &fed) || !fed) {
        return { {"success", false}, {"error", "该槽位没有联邦对象"} };
    }
    void* prog = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)fed + 0x80), &prog) || !prog) {
        return { {"success", false}, {"error", "progression 读不出"} };
    }
    constexpr unsigned long long kLawVtRva = 0x2388C28ULL;
    uint64_t cont = 0;
    if (!SafeReadU64((const void*)((uintptr_t)prog + 0x2E8), &cont) || !cont) {
        return { {"success", false}, {"error", "progression+0x2E8 为空（这版没有当前法律容器？）"} };
    }

    // 一个槽 / 一个待表决项：验 vtable，再读下标与键
    auto read_law = [&](uint64_t p, nlohmann::json& row) -> bool {
        void* vt = nullptr;
        if (!SafeReadPtr((const void*)(uintptr_t)p, &vt) || !vt) return false;
        if ((unsigned long long)((uintptr_t)vt - base_address_) != kLawVtRva) return false;
        uint32_t idx = 0xFFFFFFFFu;
        SafeReadU32((const void*)((uintptr_t)p + 0x08), &idx);
        void* keyp = nullptr;
        char kb[64] = {0};
        if (SafeReadPtr((const void*)((uintptr_t)p + 0x20), &keyp) && keyp) SafeReadCStr(keyp, kb, sizeof kb);
        if (!kb[0]) return false;
        row = { {"key", std::string(kb)},
                {"law_index", idx == 0xFFFFFFFFu ? -1 : (int32_t)idx},
                {"ptr", (unsigned long long)p} };
        return true;
    };

    nlohmann::json laws = nlohmann::json::array();
    uint32_t empty_run = 0;
    for (uint32_t off = 0x10; off + 0x18 <= 0x300; off += 0x18) {
        uint64_t p = 0;
        if (!SafeReadU64((const void*)((uintptr_t)cont + off), &p)) break;
        if (!p) {                       // 空槽：连着 4 个就停，别把隔壁对象读进来
            if (++empty_run >= 4) break;
            continue;
        }
        nlohmann::json row;
        if (read_law(p, row)) {
            row["offset"] = off;
            laws.push_back(row);
            empty_run = 0;
        }
    }

    // 待表决 / 最近一次表决：progression +0x2D0 → 两个 law 指针（+0x00 / +0x10）
    nlohmann::json pending = nlohmann::json::array();
    uint64_t pend_obj = 0;
    if (SafeReadU64((const void*)((uintptr_t)prog + 0x2D0), &pend_obj) && pend_obj) {
        for (uint32_t off : {0u, 0x10u}) {
            uint64_t p = 0;
            if (!SafeReadU64((const void*)((uintptr_t)pend_obj + off), &p) || !p) continue;
            nlohmann::json row;
            if (read_law(p, row)) { row["offset"] = off; pending.push_back(row); }
        }
    }

    return { {"success", true}, {"fed_id", fed_id}, {"count", laws.size()}, {"laws", laws},
             {"pending", pending}, {"container", (unsigned long long)cont},
             {"law_vt_rva", (unsigned long long)kLawVtRva} };
}

// ———— 全银河联邦列表（2026-11-21）————
// 只读：TPdxRef<CFederation>::_pDatabase → {cap +0x20, slots +0x18}。
// 2026-11-21 修正：**不能只扫到 "size"**（+0x24 在本档读 1，可档里有 2 个联邦）——
// 直接按 cap 扫整张表、跳过空槽。成员表在 +0x90（数据指针），大小/容量在 +0xA0 的低/高 32 位。
// 逐槽读：id(+0x08)、名字指针(+0x28)、成员数(+0xA0 低 32)、progression(+0x80) →
// 类型(+0x1A0)、等级(+0x08)、累计经验(+0x1A8)、本级经验(+0x1B0)、团结度(+0x1C0)。
// 经验/团结度都是 ×1e5 的定点数（2026-11-21 用 UI 的 1313 / 89 对过账）。
nlohmann::json ContactsManager::GetFederationsJson() {
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFederation_pDatabase), &db) || !db) {
        return { {"success", false}, {"error", "TPdxRef<CFederation>::_pDatabase 读不出来"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap > 1000000u) {
        return { {"success", false}, {"error", "联邦对象表的 slots/cap 读不出来"} };
    }
    nlohmann::json items = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* fed = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &fed) || !fed) continue;
        uint32_t id = 0xFFFFFFFFu, member_size = 0;
        SafeReadU32((const void*)((uintptr_t)fed + 0x08), &id);
        char name[96] = {0};
        void* name_ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)fed + 0x28), &name_ptr) && name_ptr) {
            // +0x28 可能直接是字符串，也可能是 persistent string 结构（里面再指一次）
            if (!SafeReadCStr(name_ptr, name, sizeof name) || !name[0]) {
                void* inner = nullptr;
                if (SafeReadPtr((const void*)name_ptr, &inner) && inner) SafeReadCStr(inner, name, sizeof name);
            }
        }
        uint32_t ms = 0;
        SafeReadU32((const void*)((uintptr_t)fed + 0xA0), &ms);   // members 的 {size,cap} 里的 size
        member_size = ms;
        nlohmann::json row = { {"index", i}, {"id", id}, {"name", std::string(name)},
                               {"members", member_size} };
        // 成员名单还没解出来：+0x90 指到 .rdata 的指针表、不是国家引用数组（2026-11-21 试过，值不对）。
        // 现在只有人数（+0xA0 低 32 位，实测与该联邦界面的人数一致）。
        void* prog = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)fed + 0x80), &prog) && prog) {
            uint32_t level = 0;
            SafeReadU32((const void*)((uintptr_t)prog + 0x08), &level);
            void* type = nullptr;
            SafeReadPtr((const void*)((uintptr_t)prog + 0x1A0), &type);
            std::string type_key;
            if (type) {
                void* key_ptr = nullptr;
                char kb[64] = {0};
                if (SafeReadPtr((const void*)((uintptr_t)type + 0x20), &key_ptr) && key_ptr) SafeReadCStr(key_ptr, kb, sizeof kb);
                type_key = kb;
            }
            uint64_t exp_total = 0, exp_level = 0, cohesion = 0;
            SafeReadU64((const void*)((uintptr_t)prog + 0x1A8), &exp_total);
            SafeReadU64((const void*)((uintptr_t)prog + 0x1B0), &exp_level);
            SafeReadU64((const void*)((uintptr_t)prog + 0x1C0), &cohesion);
            row["type"] = type_key;
            row["level"] = level;
            row["experience_total"] = (double)(int64_t)exp_total / 100000.0;
            row["experience_level"] = (double)(int64_t)exp_level / 100000.0;
            row["cohesion"] = (double)(int64_t)cohesion / 100000.0;
        }
        items.push_back(row);
    }
    // 成员：反过来标——扫一遍国家表，读每个国家的 CCountry+0x1C38（低 32 位 = 联邦 id，
    // 0xFFFFFFFF = 不在联邦），按 id 归到各联邦下面。2026-11-21 玩家提的办法，比解 +0x90 那个数组稳。
    {
        void* cdb = nullptr;
        void* cslots = nullptr;
        uint32_t ccap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) && cdb &&
            SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &cslots) && cslots &&
            SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &ccap) && ccap < 100000u) {
            for (uint32_t c = 0; c < ccap; ++c) {
                void* country = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)cslots + (size_t)c * 16 + 8), &country) || !country) continue;
                uint32_t cid = 0xFFFFFFFFu;
                uint64_t ref = 0;
                if (!SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &cid)) continue;
                if (!SafeReadU64((const void*)((uintptr_t)country + 0x1C38), &ref)) continue;
                const uint32_t fed_low = (uint32_t)(ref & 0xFFFFFFFFull);
                if (fed_low == 0xFFFFFFFFu) continue;   // 不在联邦
                for (auto& row : items) {
                    if (row.contains("id") && row["id"].get<uint32_t>() == fed_low) {
                        if (!row.contains("member_ids")) row["member_ids"] = nlohmann::json::array();
                        row["member_ids"].push_back(cid & 0xFFFFFFu);
                        break;
                    }
                }
            }
        }
    }
    return { {"success", true}, {"count", items.size()}, {"items", items} };
}

// 只读探针：国家是不是星海共同体成员——**问引擎，不读共同体对象**（实例指针还没有）。
// 做法：对每个 id 造 leave_community_command / join_community_command，只跑 IsValid、不投递。
// 判据的方向还要实测确认（成员应当 leave ✓ / join ✗），所以两种编码都回，原样带上引擎理由。
nlohmann::json ContactsManager::ProbeCommunityMembershipJson(const std::vector<uint32_t>& ids) {
    namespace lc = sdk::cmd::leave_community_command;
    namespace jc = sdk::cmd::join_community_command;
    nlohmann::json rows = nlohmann::json::array();
    for (uint32_t id : ids) {
        nlohmann::json row = { {"country_id", id} };
        for (int enc = 0; enc < 2; ++enc) {
            const uint32_t ref = (enc == 0) ? id : (0x01000000u | (id & 0xFFFFFFu));
            auto leave = CommandBuilder::Get().Create(lc::kSpec);
            leave.Set<uint32_t>(lc::country, ref);
            std::string lwhy;
            const bool lok = leave.IsValid(&lwhy);
            auto join = CommandBuilder::Get().Create(jc::kSpec);
            join.Set<uint32_t>(jc::country, ref).Set<uint8_t>(jc::has_galactic_emperor, 0);
            std::string jwhy;
            const bool jok = join.IsValid(&jwhy);
            row[enc == 0 ? "bare" : "tagged"] = {
                {"leave_valid", lok}, {"leave_reason", lwhy},
                {"join_valid", jok}, {"join_reason", jwhy},
            };
        }
        rows.push_back(row);
    }
    return { {"success", true}, {"items", rows} };
}

// 读到 serializer 抓下来的 CGalacticCommunity：只按 SDK 偏移读，不写、不造对象。
// 数组字段的形状照议案支持者那一份实测：字段 +8 = 数据指针，+0x10 = size，条目是 4 字节国家/议案引用。
nlohmann::json ContactsManager::ReadCommunityCaptureJson() {
    const uintptr_t self = (uintptr_t)CommandBuilder::Get().CapturedCommunity();
    nlohmann::json out = { {"success", true}, {"captured", self != 0}, {"self", (unsigned long long)self} };
    if (!self) {
        out["hint"] = "还没抓到：先 install_community_capture，再让游戏存一次档（save_game 即可）";
        return out;
    }
    auto u32 = [&](uintptr_t off) -> long long {
        uint32_t v = 0;
        if (!SafeReadU32((const void*)(self + off), &v)) return -1;
        return (long long)v;
    };
    auto refs = [&](uintptr_t off) {
        nlohmann::json arr = nlohmann::json::array();
        void* data = nullptr;
        uint32_t cap = 0, size = 0;
        if (!SafeReadPtr((const void*)(self + off + 0x08), &data) || !data) return arr;
        // 字段形状：{哨兵 +0x00, 数据 +0x08, **cap +0x10, size +0x14**}（2026-11-21 用 proposed 量准：
        // cap 13 / size 10，先前把 +0x10 当 size，多读了 3 条陈旧值）
        if (!SafeReadU32((const void*)(self + off + 0x10), &cap)) return arr;
        if (!SafeReadU32((const void*)(self + off + 0x14), &size)) return arr;
        if (size > 512) size = 512;
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)((uintptr_t)data + (size_t)i * 4), &v)) break;
            arr.push_back(v);
        }
        return arr;
    };
    out["formed_raw"] = u32(0x08);
    out["leader"] = u32(0x178);
    out["members"] = refs(0x180);
    out["council"] = refs(0x198);
    out["permanent_council"] = refs(0x1B0);
    out["proposed"] = refs(0x1C8);
    out["passed"] = refs(0x1E0);
    out["failed"] = refs(0x1F8);
    out["voting"] = u32(0x2D0);
    out["last"] = u32(0x2D4);
    out["vote_length"] = u32(0x2D8);
    out["days"] = u32(0x2DC);
    out["cooldown"] = u32(0x2E0);
    out["election"] = u32(0x2E4);
    out["custodian_term"] = u32(0x2E8);
    out["council_positions"] = u32(0x2EC);
    uint64_t ia = 0;
    if (SafeReadU64((const void*)(self + 0x2F8), &ia)) out["imperial_authority"] = (unsigned long long)ia;
    return out;
}

// 游戏里的日期是 u32：值 = (年*360 + (月-1)*30 + (日-1)) * 24 + 43800000。
// 2026-11-21 用四个已知日期对出来（2269.01.01 = 63404160、2286.12.30、2274.12.30、2319.02.01 全部吻合）。
static std::string DecodeGameDate(uint32_t v) {
    if (v < 43800000u) return std::string();
    const uint32_t ord = (v - 43800000u) / 24u;
    const uint32_t y = ord / 360u;
    const uint32_t rem = ord % 360u;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%02u.%02u", y, rem / 30u + 1u, rem % 30u + 1u);
    return std::string(buf);
}

// 只读：共同体议案库。TPdxRef<CResolution>::_pDatabase → {slots +0x18, cap +0x20}，
// 16 字节一条、对象在 +8（与联邦同一套。注意 cap 是 0x400，别扫 size）。
nlohmann::json ContactsManager::GetResolutionsJson() {
    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CResolution_pDatabase), &db) || !db) {
        return { {"success", false}, {"error", "TPdxRef<CResolution>::_pDatabase 读不出来"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap > 100000u) {
        return { {"success", false}, {"error", "议案库的 slots/cap 读不出来"} };
    }
    auto refs = [](uintptr_t field) {
        nlohmann::json arr = nlohmann::json::array();
        void* data = nullptr;
        uint32_t size = 0;
        if (!SafeReadPtr((const void*)(field + 0x08), &data) || !data) return arr;
        if (!SafeReadU32((const void*)(field + 0x14), &size)) return arr;   // cap 在 +0x10、size 在 +0x14
        if (size > 512) size = 512;
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t v = 0;
            if (!SafeReadU32((const void*)((uintptr_t)data + (size_t)i * 4), &v)) break;
            arr.push_back(v);
        }
        return arr;
    };
    nlohmann::json items = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* res = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &res) || !res) continue;
        uint32_t id = 0xFFFFFFFFu, country = 0xFFFFFFFFu, target = 0xFFFFFFFFu;
        uint32_t freeze = 0, pass = 0;
        SafeReadU32((const void*)((uintptr_t)res + 0x08), &id);
        SafeReadU32((const void*)((uintptr_t)res + 0x20), &country);
        SafeReadU32((const void*)((uintptr_t)res + 0x24), &target);
        SafeReadU32((const void*)((uintptr_t)res + 0x28), &freeze);
        SafeReadU32((const void*)((uintptr_t)res + 0x2C), &pass);
        std::string type_key;
        void* type = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)res + 0x18), &type) && type) {
            void* key_ptr = nullptr;
            char kb[128] = {0};
            if (SafeReadPtr((const void*)((uintptr_t)type + 0x20), &key_ptr) && key_ptr &&
                SafeReadCStr(key_ptr, kb, sizeof kb)) {
                type_key = kb;
            }
        }
        items.push_back({
            {"id", id},
            {"type", type_key},
            {"country", country},
            {"target", target},
            {"freeze_date", DecodeGameDate(freeze)},
            {"pass_date", DecodeGameDate(pass)},
            {"supporters", refs((uintptr_t)res + 0x30)},
            {"opponents", refs((uintptr_t)res + 0x48)},
        });
    }
    return { {"success", true}, {"count", items.size()}, {"items", items} };
}

// 只读：逐个议案类型造一条 propose_resolution_command，只跑 IsValid（不投递）。
// 这就是界面「能不能提这条」那一下：potential（根本不列出来）与 allow（列出来但提交被拒）
// 都由引擎判，包括「必须有监管人」「泛星系防卫军要已存在」这些脚本条件。
nlohmann::json ContactsManager::ProbeResolutionTypesJson(uint32_t country_id, uint32_t target_id, bool all) {
    namespace pr = sdk::cmd::propose_resolution_command;
    if (country_id == 0xFFFFFFFFu) {
        void* me = GetPlayerCountry();
        if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &country_id) ||
            country_id == 0xFFFFFFFFu) {
            return { {"success", false}, {"error", "player country unreadable"} };
        }
    }
    uintptr_t db = base_address_ + sdk::glob::TGameDatabase_CResolutionTypeDatabase_pInstance;
    nlohmann::json items = nlohmann::json::array();
    for (void* t : ScriptTypes(db)) {
        void* key_ptr = nullptr;
        char kb[128] = {0};
        if (!SafeReadPtr((const void*)((uintptr_t)t + 0x20), &key_ptr) || !key_ptr ||
            !SafeReadCStr(key_ptr, kb, sizeof kb) || !kb[0]) {
            continue;
        }
        auto probe = CommandBuilder::Get().Create(pr::kSpec);
        probe.Set<void*>(pr::type, t)
             .Set<uint32_t>(pr::country, country_id)
             .Set<uint32_t>(pr::target, target_id);
        std::string why;
        const bool ok = probe.IsValid(&why);
        if (ok || all) {
            items.push_back({ {"key", std::string(kb)}, {"proposable", ok}, {"reason", why} });
        }
    }
    return { {"success", true}, {"country_id", country_id}, {"count", items.size()}, {"items", items} };
}

// 提交一条议案。类型对象只能按 key 现查（命令要的是指针，不是字符串），
// 所以要遍历一遍 CResolutionTypeDatabase——本档 277 个，几毫秒。
nlohmann::json ContactsManager::ProposeResolution(const std::string& type_key, uint32_t target_id, bool dry_run) {
    namespace pr = sdk::cmd::propose_resolution_command;
    if (type_key.empty()) {
        return { {"success", false}, {"error", "type_key must not be empty"} };
    }
    void* me = GetPlayerCountry();
    uint32_t country_id = 0xFFFFFFFFu;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &country_id) ||
        country_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country unreadable"} };
    }
    void* found = nullptr;
    uintptr_t db = base_address_ + sdk::glob::TGameDatabase_CResolutionTypeDatabase_pInstance;
    for (void* t : ScriptTypes(db)) {
        void* key_ptr = nullptr;
        char kb[128] = {0};
        if (SafeReadPtr((const void*)((uintptr_t)t + 0x20), &key_ptr) && key_ptr &&
            SafeReadCStr(key_ptr, kb, sizeof kb) && type_key == kb) {
            found = t;
            break;
        }
    }
    if (!found) {
        return { {"success", false}, {"error", "no resolution type with key " + type_key} };
    }
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    cmd.Set<void*>(pr::type, found)
       .Set<uint32_t>(pr::country, country_id)
       .Set<uint32_t>(pr::target, target_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"stage", "IsValid"}, {"type", type_key}, {"reason", why} };
    }
    if (dry_run) {
        return { {"success", true}, {"dry_run", true}, {"type", type_key},
                 {"country_id", country_id}, {"target", target_id} };
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"stage", "Post"}, {"type", type_key}, {"error", cmd.error()} };
    }
    LOGF("[COMMUNITY] proposed resolution '%s' (target %u)", type_key.c_str(), target_id);
    return { {"success", true}, {"posted", true}, {"type", type_key},
             {"country_id", country_id}, {"target", target_id} };
}

// 对在台上的议案投票。resolution 是 CResolution 的 4 字节引用（不是指针），vote 是引擎的枚举值。
nlohmann::json ContactsManager::VoteResolution(uint32_t resolution_id, int vote_value, bool dry_run) {
    namespace cv = sdk::cmd::cast_vote_command;
    void* me = GetPlayerCountry();
    uint32_t country_id = 0xFFFFFFFFu;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &country_id) ||
        country_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country unreadable"} };
    }
    auto cmd = CommandBuilder::Get().Create(cv::kSpec);
    cmd.Set<uint32_t>(cv::resolution, resolution_id)
       .Set<uint32_t>(cv::country, country_id)
       .Set<uint32_t>(cv::vote, (uint32_t)vote_value);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"stage", "IsValid"}, {"resolution", resolution_id},
                 {"vote", vote_value}, {"reason", why} };
    }
    if (dry_run) {
        return { {"success", true}, {"dry_run", true}, {"resolution", resolution_id},
                 {"country_id", country_id}, {"vote", vote_value} };
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"stage", "Post"}, {"error", cmd.error()} };
    }
    LOGF("[COMMUNITY] vote on resolution %u = %d", resolution_id, vote_value);
    return { {"success", true}, {"posted", true}, {"resolution", resolution_id},
             {"country_id", country_id}, {"vote", vote_value} };
}

// 提前结束会议：只有监管人能用，引擎自己判（拒绝原因原样回）。
nlohmann::json ContactsManager::EndSenateSession(uint32_t resolution_id, bool dry_run) {
    namespace es = sdk::cmd::end_senate_session_command;
    void* me = GetPlayerCountry();
    uint32_t country_id = 0xFFFFFFFFu;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &country_id) ||
        country_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country unreadable"} };
    }
    auto cmd = CommandBuilder::Get().Create(es::kSpec);
    cmd.Set<uint32_t>(es::resolution, resolution_id)
       .Set<uint32_t>(es::country, country_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"stage", "IsValid"}, {"resolution", resolution_id}, {"reason", why} };
    }
    if (dry_run) {
        return { {"success", true}, {"dry_run", true}, {"resolution", resolution_id}, {"country_id", country_id} };
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"stage", "Post"}, {"error", cmd.error()} };
    }
    LOGF("[COMMUNITY] end senate session on resolution %u", resolution_id);
    return { {"success", true}, {"posted", true}, {"resolution", resolution_id}, {"country_id", country_id} };
}

// 排队中的议案：支持 / 反对 / 撤支持 / 否决 / 冻结 / 紧急措施。
// 六个命令形状一致（resolution +0x20、country +0x24），只有前三个多一个 force 字节。
nlohmann::json ContactsManager::QueuedResolutionAction(uint32_t resolution_id, const std::string& action, bool dry_run) {
    void* me = GetPlayerCountry();
    uint32_t country_id = 0xFFFFFFFFu;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &country_id) ||
        country_id == 0xFFFFFFFFu) {
        return { {"success", false}, {"error", "player country unreadable"} };
    }
    const sdk::CmdSpec* spec = nullptr;
    if (action == "support") spec = &sdk::cmd::support_proposed_resolution_command::kSpec;
    else if (action == "oppose") spec = &sdk::cmd::oppose_proposed_resolution_command::kSpec;
    else if (action == "abstain" || action == "withdraw") spec = &sdk::cmd::remove_support_for_proposed_resolution_command::kSpec;
    else if (action == "veto") spec = &sdk::cmd::veto_resolution_command::kSpec;
    else if (action == "freeze") spec = &sdk::cmd::freeze_proposed_resolution_command::kSpec;
    else if (action == "emergency") spec = &sdk::cmd::propose_emergency_measure_command::kSpec;
    if (!spec) {
        return { {"success", false}, {"error", "action 只能是 support/oppose/abstain/veto/freeze/emergency"} };
    }
    auto cmd = CommandBuilder::Get().Create(*spec);
    cmd.Set<uint32_t>(0x20, resolution_id).Set<uint32_t>(0x24, country_id);
    std::string why;
    if (!cmd.IsValid(&why)) {
        return { {"success", false}, {"stage", "IsValid"}, {"action", action},
                 {"resolution", resolution_id}, {"reason", why} };
    }
    if (dry_run) {
        return { {"success", true}, {"dry_run", true}, {"action", action}, {"resolution", resolution_id} };
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"stage", "Post"}, {"action", action}, {"error", cmd.error()} };
    }
    LOGF("[COMMUNITY] queued resolution %u: %s", resolution_id, action.c_str());
    return { {"success", true}, {"posted", true}, {"action", action}, {"resolution", resolution_id},
             {"country_id", country_id} };
}

// ———— 联邦读取（2026-11-21 只读探针）————
// CCountry+0x1C38 = federation ref（低 32 位 id，高位 0xFFFFFFFF；id 0 = 不在联邦）。
// 这里只做只读：全局 TPdxRef<CFederation>::_pDatabase → 对象表 → 第 id 个对象，
// 再把 db 头、对象头、progression 头按 qword 吐出来，先看清布局再谈正式工具。
nlohmann::json ContactsManager::ProbeFederationJson(uint32_t fed_id) {
    nlohmann::json out = { {"success", true}, {"fed_id", fed_id} };
    auto dump = [&](const void* base, uint32_t from, uint32_t to) {
        nlohmann::json words = nlohmann::json::array();
        for (uint32_t o = from; o < to; o += 8) {
            uint64_t v = 0;
            if (!SafeReadU64((const void*)((uintptr_t)base + o), &v)) break;
            words.push_back({ {"offset", o}, {"value", (int64_t)v},
                              {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
        }
        return words;
    };
    const uintptr_t g = base_address_ + sdk::glob::TPdxRef_CFederation_pDatabase;
    out["global_addr"] = (unsigned long long)g;
    void* db = nullptr;
    if (!SafeReadPtr((const void*)g, &db) || !db) {
        out["success"] = false;
        out["error"] = "TPdxRef<CFederation>::_pDatabase 读不出来";
        return out;
    }
    out["db"] = (unsigned long long)(uintptr_t)db;
    out["db_head"] = dump(db, 0, 0x40);
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap > 1000000u) {
        out["note"] = "db+0x18/+0x20 不像 slots/cap（先看 db_head）";
        return out;
    }
    out["slots"] = (unsigned long long)(uintptr_t)slots;
    out["cap"] = cap;
    if (fed_id >= cap) {
        out["note"] = "fed_id 超出 cap";
        return out;
    }
    void* fed = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)fed_id * 16 + 8), &fed) || !fed) {
        out["note"] = "该槽位没有对象（id 0 通常就是这种情况）";
        return out;
    }
    out["fed_ptr"] = (unsigned long long)(uintptr_t)fed;
    void* vt = nullptr;
    out["vt_rva"] = (SafeReadPtr((const void*)fed, &vt) && vt) ? (unsigned long long)((uintptr_t)vt - base_address_) : 0ULL;
    out["words"] = dump(fed, 0, 0x120);
    void* prog = nullptr;
    if (SafeReadPtr((const void*)((uintptr_t)fed + 0x80), &prog) && prog) {
        out["prog_ptr"] = (unsigned long long)(uintptr_t)prog;
        out["prog_words"] = dump(prog, 0x190, 0x1E0);
    }
    return out;
}

// ———— 待回应的外交动作（2026-11-20 实测布局，见头文件注释）————
// 只读第一条：元素步长还没量出来（size=1 时量不出间隔），size>1 会在 note 里说明。
nlohmann::json ContactsManager::GetIncomingActionsJson(uint32_t country_id) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    if (country_id == kNoId) {
        void* me = GetPlayerCountry();
        uint32_t mine = kNoId;
        if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &mine) || mine == kNoId) {
            return { {"success", false}, {"error", "player country id unreadable"} };
        }
        country_id = mine;
    }
    void* db = nullptr;
    void* slots = nullptr;
    void* country = nullptr;
    uint32_t cap = 0, check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || (country_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (country_id & 0xFFFFFF) * 16 + 8), &country) || !country ||
        !SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &check) ||
        (check & 0xFFFFFFu) != (country_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(country_id)} };
    }
    const ptrdiff_t kIncoming = sdk::ent::CCountry::incoming_actions;  // 0x1950
    uint64_t data = 0, packed = 0;
    if (!SafeReadU64((const void*)((uintptr_t)country + kIncoming), &data)) {
        return { {"success", false}, {"error", "incoming_actions unreadable"} };
    }
    uint64_t size = 0, ccap = 0;
    if (SafeReadU64((const void*)((uintptr_t)country + kIncoming + 8), &packed)) {
        size = packed & 0xFFFFFFFFull;
        ccap = packed >> 32;
    }
    nlohmann::json items = nlohmann::json::array();
    // 元素步长 0x10（2026-11-21 实测：{type_desc +0x00, action +0x08}，两条提案相隔 0x10）
    for (uint64_t ei = 0; data && ei < size; ++ei) {
        const uintptr_t elem = (uintptr_t)data + ei * 0x10;
        uint64_t type_desc = 0, action_ptr = 0;
        SafeReadU64((const void*)elem, &type_desc);
        SafeReadU64((const void*)(elem + 0x08), &action_ptr);
        if (action_ptr > 0x10000ULL && action_ptr < 0x7FFFFFFFFFFFULL) {
            uint64_t avt = 0;
            if (!SafeReadPtr((const void*)(uintptr_t)action_ptr, (void**)&avt) || avt < base_address_ ||
                avt - base_address_ > 0x4000000ULL) {
                continue;   // 空槽：指针不像动作对象（vtable 不在 exe 模块里）
            }
            uint64_t vt = 0, tag50 = 0;
            uint32_t actor = 0, serial = 0, tag = 0, offer_actor = 0, offer_recipient = 0, offer_war = 0, offer_status = 0;
            SafeReadU64((const void*)(uintptr_t)action_ptr, &vt);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x20), &actor);
            // 基类 recipient 在 +0x24（与求和动作同一处；条约类提案的双方只有这一个可靠来源）
            uint32_t base_recipient = 0xFFFFFFFFu;
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x24), &base_recipient);
            // +0x30 是一对 u32：低位 serial、高位 tag（实测 qword = 0x000001C0_00000000，tag 在 +0x34）
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x30), &serial);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x34), &tag);
            SafeReadU64((const void*)((uintptr_t)action_ptr + 0x50), &tag50);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x58), &offer_actor);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x5C), &offer_recipient);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x60), &offer_war);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x64), &offer_status);
            const unsigned long long vt_rva = vt ? (unsigned long long)(vt - base_address_) : 0ULL;
            // 动作类 → 名字（2026-11-21 实测，逐个用一条真提案验过 vtable）
            //   0x23B6B20 COfferPeaceAction          求和
            //   0x23CCCF8 要求臣服 / 0x23CC440 CProposeAgreementAction   附庸协议（双方在内嵌协议 +0x70/+0x74）
            //   0x23B8FC8 商业条约 ｜ 0x23B8928 驿站条约 ｜ 0x23B8C78 互不侵犯 ｜ 0x23B8288 防御同盟
            //   0x23B9318 研究协定
            const bool is_peace = (vt_rva == 0x23B6B20ULL);
            const bool is_agr = (vt_rva == 0x23CCCF8ULL || vt_rva == 0x23CC440ULL);
            const char* kind_name = is_agr ? "propose_agreement"
                                   : is_peace ? "offer_peace"
                                   : vt_rva == 0x23B8FC8ULL ? "form_commercial_pact"
                                   : vt_rva == 0x23B8928ULL ? "form_waystation_pact"
                                   : vt_rva == 0x23B8C78ULL ? "form_non_aggression_pact"
                                   : vt_rva == 0x23B8288ULL ? "form_defensive_pact"
                                   : vt_rva == 0x23B9318ULL ? "form_research_agreement"
                                   : vt_rva == 0x23B4158ULL ? "embassy"
                                   : vt_rva == 0x23B94C0ULL ? "guarantee_independence"
                                   : vt_rva == 0x23CC7C8ULL ? "ask_to_be_released_subject"
                                   : "unknown";
            const unsigned long long id_candidate = ((unsigned long long)tag << 32) | (unsigned long long)serial;
            static const char* kStatusNames[3] = { "status_quo", "surrender", "demand_surrender" };
            if (is_agr) {
                uint32_t a_owner = 0xFFFFFFFFu, a_target = 0xFFFFFFFFu;
                SafeReadU32((const void*)((uintptr_t)action_ptr + 0x70), &a_owner);
                SafeReadU32((const void*)((uintptr_t)action_ptr + 0x74), &a_target);
                uint64_t preset_obj = 0;
                std::string preset;
                if (SafeReadU64((const void*)((uintptr_t)action_ptr + 0xC8), &preset_obj) && preset_obj > 0x10000ULL) {
                    SafeReadPdxString((const void*)(uintptr_t)(preset_obj + 0x20), preset);
                }
                nlohmann::json terms = nlohmann::json::array();
                nlohmann::json rterms = nlohmann::json::array();
                const uintptr_t td = (uintptr_t)action_ptr + 0xC0;
                uint64_t ddata = 0, rdata = 0;
                uint32_t dsize = 0, rsize = 0;
                SafeReadU64((const void*)(td + 0x08), &ddata);
                SafeReadU32((const void*)(td + 0x14), &dsize);
                if (ddata > 0x10000ULL && dsize > 0 && dsize <= 64) {
                    for (uint32_t i = 0; i < dsize; ++i) {
                        uint64_t kp = 0, vp = 0;
                        if (!SafeReadU64((const void*)(uintptr_t)(ddata + i * 0x10), &kp)) break;
                        SafeReadU64((const void*)(uintptr_t)(ddata + i * 0x10 + 8), &vp);
                        std::string k, v;
                        if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                        if (vp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(vp + 0x20), v);
                        if (!k.empty()) terms.push_back({{"key", k}, {"value", v}});
                    }
                }
                SafeReadU64((const void*)(td + 0x20), &rdata);
                SafeReadU32((const void*)(td + 0x2C), &rsize);
                if (rdata > 0x10000ULL && rsize > 0 && rsize <= 16) {
                    for (uint32_t i = 0; i < rsize; ++i) {
                        uint64_t kp = 0, v = 0;
                        if (!SafeReadU64((const void*)(uintptr_t)(rdata + i * 0x10), &kp)) break;
                        SafeReadU64((const void*)(uintptr_t)(rdata + i * 0x10 + 8), &v);
                        std::string k;
                        if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                        if (!k.empty()) rterms.push_back({{"key", k}, {"value", (double)(int64_t)v / 100000.0}});
                    }
                }
                items.push_back({
                    {"action_ptr", (unsigned long long)action_ptr},
                    {"action_vtable_rva", vt_rva},
                    {"kind", "propose_agreement"},
                    {"actor", a_owner},
                    {"recipient", a_target},
                    {"agreement_owner", a_owner},
                    {"agreement_target", a_target},
                    {"agreement_preset", preset},
                    {"terms", terms},
                    {"resource_terms", rterms},
                    {"tag", tag},
                    {"serial", serial},
                    {"id_candidate", id_candidate},
                });
            } else items.push_back({
                {"action_ptr", (unsigned long long)action_ptr},
                {"action_vtable_rva", vt_rva},
                {"kind", kind_name},
                {"recipient", base_recipient},
                {"type_desc_rva", type_desc ? (unsigned long long)(type_desc - base_address_) : 0ULL},
                {"tag", tag},
                {"serial", serial},
                {"peace_offer_tag_rva", tag50 ? (unsigned long long)(tag50 - base_address_) : 0ULL},
                {"actor", actor},
                {"offer", { {"actor", offer_actor},
                            {"recipient", offer_recipient},
                            {"war", offer_war},
                            {"status", offer_status},
                            {"status_name", offer_status < 3 ? kStatusNames[offer_status] : "?"} }},
                {"id_candidate", id_candidate},
            });
        }
    }
    nlohmann::json r = { {"success", true},
                         {"country_id", country_id},
                         {"data_ptr", (unsigned long long)data},
                         {"size", (unsigned long long)size},
                         {"cap", (unsigned long long)ccap},
                         {"items", items} };
    if (items.size() != size) {
        r["note"] = "数组里有 " + std::to_string((unsigned long long)size) + " 条，只读到 " +
                    std::to_string((unsigned long long)items.size()) + " 条（空槽或读失败）";
    }
    return r;
}

// ———— 待表决的求和提案：扫所有国家的 proposed_actions(+0x1980) ————
// 成员侧 incoming_actions / proposed_actions 都是空的（2026-11-20 实测），
// 提案只存在**提案方**的 proposed_actions 里；投票前先用它把 id 找出来。
nlohmann::json ContactsManager::GetPendingVotesJson() {
    void* db = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap > 10000) {
        return { {"success", false}, {"error", "country database unreadable"} };
    }
    const ptrdiff_t kProposed = sdk::ent::CCountry::proposed_actions;  // 0x1980
    nlohmann::json items = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* country = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &country) || !country) continue;
        uint32_t check = 0;
        if (!SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &check)) continue;
        const uint32_t cid = check & 0xFFFFFFu;
        uint64_t data = 0, packed = 0;
        if (!SafeReadU64((const void*)((uintptr_t)country + kProposed), &data) || !data) continue;
        SafeReadU64((const void*)((uintptr_t)country + kProposed + 8), &packed);
        const uint64_t size = packed & 0xFFFFFFFFull;
        if (size == 0) continue;
        // 一个国家的 proposed_actions 可能挂着不止一条（2026-11-21 实测 size=4：四条法律表决）。
        // 元素 **0x30 字节**（48），动作对象在 +0x08——两组实测动作指针间隔正好 0x30（2026-11-21 量）。
        for (uint64_t e = 0; e < size; ++e) {
        uint64_t action_ptr = 0;
        SafeReadU64((const void*)((uintptr_t)data + (size_t)e * 0x30 + 0x08), &action_ptr);
        if (!action_ptr) continue;
        uint64_t vt = 0;
        SafeReadU64((const void*)(uintptr_t)action_ptr, &vt);
        const unsigned long long vt_rva = vt ? (unsigned long long)(vt - base_address_) : 0ULL;
        const bool is_peace = (vt_rva == 0x23B6B20ULL);        // COfferPeaceAction
        const bool is_declare = (vt_rva == 0x23B9EB0ULL);      // CDeclareWarAction（2026-11-20）
        // 踢出联邦成员（2026-11-21 实测：vtable 0x23B73A0，id 与前两类同偏移）
        const bool is_kick = (vt_rva == 0x23B73A0ULL);
        // 联邦法律表决（2026-11-21 实测：vtable 0x23B7050；法律对象在 +0x50，键在对象 +0x20）
        const bool is_fed_law = (vt_rva == 0x23B7050ULL);
        // 联邦类型表决（2026-11-21 实测：vtable 0x23B7548；类型对象在 +0x50，键在对象 +0x28 → "default_federation"）
        const bool is_fed_type = (vt_rva == 0x23B7548ULL);
        // 2026-11-21 改成通用：白名单之外的动作也列出来（kind = unknown + vtable_rva）。
        // 投票命令本身对任意 id 都通用（踢人已证），所以「认不出类」不该等于「看不见」——
        // 法律表决、类型表决、邀请表决这些没抓过 vtable 的，先列出来交给人判断。
        const bool known = is_peace || is_declare || is_kick || is_fed_law || is_fed_type;
        uint32_t actor = 0, serial = 0, tag = 0, offer_actor = 0, offer_recipient = 0, offer_war = 0, offer_status = 0;
        SafeReadU32((const void*)((uintptr_t)action_ptr + 0x20), &actor);
        // 三类已知动作的 id 都在同一对 u32：+0x30 serial、+0x34 tag（2026-11-20 / 11-21 实测对位）。
        // 未知类照样按这两个位置读——读出来的可能不是 id，标记 unknown 就是为了提醒这一点
        SafeReadU32((const void*)((uintptr_t)action_ptr + 0x30), &serial);
        SafeReadU32((const void*)((uintptr_t)action_ptr + 0x34), &tag);
        if (is_peace) {
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x58), &offer_actor);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x5C), &offer_recipient);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x60), &offer_war);
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x64), &offer_status);
        } else {
            // 宣战 / 踢人：recipient 在 +0x24（带标记的引用，踢人时就是被踢的国家），没有 offer 那四个字段
            SafeReadU32((const void*)((uintptr_t)action_ptr + 0x24), &offer_recipient);
            offer_actor = actor;
        }
        static const char* kStatusNames[3] = { "status_quo", "surrender", "demand_surrender" };
        std::string goal_key;
        if (is_declare) {
            uint64_t goal_ptr = 0;
            SafeReadU64((const void*)((uintptr_t)action_ptr + 0x58), &goal_ptr);
            if (goal_ptr) SafeReadPdxString((const void*)((uintptr_t)goal_ptr + 0x20), goal_key);
        }
        // 法律表决：法律对象在 +0x50，它的键（如 succession_type_strongest）在对象 +0x20 的字符串指针
        std::string law_key;
        if (is_fed_law) {
            uint64_t law_ptr = 0;
            SafeReadU64((const void*)((uintptr_t)action_ptr + 0x50), &law_ptr);
            if (law_ptr) {
                void* key_chars = nullptr;
                char kb[64] = {0};
                if (SafeReadPtr((const void*)((uintptr_t)law_ptr + 0x20), &key_chars) && key_chars) {
                    if (SafeReadCStr(key_chars, kb, sizeof kb)) law_key = kb;
                }
            }
        }
        // 类型表决：类型对象在 +0x50；键在 **+0x20**（和联邦类型库条目一致），+0x28 是所有类型共用的
        // 一个 .rdata 指针（读出来只有一个 "H"——2026-11-21 就是因为先试了 +0x28 才误判）。
        std::string federation_type_key;
        unsigned long long federation_type_ptr = 0;
        if (is_fed_type) {
            uint64_t tp = 0;
            SafeReadU64((const void*)((uintptr_t)action_ptr + 0x50), &tp);
            federation_type_ptr = tp;
            if (tp) {
                for (uint32_t off : {0x20u, 0x28u}) {
                    void* kp = nullptr;
                    char kb[64] = {0};
                    if (SafeReadPtr((const void*)((uintptr_t)tp + off), &kp) && kp &&
                        SafeReadCStr(kp, kb, sizeof kb) && kb[0]) {
                        federation_type_key = kb;
                        break;
                    }
                }
            }
        }
        items.push_back({
            {"holder_country", cid},
            {"kind", is_peace ? "offer_peace"
                              : (is_declare ? "declare_war"
                                            : (is_kick ? "kick_from_federation"
                                                       : (is_fed_law ? "vote_for_federation_law"
                                                                     : (is_fed_type ? "vote_for_federation_type"
                                                                                    : "unknown"))))},
            {"known", known},
            {"vtable_rva", vt_rva},
            {"goal_key", goal_key},
            {"law_key", law_key},
            {"federation_type_key", federation_type_key},
            {"federation_type_ptr", federation_type_ptr},
            {"action_ptr", (unsigned long long)action_ptr},
            {"actor", actor},
            {"tag", tag},
            {"serial", serial},
            {"id_candidate", ((unsigned long long)tag << 32) | (unsigned long long)serial},
            {"size", (unsigned long long)size},
            {"offer", { {"actor", offer_actor},
                        {"recipient", offer_recipient},
                        {"war", offer_war},
                        {"status", offer_status},
                        {"status_name", offer_status < 3 ? kStatusNames[offer_status] : "?"} }},
        });
        }  // for e
    }
    nlohmann::json r = { {"success", true}, {"count", items.size()}, {"items", items} };
    if (items.empty()) r["note"] = "没有任何国家挂着待处理的提案（proposed_actions 为空）";
    return r;
}

// ———— 宣战：用引擎自己的生产者造 CDeclareWarAction ————
// 2026-11-20 定案：NDiplomacyUtil::CreateDiplomaticAction 对 action_declare_war **是能造的**
// （对 action_offer_peace 返回 null，那是求和必须自己组的原因）。造出来的对象已经带好
// vtable/base/动作类型/内嵌 CWarGoal（类级静态 + 默认目标类型）/空的 extra_attacker，
// actor 与 recipient 也填好了，所以这里只需要：换战争目标类型（+0x58）→ 包 propose → IsValid → Post。
// 不再需要「界面宣战一次种模板」。
nlohmann::json ContactsManager::DeclareWar(uint32_t target_id, const std::string& goal_key, bool dry_run) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* me = GetPlayerCountry();
    uint32_t my_id = kNoId;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == kNoId) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    void* cdb = nullptr;
    void* slots = nullptr;
    void* recipient = nullptr;
    uint32_t cap = 0, check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (target_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (target_id & 0xFFFFFF) * 16 + 8), &recipient) || !recipient ||
        !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) ||
        (check & 0xFFFFFFu) != (target_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(target_id)} };
    }
    const uint32_t target_ref = check;
    if (goal_key == "wg_proxy_war") {
        // 脚本里带 proxy_wars_only = yes：只用于「代理人战争」那种特殊战争类型。
        // 引擎的 IsValid 不查这一项，真投会生成一场目标类型对不上的战争（2026-11-21 实测）。
        return { {"success", false},
                 {"error", "wg_proxy_war 只用于代理人战争（脚本里 proxy_wars_only = yes），"
                           "普通宣战不能用它；引擎 IsValid 不查这一项，真投会生成一场坏掉的战争"} };
    }
    if (goal_key.empty()) {
        // 生产者给的默认目标不是可用的宣战理由（IsValid 会回「尚未设置战争目标」），所以这里直接拒。
        return { {"success", false},
                 {"error", "goal_key is required（如 wg_subjugation / wg_conquest；生产者给的默认目标不可用）"} };
    }
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == "action_declare_war") {
            type = t;
            break;
        }
    }
    if (!type) return { {"success", false}, {"error", "找不到 action_declare_war 这个动作类型对象（类型库枚举失败）"} };

    CreateOnlyCtx ctx{ base_address_ + sdk::fn::NDiplomacyUtil_CreateDiplomaticAction, type, me, recipient, nullptr };
    if (!CommandBuilder::Get().CallGuarded(&CallCreateOnly, &ctx) || !ctx.action) {
        return { {"success", false}, {"error", "CreateDiplomaticAction 没有为 action_declare_war 造出对象"} };
    }
    void* action = ctx.action;

    void* goal_type = nullptr;
    if (!goal_key.empty()) {
        for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CWarGoalTypeDatabase_pInstance)) {
            std::string k;
            if (SafeReadPdxString((const void*)((uintptr_t)t + 0x20), k) && k == goal_key) {
                goal_type = t;
                break;
            }
        }
        if (!goal_type) {
            CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
            return { {"success", false}, {"error", "unknown war goal type " + goal_key} };
        }
        *(uint64_t*)((uintptr_t)action + 0x58) = (uint64_t)(uintptr_t)goal_type;
    }

    namespace pr = sdk::cmd::diplo_action_propose_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    if (!cmd) {
        CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
        return { {"success", false}, {"error", std::string("create propose command failed: ") + cmd.error()} };
    }
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    std::string why;
    const bool ok = cmd.IsValid(&why);
    nlohmann::json r = { {"success", ok}, {"dry_run", dry_run},
                         {"target_id", target_id}, {"actor", my_id}, {"goal_key", goal_key},
                         {"goal_type_ptr", goal_type ? (unsigned long long)(uintptr_t)goal_type : 0ULL},
                         {"action_ptr", (unsigned long long)(uintptr_t)action},
                         {"is_valid", ok} };
    if (!ok) {
        // 注意：命令对象已经持有这个动作（cmd.Set(+0x20)），它的析构会删；
        // 这里再删就是双重释放（2026-11-20 实测直接把游戏打崩）。别删。
        r["reason"] = AlertManager::Get().CleanPdxString(why);
        return r;
    }
    if (dry_run) {
        r["note"] = "IsValid 通过，没有投递（dry_run）";  // 动作随命令析构一起收，别手动删
        return r;
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())},
                 {"target_id", target_id}, {"actor", my_id} };
    }
    LOGF("[CONTACTS] declare war posted: %u -> %u goal %s", my_id, target_ref, goal_key.c_str());
    return { {"success", true}, {"posted", true}, {"target_id", target_id}, {"actor", my_id},
             {"goal_key", goal_key}, {"action_ptr", (unsigned long long)(uintptr_t)action} };
}

// ———— 对某个国家可选的战争目标：逐个问引擎 ————
// 每个 CWarGoalType 造一份带该目标的动作、只跑 IsValid（不投递）。默认只报可用的（all=true 才全列）。
// 名字用本地化键 war_goal_<key>（实测："war_goal_wg_subjugation" → 要求附庸）。
nlohmann::json ContactsManager::ProbeWarGoalsJson(uint32_t target_id, bool all) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* me = GetPlayerCountry();
    uint32_t my_id = kNoId;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == kNoId) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    void* cdb = nullptr;
    void* slots = nullptr;
    void* recipient = nullptr;
    uint32_t cap = 0, check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (target_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (target_id & 0xFFFFFF) * 16 + 8), &recipient) || !recipient ||
        !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) ||
        (check & 0xFFFFFFu) != (target_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(target_id)} };
    }
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == "action_declare_war") {
            type = t;
            break;
        }
    }
    if (!type) return { {"success", false}, {"error", "找不到 action_declare_war 这个动作类型对象"} };
    namespace pr = sdk::cmd::diplo_action_propose_command;
    nlohmann::json rows = nlohmann::json::array();
    int valid_count = 0, total = 0;
    for (void* goal : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CWarGoalTypeDatabase_pInstance)) {
        std::string key;
        if (!SafeReadPdxString((const void*)((uintptr_t)goal + 0x20), key) || key.empty()) continue;
        ++total;
        CreateOnlyCtx ctx{ base_address_ + sdk::fn::NDiplomacyUtil_CreateDiplomaticAction, type, me, recipient, nullptr };
        if (!CommandBuilder::Get().CallGuarded(&CallCreateOnly, &ctx) || !ctx.action) continue;
        void* action = ctx.action;
        *(uint64_t*)((uintptr_t)action + 0x58) = (uint64_t)(uintptr_t)goal;
        auto cmd = CommandBuilder::Get().Create(pr::kSpec);
        if (!cmd) continue;
        cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
        std::string why;
        const bool ok = cmd.IsValid(&why);
        const bool proxy_only = (key == "wg_proxy_war");  // proxy_wars_only = yes：普通宣战用不了
        if (ok && !proxy_only) ++valid_count;
        if ((!ok || proxy_only) && !all) continue;
        nlohmann::json row = { {"key", key}, {"valid", ok} };
        if (proxy_only) row["note"] = "proxy_wars_only：只用于代理人战争，普通宣战不可用";
        const std::string name = LocalizeKey("war_goal_" + key);
        if (!name.empty() && name != "war_goal_" + key) row["name"] = name;
        if (!ok) {
            const std::string cleaned = AlertManager::Get().CleanPdxString(why);
            if (!cleaned.empty()) row["reason"] = cleaned;
        }
        rows.push_back(row);
    }
    return { {"success", true}, {"target_id", target_id}, {"actor", my_id},
             {"total", total}, {"valid_count", valid_count}, {"all", all}, {"items", rows} };
}

// ———— 组建联邦：可选类型（2026-11-21） ————
// 第一个版本（probe_federation_types）在同一个调用里"枚举 DB → 逐条造动作 → 写 +0xB0 → IsValid"，
// 一调就把游戏静默崩掉两次。按 README「静默崩溃」那节的规矩，先只读：
// 这个探针不造对象、不写内存、不调 native，只把数据库头与每个条目的指针 / vtable / 两个候选键读出来。
nlohmann::json ContactsManager::ProbeFederationTypeDbJson() {
    const uintptr_t g = base_address_ + sdk::glob::TGameDatabase_CFederationTypeDatabase_pInstance;
    void* db = nullptr;
    if (!SafeReadPtr((const void*)g, &db) || !db) {
        return { {"success", false}, {"error", "CFederationTypeDatabase 指针读不出来"},
                 {"global_addr", (unsigned long long)g} };
    }
    void* arr = nullptr;
    uint32_t n = 0;
    const bool arr_ok = SafeReadPtr((const void*)((uintptr_t)db + 0x50), &arr) && arr;
    const bool n_ok = SafeReadU32((const void*)((uintptr_t)db + 0x5C), &n);
    nlohmann::json rows = nlohmann::json::array();
    if (arr_ok && n_ok && n <= 4096) {
        for (uint32_t i = 0; i < n; ++i) {
            void* t = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)arr + (size_t)i * 8), &t) || !t) continue;
            void* vt = nullptr;
            const unsigned long long rva =
                (SafeReadPtr((const void*)t, &vt) && vt) ? (unsigned long long)((uintptr_t)vt - base_address_) : 0ULL;
            void* p20 = nullptr;
            void* p28 = nullptr;
            SafeReadPtr((const void*)((uintptr_t)t + 0x20), &p20);
            SafeReadPtr((const void*)((uintptr_t)t + 0x28), &p28);
            char b20[64] = {0}, b28[64] = {0};
            if (p20) SafeReadCStr(p20, b20, sizeof b20);
            if (p28) SafeReadCStr(p28, b28, sizeof b28);
            rows.push_back({ {"i", i},
                             {"ptr", (unsigned long long)(uintptr_t)t},
                             {"vt_rva", rva},
                             {"p20", (unsigned long long)(uintptr_t)p20},
                             {"s20", std::string(b20)},
                             {"p28", (unsigned long long)(uintptr_t)p28},
                             {"s28", std::string(b28)} });
        }
    }
    return { {"success", true},
             {"global_addr", (unsigned long long)g},
             {"db", (unsigned long long)(uintptr_t)db},
             {"arr", (unsigned long long)(uintptr_t)arr},
             {"count", n},
             {"arr_ok", arr_ok},
             {"n_ok", n_ok},
             {"items", rows} };
}

// 对 CFederationTypeDatabase 里**指定那一个**类型造一份邀请动作、只跑 IsValid（不投递）。
// 2026-11-21 修正：类型对象的键在 **+0x20**（只读探针实测，7 条全是这样；+0x28 是一个共享指针，
// 之前按它读键、还拿 SafeReadPdxString 解，疑似把游戏读崩的直接原因）。
// only_type 空 → 只回类型清单（不造对象）；给 key → 只验那一条，一次一个动作，崩了也定位得到。
nlohmann::json ContactsManager::ProbeFederationTypesJson(uint32_t target_id, const std::string& only_type) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    constexpr unsigned long long kFedTypeVtRva = 0x2388990ULL;  // CFederationType（只读探针实测）
    void* me = GetPlayerCountry();
    uint32_t my_id = kNoId;
    if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &my_id) || my_id == kNoId) {
        return { {"success", false}, {"error", "player country id unreadable"} };
    }
    // 先把类型清单读出来（纯读）
    std::vector<std::pair<std::string, void*>> types;
    for (void* ft : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CFederationTypeDatabase_pInstance)) {
        unsigned long long vt = 0;
        void* pvt = nullptr;
        if (!SafeReadPtr(ft, &pvt) || !pvt) continue;
        vt = (unsigned long long)((uintptr_t)pvt - base_address_);
        if (vt != kFedTypeVtRva) continue;
        char key[64] = {0};
        void* pk = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)ft + kTypeKey), &pk) || !pk) continue;
        if (!SafeReadCStr(pk, key, sizeof key) || !key[0]) continue;
        types.emplace_back(std::string(key), ft);
    }
    if (only_type.empty()) {
        nlohmann::json list = nlohmann::json::array();
        for (auto& kv : types) list.push_back(kv.first);
        return { {"success", true}, {"mode", "list"}, {"actor", my_id}, {"count", types.size()}, {"types", list} };
    }
    void* chosen = nullptr;
    for (auto& kv : types) {
        if (kv.first == only_type) { chosen = kv.second; break; }
    }
    if (!chosen) {
        nlohmann::json list = nlohmann::json::array();
        for (auto& kv : types) list.push_back(kv.first);
        return { {"success", false}, {"error", "unknown federation type " + only_type}, {"types", list} };
    }
    void* cdb = nullptr;
    void* slots = nullptr;
    void* recipient = nullptr;
    uint32_t cap = 0, check = kNoId;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cdb) || !cdb ||
        !SafeReadPtr((const void*)((uintptr_t)cdb + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)cdb + 0x20), &cap) || (target_id & 0xFFFFFF) >= cap ||
        !SafeReadPtr((const void*)((uintptr_t)slots + (target_id & 0xFFFFFF) * 16 + 8), &recipient) || !recipient ||
        !SafeReadU32((const void*)((uintptr_t)recipient + sdk::rt::CCountry_id), &check) ||
        (check & 0xFFFFFFu) != (target_id & 0xFFFFFFu)) {
        return { {"success", false}, {"error", "no country with id " + std::to_string(target_id)} };
    }
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == "action_invite_to_federation") { type = t; break; }
    }
    if (!type) return { {"success", false}, {"error", "找不到 action_invite_to_federation 这个动作类型对象"} };
    namespace pr = sdk::cmd::diplo_action_propose_command;
    CreateOnlyCtx ctx{ base_address_ + sdk::fn::NDiplomacyUtil_CreateDiplomaticAction, type, me, recipient, nullptr };
    if (!CommandBuilder::Get().CallGuarded(&CallCreateOnly, &ctx) || !ctx.action) {
        return { {"success", false}, {"error", "造不出邀请动作"} };
    }
    void* action = ctx.action;
    *(void**)((uintptr_t)action + 0xB0) = chosen;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    if (!cmd) return { {"success", false}, {"error", "建不出 propose 命令"} };
    cmd.Set<uint64_t>(0x20, (uint64_t)(uintptr_t)action);
    std::string why;
    const bool ok = cmd.IsValid(&why);
    nlohmann::json r = { {"success", true}, {"mode", "validate"}, {"actor", my_id}, {"type", only_type}, {"valid", ok} };
    if (!ok) {
        const std::string cleaned = AlertManager::Get().CleanPdxString(why);
        if (!cleaned.empty()) r["reason"] = cleaned;
    }
    return r;
}

// ———— 待回应的外交动作：应答 / 投票 ————
nlohmann::json ContactsManager::RespondDiploAction(uint64_t id, bool accept, bool dry_run) {
    namespace pr = sdk::cmd::diplo_action_respond_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", std::string("create respond command failed: ") + cmd.error()} };
    }
    cmd.Set<uint64_t>(pr::id, id);
    cmd.Set<uint8_t>(pr::accept, accept ? (uint8_t)1 : (uint8_t)0);  // 实测：引擎只写 1 字节
    std::string why;
    const bool ok = cmd.IsValid(&why);
    nlohmann::json r = { {"success", ok}, {"dry_run", dry_run}, {"id", (unsigned long long)id},
                         {"id_low", (uint32_t)(id & 0xFFFFFFFFull)}, {"id_high", (uint32_t)(id >> 32)},
                         {"accept", accept}, {"is_valid", ok} };
    if (!ok) {
        r["reason"] = AlertManager::Get().CleanPdxString(why);
        return r;
    }
    if (dry_run) {
        r["note"] = "IsValid 通过，没有投递（dry_run）";
        return r;
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())},
                 {"id", (unsigned long long)id}, {"accept", accept} };
    }
    LOGF("[CONTACTS] respond posted: id 0x%llX accept %d", (unsigned long long)id, accept ? 1 : 0);
    return { {"success", true}, {"posted", true}, {"id", (unsigned long long)id}, {"accept", accept} };
}

nlohmann::json ContactsManager::VoteDiploAction(uint64_t id, int vote, uint32_t voter, bool dry_run) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    if (voter == kNoId) {
        void* me = GetPlayerCountry();
        if (!me || !SafeReadU32((const void*)((uintptr_t)me + sdk::rt::CCountry_id), &voter)) {
            return { {"success", false}, {"error", "player country id unreadable"} };
        }
    }
    namespace pr = sdk::cmd::diplo_action_vote_command;
    auto cmd = CommandBuilder::Get().Create(pr::kSpec);
    if (!cmd) {
        return { {"success", false}, {"error", std::string("create vote command failed: ") + cmd.error()} };
    }
    cmd.Set<uint64_t>(pr::id, id);
    cmd.Set<uint32_t>(pr::voter, voter);
    cmd.Set<uint32_t>(pr::vote, (uint32_t)vote);
    std::string why;
    const bool ok = cmd.IsValid(&why);
    nlohmann::json r = { {"success", ok}, {"dry_run", dry_run}, {"id", (unsigned long long)id},
                         {"voter", voter}, {"vote", vote}, {"is_valid", ok} };
    if (!ok) {
        r["reason"] = AlertManager::Get().CleanPdxString(why);
        return r;
    }
    if (dry_run) {
        r["note"] = "IsValid 通过，没有投递（dry_run）";
        return r;
    }
    if (!cmd.Post()) {
        return { {"success", false}, {"error", AlertManager::Get().CleanPdxString(cmd.error())},
                 {"id", (unsigned long long)id}, {"voter", voter}, {"vote", vote} };
    }
    LOGF("[CONTACTS] vote posted: id 0x%llX voter %u vote %d", (unsigned long long)id, voter, vote);
    return { {"success", true}, {"posted", true}, {"id", (unsigned long long)id},
             {"voter", voter}, {"vote", vote} };
}

// 只读探针：CWar 对象的一段内存，按 qword 吐（参战方数组与战争目标的真实布局未知时用它）。
// follow=true：把 war+offset 处的 8 字节当指针，从那里开始吐（参战方数组就是这样读出来的）。
nlohmann::json ContactsManager::ProbeWarJson(uint32_t war_id, uint32_t offset, uint32_t length, bool follow) {
    constexpr uint32_t kNoId = 0xFFFFFFFFu;
    void* war = nullptr;
    void* db = nullptr;
    void* arr = nullptr;
    uint32_t cap = 0;
    if (SafeReadPtr((const void*)(base_address_ + sdk::db::CWar), &db) && db &&
        SafeReadPtr((const void*)((uintptr_t)db + 0x18), &arr) && arr &&
        SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) && cap < 1000000) {
        for (uint32_t i = 0; i < cap; ++i) {
            void* obj = nullptr;
            uint32_t id = kNoId;
            if (!SafeReadPtr((const void*)((uintptr_t)arr + (size_t)i * 16 + 8), &obj) || !obj) continue;
            if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CWar_id), &id) || id != war_id) continue;
            war = obj;
            break;
        }
    }
    if (!war) return { {"error", "war not found: " + std::to_string(war_id)} };
    if (offset > 0x400) offset = 0;
    length = length == 0 ? 0x100u : length;
    if (length > 0x400) length = 0x400;
    // follow=false：从 war+offset 开始读，报告的是绝对偏移（老行为，别改）。
    // follow=true ：把 war+offset 处的值当指针，从那个地址开始读，报告的偏移相对那个地址。
    uintptr_t dump_base = (uintptr_t)war;
    uint32_t first = offset;
    if (follow) {
        uint64_t p = 0;
        if (!SafeReadU64((const void*)((uintptr_t)war + offset), &p) || !p) {
            return { {"success", false}, {"error", "no pointer at war+" + std::to_string(offset)} };
        }
        dump_base = (uintptr_t)p;
        first = 0;
    }
    nlohmann::json words = nlohmann::json::array();
    for (uint32_t o = first; o < first + length; o += 8) {
        uint64_t v = 0;
        if (!SafeReadU64((const void*)(dump_base + o), &v)) break;
        words.push_back({ {"offset", o}, {"value", (int64_t)v}, {"low32", (uint32_t)(v & 0xFFFFFFFF)}, {"high32", (uint32_t)(v >> 32)} });
    }
    return { {"success", true}, {"war_id", war_id}, {"ptr", (unsigned long long)(uintptr_t)war},
             {"followed", follow}, {"dump_base", (unsigned long long)dump_base}, {"words", words} };
}

nlohmann::json ContactsManager::GetContactsInfo(const std::string& mode) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };

    void* country = GetPlayerCountry();
    if (!country) return { {"error", "Player country not found"} };

    void* cmgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cmgr) || !cmgr || (uintptr_t)cmgr < 0x10000) {
        return { {"error", "Country manager not found"} };
    }

    void* carr = nullptr;
    uint32_t c_cap = 0;
    SafeReadPtr((const void*)((uintptr_t)cmgr + 0x18), &carr);
    SafeReadU32((const void*)((uintptr_t)cmgr + 0x20), &c_cap);

    void* rel_arr = nullptr;
    uint32_t rel_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + sdk::ent::CCountry::relations_manager + 0x18), &rel_arr);
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::relations_manager + 0x20), &rel_cnt);
    uint32_t own_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &own_id);

    // 国家名 / 国家指针：宗主国不一定在我们自己的 relation 表里，得按 id 从国家表里取
    auto country_ptr_by_id = [&](uint32_t id) -> void* {
        if (id >= c_cap) return nullptr;
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)carr + id * 16 + 8), &p)) return nullptr;
        return p;
    };
    auto country_display_name = [&](void* ptr) -> std::string {
        if (!ptr) return std::string();
        std::string key;
        SafeReadPdxString((const void*)((uintptr_t)ptr + sdk::ent::CCountry::name + 0x18), key);
        std::string nm = PersistentNameText((const void*)((uintptr_t)ptr + sdk::ent::CCountry::name));
        if (nm.empty()) nm = LocalizeKey(key);
        if (nm.empty()) nm = key;
        return nm;
    };
    // 附庸三字段：overlord 是 4 字节带标记引用（0xFFFFFFFF = 没有宗主）、
    // loyalty 是 u64 定点（×1e5，实测 -100 读成 -10000000）、subject_date 是 u32 打包日期。
    auto subject_block = [&](void* ptr) -> nlohmann::json {
        nlohmann::json j;
        uint32_t ov_ref = 0xFFFFFFFFu;
        uint64_t loyalty = 0;
        uint32_t sdate = 0;
        if (!ptr) return j;
        SafeReadU32((const void*)((uintptr_t)ptr + sdk::ent::CCountry::overlord), &ov_ref);
        SafeReadU64((const void*)((uintptr_t)ptr + sdk::ent::CCountry::loyalty), &loyalty);
        SafeReadU32((const void*)((uintptr_t)ptr + sdk::ent::CCountry::subject_date), &sdate);
        const bool sub = ov_ref != 0xFFFFFFFFu;
        uint32_t ov_id = ov_ref & 0xFFFFFF;
        j["is_subject"] = sub;
        j["overlord_id"] = sub ? (int)ov_id : -1;
        j["overlord_name"] = sub ? country_display_name(country_ptr_by_id(ov_id)) : std::string();
        j["loyalty"] = sub ? (double)(int64_t)loyalty / 100000.0 : 0.0;
        j["subject_date"] = sub ? DecodeGameDate(sdate) : std::string();
        return j;
    };

    std::string q_mode = mode;
    std::transform(q_mode.begin(), q_mode.end(), q_mode.begin(), ::tolower);
    if (q_mode.empty()) q_mode = "all";

    nlohmann::json empires = nlohmann::json::array();
    nlohmann::json first_contacts = nlohmann::json::array();

    uint32_t known_count = 0;

    for (uint32_t i = 0; i < rel_cnt && rel_arr; ++i) {
        void* rel = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)rel_arr + i * 8), &rel) || !rel) continue;

        // a relation of this country (the array also holds unused, zeroed slots)
        uint32_t rel_owner = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)rel + sdk::ent::CRelation::owner), &rel_owner) || rel_owner != own_id) continue;
        uint32_t other_ref = 0;
        SafeReadU32((const void*)((uintptr_t)rel + sdk::ent::CRelation::country), &other_ref);
        uint32_t other_id = other_ref & 0xFFFFFF;

        // skip the player's own relation (any country id is valid, 0 included)
        if (other_ref == own_id || other_id >= c_cap) continue;

        void* other_ptr = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)carr + other_id * 16 + 8), &other_ptr) || !other_ptr) continue;

        uint8_t f0 = 0, f1 = 0, f2 = 0;
        // the relation's state bits (contact, communications, pacts ...) start at CRelation::contact
        SafeReadU8((const void*)((uintptr_t)rel + sdk::ent::CRelation::contact), &f0);
        SafeReadU8((const void*)((uintptr_t)rel + sdk::ent::CRelation::contact + 1), &f1);
        SafeReadU8((const void*)((uintptr_t)rel + sdk::ent::CRelation::contact + 2), &f2);

        bool has_comm = (f0 & 0x08) != 0;
        bool has_contact = (f2 & 0x01) != 0;
        bool comm_pact = (f1 & 0x08) != 0;
        bool res_pact = (f1 & 0x10) != 0;
        bool mig_pact = (f1 & 0x20) != 0;

        // the empire's name as the game shows it (a generated name's key is a template such as
        // %ADJECTIVE%, filled from its variables); name_key is that key
        std::string name_key;
        SafeReadPdxString((const void*)((uintptr_t)other_ptr + sdk::ent::CCountry::name + 0x18), name_key);
        std::string loc_name = PersistentNameText((const void*)((uintptr_t)other_ptr + sdk::ent::CCountry::name));
        if (loc_name.empty()) loc_name = LocalizeKey(name_key);

        // Country type
        std::string country_type = "default";
        void* ct_ptr = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)other_ptr + sdk::ent::CCountry::type), &ct_ptr) && ct_ptr) {
            std::string ct_key;
            SafeReadPdxString((const void*)((uintptr_t)ct_ptr + 0x20), ct_key);
            if (!ct_key.empty()) country_type = ct_key;
        }

        if (has_comm) {
            known_count++;
            nlohmann::json emp = {
                {"country_id", other_id},
                {"name", loc_name},
                {"name_key", name_key},
                {"country_type", country_type},
                {"has_communications", true},
                {"treaties", {
                    {"commercial_pact", comm_pact},
                    {"research_agreement", res_pact},
                    {"migration_pact", mig_pact}
                }}
            };
            emp.update(subject_block(other_ptr));
            empires.push_back(emp);
        } else if (has_contact) {
            // First contact in progress
            first_contacts.push_back({
                {"country_id", other_id},
                {"name", loc_name},
                {"country_type", country_type},
                {"status", "first_contact_in_progress"}
            });
        }
    }

    nlohmann::json resp = {
        {"total_known_empires", known_count},
        {"pending_first_contacts_count", first_contacts.size()}
    };

    // 自己不在 contacts 表里（那张表是「我们与别人的关系」），单列一块，
    // 否则「我们是别人的附庸」在看板上一个字都看不见。
    if (q_mode == "all" || q_mode == "empires") {
        nlohmann::json self = {
            {"country_id", own_id},
            {"name", country_display_name(country)}
        };
        self.update(subject_block(country));
        resp["self"] = self;
    }

    if (q_mode == "all" || q_mode == "empires") {
        resp["empires"] = empires;
    }
    if (q_mode == "all" || q_mode == "first_contacts") {
        resp["first_contacts"] = first_contacts;
    }

    return resp;
}

nlohmann::json ContactsManager::GetSummaryJson() {
    void* country = GetPlayerCountry();
    if (!country) return { {"known_empires_count", 0}, {"pending_first_contacts_count", 0} };

    void* cmgr = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cmgr) || !cmgr || (uintptr_t)cmgr < 0x10000) {
        return { {"known_empires_count", 0}, {"pending_first_contacts_count", 0} };
    }
    uint32_t c_cap = 0;
    SafeReadU32((const void*)((uintptr_t)cmgr + 0x20), &c_cap);

    void* rel_arr = nullptr;
    uint32_t rel_cnt = 0;
    SafeReadPtr((const void*)((uintptr_t)country + sdk::ent::CCountry::relations_manager + 0x18), &rel_arr);
    SafeReadU32((const void*)((uintptr_t)country + sdk::ent::CCountry::relations_manager + 0x20), &rel_cnt);
    uint32_t own_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)country + sdk::rt::CCountry_id), &own_id);

    uint32_t known = 0;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < rel_cnt && rel_arr; ++i) {
        void* rel = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)rel_arr + i * 8), &rel) || !rel) continue;

        // a relation of this country (the array also holds unused, zeroed slots)
        uint32_t rel_owner = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)rel + sdk::ent::CRelation::owner), &rel_owner) || rel_owner != own_id) continue;
        uint32_t other_ref = 0;
        SafeReadU32((const void*)((uintptr_t)rel + sdk::ent::CRelation::country), &other_ref);
        uint32_t other_id = other_ref & 0xFFFFFF;
        if (other_ref == own_id || other_id >= c_cap) continue;

        uint8_t f0 = 0, f2 = 0;
        SafeReadU8((const void*)((uintptr_t)rel + sdk::ent::CRelation::contact), &f0);
        SafeReadU8((const void*)((uintptr_t)rel + sdk::ent::CRelation::contact + 2), &f2);

        if (f0 & 0x08) {
            known++;
        } else if (f2 & 0x01) {
            pending++;
        }
    }

    return {
        {"known_empires_count", known},
        {"pending_first_contacts_count", pending}
    };
}

// The first-contact table, read straight from the game: TPdxRef<CFirstContact>::_pDatabase
// (sdk::glob, 0x311BC50) -> { slots +0x18, capacity +0x20 }, one entry per 16 bytes, object at +8.
// Field offsets are sdk::ent::CFirstContact (stellaris_sdk.hpp:2178-2195).
nlohmann::json ContactsManager::ProbeFirstContactsJson() {
    if (!base_address_) return { {"error", "Bridge base address not set"} };

    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db) {
        return { {"error", "CFirstContact database pointer not readable"} };
    }

    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"error", "CFirstContact database has no plausible slot array"} };
    }

    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;

        uint32_t id = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CFirstContact_id), &id) || id == 0xFFFFFFFF) continue;

        uint32_t location = 0, owner = 0, other = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::location), &location);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::owner), &owner);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::country), &other);

        // stage is a pointer to the stage object; its key sits at +0x20 like every script type
        std::string stage_key;
        void* stage = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::stage), &stage) && stage) {
            SafeReadPdxString((const void*)((uintptr_t)stage + 0x20), stage_key);
        }

        std::string name;
        SafeReadPdxString((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::name), name);

        uint32_t date_raw = 0, clues = 0, last_roll = 0, difficulty = 0, status = 0;
        uint64_t days_left = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::date), &date_raw);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::clues), &clues);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::last_roll), &last_roll);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::difficulty), &difficulty);
        SafeReadU64((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::days_left), &days_left);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::status), &status);

        // 64 bytes from +0x28 (location .. days_left, plus the name head) for eyeballing a moved field
        char raw[64]{};
        SafeCopyChars(raw, (const char*)((uintptr_t)obj + sdk::ent::CFirstContact::location), sizeof(raw));
        static const char* kHex = "0123456789abcdef";
        std::string dump;
        dump.reserve(80);
        for (size_t b = 0; b < sizeof(raw); ++b) {
            unsigned char c = (unsigned char)raw[b];
            dump.push_back(kHex[c >> 4]);
            dump.push_back(kHex[c & 0xF]);
            if ((b & 7) == 7) dump.push_back(' ');
        }

        // flags (+0x290) 与 event 句柄 (+0xB8)：界面上的「locked」很可能就是 flags 里的位
        uint32_t flags = 0, event_handle = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::flags), &flags);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::event), &event_handle);
        arr.push_back({
            {"slot", i},
            {"obj", (uint64_t)(uintptr_t)obj},
            {"id", id},
            {"flags", flags},
            {"event_handle", event_handle},
            {"owner_id", owner & 0xFFFFFF},
            {"country_id", other & 0xFFFFFF},
            {"location", location & 0xFFFFFF},
            {"stage_key", stage_key},
            {"clues", (int32_t)clues},
            {"difficulty", (int32_t)difficulty},
            {"last_roll", (int32_t)last_roll},
            {"days_left", days_left},
            {"status", status},
            {"date_raw", date_raw},
            {"name", name},
            {"dump_28", dump}
        });
    }

    return {
        {"success", true},
        {"slot_capacity", cap},
        {"count", arr.size()},
        {"contacts", arr}
    };
}

// Readable view of the same table: the counterparty's name and the assigned envoy resolved, and
// (unless all=true) only the player's own contacts.
// 解锁一条「已发现但还锁着」的第一次接触：把记录 +0xC0 置 1。
// 依据 2026-11-19 实测：在事件窗里点选项后，接触记录 832 字节里**只有 +0xC0 从 00 变 01**，
// status/stage/days_left 都没动，而界面上随即出现特使派遣入口。
// 注意：这只是复现 UI 那一步（「关闭」按钮）的写入；阶段锁要应答事件，用 select_open_event_option。
nlohmann::json ContactsManager::AdvanceFirstContactJson(uint32_t country_id, uint32_t value) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    void* player = GetPlayerCountry();
    if (!player) return { {"error", "No player country (not in game?)"} };
    uint32_t player_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)player + sdk::rt::CCountry_id), &player_id);
    player_id &= 0xFFFFFF;

    nlohmann::json result = { {"success", false}, {"error", "contact not found: " + std::to_string(country_id)} };
    void* db = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"success", false}, {"error", "CFirstContact database is not readable"} };
    }
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;
        {
        uint32_t owner = 0, other = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::owner), &owner);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::country), &other);
        owner &= 0xFFFFFF;
        other &= 0xFFFFFF;
        if (owner != player_id || other != country_id) continue;

        // 保护（2026-11-19 深夜）：+0xC0 置 1 只对「初始锁」（status 0）安全。
        // status 1 = 一个阶段跑完、锁定等处理，那种要用 select_open_event_option 应答它背后的事件；
        // 硬写 1 会让推进按钮消失、接触卡死。重锁（value 0）不受此限。
        uint32_t status = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::status), &status);
        if (value != 0 && status != 0) {
            return { {"success", false},
                     {"country_id", country_id},
                     {"status", status},
                     {"error", "this contact is at status " + std::to_string(status) +
                                   " - only an initial lock (status 0) may be lifted here; a stage lock "
                                   "(status 1) must be answered with select_open_event_option, and "
                                   "status 2/3 are already finished. Writing +0xC0 on those can remove "
                                   "the advance button and stick the contact"} };
        }

        uint8_t before = 0xFF;
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &before);
        uint8_t want = (uint8_t)(value != 0 ? 1 : 0);
        SIZE_T wrote = 0;
        const BOOL ok = WriteProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)obj + 0xC0), &want, 1, &wrote);
        uint8_t after = 0xFF;
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &after);
        result = { {"success", ok && wrote == 1 && after == want}, {"country_id", country_id}, {"want", (int)want},
                   {"obj", (uint64_t)(uintptr_t)obj}, {"before", (int)before}, {"after", (int)after},
                   {"wrote", (uint64_t)wrote} };
        if (!result["success"].get<bool>()) result["error"] = "WriteProcessMemory failed";
        break;
        }
    }
    return result;
}
// 把 +0xC0 置 1 再立刻还原（2026-11-19 深夜）：不改最终状态，只当「通知界面重读一下」的敲门砖。
// 由来：首触的弹窗/卡片在新数据到位后不会自己刷新，写一次这个字节游戏就会重读；
// 写完立刻还原，接触的最终状态与调用前完全一致（before == restored 才算成功）。
nlohmann::json ContactsManager::PokeFirstContactJson(uint32_t country_id, bool hold) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    void* player = GetPlayerCountry();
    if (!player) return { {"error", "No player country (not in game?)"} };
    uint32_t player_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)player + sdk::rt::CCountry_id), &player_id);
    player_id &= 0xFFFFFF;

    nlohmann::json result = { {"success", false}, {"error", "contact not found: " + std::to_string(country_id)} };
    void* db = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"success", false}, {"error", "CFirstContact database is not readable"} };
    }
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;
        uint32_t owner = 0, other = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::owner), &owner);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::country), &other);
        if ((owner & 0xFFFFFF) != player_id || (other & 0xFFFFFF) != country_id) continue;

        uint8_t before = 0xFF;
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &before);
        uint8_t one = 1, after_set = 0xFF, restored = 0xFF;
        SIZE_T w1 = 0, w2 = 0;
        WriteProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)obj + 0xC0), &one, 1, &w1);
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &after_set);
        if (!hold) {
            WriteProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)obj + 0xC0), &before, 1, &w2);
            SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &restored);
        }

        return { {"success", w1 == 1 && (hold || (w2 == 1 && restored == before))},
                 {"hold", hold},
                 {"country_id", country_id},
                 {"obj", (uint64_t)(uintptr_t)obj},
                 {"before", (int)before},
                 {"after_set", (int)after_set},
                 {"restored", hold ? (int)after_set : (int)restored} };
    }
    return result;
}
// 推进一个阶段锁 / 收尾（2026-11-19 深夜，玩家推出的模型，实测符合）
// 界面上的那一下点击 = ① 把 +0xC0 置 1（警报收起）② 投事件（推进）③ 阶段切换时游戏把 0xC0 清回 0。
// 我们照这个顺序做，行为与点击一致。差别只有一处：最后一段（status 1 → 2）游戏不再清 0，
// 所以那种情况要调 settle 再收一次尾。
//
// settle=false：置 1 + 投事件（不碰事件本身以外的任何状态）
// settle=true ：只做收尾——status != 1 且 flag != 0 时把它写回 0（不投事件）
nlohmann::json ContactsManager::AdvanceFirstContactStageJson(uint32_t country_id, uint64_t ope, int option, bool settle) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    void* player = GetPlayerCountry();
    if (!player) return { {"error", "No player country (not in game?)"} };
    uint32_t player_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)player + sdk::rt::CCountry_id), &player_id);
    player_id &= 0xFFFFFF;

    void* db = nullptr;
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db ||
        !SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"success", false}, {"error", "CFirstContact database is not readable"} };
    }

    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;
        uint32_t owner = 0, other = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::owner), &owner);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::country), &other);
        if ((owner & 0xFFFFFF) != player_id || (other & 0xFFFFFF) != country_id) continue;

        uint32_t status = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::status), &status);
        uint8_t flag = 0xFF;
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &flag);

        if (settle) {
            if (status == 1 || flag == 0) {
                return { {"success", true}, {"settle", true}, {"changed", false}, {"country_id", country_id},
                         {"status", status}, {"flag", (int)flag} };
            }
            uint8_t zero = 0;
            SIZE_T wrote = 0;
            WriteProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)obj + 0xC0), &zero, 1, &wrote);
            uint8_t after = 0xFF;
            SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &after);
            return { {"success", wrote == 1 && after == 0}, {"settle", true}, {"changed", true},
                     {"country_id", country_id}, {"status", status}, {"flag_before", (int)flag}, {"flag_after", (int)after} };
        }

        // 第一段：把 flag 置 1（和 UI 点击的第一步一致）
        uint8_t one = 1;
        SIZE_T wrote = 0;
        WriteProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)obj + 0xC0), &one, 1, &wrote);
        uint8_t flag_set = 0xFF;
        SafeReadU8((const void*)((uintptr_t)obj + 0xC0), &flag_set);

        nlohmann::json posted = EventManager::Get().SelectOpenEventOptionJson(ope, option, false);
        return { {"success", posted.value("success", false)},
                 {"country_id", country_id},
                 {"status_before", status},
                 {"flag_before", (int)flag},
                 {"flag_set", (int)flag_set},
                 {"posted", posted} };
    }
    return { {"success", false}, {"error", "contact not found: " + std::to_string(country_id)} };
}
nlohmann::json ContactsManager::GetFirstContactsJson(bool all) {    if (!base_address_) return { {"error", "Bridge base address not set"} };

    void* player = GetPlayerCountry();
    uint32_t player_id = 0xFFFFFFFF;
    if (player) {
        // CCountry::id 里带类型位（例如 0x01000006 = country 6），而接触记录里的 owner 是裸 id，
        // 不掩掉低 24 位就永远比不上——换档后 country id 一变，整张表会被过滤成空
        uint32_t raw_id = 0xFFFFFFFF;
        if (SafeReadU32((const void*)((uintptr_t)player + sdk::rt::CCountry_id), &raw_id)) {
            player_id = raw_id & 0xFFFFFF;
        }
    }

    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db) {
        return { {"error", "CFirstContact database pointer not readable"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"error", "CFirstContact database has no plausible slot array"} };
    }

    // the country manager, for counterparty objects (same layout the rest of this file uses)
    void* cmgr = nullptr;
    void* countries = nullptr;
    uint32_t country_cap = 0;
    SafeReadPtr((const void*)(base_address_ + sdk::db::CCountry), &cmgr);
    if (cmgr) {
        SafeReadPtr((const void*)((uintptr_t)cmgr + 0x18), &countries);
        SafeReadU32((const void*)((uintptr_t)cmgr + 0x20), &country_cap);
    }

    // the galactic-object table, to turn a contact's location (a system ref) into a name
    void* gobj_slots = nullptr;
    uint32_t gobj_cap = 0;
    {
        void* gobj_db = nullptr;
        if (SafeReadPtr((const void*)(base_address_ + sdk::db::CGalacticObject), &gobj_db) && gobj_db) {
            SafeReadPtr((const void*)((uintptr_t)gobj_db + 0x18), &gobj_slots);
            SafeReadU32((const void*)((uintptr_t)gobj_db + 0x20), &gobj_cap);
        }
    }

    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t i = 0; i < cap; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;

        uint32_t id = 0xFFFFFFFF, owner = 0xFFFFFFFF, other = 0xFFFFFFFF, leader = 0xFFFFFFFF;
        if (!SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CFirstContact_id), &id) || id == 0xFFFFFFFF) continue;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::owner), &owner);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::country), &other);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::leader), &leader);
        owner &= 0xFFFFFF;
        other &= 0xFFFFFF;
        leader &= 0xFFFFFF;

        if (!all && player_id != 0xFFFFFFFF && owner != player_id) continue;

        std::string stage_key, name;
        void* stage = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::stage), &stage) && stage) {
            SafeReadPdxString((const void*)((uintptr_t)stage + 0x20), stage_key);
        }
        SafeReadPdxString((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::name), name);

        uint32_t clues = 0, last_roll = 0, difficulty = 0, status = 0, date_raw = 0;
        uint64_t days_left = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::clues), &clues);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::last_roll), &last_roll);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::difficulty), &difficulty);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::status), &status);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::date), &date_raw);
        SafeReadU64((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::days_left), &days_left);

        // the counterparty's name, as the game shows it (PersistentNameText first, key second)
        std::string other_name;
        if (countries && other < country_cap) {
            void* other_obj = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)countries + (size_t)other * 16 + 8), &other_obj) && other_obj) {
                other_name = PersistentNameText((const void*)((uintptr_t)other_obj + sdk::ent::CCountry::name));
                if (other_name.empty()) {
                    std::string name_key;
                    if (SafeReadPdxString((const void*)((uintptr_t)other_obj + sdk::ent::CCountry::name + 0x18), name_key)) {
                        other_name = LocalizeKey(name_key);
                    }
                }
            }
        }

        // where the contact sits: location is a ref to the system (0xFFFFFF = none)
        uint32_t location = 0xFFFFFF, system_name_id = 0xFFFFFF;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::location), &location);
        location &= 0xFFFFFF;
        std::string system_name;
        if (location != 0xFFFFFF && gobj_slots && location < gobj_cap) {
            void* gobj = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)gobj_slots + (size_t)location * 16 + 8), &gobj) && gobj) {
                system_name = PersistentNameText((const void*)((uintptr_t)gobj + sdk::ent::CGalacticObject::name));
                if (!system_name.empty()) system_name_id = location;
            }
        }

        // flags (+0x290) 与 event 句柄 (+0xB8)：界面上的「locked」很可能就是 flags 里的位
        uint32_t flags = 0, event_handle = 0;
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::flags), &flags);
        SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CFirstContact::event), &event_handle);
        arr.push_back({
            {"slot", i},
            {"obj", (uint64_t)(uintptr_t)obj},
            {"id", id},
            {"flags", flags},
            {"event_handle", event_handle},
            {"owner_id", owner},
            {"owner_is_player", owner == player_id},
            {"country_id", other},
            {"country_name", other_name},
            {"location", system_name_id},
            {"system_name", system_name},
            {"stage_key", stage_key},
            {"clues", (int32_t)clues},
            {"difficulty", (int32_t)difficulty},
            {"last_roll", (int32_t)last_roll},
            {"days_left", days_left},
            {"status", status},
            {"envoy_id", leader == 0xFFFFFF ? 0xFFFFFFFF : leader},
            {"date_raw", date_raw},
            {"name", name}
        });
    }

    return {
        {"success", true},
        {"player_country_id", player_id},
        {"all", all},
        {"count", arr.size()},
        {"contacts", arr}
    };
}

// Read-only: decode one leader's location block. CLeaderLocation is 16 bytes
// {type +0 (8 = first contact), assignment +4 (EEnvoyAssignment: 4 = first_contact),
//  id +8 (target id; a country for an envoy), position +0xC}.
nlohmann::json ContactsManager::ProbeLeaderJson(uint32_t leader_id) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };

    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CLeader_pDatabase), &db) || !db) {
        return { {"error", "CLeader database pointer not readable"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || leader_id >= cap) {
        return { {"error", "CLeader id out of range"} };
    }
    void* obj = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)leader_id * 16 + 8), &obj) || !obj) {
        return { {"error", "No leader in that slot"} };
    }

    std::string name;
    SafeReadPdxString((const void*)((uintptr_t)obj + sdk::ent::CLeader::name), name);

    uint32_t country = 0xFFFFFFFF, level = 0, id_ref = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CLeader::country), &country);
    SafeReadU32((const void*)((uintptr_t)obj + sdk::ent::CLeader::level), &level);
    SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CLeader_id), &id_ref);

    auto read_loc = [&](std::ptrdiff_t off) -> nlohmann::json {
        uint32_t type = 0, assignment = 0, id = 0, position = 0;
        SafeReadU32((const void*)((uintptr_t)obj + off + 0), &type);
        SafeReadU32((const void*)((uintptr_t)obj + off + 4), &assignment);
        SafeReadU32((const void*)((uintptr_t)obj + off + 8), &id);
        SafeReadU32((const void*)((uintptr_t)obj + off + 0xC), &position);
        return {
            {"type", type},
            {"assignment", assignment},
            {"id", id & 0xFFFFFF},
            {"id_raw", id},
            {"position", (int32_t)position}
        };
    };

    return {
        {"success", true},
        {"leader_id", leader_id},
        {"id_ref", id_ref},
        {"name", name},
        {"country_id", country & 0xFFFFFF},
        {"level", (int32_t)level},
        {"location", read_loc(sdk::ent::CLeader::location)},
        {"council_location", read_loc(sdk::ent::CLeader::council_location)}
    };
}

// 只读：把一条 CFirstContact 的一段原始字节打出来（外加逐 qword 解码）。
// 用途：阶段事件就挂在这个对象上（CFirstContact::event = +0xB8、events = +0x310），
// 而视图把这些事件的选项渲染成按钮，所以「按按钮」其实等价于「提交那个事件的选项」。
nlohmann::json ContactsManager::ProbeFirstContactJson(uint32_t contact_id, int32_t offset, int32_t length) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    if (offset < 0) offset = 0;
    if (length <= 0) length = 0x80;
    if (length > 0x400) length = 0x400;

    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db) {
        return { {"error", "CFirstContact database pointer not readable"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"error", "CFirstContact database has no plausible slot array"} };
    }
    void* obj = nullptr;
    for (uint32_t i = 0; i < cap && !obj; ++i) {
        void* o = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &o) || !o) continue;
        uint32_t id = 0xFFFFFFFF;
        if (SafeReadU32((const void*)((uintptr_t)o + sdk::rt::CFirstContact_id), &id) && id == contact_id) obj = o;
    }
    if (!obj) return { {"error", "First contact id not found: " + std::to_string(contact_id)} };

    std::vector<char> buf((size_t)length, 0);
    if (!SafeCopyChars(buf.data(), (const char*)((uintptr_t)obj + offset), (size_t)length)) {
        return { {"error", "Reading that window faulted"} };
    }

    static const char* kHex = "0123456789abcdef";
    std::string hex, ascii;
    for (int i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)buf[(size_t)i];
        int base = offset + i;
        if (base % 16 == 0) {
            if (!hex.empty()) hex += "\n";
            char head[16]{};
            snprintf(head, sizeof(head), "%04X  ", base);
            hex += head;
        }
        hex.push_back(kHex[c >> 4]);
        hex.push_back(kHex[c & 0xF]);
        hex.push_back(((base % 16) == 7 || (base % 16) == 15) ? ' ' : ' ');
        ascii.push_back(c >= 0x20 && c < 0x7f ? (char)c : '.');
        if (base % 16 == 15) ascii.push_back('\n');
    }

    // 逐 qword 解码，方便一眼看出指针 / 小整数
    nlohmann::json qwords = nlohmann::json::array();
    for (int i = 0; i + 8 <= length; i += 8) {
        uint64_t v = 0;
        SafeReadU64((const void*)((uintptr_t)obj + offset + i), &v);
        char label[16]{};
        snprintf(label, sizeof(label), "%04X", offset + i);
        qwords.push_back({{"at", label}, {"u64", v}, {"lo32", (uint32_t)(v & 0xFFFFFFFF)}, {"hi32", (uint32_t)(v >> 32)}});
    }

    return {
        {"success", true},
        {"contact_id", contact_id},
        {"ptr", (uint64_t)(uintptr_t)obj},
        {"offset", offset},
        {"length", length},
        {"hex", hex},
        {"ascii", ascii},
        {"qwords", qwords}
    };
}
// 派特使：CAssignLeaderCommand + CLeaderLocation{type 8 首次接触, assignment 4 first_contact,
// id = 接触记录 id}。实测（2026-11-19）挂在虚空虫那条上的特使 62 读出来正是
// {type 8, assignment 4, id 4, position 0}，而那条接触的 id 就是 4 —— 不是国家 id。
nlohmann::json ContactsManager::AssignEnvoyJson(uint32_t leader_id, uint32_t contact_id, bool dry_run, bool reset_cooldown) {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    if (!GetPlayerCountry()) return { {"error", "No player country (not in game?)"} };

    void* db = nullptr;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CFirstContact_pDatabase), &db) || !db) {
        return { {"error", "CFirstContact database pointer not readable"} };
    }
    void* slots = nullptr;
    uint32_t cap = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)db + 0x18), &slots) || !slots ||
        !SafeReadU32((const void*)((uintptr_t)db + 0x20), &cap) || cap == 0 || cap > 65536) {
        return { {"error", "CFirstContact database has no plausible slot array"} };
    }

    void* contact = nullptr;
    for (uint32_t i = 0; i < cap && !contact; ++i) {
        void* obj = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)slots + (size_t)i * 16 + 8), &obj) || !obj) continue;
        uint32_t id = 0xFFFFFFFF;
        if (SafeReadU32((const void*)((uintptr_t)obj + sdk::rt::CFirstContact_id), &id) && id == contact_id) contact = obj;
    }
    if (!contact) return { {"success", false}, {"error", "First contact id not found: " + std::to_string(contact_id)} };

    namespace ac = sdk::cmd::assign_leader_command;

    // 命令要的是「带高位标记的 leader 引用」，不是裸 id：本档特使 62 的引用是 0x0900003E
    // （150995006），只传 62 时引擎会接受命令却把人从原来的位置上撤掉、不派到新位置。
    // 所以引用从 leader 对象自己的 id 字段（CLeader_id = +0x20）读。
    uint32_t leader_ref = leader_id;
    {
        void* ldb = nullptr;
        void* lslots = nullptr;
        uint32_t lcap = 0;
        if (SafeReadPtr((const void*)(base_address_ + sdk::glob::TPdxRef_CLeader_pDatabase), &ldb) && ldb &&
            SafeReadPtr((const void*)((uintptr_t)ldb + 0x18), &lslots) && lslots &&
            SafeReadU32((const void*)((uintptr_t)ldb + 0x20), &lcap) && leader_id < lcap) {
            void* lobj = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)lslots + (size_t)leader_id * 16 + 8), &lobj) && lobj) {
                uint32_t ref = 0;
                if (SafeReadU32((const void*)((uintptr_t)lobj + sdk::rt::CLeader_id), &ref) && ref != 0xFFFFFFFF) {
                    leader_ref = ref;
                }
            }
        }
    }

    auto cmd = CommandBuilder::Get().Create(ac::kSpec);
    cmd.Set<uint32_t>(ac::leader, leader_ref);
    cmd.Set<uint32_t>(ac::location + 0x0, 8u);            // ELeaderLocationType: 8 = first contact
    cmd.Set<uint32_t>(ac::location + 0x4, 4u);            // EEnvoyAssignment: 4 = first_contact
    cmd.Set<uint32_t>(ac::location + 0x8, contact_id);    // target = the first contact record
    cmd.Set<uint32_t>(ac::location + 0xC, 0);             // position
    if (reset_cooldown) cmd.Set<uint8_t>(ac::reset_cooldown, 1);  // 跳过「360 内无法移动」的冷却

    std::string why;
    const bool can = cmd.IsValid(&why);
    nlohmann::json out = {
        {"leader_id", leader_id},
        {"leader_ref", leader_ref},
        {"contact_id", contact_id},
        {"location", {{"type", 8}, {"assignment", 4}, {"id", contact_id}, {"position", 0}}},
        {"can_assign", can}
    };
    if (!can) {
        out["success"] = false;
        out["reason"] = AlertManager::Get().CleanPdxString(why);
        return out;
    }
    if (dry_run) {
        out["success"] = true;
        out["posted"] = false;
        return out;
    }
    if (!cmd.Post()) {
        out["success"] = false;
        out["error"] = AlertManager::Get().CleanPdxString(cmd.error());
        return out;
    }
    out["success"] = true;
    out["posted"] = true;
    return out;
}

// 读 hook 抓到的 CAgreement：owner / target / 日期 / active_status，再扫一遍对象里的
// 字符串指针，把协议里的键（preset_* / subject_* / specialist_* …）全列出来。
// term_data 的内部布局还没定，先用「两份不同协议对比」把键和偏移对上。
nlohmann::json ContactsManager::GetAgreementsJson() {
    auto& cb = CommandBuilder::Get();
    const int n = cb.CapturedAgreementCount();
    nlohmann::json out;
    if (n <= 0) {
        out["captured"] = false;
        out["count"] = 0;
        out["hint"] = "还没抓到：先 install_agreement_capture，再存一次档（save_game 即可；读档也会走序列化）";
        return out;
    }
    nlohmann::json list = nlohmann::json::array();
    for (int i = 0; i < n; ++i) {
        const uintptr_t p = (uintptr_t)cb.CapturedAgreement(i);
        if (!p) continue;
        uint32_t owner = 0xFFFFFFFFu, target = 0xFFFFFFFFu, da = 0, dc = 0, act = 0;
        // hook 抓到的是 CAgreement 里那个被序列化的子对象（外层对象 +0x10），
        // 所以字段相对 this 依次提前 0x10：owner +0x08 / target +0x0C / 日期 +0x10,+0x14 / 状态 +0x18
        SafeReadU32((const void*)(p + 0x08), &owner);
        SafeReadU32((const void*)(p + 0x0C), &target);
        SafeReadU32((const void*)(p + 0x10), &da);
        SafeReadU32((const void*)(p + 0x14), &dc);
        SafeReadU32((const void*)(p + 0x18), &act);
        nlohmann::json terms = nlohmann::json::array();
        nlohmann::json resource_terms = nlohmann::json::array();
        std::string preset;
        // agreement_preset 在 +0xC8，指向 CAgreementPreset（键是它 +0x20 的 PdxString）；
        // forced_initial_loyalty 在 +0xF0（i64 ×1e5，实测 preset_vassal_oppressive_ai = -10000000）
        uint64_t preset_obj = 0;
        int64_t forced = 0;
        if (SafeReadU64((const void*)(p + 0xC8), &preset_obj) && preset_obj > 0x10000ULL) {
            SafeReadPdxString((const void*)(uintptr_t)(preset_obj + 0x20), preset);
        }
        SafeReadU64((const void*)(p + 0xF0), (uint64_t*)&forced);
        // 补贴：协议对象里两套 26 格数组（数据在 +0x90 / +0xB0，条数在 +0x98 / +0xB8 的低 32 位），
        // 每格 u64 ×1e5。实测归类（2026-11-21 两次改条款对照）：
        //   0–2 基础（能源/矿物/食物）｜3–5 研究（物理/社会/工程）｜9–10 进阶（消费品/合金）
        //   6–8 战略、11 凝聚力 未单独验过
        uint64_t slots[26] = {};
        for (int k = 0; k < 2; ++k) {
            const uintptr_t data_off = (k == 0) ? 0x90 : 0xB0;
            const uintptr_t cnt_off = (k == 0) ? 0x98 : 0xB8;
            uint64_t data = 0;
            uint32_t cnt = 0;
            if (!SafeReadU64((const void*)(p + data_off), &data) || data < 0x10000ULL) continue;
            if (!SafeReadU32((const void*)(p + cnt_off), &cnt) || cnt == 0 || cnt > 64) continue;
            for (uint32_t i = 0; i < 26 && i < cnt; ++i) {
                uint64_t v = 0;
                if (!SafeReadU64((const void*)(uintptr_t)(data + i * 8), &v)) break;
                if (v > slots[i]) slots[i] = v;
            }
        }
        auto slot_max = [&](int from, int to) -> double {
            uint64_t m = 0;
            for (int i = from; i <= to; ++i) if (slots[i] > m) m = slots[i];
            return (double)m / 100000.0;
        };
        nlohmann::json subsidies = {
            {"basic", slot_max(0, 2)},
            {"research", slot_max(3, 5)},
            {"strategic", slot_max(11, 13)},
            {"advanced", slot_max(9, 10)},
            {"unity", slots[14] ? (double)slots[14] / 100000.0 : 0.0}
        };
        // 条款：+0x60 的数组是 [key_ptr, value_ptr] 交替（离散条款，9 条），
        // +0x78 的数组是 [key_ptr, i64 定点 ×1e5]（资源条款，5 条）；条数在 +0x68 / +0x80 的高 32 位。
        // key 是对象 +0x20 的 PdxString。实测：subject_integration → subject_can_not_be_integrated、
        // joins_overlord_wars → joins_overlord_wars_offensive、joins_subject_wars → …defensive。
        {
            uint64_t disc = 0, disc_n = 0, res = 0, res_n = 0;
            if (SafeReadU64((const void*)(p + 0x60), &disc) && disc > 0x10000ULL &&
                SafeReadU64((const void*)(p + 0x68), &disc_n)) {
                uint32_t n = (uint32_t)(disc_n >> 32);
                if (n > 32) n = 32;
                for (uint32_t i = 0; i < n; ++i) {
                    uint64_t kp = 0, vp = 0;
                    if (!SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10), &kp)) break;
                    SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10 + 8), &vp);
                    std::string k, v;
                    if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                    if (vp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(vp + 0x20), v);
                    if (!k.empty()) terms.push_back({{"key", k}, {"value", v}});
                }
            }
            if (SafeReadU64((const void*)(p + 0x78), &res) && res > 0x10000ULL &&
                SafeReadU64((const void*)(p + 0x80), &res_n)) {
                uint32_t n = (uint32_t)(res_n >> 32);
                if (n > 16) n = 16;
                for (uint32_t i = 0; i < n; ++i) {
                    uint64_t kp = 0, v = 0;
                    if (!SafeReadU64((const void*)(uintptr_t)(res + i * 0x10), &kp)) break;
                    SafeReadU64((const void*)(uintptr_t)(res + i * 0x10 + 8), &v);
                    std::string k;
                    if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                    if (!k.empty()) {
                        // i64 定点 ×1e5；宗主给附庸时内存里是负数（存档同样写 -0.15），这里取绝对值
                        double pct = (double)(int64_t)v / 100000.0;
                        if (pct < 0) pct = -pct;
                        resource_terms.push_back({{"key", k}, {"value", pct}});
                    }
                }
            }
        }
        list.push_back({
            {"ptr", (unsigned long long)p},
            {"owner_ref", owner},
            {"target_ref", target},
            {"owner_id", (int)(owner & 0xFFFFFF)},
            {"target_id", (int)(target & 0xFFFFFF)},
            {"date_added", DecodeGameDate(da)},
            {"date_changed", DecodeGameDate(dc)},
            {"active_status", act},
            {"agreement_preset", preset},
            {"forced_initial_loyalty", (double)(int64_t)forced / 100000.0},
            {"subsidies", subsidies},
            {"terms", terms},
            {"resource_terms", resource_terms}
        });
    }
    out["captured"] = true;
    out["count"] = (int)list.size();
    out["agreements"] = list;
    return out;
}


// —— 只读/分步探针：造协议动作 → 报告 →（可选）写条款 →（可选）删除 ——
// 2026-11-21 两次静默崩溃（一次真投、一次 dry_run）之后用来二分定位：
//   step 1 只造不写不删｜step 2 写但不删｜step 3 写并删
// 每一步都只回 JSON，不投递。
nlohmann::json ContactsManager::ProbeAgreementAction(uint32_t country_id, const std::string& action_key,
                                                     const nlohmann::json& terms, const nlohmann::json& subsidies,
                                                     bool do_write, bool do_delete) {
    if (!GetPlayerCountry()) return { {"error", "No player country (not in game?)"} };
    void* type = nullptr;
    for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CDiplomaticActionTypeDatabase_pInstance)) {
        std::string key;
        if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), key) && key == action_key) type = t;
    }
    if (!type) return { {"success", false}, {"error", "Unknown diplomatic action " + action_key} };
    bool shown = false;
    void* action = NewAction(type, country_id, &shown);
    if (!action) return { {"success", false}, {"error", "producer made no action"} };
    nlohmann::json out;
    out["action_ptr"] = (unsigned long long)(uintptr_t)action;
    out["shown"] = shown;
    uint64_t vt = 0;
    if (SafeReadPtr(action, (void**)&vt) && vt > base_address_) out["vtable_rva"] = vt - base_address_;
    uint64_t disc = 0, disc_meta = 0, res = 0, res_meta = 0;
    if (SafeReadU64((const void*)((uintptr_t)action + 0xC8), &disc)) out["disc_ptr"] = (unsigned long long)disc;
    if (SafeReadU64((const void*)((uintptr_t)action + 0xD0), &disc_meta)) {
        out["disc_cap"] = (uint32_t)(disc_meta & 0xFFFFFFFF);
        out["disc_size"] = (uint32_t)(disc_meta >> 32);
    }
    if (SafeReadU64((const void*)((uintptr_t)action + 0xE0), &res)) out["res_ptr"] = (unsigned long long)res;
    if (SafeReadU64((const void*)((uintptr_t)action + 0xE8), &res_meta)) {
        out["res_cap"] = (uint32_t)(res_meta & 0xFFFFFFFF);
        out["res_size"] = (uint32_t)(res_meta >> 32);
    }
    out["disc_writable"] = disc >= 0x10000ULL && WritableRange((const void*)disc, 0x10);
    out["res_writable"] = res >= 0x10000ULL && WritableRange((const void*)res, 0x10);
    // 读出当前的 14 / 5 条（只读）
    nlohmann::json cur = nlohmann::json::array();
    uint32_t dn = (uint32_t)(disc_meta >> 32);
    if (disc >= 0x10000ULL && dn > 0 && dn <= 64) {
        for (uint32_t i = 0; i < dn; ++i) {
            uint64_t kp = 0, vp = 0;
            if (!SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10), &kp)) break;
            SafeReadU64((const void*)(uintptr_t)(disc + i * 0x10 + 8), &vp);
            std::string k, v;
            if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
            if (vp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(vp + 0x20), v);
            if (!k.empty()) cur.push_back({{"key", k}, {"value", v}});
        }
    }
    out["terms_before"] = cur;
    {   // 条款库里都有哪些键（看 resource_subsidies_* 在不在这个库）
        nlohmann::json dbk = nlohmann::json::array();
        for (void* t : ScriptTypes(base_address_ + sdk::glob::TGameDatabase_CAgreementTermDatabase_pInstance)) {
            std::string k;
            if (SafeReadPdxString((const void*)((uintptr_t)t + kTypeKey), k) && !k.empty()) dbk.push_back(k);
            if (dbk.size() >= 200) break;
        }
        out["term_db_count"] = (int)dbk.size();
        out["term_db_keys"] = dbk;
    }
    if (do_write) {
        std::string terr;
        if (!InitAgreementInAction(action, base_address_, terr)) {
            out["write_error"] = terr;
            {
                uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                SafeReadU64((const void*)((uintptr_t)action + 0x58), &a0);
                SafeReadU64((const void*)((uintptr_t)action + 0x68), &a1);
                SafeReadU64((const void*)((uintptr_t)action + 0xB8), &a2);
                SafeReadU64((const void*)((uintptr_t)action + 0xC0), &a3);
                out["debug"] = { {"base", (unsigned long long)base_address_},
                                 {"agree_vt", (unsigned long long)a0}, {"agree_vt2", (unsigned long long)a1},
                                 {"b8", (unsigned long long)a2}, {"td_vt", (unsigned long long)a3} };
                if (a0 > base_address_) out["debug"]["agree_rva"] = (unsigned long long)(a0 - base_address_);
                if (a3 > base_address_) out["debug"]["td_rva"] = (unsigned long long)(a3 - base_address_);
            }
        } else if (!ApplyAgreementTerms(action, base_address_, terms, subsidies, terr)) {
            out["write_error"] = terr;
        } else {
            // 回读：term data +0x08 离散容器（data +0x08、条数 +0x14、0x10 步长）
            nlohmann::json rb = nlohmann::json::array();
            const uintptr_t td = (uintptr_t)action + 0xC0;
            uint64_t data = 0;
            uint32_t cnt = 0;
            SafeReadU64((const void*)(td + 0x08), &data);
            SafeReadU32((const void*)(td + 0x14), &cnt);
            if (data > 0x10000ULL && cnt > 0 && cnt <= 64) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    uint64_t kp = 0, vp = 0;
                    if (!SafeReadU64((const void*)(uintptr_t)(data + i * 0x10), &kp)) break;
                    SafeReadU64((const void*)(uintptr_t)(data + i * 0x10 + 8), &vp);
                    std::string k, v;
                    if (kp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(kp + 0x20), k);
                    if (vp > 0x10000ULL) SafeReadPdxString((const void*)(uintptr_t)(vp + 0x20), v);
                    if (!k.empty()) rb.push_back({{"key", k}, {"value", v}});
                }
            }
            out["written_count"] = cnt;
            out["written"] = rb;
        }
    }
    out["deleted"] = false;
    if (do_delete) {
        CommandBuilder::Get().CallGuarded(&CallDeleteAction, action);
        out["deleted"] = true;
    } else {
        out["note"] = "动作对象故意不删（泄漏一点内存），便于分步排查";
    }
    out["success"] = true;
    return out;
}

} // namespace bridge
