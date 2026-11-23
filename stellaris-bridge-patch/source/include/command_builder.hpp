#pragma once

#include "common.hpp"
#include "sdk/stellaris_sdk.hpp"

namespace bridge {

// A native command created by the engine's own factory (sdk::cmd::<name>::kSpec), so the
// allocation size, header and engine defaults always match the running build. Callers only
// write payload fields, using offsets from the generated SDK.
//
//   auto cmd = CommandBuilder::Get().Create(sdk::cmd::research_technology_command::kSpec);
//   cmd.Set<uint32_t>(sdk::cmd::research_technology_command::country, country_id)
//      .Set<void*>(sdk::cmd::research_technology_command::technology, tech);
//   if (!cmd.Post()) return error(cmd.error());
class NativeCommand {
public:
    NativeCommand(NativeCommand&& other) noexcept;
    NativeCommand(const NativeCommand&) = delete;
    NativeCommand& operator=(const NativeCommand&) = delete;
    NativeCommand& operator=(NativeCommand&&) = delete;
    ~NativeCommand();  // destroys the command if it was never handed to the engine

    explicit operator bool() const { return obj_ != nullptr && error_.empty(); }
    const std::string& error() const { return error_; }
    void* get() const { return obj_; }

    template <typename T>
    NativeCommand& Set(std::ptrdiff_t off, T value) {
        if (CheckRange(off, sizeof(T))) {
            *(T*)((uintptr_t)obj_ + off) = value;
        }
        return *this;
    }

    // Copies a block (e.g. a POD sub-struct such as SSpeciesRights) into the payload.
    NativeCommand& SetBytes(std::ptrdiff_t off, const void* data, size_t n);

    // Writes a PdxString (MSVC std::string layout: inline buf / heap ptr [16], size, capacity).
    NativeCommand& SetString(std::ptrdiff_t off, const std::string& value);

    // Runs the command's own IsValid(CString* reason) (vtable slot 8) without dispatching it.
    // On failure *reason receives the engine's explanation when it provides one. Main thread only.
    bool IsValid(std::string* reason = nullptr);

    // Hands the command to the engine (PostCommand takes ownership). Main thread only.
    enum class Check {
        // Run IsValid first and fail with the engine's reason instead of letting PostCommand's
        // gate drop the command silently. What the UI does before enabling an action.
        IsValid,
        // Skip IsValid but keep PostCommand's own gate (force=false, as 330 of 332 engine call
        // sites do). For commands that only become valid after an earlier queued command runs.
        EngineGate,
        // PostCommand(force=true): no checks at all. Only when a check is known to be wrong.
        None,
    };
    bool Post(Check check = Check::IsValid);

    // Gives the object up without destroying it, for when something it embeds is no longer in a
    // destructible state (a rebuild of an embedded engine object faulted). Leaks the allocation.
    void Abandon(const std::string& why) {
        obj_ = nullptr;
        error_ = why;
    }

private:
    friend class CommandBuilder;
    NativeCommand(const sdk::CmdSpec* spec, void* obj, std::string error);
    bool CheckRange(std::ptrdiff_t off, size_t n);

    const sdk::CmdSpec* spec_;
    void* obj_;
    std::string error_;
};

class CommandBuilder {
public:
    static CommandBuilder& Get();

    void Init(uintptr_t base_address);

    // Hooks the engine's _purecall so a pure virtual call inside a guarded engine call becomes
    // an error instead of abort(). Needs MinHook initialized (after HookManager::Init).
    bool InstallGuards();
    // 旁听（只读）：把引擎的 PostCommand 挂上，凡是**投递出去的外交提议命令**
    // （diplo_action_propose_command）就把 +0x20 那个动作对象抄一份 —— vtable RVA 与前 0x68 字节。
    // 用途：界面点「递交议和条件」时是游戏自己构造 COfferPeaceAction，我们从旁听里拿它的形状，
    // 不必逆构造函数（上游 OGAS 就是这么拿到 set_defender_war_goal 的：挂 Post 旁听）。
    bool InstallProposeCapture();
    nlohmann::json ProposeCaptureJson();
    void ClearProposeCapture();
    // 投票 / 应答命令的捕获（联邦投票的「同意」）
    nlohmann::json CmdCaptureJson();
    void ClearCmdCapture();
    // 第二个旁听：COfferPeaceAction 的「模板 → 新动作」那个小函数（RVA 0x9A8E80，只收一个入参）。
    // 目的：确认它是被「选择议和方案」触发还是被「递交」触发；trigger 时把入参那份 0x68 模板
    // 整个抄下来 —— 那是引擎自己填好的、基类字段非零的合法模板。
    bool InstallCloneCapture();
    nlohmann::json CloneCaptureJson();
    void ClearCloneCapture();
    // 抓 CGalacticCommunity 的 serializer（RVA 0x6249E0）：存档/读档都会走它，RCX = this。
    // 只记指针，不碰对象；抓到之后共同体所有字段都能直接读（成员/议会/在台/倒计时）。
    bool InstallCommunitySerializerCapture();
    unsigned long long CapturedCommunity() const;
    // 抓 CAgreement 的 serializer（RVA 0x7AA310）：附庸协议走同一条序列化路径，
    // 一次 save_game 会把当前所有协议各过一遍。只记指针（最多 64 条），不碰对象。
    bool InstallAgreementSerializerCapture();
    // 抓 CTimedModifier 的 serializer（RVA 0x375FF0）：存档时每个定时修正各走一次，
    // 于是「存一次档」就能把当前所有星球的定时修正连同它的 modifier 指针一起收齐。
    bool InstallTimedModifierCapture();
    int CapturedTimedModifierCount() const;
    unsigned long long CapturedTimedModifierSelf(int index) const;
    unsigned long long CapturedTimedModifierType(int index) const;
    long long CapturedTimedModifierDays(int index) const;
    unsigned long long CapturedTimedModifierWord(int index, int word) const;
    void ClearTimedModifierCapture();
    int CapturedAgreementCount() const;
    unsigned long long CapturedAgreement(int index) const;
    void ClearAgreementCapture();
    // 用捕获到的模板造一个 COfferPeaceAction：抄 104 字节模板 → 覆写 +0x20/+0x58/+0x5C/+0x60/+0x64
    // → 调那个「模板→新动作」函数（RVA 0x9A8E80，引擎自己的初始化路径，含基类初始化与引用计数）。
    // 失败（没捕获过 / 钩子没装）返回 nullptr。只在主线程、且模板是本会话捕获时有效。
    // 用捕获到的模板造一个 COfferPeaceAction：抄 104 字节模板 → 覆写 +0x20/+0x58/+0x5C/+0x60/+0x64
    // → 调那个「模板→新动作」函数（RVA 0x9A8E80）。synthetic=true 时不用模板，从全零起步（消融用）。
    // actor_ref / recipient_ref 传**国家在内存里存的 id 原值**（裸或 0x01000000|id 都可能，实测同一档里
    // 两者混用：玩家存裸值、别国存带标记的），不要自己拼。
    void* BuildPeaceAction(uint32_t actor_ref, uint32_t recipient_ref, uint32_t war_ref, int status,
                           bool synthetic, uint64_t f40 = 0, uint32_t f4c = 0);
    bool HasPeaceTemplate();
    // propose 旁听抄下来的动作对象（模板）。宣战要用它：CDeclareWarAction 的构造器还没解，
    // 只能抄界面的那一份再改字段。n 最多 0x100（缓冲大小）。
    bool CopyProposeCapture(unsigned char* out, size_t n) const;
    unsigned long long ProposeCaptureActionVtableRva() const;
    // Deliberately triggers a pure virtual call inside a guarded call; only via IPC self_test.
    nlohmann::json SelfTestPurecallGuard();

    // Fails (without touching the engine) when the SDK was dumped from a different exe or when
    // the vtable at spec.vtable_rva does not report spec.token.
    NativeCommand Create(const sdk::CmdSpec& spec);

    // Takes ownership of a command the engine constructed itself (e.g. a Clone of a stack-built
    // command whose embedded objects need engine copy constructors), after checking that it is
    // `spec`'s command type. Validate/post it like any created command.
    NativeCommand Adopt(const sdk::CmdSpec& spec, void* engine_object);

    bool SdkMatchesExe() const { return sdk_matches_; }
    uintptr_t Base() const { return base_; }
    uint32_t ExeTimestamp() const { return exe_timestamp_; }

    // Calls a const engine predicate `bool fn(self, a, b, CString* reason)` (SEH/purecall
    // guarded). On false, `reason` gets the engine's text with markup stripped; passing
    // reason = nullptr passes a null CString*, which such predicates accept.
    bool CallPredicate(uintptr_t fn_rva, void* self, void* a, void* b, std::string* reason);

    // Runs `call(ctx, out)` under the SEH/purecall guard, where `out` is an engine CString that
    // the engine function fills (return slot or append target), and returns its text with
    // rich-text markup stripped. `call` forwards to the engine function with its own argument
    // order; keep it free of C++ objects with destructors.
    using FnTextCall = void (*)(void* ctx, void* out_cstring);
    bool CallForText(FnTextCall call, void* ctx, std::string* text);
    // Runs `call(ctx, nullptr)` under the same guard, for engine queries whose result `call`
    // stores into ctx itself. Returns false if the engine call raised.
    bool CallGuarded(FnTextCall call, void* ctx);

    // Runs one console command line (e.g. "effect ...", "smooth") through
    // CConsole::RunCommandNow, exactly as if typed into the in-game console. Main thread only.
    // The command's own output goes to the console and the game logs, not back to the caller.
    bool RunConsoleCommand(const std::string& line, std::string* error);

    // Engine operator new; memory handed to engine-owned objects must come from here.
    void* EngineAlloc(size_t n) const;

private:
    CommandBuilder() = default;

    uintptr_t base_{ 0 };
    uint32_t exe_timestamp_{ 0 };
    bool sdk_matches_{ false };
};

} // namespace bridge
