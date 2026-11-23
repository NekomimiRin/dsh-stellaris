#pragma once

#include "common.hpp"

namespace bridge {

struct EventOptionInfo {
    int index{ 0 };
    std::string text;
    bool is_valid{ true };
    std::string effects;  // the option's effect tooltip, as the game shows it on hover
};

struct EventInfo {
    uint32_t window_id{ 0 };
    std::string title;
    std::string description;
    std::string event_key;                 // script id, e.g. "colony.1"
    uint32_t open_event_id{ 0xFFFFFFFF };  // COpenPlayerEvent id behind a standard event window
    std::vector<EventOptionInfo> options;
};

class EventManager {
public:
    static constexpr uint32_t START_SCREEN_EVENT_ID = 0xFFFF0001;
    static constexpr uint32_t ANOMALY_EVENT_ID = 0xFFFF0002;
    static constexpr uint32_t FIRST_CONTACT_EVENT_ID = 0xFFFF0003;

    static EventManager& Get();

    bool Init(uintptr_t base_address);

    // Call on main thread to get active events snapshot
    std::vector<EventInfo> GetActiveEvents();
    nlohmann::json GetActiveEventsJson();
    // the windows waiting for the player, compact (window_id, kind, event_key, title)
    nlohmann::json GetPendingJson();

    // Call on main thread to resolve an event choice
    nlohmann::json ResolveEvent(uint32_t window_id, int option_index);

    // Read-only probe: the shown first-contact view's gui controls, as a name/text tree
    // (its real buttons are NOT under the options_box that GetActiveEvents walks)
    nlohmann::json ProbeFirstContactViewJson(int max_depth, const std::string& find_text);

    // Read-only probe: search the in-game idler for whichever object's gui control tree contains
    // a control whose caption holds `needle`. Used to locate popups that are neither event windows
    // nor one of the three idler views (archaeology sites render their options in a custom panel).
    nlohmann::json FindGuiByTextJson(const std::string& needle, int max_depth);

    // Read-only probe: walk any gui-window address, optionally only reporting controls whose
    // caption holds `find`. For checking the sub-windows a view owns.
    nlohmann::json WalkGuiJson(uint64_t address, int depth, const std::string& find);

    // Read-only probe: look for CEventWindow-shaped objects reachable from the idler
    // (+0x9E8 option-button count, +0x98 window id, +0xB8 scope). Popups that are not in the
    // idler's window array still may be event windows hanging off some other object.
    nlohmann::json FindEventWindowsJson(int depth);

    // Read-only probe: every idler member that looks like a view, with a few control texts
    // from its window — to tell which view is the popup actually on screen.
    nlohmann::json ListViewsJson();

    // Read-only probe: try every pointer inside a view as a gui window and report the ones
    // that have children — finds the view's second window (story panel with the option buttons).
    // 只读：列出所有已打开的玩家事件（CGameState +0x180），含 event_key 与“有无窗口”
    nlohmann::json ListOpenEventsJson();
    // 用 COpenPlayerEvent 指针直接选选项——不依赖 CEventWindow。
    // 走 UI 点击那条链：CEventWindow::PostEventOptionSelection 找到 open event 之后调的那个 worker
    // （RVA 0x8E4550），它自己构造 CSelectEventOptionCommand 并派发。
    // 首触 / 考古 / 裂隙的弹窗没有 CEventWindow，但背后都有 open event，所以这条路对它们可用。
    nlohmann::json SelectOpenEventOptionJson(uint64_t ope_addr, int option_index, bool dry_run);
    // 只读：给 ope 或 open event id，返回选项明细（下标/文本/能否选/效果）
    nlohmann::json GetEventOptionsJson(uint64_t ope_addr, uint32_t open_event_id);

    // 只对三个已知视图（考古 / 首触 / 裂隙）调用它 vtable 的某一槽。
    // 用途：弹窗文本只在视图自己的刷新槽里重写——引擎侧改了数据它不会自己更新
    // （2026-11-19 深夜：首触 +0xC0 变了它会刷；考古答完事件它不刷）。
    nlohmann::json CallViewSlotJson(uint64_t view_addr, int slot);

        nlohmann::json ScanViewWindowsJson(uint64_t view_addr);

private:
    void ReadEventData(void* win, EventInfo& info);
    void ReadShownOptions(void* win, void* event, EventInfo& info);
    void ReadOpenEventOptions(void* ope, void* event, EventInfo& info);
public:

    using FnFindChild = void*(*)(void* container, void* pdx_string);
    using FnSelectOption = void(*)(void* event_window, int option_index);

private:
    EventManager() = default;
    ~EventManager() = default;

    uintptr_t base_address_{ 0 };

    FnFindChild fn_find_child_{ nullptr };
    FnSelectOption fn_select_option_{ nullptr };

    void* ShownView(void* idler, std::ptrdiff_t member, uintptr_t vtable_rva);
    bool HideView(void* view);
    void* FindChildByName(void* container, const char* name);
    std::string ExtractPdxString(void* ptr);
};

} // namespace bridge
