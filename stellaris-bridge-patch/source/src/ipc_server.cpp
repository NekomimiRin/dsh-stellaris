#include "ipc_server.hpp"
#include "command_builder.hpp"
#include "task_queue.hpp"
#include "game_state.hpp"
#include "commands.hpp"
#include "event_manager.hpp"
#include "notification_manager.hpp"
#include "alert_manager.hpp"
#include "tech_manager.hpp"
#include "situation_log_manager.hpp"
#include "galaxy_manager.hpp"
#include "government_manager.hpp"
#include "society_manager.hpp"
#include "leader_manager.hpp"
#include "species_manager.hpp"
#include "fleet_manager.hpp"
#include "ship_designer.hpp"
#include "market_manager.hpp"
#include "discoveries_manager.hpp"
#include "contacts_manager.hpp"
#include "outliner_manager.hpp"
#include "menu_manager.hpp"
#include "empire_design_manager.hpp"
#include "galaxy_setup_manager.hpp"

namespace bridge {

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\stellaris_mcp_bridge";

IPCServer& IPCServer::Get() {
    static IPCServer instance;
    return instance;
}

bool IPCServer::Start() {
    if (is_running_) return true;

    // Refuse to share the pipe name: a stale game instance or a bridge that did not unload
    // cleanly would otherwise receive a random share of the client connections.
    if (WaitNamedPipeW(kPipeName, 1) || GetLastError() != ERROR_FILE_NOT_FOUND) {
        LOG("[IPC_FATAL] Pipe stellaris_mcp_bridge is already served by another bridge "
            "(another stellaris.exe, or a previous injection still loaded). Not starting.");
        return false;
    }

    is_running_ = true;
    worker_thread_ = std::thread(&IPCServer::WorkerLoop, this);
    LOG("[IPC] Named Pipe server thread started.");
    return true;
}

void IPCServer::Stop() {
    if (!is_running_) return;

    is_running_ = false;

    // Connect to the pipe locally to unblock ConnectNamedPipe
    HANDLE hDummy = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr
    );
    if (hDummy != INVALID_HANDLE_VALUE) {
        CloseHandle(hDummy);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.detach();
    }

    LOG("[IPC] Named Pipe server stopped.");
}

nlohmann::json IPCServer::ProcessRequest(const nlohmann::json& req) {
    auto id = req.value("id", nlohmann::json(nullptr));
    std::string method = req.value("method", "");
    nlohmann::json params = req.value("params", nlohmann::json::object());

    // answered here, not on the main thread: memory reads only, and they must work while the game
    // loads (no tasks run then)
    if (method == "get_screen") {
        return { {"jsonrpc", "2.0"}, {"result", MenuManager::Get().ScreenJson()}, {"id", id} };
    }
    if (method == "get_status" && MenuManager::Get().Screen() != "in_game") {
        nlohmann::json r = MenuManager::Get().ScreenJson();
        r["in_game"] = false;
        return { {"jsonrpc", "2.0"}, {"result", r}, {"id", id} };
    }

    if (method == "ping") {
        return {
            {"jsonrpc", "2.0"},
            {"result", {{"status", "pong"}}},
            {"id", id}
        };
    }

    std::future<nlohmann::json> fut;

    if (method == "self_test_purecall_guard") {
        fut = TaskQueue::Get().Enqueue([]() {
            return CommandBuilder::Get().SelfTestPurecallGuard();
        });
    } else if (method == "run_console") {
        // Debug/test helper: runs one line in the in-game console (e.g. "effect ...").
        std::string line = params.value("command", std::string());
        fut = TaskQueue::Get().Enqueue([line]() {
            std::string error;
            bool ok = CommandBuilder::Get().RunConsoleCommand(line, &error);
            nlohmann::json r = {{"ok", ok}, {"command", line}};
            if (!ok) r["error"] = error;
            return r;
        });
    } else if (method == "get_status") {
        fut = TaskQueue::Get().Enqueue([]() {
            return GameState::Get().GetStatusJson();
        });
    } else if (method == "set_paused") {
        bool paused = params.value("paused", true);
        fut = TaskQueue::Get().Enqueue([paused]() {
            return Commands::Get().SetPaused(paused);
        });
    } else if (method == "set_speed") {
        uint32_t speed = params.value("speed", 0);
        fut = TaskQueue::Get().Enqueue([speed]() {
            return Commands::Get().SetSpeed(speed);
        });
    } else if (method == "load_save") {
        std::string campaign = params.value("campaign", std::string());
        std::string file = params.value("file", std::string());
        fut = TaskQueue::Get().Enqueue([campaign, file]() { return MenuManager::Get().LoadSave(campaign, file); });
    } else if (method == "list_empire_designs") {
        fut = TaskQueue::Get().Enqueue([]() { return EmpireDesignManager::Get().ListJson(); });
    } else if (method == "get_empire_design") {
        std::string key = params.value("key", std::string());
        fut = TaskQueue::Get().Enqueue([key]() { return EmpireDesignManager::Get().DetailJson(key); });
    } else if (method == "save_empire_design") {
        std::string file = params.value("file", std::string());
        bool validate_only = params.value("validate_only", false);
        bool overwrite = params.value("overwrite", false);
        fut = TaskQueue::Get().Enqueue([file, validate_only, overwrite]() {
            return EmpireDesignManager::Get().SaveFromFile(file, validate_only, overwrite);
        });
    } else if (method == "delete_empire_design") {
        std::string key = params.value("key", std::string());
        fut = TaskQueue::Get().Enqueue([key]() { return EmpireDesignManager::Get().Delete(key); });
    } else if (method == "get_attention") {
        fut = TaskQueue::Get().Enqueue([]() { return GameState::Get().GetAttentionJson(); });
    } else if (method == "get_galaxy_settings") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxySetupManager::Get().GetJson(); });
    } else if (method == "set_galaxy_settings") {
        nlohmann::json settings = params.value("settings", nlohmann::json::object());
        fut = TaskQueue::Get().Enqueue([settings]() { return GalaxySetupManager::Get().Set(settings); });
    } else if (method == "start_new_game") {
        std::string empire = params.value("empire", std::string());
        fut = TaskQueue::Get().Enqueue([empire]() { return MenuManager::Get().StartNewGame(empire); });
    } else if (method == "finish_load") {
        fut = TaskQueue::Get().Enqueue([]() { return MenuManager::Get().FinishLoad(); });
    } else if (method == "exit_to_main_menu") {
        fut = TaskQueue::Get().Enqueue([]() { return MenuManager::Get().ExitToMainMenu(); });
    } else if (method == "save_game") {
        std::string name = params.value("name", std::string());
        bool overwrite = params.value("overwrite", false);
        fut = TaskQueue::Get().Enqueue([name, overwrite]() { return MenuManager::Get().SaveGame(name, overwrite); });
    } else if (method == "get_active_events") {
        fut = TaskQueue::Get().Enqueue([]() {
            return EventManager::Get().GetActiveEventsJson();
        });
    } else if (method == "resolve_event") {
        uint32_t window_id = params.value("window_id", 0);
        int option_index = params.value("option_index", 0);
        fut = TaskQueue::Get().Enqueue([window_id, option_index]() {
            return EventManager::Get().ResolveEvent(window_id, option_index);
        });
    } else if (method == "get_notifications") {
        fut = TaskQueue::Get().Enqueue([]() {
            return NotificationManager::Get().GetNotificationsJson();
        });
    } else if (method == "open_notification" || method == "probe_notification") {
        uint32_t index = params.value("index", 0);
        const bool probe = (method == "probe_notification");
        fut = TaskQueue::Get().Enqueue([index, probe]() {
            return probe ? NotificationManager::Get().ProbeNotificationJson(index)
                         : NotificationManager::Get().OpenNotification(index);
        });
    } else if (method == "get_alerts") {
        fut = TaskQueue::Get().Enqueue([]() {
            return AlertManager::Get().GetAlertsJson();
        });
    } else if (method == "probe_alert_window") {
        uint32_t alert_id = params.value("alert_id", 0u);
        uint32_t length = params.value("length", 0x100u);
        fut = TaskQueue::Get().Enqueue([alert_id, length]() {
            return AlertManager::Get().ProbeAlertWindowJson(alert_id, length);
        });
    } else if (method == "open_alert") {
        uint32_t alert_id = params.value("alert_id", 0);
        fut = TaskQueue::Get().Enqueue([alert_id]() {
            return AlertManager::Get().OpenAlert(alert_id);
        });
    } else if (method == "get_research_state") {
        fut = TaskQueue::Get().Enqueue([]() {
            return TechManager::Get().GetResearchStateJson();
        });
    } else if (method == "get_researched_techs") {
        fut = TaskQueue::Get().Enqueue([]() {
            return TechManager::Get().GetResearchedTechsJson();
        });
    } else if (method == "select_research") {
        uint32_t area = 0;
        if (params.contains("area")) {
            if (params["area"].is_number()) {
                area = params["area"].get<uint32_t>();
            } else if (params["area"].is_string()) {
                std::string a_str = params["area"].get<std::string>();
                if (a_str == "physics" || a_str == "0") area = 0;
                else if (a_str == "society" || a_str == "1") area = 1;
                else if (a_str == "engineering" || a_str == "2") area = 2;
            }
        }
        std::string tech_key = params.value("tech_key", "");
        fut = TaskQueue::Get().Enqueue([area, tech_key]() {
            return TechManager::Get().SelectResearch(area, tech_key);
        });
    } else if (method == "cancel_research") {
        uint32_t area = 0;
        if (params.contains("area")) {
            if (params["area"].is_number()) {
                area = params["area"].get<uint32_t>();
            } else if (params["area"].is_string()) {
                std::string a_str = params["area"].get<std::string>();
                if (a_str == "physics" || a_str == "0") area = 0;
                else if (a_str == "society" || a_str == "1") area = 1;
                else if (a_str == "engineering" || a_str == "2") area = 2;
            }
        }
        fut = TaskQueue::Get().Enqueue([area]() {
            return TechManager::Get().CancelResearch(area);
        });
    } else if (method == "get_situation_log") {
        bool player_only = params.value("player_only", true);
        fut = TaskQueue::Get().Enqueue([player_only]() {
            return SituationLogManager::Get().GetSituationLogJson(player_only);
        });
    } else if (method == "get_situation_approaches") {
        uint32_t situation_id = params.value("situation_id", 0u);
        fut = TaskQueue::Get().Enqueue([situation_id]() {
            return SituationLogManager::Get().GetSituationApproachesJson(situation_id);
        });
    } else if (method == "probe_situation") {
        uint32_t situation_id = params.value("situation_id", 0u);
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x100u);
        uint64_t vec_vtable = params.value("vec_vtable", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([situation_id, offset, length, vec_vtable]() {
            return SituationLogManager::Get().ProbeSituationJson(situation_id, offset, length, vec_vtable);
        });
    } else if (method == "probe_situation_type") {
        uint32_t situation_id = params.value("situation_id", 0u);
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x200u);
        bool follow = params.value("follow", false);
        uint64_t match_vtable = params.value("match_vtable", (uint64_t)0);
        uint64_t vec_vtable = params.value("vec_vtable", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([situation_id, offset, length, follow, match_vtable, vec_vtable]() {
            return SituationLogManager::Get().ProbeSituationTypeJson(situation_id, offset, length, follow, match_vtable, vec_vtable);
        });
    } else if (method == "get_galaxy_overview") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetOverviewJson(); });
    } else if (method == "get_galaxy_map" || method == "get_bypasses" || method == "get_all_fleets" ||
               method == "get_fleet_templates" || method == "get_all_designs" ||
               method == "get_all_megastructures" || method == "find_arc_furnace_systems" ||
               method == "find_dyson_swarm_systems") {
        // 三个方法并一个分支：ipc_server 的 else-if 链顶到过 C1061（块嵌套太深），
        // 再单开一支就编不过；这里用 which 在 lambda 里分派。
        uint32_t center = params.value("center_system_id", 0xFFFFFFFFu);
        int jumps = params.value("jumps", -1);  // 不给 = 全银河；给了就是中心起算的跳数
        const int which = (method == "get_bypasses")   ? 1
                          : (method == "get_all_fleets") ? 2
                          : (method == "get_fleet_templates") ? 3
                          : (method == "get_all_designs") ? 4
                          : (method == "get_all_megastructures") ? 5
                          : (method == "find_arc_furnace_systems") ? 6
                          : (method == "find_dyson_swarm_systems") ? 7
                                                              : 0;
        std::string type_filter = params.value("type_filter", "");
        bool foreign_only = params.value("foreign_only", true);
        uint32_t fleet_limit = params.value("limit", 500u);
        nlohmann::json design_params = params;
        fut = TaskQueue::Get().Enqueue([center, jumps, which, type_filter, foreign_only, fleet_limit, design_params]() {
            auto& g = GalaxyManager::Get();
            if (which == 1) return g.GetBypassesJson();
            if (which == 2) return g.GetFleetsAllJson(type_filter, foreign_only, fleet_limit);
            if (which == 3) return g.GetFleetTemplatesJson();
            if (which == 4) return ShipDesigner::Get().GetAllDesignsJson(design_params);
            if (which == 5) return g.GetMegastructuresJson();
            if (which == 7) {
                const uint32_t limit = design_params.value("limit", 30u);
                const uint32_t fleet_id = design_params.value("fleet_id", 0xFFFFFFFFu);
                const uint32_t from_system_id = design_params.value("from_system_id", 0xFFFFFFFFu);
                const std::string sort_key = design_params.value("sort", std::string("jumps"));
                return g.FindDysonSwarmSystems(limit, fleet_id, from_system_id, sort_key == "value");
            }
            if (which == 6) {
                const uint32_t min_bodies = design_params.value("min_bodies", 0u);
                const uint32_t limit = design_params.value("limit", 30u);
                const uint32_t fleet_id = design_params.value("fleet_id", 0xFFFFFFFFu);
                const uint32_t from_system_id = design_params.value("from_system_id", 0xFFFFFFFFu);
                const std::string sort_key = design_params.value("sort", std::string("bodies"));
                return g.FindArcFurnaceSystems(min_bodies, limit, fleet_id, from_system_id, sort_key == "jumps");
            }
            return g.GetMapJson(center, jumps);
        });
    } else if (method == "get_system") {
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([system_id]() { return GalaxyManager::Get().GetSystemJson(system_id); });
    } else if (method == "research_project") {
        uint32_t project_id = params.value("project_id", 0xFFFFFFFFu);
        bool cancel = params.value("cancel", false);
        fut = TaskQueue::Get().Enqueue([=]() {
            return SituationLogManager::Get().ResearchProjectJson(project_id, cancel);
        });
    } else if (method == "probe_anomaly") {
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        int32_t offset = params.value("offset", 0);
        int32_t length = params.value("length", 0x80);
        fut = TaskQueue::Get().Enqueue([=]() {
            return SituationLogManager::Get().ProbeAnomalyJson(planet_id, offset, length);
        });
    } else if (method == "probe_fleet_orders") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id]() {
            return GalaxyManager::Get().ProbeFleetOrdersJson(fleet_id);
        });
    } else if (method == "scan_fleet_orders") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        int depth = params.value("depth", 2);
        fut = TaskQueue::Get().Enqueue([=]() {
            return GalaxyManager::Get().ScanFleetOrdersJson(fleet_id, planet_id, depth);
        });
    } else if (method == "research_anomaly") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([=]() {
            return GalaxyManager::Get().ResearchAnomaly(fleet_id, planet_id, queue);
        });
    } else if (method == "get_astral_rifts") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetAstralRiftsJson(); });
    } else if (method == "get_player_country") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetPlayerCountryJson(); });
    } else if (method == "probe_astral_rift") {
        uint32_t rift_id = params.value("rift_id", 0u);
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x100u);
        fut = TaskQueue::Get().Enqueue([rift_id, offset, length]() {
            return GalaxyManager::Get().ProbeAstralRiftJson(rift_id, offset, length);
        });
    } else if (method == "probe_system") {
        uint32_t system_id = params.value("system_id", 0u);
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x100u);
        fut = TaskQueue::Get().Enqueue([system_id, offset, length]() {
            return GalaxyManager::Get().ProbeSystemJson(system_id, offset, length);
        });
    } else if (method == "scan_rift_systems") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().ScanRiftSystemsJson(); });
    } else if (method == "scan_astral_scar" || method == "get_wars") {
        // 两条并一条：ipc_server 的 if-else 链已经顶到编译器的嵌套上限（C1061），不能再加一层
        const bool want_wars = (method == "get_wars");
        fut = TaskQueue::Get().Enqueue([want_wars]() {
            return want_wars ? GalaxyManager::Get().GetWarsJson() : GalaxyManager::Get().ScanAstralScarJson();
        });
    } else if (method == "explore_astral_rift") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t rift_id = params.value("rift_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        bool dry_run = params.value("dry_run", false);        fut = TaskQueue::Get().Enqueue([fleet_id, rift_id, queue, dry_run]() {
            return GalaxyManager::Get().ExploreAstralRift(fleet_id, rift_id, queue, dry_run);
        });
    } else if (method == "probe_rift_explorability") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id]() { return GalaxyManager::Get().ProbeRiftExplorability(fleet_id); });
    } else if (method == "get_archaeological_sites") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetArchaeologicalSitesJson(); });
    } else if (method == "probe_excavatability") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id]() { return GalaxyManager::Get().ProbeExcavatability(fleet_id); });
    } else if (method == "move_fleet") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, system_id, queue]() {
            return GalaxyManager::Get().MoveFleet(fleet_id, system_id, queue);
        });
    } else if (method == "build_outpost") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, system_id, queue]() {
            return GalaxyManager::Get().BuildOutpost(fleet_id, system_id, queue);
        });
    } else if (method == "get_diplomatic_actions") {
        uint32_t country_id = params.value("country_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([country_id]() { return ContactsManager::Get().GetDiplomaticActionsJson(country_id); });
    } else if (method == "propose_diplomatic_action" || method == "set_defender_war_goal" || method == "probe_war" ||
               method == "probe_diplomatic_action" || method == "probe_country" || method == "probe_federation_types" ||
               method == "probe_federation_type_db" || method == "probe_federation" || method == "get_federations" ||
               method == "probe_federation_laws" || method == "get_federation_laws" ||
               method == "probe_law_db" || method == "propose_federation_law" || method == "propose_federation_type" || method == "propose_trade_deal" || method == "probe_trade_deals" || method == "probe_windows" ||
               method == "propose_capture" || method == "propose_capture_clear" ||
               method == "clone_capture" || method == "clone_capture_clear" || method == "cmd_capture" || method == "cmd_capture_clear" ||
               method == "get_incoming_actions" || method == "respond_diplo_action" || method == "vote_diplo_action" ||
               method == "get_pending_votes" || method == "declare_war" || method == "probe_war_goals" ||
               method == "probe_community_membership" || method == "install_community_capture" ||
               method == "read_community_capture" || method == "get_resolutions" ||
               method == "install_agreement_capture" || method == "get_agreements" || method == "probe_agreement_action" ||
               method == "probe_resolution_types" || method == "propose_resolution" || method == "vote_resolution" || method == "end_senate_session" || method == "queued_resolution_action" ||
               method == "try_peace_offer") {
        // 九条并一条：if-else 链已顶到编译器嵌套上限（C1061）
        const bool want_goal = (method == "set_defender_war_goal");
        const bool want_probe_action = (method == "probe_diplomatic_action");
        const bool want_probe_country = (method == "probe_country");
        const bool want_fed_types = (method == "probe_federation_types");
        const bool want_fed_type_db = (method == "probe_federation_type_db");
        const bool want_fed_probe = (method == "probe_federation");
        const bool want_feds = (method == "get_federations");
        const bool want_fed_laws = (method == "probe_federation_laws");
        const bool want_fed_law_state = (method == "get_federation_laws");
        const bool want_law_db = (method == "probe_law_db");
        const bool want_law_propose = (method == "propose_federation_law");
        const std::string law_key = params.value("law_key", std::string());
        uint32_t fed_id = params.value("fed_id", 0u);
        const bool want_capture = (method == "propose_capture");
        const bool want_capture_clear = (method == "propose_capture_clear");
        const bool want_clone = (method == "clone_capture");
        const bool want_clone_clear = (method == "clone_capture_clear");
        const bool want_cmd_capture = (method == "cmd_capture");
        const bool want_cmd_clear = (method == "cmd_capture_clear");
        const bool want_try_peace = (method == "try_peace_offer");
        const bool want_incoming = (method == "get_incoming_actions");
        const bool want_pending_votes = (method == "get_pending_votes");
        const bool want_declare_war = (method == "declare_war");
        const bool want_war_goals = (method == "probe_war_goals");
        bool war_goals_all = params.value("all", false);
        const bool want_respond = (method == "respond_diplo_action");
        const bool want_vote = (method == "vote_diplo_action");
        uint64_t action_id = params.value("id", (uint64_t)0);
        bool respond_accept = params.value("accept", false);
        int vote_value = params.value("vote", 1);
        uint32_t voter_id = params.value("voter", 0xFFFFFFFFu);
        uint32_t country_id = params.value("country_id", 0xFFFFFFFFu);
        int peace_status = params.value("status", 0);
        bool peace_synth = params.value("synth", true);   // 默认走无模板路径（2026-11-20 解出 +0x40 = 动作类型对象）
        uint64_t peace_f40 = params.value("f40", (uint64_t)0);
        uint32_t peace_f4c = params.value("f4c", 0u);
        // force=true：跳过我们自己的 IsValid，只留 PostCommand 的门（界面投递就是这样，不加那道校验）
        bool peace_force = params.value("force", false);
        std::string action = params.value("action", std::string());
        // 2026-11-21：组建联邦的类型（CInviteToFederationAction +0xB0）；空串 = 用引擎默认
        std::string federation_type = params.value("federation_type", std::string());
        // 2026-11-21：带条款地发协议类动作（UI 的发送方式）
        nlohmann::json agr_terms = params.value("terms", nlohmann::json::object());
        nlohmann::json agr_subsidies = params.value("subsidies", nlohmann::json::object());
        // 2026-11-21：贸易协定（两侧各两张 26 格资源表 + 年数）
        const bool want_fed_type = (method == "propose_federation_type");
        const std::string fed_type_key = params.value("type_key", std::string());
        const bool want_probe_trade_deals = (method == "probe_trade_deals");
        const bool want_probe_windows = (method == "probe_windows");
        const bool want_trade_deal = (method == "propose_trade_deal");
        const nlohmann::json trade_give = params.value("give", nlohmann::json::array());
        const nlohmann::json trade_monthly_give = params.value("monthly_give", nlohmann::json::array());
        const nlohmann::json trade_take = params.value("take", nlohmann::json::array());
        const nlohmann::json trade_monthly_take = params.value("monthly_take", nlohmann::json::array());
        const int trade_years = params.value("years", 10);
        const bool trade_reserve = params.value("reserve", true);
        const bool trade_sensor_charts = params.value("sensor_charts", false);
        const bool trade_sensor_link = params.value("sensor_link", false);
        const nlohmann::json trade_specimens = params.value("specimens", nlohmann::json::array());
        const nlohmann::json trade_fleets = params.value("fleets", nlohmann::json::array());
        const bool want_agr_probe = (method == "probe_agreement_action");
        const bool agr_do_write = params.value("write", false);
        const bool agr_do_delete = params.value("delete", false);
        // probe_federation_types 要验的那一个类型（空 = 只列清单）
        std::string fed_probe_type = params.value("type", std::string());
        uint32_t war_id = params.value("war_id", 0u);
        std::string goal_key = params.value("goal_key", std::string());
        bool dry_run = params.value("dry_run", false);
        bool swap = params.value("swap", false);
        uint32_t probe_offset = params.value("offset", 0u);
        uint32_t probe_length = params.value("length", 0x100u);
        bool probe_follow = params.value("follow", false);
        const bool want_probe_war = (method == "probe_war");
        const bool want_community = (method == "probe_community_membership");
        const bool want_comm_install = (method == "install_community_capture");
        const bool want_comm_read = (method == "read_community_capture");
        const bool want_agr_install = (method == "install_agreement_capture");
        const bool want_agreements = (method == "get_agreements");
        const bool want_resolutions = (method == "get_resolutions");
        const bool want_res_types = (method == "probe_resolution_types");
        const bool want_propose_res = (method == "propose_resolution");
        const bool want_vote_res = (method == "vote_resolution");
        const bool want_end_session = (method == "end_senate_session");
        const bool want_queued = (method == "queued_resolution_action");
        uint32_t res_id = params.value("resolution_id", 0xFFFFFFFFu);
        int res_vote = params.value("vote", 1);
        const std::string res_key = params.value("type_key", std::string());
        bool res_types_all = params.value("all", false);
        uint32_t res_target = params.value("target_id", 0xFFFFFFFFu);
        std::vector<uint32_t> community_ids;
        if (want_community && params.contains("country_ids") && params["country_ids"].is_array()) {
            for (const auto& v : params["country_ids"]) {
                if (v.is_number()) community_ids.push_back(v.get<uint32_t>());
            }
        }
        fut = TaskQueue::Get().Enqueue([want_goal, want_probe_war, want_probe_action, want_probe_country, want_fed_types, want_fed_type_db, want_fed_probe, want_feds, want_fed_laws, want_fed_law_state, want_law_db, want_law_propose, law_key, fed_id, want_fed_type, fed_type_key,
                want_probe_trade_deals, want_probe_windows, want_trade_deal, trade_give, trade_monthly_give, trade_take, trade_monthly_take, trade_years, trade_reserve, trade_sensor_charts, trade_sensor_link, trade_specimens, trade_fleets, want_capture, want_capture_clear, want_clone, want_clone_clear, want_cmd_capture, want_cmd_clear, want_try_peace, country_id, peace_status, peace_synth, peace_f40, peace_f4c, peace_force, action, federation_type, fed_probe_type, war_id, goal_key, dry_run, swap, probe_offset, probe_length, probe_follow, want_incoming, want_pending_votes, want_declare_war, want_war_goals, war_goals_all, want_respond, want_vote, action_id, respond_accept, vote_value, voter_id, want_community, community_ids, want_comm_install, want_comm_read, want_agr_install, want_agreements, agr_terms, agr_subsidies, want_agr_probe, agr_do_write, agr_do_delete, want_resolutions, want_res_types, res_types_all, res_target, want_propose_res, res_key, want_vote_res, want_end_session, res_id, res_vote, want_queued]() {
            if (want_vote_res) return ContactsManager::Get().VoteResolution(res_id, res_vote, dry_run);
            if (want_end_session) return ContactsManager::Get().EndSenateSession(res_id, dry_run);
            if (want_queued) return ContactsManager::Get().QueuedResolutionAction(res_id, action, dry_run);
            if (want_propose_res) return ContactsManager::Get().ProposeResolution(res_key, res_target, dry_run);
            if (want_res_types) return ContactsManager::Get().ProbeResolutionTypesJson(country_id, res_target, res_types_all);
            if (want_resolutions) return ContactsManager::Get().GetResolutionsJson();
            if (want_comm_install) return nlohmann::json{ {"success", CommandBuilder::Get().InstallCommunitySerializerCapture()} };
            if (want_comm_read) return ContactsManager::Get().ReadCommunityCaptureJson();
            if (want_agr_install) return nlohmann::json{ {"success", CommandBuilder::Get().InstallAgreementSerializerCapture()} };
            if (want_agreements) return ContactsManager::Get().GetAgreementsJson();
            if (want_community) return ContactsManager::Get().ProbeCommunityMembershipJson(community_ids);
            if (want_law_propose) return ContactsManager::Get().ProposeFederationLaw(law_key, dry_run, country_id);
            if (want_fed_type) return ContactsManager::Get().ProposeFederationType(fed_type_key, dry_run);
            if (want_probe_trade_deals) return ContactsManager::Get().ProbeTradeDealsJson();
            if (want_probe_windows) return ContactsManager::Get().ProbeWindowsJson();
            if (want_trade_deal) return ContactsManager::Get().ProposeTradeDeal(country_id, trade_give, trade_monthly_give,
                                                                               trade_take, trade_monthly_take, trade_years,
                                                                               trade_reserve, trade_sensor_charts, trade_sensor_link, trade_specimens, trade_fleets, dry_run);
            if (want_law_db) return ContactsManager::Get().ProbeLawDbJson();
            if (want_fed_laws) return ContactsManager::Get().ProbeFederationLawsJson(fed_id);
            if (want_fed_law_state) return ContactsManager::Get().GetFederationLawsJson(fed_id);
            if (want_feds) return ContactsManager::Get().GetFederationsJson();
            if (want_fed_probe) return ContactsManager::Get().ProbeFederationJson(fed_id);
            if (want_fed_type_db) return ContactsManager::Get().ProbeFederationTypeDbJson();
            if (want_fed_types) return ContactsManager::Get().ProbeFederationTypesJson(country_id, fed_probe_type);
            if (want_incoming) return ContactsManager::Get().GetIncomingActionsJson(country_id);
            if (want_pending_votes) return ContactsManager::Get().GetPendingVotesJson();
            if (want_declare_war) return ContactsManager::Get().DeclareWar(country_id, goal_key, dry_run);
            if (want_war_goals) return ContactsManager::Get().ProbeWarGoalsJson(country_id, war_goals_all);
            if (want_respond) return ContactsManager::Get().RespondDiploAction(action_id, respond_accept, dry_run);
            if (want_vote) return ContactsManager::Get().VoteDiploAction(action_id, vote_value, voter_id, dry_run);
            if (want_try_peace) return ContactsManager::Get().TryPeaceOffer(war_id, country_id, peace_status, dry_run, peace_synth, peace_f40, peace_f4c, peace_force);
            if (want_cmd_clear) { CommandBuilder::Get().ClearCmdCapture(); return nlohmann::json{ {"success", true}, {"cleared", true} }; }
            if (want_cmd_capture) return CommandBuilder::Get().CmdCaptureJson();
            if (want_clone_clear) { CommandBuilder::Get().ClearCloneCapture(); return nlohmann::json{ {"success", true}, {"cleared", true} }; }
            if (want_clone) return CommandBuilder::Get().CloneCaptureJson();
            if (want_capture_clear) { CommandBuilder::Get().ClearProposeCapture(); return nlohmann::json{ {"success", true}, {"cleared", true} }; }
            if (want_capture) return CommandBuilder::Get().ProposeCaptureJson();
            if (want_probe_war) return ContactsManager::Get().ProbeWarJson(war_id, probe_offset, probe_length, probe_follow);
            if (want_probe_country) return ContactsManager::Get().ProbeCountryJson(country_id, probe_offset, probe_length);
            if (want_probe_action) return ContactsManager::Get().ProbeDiplomaticAction(country_id, action);
            if (want_agr_probe) return ContactsManager::Get().ProbeAgreementAction(country_id, action, agr_terms, agr_subsidies, agr_do_write, agr_do_delete);
            return want_goal ? ContactsManager::Get().SetDefenderWarGoal(war_id, goal_key, dry_run, swap)
                             : ContactsManager::Get().ProposeDiplomaticAction(country_id, action, federation_type, agr_terms, agr_subsidies, dry_run);
        });
    } else if (method == "build_station") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        std::string station = params.value("station", std::string());
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, planet_id, station, queue]() {
            return GalaxyManager::Get().BuildStation(fleet_id, planet_id, station, queue);
        });
    } else if (method == "get_megastructure") {
        uint32_t id = params.value("megastructure_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([id]() { return GalaxyManager::Get().GetMegastructureJson(id); });
    } else if (method == "upgrade_megastructure") {
        uint32_t id = params.value("megastructure_id", 0xFFFFFFFFu);
        std::string type = params.value("type", std::string());
        fut = TaskQueue::Get().Enqueue([id, type]() { return GalaxyManager::Get().UpgradeMegastructure(id, type); });
    } else if (method == "get_buildable_megastructures") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id, system_id]() {
            return GalaxyManager::Get().GetBuildableMegastructures(fleet_id, system_id);
        });
    } else if (method == "build_megastructure") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        std::string type = params.value("type", std::string());
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        uint32_t toward = params.value("toward_system_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, type, planet_id, system_id, toward, queue]() {
            return GalaxyManager::Get().BuildMegastructure(fleet_id, type, planet_id, system_id, toward, queue);
        });
    } else if (method == "colonize") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, planet_id, queue]() {
            return GalaxyManager::Get().Colonize(fleet_id, planet_id, queue);
        });
    } else if (method == "cancel_fleet_orders") {
        std::vector<uint32_t> ids = params.value("fleet_ids", std::vector<uint32_t>{});
        fut = TaskQueue::Get().Enqueue([ids]() { return GalaxyManager::Get().CancelFleetOrders(ids); });
    } else if (method == "follow_fleet") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t target = params.value("target_fleet_id", 0xFFFFFFFFu);
        bool attack = params.value("attack", false);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, target, attack, queue]() {
            return GalaxyManager::Get().FollowFleet(fleet_id, target, attack, queue);
        });
    } else if (method == "set_fleet_stance" || method == "set_bombardment_stance" || method == "get_bombardment_stances") {
        // 三条并一个分支：这个 if/else 链贴着 MSVC 块嵌套上限（C1061）
        const bool is_bomb = method == "set_bombardment_stance";
        const bool list_bomb = method == "get_bombardment_stances";
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        std::string stance = params.value("stance", "");
        bool dry_run = params.value("dry_run", false);
        fut = TaskQueue::Get().Enqueue([=]() {
            if (list_bomb) return OutlinerManager::Get().GetBombardmentStancesJson();
            if (is_bomb) return OutlinerManager::Get().SetBombardmentStanceJson(fleet_id, stance, !dry_run);
            return GalaxyManager::Get().SetFleetStance(fleet_id, stance);
        });
    } else if (method == "set_fleet_home_base") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t starbase_id = params.value("starbase_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id, starbase_id]() {
            return GalaxyManager::Get().SetFleetHomeBase(fleet_id, starbase_id);
        });
    } else if (method == "probe_fleet_ships") {
        uint32_t fid = params.value("fleet_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fid]() { return GalaxyManager::Get().ProbeFleetShips(fid); });
    } else if (method == "set_fleet_template_design") {
        uint32_t fid = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t from = params.value("from_design_id", 0xFFFFFFFFu);
        uint32_t to = params.value("to_design_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fid, from, to]() {
            return GalaxyManager::Get().SetFleetTemplateDesign(fid, from, to);
        });
    } else if (method == "set_ship_upgrade_design") {
        uint32_t fid = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t to = params.value("to_design_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fid, to]() {
            return GalaxyManager::Get().SetShipUpgradeDesign(fid, to);
        });
    } else if (method == "scan_ship_for_values") {
        uint32_t sid = params.value("ship_id", 0xFFFFFFFFu);
        std::vector<uint32_t> vals;
        for (const auto& v : params.value("values", nlohmann::json::array())) {
            if (v.is_number()) vals.push_back(v.get<uint32_t>());
        }
        fut = TaskQueue::Get().Enqueue([sid, vals]() {
            return GalaxyManager::Get().ScanShipForValues(sid, vals);
        });
    } else if (method == "scan_console_for_text") {
        std::string needle = params.value("needle", std::string());
        fut = TaskQueue::Get().Enqueue([needle]() {
            return GalaxyManager::Get().ScanConsoleForText(needle);
        });
    } else if (method == "probe_console_slot") {
        uint32_t slot = params.value("slot_offset", 0x40u);
        uint32_t len = params.value("length", 0x200u);
        fut = TaskQueue::Get().Enqueue([slot, len]() {
            return GalaxyManager::Get().ProbeConsoleSlot(slot, len);
        });
    } else if (method == "scan_memory_for_text") {
        std::string needle = params.value("needle", std::string());
        bool include_code = params.value("include_code", false);
        int secs = params.value("max_seconds", 1);
        uint64_t start_addr = params.value("start_addr", (uint64_t)0);
        uint64_t max_bytes = params.value("max_bytes", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([needle, include_code, secs, start_addr, max_bytes]() {
            return GalaxyManager::Get().ScanMemoryForText(needle, include_code, secs, start_addr, max_bytes);
        });
    } else if (method == "probe_colony_carriers") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().ProbeColonyCarriersJson(); });
    } else if (method == "probe_tech_status") {
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x400u);
        bool dump_hex = params.value("dump", false);
        fut = TaskQueue::Get().Enqueue([offset, length, dump_hex]() {
            return GalaxyManager::Get().ProbeTechStatusJson(offset, length, dump_hex);
        });
    } else if (method == "scan_edict_arrays") {
        int max_seconds = params.value("max_seconds", 8);
        fut = TaskQueue::Get().Enqueue([max_seconds]() {
            return GalaxyManager::Get().ScanEdictArraysJson(max_seconds);
        });
    } else if (method == "read_memory") {
        uint64_t address = params.value("address", (uint64_t)0);
        uint32_t length = params.value("length", 0x200u);
        fut = TaskQueue::Get().Enqueue([address, length]() {
            return GalaxyManager::Get().ReadMemoryAt(address, length);
        });
    } else if (method == "get_planet_job_capacity") {
        uint32_t planet_id = params.value("planet_id", 0u);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetPlanetJobCapacityJson(planet_id);
        });
    } else if (method == "probe_colony_jobs") {
        uint32_t planet_id = params.value("planet_id", 0u);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().ProbeColonyJobsRaw(planet_id);
        });
    } else if (method == "scan_colony_for_int") {
        uint32_t planet_id = params.value("planet_id", 0u);
        int64_t value = params.value("value", (int64_t)0);
        uint32_t radius = params.value("radius", 0x400u);
        fut = TaskQueue::Get().Enqueue([planet_id, value, radius]() {
            return OutlinerManager::Get().ScanColonyForInt(planet_id, value, radius);
        });
    } else if (method == "dump_colony_map") {
        uint32_t planet_id = params.value("planet_id", 0u);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().DumpColonyMap(planet_id);
        });
    } else if (method == "scan_colony_for_int_deep") {
        uint32_t planet_id = params.value("planet_id", 0u);
        int64_t value = params.value("value", (int64_t)0);
        fut = TaskQueue::Get().Enqueue([planet_id, value]() {
            return OutlinerManager::Get().ScanColonyForIntDeep(planet_id, value);
        });
    } else if (method == "hook_job_update") {
        bool enable = params.value("enable", true);
        fut = TaskQueue::Get().Enqueue([enable]() {
            return GalaxyManager::Get().HookJobUpdate(enable);
        });
    } else if (method == "read_job_hook") {
        fut = TaskQueue::Get().Enqueue([]() {
            return GalaxyManager::Get().ReadJobHook();
        });
    } else if (method == "hook_colony_update") {
        bool enable = params.value("enable", true);
        fut = TaskQueue::Get().Enqueue([enable]() {
            return GalaxyManager::Get().HookColonyUpdate(enable);
        });
    } else if (method == "read_colony_hook") {
        fut = TaskQueue::Get().Enqueue([]() {
            return GalaxyManager::Get().ReadColonyHook();
        });
    } else if (method == "find_popgroup_slots") {
        uint32_t planet_id = params.value("planet_id", 0u);
        std::vector<int64_t> sizes;
        if (params.contains("sizes") && params["sizes"].is_array()) {
            for (const auto& v : params["sizes"]) sizes.push_back(v.get<int64_t>());
        }
        uint32_t radius = params.value("radius", 0x400u);
        fut = TaskQueue::Get().Enqueue([planet_id, sizes, radius]() {
            return OutlinerManager::Get().FindPopGroupSlots(planet_id, sizes, radius);
        });
    } else if (method == "dump_popgroup") {
        uint32_t group_id = params.value("group_id", 0u);
        fut = TaskQueue::Get().Enqueue([group_id]() {
            return OutlinerManager::Get().DumpPopGroup(group_id);
        });
    } else if (method == "hook_popgroup_size") {
        bool enable = params.value("enable", true);
        fut = TaskQueue::Get().Enqueue([enable]() {
            return GalaxyManager::Get().HookPopGroupSize(enable);
        });
     } else if (method == "scan_memory_for_u64_range") {
        uint64_t lo = params.value("lo", (uint64_t)0);
        uint64_t hi = params.value("hi", (uint64_t)0);
        int secs = params.value("max_seconds", 2);
        uint64_t start_addr = params.value("start_addr", (uint64_t)0);
        uint64_t max_bytes = params.value("max_bytes", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([lo, hi, secs, start_addr, max_bytes]() {
            return GalaxyManager::Get().ScanMemoryForU64Range(lo, hi, secs, start_addr, max_bytes);
        });
    } else if (method == "localize") {
        std::vector<std::string> keys;
        if (params.contains("keys") && params["keys"].is_array()) {
            for (const auto& k : params["keys"]) {
                if (k.is_string()) keys.push_back(k.get<std::string>());
            }
        }
        if (params.contains("key") && params["key"].is_string()) {
            keys.push_back(params["key"].get<std::string>());
        }
        fut = TaskQueue::Get().Enqueue([keys]() {
            nlohmann::json names = nlohmann::json::object();
            for (const auto& k : keys) {
                names[k] = OutlinerManager::Get().Localize(k);
            }
            return nlohmann::json{ {"success", true}, {"names", names} };
        });
    } else if (method == "resolve_popjob") {
        uint32_t handle = params.value("handle", 0u);
        fut = TaskQueue::Get().Enqueue([handle]() {
            return OutlinerManager::Get().ResolvePopJob(handle);
        });
    } else if (method == "disband_fleets") {
        std::vector<uint32_t> ids;
        if (params.contains("fleet_ids") && params["fleet_ids"].is_array()) {
            for (const auto& v : params["fleet_ids"]) {
                if (v.is_number()) ids.push_back(v.get<uint32_t>());
            }
        }
        fut = TaskQueue::Get().Enqueue([ids]() {
            return GalaxyManager::Get().DisbandFleetsJson(ids);
        });
    } else if (method == "downgrade_starbase") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        fut = TaskQueue::Get().Enqueue([starbase_id]() {
            return GalaxyManager::Get().DowngradeStarbaseJson(starbase_id);
        });
    } else if (method == "find_ships") {
        int design_id = params.value("design_id", -1);
        uint32_t limit = params.value("limit", 200u);
        int system_id = params.value("system_id", -1);
        fut = TaskQueue::Get().Enqueue([design_id, limit, system_id]() {
            return OutlinerManager::Get().FindShipsJson(design_id, limit, system_id);
        });
    } else if (method == "get_starbase_queue" || method == "get_queue") {
        // 两条合并：这个 if/else 链已贴着 MSVC 的块嵌套上限（C1061）
        const bool raw_queue = method == "get_queue";
        uint32_t starbase_id = params.value("starbase_id", 0u);
        uint32_t queue_id = params.value("queue_id", 0u);
        fut = TaskQueue::Get().Enqueue([raw_queue, starbase_id, queue_id]() {
            return raw_queue ? OutlinerManager::Get().GetQueueJson(queue_id)
                             : OutlinerManager::Get().GetStarbaseQueueJson(starbase_id);
        });
    } else if (method == "dismantle_starbase") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        fut = TaskQueue::Get().Enqueue([starbase_id]() {
            return GalaxyManager::Get().DismantleStarbaseJson(starbase_id);
        });
    } else if (method == "merge_fleets") {
        std::vector<uint32_t> ids;
        if (params.contains("fleet_ids") && params["fleet_ids"].is_array()) {
            for (const auto& v : params["fleet_ids"]) {
                if (v.is_number()) ids.push_back(v.get<uint32_t>());
            }
        }
        fut = TaskQueue::Get().Enqueue([ids]() {
            return GalaxyManager::Get().MergeFleetsJson(ids);
        });
    } else if (method == "get_starbase_pointers") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetStarbasePointers(); });
    } else if (method == "get_starbase_overview" || method == "get_our_starbases") {
        // 两条合并成一个分支：这个 if/else 链已经贴着 MSVC 的块嵌套上限（C1061），
        // 再单独加一条就会编译失败。
        const bool ours = method == "get_our_starbases";
        fut = TaskQueue::Get().Enqueue([ours]() {
            return ours ? GalaxyManager::Get().GetOurStarbasesJson() : GalaxyManager::Get().GetStarbaseOverviewJson();
        });
    } else if (method == "get_starbase_details") {
        uint32_t starbase_id = params.value("starbase_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([starbase_id]() { return GalaxyManager::Get().GetStarbaseDetails(starbase_id); });
    } else if (method == "get_fleet_home_base") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id]() { return GalaxyManager::Get().GetFleetHomeBase(fleet_id); });
    } else if (method == "fleet_mia") {
        std::vector<uint32_t> ids = params.value("fleet_ids", std::vector<uint32_t>{});
        std::string type = params.value("type", "return_home");
        fut = TaskQueue::Get().Enqueue([ids, type]() { return GalaxyManager::Get().FleetMia(ids, type); });
    } else if (method == "claim_system") {
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        bool remove = params.value("remove", false);
        int count = params.value("count", 0);
        fut = TaskQueue::Get().Enqueue([system_id, remove, count]() {
            return GalaxyManager::Get().ClaimSystem(system_id, remove, count);
        });
    } else if (method == "orbit_planet" || method == "research_anomalies" || method == "excavate_site" ||
               method == "use_bypass" || method == "explore_bypass") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t target = params.value(method == "orbit_planet" ? "planet_id" : method == "research_anomalies" ? "system_id"
                                       : method == "excavate_site" ? "site_id" : "bypass_id", 0xFFFFFFFFu);
        uint32_t to_system = params.value("to_system_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        bool dry_run = params.value("dry_run", false);
        std::string m = method;
        fut = TaskQueue::Get().Enqueue([m, fleet_id, target, to_system, queue, dry_run]() {
            auto& g = GalaxyManager::Get();
            if (m == "orbit_planet") return g.OrbitPlanet(fleet_id, target, queue);
            if (m == "research_anomalies") return g.ResearchAnomalies(fleet_id, target, queue);
            if (m == "excavate_site") return g.ExcavateSite(fleet_id, target, queue, dry_run);
            if (m == "use_bypass") return g.UseBypass(fleet_id, target, to_system, queue);
            return g.ExploreBypass(fleet_id, target, queue);
        });
    } else if (method == "land_armies") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, planet_id, queue]() { return GalaxyManager::Get().LandArmies(fleet_id, planet_id, queue); });
    } else if (method == "collect_data") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t project_id = params.value("project_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, project_id, system_id, queue]() {
            return GalaxyManager::Get().CollectData(fleet_id, project_id, system_id, queue);
        });
    } else if (method == "find_path") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t to = params.value("to_system_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([fleet_id, to]() { return GalaxyManager::Get().FindPath(fleet_id, to); });
    } else if (method == "find_systems") {
        std::string purpose = params.value("purpose", "");
        uint32_t from = params.value("from_system_id", 0xFFFFFFFFu);
        int limit = params.value("limit", 10);
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t species_id = params.value("species_id", 0xFFFFFFFFu);
        std::string resource = params.value("resource", "");
        std::string name = params.value("name", "");  // 非空 = 按名字找（全银河子串）
        fut = TaskQueue::Get().Enqueue([purpose, from, limit, fleet_id, resource, species_id, name]() {
            return GalaxyManager::Get().FindSystems(purpose, from, limit, fleet_id, resource, species_id, name);
        });
    } else if (method == "survey") {
        uint32_t fleet_id = params.value("fleet_id", 0xFFFFFFFFu);
        uint32_t system_id = params.value("system_id", 0xFFFFFFFFu);
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        bool queue = params.value("queue", false);
        fut = TaskQueue::Get().Enqueue([fleet_id, system_id, planet_id, queue]() {
            return GalaxyManager::Get().Survey(fleet_id, system_id, planet_id, queue);
        });
    } else if (method == "set_situation_approach") {
        uint32_t situation_id = params.value("situation_id", 0);
        std::string approach_key = params.value("approach_key", "");
        fut = TaskQueue::Get().Enqueue([situation_id, approach_key]() {
            return SituationLogManager::Get().SetSituationApproach(situation_id, approach_key);
        });
    } else if (method == "validate_situation_approach") {
        uint32_t situation_id = params.value("situation_id", 0);
        std::string approach_key = params.value("approach_key", "");
        fut = TaskQueue::Get().Enqueue([situation_id, approach_key]() {
            return SituationLogManager::Get().ValidateSituationApproachJson(situation_id, approach_key);
        });
    } else if (method == "get_government") {
        fut = TaskQueue::Get().Enqueue([]() {
            return GovernmentManager::Get().GetGovernmentJson();
        });
    } else if (method == "set_council_agenda") {
        std::string agenda_key = params.value("agenda_key", "");
        fut = TaskQueue::Get().Enqueue([agenda_key]() {
            return GovernmentManager::Get().SetCouncilAgenda(agenda_key);
        });
    } else if (method == "get_civics") {
        std::string civic_key = params.value("civic_key", "");
        fut = TaskQueue::Get().Enqueue([civic_key]() {
            return GovernmentManager::Get().GetCivicsJson(civic_key);
        });
    } else if (method == "change_civics") {
        std::vector<std::string> add = params.value("add", std::vector<std::string>{});
        std::vector<std::string> remove = params.value("remove", std::vector<std::string>{});
        fut = TaskQueue::Get().Enqueue([add, remove]() {
            return GovernmentManager::Get().ChangeCivics(add, remove);
        });
    } else if (method == "launch_council_agenda") {
        bool early = params.value("early", false);
        fut = TaskQueue::Get().Enqueue([early]() {
            return GovernmentManager::Get().LaunchCouncilAgenda(early);
        });
    } else if (method == "get_traditions") {
        fut = TaskQueue::Get().Enqueue([]() {
            return SocietyManager::Get().GetTraditionsJson();
        });
    } else if (method == "adopt_tradition") {
        std::string tradition_key = params.value("tradition_key", "");
        fut = TaskQueue::Get().Enqueue([tradition_key]() {
            return SocietyManager::Get().AdoptTradition(tradition_key);
        });
    } else if (method == "activate_ascension_perk") {
        std::string perk_key = params.contains("perk") ? params.value("perk", std::string()) : params.value("perk_key", std::string());
        fut = TaskQueue::Get().Enqueue([perk_key]() {
            return SocietyManager::Get().ActivateAscensionPerk(perk_key);
        });
    } else if (method == "get_edicts") {
        fut = TaskQueue::Get().Enqueue([]() {
            return SocietyManager::Get().GetEdictsJson();
        });
    } else if (method == "toggle_edict") {
        std::string edict_key = params.value("edict_key", "");
        bool enabled = params.value("enabled", true);
        fut = TaskQueue::Get().Enqueue([edict_key, enabled]() {
            return SocietyManager::Get().ToggleEdict(edict_key, enabled);
        });
    } else if (method == "get_leaders") {
        fut = TaskQueue::Get().Enqueue([]() {
            return LeaderManager::Get().GetLeadersJson();
        });
    } else if (method == "probe_leader_manager") {
        uint32_t offset = params.value("offset", 0u);
        uint32_t length = params.value("length", 0x100u);
        fut = TaskQueue::Get().Enqueue([offset, length]() {
            return LeaderManager::Get().ProbeLeaderManagerJson(offset, length);
        });
    }
    // the dispatch goes on in a second chain: MSVC limits how deep one if / else-if chain may nest
    // (C1061). A method matched above has its task already
    if (fut.valid()) {
    } else if (method == "hire_leader") {
        uint32_t candidate_id = params.value("candidate_id", 0);
        fut = TaskQueue::Get().Enqueue([candidate_id]() {
            return LeaderManager::Get().HireLeader(candidate_id);
        });
    } else if (method == "dismiss_leader") {
        uint32_t leader_id = params.value("leader_id", 0);
        fut = TaskQueue::Get().Enqueue([leader_id]() {
            return LeaderManager::Get().DismissLeader(leader_id);
        });
    } else if (method == "select_leader_trait") {
        uint32_t leader_id = params.value("leader_id", 0u);
        std::string trait_key = params.value("trait_key", "");
        fut = TaskQueue::Get().Enqueue([leader_id, trait_key]() {
            return LeaderManager::Get().SelectTrait(leader_id, trait_key);
        });
    } else if (method == "assign_leader") {
        uint32_t leader_id = params.value("leader_id", 0xFFFFFFFFu);
        std::string slot = params.value("slot", std::string());
        uint32_t target_id = params.value("target_id", 0xFFFFFFFFu);
        std::string task = params.value("task", std::string());
        const int task_index = params.value("task_index", -1);
        fut = TaskQueue::Get().Enqueue([leader_id, slot, target_id, task, task_index]() {
            return LeaderManager::Get().AssignLeader(leader_id, slot, target_id, task, task_index);
        });
    } else if (method == "get_leader_assignments") {
        uint32_t leader_id = params.value("leader_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([leader_id]() { return LeaderManager::Get().GetAssignmentsJson(leader_id); });
    } else if (method == "get_species") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return SpeciesManager::Get().GetSpeciesJson(params);
        });
    } else if (method == "set_species_rights") {
        uint32_t species_id = params.value("species_id", 0);
        std::string category = params.value("category", "");
        if (category.empty() && params.contains("right_category")) {
            category = params.value("right_category", "");
        }
        std::string right_value = params.value("right_value", "");
        fut = TaskQueue::Get().Enqueue([species_id, category, right_value]() {
            return SpeciesManager::Get().SetSpeciesRight(species_id, category, right_value);
        });
    } else if (method == "get_species_modification_info") {
        uint32_t species_id = params.value("species_id", 0);
        fut = TaskQueue::Get().Enqueue([species_id]() {
            return SpeciesManager::Get().GetSpeciesModificationInfoJson(species_id);
        });
    } else if (method == "create_species_template") {
        uint32_t base_species_id = params.value("base_species_id", (uint32_t)params.value("species_id", 0));
        std::string name = params.value("name", "");
        std::vector<std::string> traits = params.value("traits", std::vector<std::string>{});
        // no_base: seed the new species from base_species_id but leave its base_ref at 0xFFFFFFFF
        // (a standalone species, the way 82/290 look). Experiment switch, default off.
        bool no_base = params.value("no_base", false);
        // root_base_id: the base recorded on the new species when it differs from the seed. The game
        // normalises a template of the founder onto the founder's own base (115 → 3791650817), so the
        // caller may pass the root explicitly. 0xFFFFFFFF = use base_species_id.
        uint32_t root_base_id = params.value("root_base_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([base_species_id, name, traits, no_base, root_base_id]() {
            return SpeciesManager::Get().CreateSpeciesTemplateJson(base_species_id, name, traits, no_base, root_base_id);
        });
    } else if (method == "delete_species_template") {
        uint32_t species_id = params.value("species_id", 0);
        fut = TaskQueue::Get().Enqueue([species_id]() {
            return SpeciesManager::Get().DeleteSpeciesTemplateJson(species_id);
        });
    } else if (method == "modify_species_template") {
        uint32_t template_species_id = params.value("template_species_id", (uint32_t)params.value("species_id", 0));
        std::string name = params.value("name", "");
        std::vector<std::string> traits = params.value("traits", std::vector<std::string>{});
        fut = TaskQueue::Get().Enqueue([template_species_id, name, traits]() {
            return SpeciesManager::Get().ModifySpeciesTemplateJson(template_species_id, name, traits);
        });
    } else if (method == "begin_uplifting") {
        // CBeginUpliftingCommand (token 0x2F78): country +0x20 / species +0x24. Starts the uplift
        // special project; dry_run only runs IsValid.
        uint32_t species_id = params.value("species_id", 0u);
        bool dry_run = params.value("dry_run", false);
        fut = TaskQueue::Get().Enqueue([species_id, dry_run]() {
            return SpeciesManager::Get().BeginUpliftingJson(species_id, dry_run);
        });
    } else if (method == "apply_species_template") {
        uint32_t template_species_id = params.value("template_species_id", (uint32_t)params.value("species_id", 0));
        std::vector<uint32_t> colony_ids = params.value("colony_ids", std::vector<uint32_t>{});
        // 0x01000000-tagged refs. The pair is {species, colony}; which encoding the engine wants is
        // what the dry experiments decide, so it is a parameter for now.
        bool tag_refs = params.value("tag_refs", false);
        fut = TaskQueue::Get().Enqueue([template_species_id, colony_ids, tag_refs]() {
            return SpeciesManager::Get().ApplySpeciesTemplateJson(template_species_id, colony_ids, tag_refs);
        });
    } else if (method == "get_fleets") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return FleetManager::Get().GetFleetsJson(params);
        });
    } else if (method == "reinforce_fleet") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return FleetManager::Get().ReinforceFleetJson(params);
        });
    } else if (method == "set_fleet_template_quota") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return FleetManager::Get().SetFleetTemplateQuotaJson(params);
        });
    } else if (method == "get_ship_designs") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().GetShipDesignsJson(params);
        });
    } else if (method == "get_ship_design_catalog") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().GetShipDesignCatalogJson(params);
        });
    } else if (method == "get_component_details") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().GetComponentDetailsJson(params);
        });
    } else if (method == "create_ship_design") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().CreateShipDesignJson(params);
        });
    } else if (method == "update_ship_design") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().UpdateShipDesignJson(params);
        });
    } else if (method == "probe_design_sections") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().ProbeDesignSectionsJson(params);
        });
    } else if (method == "upgrade_fleet") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().UpgradeFleetJson(params);
        });
    } else if (method == "delete_ship_design") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return ShipDesigner::Get().DeleteShipDesignJson(params);
        });
    } else if (method == "get_market") {
        fut = TaskQueue::Get().Enqueue([]() {
            return MarketManager::Get().GetMarketInfo();
        });
    } else if (method == "probe_market") {
        fut = TaskQueue::Get().Enqueue([]() {
            return MarketManager::Get().ProbeMarketJson();
        });
    } else if (method == "market_trade") {
        std::string resource = params.value("resource", "");
        std::string action = params.value("action", "");
        if (action.empty()) {
            if (params.contains("is_buy")) {
                action = params["is_buy"].get<bool>() ? "buy" : "sell";
            } else {
                action = "buy";
            }
        }
        uint32_t units = params.value("units", (uint32_t)params.value("amount", 0));
        fut = TaskQueue::Get().Enqueue([resource, action, units]() {
            std::string msg;
            bool ok = MarketManager::Get().ExecuteInstantTrade(resource, action, units, msg);
            return nlohmann::json{
                {"success", ok},
                {"message", msg}
            };
        });
    } else if (method == "set_monthly_trade") {
        std::string resource = params.value("resource", "");
        std::string action = params.value("action", "buy");
        double amount = params.value("amount", 0.0);
        double price_limit = params.value("price_limit", 0.0);
        bool cancel = params.value("cancel", false);
        int32_t order_id = params.value("order_id", -1);
        fut = TaskQueue::Get().Enqueue([resource, action, amount, price_limit, cancel, order_id]() {
            std::string msg;
            bool ok = MarketManager::Get().SetMonthlyTrade(resource, action, amount, price_limit, cancel, order_id, msg);
            return nlohmann::json{
                {"success", ok},
                {"message", msg}
            };
        });
    } else if (method == "get_discoveries") {
        std::string tab = params.value("tab", "all");
        bool probe = params.value("probe", false);
        fut = TaskQueue::Get().Enqueue([tab, probe]() {
            return DiscoveriesManager::Get().GetDiscoveriesInfo(tab, probe);
        });
    } else if (method == "probe_grand_archive") {
        uint32_t cid = params.value("country_id", 0xFFFFFFFFu); bool dec = params.value("decode", false);
        fut = TaskQueue::Get().Enqueue([cid, dec]() {
            const uint32_t id = cid == 0xFFFFFFFFu ? GameState::Get().GetPlayerCountryId() : cid;
            return DiscoveriesManager::Get().ProbeGrandArchive(id, dec);
        });
    } else if (method == "toggle_exhibit") {
        uint32_t eid = params.value("exhibit_id", 0xFFFFFFFFu);
        bool force = params.value("force", false);
        fut = TaskQueue::Get().Enqueue([eid, force]() {
            std::string msg;
            bool ok = DiscoveriesManager::Get().ToggleExhibit(eid, msg, force);
            return nlohmann::json{{"success", ok}, {"message", msg}};
        });
    } else if (method == "move_specimen") {
        uint32_t eid = params.value("exhibit_id", 0xFFFFFFFFu);
        bool to_storage = params.value("to_storage", false);
        bool force = params.value("force", false);
        fut = TaskQueue::Get().Enqueue([eid, to_storage, force]() {
            std::string msg;
            bool ok = DiscoveriesManager::Get().MoveSpecimen(eid, to_storage, msg, force);
            return nlohmann::json{{"success", ok}, {"message", msg}};
        });
    } else if (method == "get_artifact_actions") {
        bool probe = params.value("probe", false);
        fut = TaskQueue::Get().Enqueue([probe]() {
            return DiscoveriesManager::Get().GetArtifactActions(probe);
        });
    } else if (method == "probe_country_flags") {
        fut = TaskQueue::Get().Enqueue([]() {
            return DiscoveriesManager::Get().ProbeCountryFlags();
        });
    } else if (method == "activate_artifact_action") {
        std::string key = params.value("key", std::string());
        fut = TaskQueue::Get().Enqueue([key]() {
            std::string msg;
            bool ok = DiscoveriesManager::Get().ActivateArtifactAction(key, msg);
            return nlohmann::json{{"success", ok}, {"message", msg}};
        });
    } else if (method == "activate_relic") {
        std::string relic_key = params.value("relic_key", params.value("relic", ""));
        fut = TaskQueue::Get().Enqueue([relic_key]() {
            std::string msg;
            bool ok = DiscoveriesManager::Get().ActivateRelic(relic_key, msg);
            return nlohmann::json{
                {"success", ok},
                {"message", msg}
            };
        });
    } else if (method == "activate_astral_action") {
        std::string action_key = params.value("action_key", params.value("key", ""));
        fut = TaskQueue::Get().Enqueue([action_key]() {
            std::string msg;
            bool ok = DiscoveriesManager::Get().ActivateAstralAction(action_key, msg);
            return nlohmann::json{
                {"success", ok},
                {"message", msg}
            };
        });
    } else if (method == "get_contacts") {
        std::string mode = params.value("mode", "all");
        fut = TaskQueue::Get().Enqueue([mode]() {
            return ContactsManager::Get().GetContactsInfo(mode);
        });
    } else if (method == "probe_first_contacts") {
        fut = TaskQueue::Get().Enqueue([]() {
            return ContactsManager::Get().ProbeFirstContactsJson();
        });
    } else if (method == "probe_first_contact") {
        uint32_t id = params.value("id", 0xFFFFFFFFu);
        int32_t offset = params.value("offset", 0xB0);
        int32_t length = params.value("length", 0x80);
        fut = TaskQueue::Get().Enqueue([id, offset, length]() {
            return ContactsManager::Get().ProbeFirstContactJson(id, offset, length);
        });
    } else if (method == "probe_first_contact_view") {
        int depth = params.value("depth", 3);
        std::string find = params.value("find", std::string());
        fut = TaskQueue::Get().Enqueue([depth, find]() {
            return EventManager::Get().ProbeFirstContactViewJson(depth, find);
        });
    } else if (method == "list_open_events") {
        fut = TaskQueue::Get().Enqueue([]() { return EventManager::Get().ListOpenEventsJson(); });
    } else if (method == "get_event_options") {
        uint64_t ope = params.value("ope", (uint64_t)0);
        uint32_t id = params.value("id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([ope, id]() {
            return EventManager::Get().GetEventOptionsJson(ope, id);
        });
    } else if (method == "select_open_event_option") {
        uint64_t ope = params.value("ope", (uint64_t)0);
        int option = params.value("option", 0);
        bool dry_run = params.value("dry_run", false);
        fut = TaskQueue::Get().Enqueue([ope, option, dry_run]() {
            return EventManager::Get().SelectOpenEventOptionJson(ope, option, dry_run);
        });
    } else if (method == "scan_view_windows") {
        uint64_t addr = params.value("view", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([addr]() {
            return EventManager::Get().ScanViewWindowsJson(addr);
        });
    } else if (method == "call_view_slot") {
        uint64_t view = params.value("view", (uint64_t)0);
        int slot = params.value("slot", 0);
        fut = TaskQueue::Get().Enqueue([view, slot]() {
            return EventManager::Get().CallViewSlotJson(view, slot);
        });
    } else if (method == "scan_memory_for_qword") {
        uint64_t value = params.value("value", (uint64_t)0);
        int max_seconds = params.value("max_seconds", 6);
        uint64_t start_addr = params.value("start_addr", (uint64_t)0);
        uint64_t max_bytes = params.value("max_bytes", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([=]() {
            return GalaxyManager::Get().ScanMemoryForQword(value, max_seconds, start_addr, max_bytes);
        });
    } else if (method == "list_views") {
        fut = TaskQueue::Get().Enqueue([]() {
            return EventManager::Get().ListViewsJson();
        });
    } else if (method == "find_event_windows") {
        int depth = params.value("depth", 2);
        fut = TaskQueue::Get().Enqueue([depth]() {
            return EventManager::Get().FindEventWindowsJson(depth);
        });
    } else if (method == "walk_gui") {
        uint64_t addr = params.value("address", (uint64_t)0);
        int depth = params.value("depth", 2);
        std::string find = params.value("find", std::string());
        fut = TaskQueue::Get().Enqueue([addr, depth, find]() {
            return EventManager::Get().WalkGuiJson(addr, depth, find);
        });
    } else if (method == "probe_site_events") {
        uint32_t site_id = params.value("site_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([site_id]() {
            return GalaxyManager::Get().ProbeSiteEventsJson(site_id);
        });
    } else if (method == "find_gui_by_text") {
        std::string needle = params.value("needle", std::string());
        int depth = params.value("depth", 2);
        fut = TaskQueue::Get().Enqueue([needle, depth]() {
            return EventManager::Get().FindGuiByTextJson(needle, depth);
        });
    } else if (method == "probe_leader") {
        uint32_t leader_id = params.value("leader_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([leader_id]() {
            return ContactsManager::Get().ProbeLeaderJson(leader_id);
        });
    } else if (method == "assign_envoy") {
        uint32_t leader_id = params.value("leader_id", 0xFFFFFFFFu);
        uint32_t contact_id = params.value("contact_id", 0xFFFFFFFFu);
        bool dry_run = params.value("dry_run", false);
        bool reset_cooldown = params.value("reset_cooldown", false);
        fut = TaskQueue::Get().Enqueue([leader_id, contact_id, dry_run, reset_cooldown]() {
            return ContactsManager::Get().AssignEnvoyJson(leader_id, contact_id, dry_run, reset_cooldown);
        });
    } else if (method == "advance_first_contact") {
        uint32_t country_id = params.value("country_id", 0xFFFFFFFFu);
        uint32_t value = params.value("value", 1u);
        fut = TaskQueue::Get().Enqueue([country_id, value]() {
            return ContactsManager::Get().AdvanceFirstContactJson(country_id, value);
        });
    } else if (method == "poke_first_contact") {
        uint32_t country_id = params.value("country_id", 0xFFFFFFFFu);
        bool hold = params.value("hold", false);
        fut = TaskQueue::Get().Enqueue([country_id, hold]() {
            return ContactsManager::Get().PokeFirstContactJson(country_id, hold);
        });
    } else if (method == "advance_first_contact_stage") {
        uint32_t country_id = params.value("country_id", 0xFFFFFFFFu);
        uint64_t ope = params.value("ope", (uint64_t)0);
        int option = params.value("option", 0);
        bool settle = params.value("settle", false);
        fut = TaskQueue::Get().Enqueue([country_id, ope, option, settle]() {
            return ContactsManager::Get().AdvanceFirstContactStageJson(country_id, ope, option, settle);
        });
    } else if (method == "get_first_contacts") {
        bool all = params.value("all", false);
        fut = TaskQueue::Get().Enqueue([all]() {
            return ContactsManager::Get().GetFirstContactsJson(all);
        });
    } else if (method == "get_outliner") {
        fut = TaskQueue::Get().Enqueue([]() {
            return OutlinerManager::Get().GetOutlinerSummaryJson();
        });
    } else if (method == "get_sectors") {
        int32_t sector_id = params.value("sector_id", -1);
        fut = TaskQueue::Get().Enqueue([sector_id]() {
            return OutlinerManager::Get().GetSectorsJson(sector_id);
        });
    } else if (method == "get_military_fleets") {
        fut = TaskQueue::Get().Enqueue([]() {
            return OutlinerManager::Get().GetMilitaryFleetsJson();
        });
    } else if (method == "get_civilian_fleets") {
        fut = TaskQueue::Get().Enqueue([]() {
            return OutlinerManager::Get().GetCivilianFleetsJson();
        });
    } else if (method == "get_armies") {
        fut = TaskQueue::Get().Enqueue([]() {
            return OutlinerManager::Get().GetArmiesJson();
        });
    } else if (method == "get_planet_details") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetPlanetDetailsJson(planet_id);
        });
    } else if (method == "get_available_district_zones") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string district_type = params.value("district_type", "");
        bool include_blocked = params.value("include_blocked", false);
        fut = TaskQueue::Get().Enqueue([planet_id, district_type, include_blocked]() {
            return OutlinerManager::Get().GetAvailableDistrictZonesJson(planet_id, district_type, include_blocked);
        });
    } else if (method == "list_colony_designations") {
        uint32_t planet_id = params.value("planet_id", 0u);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().ListColonyDesignationsJson(planet_id);
        });
    } else if (method == "set_colony_designation") {
        uint32_t planet_id = params.value("planet_id", 0u);
        std::string key = params.value("key", std::string());
        uint64_t designation_ptr = params.value("designation_ptr", (uint64_t)0);
        fut = TaskQueue::Get().Enqueue([planet_id, key, designation_ptr]() {
            return OutlinerManager::Get().SetColonyDesignationJson(planet_id, key, designation_ptr);
        });
    } else if (method == "set_district_zone") {
        uint32_t planet_id = params.value("planet_id", 0xFFFFFFFFu);
        uint32_t district_id = params.value("district_id", 0xFFFFFFFFu);
        int32_t slot = params.value("slot", -1);
        std::string zone_key = params.value("zone_key", "");
        fut = TaskQueue::Get().Enqueue([planet_id, district_id, slot, zone_key]() {
            return OutlinerManager::Get().SetDistrictZoneJson(planet_id, district_id, slot, zone_key);
        });
    } else if (method == "get_buildable_buildings") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string building_key = params.value("building_key", "");
        int32_t zone_id = params.value("zone_id", params.value("slot_index", -1));
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, building_key, zone_id, colony_id]() {
            return OutlinerManager::Get().GetBuildableBuildingsJson(planet_id, building_key, zone_id, colony_id);
        });
    } else if (method == "get_buildable_districts") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string district_key = params.value("district_key", "");
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, district_key, colony_id]() {
            return OutlinerManager::Get().GetBuildableDistrictsJson(planet_id, district_key, colony_id);
        });
    } else if (method == "build_district") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string district_key = params.value("district_key", "");
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, district_key, colony_id]() {
            return OutlinerManager::Get().BuildDistrictJson(planet_id, district_key, colony_id);
        });
    } else if (method == "demolish_district") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string district_key = params.value("district_key", "");
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, district_key, colony_id]() {
            return OutlinerManager::Get().DemolishDistrictJson(planet_id, district_key, colony_id);
        });
    } else if (method == "cancel_construction") {
        uint32_t item_id = params.value("item_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([item_id]() { return OutlinerManager::Get().CancelConstructionJson(item_id); });
    } else if (method == "get_buildable_starbase_buildings") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        int32_t slot = params.value("slot", -1);
        std::string building_key = params.value("building_key", std::string());
        bool include_blocked = params.value("include_blocked", false);
        fut = TaskQueue::Get().Enqueue([starbase_id, slot, building_key, include_blocked]() {
            return OutlinerManager::Get().GetBuildableStarbaseBuildingsJson(starbase_id, slot, building_key, include_blocked);
        });
    } else if (method == "get_buildable_starbase_modules") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        int32_t slot = params.value("slot", -1);
        std::string module_key = params.value("module_key", std::string());
        bool include_blocked = params.value("include_blocked", false);
        fut = TaskQueue::Get().Enqueue([starbase_id, slot, module_key, include_blocked]() {
            return OutlinerManager::Get().GetBuildableStarbaseModulesJson(starbase_id, slot, module_key, include_blocked);
        });
    } else if (method == "get_buildable_ships") {
        fut = TaskQueue::Get().Enqueue([params]() { return ShipDesigner::Get().GetBuildableShipsJson(params); });
    } else if (method == "build_ship") {
        fut = TaskQueue::Get().Enqueue([params]() { return ShipDesigner::Get().BuildShipJson(params); });
    } else if (method == "get_starbase_upgrade") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        fut = TaskQueue::Get().Enqueue([starbase_id]() {
            return OutlinerManager::Get().GetStarbaseUpgradeJson(starbase_id, false);
        });
    } else if (method == "queue_starbase_upgrade") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        fut = TaskQueue::Get().Enqueue([starbase_id]() {
            return OutlinerManager::Get().GetStarbaseUpgradeJson(starbase_id, true);
        });
    } else if (method == "queue_starbase_building") {
        uint32_t starbase_id = params.value("starbase_id", 0u);
        int32_t slot = params.value("slot", -1);
        std::string building_key = params.value("building_key", std::string());
        fut = TaskQueue::Get().Enqueue([starbase_id, slot, building_key]() {
            return OutlinerManager::Get().QueueStarbaseBuildingJson(starbase_id, slot, building_key);
        });
    } else if (method == "queue_starbase_module") {
        uint32_t starbase_id = params.value("starbase_id", 0xFFFFFFFFu);
        int32_t slot = params.value("slot", -1);
        std::string key = params.value("module_key", std::string());
        fut = TaskQueue::Get().Enqueue([starbase_id, slot, key]() {
            return OutlinerManager::Get().QueueStarbaseModuleJson(starbase_id, slot, key);
        });
    } else if (method == "build_building") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string building_key = params.value("building_key", "");
        std::string district_type = params.value("district_type", "");
        int32_t slot_index = params.value("slot_index", -1);
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        if (district_type == "upgrade" || params.contains("building_id")) {
            uint32_t building_id = params.value("building_id", (uint32_t)slot_index);
            fut = TaskQueue::Get().Enqueue([planet_id, building_id, building_key, colony_id]() {
                return OutlinerManager::Get().UpgradeBuildingJson(planet_id, building_id, building_key,
                                                             colony_id);
            });
        } else {
            fut = TaskQueue::Get().Enqueue([planet_id, building_key, district_type, slot_index, colony_id]() {
                return OutlinerManager::Get().BuildBuildingJson(planet_id, building_key, district_type, slot_index, colony_id);
            });
        }
    } else if (method == "upgrade_building") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t building_id = params.value("building_id", 0);
        std::string upgrade_to_key = params.value("upgrade_to_key", "");
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, building_id, upgrade_to_key, colony_id]() {
            return OutlinerManager::Get().UpgradeBuildingJson(planet_id, building_id, upgrade_to_key, colony_id);
        });
    } else if (method == "get_clearable_blockers") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetClearableBlockersJson(planet_id);
        });
    } else if (method == "clear_blocker") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t deposit_id = params.value("deposit_id", 0);
        std::string deposit_key = params.value("deposit_key", "");
        fut = TaskQueue::Get().Enqueue([planet_id, deposit_id, deposit_key]() {
            return OutlinerManager::Get().ClearBlockerJson(planet_id, deposit_id, deposit_key);
        });
    } else if (method == "get_planetary_decisions") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, colony_id]() {
            return OutlinerManager::Get().GetPlanetaryDecisionsJson(planet_id, colony_id);
        });
    } else if (method == "enact_decision" || method == "enact_planetary_decision") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string decision_key = params.value("decision_key", params.value("decision", ""));
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, decision_key, colony_id]() {
            return OutlinerManager::Get().EnactDecisionJson(planet_id, decision_key, colony_id);
        });
    } else if (method == "get_terraforming_options") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetTerraformingOptionsJson(planet_id);
        });
    } else if (method == "start_terraforming") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string target_class = params.value("target_class", params.value("target_planet_class", ""));
        int32_t link_index = params.value("link_index", -1);
        fut = TaskQueue::Get().Enqueue([planet_id, target_class, link_index]() {
            return OutlinerManager::Get().StartTerraformingJson(planet_id, target_class, link_index);
        });
    } else if (method == "cancel_terraforming") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().CancelTerraformingJson(planet_id);
        });
    } else if (method == "get_planetary_features") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetPlanetaryFeaturesJson(planet_id);
        });
    } else if (method == "ascend_colony") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, colony_id]() {
            return OutlinerManager::Get().AscendColonyJson(planet_id, colony_id);
        });
    } else if (method == "get_planet_jobs") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().GetPlanetJobsJson(planet_id);
        });
    } else if (method == "set_job_priority" || method == "prioritize_job") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string job_key = params.value("job_key", "");
        fut = TaskQueue::Get().Enqueue([planet_id, job_key]() {
            return OutlinerManager::Get().SetJobPriorityJson(planet_id, job_key);
        });
    } else if (method == "set_job_workforce_limit") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string job_key = params.value("job_key", "");
        int32_t limit = params.value("limit", -1);
        fut = TaskQueue::Get().Enqueue([planet_id, job_key, limit]() {
            return OutlinerManager::Get().SetJobWorkforceLimitJson(planet_id, job_key, limit);
        });
    } else if (method == "get_planet_armies") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, colony_id]() {
            return OutlinerManager::Get().GetPlanetArmiesJson(planet_id, colony_id);
        });
    } else if (method == "set_planet_army_settings") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::optional<bool> deploy_in_orbit;
        if (params.contains("deploy_in_orbit") && !params["deploy_in_orbit"].is_null()) {
            deploy_in_orbit = params["deploy_in_orbit"].get<bool>();
        }
        std::optional<bool> include_in_builder;
        if (params.contains("include_in_builder") && !params["include_in_builder"].is_null()) {
            include_in_builder = params["include_in_builder"].get<bool>();
        }
        fut = TaskQueue::Get().Enqueue([planet_id, deploy_in_orbit, include_in_builder]() {
            return OutlinerManager::Get().SetPlanetArmySettingsJson(planet_id, deploy_in_orbit, include_in_builder);
        });
    } else if (method == "embark_all_armies") {
        uint32_t planet_id = params.value("planet_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id]() {
            return OutlinerManager::Get().EmbarkAllArmiesJson(planet_id);
        });
    } else if (method == "disband_planet_army") {
        uint32_t planet_id = params.value("planet_id", 0);
        uint32_t army_id = params.value("army_id", 0);
        fut = TaskQueue::Get().Enqueue([planet_id, army_id]() {
            return OutlinerManager::Get().DisbandArmyJson(planet_id, army_id);
        });
    } else if (method == "recruit_planet_army") {
        uint32_t planet_id = params.value("planet_id", 0);
        std::string army_key = params.value("army_key", "");
        std::optional<uint32_t> species_id;
        if (params.contains("species_id") && !params["species_id"].is_null()) {
            species_id = params["species_id"].get<uint32_t>();
        }
        uint32_t colony_id = params.value("colony_id", 0xFFFFFFFFu);
        fut = TaskQueue::Get().Enqueue([planet_id, army_key, species_id, colony_id]() {
            return OutlinerManager::Get().RecruitArmyJson(planet_id, army_key, species_id, colony_id);
        });
    }

    // 第三条链：隐形（同样的理由——一条 if / else-if 梯子太深会 C1061，所以链尾那个
    // "Method not found" 的 else 也摘出来，单独放到链外）
    if (fut.valid()) {
    } else if (method == "set_cloaking" || method == "probe_fleet_flags") {
        fut = TaskQueue::Get().Enqueue([method, params]() {
            if (method == "probe_fleet_flags") {
                return GalaxyManager::Get().ProbeFleetFlags(params.value("fleet_id", 0xFFFFFFFFu),
                                                            params.value("offset", 0x1268u),
                                                            params.value("length", 0x100u));
            }
            return GalaxyManager::Get().SetCloaking(params.value("fleet_id", 0xFFFFFFFFu),
                                                    params.value("active", true),
                                                    params.value("dry_run", false));
        });
    } else if (method == "install_timed_modifier_capture") {
        fut = TaskQueue::Get().Enqueue([]() {
            bool ok = CommandBuilder::Get().InstallTimedModifierCapture();
            return nlohmann::json{ {"success", ok} };
        });
    } else if (method == "get_timed_modifier_capture") {
        fut = TaskQueue::Get().Enqueue([]() {
            auto& cb = CommandBuilder::Get();
            nlohmann::json arr = nlohmann::json::array();
            const int n = cb.CapturedTimedModifierCount();
            for (int i = 0; i < n; ++i) {
                nlohmann::json w = nlohmann::json::array();
                for (int k = 0; k < 6; ++k) w.push_back(cb.CapturedTimedModifierWord(i, k));
                arr.push_back({ {"self", cb.CapturedTimedModifierSelf(i)},
                                {"type", cb.CapturedTimedModifierType(i)},
                                {"days", cb.CapturedTimedModifierDays(i)},
                                {"words", w} });
            }
            return nlohmann::json{ {"success", true}, {"count", n}, {"items", arr} };
        });
    } else if (method == "clear_timed_modifier_capture") {
        fut = TaskQueue::Get().Enqueue([]() {
            CommandBuilder::Get().ClearTimedModifierCapture();
            return nlohmann::json{ {"success", true} };
        });
    } else if (method == "storm_fields_at_system") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return OutlinerManager::Get().StormFieldsAtSystem(params.value("system_id", 0xFFFFFFFFu));
        });
    } else if (method == "storms_in_system") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return OutlinerManager::Get().StormsInSystem(params.value("system_id", 0xFFFFFFFFu));
        });
    } else if (method == "probe_storms") {
        fut = TaskQueue::Get().Enqueue([]() { return OutlinerManager::Get().ProbeStormsJson(); });
    } else if (method == "get_module_base") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetModuleBase(); });
    } else if (method == "probe_buildable") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return OutlinerManager::Get().ProbeBuildableJson(params.value("item_id", 0u),
                                                             params.value("words", 12u));
        });
    } else if (method == "probe_deposits") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return OutlinerManager::Get().ProbeDepositsJson(params.value("key", std::string()),
                                                             params.value("holder_type", 0xFFFFFFFFu),
                                                             params.value("holder_id", 0xFFFFFFFFu),
                                                             params.value("limit", 50u),
                                                             params.value("dump_words", 0u));
        });
    } else if (method == "probe_build_queue") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return GalaxyManager::Get().ProbeBuildQueue(params.value("colony_id", 0xFFFFFFFFu));
        });
    } else if (method == "get_colonies") {
        fut = TaskQueue::Get().Enqueue([]() { return GalaxyManager::Get().GetColoniesJson(); });
    } else if (method == "probe_colony_structures") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return OutlinerManager::Get().ProbeColonyStructuresJson(params.value("colony_id", 0xFFFFFFFFu));
        });
    } else if (method == "probe_colony") {
        fut = TaskQueue::Get().Enqueue([params]() {
            return GalaxyManager::Get().ProbeColony(params.value("colony_id", 0xFFFFFFFFu),
                                                    params.value("offset", 0u),
                                                    params.value("length", 0x100u));
        });
    }

    if (!fut.valid()) {
        return {
            {"jsonrpc", "2.0"},
            {"error", {
                {"code", -32601},
                {"message", "Method not found: " + method}
            }},
            {"id", id}
        };
    }

    // Wait for the game main thread to complete the task
    if (fut.wait_for(std::chrono::milliseconds(30000)) == std::future_status::timeout) {
        LOGF("[IPC_TIMEOUT] Task timed out for method: %s", method.c_str());
        return {
            {"jsonrpc", "2.0"},
            {"error", {
                {"code", -32000},
                {"message", "Main thread task timed out (game may be loading, frozen, or background throttled)"}
            }},
            {"id", id}
        };
    }

    nlohmann::json result = fut.get();
    if (result.is_object() && result.contains("error") && result["error"].is_object()) {
        return {
            {"jsonrpc", "2.0"},
            {"error", result["error"]},
            {"id", id}
        };
    }

    return {
        {"jsonrpc", "2.0"},
        {"result", result},
        {"id", id}
    };
}

void IPCServer::WorkerLoop() {
    const wchar_t* pipe_name = kPipeName;

    while (is_running_) {
        pipe_handle_ = CreateNamedPipeW(
            pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            nullptr
        );

        if (pipe_handle_ == INVALID_HANDLE_VALUE) {
            LOGF("[IPC_ERROR] Failed to create named pipe (0x%08X). Retrying in 1s...", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        LOGF_DEBUG("%s", "[IPC] Named pipe created. Awaiting client connection...");
        BOOL connected = ConnectNamedPipe(pipe_handle_, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!is_running_) {
            CloseHandle(pipe_handle_);
            break;
        }

        if (connected) {
            LOGF_DEBUG("%s", "[IPC] Client connected to Named Pipe.");
            std::string read_buffer;
            char chunk[4096];

            while (is_running_) {
                DWORD bytes_read = 0;
                BOOL ok = ReadFile(pipe_handle_, chunk, sizeof(chunk) - 1, &bytes_read, nullptr);
                if (!ok || bytes_read == 0) {
                    DWORD err = GetLastError();
                    if (err == ERROR_BROKEN_PIPE) {
                        LOGF_DEBUG("%s", "[IPC] Client disconnected normally.");
                    } else {
                        LOGF("[IPC] Pipe read error: 0x%08X", err);
                    }
                    break;
                }

                chunk[bytes_read] = '\0';
                read_buffer.append(chunk, bytes_read);

                // Process all full lines
                size_t pos;
                while ((pos = read_buffer.find('\n')) != std::string::npos) {
                    std::string line = read_buffer.substr(0, pos);
                    read_buffer.erase(0, pos + 1);

                    // Trim CR if present
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    if (line.empty()) continue;

                    nlohmann::json resp_json;
                    try {
                        auto req_json = nlohmann::json::parse(line);
                        const auto t0 = std::chrono::steady_clock::now();
                        resp_json = ProcessRequest(req_json);
                        if (Logger::Get().DebugEnabled()) {
                            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
                            const bool failed = resp_json.contains("error") && resp_json["error"].is_object();
                            LOGF_DEBUG("[IPC_REQ] %s id=%s %lld ms%s%s", req_json.value("method", "").c_str(),
                                       req_json.value("id", nlohmann::json(nullptr)).dump().c_str(), ms,
                                       failed ? " error " : "",
                                       failed ? std::to_string(resp_json["error"].value("code", 0)).c_str() : "");
                        }
                    } catch (const std::exception& ex) {
                        resp_json = {
                            {"jsonrpc", "2.0"},
                            {"error", {
                                {"code", -32700},
                                {"message", std::string("Parse error: ") + ex.what()}
                            }},
                            {"id", nullptr}
                        };
                    }

                    std::string out_str = resp_json.dump() + "\n";
                    DWORD written = 0;
                    WriteFile(pipe_handle_, out_str.data(), (DWORD)out_str.size(), &written, nullptr);
                    FlushFileBuffers(pipe_handle_);
                }
            }
        }

        DisconnectNamedPipe(pipe_handle_);
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
}

} // namespace bridge









