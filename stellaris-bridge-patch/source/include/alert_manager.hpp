#pragma once

#include "common.hpp"

namespace bridge {

struct AlertItem {
    uint32_t alert_id{ 0 };
    std::string type;
    std::string title;
    std::string description;
    std::string tooltip;
};

class AlertManager {
public:
    static AlertManager& Get();

    bool Init(uintptr_t base_address);

    // Call on main thread to get active alerts snapshot
    std::vector<AlertItem> GetAlerts();
    nlohmann::json GetAlertsJson();

    // Call on main thread to trigger alert click (open full view / window)
    nlohmann::json OpenAlert(uint32_t alert_id);

    // 只读探针：某条警报的子窗口原始字节（判断它到底指向谁时用）
    nlohmann::json ProbeAlertWindowJson(uint32_t alert_id, uint32_t length);

    using FnOnAlertClick = void(*)(void* alert_win, void* banner_button);

    // engine text without its formatting codes and icons
    std::string CleanPdxString(const std::string& input);

private:
    AlertManager() = default;
    ~AlertManager() = default;

    bool ReadAlertWindow(void* idler, void** out);
    uintptr_t base_address_{ 0 };
    FnOnAlertClick fn_on_alert_click_{ nullptr };

    std::string ExtractPdxString(void* ptr);
    std::string GetAlertTypeName(uint32_t alert_id);
};

} // namespace bridge
