#pragma once

#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace bridge {

class ContactsManager {
public:
    static ContactsManager& Get();


    bool Init(uintptr_t base_address);

    // Queries
    nlohmann::json GetContactsInfo(const std::string& mode);
    nlohmann::json GetSummaryJson();

    // Read-only probe: walk the game's own first-contact table (TPdxRef<CFirstContact>) and report
    // every entry with its stage object's key, clues, difficulty, days_left and status.
    // No command is posted and nothing is written — it exists to make the stage observable.
    nlohmann::json ProbeFirstContactsJson();

    // The first-contact table, readable form: one record per CFirstContact, with the counterparty's
    // name and the assigned envoy. all=true also lists other countries' contacts (the table is
    // global); all=false keeps only the player's own (owner == player country).
    nlohmann::json GetFirstContactsJson(bool all);
    // 解锁一条锁着的第一次接触（把记录 +0xC0 置 1）
    nlohmann::json AdvanceFirstContactJson(uint32_t country_id, uint32_t value);
    // 把 +0xC0 置 1 再立刻还原（最终状态不变）：让界面重读一次这条接触
    nlohmann::json PokeFirstContactJson(uint32_t country_id, bool hold);
    // 置 1 + 投事件（settle=false）或只收尾写回 0（settle=true）
    nlohmann::json AdvanceFirstContactStageJson(uint32_t country_id, uint64_t ope, int option, bool settle);

    // Read-only probe: one leader's CLeader::location / council_location, decoded as
    // {type, assignment, id, position}. type 8 = first contact, assignment = EEnvoyAssignment index.
    nlohmann::json ProbeLeaderJson(uint32_t leader_id);

    // Read-only probe: raw bytes of one CFirstContact, plus every qword in the window decoded —
    // to find the stage event (the view renders its options as buttons, so the event is the way in).
    nlohmann::json ProbeFirstContactJson(uint32_t contact_id, int32_t offset, int32_t length);

    // Assign one envoy to one first contact: CAssignLeaderCommand with
    // CLeaderLocation{type 8 (first contact), assignment 4 (first_contact), id = contact id}.
    // dry_run only runs the command's IsValid and reports the engine's reason.
    nlohmann::json AssignEnvoyJson(uint32_t leader_id, uint32_t contact_id, bool dry_run, bool reset_cooldown);

    // Diplomatic actions toward one empire, as the diplomacy view lists them (Layer 2): every
    // CDiplomaticActionType the game shows for that pair, with whether the player can propose it
    // now and, when not, the game's reason (CDiploActionProposeCommand::IsValid)
    nlohmann::json GetDiplomaticActionsJson(uint32_t country_id);
    // propose one (native CDiploActionProposeCommand, as the view's confirm button)
    // federation_type（2026-11-21）：只对 action_invite_to_federation 有意义——组建新联邦时写进
    // CInviteToFederationAction::federation_type（+0xB0）。空串则沿用引擎生产者填的默认类型。
    nlohmann::json ProposeDiplomaticAction(uint32_t country_id, const std::string& action,
                                           const std::string& federation_type = std::string(),
                                           const nlohmann::json& terms = nlohmann::json::object(),
                                           const nlohmann::json& subsidies = nlohmann::json::object(),
                                           bool dry_run = false);
    // 组建联邦时**可选的联邦类型**。only_type 空 → 只回类型清单（纯读、不造对象）；给 key → 对**那一个**
    // 类型造一份邀请动作、写 +0xB0、跑 IsValid（一次只造一个，崩了也定位得到）。
    // 键在类型对象 +0x20（2026-11-21 只读探针实测），vtable RVA 0x2388990。
    nlohmann::json ProbeFederationTypesJson(uint32_t country_id, const std::string& only_type);
    // ⚠ 2026-11-21：上一行那条探针一调就静默崩（两次）。这是**纯只读**版：只把数据库头与每个条目的
    // 指针 / vtable RVA / 候选键（+0x20 与 +0x28）读出来，不造对象、不写内存、不调 native。
    nlohmann::json ProbeFederationTypeDbJson();
    // 只读探针：联邦对象。CCountry+0x1C38 = federation ref（低 32 位 id，高位 0xFFFFFFFF；0 = 不在联邦）。
    // 从 TPdxRef<CFederation>::_pDatabase 找对象表，吐 db 头 / 对象头 / progression 头（全是 qword）。
    nlohmann::json ProbeFederationJson(uint32_t fed_id);
    // 全银河联邦列表（只读）：id / 名字 / 成员数 / 类型 key / 等级 / 累计经验 / 本级经验 / 团结度。
    // 经验与团结度按 ×1e5 定点还原（2026-11-21 用界面的 1313、89 对过账）。
    nlohmann::json GetFederationsJson();
    // 只读探针：**某几个国家是不是星海共同体成员**——不读共同体对象，问引擎：
    // 对每个 id 造一条 leave_community_command 与 join_community_command，只跑 IsValid（不投递）。
    // 成员应当 leave 可过、join 被拒；两种引用编码（裸 id / 0x01000000|id）各试一次，
    // 免得「编码猜错」被误读成「不是成员」。
    nlohmann::json ProbeCommunityMembershipJson(const std::vector<uint32_t>& ids);
    // 读抓到的 CGalacticCommunity 对象：成立日 / 领袖 / 成员 / 议会 / 在台与队列与已通过 / 倒计时。
    // 数组字段（+0x180 起）是 {…, data +8, size +0x10} 的 4 字节国家引用数组，与议案支持者同一形状。
    nlohmann::json ReadCommunityCaptureJson();
    // 读 hook 抓到的 CAgreement（附庸协议）：owner / target / date_added / date_changed /
    // active_status，外加对象里能读到的键（preset_* / subject_* / specialist_* …）。
    // 前提：先 install_agreement_capture，再让游戏存一次档（序列化会带出每个协议的 this）。
    nlohmann::json GetAgreementsJson();
    // 分步探针：造协议动作 → 报告 →（可选）写条款 →（可选）删除，全程不投递。
    nlohmann::json ProbeAgreementAction(uint32_t country_id, const std::string& action_key,
                                        const nlohmann::json& terms, const nlohmann::json& subsidies,
                                        bool do_write, bool do_delete);
    // 共同体全部议案（只读）：TPdxRef<CResolution> 库表，逐条 id / type / 提案国 / 目标 /
    // 支持者 / 反对者 / 冻结日 / 通过日（日期按 360 天年、30 天月、单位 24 解出 Y.M.D）。
    nlohmann::json GetResolutionsJson();
    // 当前**能不能提**每一条议案：CResolutionTypeDatabase 里逐个类型造一条
    // propose_resolution_command（类型指针 + 国家）只跑 IsValid——potential/allow
    // （含「必须有监管人」「GDF 要已存在」这类脚本条件）由引擎自己判，理由原样带回。
    // target_id 给 0xFFFFFFFF 表示「没有目标」：带 target = yes 的那批（天灾指定、谴责…）会因此
    // 没有答案；要问它们就指定一个目标国家。
    nlohmann::json ProbeResolutionTypesJson(uint32_t country_id, uint32_t target_id, bool all);
    // 提交一条议案：按 key 在 CResolutionTypeDatabase 里取类型对象 → 造
    // propose_resolution_command（type + country + target）→ IsValid → Post。
    // dry_run 只跑 IsValid、不投递；拒绝原因原样带回。
    nlohmann::json ProposeResolution(const std::string& type_key, uint32_t target_id, bool dry_run);
    // 对**在台上**的议案投一票（CCastVoteCommand：resolution + country + vote + force）。
    // vote 的取值（赞成／反对／弃权）由调用方给，桥不猜。
    nlohmann::json VoteResolution(uint32_t resolution_id, int vote_value, bool dry_run);
    // 提前结束本次会议（CEndSenateSessionCommand，仅星海监管人；不合格时 IsValid 自己会拒）。
    nlohmann::json EndSenateSession(uint32_t resolution_id, bool dry_run);
    // 对**排队中**的议案表态 / 理事会手段：
    //   support / oppose / abstain(撤支持) / veto（否决，移出队列并进提交冷却）/
    //   freeze（冻结 1440 天不能上台）/ emergency（紧急措施，绕过队列与休会直接上台）
    // 都是 resolution + country 的轻命令，IsValid 由引擎判，理由原样回。
    nlohmann::json QueuedResolutionAction(uint32_t resolution_id, const std::string& action, bool dry_run);
    // 只读探针：扫联邦对象与 progression 的 qword，找指向 CFederationLaw 对象（vtable RVA 0x2388C28）的指针，
    // 回 {where, offset, obj, key}。用来找「当前生效的法律」存在哪。
    nlohmann::json ProbeFederationLawsJson(uint32_t fed_id);
    // 当前法律（2026-11-21 打通）：progression+0x2E8 → 容器（0x10 表头后每 0x18 一槽，
    // 槽 +0x00 = CFederationLaw*，空槽为 0）。回 {laws:[{key, law_index, ptr}], pending:[…]}。
    // 深一层才见——之前按「qword 直接指向 law」扫是零命中，容器头不是 law 的 vtable。
    nlohmann::json GetFederationLawsJson(uint32_t fed_id);
    // 只读探针：CDiplomaticActionTypeDatabase 里带 law 的动作类型 + CFederationLawDatabase /
    // CFederationLawCategoryDatabase 的条目键。用来确认能否自己造「法律表决」动作。
    nlohmann::json ProbeLawDbJson();
    // 发起一条联邦法律表决：造 CVoteForFederationLawAction（动作类型 action_vote_for_federation_law）
    // → 把法律对象写进 +0x50 → 包 CDiploActionProposeCommand 投递。dry_run 只跑 IsValid。
    nlohmann::json ProposeFederationLaw(const std::string& law_key, bool dry_run, uint32_t recipient_id = 0xFFFFFFFFu);
    // 贸易协定（2026-11-21 抓现场后接线）：生产者造 action_offer_trade_deal → 在 +0x50 的内嵌
    // CTradeDeal 里写两侧的资源表（26 格 i64 定点 ×1e5，索引 = 资源声明序）与 length（年）→ 投递。
    // give/take/monthly_* 都是 [[资源索引, 数量], …] 的数组。dry_run 只造 + 回读，不写不投。
    // sensor_charts / sensor_link 是发起方那侧 +0x104 / +0x105 的两个字节（「通信同步 / 主动传感链接」）。
    // 换联邦类型表决（2026-11-21 抓 UI 现场后接）：生产者不造，手工摆 CVoteForFederationTypeAction：
    // vtable RVA 0x23B7548、基类 0x2388BA0、+0x20 发起国、+0x40 动作类型对象、+0x50 CFederationType*、
    // +0x58 名字（UI 传的是模块内共享对象 base+0x234A778）。
    // 只读探针：列出 CTradeDeal 库（找界面正在编辑的那份草稿、配合差分找界面字段）
    nlohmann::json ProbeTradeDealsJson();
    // 只读探针：idler 里像窗口的对象（关/开窗口各取一次，差集找新窗口）
    nlohmann::json ProbeWindowsJson();

    nlohmann::json ProposeFederationType(const std::string& type_key, bool dry_run);

    nlohmann::json ProposeTradeDeal(uint32_t country_id, const nlohmann::json& give,
                                    const nlohmann::json& monthly_give, const nlohmann::json& take,
                                    const nlohmann::json& monthly_take, int32_t years, bool reserve,
                                    bool sensor_charts, bool sensor_link, const nlohmann::json& specimens,
                                    const nlohmann::json& fleets, bool dry_run);

    // 只读探针：造出一个动作对象但**不过 IsPotential 那道"是否上界面"的筛**，报它存在与否、
    // vtable、IsPotential(true/false) 两种答案，以及对象头 0x80 字节。用于 is_listed = no 的动作
    // （求和就是这一类：is_listed = no，永远不会出现在外交动作表里）。造完立刻删掉，不投递。
    nlohmann::json ProbeDiplomaticAction(uint32_t country_id, const std::string& action_key);
    // 给一场战争里的「我方（防守方）」设战争目标：CDiploActionSetDefenderWarGoalCommand。
    // 引擎没有的字段（defender_war_goal）是内嵌 CWarGoal，这里按 0x20 字节原样写进命令；dry_run 只跑 IsValid。
    nlohmann::json SetDefenderWarGoal(uint32_t war_id, const std::string& goal_key, bool dry_run, bool swap);
    // 程序化递交求和提案：用本会话捕获到的 COfferPeaceAction 模板造一个动作，包进
    // CDiploActionProposeCommand。dry_run 只跑 IsValid 不投递。前提：本会话已经从界面递交过一次
    // （模板来自那次捕获），否则没有模板可用。
    nlohmann::json TryPeaceOffer(uint32_t war_id, uint32_t target_id, int status, bool dry_run,
                                 bool synthetic = false, uint64_t f40 = 0, uint32_t f4c = 0, bool force = false);
    // —— 待回应的外交动作（2026-11-20 读出布局）——
    // CCountry+0x1950 = ref_array<SIncomingAction>{data ptr, size, cap}；元素 +0x08 = CDiplomaticAction*。
    // 当下的样本全是求和提案（COfferPeaceAction）：+0x20 actor、+0x30 低 dword = id 的 tag、
    // +0x58/0x5C/0x60/0x64 = offer 的 actor/recipient/war/status。
    // respond / vote 命令要的 id = tag<<32 | serial（本档 serial 恒 0）。
    // 元素步长未定：size>1 时只报第一条，并在 note 里说明。
    nlohmann::json GetIncomingActionsJson(uint32_t country_id);
    // 银河范围扫 proposed_actions（+0x1980），只报 COfferPeaceAction 的待表决提案。
    // 成员侧两张表都是空的（2026-11-20 实测），提案只在**提案方**的这张表里——投票前先用它找 id。
    nlohmann::json GetPendingVotesJson();
    // 应答一条待回应的提案：CDiploActionRespondCommand（id +0x20，accept 是 **bool** 在 +0x28；实测引擎只写 1 字节）
    nlohmann::json RespondDiploAction(uint64_t id, bool accept, bool dry_run);
    // 投我们自己那一票：CDiploActionVoteCommand（id +0x20 / voter +0x28 / vote +0x2C，实测与 SDK 一致）
    nlohmann::json VoteDiploAction(uint64_t id, int vote, uint32_t voter, bool dry_run);
    // 宣战：抄 propose 旁听里的 CDeclareWarAction 模板（vtable 0x23B9EB0，每会话要在界面宣战一次），
    // 改 actor(+0x20)/recipient(+0x24) 与内嵌 CWarGoal 的类型指针(+0x58)，包进 propose 命令。
    // 战争目标里的 war/actor/target 保持模板里的 -1：引擎在对方接受后才造出战争。goal_key 空则沿用模板类型。
    nlohmann::json DeclareWar(uint32_t target_id, const std::string& goal_key, bool dry_run);
    // 对某个国家**可选的战争目标**：逐个 CWarGoalType 造一份带该目标的动作、只跑 IsValid（不投递），
    // 回 {key, valid, reason}。与法令/外交动作/可挖遗址同一套「问引擎」的做法。
    nlohmann::json ProbeWarGoalsJson(uint32_t target_id, bool all);
    // 只读探针：CWar 对象的一段内存（看参战方数组 / 战争目标的真实布局）
    // follow=true 时把 war+offset 处的值当作指针，从那个地址开始吐 length 字节（读参战方数组用）
    nlohmann::json ProbeWarJson(uint32_t war_id, uint32_t offset, uint32_t length, bool follow = false);
    // 只读探针：CCountry 对象的一段内存，按 qword 吐。用来定位 incoming_actions（待回应的外交动作：
    // 存档里挂在**收件方**国家的 incoming_actions 下，含 id / actor / recipient / war / timeout）。
    nlohmann::json ProbeCountryJson(uint32_t country_id, uint32_t offset, uint32_t length);

private:
    ContactsManager() = default;
    // a new CDiplomaticAction of that type from the player to the country, or null; *shown: the
    // game would list it (CDiplomaticAction::IsPotential). The caller owns it
    void* NewAction(void* type, uint32_t country_id, bool* shown);

    void* GetPlayerCountry();
    std::string LocalizeKey(const std::string& key);

    uintptr_t base_address_{ 0 };
};

} // namespace bridge
