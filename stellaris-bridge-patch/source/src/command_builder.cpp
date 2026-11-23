#include "command_builder.hpp"
#include "commands.hpp"
#include "MinHook.h"

namespace bridge {

namespace {

using FnFactory = void* (*)();
using FnEngineAlloc = void* (*)(size_t);
using FnScalarDtor = void* (*)(void* self, unsigned flags);
using FnIsValid = bool (*)(void* self, void* reason_pdx_string);
using FnFreePdxString = void (*)(void* pdx_string);

constexpr size_t kSlotIsValid = 8;  // CCommand::IsValid(CString*) const

// MSVC std::string layout; command string payloads (sdk offsets) point at one of these.
struct RawPdxString {
    union {
        char buf[16];
        char* heap_ptr;
    };
    uint64_t size;
    uint64_t capacity;
};

// Engine CString (IsValid's reason out-param, localisation results): a 16-byte header followed
// by a std::string. Same layout as PdxLocResult in common.hpp.
struct RawCString {
    uint64_t header[2];
    RawPdxString str;
};
static_assert(sizeof(RawCString) == 0x30, "CString is 0x30 bytes");

// ---- pure virtual call guard -------------------------------------------------------------
// A pure virtual call makes the CRT call abort(), which SEH cannot catch. While the bridge is
// inside one of the SEH-guarded engine calls below, the hooked _purecall raises this exception
// instead, so a bad object turns into an error rather than a dead game.
constexpr DWORD kPureVirtualCallException = 0xE0505643;  // 'PVC'
thread_local int t_engine_call_depth = 0;
thread_local DWORD t_last_exception = 0;

using FnPurecall = void (*)();
FnPurecall g_original_purecall = nullptr;

void PurecallDetour() {
    if (t_engine_call_depth > 0) {
        RaiseException(kPureVirtualCallException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    g_original_purecall();
}

int RecordException(DWORD code) {
    t_last_exception = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string LastExceptionText() {
    if (t_last_exception == kPureVirtualCallException) {
        return "pure virtual call inside the engine (an object handed to it has a wrong vtable)";
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "exception 0x%08lX", (unsigned long)t_last_exception);
    return buf;
}

// SEH wrappers stay free of C++ objects with destructors (C2712). Each one marks the thread as
// being inside a guarded engine call for PurecallDetour.

bool ReadTokenGetter(uintptr_t vtable, uint32_t* out_token) {
    __try {
        const uint8_t* fn = *(const uint8_t**)(vtable + 10 * sizeof(void*));
        if (fn[0] != 0xB8 || fn[5] != 0xC3) {  // mov eax, imm32; ret
            return false;
        }
        *out_token = *(const uint32_t*)(fn + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* CallFactory(uintptr_t factory) {
    void* obj = nullptr;
    ++t_engine_call_depth;
    __try {
        obj = ((FnFactory)factory)();
    } __except (RecordException(GetExceptionCode())) {
        obj = nullptr;
    }
    --t_engine_call_depth;
    return obj;
}

bool CallPost(Commands::FnPostCommand post, void* cmd, bool force) {
    bool ok = true;
    ++t_engine_call_depth;
    __try {
        post(cmd, force);
    } __except (RecordException(GetExceptionCode())) {
        ok = false;
    }
    --t_engine_call_depth;
    return ok;
}

void CallDestroy(void* cmd) {
    ++t_engine_call_depth;
    __try {
        auto dtor = *(FnScalarDtor*)(*(uintptr_t*)cmd);  // vtable slot 0: scalar deleting dtor
        dtor(cmd, 1);
    } __except (RecordException(GetExceptionCode())) {
    }
    --t_engine_call_depth;
}

bool CallIsValid(void* cmd, void* reason, bool* out_ok) {
    bool ok = true;
    ++t_engine_call_depth;
    __try {
        auto fn = *(FnIsValid*)(*(uintptr_t*)cmd + kSlotIsValid * sizeof(void*));
        *out_ok = fn(cmd, reason);
    } __except (RecordException(GetExceptionCode())) {
        ok = false;
    }
    --t_engine_call_depth;
    return ok;
}

void CallFreePdxString(uintptr_t fn, void* str) {
    __try {
        ((FnFreePdxString)fn)(str);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool CallRawGuarded(uintptr_t fn) {
    bool ok = true;
    ++t_engine_call_depth;
    __try {
        ((void (*)())fn)();
    } __except (RecordException(GetExceptionCode())) {
        ok = false;
    }
    --t_engine_call_depth;
    return ok;
}

bool CallPredicate4(uintptr_t fn, void* a, void* b, void* c, void* d, bool* out) {
    bool ok = true;
    ++t_engine_call_depth;
    __try {
        *out = ((bool (*)(void*, void*, void*, void*))fn)(a, b, c, d);
    } __except (RecordException(GetExceptionCode())) {
        ok = false;
    }
    --t_engine_call_depth;
    return ok;
}

bool CallTextGuarded(CommandBuilder::FnTextCall call, void* ctx, void* out) {
    bool ok = true;
    ++t_engine_call_depth;
    __try {
        call(ctx, out);
    } __except (RecordException(GetExceptionCode())) {
        ok = false;
    }
    --t_engine_call_depth;
    return ok;
}

void* CallAlloc(uintptr_t fn, size_t n) {
    __try {
        return ((FnEngineAlloc)fn)(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string Hex(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

} // namespace

// ------------------------------------------------------------------ CommandBuilder

CommandBuilder& CommandBuilder::Get() {
    static CommandBuilder instance;
    return instance;
}

void CommandBuilder::Init(uintptr_t base_address) {
    base_ = base_address;
    auto dos = (PIMAGE_DOS_HEADER)base_;
    auto nt = (PIMAGE_NT_HEADERS)(base_ + dos->e_lfanew);
    exe_timestamp_ = nt->FileHeader.TimeDateStamp;
    sdk_matches_ = exe_timestamp_ == sdk::kExeTimestamp;
    if (sdk_matches_) {
        LOGF("[CMD_BUILDER] SDK matches stellaris.exe (TimeDateStamp 0x%08X)", exe_timestamp_);
    } else {
        LOGF("[CMD_BUILDER] SDK MISMATCH: exe TimeDateStamp 0x%08X, SDK dumped from 0x%08X. "
             "Commands are disabled; run tools/sdk_dumper/dump.py.", exe_timestamp_, sdk::kExeTimestamp);
    }
}

bool CommandBuilder::InstallGuards() {
    if (!base_ || !sdk_matches_) {
        return false;
    }
    void* target = (void*)(base_ + sdk::fn::CRT_purecall);
    if (MH_CreateHook(target, (LPVOID)&PurecallDetour, (LPVOID*)&g_original_purecall) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("[CMD_BUILDER] could not hook _purecall; pure virtual calls will still abort the game");
        return false;
    }
    LOGF("[CMD_BUILDER] pure virtual call guard installed on _purecall (0x%llX)", (unsigned long long)sdk::fn::CRT_purecall);
    return true;
}

// ---- PostCommand 旁听：抄下投递出去的外交提议动作对象 ----------------------------------------
// 参考上游 OGAS 的做法（docs/runtime/LONG_RUNNING_AI_COMMAND_CAPTURE_4_4_6.md）：不自己构造命令，
// 只挂 Post 旁听，等游戏自己把命令发出来。这里盯 CDiploActionProposeCommand ——
// 界面点「递交议和条件」时游戏会构造一个 COfferPeaceAction 放在命令 +0x20，我们从旁听里抄它的
// vtable RVA 与前 0x68 字节，就省掉了逆构造函数。
namespace {
uintptr_t g_capture_base = 0;
Commands::FnPostCommand g_capture_orig_post = nullptr;

struct ProposeCaptureSlot {
    uint32_t seq;
    unsigned long long cmd_ptr;
    unsigned int cmd_vtable_rva;
    unsigned long long action_ptr;
    unsigned int action_vtable_rva;
    unsigned int len;
    unsigned char bytes[0x200];   // 0x68 够求和；CDeclareWarAction 的 instigator 在 +0x88；2026-11-21 加宽到 0x200 看法律动作的尾巴
};
ProposeCaptureSlot g_propose_capture{};
volatile LONG g_propose_capture_seq = 0;

// 第二个槽：投票 / 应答 命令（联邦投票里「同意/反对」走的是 CDiploActionVoteCommand）
// 第二个槽：**任意**命令（2026-11-21 加）——原来只记投票/应答/提案三种 vtable，
// 联邦法律提案（CVoteForFederationLawAction 的发起命令）根本不在白名单里，永远录不到。
// 现在所有 Post 过的命令都记一份 vtable RVA + 头 0x40 字节，再用 vtable 去认。
struct AnyCmdSlot {
    uint32_t seq;
    unsigned long long cmd_ptr;
    unsigned int vtable_rva;
    unsigned int len;
    unsigned char bytes[0x40];
};
AnyCmdSlot g_any_cmd{};
volatile LONG g_any_cmd_seq = 0;

struct CmdCaptureSlot {
    uint32_t seq;
    unsigned long long cmd_ptr;
    unsigned int vtable_rva;
    unsigned int len;
    unsigned char bytes[0x40];
};
CmdCaptureSlot g_cmd_capture{};
volatile LONG g_cmd_capture_seq = 0;

void ProposeCaptureDetour(void* cmd, bool force) {
    __try {
        if (cmd && g_capture_base) {
            void* vt = *(void**)cmd;
            // 先无条件记一份（认命令用；投票/应答那两种仍另外记）
            {
                const LONG an = InterlockedIncrement(&g_any_cmd_seq);
                g_any_cmd.seq = (uint32_t)an;
                g_any_cmd.cmd_ptr = (unsigned long long)(uintptr_t)cmd;
                g_any_cmd.vtable_rva = (unsigned int)((uintptr_t)vt - g_capture_base);
                g_any_cmd.len = (unsigned int)sizeof(g_any_cmd.bytes);
                memcpy(g_any_cmd.bytes, cmd, sizeof(g_any_cmd.bytes));
            }
            const uintptr_t want = g_capture_base + sdk::cmd::diplo_action_propose_command::kVtableRva;
            const uintptr_t vt_vote = g_capture_base + sdk::cmd::diplo_action_vote_command::kVtableRva;
            const uintptr_t vt_respond = g_capture_base + sdk::cmd::diplo_action_respond_command::kVtableRva;
            if ((uintptr_t)vt == want) {
                void* action = *(void**)((uintptr_t)cmd + 0x20);
                if (action) {
                    void* avt = *(void**)action;
                    const LONG n = InterlockedIncrement(&g_propose_capture_seq);
                    g_propose_capture.seq = (uint32_t)n;
                    g_propose_capture.cmd_ptr = (unsigned long long)(uintptr_t)cmd;
                    g_propose_capture.cmd_vtable_rva = (unsigned int)((uintptr_t)vt - g_capture_base);
                    g_propose_capture.action_ptr = (unsigned long long)(uintptr_t)action;
                    g_propose_capture.action_vtable_rva = avt ? (unsigned int)((uintptr_t)avt - g_capture_base) : 0;
                    memcpy(g_propose_capture.bytes, action, sizeof(g_propose_capture.bytes));
                    g_propose_capture.len = (unsigned int)sizeof(g_propose_capture.bytes);
                    LOGF("[CAPTURE] propose action 0x%llX vtable_rva 0x%X (seq %d)",
                         g_propose_capture.action_ptr, g_propose_capture.action_vtable_rva, (int)n);
                }
            } else if ((uintptr_t)vt == vt_vote || (uintptr_t)vt == vt_respond) {
                const LONG n = InterlockedIncrement(&g_cmd_capture_seq);
                g_cmd_capture.seq = (uint32_t)n;
                g_cmd_capture.cmd_ptr = (unsigned long long)(uintptr_t)cmd;
                g_cmd_capture.vtable_rva = (unsigned int)((uintptr_t)vt - g_capture_base);
                g_cmd_capture.len = (unsigned int)sizeof(g_cmd_capture.bytes);
                memcpy(g_cmd_capture.bytes, cmd, sizeof(g_cmd_capture.bytes));
                LOGF("[CAPTURE] vote/respond cmd 0x%llX vtable_rva 0x%X (seq %d)",
                     g_cmd_capture.cmd_ptr, g_cmd_capture.vtable_rva, (int)n);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_capture_orig_post(cmd, force);
}
}  // namespace

bool CommandBuilder::InstallProposeCapture() {
    if (!base_ || !sdk_matches_) {
        LOG("[CAPTURE] SDK mismatch or no base; capture not installed");
        return false;
    }
    auto post = Commands::Get().GetPostCommand();
    if (!post) {
        LOG("[CAPTURE] PostCommand not resolved; capture not installed");
        return false;
    }
    g_capture_base = base_;
    if (MH_CreateHook((LPVOID)post, (LPVOID)&ProposeCaptureDetour, (LPVOID*)&g_capture_orig_post) != MH_OK ||
        MH_EnableHook((LPVOID)post) != MH_OK) {
        LOG("[CAPTURE] could not hook PostCommand");
        return false;
    }
    LOGF("[CAPTURE] PostCommand hooked at 0x%llX", (unsigned long long)(uintptr_t)post);
    return true;
}

// __try 不能进带对象展开的函数（C2712），所以这两个裸读单独放文件级。
static bool CapDeref(uintptr_t addr, uintptr_t* out) {
    __try {
        *out = *(volatile uintptr_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool CapStr(uintptr_t addr, char* buf, size_t n) {
    __try {
        size_t i = 0;
        for (; i + 1 < n; ++i) {
            buf[i] = *(volatile char*)(addr + i);
            if (!buf[i]) return true;
        }
        buf[n - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

nlohmann::json CommandBuilder::ProposeCaptureJson() {
    if (!g_propose_capture_seq) {
        return { {"success", true}, {"captured", false},
                 {"hint", "还没有捕获：请在战争窗口点一次「维持现状」→「递交议和条件」"} };
    }
    static const char* kHexDigits = "0123456789ABCDEF";
    std::string hex;
    hex.reserve((size_t)g_propose_capture.len * 3);
    for (unsigned i = 0; i < g_propose_capture.len; ++i) {
        unsigned char b = g_propose_capture.bytes[i];
        hex += kHexDigits[b >> 4];
        hex += kHexDigits[b & 0xF];
        hex += ' ';
    }
    // qword 表：按 8 字节对齐，方便对账（手工数 hex 太容易错位）
    nlohmann::json qwords = nlohmann::json::array();
    for (unsigned o = 0; o + 8 <= g_propose_capture.len; o += 8) {
        uint64_t v = 0;
        memcpy(&v, g_propose_capture.bytes + o, 8);
        char buf[24];
        std::snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)v);
        qwords.push_back({ {"offset", o}, {"value", buf} });
    }
    // 法律对象探测：逐个字节偏移试 u64，看它指不指向 CFederationLaw（vtable RVA 0x2388C28，键在 +0x20）。
    // 这样即使对象里有 UTF-16 字符串把 8 字节网格错开，也能把 law 字段揪出来。
    nlohmann::json law_hits = nlohmann::json::array();
    if (g_capture_base) {
        const unsigned long long kLawVtRva = 0x2388C28ULL;
        for (unsigned o = 0; o + 8 <= g_propose_capture.len; ++o) {
            uint64_t v = 0;
            memcpy(&v, g_propose_capture.bytes + o, 8);
            if (v < 0x10000ull || v > 0x7FFF00000000ull || (v & 7)) continue;
            uintptr_t vt = 0;
            if (!CapDeref((uintptr_t)v, &vt) || !vt) continue;
            if ((unsigned long long)(vt - g_capture_base) != kLawVtRva) continue;
            uintptr_t keyp = 0;
            char kb[64] = {0};
            if (!CapDeref((uintptr_t)v + 0x20, &keyp) || !keyp) continue;
            CapStr(keyp, kb, sizeof kb);
            law_hits.push_back({ {"offset", o}, {"obj", (unsigned long long)v}, {"key", std::string(kb)} });
        }
    }
    return { {"success", true}, {"captured", true},
             {"seq", g_propose_capture.seq},
             {"cmd_ptr", g_propose_capture.cmd_ptr},
             {"cmd_vtable_rva", g_propose_capture.cmd_vtable_rva},
             {"action_ptr", g_propose_capture.action_ptr},
             {"action_vtable_rva", g_propose_capture.action_vtable_rva},
             {"len", g_propose_capture.len},
             {"qwords", qwords},
             {"law_hits", law_hits},
             {"hex", hex} };
}

void CommandBuilder::ClearProposeCapture() {
    InterlockedExchange(&g_propose_capture_seq, 0);
    memset(&g_propose_capture, 0, sizeof(g_propose_capture));
}

// 投票 / 应答命令的捕获（联邦投票的「同意」就是 CDiploActionVoteCommand）
nlohmann::json CommandBuilder::CmdCaptureJson() {
    static const char* kHexDigits = "0123456789ABCDEF";
    auto hex_of = [](const unsigned char* bytes, unsigned len) {
        std::string hex;
        for (unsigned i = 0; i < len; ++i) {
            unsigned char b = bytes[i];
            hex += kHexDigits[b >> 4];
            hex += kHexDigits[b & 0xF];
            hex += ' ';
        }
        return hex;
    };
    nlohmann::json out = { {"success", true} };
    if (g_cmd_capture_seq) {
        out["captured"] = true;
        out["seq"] = g_cmd_capture.seq;
        out["cmd_ptr"] = g_cmd_capture.cmd_ptr;
        out["vtable_rva"] = g_cmd_capture.vtable_rva;
        out["len"] = g_cmd_capture.len;
        out["hex"] = hex_of(g_cmd_capture.bytes, g_cmd_capture.len);
    } else {
        out["captured"] = false;
    }
    if (g_any_cmd_seq) {
        out["any"] = { {"seq", g_any_cmd.seq},
                       {"cmd_ptr", g_any_cmd.cmd_ptr},
                       {"vtable_rva", g_any_cmd.vtable_rva},
                       {"len", g_any_cmd.len},
                       {"hex", hex_of(g_any_cmd.bytes, g_any_cmd.len)} };
    }
    if (!g_cmd_capture_seq && !g_any_cmd_seq) {
        out["hint"] = "还没捕获：请打开通知里的投票页点一次「同意」，或发起一次法律提案";
    }
    return out;
}

void CommandBuilder::ClearCmdCapture() {
    InterlockedExchange(&g_cmd_capture_seq, 0);
    memset(&g_cmd_capture, 0, sizeof(g_cmd_capture));
    InterlockedExchange(&g_any_cmd_seq, 0);
    memset(&g_any_cmd, 0, sizeof(g_any_cmd));
}

// ---- 第二个旁听：COfferPeaceAction 的模板→新动作函数（RVA 0x9A8E80） -------------------------
// 那个函数只读 rcx：以入参为模板，分配 0x68、调基类初始化、写 vtable 0x23B6B20 与 +0x50 的
// tag 0x23B6CC8，再从入参 +0x58/+0x5C/+0x60/+0x64 抄 actor/recipient/war/status。
// 玩家的猜测：它可能是「选择议和方案」那一下触发的（先选方案、再递交）。挂上来看触发时机，
// 顺便把入参那份模板整个抄下来 —— 那是引擎填好的合法模板。
namespace {
using FnClonePeace = void* (*)(void*);
FnClonePeace g_clone_orig = nullptr;

struct CloneCaptureSlot {
    uint32_t seq;
    unsigned long long src_ptr;
    unsigned long long ret_ptr;
    unsigned int ret_vtable_rva;
    unsigned char src_bytes[0x68];
};
CloneCaptureSlot g_clone_capture{};
volatile LONG g_clone_capture_seq = 0;

void* ClonePeaceDetour(void* src) {
    void* ret = nullptr;
    __try {
        g_clone_capture.src_ptr = (unsigned long long)(uintptr_t)src;
        if (src) {
            memcpy(g_clone_capture.src_bytes, src, sizeof(g_clone_capture.src_bytes));
        }
        ret = g_clone_orig(src);
        g_clone_capture.ret_ptr = (unsigned long long)(uintptr_t)ret;
        if (ret) {
            void* vt = *(void**)ret;
            g_clone_capture.ret_vtable_rva =
                (vt && g_capture_base) ? (unsigned int)((uintptr_t)vt - g_capture_base) : 0;
        }
        const LONG n = InterlockedIncrement(&g_clone_capture_seq);
        g_clone_capture.seq = (uint32_t)n;
        LOGF("[CAPTURE] peace-clone src 0x%llX -> 0x%llX (seq %d)",
             g_clone_capture.src_ptr, g_clone_capture.ret_ptr, (int)n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ret;
}
}  // namespace

// —— 抓 CGalacticCommunity 的 serializer（RVA 0x6249E0）——
// 存档 / 读档都会走它，入参 RCX = this。只记指针，不在 detour 里解任何字段。
static volatile LONG g_community_seq = 0;
static unsigned long long g_community_self = 0;
static void (*g_community_orig)(void*, void*) = nullptr;

static void CommunitySerializerDetour(void* self, void* serializer) {
    g_community_self = (unsigned long long)(uintptr_t)self;
    InterlockedIncrement(&g_community_seq);
    if (g_community_orig) g_community_orig(self, serializer);
}

bool CommandBuilder::InstallCommunitySerializerCapture() {
    if (!base_ || !sdk_matches_) {
        LOG("[CAPTURE] community: SDK mismatch or no base");
        return false;
    }
    const uintptr_t rva = 0x6249E0;   // CGalacticCommunity serializer（sdk.json: serializer 0x6249E0）
    void* target = (void*)(base_ + rva);
    if (MH_CreateHook(target, (LPVOID)&CommunitySerializerDetour, (LPVOID*)&g_community_orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("[CAPTURE] could not hook CGalacticCommunity serializer");
        return false;
    }
    LOGF("[CAPTURE] CGalacticCommunity serializer hooked at RVA 0x%llX", (unsigned long long)rva);
    return true;
}

unsigned long long CommandBuilder::CapturedCommunity() const {
    return g_community_self;
}

// —— 抓 CAgreement 的 serializer（RVA 0x7AA310）——
// 附庸协议也走存档/读档的序列化，入参 RCX = this。只记指针，不在 detour 里解任何字段。
// 实测（2026-11-21）：一次 save_game 会把当前所有协议各序列化一遍，所以存一次档就能拿全。
static const int kAgreementCaptureMax = 64;
static volatile LONG g_agreement_seq = 0;
static volatile LONG g_agreement_count = 0;
static unsigned long long g_agreement_ptrs[kAgreementCaptureMax] = {};
static void (*g_agreement_orig)(void*, void*) = nullptr;

static void AgreementSerializerDetour(void* self, void* serializer) {
    const unsigned long long p = (unsigned long long)(uintptr_t)self;
    const LONG n = g_agreement_count;
    bool seen = false;
    for (LONG i = 0; i < n && i < kAgreementCaptureMax; ++i) {
        if (g_agreement_ptrs[i] == p) { seen = true; break; }
    }
    if (!seen && n < kAgreementCaptureMax) {
        g_agreement_ptrs[n] = p;
        InterlockedIncrement(&g_agreement_count);
    }
    InterlockedIncrement(&g_agreement_seq);
    if (g_agreement_orig) g_agreement_orig(self, serializer);
}

bool CommandBuilder::InstallAgreementSerializerCapture() {
    if (!base_ || !sdk_matches_) {
        LOG("[CAPTURE] agreement: SDK mismatch or no base");
        return false;
    }
    const uintptr_t rva = 0x7AA310;   // CAgreement serializer（sdk.json: serializer 0x7AA310）
    void* target = (void*)(base_ + rva);
    if (MH_CreateHook(target, (LPVOID)&AgreementSerializerDetour, (LPVOID*)&g_agreement_orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("[CAPTURE] could not hook CAgreement serializer");
        return false;
    }
    LOGF("[CAPTURE] CAgreement serializer hooked at RVA 0x%llX", (unsigned long long)rva);
    return true;
}

int CommandBuilder::CapturedAgreementCount() const {
    const LONG n = g_agreement_count;
    return n < 0 ? 0 : (n > kAgreementCaptureMax ? kAgreementCaptureMax : (int)n);
}

unsigned long long CommandBuilder::CapturedAgreement(int index) const {
    if (index < 0 || index >= CapturedAgreementCount()) return 0;
    return g_agreement_ptrs[index];
}

void CommandBuilder::ClearAgreementCapture() {
    for (int i = 0; i < kAgreementCaptureMax; ++i) g_agreement_ptrs[i] = 0;
    InterlockedExchange(&g_agreement_count, 0);
    InterlockedExchange(&g_agreement_seq, 0);
}

// —— 抓 CTimedModifier 的 serializer（RVA 0x375FF0）——
// 定论依据：CTimedModifier { modifier +0x8 (ptr, 序列化成键名), days +0x10, clear_on_owner_change +0x20 }。
// detour 里只做「安全读 + 记指针」，每个条目记 self / type / days 与 type 对象的 6 个 qword，
// 之后由脚本侧和存档里那行 modifier="..." 对账。
static const int kTimedCap = 512;
static volatile LONG g_timed_count = 0;
static unsigned long long g_timed_self[kTimedCap] = {};
static unsigned long long g_timed_type[kTimedCap] = {};
static long long g_timed_days[kTimedCap] = {};
static unsigned long long g_timed_words[kTimedCap][6] = {};
static void (*g_timed_orig)(void*, void*) = nullptr;

static bool SafeReadQword(unsigned long long addr, unsigned long long* out) {
    __try {
        *out = *(volatile unsigned long long*)(uintptr_t)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void TimedModifierSerializerDetour(void* self, void* serializer) {
    const unsigned long long p = (unsigned long long)(uintptr_t)self;
    if (p > 0x10000) {
        const LONG n = g_timed_count;
        if (n < kTimedCap) {
            unsigned long long type_ptr = 0;
            unsigned long long days_raw = 0;
            if (SafeReadQword(p + 0x08, &type_ptr) && SafeReadQword(p + 0x10, &days_raw)) {
                bool seen = false;
                for (LONG i = 0; i < n && i < kTimedCap; ++i) {
                    if (g_timed_self[i] == p && g_timed_type[i] == type_ptr) { seen = true; break; }
                }
                if (!seen) {
                    g_timed_self[n] = p;
                    g_timed_type[n] = type_ptr;
                    g_timed_days[n] = (long long)(int32_t)(uint32_t)(days_raw & 0xFFFFFFFFull);
                    for (int w = 0; w < 6; ++w) {
                        unsigned long long v = 0;
                        SafeReadQword(type_ptr + (unsigned long long)w * 8, &v);
                        g_timed_words[n][w] = v;
                    }
                    InterlockedIncrement(&g_timed_count);
                }
            }
        }
    }
    if (g_timed_orig) g_timed_orig(self, serializer);
}

bool CommandBuilder::InstallTimedModifierCapture() {
    if (!base_ || !sdk_matches_) {
        LOG("[CAPTURE] timed modifier: SDK mismatch or no base");
        return false;
    }
    const uintptr_t rva = 0x375FF0;   // CTimedModifier serializer（sdk.json）
    void* target = (void*)(base_ + rva);
    if (MH_CreateHook(target, (LPVOID)&TimedModifierSerializerDetour, (LPVOID*)&g_timed_orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("[CAPTURE] could not hook CTimedModifier serializer");
        return false;
    }
    LOGF("[CAPTURE] CTimedModifier serializer hooked at RVA 0x%llX", (unsigned long long)rva);
    return true;
}

int CommandBuilder::CapturedTimedModifierCount() const {
    const LONG n = g_timed_count;
    return n < 0 ? 0 : (n > kTimedCap ? kTimedCap : (int)n);
}

unsigned long long CommandBuilder::CapturedTimedModifierSelf(int index) const {
    if (index < 0 || index >= CapturedTimedModifierCount()) return 0;
    return g_timed_self[index];
}

unsigned long long CommandBuilder::CapturedTimedModifierType(int index) const {
    if (index < 0 || index >= CapturedTimedModifierCount()) return 0;
    return g_timed_type[index];
}

long long CommandBuilder::CapturedTimedModifierDays(int index) const {
    if (index < 0 || index >= CapturedTimedModifierCount()) return 0;
    return g_timed_days[index];
}

unsigned long long CommandBuilder::CapturedTimedModifierWord(int index, int word) const {
    if (index < 0 || index >= CapturedTimedModifierCount() || word < 0 || word >= 6) return 0;
    return g_timed_words[index][word];
}

void CommandBuilder::ClearTimedModifierCapture() {
    for (int i = 0; i < kTimedCap; ++i) {
        g_timed_self[i] = 0;
        g_timed_type[i] = 0;
        g_timed_days[i] = 0;
        for (int w = 0; w < 6; ++w) g_timed_words[i][w] = 0;
    }
    InterlockedExchange(&g_timed_count, 0);
}

bool CommandBuilder::InstallCloneCapture() {
    if (!base_ || !sdk_matches_) {
        LOG("[CAPTURE] clone: SDK mismatch or no base");
        return false;
    }
    // 静态还原出来的地址：COfferPeaceAction 的「模板 → 新动作」函数（写 vtable 0x23B6B20 的那处 lea
    // 位于 RVA 0x9A8EAA，所属函数 0x9A8E80..0x9A8EE5）。
    const uintptr_t rva = 0x9A8E80;
    void* target = (void*)(base_ + rva);
    g_capture_base = base_;
    if (MH_CreateHook(target, (LPVOID)&ClonePeaceDetour, (LPVOID*)&g_clone_orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("[CAPTURE] could not hook peace-clone function");
        return false;
    }
    LOGF("[CAPTURE] peace-clone hooked at RVA 0x%llX (base 0x%llX)", (unsigned long long)rva, (unsigned long long)base_);
    return true;
}

nlohmann::json CommandBuilder::CloneCaptureJson() {
    if (!g_clone_capture_seq) {
        return { {"success", true}, {"captured", false},
                 {"hint", "还没触发：请只点一次「维持现状」（先别点递交），看这个函数会不会被调用"} };
    }
    static const char* kHexDigits = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(sizeof(g_clone_capture.src_bytes) * 3);
    for (unsigned i = 0; i < sizeof(g_clone_capture.src_bytes); ++i) {
        unsigned char b = g_clone_capture.src_bytes[i];
        hex += kHexDigits[b >> 4];
        hex += kHexDigits[b & 0xF];
        hex += ' ';
    }
    return { {"success", true}, {"captured", true},
             {"seq", g_clone_capture.seq},
             {"src_ptr", g_clone_capture.src_ptr},
             {"ret_ptr", g_clone_capture.ret_ptr},
             {"ret_vtable_rva", g_clone_capture.ret_vtable_rva},
             {"len", (unsigned)sizeof(g_clone_capture.src_bytes)},
             {"src_hex", hex} };
}

void CommandBuilder::ClearCloneCapture() {
    InterlockedExchange(&g_clone_capture_seq, 0);
    memset(&g_clone_capture, 0, sizeof(g_clone_capture));
}

bool CommandBuilder::HasPeaceTemplate() {
    return g_clone_capture_seq != 0;
}

bool CommandBuilder::CopyProposeCapture(unsigned char* out, size_t n) const {
    if (!g_propose_capture_seq) return false;
    const size_t len = sizeof(g_propose_capture.bytes);
    if (n > len) n = len;
    memcpy(out, g_propose_capture.bytes, n);
    return true;
}

unsigned long long CommandBuilder::ProposeCaptureActionVtableRva() const {
    return g_propose_capture_seq ? (unsigned long long)g_propose_capture.action_vtable_rva : 0ULL;
}

namespace {
// __try 不能出现在需要对象展开的函数里（C2712），所以把受保护调用单独放这儿。
void* CallCloneGuarded(FnClonePeace fn, void* arg) {
    __try {
        return fn(arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
}  // namespace

// 用模板造一个 COfferPeaceAction。
// 依据（2026-11-20 两次捕获对比）：status_quo 与 force_demands 两份模板只有 +0x64 一位不同，
// 其余 103 字节完全相同（含 +0x40 那个单例指针，两次读档都是 0x24736D2B960）。
// 所以覆写 +0x20（基类 actor）与 +0x58/+0x5C/+0x60/+0x64 四个 offer 字段即可。
void* CommandBuilder::BuildPeaceAction(uint32_t actor_ref, uint32_t recipient_ref, uint32_t war_ref, int status,
                                       bool synthetic, uint64_t f40, uint32_t f4c) {
    if (!g_clone_orig) {
        LOG("[CAPTURE] build: clone function not hooked");
        return nullptr;
    }
    unsigned char buf[0x68];
    if (synthetic) {
        memset(buf, 0, sizeof(buf));
    } else {
        if (!g_clone_capture_seq) {
            LOG("[CAPTURE] build: no template captured this session");
            return nullptr;
        }
        memcpy(buf, g_clone_capture.src_bytes, sizeof(buf));
    }
    // 直接用「内存里存着的 id 原值」—— 同一档里玩家是裸值、别国可能是 0x01000000|id，
    // 自己拼会拼错（2026-11-20 实测：灵能魔女那档 actor=1 裸、recipient=0x0100000A 带标记）。
    *(uint32_t*)(buf + 0x20) = actor_ref;
    *(uint32_t*)(buf + 0x24) = recipient_ref;   // 基类 recipient（界面递交时这里也是带标记的引用；
    //                                              2026-11-21 实测：不写它 → IsValid 回空原因、发不出去，
    //                                              因为旧用例的收件方恰好是 0，缺这个字段没暴露）
    *(uint32_t*)(buf + 0x58) = actor_ref;
    *(uint32_t*)(buf + 0x5C) = recipient_ref;
    *(uint32_t*)(buf + 0x60) = war_ref;
    *(uint32_t*)(buf + 0x64) = (uint32_t)status;
    if (synthetic && f40) *(uint64_t*)(buf + 0x40) = f40;
    if (synthetic && f4c) *(uint32_t*)(buf + 0x4C) = f4c;
    void* action = CallCloneGuarded(g_clone_orig, buf);
    if (!action) {
        LOG("[CAPTURE] build: clone call faulted or returned null");
        return nullptr;
    }
    LOGF("[CAPTURE] built peace action 0x%llX (actor 0x%X recipient 0x%X war 0x%X status %d)",
         (unsigned long long)(uintptr_t)action, actor_ref, recipient_ref, war_ref, status);
    return action;
}

nlohmann::json CommandBuilder::SelfTestPurecallGuard() {
    if (!g_original_purecall) {
        return { {"error", {{"code", -32090}, {"message", "guard not installed; refusing to call _purecall"}}}} ;
    }
    // Calls the engine's _purecall inside a guarded wrapper: with the guard working this raises
    // the bridge's exception and comes back; without it the CRT would abort the game.
    t_last_exception = 0;
    bool returned_normally = CallRawGuarded(base_ + sdk::fn::CRT_purecall);
    bool caught = !returned_normally && t_last_exception == kPureVirtualCallException;
    return { {"success", caught}, {"guard_caught_pure_virtual_call", caught}, {"detail", LastExceptionText()} };
}

void* CommandBuilder::EngineAlloc(size_t n) const {
    if (!base_ || !sdk_matches_) return nullptr;
    return CallAlloc(base_ + sdk::kRvaEngineAlloc, n);
}

NativeCommand CommandBuilder::Create(const sdk::CmdSpec& spec) {
    if (!base_) {
        return NativeCommand(&spec, nullptr, "CommandBuilder not initialized");
    }
    if (!sdk_matches_) {
        return NativeCommand(&spec, nullptr,
            "SDK was generated for a different stellaris.exe (exe " + Hex(exe_timestamp_) + ", SDK " +
            Hex(sdk::kExeTimestamp) + "); run tools/sdk_dumper/dump.py");
    }
    uintptr_t vtable = base_ + spec.vtable_rva;
    uint32_t token = 0;
    if (!ReadTokenGetter(vtable, &token) || token != spec.token) {
        return NativeCommand(&spec, nullptr,
            std::string(spec.name) + ": vtable " + Hex(spec.vtable_rva) + " does not report token " + Hex(spec.token));
    }
    void* obj = nullptr;
    if (spec.factory_rva) {
        obj = CallFactory(base_ + spec.factory_rva);
    } else if (spec.size >= 0x20) {
        // No engine factory (the UI builds this one on the stack). Reproduce what every
        // factory does for the CCommand header; payload fields start zeroed.
        obj = CallAlloc(base_ + sdk::kRvaEngineAlloc, spec.size);
        if (obj) {
            memset(obj, 0, spec.size);
            *(uintptr_t*)obj = vtable;
            *(uint32_t*)((uintptr_t)obj + 0x08) = 0xFFFFFFFF;
            *(uint32_t*)((uintptr_t)obj + 0x12) = 0xFFFF;
        }
    }
    if (!obj) {
        return NativeCommand(&spec, nullptr, std::string(spec.name) + ": engine factory failed (" + LastExceptionText() + ")");
    }
    if (*(uintptr_t*)obj != vtable) {
        // Not our object type; do not try to destroy something we do not understand.
        return NativeCommand(&spec, nullptr, std::string(spec.name) + ": factory returned an object with an unexpected vtable");
    }
    return NativeCommand(&spec, obj, {});
}

NativeCommand CommandBuilder::Adopt(const sdk::CmdSpec& spec, void* engine_object) {
    if (!engine_object) {
        return NativeCommand(&spec, nullptr, std::string(spec.name) + ": no command object to adopt");
    }
    if (!base_ || !sdk_matches_) {
        return NativeCommand(&spec, nullptr, "SDK was generated for a different stellaris.exe; run tools/sdk_dumper/dump.py");
    }
    uintptr_t vtable = base_ + spec.vtable_rva;
    uint32_t token = 0;
    if (*(uintptr_t*)engine_object != vtable || !ReadTokenGetter(vtable, &token) || token != spec.token) {
        // Not the expected type: leave it alone rather than destroy something unknown.
        return NativeCommand(&spec, nullptr, std::string(spec.name) + ": adopted object is not this command type");
    }
    return NativeCommand(&spec, engine_object, {});
}

// ------------------------------------------------------------------ NativeCommand

NativeCommand::NativeCommand(const sdk::CmdSpec* spec, void* obj, std::string error)
    : spec_(spec), obj_(obj), error_(std::move(error)) {
    if (!error_.empty()) {
        LOGF("[CMD_BUILDER] %s", error_.c_str());
    }
}

NativeCommand::NativeCommand(NativeCommand&& other) noexcept
    : spec_(other.spec_), obj_(other.obj_), error_(std::move(other.error_)) {
    other.obj_ = nullptr;
}

NativeCommand::~NativeCommand() {
    if (obj_) {
        CallDestroy(obj_);
    }
}

bool NativeCommand::CheckRange(std::ptrdiff_t off, size_t n) {
    if (!obj_ || !error_.empty()) {
        return false;
    }
    if (off < 0x20 || (size_t)off + n > spec_->size) {  // never touch the CCommand header
        error_ = std::string(spec_->name) + ": field write at " + Hex((uint64_t)off) + " outside payload (size " + Hex(spec_->size) + ")";
        LOGF("[CMD_BUILDER] %s", error_.c_str());
        return false;
    }
    return true;
}

NativeCommand& NativeCommand::SetBytes(std::ptrdiff_t off, const void* data, size_t n) {
    if (CheckRange(off, n)) {
        memcpy((void*)((uintptr_t)obj_ + off), data, n);
    }
    return *this;
}

NativeCommand& NativeCommand::SetString(std::ptrdiff_t off, const std::string& value) {
    if (!CheckRange(off, sizeof(RawPdxString))) {
        return *this;
    }
    auto* str = (RawPdxString*)((uintptr_t)obj_ + off);
    // Factories leave strings empty and inline (capacity 15), so nothing needs freeing here.
    if (value.size() <= 15) {
        memset(str->buf, 0, sizeof(str->buf));
        memcpy(str->buf, value.data(), value.size());
        str->capacity = 15;
    } else {
        if (value.size() >= 4096) {  // MSVC uses an over-aligned block with a header for big strings
            error_ = std::string(spec_->name) + ": string too long";
            return *this;
        }
        char* heap = (char*)CommandBuilder::Get().EngineAlloc(value.size() + 1);
        if (!heap) {
            error_ = std::string(spec_->name) + ": engine allocation for string failed";
            return *this;
        }
        memcpy(heap, value.data(), value.size());
        heap[value.size()] = '\0';
        str->heap_ptr = heap;
        str->capacity = value.size();
    }
    str->size = value.size();
    return *this;
}

// Copies an engine CString reason out (rich-text markup stripped) and frees its heap buffer.
static void TakeReasonText(RawCString& text, std::string* reason, uint64_t max_size = 4096) {
    if (reason && text.str.size > 0 && text.str.size < max_size) {
        const char* p = text.str.capacity > 15 ? text.str.heap_ptr : text.str.buf;
        if (p) {
            *reason = RenderPdxMarkup(p, (size_t)text.str.size);
        }
    }
    if (text.str.capacity > 15) {
        CallFreePdxString(CommandBuilder::Get().Base() + sdk::fn::CString_Free, &text);
    }
}

bool CommandBuilder::CallGuarded(FnTextCall call, void* ctx) {
    if (!base_ || !sdk_matches_) {
        return false;
    }
    if (!CallTextGuarded(call, ctx, nullptr)) {
        LOGF("[CMD_BUILDER] engine call failed: %s", LastExceptionText().c_str());
        return false;
    }
    return true;
}

static bool SafeReadPtr(const void* addr, void** out) {
    __try {
        *out = *(void* const*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CommandBuilder::RunConsoleCommand(const std::string& line, std::string* error) {
    if (!base_ || !sdk_matches_) {
        if (error) *error = "SDK does not match the running exe";
        return false;
    }
    if (line.empty() || line.size() >= 4096) {
        if (error) *error = "command line must be 1..4095 characters";
        return false;
    }
    void* console = nullptr;
    if (!SafeReadPtr((const void*)(base_ + sdk::glob::CConsole_pInstance), &console) || !console) {
        if (error) *error = "CConsole::_pInstance is null (console not created yet)";
        return false;
    }

    // Engine CString: 16-byte header (zero) + MSVC std::string. A long line lives in an engine
    // heap block, like command string payloads; RunCommandNow only reads it.
    RawCString cmd{};
    if (line.size() <= 15) {
        memcpy(cmd.str.buf, line.data(), line.size());
        cmd.str.capacity = 15;
    } else {
        char* heap = (char*)EngineAlloc(line.size() + 1);
        if (!heap) {
            if (error) *error = "engine allocation for the command line failed";
            return false;
        }
        memcpy(heap, line.data(), line.size());
        heap[line.size()] = '\0';
        cmd.str.heap_ptr = heap;
        cmd.str.capacity = line.size();
    }
    cmd.str.size = line.size();

    struct Ctx { uintptr_t fn; void* console; void* cmd; };
    Ctx ctx{ base_ + sdk::fn::CConsole_RunCommandNow, console, &cmd };
    const bool ok = CallTextGuarded([](void* c, void*) {
        auto* p = (Ctx*)c;
        ((void (*)(void*, const void*))p->fn)(p->console, p->cmd);
    }, &ctx, nullptr);
    if (cmd.str.capacity > 15) {
        CallFreePdxString(base_ + sdk::fn::CString_Free, &cmd);
    }
    if (!ok) {
        if (error) *error = "engine raised: " + LastExceptionText();
        LOGF("[CMD_BUILDER] console command failed: %s", LastExceptionText().c_str());
    }
    return ok;
}

bool CommandBuilder::CallForText(FnTextCall call, void* ctx, std::string* text) {
    if (!base_ || !sdk_matches_) {
        return false;
    }
    // The callee constructs the CString in place (return slot) or appends to it.
    RawCString out{};
    out.str.capacity = 15;
    if (!CallTextGuarded(call, ctx, &out)) {
        LOGF("[CMD_BUILDER] engine text call failed: %s", LastExceptionText().c_str());
        return false;
    }
    TakeReasonText(out, text, 1u << 20);
    return true;
}

bool CommandBuilder::CallPredicate(uintptr_t fn_rva, void* self, void* a, void* b, std::string* reason) {
    if (!base_ || !sdk_matches_) {
        if (reason) *reason = "SDK was generated for a different stellaris.exe; run tools/sdk_dumper/dump.py";
        return false;
    }
    RawCString text{};
    text.str.capacity = 15;
    bool result = false;
    if (!CallPredicate4(base_ + fn_rva, self, a, b, reason ? &text : nullptr, &result)) {
        if (reason) *reason = "engine check failed: " + LastExceptionText();
        return false;
    }
    TakeReasonText(text, result ? nullptr : reason);
    return result;
}

bool NativeCommand::IsValid(std::string* reason) {
    if (!obj_ || !error_.empty()) {
        if (reason) *reason = error_.empty() ? "command was not created" : error_;
        return false;
    }
    RawCString text{};
    text.str.capacity = 15;
    bool ok = false;
    if (!CallIsValid(obj_, &text, &ok)) {
        std::string what = std::string(spec_->name) + ": IsValid failed: " + LastExceptionText();
        if (reason) *reason = what;
        LOGF("[CMD_BUILDER] %s", what.c_str());
        return false;
    }
    TakeReasonText(text, ok ? nullptr : reason);
    return ok;
}

bool NativeCommand::Post(Check check) {
    if (!obj_ || !error_.empty()) {
        if (error_.empty()) error_ = "command was not created";
        return false;
    }
    auto post = Commands::Get().GetPostCommand();
    if (!post) {
        error_ = "PostCommand not resolved";
        return false;
    }
    if (check == Check::IsValid) {
        std::string why;
        if (!IsValid(&why)) {
            error_ = std::string(spec_->name) + " rejected by the engine" + (why.empty() ? "" : ": " + why);
            LOGF("[CMD_BUILDER] %s", error_.c_str());
            return false;  // still ours: the destructor frees it
        }
    }
    void* cmd = obj_;
    obj_ = nullptr;  // the engine owns it from here, even if PostCommand faults part-way
    const bool force = check == Check::None;
    if (!CallPost(post, cmd, force)) {
        error_ = std::string(spec_->name) + ": PostCommand failed: " + LastExceptionText();
        LOGF("[CMD_BUILDER] %s", error_.c_str());
        return false;
    }
    LOGF("[CMD_BUILDER] posted %s%s", spec_->name, force ? " (force)" : "");
    return true;
}

} // namespace bridge
