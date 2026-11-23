#include "event_manager.hpp"
#include "game_state.hpp"
#include "command_builder.hpp"
#include "sdk/stellaris_sdk.hpp"
#include <unordered_set>

namespace bridge {

// 只保留合法 UTF-8。探针会从任意地址取"字符串"，里面混进非法字节时
// nlohmann::json 序列化会抛 type_error，而抛出点不在 try/catch 内 → std::terminate
// （游戏无 dump 静默退出）。所有探针出字符串前都必须过这一层。
static std::string Utf8Safe(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = (unsigned char)in[i];
        size_t n = 0;
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else { ++i; continue; }
        if (i + n >= in.size()) break;
        bool ok = true;
        for (size_t k = 1; k <= n; ++k) {
            if (i + k >= in.size() || ((unsigned char)in[i + k] & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { ++i; continue; }
        out.append(in, i, n + 1);
        i += n + 1;
    }
    return out;
}
#pragma pack(push, 1)
struct RawPdxString {
    union {
        char buf[16];
        char* heap_ptr;
    };
    uint64_t size;
    uint64_t capacity;
};

struct OptionNode {
    void* button;
    void* unk_8;
    OptionNode* next;
};
#pragma pack(pop)

// Safe wrappers to avoid C2712 SEH unwind conflicts
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

static bool SafeReadInt(const void* addr, int* out) {
    __try {
        *out = *(const int*)addr;
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

static bool SafeCopyChars(char* dest, const char* src, size_t count) {
    __try {
        memcpy(dest, src, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* SafeFindChild(EventManager::FnFindChild fn, void* container, void* pdx_str) {
    __try {
        return fn(container, pdx_str);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeSelectOption(EventManager::FnSelectOption fn, void* win, int option_index) {
    __try {
        fn(win, option_index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 同一个函数的 open-event 版：__try 必须待在没有 C++ 对象的函数里（否则 C2712）
static bool SafeSelectByOpenEvent(uintptr_t fn, void* ope, int option_index) {
    __try {
        ((void(*)(void*, int))fn)(ope, option_index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnViewCall = void (*)(void* view);
static bool SafeViewCall(FnViewCall fn, void* view) {
    __try {
        fn(view);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

EventManager& EventManager::Get() {
    static EventManager instance;
    return instance;
}

bool EventManager::Init(uintptr_t base_address) {
    base_address_ = base_address;

    fn_find_child_ = nullptr;
    fn_select_option_ = SdkMatchesImage(base_address_)
        ? (FnSelectOption)(base_address_ + sdk::fn::CEventWindow_PostEventOptionSelection) : nullptr;

    return true;
}

static std::string CleanPdxString(const std::string& input) {
    return RenderPdxMarkup(input);
}

std::string EventManager::ExtractPdxString(void* ptr) {
    if (!ptr) return "";

    RawPdxString raw{};
    if (!SafeCopyChars((char*)&raw, (const char*)ptr, sizeof(RawPdxString))) {
        return "";
    }

    if (raw.size == 0) return "";

    if (raw.capacity < 16) {
        size_t len = raw.size < 16 ? (size_t)raw.size : 15;
        char temp[16]{ 0 };
        if (SafeCopyChars(temp, raw.buf, len)) {
            temp[len] = '\0';
            return CleanPdxString(std::string(temp, len));
        }
    } else if (raw.heap_ptr) {
        uintptr_t addr = (uintptr_t)raw.heap_ptr;
        if (addr > 0x10000 && addr < 0x7FFFFFFFFFFF) {
            size_t len = raw.size < 4096 ? (size_t)raw.size : 4096;
            std::string result(len, '\0');
            if (SafeCopyChars(&result[0], raw.heap_ptr, len)) {
                return CleanPdxString(result);
            }
        }
    }
    return "";
}

void* EventManager::FindChildByName(void* container, const char* name) {
    if (!container || !name) return nullptr;

    void* keys_arr = nullptr;
    uint32_t cnt = 0;
    void* vals_arr = nullptr;

    if (SafeReadPtr((const void*)((uintptr_t)container + 0x710), &keys_arr) &&
        SafeReadU32((const void*)((uintptr_t)container + 0x71C), &cnt) &&
        SafeReadPtr((const void*)((uintptr_t)container + 0x6F8), &vals_arr) &&
        keys_arr && vals_arr && cnt > 0 && cnt < 200) {

        for (uint32_t i = 0; i < cnt; ++i) {
            std::string k_name = ExtractPdxString((void*)((uintptr_t)keys_arr + i * 48 + 16));
            if (_stricmp(k_name.c_str(), name) == 0) {
                void* ctrl = nullptr;
                if (SafeReadPtr((const void*)((uintptr_t)vals_arr + i * 8), &ctrl) && ctrl) {
                    return ctrl;
                }
            }
        }
    }

    // Recurse into child containers in [container + 0x878]
    void* c_arr = nullptr;
    uint32_t c_cnt = 0;
    if (SafeReadPtr((const void*)((uintptr_t)container + 0x878), &c_arr) &&
        SafeReadU32((const void*)((uintptr_t)container + 0x884), &c_cnt) &&
        c_arr && c_cnt > 0 && c_cnt < 50) {

        for (uint32_t j = 0; j < c_cnt; ++j) {
            void* child = nullptr;
            if (SafeReadPtr((const void*)((uintptr_t)c_arr + j * 8), &child) && child) {
                void* found = FindChildByName(child, name);
                if (found) return found;
            }
        }
    }

    return nullptr;
}

// ---- data behind a standard event window -------------------------------------------------
// CEventWindow (as listed at [idler + 0x170]): +0xA1C id of its COpenPlayerEvent (Setup reads it
// for CGameState::GetOpenPlayerEvent). CGameState: +0x180 COpenPlayerEvent* array, +0x18C count.
// COpenPlayerEvent: +0x8 CEventHandle* (-> CEvent*), +0x10 CEventScope (sdk::ent).
// CEvent: +0x10 CString script id, +0x5E0 CPdxArray of options, +0x590 pointer whose CEffect at
// +0x60 is passed with the option (both as CEventWindow::GetToolTip passes them).
constexpr std::ptrdiff_t kWinOpenEventId = 0xA1C;
constexpr std::ptrdiff_t kGsOpenEvents = 0x180;
constexpr std::ptrdiff_t kGsOpenEventCount = 0x18C;
constexpr std::ptrdiff_t kEventKey = 0x10 + 0x10;  // CString header, then its std::string
// CPdxArray of 0x10-byte entries {CEventOption*, ..}: data +8, size +0x14
constexpr std::ptrdiff_t kArrayData = 0x8, kArraySize = 0x14, kOptionEntry = 0x10;
constexpr std::ptrdiff_t kEventOptionEffectOwner = 0x590;
constexpr std::ptrdiff_t kEffectInOwner = 0x60;

struct EventTextCtx {
    uintptr_t fn;
    void* event;
    void* scope;
    void* options;
    int index;
    void* effect;
};

static void CallEventTitle(void* c, void* out) {
    auto* x = (EventTextCtx*)c;
    ((void* (*)(void*, void*, void*))x->fn)(x->event, out, x->scope);
}

static void CallEventDesc(void* c, void* out) {
    auto* x = (EventTextCtx*)c;
    ((void* (*)(void*, void*, void*))x->fn)(out, x->event, x->scope);
}

static void CallOptionEffects(void* c, void* out) {
    auto* x = (EventTextCtx*)c;
    ((void* (*)(void*, void*, bool, void*, int, void*, bool))x->fn)(out, x->scope, false, x->options, x->index,
                                                                    x->effect, true);
}

struct OptionCall {
    uintptr_t fn;
    const void* a;
    const void* b;
    const void* c;
    bool flag;
    int result;
};
static void CallFindExclusive(void* p, void*) {
    auto* x = (OptionCall*)p;
    x->result = ((int (*)(const void*, bool, const void*))x->fn)(x->a, x->flag, x->b);
}
static void CallIsPotential(void* p, void*) {
    auto* x = (OptionCall*)p;
    x->result = ((bool (*)(const void*, const void*, bool, bool))x->fn)(x->a, x->b, x->flag, true);
}
static void CallIsAllowed(void* p, void*) {
    auto* x = (OptionCall*)p;
    x->result = ((bool (*)(const void*, const void*))x->fn)(x->a, x->b);
}
static void CallOptionName(void* p, void* out) {
    auto* x = (OptionCall*)p;
    ((void* (*)(const void*, void*, const void*))x->fn)(x->a, out, x->b);
}

static void* FindOpenPlayerEvent(uintptr_t base, uint32_t id) {
    void* gs = nullptr;
    void** arr = nullptr;
    int count = 0;
    if (!SafeReadPtr((const void*)(base + sdk::glob::g_CurrentGameState), &gs) || !gs ||
        !SafeReadPtr((const void*)((uintptr_t)gs + kGsOpenEvents), (void**)&arr) || !arr ||
        !SafeReadInt((const void*)((uintptr_t)gs + kGsOpenEventCount), &count) || count <= 0 || count > 512) {
        return nullptr;
    }
    for (int i = 0; i < count; ++i) {
        void* ope = nullptr;
        uint32_t ope_id = 0xFFFFFFFF;
        if (SafeReadPtr(&arr[i], &ope) && ope &&
            SafeReadU32((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::id), &ope_id) && ope_id == id) {
            return ope;
        }
    }
    return nullptr;
}

// Fills title, description, script id and per-option effects of a standard event window from the
// event and scope it was opened with, through the engine functions the window itself uses.
void EventManager::ReadEventData(void* win, EventInfo& info) {
    uint32_t open_id = 0xFFFFFFFF;
    if (!SafeReadU32((const void*)((uintptr_t)win + kWinOpenEventId), &open_id)) {
        return;
    }
    void* ope = FindOpenPlayerEvent(base_address_, open_id);
    void* handle = nullptr;
    void* event = nullptr;
    if (!ope || !SafeReadPtr((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::event), &handle) || !handle ||
        !SafeReadPtr(handle, &event) || !event) {
        return;
    }
    info.open_event_id = open_id;
    info.event_key = ExtractPdxString((void*)((uintptr_t)event + kEventKey));

    auto& cb = CommandBuilder::Get();
    EventTextCtx ctx{};
    ctx.event = event;
    ctx.scope = (void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::scope);
    std::string text;
    ctx.fn = base_address_ + sdk::fn::CEvent_GetTitle;
    if (cb.CallForText(&CallEventTitle, &ctx, &text) && !text.empty()) {
        info.title = CleanPdxString(text);
    }
    text.clear();
    ctx.fn = base_address_ + sdk::fn::NEventWindowUtil_GetEventWindowDesc;
    if (cb.CallForText(&CallEventDesc, &ctx, &text) && !text.empty()) {
        info.description = CleanPdxString(text);
    }

    ReadShownOptions(win, event, info);

    void* effect_owner = nullptr;
    SafeReadPtr((const void*)((uintptr_t)event + kEventOptionEffectOwner), &effect_owner);
    ctx.fn = base_address_ + sdk::fn::CEventOption_GetDescForOptionAtIndex;
    ctx.options = (void*)((uintptr_t)event + sdk::rt::CEvent_options);
    ctx.effect = effect_owner ? (void*)((uintptr_t)effect_owner + kEffectInOwner) : nullptr;
    for (auto& opt : info.options) {
        text.clear();
        ctx.index = opt.index;
        if (cb.CallForText(&CallOptionEffects, &ctx, &text)) {
            opt.effects = CleanPdxString(text);
        }
    }
}

// 读一个「没有窗口的」open event 的选项：下标 + 文本 + 效果 + 能不能选。
// 逻辑照 ReadShownOptions，但 scope 直接取 ope+0x10（不再需要 CEventWindow）。
void EventManager::ReadOpenEventOptions(void* ope, void* event, EventInfo& info) {
    auto& cb = CommandBuilder::Get();
    const void* scope = (const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::scope);
    const uintptr_t options = (uintptr_t)event + sdk::rt::CEvent_options;
    void* data = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)(options + kArrayData), &data) || !data ||
        !SafeReadU32((const void*)(options + kArraySize), &count) || count == 0 || count > 64) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        void* opt = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)data + i * kOptionEntry), &opt) || !opt) continue;
        EventOptionInfo o;
        o.index = (int)i;
        OptionCall name{ base_address_ + sdk::fn::CEventOption_GetName,
                         (const void*)((uintptr_t)opt + sdk::rt::CEventOption_name), scope, nullptr, false, 0 };
        std::string text;
        if (cb.CallForText(&CallOptionName, &name, &text)) o.text = CleanPdxString(text);
        OptionCall allowed{ base_address_ + sdk::fn::CEventOption_IsAllowedSkipPotential, opt, scope, nullptr, false, 0 };
        o.is_valid = cb.CallGuarded(&CallIsAllowed, &allowed) && allowed.result;
        info.options.push_back(o);
    }

    void* effect_owner = nullptr;
    SafeReadPtr((const void*)((uintptr_t)event + kEventOptionEffectOwner), &effect_owner);
    EventTextCtx ctx{};
    ctx.event = event;
    ctx.scope = (void*)scope;
    ctx.fn = base_address_ + sdk::fn::CEventOption_GetDescForOptionAtIndex;
    ctx.options = (void*)options;
    ctx.effect = effect_owner ? (void*)((uintptr_t)effect_owner + kEffectInOwner) : nullptr;
    for (auto& opt : info.options) {
        std::string text;
        ctx.index = opt.index;
        if (cb.CallForText(&CallOptionEffects, &ctx, &text)) opt.effects = CleanPdxString(text);
    }
}

// The options the window shows, as CEventWindow::Setup picks them (the matching exclusive option,
// else every potential one), with their text and whether they can be chosen. Works for every event
// window type (standard, leader story ...), unlike the window's own button list. The index is the
// option's place in the event, what the button carries and PostEventOptionSelection takes.
void EventManager::ReadShownOptions(void* win, void* event, EventInfo& info) {
    auto& cb = CommandBuilder::Get();
    const void* scope = (const void*)((uintptr_t)win + sdk::rt::CEventWindow_scope);
    uint8_t flag = 0;
    SafeReadU8((const void*)((uintptr_t)win + sdk::rt::CEventWindow_option_flag), &flag);
    const uintptr_t options = (uintptr_t)event + sdk::rt::CEvent_options;
    void* data = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)(options + kArrayData), &data) || !data ||
        !SafeReadU32((const void*)(options + kArraySize), &count) || count == 0 || count > 64) {
        return;
    }
    auto option_at = [&](uint32_t i) {
        void* opt = nullptr;
        SafeReadPtr((const void*)((uintptr_t)data + i * kOptionEntry), &opt);
        return opt;
    };

    std::vector<uint32_t> shown;
    OptionCall find{ base_address_ + sdk::fn::CEventOption_FindMatchingPotentialExclusiveOptionIndex, scope,
                     (const void*)options, nullptr, flag != 0, -1 };
    if (!cb.CallGuarded(&CallFindExclusive, &find)) return;
    if (find.result >= 0 && (uint32_t)find.result < count) {
        shown.push_back((uint32_t)find.result);
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            OptionCall pot{ base_address_ + sdk::fn::CEventOption_IsPotentialIgnoreExclusive, option_at(i), scope,
                            nullptr, flag != 0, 0 };
            if (pot.a && cb.CallGuarded(&CallIsPotential, &pot) && pot.result) shown.push_back(i);
        }
    }

    std::vector<EventOptionInfo> out;
    for (uint32_t i : shown) {
        void* opt = option_at(i);
        if (!opt) continue;
        EventOptionInfo o;
        o.index = (int)i;
        OptionCall name{ base_address_ + sdk::fn::CEventOption_GetName,
                         (const void*)((uintptr_t)opt + sdk::rt::CEventOption_name), scope, nullptr, false, 0 };
        std::string text;
        if (cb.CallForText(&CallOptionName, &name, &text)) o.text = CleanPdxString(text);
        OptionCall allowed{ base_address_ + sdk::fn::CEventOption_IsAllowedSkipPotential, opt, scope, nullptr, false, 0 };
        o.is_valid = cb.CallGuarded(&CallIsAllowed, &allowed) && allowed.result;
        // the window's own button text when the name call gave none
        for (const auto& b : info.options) {
            if (b.index == o.index && o.text.empty()) o.text = b.text;
        }
        out.push_back(o);
    }
    if (!out.empty()) info.options = std::move(out);
}

// A CInGameIdler window view (start screen / anomaly / first contact) when it is the class the SDK
// names and its gui window is shown.
void* EventManager::ShownView(void* idler, std::ptrdiff_t member, uintptr_t vtable_rva) {
    void* view = nullptr;
    void* vt = nullptr;
    void* ui_window = nullptr;
    uint8_t shown = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)idler + member), &view) || !view) return nullptr;
    if (!SafeReadPtr(view, &vt) || (uintptr_t)vt != base_address_ + vtable_rva) return nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)view + 0x78), &ui_window) || !ui_window) return nullptr;
    if (!SafeReadU8((const void*)((uintptr_t)ui_window + 0x41), &shown) || !shown) return nullptr;
    return view;
}

// The view's CGuiView::Hide override (slot from the SDK, read from its own vtable)
bool EventManager::HideView(void* view) {
    void* vt = nullptr;
    void* fn = nullptr;
    if (!SafeReadPtr(view, &vt) || !vt) return false;
    if (!SafeReadPtr((const void*)((uintptr_t)vt + sdk::vt::CGuiView_Hide * sizeof(void*)), &fn) || !fn) return false;
    return SafeViewCall((FnViewCall)fn, view);
}

std::vector<EventInfo> EventManager::GetActiveEvents() {
    std::vector<EventInfo> events;

    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) {
        return events;
    }

    // 1. Check Opening Start Screen ("开局背景特殊事件") at [idler + 0xBE8]
    void* start_screen = ShownView(idler, sdk::rt::CInGameIdler_CStartScreenWindow, sdk::vt::CStartScreenWindow);
    if (start_screen) {
        void* ui_window = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)start_screen + 0x78), &ui_window) && ui_window) {
            {
                EventInfo start_ev;
                start_ev.window_id = START_SCREEN_EVENT_ID;

                // Title: empire_name - origin_name
                void* emp_ctrl = FindChildByName(ui_window, "empire_name");
                std::string emp_name = emp_ctrl ? ExtractPdxString((void*)((uintptr_t)emp_ctrl + 0x168)) : "";

                void* orig_ctrl = FindChildByName(ui_window, "origin_name");
                std::string orig_name = orig_ctrl ? ExtractPdxString((void*)((uintptr_t)orig_ctrl + 0x168)) : "";

                if (!emp_name.empty() && !orig_name.empty()) {
                    start_ev.title = emp_name + " - " + orig_name;
                } else if (!emp_name.empty()) {
                    start_ev.title = emp_name;
                } else {
                    start_ev.title = "开局帝国背景";
                }

                // Description: starting_description
                void* desc_ctrl = FindChildByName(ui_window, "starting_description");
                if (desc_ctrl) {
                    start_ev.description = ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x168));
                }

                // Close Button Option
                void* close_btn = FindChildByName(ui_window, "close");
                std::string btn_text = close_btn ? ExtractPdxString((void*)((uintptr_t)close_btn + 0x168)) : "";
                if (btn_text.empty()) {
                    btn_text = "我们的征途是星辰大海！";
                }

                start_ev.options.push_back({ 0, btn_text, true });
                events.push_back(start_ev);
            }
        }
    }

    // 2. Check Anomaly Window ("异常现象发现窗口") at [idler + 0xB08]
    void* anomaly_view = ShownView(idler, sdk::rt::CInGameIdler_CAnomalyWindow, sdk::vt::CAnomalyWindow);
    if (anomaly_view) {
        void* ui_window = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)anomaly_view + 0x78), &ui_window) && ui_window) {
            {
                EventInfo anom_ev;
                anom_ev.window_id = ANOMALY_EVENT_ID;

                // Anomaly title & description from header_win at [anomaly_view + 0xD0]
                void* header_win = nullptr;
                SafeReadPtr((const void*)((uintptr_t)anomaly_view + 0xD0), &header_win);

                std::string header_str;
                std::string desc_str;
                if (header_win) {
                    header_str = ExtractPdxString((void*)((uintptr_t)header_win + 0x168));
                    desc_str = ExtractPdxString((void*)((uintptr_t)header_win + 0x12E8));
                }

                if (!header_str.empty()) {
                    anom_ev.title = header_str;
                } else {
                    void* title_ctrl = FindChildByName(ui_window, "title");
                    std::string title_str = title_ctrl ? ExtractPdxString((void*)((uintptr_t)title_ctrl + 0x168)) : "";
                    anom_ev.title = !title_str.empty() ? title_str : "异常现象";
                }

                if (!desc_str.empty()) {
                    anom_ev.description = desc_str;
                } else {
                    void* anom_sub_win = FindChildByName(ui_window, "anomaly_window");
                    if (anom_sub_win) {
                        void* desc_ctrl = FindChildByName(anom_sub_win, "desc");
                        if (desc_ctrl) anom_ev.description = ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x168));
                    }
                }

                // Option 0: ok ("暂时离开")
                void* ok_btn = FindChildByName(ui_window, "ok");
                std::string ok_text = ok_btn ? ExtractPdxString((void*)((uintptr_t)ok_btn + 0x168)) : "";
                if (ok_text.empty()) ok_text = "暂时离开";
                anom_ev.options.push_back({ 0, ok_text, true });

                // Option 1: research ("调查")
                void* res_btn = FindChildByName(ui_window, "research");
                std::string res_text = res_btn ? ExtractPdxString((void*)((uintptr_t)res_btn + 0x168)) : "";
                if (res_text.empty()) res_text = "调查";
                anom_ev.options.push_back({ 1, res_text, true });

                events.push_back(anom_ev);
            }
        }
    }

    // 3. Check First Contact View ("第一次接触事件/阶段窗口") at [idler + 0xC90]
    void* fc_view = ShownView(idler, sdk::rt::CInGameIdler_CFirstContactView, sdk::vt::CFirstContactView);
    if (fc_view) {
        void* ui_window = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)fc_view + 0x78), &ui_window) && ui_window) {
            {
                EventInfo fc_ev;
                fc_ev.window_id = FIRST_CONTACT_EVENT_ID;

                // Title: site_name + " - " + chapter_subtitle
                void* site_ctrl = FindChildByName(ui_window, "site_name");
                std::string site_name = site_ctrl ? ExtractPdxString((void*)((uintptr_t)site_ctrl + 0x168)) : "";

                void* sub_ctrl = FindChildByName(ui_window, "chapter_subtitle");
                std::string subtitle = sub_ctrl ? ExtractPdxString((void*)((uintptr_t)sub_ctrl + 0x168)) : "";

                void* stage_ctrl = FindChildByName(ui_window, "title_text");
                std::string stage_name = stage_ctrl ? ExtractPdxString((void*)((uintptr_t)stage_ctrl + 0x168)) : "";

                if (!site_name.empty() && !subtitle.empty()) {
                    fc_ev.title = site_name + " - " + subtitle;
                } else if (!site_name.empty()) {
                    fc_ev.title = site_name;
                } else {
                    fc_ev.title = "第一次接触事件";
                }
                if (!stage_name.empty()) {
                    fc_ev.title += " (" + stage_name + ")";
                }

                // Description: check description control, or locked status text
                void* desc_ctrl = FindChildByName(ui_window, "description");
                std::string desc_str = desc_ctrl ? ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x168)) : "";
                if (desc_str.empty() && desc_ctrl) {
                    desc_str = ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x268));
                }

                if (desc_str.empty()) {
                    void* status_text_ctrl = FindChildByName(ui_window, "text");
                    if (status_text_ctrl) {
                        desc_str = ExtractPdxString((void*)((uintptr_t)status_text_ctrl + 0x168));
                    }
                }
                if (desc_str.empty()) {
                    void* no_rep_ctrl = FindChildByName(ui_window, "no_report_text");
                    if (no_rep_ctrl) {
                        desc_str = ExtractPdxString((void*)((uintptr_t)no_rep_ctrl + 0x168));
                    }
                }
                fc_ev.description = desc_str;

                // Options: check options_box
                void* opt_box = nullptr;
                SafeReadPtr((const void*)((uintptr_t)fc_view + 0xE0), &opt_box);
                if (opt_box) {
                    void* c_arr = nullptr;
                    uint32_t c_cnt = 0;
                    if (SafeReadPtr((const void*)((uintptr_t)opt_box + 0x878), &c_arr) &&
                        SafeReadU32((const void*)((uintptr_t)opt_box + 0x884), &c_cnt) &&
                        c_arr && c_cnt > 0 && c_cnt < 20) {

                        for (uint32_t j = 0; j < c_cnt; ++j) {
                            void* child_opt = nullptr;
                            if (SafeReadPtr((const void*)((uintptr_t)c_arr + j * 8), &child_opt) && child_opt) {
                                void* text_ctrl = FindChildByName(child_opt, "text");
                                std::string opt_text = text_ctrl ? ExtractPdxString((void*)((uintptr_t)text_ctrl + 0x168)) : "";
                                if (opt_text.empty()) {
                                    opt_text = "选项 " + std::to_string(j);
                                }
                                fc_ev.options.push_back({ (int)j, opt_text, true });
                            }
                        }
                    }
                }

                if (fc_ev.options.empty()) {
                    fc_ev.options.push_back({ 0, "关闭", true });
                }

                events.push_back(fc_ev);
            }
        }
    }

    // 4. Check Standard Modal Event Windows at [idler + 0x170]
    void** windows = nullptr;
    uint32_t count = 0;

    if (!SafeReadPtr((const void*)((uintptr_t)idler + 0x170), (void**)&windows) ||
        !SafeReadU32((const void*)((uintptr_t)idler + 0x17c), &count)) {
        return events;
    }

    if (!windows || count == 0 || count > 200) {
        return events;
    }

    for (uint32_t i = 0; i < count; ++i) {
        void* win = nullptr;
        if (!SafeReadPtr((const void*)&windows[i], &win) || !win) {
            continue;
        }

        EventInfo info;
        SafeReadU32((const void*)((uintptr_t)win + 0x98), &info.window_id);

        void* ui_window = nullptr;
        SafeReadPtr((const void*)((uintptr_t)win + 0x78), &ui_window);

        if (ui_window) {
            // Extract Title
            void* title_ctrl = FindChildByName(ui_window, "title");
            if (!title_ctrl) title_ctrl = FindChildByName(win, "title");
            if (title_ctrl) {
                info.title = ExtractPdxString((void*)((uintptr_t)title_ctrl + 0x168));
                if (info.title.empty()) {
                    info.title = ExtractPdxString((void*)((uintptr_t)title_ctrl + 0x268));
                }
            }

            // Extract Description (search container tree for description)
            void* desc_ctrl = FindChildByName(ui_window, "description");
            if (!desc_ctrl) desc_ctrl = FindChildByName(win, "description");
            if (desc_ctrl) {
                info.description = ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x168));
                if (info.description.empty()) {
                    info.description = ExtractPdxString((void*)((uintptr_t)desc_ctrl + 0x268));
                }
            }
        }

        // Extract Options list from linked list at +0x9d8
        void* node = nullptr;
        SafeReadPtr((const void*)((uintptr_t)win + 0x9d8), &node);

        uint32_t opt_count = 0;
        SafeReadU32((const void*)((uintptr_t)win + 0x9e8), &opt_count);

        for (uint32_t o = 0; o < opt_count && node != nullptr; ++o) {
            void* button = nullptr;
            SafeReadPtr((const void*)((uintptr_t)node + 0), &button);

            if (button) {
                int opt_idx = 0;
                SafeReadInt((const void*)((uintptr_t)button + 0x20), &opt_idx);

                std::string opt_text = ExtractPdxString((void*)((uintptr_t)button + 0x268));
                if (opt_text.empty()) {
                    opt_text = ExtractPdxString((void*)((uintptr_t)button + 0x168));
                }

                info.options.push_back({ opt_idx, opt_text, true });
            }

            void* next_node = nullptr;
            SafeReadPtr((const void*)((uintptr_t)node + 16), &next_node);
            node = next_node;
        }

        ReadEventData(win, info);
        events.push_back(info);
    }

    return events;
}

nlohmann::json EventManager::GetPendingJson() {
    // the windows waiting for the player, compact: what GetActiveEvents shows, without
    // descriptions, options and effects
    nlohmann::json out = nlohmann::json::array();
    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) return out;
    const std::pair<std::ptrdiff_t, uintptr_t> views[] = {
        { sdk::rt::CInGameIdler_CStartScreenWindow, sdk::vt::CStartScreenWindow },
        { sdk::rt::CInGameIdler_CAnomalyWindow, sdk::vt::CAnomalyWindow },
        { sdk::rt::CInGameIdler_CFirstContactView, sdk::vt::CFirstContactView },
    };
    const uint32_t ids[] = { START_SCREEN_EVENT_ID, ANOMALY_EVENT_ID, FIRST_CONTACT_EVENT_ID };
    const char* kinds[] = { "start_screen", "anomaly", "first_contact" };
    for (int k = 0; k < 3; ++k) {
        if (ShownView(idler, views[k].first, views[k].second)) out.push_back({ {"window_id", ids[k]}, {"kind", kinds[k]} });
    }

    void** windows = nullptr;
    uint32_t count = 0;
    if (!SafeReadPtr((const void*)((uintptr_t)idler + 0x170), (void**)&windows) ||
        !SafeReadU32((const void*)((uintptr_t)idler + 0x17c), &count) || !windows || count == 0 || count > 200) {
        return out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        void* win = nullptr;
        if (!SafeReadPtr((const void*)&windows[i], &win) || !win) continue;
        nlohmann::json e = { {"kind", "event"} };
        uint32_t window_id = 0;
        SafeReadU32((const void*)((uintptr_t)win + 0x98), &window_id);
        e["window_id"] = window_id;
        uint32_t open_id = 0xFFFFFFFF;
        void* ope = SafeReadU32((const void*)((uintptr_t)win + kWinOpenEventId), &open_id) ? FindOpenPlayerEvent(base_address_, open_id) : nullptr;
        void* handle = nullptr;
        void* event = nullptr;
        if (ope && SafeReadPtr((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::event), &handle) && handle &&
            SafeReadPtr(handle, &event) && event) {
            e["event_key"] = ExtractPdxString((void*)((uintptr_t)event + kEventKey));
            EventTextCtx ctx{};
            ctx.event = event;
            ctx.scope = (void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::scope);
            ctx.fn = base_address_ + sdk::fn::CEvent_GetTitle;
            std::string text;
            if (CommandBuilder::Get().CallForText(&CallEventTitle, &ctx, &text) && !text.empty()) e["title"] = CleanPdxString(text);
        }
        out.push_back(e);
    }
    return out;
}

nlohmann::json EventManager::GetActiveEventsJson() {
    auto events = GetActiveEvents();
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& ev : events) {
        nlohmann::json opt_arr = nlohmann::json::array();
        for (const auto& opt : ev.options) {
            nlohmann::json o = {
                {"index", opt.index},
                {"text", opt.text},
                {"is_valid", opt.is_valid}
            };
            if (!opt.effects.empty()) o["effects"] = opt.effects;
            opt_arr.push_back(o);
        }

        nlohmann::json e = {
            {"window_id", ev.window_id},
            {"title", ev.title},
            {"description", ev.description},
            {"options", opt_arr}
        };
        if (!ev.event_key.empty()) e["event_key"] = ev.event_key;
        arr.push_back(e);
    }

    return arr;
}

nlohmann::json EventManager::ResolveEvent(uint32_t window_id, int option_index) {
    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) {
        return {
            {"error", {
                {"code", -32001},
                {"message", "InGameIdler is null"}
            }}
        };
    }

    auto fail = [](int code, const std::string& message) {
        return nlohmann::json{ {"error", { {"code", code}, {"message", message} }} };
    };
    auto done = [&](const char* message) {
        return nlohmann::json{
            {"success", true},
            {"resolved_window_id", window_id},
            {"selected_option", option_index},
            {"message", message}
        };
    };
    bool pseudo = window_id == START_SCREEN_EVENT_ID || window_id == ANOMALY_EVENT_ID ||
                  window_id == FIRST_CONTACT_EVENT_ID;
    if (pseudo && !CommandBuilder::Get().SdkMatchesExe()) {
        return fail(-32003, "SDK does not match this stellaris.exe; regenerate it with tools/sdk_dumper/dump.py");
    }

    // Start screen: CStartScreenWindow::Close, what its button runs (hide, focus the capital,
    // fire on_press_begin)
    if (window_id == START_SCREEN_EVENT_ID) {
        void* view = ShownView(idler, sdk::rt::CInGameIdler_CStartScreenWindow, sdk::vt::CStartScreenWindow);
        if (!view) return fail(-32002, "Start screen is not shown");
        LOG("[EVENT_MGR] Resolving start screen: CStartScreenWindow::Close");
        if (!SafeViewCall((FnViewCall)(base_address_ + sdk::fn::CStartScreenWindow_Close), view)) {
            return fail(-32004, "CStartScreenWindow::Close raised an exception");
        }
        return done("Start screen closed");
    }

    // Anomaly window: "leave be" hides it (the window's CGuiView::Hide override is OnLeaveBe);
    // research is a fleet order, stellaris_research_anomalies
    if (window_id == ANOMALY_EVENT_ID) {
        void* view = ShownView(idler, sdk::rt::CInGameIdler_CAnomalyWindow, sdk::vt::CAnomalyWindow);
        if (!view) return fail(-32010, "Anomaly window is not shown");
        if (option_index == 1) {
            return fail(-32013, "Research the anomaly with stellaris_research_anomalies (fleet_id of the science ship)");
        }
        if (option_index != 0) {
            return fail(-32015, "Invalid option_index for the anomaly window. Legal options: 0 (暂时离开), 1 (调查)");
        }
        if (!HideView(view)) return fail(-32012, "CAnomalyWindow::OnLeaveBe raised an exception");
        return done("Anomaly left be (暂时离开)");
    }

    // First contact view: closing it (CFirstContactView::Hide); the contact itself goes on
    if (window_id == FIRST_CONTACT_EVENT_ID) {
        void* view = ShownView(idler, sdk::rt::CInGameIdler_CFirstContactView, sdk::vt::CFirstContactView);
        if (!view) return fail(-32016, "First contact view is not shown");
        if (!HideView(view)) return fail(-32020, "CFirstContactView::Hide raised an exception");
        return done("First contact view closed");
    }

    // Handle Standard Modal Event Window
    void** windows = nullptr;
    uint32_t count = 0;

    if (!SafeReadPtr((const void*)((uintptr_t)idler + 0x170), (void**)&windows) ||
        !SafeReadU32((const void*)((uintptr_t)idler + 0x17c), &count)) {
        return {
            {"error", {
                {"code", -32005},
                {"message", "Failed to read active event windows array"}
            }}
        };
    }

    void* target_win = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        void* win = nullptr;
        if (SafeReadPtr((const void*)&windows[i], &win) && win) {
            uint32_t cur_id = 0;
            if (SafeReadU32((const void*)((uintptr_t)win + 0x98), &cur_id) && cur_id == window_id) {
                target_win = win;
                break;
            }
        }
    }

    if (!target_win) {
        return {
            {"error", {
                {"code", -32006},
                {"message", "Event window_id not found among active events"}
            }}
        };
    }

    if (!fn_select_option_) {
        return {
            {"error", {
                {"code", -32007},
                {"message", "SDK does not match this stellaris.exe; regenerate it with tools/sdk_dumper/dump.py"}
            }}
        };
    }

    // PostEventOptionSelection does not check the index, so only accept an option the window shows
    // (hidden options fail their potential trigger and must not be picked).
    bool option_shown = false;
    bool option_valid = false;
    std::string shown;
    for (const auto& ev : GetActiveEvents()) {
        if (ev.window_id != window_id) continue;
        for (const auto& opt : ev.options) {
            if (opt.index == option_index) {
                option_shown = true;
                option_valid = opt.is_valid;
            }
            shown += (shown.empty() ? "" : ", ") + std::to_string(opt.index);
        }
    }
    if (!option_shown) {
        return {
            {"error", {
                {"code", -32009},
                {"message", "Option " + std::to_string(option_index) + " is not shown in event window " +
                                std::to_string(window_id) + " (available: " + shown + ")"}
            }}
        };
    }

    if (!option_valid) {
        return {
            {"error", {
                {"code", -32011},
                {"message", "Option " + std::to_string(option_index) + " is disabled in event window " +
                                std::to_string(window_id) + " (its allow conditions are not met)"}
            }}
        };
    }

    LOGF("[EVENT_MGR] Resolving standard event window_id %u with option %d...", window_id, option_index);
    if (!SafeSelectOption(fn_select_option_, target_win, option_index)) {
        LOGF("[EVENT_MGR] Exception in SelectOption for window_id %u", window_id);
        return {
            {"error", {
                {"code", -32008},
                {"message", "Exception occurred executing CEventWindow::SelectOption"}
            }}
        };
    }

    LOGF("[EVENT_MGR] Event window %u successfully resolved with option %d.", window_id, option_index);
    return {
        {"success", true},
        {"resolved_window_id", window_id},
        {"selected_option", option_index}
    };
}

// ---- 选一个 open player event 的选项，不需要 CEventWindow -----------------------------------
// CEventWindow::PostEventOptionSelection（RVA 0x1083910）只把窗口用来找它的 COpenPlayerEvent：
// 拿 [win+0xA1C] 去和 CGameState+0x180 数组里每条 [ope+0x180] 比，找到之后就是一句
//   call 0x8E4550(rcx = ope, edx = 选项下标)
// 那个 worker 在栈上构造 CSelectEventOptionCommand（+0x40 事件编码取自 CEvent+0x8、+0x44 选项、
// +0x48 open event id、+0x4C 国家，scope 从 ope+0x10 拷进来）并派发——和点一下按钮做的是同一件事。
// 所以首触 / 考古 / 裂隙这类没有 CEventWindow 的弹窗，可以直接拿 ope 调这个 worker。
constexpr uintptr_t kFnSelectOptionByOpenEvent = 0x8E4550;
constexpr std::ptrdiff_t kOpeEventId = 0x180;

// 只读：给 ope（或 COpenPlayerEvent 的 id）就返回这条打开事件的选项明细。
// 与 select_open_event_option 分开，读的时候完全不碰投递那条路。
nlohmann::json EventManager::GetEventOptionsJson(uint64_t ope_addr, uint32_t open_event_id) {
    if (!SdkMatchesImage(base_address_)) return { {"error", "SDK does not match this stellaris.exe"} };

    void* gs = nullptr;
    void** arr = nullptr;
    int count = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &gs) || !gs ||
        !SafeReadPtr((const void*)((uintptr_t)gs + kGsOpenEvents), (void**)&arr) || !arr ||
        !SafeReadInt((const void*)((uintptr_t)gs + kGsOpenEventCount), &count) || count <= 0 || count > 512) {
        return { {"error", "no open player events"} };
    }

    void* ope = (void*)(uintptr_t)ope_addr;
    if (!ope) {
        // 没给 ope，就按 COpenPlayerEvent id 找
        for (int i = 0; i < count; ++i) {
            void* p = nullptr;
            if (!SafeReadPtr(&arr[i], &p) || !p) continue;
            uint32_t id = 0xFFFFFFFF;
            SafeReadU32((const void*)((uintptr_t)p + kOpeEventId), &id);
            if (id == open_event_id) { ope = p; break; }
        }
        if (!ope) return { {"error", "no open event with id " + std::to_string(open_event_id)} };
    } else {
        bool found = false;
        for (int i = 0; i < count; ++i) {
            void* p = nullptr;
            if (SafeReadPtr(&arr[i], &p) && p == ope) { found = true; break; }
        }
        if (!found) return { {"error", "that address is not an open player event"} };
    }

    uint32_t ope_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)ope + kOpeEventId), &ope_id);

    void* handle = nullptr;
    void* event = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::event), &handle) || !handle ||
        !SafeReadPtr(handle, &event) || !event) {
        return { {"error", "the open event has no CEvent behind it"} };
    }

    EventInfo info;
    info.event_key = ExtractPdxString((void*)((uintptr_t)event + kEventKey));
    ReadOpenEventOptions(ope, event, info);

    nlohmann::json opts = nlohmann::json::array();
    for (const auto& o : info.options) {
        opts.push_back({ {"index", o.index}, {"text", o.text}, {"is_valid", o.is_valid}, {"effects", o.effects} });
    }

    return { {"success", true},
             {"ope", (uint64_t)(uintptr_t)ope},
             {"open_event_id", ope_id},
             {"event_key", info.event_key},
             {"option_list", opts} };
}

nlohmann::json EventManager::SelectOpenEventOptionJson(uint64_t ope_addr, int option_index, bool dry_run) {
    if (!ope_addr) {
        return { {"error", {{"code", -32040}, {"message", "ope address is 0"}}} };
    }
    if (option_index < 0) {
        return { {"error", {{"code", -32041}, {"message", "option index must be >= 0"}}} };
    }
    if (!SdkMatchesImage(base_address_)) {
        return { {"error", {{"code", -32042}, {"message", "SDK does not match this stellaris.exe"}}} };
    }

    void* ope = (void*)(uintptr_t)ope_addr;

    // 1. 这个地址必须真的在 CGameState 的 open-event 数组里——野指针进 native call 会崩
    void* gs = nullptr;
    void** arr = nullptr;
    int count = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &gs) || !gs ||
        !SafeReadPtr((const void*)((uintptr_t)gs + kGsOpenEvents), (void**)&arr) || !arr ||
        !SafeReadInt((const void*)((uintptr_t)gs + kGsOpenEventCount), &count) || count <= 0 || count > 512) {
        return { {"error", {{"code", -32043}, {"message", "no open player events"}}} };
    }
    bool found = false;
    for (int i = 0; i < count; ++i) {
        void* p = nullptr;
        if (SafeReadPtr(&arr[i], &p) && p == ope) { found = true; break; }
    }
    if (!found) {
        return { {"error", {{"code", -32044}, {"message", "that address is not an open player event"}}} };
    }

    // 2. 取 id / 事件 / 选项数，顺便把下标校验掉
    uint32_t ope_id = 0xFFFFFFFF;
    SafeReadU32((const void*)((uintptr_t)ope + kOpeEventId), &ope_id);

    void* handle = nullptr;
    void* event = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::event), &handle) || !handle ||
        !SafeReadPtr(handle, &event) || !event) {
        return { {"error", {{"code", -32045}, {"message", "the open event has no CEvent behind it"}}} };
    }

    uint32_t opt_count = 0;
    SafeReadU32((const void*)((uintptr_t)event + sdk::rt::CEvent_options + kArraySize), &opt_count);
    if (option_index >= (int)opt_count) {
        return { {"error", {{"code", -32046}, {"message", "option index " + std::to_string(option_index) +
                                                                 " out of range (the event shows " +
                                                                 std::to_string(opt_count) + ")"}}} };
    }

    std::string key = ExtractPdxString((void*)((uintptr_t)event + kEventKey));
    uint32_t event_code = 0;
    SafeReadU32((const void*)((uintptr_t)event + 0x8), &event_code);

    // 选项明细：下标 / 文本 / 效果 / 能不能选（dry_run 与投递都带，方便先看再选）
    EventInfo info;
    ReadOpenEventOptions(ope, event, info);
    nlohmann::json opts = nlohmann::json::array();
    for (const auto& o : info.options) {
        opts.push_back({ {"index", o.index}, {"text", o.text}, {"is_valid", o.is_valid}, {"effects", o.effects} });
    }

    if (dry_run) {
        return {
            {"success", true},
            {"dry_run", true},
            {"ope", ope_addr},
            {"open_event_id", ope_id},
            {"event_key", key},
            {"event_id", event_code},
            {"options", opt_count},
            {"option", option_index},
            {"option_list", opts}
        };
    }

    // 3. 调引擎自己的选择 worker（SEH 在 helper 里：本函数有 C++ 对象，不能直接 __try）
    bool ok = SafeSelectByOpenEvent(base_address_ + kFnSelectOptionByOpenEvent, ope, option_index);
    if (!ok) {
        LOGF("[EVENT_MGR] exception in select-by-open-event (ope 0x%llX, option %d)",
             (unsigned long long)ope_addr, option_index);
        return { {"error", {{"code", -32047}, {"message", "exception inside the option worker (0x8E4550)"}}} };
    }

    // 4. 回读：这条 open event 还在数组里吗（还在 = 没被消费掉）
    bool still_open = false;
    for (int i = 0; i < count; ++i) {
        void* p = nullptr;
        if (SafeReadPtr(&arr[i], &p) && p == ope) { still_open = true; break; }
    }

    LOGF("[EVENT_MGR] select-by-open-event: ope 0x%llX option %d (%s) posted, still_open=%d",
         (unsigned long long)ope_addr, option_index, key.c_str(), (int)still_open);

    return {
        {"success", true},
        {"posted", true},
        {"ope", ope_addr},
        {"open_event_id", ope_id},
        {"event_key", key},
        {"event_id", event_code},
        {"option", option_index},
        {"still_open", still_open}
    };
}

// ---- 调视图 vtable 的某一槽（只对三个已知视图开放） ------------------------------------------
// 背景：2026-11-19 深夜，首触与考古的阶段弹窗都用「应答事件」推过去了，数据也真的在走；
// 但弹窗文本只在视图自己的刷新槽里重写——首触那边 +0xC0 一变它会自己刷，考古答完事件它不刷，
// 于是面板一直写着「锁定」。这个探针就是手动触发那次刷新用的。
constexpr uintptr_t kArchViewVtable = 0x2417A38;
constexpr uintptr_t kFcViewVtable = 0x2418238;
constexpr uintptr_t kRiftViewVtable = 0x2418A98;

nlohmann::json EventManager::CallViewSlotJson(uint64_t view_addr, int slot) {
    if (!view_addr) return { {"error", "view address is 0"} };
    if (slot < 0 || slot > 64) return { {"error", "slot out of range (0..64)"} };
    if (!SdkMatchesImage(base_address_)) return { {"error", "SDK does not match this stellaris.exe"} };

    void* view = (void*)(uintptr_t)view_addr;
    void* vt = nullptr;
    if (!SafeReadPtr(view, &vt) || !vt) return { {"error", "view has no vtable"} };

    const uintptr_t vt_rva = (uintptr_t)vt - base_address_;
    if (vt_rva != kArchViewVtable && vt_rva != kFcViewVtable && vt_rva != kRiftViewVtable) {
        return { {"error", "not one of the three known views"}, {"vtable_rva", vt_rva} };
    }

    void* fn = nullptr;
    if (!SafeReadPtr((const void*)((uintptr_t)vt + (size_t)slot * 8), &fn) || !fn) {
        return { {"error", "that slot has no function"}, {"vtable_rva", vt_rva} };
    }
    const uintptr_t fn_rva = (uintptr_t)fn - base_address_;
    if (fn_rva > 0x2000000) {
        return { {"error", "slot does not point inside the image"}, {"fn_rva", fn_rva} };
    }

    const bool ok = SafeViewCall((FnViewCall)fn, view);
    LOGF("[EVENT_MGR] call_view_slot: view 0x%llX vt 0x%llX slot %d fn 0x%llX -> %d",
         (unsigned long long)view_addr, (unsigned long long)vt_rva, slot, (unsigned long long)fn_rva, (int)ok);
    return { {"success", ok},
             {"view", view_addr},
             {"vtable_rva", vt_rva},
             {"slot", slot},
             {"fn_rva", fn_rva},
             {"called", ok} };
}

// Read-only: walk the first-contact view's gui. The gui keeps children in a hash map
// (values +0x6F8 / keys +0x710 / count +0x71C, a key's name at key + i*48 + 16) and a control's
// caption sits at control + 0x168 — the layout FindChildByName uses.
// The view's real buttons ("了解更多。" / the choices) are NOT under the options_box at view + 0xE0
// that GetActiveEvents walks (it is empty in the locked state), so this probe walks the gui window
// and also tries view+0x60..0x140 as extra roots, reporting whichever yields controls.
nlohmann::json EventManager::ProbeFirstContactViewJson(int max_depth, const std::string& find_text) {
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 4) max_depth = 4;

    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) return { {"error", "InGameIdler is null"} };

    void* view = ShownView(idler, sdk::rt::CInGameIdler_CFirstContactView, sdk::vt::CFirstContactView);
    if (!view) return { {"error", "First contact view is not shown"} };

    void* ui_window = nullptr;
    SafeReadPtr((const void*)((uintptr_t)view + 0x78), &ui_window);

    auto walk = [&](void* root, int depth) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        if (!root) return arr;
        struct Item {
            void* ctrl;
            int depth;
            std::string path;
        };
        std::vector<Item> stack;
        stack.push_back({ root, 0, std::string() });
        int guard = 0;
        while (!stack.empty() && guard < 400) {
            Item it = stack.back();
            stack.pop_back();
            ++guard;

            // 一个容器有两种子布局，都要走：
            //   A. hashmap：values +0x6F8 / keys +0x710 / count +0x71C（键名在 key + i*48 + 16）
            //   B. 数组：   data  +0x878 / count +0x884（event_manager 读 options_box 用的就是这套）
            struct Child {
                void* ctrl;
                std::string name;
                const char* via;
            };
            std::vector<Child> kids;

            {
                void* vals = nullptr;
                void* keys = nullptr;
                uint32_t cnt = 0;
                if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x6F8), &vals) &&
                    SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x710), &keys) &&
                    SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x71C), &cnt) &&
                    vals && keys && cnt > 0 && cnt <= 200) {
                    for (uint32_t i = 0; i < cnt; ++i) {
                        void* ctrl = nullptr;
                        if (!SafeReadPtr((const void*)((uintptr_t)vals + (size_t)i * 8), &ctrl) || !ctrl) continue;
                        kids.push_back({ ctrl, Utf8Safe(ExtractPdxString((void*)((uintptr_t)keys + (size_t)i * 48 + 16))), "map" });
                    }
                }
            }
            {
                void* data = nullptr;
                uint32_t cnt = 0;
                if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x878), &data) &&
                    SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x884), &cnt) &&
                    data && cnt > 0 && cnt <= 200) {
                    for (uint32_t i = 0; i < cnt; ++i) {
                        void* ctrl = nullptr;
                        if (!SafeReadPtr((const void*)((uintptr_t)data + (size_t)i * 8), &ctrl) || !ctrl) continue;
                        kids.push_back({ ctrl, std::string(), "array" });
                    }
                }
            }

            for (const Child& kid : kids) {
                std::string text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x168)));
                if (text.empty()) text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x268)));  // 按钮文本常在这一档
                if (!kid.name.empty() || !text.empty()) {
                    arr.push_back({
                        {"depth", it.depth + 1},
                        {"path", it.path},
                        {"via", kid.via},
                        {"ptr", (uint64_t)(uintptr_t)kid.ctrl},
                        {"name", kid.name},
                        {"text", text}
                    });
                }
                // descend even when the node has no name/text — the buttons sit under unnamed boxes
                if (it.depth + 1 < depth) {
                    stack.push_back({ kid.ctrl, it.depth + 1, it.path + "/" + (kid.name.empty() ? "?" : kid.name) });
                }
            }
        }
        return arr;
    };

    nlohmann::json roots = nlohmann::json::array();
    {
        nlohmann::json got = walk(ui_window, max_depth);
        roots.push_back({ {"root", "gui_window(+0x78)"}, {"ptr", (uint64_t)(uintptr_t)ui_window},
                          {"nodes", got.size()}, {"controls", got} });
    }

    // find=... 时把所有候选根都扫一遍（0x0..0x1000），只报文本里含这个词的控件
    if (!find_text.empty()) {
        nlohmann::json hits = nlohmann::json::array();
        for (int off = 0x0; off <= 0x1000 && hits.size() < 20; off += 8) {
            void* p = nullptr;
            if (!SafeReadPtr((const void*)((uintptr_t)view + off), &p) || !p) continue;
            if ((uintptr_t)p < 0x10000) continue;
            nlohmann::json got = walk(p, 3);
            nlohmann::json matched = nlohmann::json::array();
            for (const auto& c : got) {
                const std::string t = c.value("text", std::string());
                if (t.find(find_text) != std::string::npos) matched.push_back(c);
            }
            if (matched.empty()) continue;
            char label[32]{};
            snprintf(label, sizeof(label), "view+0x%X", off);
            hits.push_back({ {"root", label}, {"ptr", (uint64_t)(uintptr_t)p}, {"matches", matched} });
        }
        return { {"success", true}, {"view", (uint64_t)(uintptr_t)view}, {"find", find_text}, {"hits", hits} };
    }

    int extra = 0;
    for (int off = 0x60; off <= 0x400 && extra < 16; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)view + off), &p) || !p) continue;
        if ((uintptr_t)p < 0x10000) continue;
        nlohmann::json got = walk(p, 3);
        if (got.empty()) continue;

        char label[32]{};
        snprintf(label, sizeof(label), "view+0x%X", off);
        roots.push_back({ {"root", label}, {"ptr", (uint64_t)(uintptr_t)p},
                          {"nodes", got.size()}, {"controls", got} });
        ++extra;
    }

    return {
        {"success", true},
        {"view", (uint64_t)(uintptr_t)view},
        {"gui_window", (uint64_t)(uintptr_t)ui_window},
        {"roots", roots}
    };
}

// 只读：从 idler 出发按「CEventWindow 的签名」找事件窗。
// 签名：+0x9E8 = 选项按钮数（1..20）、+0x98 = 窗口 id（不是 0xFFFFFFFF）、+0xB8 = scope 指针非空。
// 用途：考古/首触这类弹窗不是 idler+0x170 那个数组里的，但它们很可能仍是 CEventWindow，
// 只是挂在别处；找到它就能直接调 CEventWindow::PostEventOptionSelection。
nlohmann::json EventManager::FindEventWindowsJson(int depth) {
    if (depth < 1) depth = 1;
    if (depth > 3) depth = 3;

    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) return { {"error", "InGameIdler is null"} };

    std::unordered_set<uintptr_t> seen;
    struct Item { void* obj; int depth; };
    std::vector<Item> queue;
    for (int off = 0; off <= 0x2000; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)idler + off), &p) || !p) continue;
        if ((uintptr_t)p > 0x10000) queue.push_back({ p, 1 });
    }

    nlohmann::json hits = nlohmann::json::array();
    int guard = 0;
    while (!queue.empty() && guard < 2000) {
        Item it = queue.back();
        queue.pop_back();
        ++guard;
        uintptr_t key = (uintptr_t)it.obj;
        if (!seen.insert(key).second) continue;

        uint32_t count = 0, wnd_id = 0;
        void* scope = nullptr;
        const bool sig = SafeReadU32((const void*)(key + 0x9E8), &count) && count > 0 && count <= 20 &&
                         SafeReadU32((const void*)(key + 0x98), &wnd_id) && wnd_id != 0xFFFFFFFF &&
                         SafeReadPtr((const void*)(key + 0xB8), &scope) && scope != nullptr;
        if (sig) {
            void* vt = nullptr;
            SafeReadPtr((const void*)key, &vt);
            hits.push_back({ {"obj", (uint64_t)key}, {"window_id", wnd_id}, {"options", count},
                             {"vtable", (uint64_t)(uintptr_t)vt} });
            if (hits.size() >= 20) break;
        }

        if (it.depth < depth) {
            for (int off = 0; off <= 0x1000; off += 8) {
                void* c = nullptr;
                if (!SafeReadPtr((const void*)(key + off), &c) || !c) continue;
                if ((uintptr_t)c > 0x10000) queue.push_back({ c, it.depth + 1 });
            }
        }
    }

    return { {"success", true}, {"visited", (int)seen.size()}, {"hits", hits} };
}
// 只读：走一个任意 gui window 地址的控件树（find 非空时只报文本里含它的控件）。
// 用于检查某个 view 名下的各个子窗口——按钮常常挂在子窗口里，而不是主窗口。
nlohmann::json EventManager::WalkGuiJson(uint64_t address, int depth, const std::string& find) {
    if (!address) return { {"error", "address is 0"} };
    if (depth < 1) depth = 1;
    if (depth > 4) depth = 4;

    void* root = (void*)(uintptr_t)address;
    nlohmann::json arr = nlohmann::json::array();
    struct Item { void* ctrl; int depth; };
    std::vector<Item> stack;
    stack.push_back({ root, 0 });
    int guard = 0;
    while (!stack.empty() && guard < 400) {
        Item it = stack.back();
        stack.pop_back();
        ++guard;

        struct Child { void* ctrl; std::string name; const char* via; };
        std::vector<Child> kids;
        {
            void* vals = nullptr; void* keys = nullptr; uint32_t cnt = 0;
            if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x6F8), &vals) &&
                SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x710), &keys) &&
                SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x71C), &cnt) &&
                vals && keys && cnt > 0 && cnt <= 200) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    void* c = nullptr;
                    if (!SafeReadPtr((const void*)((uintptr_t)vals + (size_t)i * 8), &c) || !c) continue;
                    kids.push_back({ c, Utf8Safe(ExtractPdxString((void*)((uintptr_t)keys + (size_t)i * 48 + 16))), "map" });
                }
            }
        }
        {
            void* data = nullptr; uint32_t cnt = 0;
            if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x878), &data) &&
                SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x884), &cnt) &&
                data && cnt > 0 && cnt <= 200) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    void* c = nullptr;
                    if (!SafeReadPtr((const void*)((uintptr_t)data + (size_t)i * 8), &c) || !c) continue;
                    kids.push_back({ c, std::string(), "array" });
                }
            }
        }

        for (const Child& kid : kids) {
            std::string text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x168)));
            if (text.empty()) text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x268)));
            const bool hit = find.empty() || text.find(find) != std::string::npos;
            if (hit && (!kid.name.empty() || !text.empty())) {
                arr.push_back({{"depth", it.depth + 1}, {"via", kid.via}, {"name", kid.name},
                               {"text", text}, {"ptr", (uint64_t)(uintptr_t)kid.ctrl}});
            }
            if (it.depth + 1 < depth) stack.push_back({ kid.ctrl, it.depth + 1 });
        }
    }
    return { {"success", true}, {"address", address}, {"nodes", arr.size()}, {"controls", arr} };
}
// 只读：把一个视图对象名下所有指针都当窗口试走（深度 2），报出各窗口的控件数与文本样本。
// 用途：考古/首触这类视图有两个窗口——"面板"（site_name/难度/日志）和"剧情窗"（图片+正文+选项按钮），
// 后者不在 view+0x78 上，需要这样找。
// 只读：列出 CGameState +0x180 里所有「已打开的玩家事件」（COpenPlayerEvent）。
// 这张表是全局的，不依赖窗口数组——用来判断首触/考古这类弹窗背后到底有没有一个真实事件。
// 每条给：id、event_key（CEvent+0x20）、scope、选项数（CEvent+0x5E0 数组），
// 以及「有没有哪个事件窗的 +0xA1C 指向这个 id」（有 = 能按经典窗口处理）。
nlohmann::json EventManager::ListOpenEventsJson() {
    void* gs = nullptr;
    void** arr = nullptr;
    int count = 0;
    if (!SafeReadPtr((const void*)(base_address_ + sdk::glob::g_CurrentGameState), &gs) || !gs ||
        !SafeReadPtr((const void*)((uintptr_t)gs + kGsOpenEvents), (void**)&arr) || !arr ||
        !SafeReadInt((const void*)((uintptr_t)gs + kGsOpenEventCount), &count) || count <= 0 || count > 512) {
        return { {"success", true}, {"count", 0}, {"events", nlohmann::json::array()} };
    }

    // 先把 idler+0x170 那些事件窗的 open-event id 收起来，用来标记“这个事件有窗口”
    std::unordered_set<uint32_t> windowed;
    void* idler = GameState::Get().GetInGameIdler();
    if (idler) {
        void* varr = nullptr; uint32_t vcnt = 0;
        if (SafeReadPtr((const void*)((uintptr_t)idler + 0x170), &varr) &&
            SafeReadU32((const void*)((uintptr_t)idler + 0x17C), &vcnt) && varr && vcnt <= 64) {
            for (uint32_t i = 0; i < vcnt; ++i) {
                void* w = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)varr + (size_t)i * 8), &w) || !w) continue;
                uint32_t oid = 0xFFFFFFFF;
                if (SafeReadU32((const void*)((uintptr_t)w + kWinOpenEventId), &oid)) windowed.insert(oid);
            }
        }
    }

    nlohmann::json out = nlohmann::json::array();
    for (int i = 0; i < count; ++i) {
        void* ope = nullptr;
        if (!SafeReadPtr(&arr[i], &ope) || !ope) continue;
        uint32_t id = 0xFFFFFFFF;
        SafeReadU32((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::id), &id);
        void* handle = nullptr;
        void* event = nullptr;
        std::string key;
        if (SafeReadPtr((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::event), &handle) && handle &&
            SafeReadPtr(handle, &event) && event) {
            key = ExtractPdxString((void*)((uintptr_t)event + kEventKey));
        }
        uint32_t opt_count = 0;
        if (event) {
            void* opt_data = nullptr;
            SafeReadPtr((const void*)((uintptr_t)event + 0x5E0 + kArrayData), &opt_data);
            SafeReadU32((const void*)((uintptr_t)event + 0x5E0 + kArraySize), &opt_count);
        }
        // scope 就在 COpenPlayerEvent+0x10，可能是内联结构（16 字节）而不是指针——一并给出原始字节
        uint32_t scope_lo = 0, scope_hi = 0;
        SafeReadU32((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::scope), &scope_lo);
        SafeReadU32((const void*)((uintptr_t)ope + sdk::ent::COpenPlayerEvent::scope + 4), &scope_hi);
        nlohmann::json j = {
            {"id", id},
            {"ope", (uint64_t)(uintptr_t)ope},
            {"handle", (uint64_t)(uintptr_t)handle},
            {"event", (uint64_t)(uintptr_t)event},
            {"event_key", key},
            {"event_id", event ? (uint64_t)([&]() { uint32_t v = 0; SafeReadU32((const void*)((uintptr_t)event + 0x8), &v); return v; })() : 0},
            {"options", opt_count},
            {"scope_lo32", scope_lo},
            {"scope_hi32", scope_hi},
            {"has_window", windowed.count(id) > 0}
        };
        out.push_back(j);
    }
    return { {"success", true}, {"count", (int)out.size()}, {"windowed_ids", (int)windowed.size()}, {"events", out} };
}
nlohmann::json EventManager::ScanViewWindowsJson(uint64_t view_addr) {
    if (!view_addr) return { {"error", "view address is 0"} };
    void* view = (void*)(uintptr_t)view_addr;

    nlohmann::json out = nlohmann::json::array();
    for (int off = 0x40; off <= 0x400; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)view + off), &p) || !p) continue;
        if ((uintptr_t)p < 0x10000) continue;

        int nodes = 0;
        nlohmann::json texts = nlohmann::json::array();
        std::vector<std::pair<void*, int>> stack;
        stack.push_back({ p, 0 });
        int guard = 0;
        while (!stack.empty() && guard < 200) {
            auto it = stack.back(); stack.pop_back(); ++guard;
            void* vals = nullptr; void* keys = nullptr; uint32_t cnt = 0;
            if (!SafeReadPtr((const void*)((uintptr_t)it.first + 0x6F8), &vals) ||
                !SafeReadPtr((const void*)((uintptr_t)it.first + 0x710), &keys) ||
                !SafeReadU32((const void*)((uintptr_t)it.first + 0x71C), &cnt) ||
                !vals || !keys || cnt == 0 || cnt > 200) {
                continue;
            }
            for (uint32_t i = 0; i < cnt; ++i) {
                void* c = nullptr;
                if (!SafeReadPtr((const void*)((uintptr_t)vals + (size_t)i * 8), &c) || !c) continue;
                ++nodes;
                std::string name = Utf8Safe(ExtractPdxString((void*)((uintptr_t)keys + (size_t)i * 48 + 16)));
                std::string t = Utf8Safe(ExtractPdxString((void*)((uintptr_t)c + 0x168)));
                if (t.empty()) t = Utf8Safe(ExtractPdxString((void*)((uintptr_t)c + 0x268)));
                if (texts.size() < 6 && (!t.empty() || !name.empty())) {
                    texts.push_back({ {"name", name}, {"text", t} });
                }
                if (it.second + 1 < 2) stack.push_back({ c, it.second + 1 });
            }
        }
        if (nodes > 0) {
            char label[24]{};
            snprintf(label, sizeof(label), "+0x%X", off);
            out.push_back({ {"off", label}, {"ptr", (uint64_t)(uintptr_t)p}, {"nodes", nodes}, {"sample", texts} });
        }
    }
    return { {"success", true}, {"count", out.size()}, {"windows", out} };
}
// 只读：把 idler 里所有「看着像视图」的成员列出来——每个偏移一个视图指针，
// 再走它的窗口（深度 2）抓几个关键控件的文本，用来确认哪个视图对应眼前这个弹窗。
nlohmann::json EventManager::ListViewsJson() {
    if (!base_address_) return { {"error", "Bridge base address not set"} };
    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) return { {"error", "InGameIdler is null"} };

    const uint64_t base = (uint64_t)base_address_;
    const uint64_t data_end = base + 0x2600000;   // .rdata 大致上界

    auto text_of = [&](void* win, const char* want) -> std::string {
        if (!win) return "";
        std::string found;
        std::vector<std::pair<void*, int>> stack;
        stack.push_back({ win, 0 });
        int guard = 0;
        while (!stack.empty() && guard < 260 && found.empty()) {
            auto it = stack.back(); stack.pop_back(); ++guard;
            std::vector<std::pair<void*, std::string>> kids;
            void* vals = nullptr; void* keys = nullptr; uint32_t cnt = 0;
            if (SafeReadPtr((const void*)((uintptr_t)it.first + 0x6F8), &vals) &&
                SafeReadPtr((const void*)((uintptr_t)it.first + 0x710), &keys) &&
                SafeReadU32((const void*)((uintptr_t)it.first + 0x71C), &cnt) &&
                vals && keys && cnt > 0 && cnt <= 200) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    void* c = nullptr;
                    if (!SafeReadPtr((const void*)((uintptr_t)vals + (size_t)i * 8), &c) || !c) continue;
                    kids.push_back({ c, ExtractPdxString((void*)((uintptr_t)keys + (size_t)i * 48 + 16)) });
                }
            }
            for (auto& k : kids) {
                if (k.second == want) {
                    std::string t = Utf8Safe(ExtractPdxString((void*)((uintptr_t)k.first + 0x168)));
                    if (t.empty()) t = Utf8Safe(ExtractPdxString((void*)((uintptr_t)k.first + 0x268)));
                    if (!t.empty()) { found = t; break; }
                }
                if (it.second + 1 < 2) stack.push_back({ k.first, it.second + 1 });
            }
        }
        return found;
    };

    nlohmann::json out = nlohmann::json::array();
    for (int off = 0; off <= 0x2000; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)idler + off), &p) || !p) continue;
        if ((uintptr_t)p < 0x10000) continue;
        void* vt = nullptr;
        if (!SafeReadPtr(p, &vt) || !vt) continue;
        const uint64_t vtq = (uint64_t)(uintptr_t)vt;
        if (vtq < base || vtq >= data_end) continue;          // 只认指向映像内的 vtable
        void* win = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)p + 0x78), &win) || !win) continue;

        char label[24]{};
        snprintf(label, sizeof(label), "+0x%X", off);
        nlohmann::json item = {
            {"idler_off", label},
            {"view", (uint64_t)(uintptr_t)p},
            {"vtable_rva", vtq - base}
        };
        // CArchaeologyView 在 +0x6D0 存「当前遗址句柄」（slot2/slot4 用它查库），
        // 记下来比从界面文本猜遗址可靠。
        uint32_t h = 0;
        if (SafeReadU32((const void*)((uintptr_t)p + 0x6D0), &h) && h != 0 && h != 0xFFFFFFFF) {
            item["handle_6d0"] = h & 0xFFFFFF;
        }
        std::string site = text_of(win, "site_name");
        std::string title = text_of(win, "title_text");
        std::string sub = text_of(win, "chapter_subtitle");
        std::string clues = text_of(win, "clues_value");
        if (!site.empty()) item["site_name"] = site;
        if (!title.empty()) item["title_text"] = title;
        if (!sub.empty()) item["chapter_subtitle"] = sub;
        if (!clues.empty()) item["clues_value"] = clues;
        out.push_back(item);
    }
    return { {"success", true}, {"count", out.size()}, {"views", out} };
}
// 只读：在 idler 里找「哪个对象的控件树里含某段文字」。用于定位那些既不是事件窗、
// 也不在那三个已知视图上的弹窗（考古遗址把自己的选项画在一个自定义面板里）。
// 做法：把 idler 的每个 qword 当候选根，两种解释都试——① 根本身是 gui window；
// ② 根是 view，取 +0x78 当 gui window —— 再走深度有限的控件树，比对文本。
nlohmann::json EventManager::FindGuiByTextJson(const std::string& needle, int max_depth) {
    if (needle.empty()) return { {"error", "needle is empty"} };
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 3) max_depth = 3;

    void* idler = GameState::Get().GetInGameIdler();
    if (!idler) return { {"error", "InGameIdler is null"} };

    auto walk = [&](void* root, int depth) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        if (!root) return arr;
        struct Item { void* ctrl; int depth; };
        std::vector<Item> stack;
        stack.push_back({ root, 0 });
        int guard = 0;
        while (!stack.empty() && guard < 300) {
            Item it = stack.back();
            stack.pop_back();
            ++guard;

            struct Child { void* ctrl; std::string name; };
            std::vector<Child> kids;
            {
                void* vals = nullptr; void* keys = nullptr; uint32_t cnt = 0;
                if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x6F8), &vals) &&
                    SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x710), &keys) &&
                    SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x71C), &cnt) &&
                    vals && keys && cnt > 0 && cnt <= 200) {
                    for (uint32_t i = 0; i < cnt; ++i) {
                        void* c = nullptr;
                        if (!SafeReadPtr((const void*)((uintptr_t)vals + (size_t)i * 8), &c) || !c) continue;
                        kids.push_back({ c, Utf8Safe(ExtractPdxString((void*)((uintptr_t)keys + (size_t)i * 48 + 16))) });
                    }
                }
            }
            {
                void* data = nullptr; uint32_t cnt = 0;
                if (SafeReadPtr((const void*)((uintptr_t)it.ctrl + 0x878), &data) &&
                    SafeReadU32((const void*)((uintptr_t)it.ctrl + 0x884), &cnt) &&
                    data && cnt > 0 && cnt <= 200) {
                    for (uint32_t i = 0; i < cnt; ++i) {
                        void* c = nullptr;
                        if (!SafeReadPtr((const void*)((uintptr_t)data + (size_t)i * 8), &c) || !c) continue;
                        kids.push_back({ c, std::string() });
                    }
                }
            }

            for (const Child& kid : kids) {
                std::string text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x168)));
                if (text.empty()) text = Utf8Safe(ExtractPdxString((void*)((uintptr_t)kid.ctrl + 0x268)));
                if (text.find(needle) != std::string::npos) {
                    arr.push_back({ {"name", kid.name}, {"text", text},
                                    {"ptr", (uint64_t)(uintptr_t)kid.ctrl}, {"depth", it.depth + 1} });
                }
                if (it.depth + 1 < depth) stack.push_back({ kid.ctrl, it.depth + 1 });
            }
        }
        return arr;
    };

    nlohmann::json hits = nlohmann::json::array();
    for (int off = 0; off <= 0x2000 && hits.size() < 8; off += 8) {
        void* p = nullptr;
        if (!SafeReadPtr((const void*)((uintptr_t)idler + off), &p) || !p) continue;
        if ((uintptr_t)p < 0x10000) continue;

        // ① 根是 view：gui window 在 +0x78
        void* win = nullptr;
        if (SafeReadPtr((const void*)((uintptr_t)p + 0x78), &win) && win) {
            nlohmann::json got = walk(win, max_depth);
            if (!got.empty()) {
                char label[32]{};
                snprintf(label, sizeof(label), "idler+0x%X -> view+0x78", off);
                hits.push_back({ {"root", label}, {"ptr", (uint64_t)(uintptr_t)p},
                                 {"gui_window", (uint64_t)(uintptr_t)win}, {"matches", got} });
                continue;
            }
        }
        // ② 根本身就是 gui window
        nlohmann::json got = walk(p, max_depth);
        if (!got.empty()) {
            char label[32]{};
            snprintf(label, sizeof(label), "idler+0x%X", off);
            hits.push_back({ {"root", label}, {"ptr", (uint64_t)(uintptr_t)p}, {"matches", got} });
        }
    }

    return { {"success", true}, {"needle", needle}, {"hits", hits} };
}

} // namespace bridge