# 工具参考（187 个）

> 自动生成，来源是插件的实际注册与源码里的参数 schema。**参数名、类型、是否必填以代码为准**；
> 「做什么」是玩法口径，不含任何一局的具体数字。状态：✅ 已在游戏里跑通；⚠ 桥里有、插件已接，但没实测过。


## 帝国 · 执政 · 资源 · 科研

### `stellaris_adopt_tradition`

采纳一棵传统树，或解锁其中一个传统

参数：`tradition_key` string

状态：✅ 已跑通

### `stellaris_agenda_options`

内阁议程清单：`potential` 与 `allow` 都评估，标出会被静默回滚的

参数：`raw` boolean、`all` boolean

状态：✅ 已跑通

### `stellaris_armies`

帝国全部驻军与运输军两张总表；不知道哪颗星有兵时先看它

参数：无

状态：✅ 已跑通

### `stellaris_assign_leader`

派人：`slot` = council / ship / governor / army / envoy（特使还要 `task`）；引擎拒绝原因原样回

参数：`leader_id` number（必填）、`slot` string（council / ship / governor / army / envoy）（必填）、`target_id` number（必填）、`task` string

状态：✅ 已跑通（部分参数未单独验）

### `stellaris_cancel_research`

取消某个槽：槽变空、科技回候选池、**进度保留**。空槽不浪费——点数攒着，选中后双倍速释放

参数：`slot` string（必填）

状态：✅ 已跑通

### `stellaris_change_civics`

改国民理念：真花 unity，投前自查（已采纳 / 点数不够都拒）；默认只印计划，`confirm: true` 才投；改完进 20 年冷却

参数：`add` array<string>、`remove` array<string>、`confirm` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_choose_ascension_perk`

选一个飞升天赋；引擎校验，没空槽会回原因

参数：`perk` string

状态：✅ 已跑通

### `stellaris_civics`

国民理念（只读）：采纳的国策、国策点预算、改革状态；给 `civic_key` 问单条能否采纳，并带出说明 / 效果 / 前置

参数：`civic_key` string、`effects` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_dismiss_leader`

解雇领袖，**不可逆**；统治者被拦、裂隙探索中被引擎拒

参数：`leader_id` number（必填）

状态：✅ 已跑通

### `stellaris_edicts`

生效与可开法令：脚本条件三值判定，标出「引擎接受、下月被静默回滚」的那些

参数：`all` boolean、`available_only` boolean

状态：✅ 已跑通

### `stellaris_galaxy_settings`

开局星系设置（只读，53 项）：银河模板 / 形状、帝国数、中后期年、难度、天灾、虫洞与星门、各种倍率；`null` = 跟随模板

参数：`raw` boolean

状态：✅ 已跑通

### `stellaris_government`

政体、权威、国策、议会席位、议程进度

参数：无

状态：✅ 已跑通

### `stellaris_hire_leader`

从招募池雇一个（先查名字再投，拒绝原因原样回）

参数：`candidate_id` number（必填）

状态：✅ 已跑通

### `stellaris_launch_council_agenda`

启动已跑完的议程；`early: true` 提前启动、花凝聚力

参数：`early` boolean

状态：✅ 已跑通

### `stellaris_leader_assignments`

某个领袖**能派到哪**：议会席位、可派的舰船与行星、被拒原因

参数：`leader_id` number（必填）

状态：✅ 已跑通

### `stellaris_leaders`

已雇领袖与招募池，按职业分组；位置是 `location`，`class` 可筛、`raw` 给原始 JSON

参数：`raw` boolean、`class` string

状态：✅ 已跑通

### `stellaris_market`

银河市场：费率、可交易资源的买卖单价与批量、本方的月度挂单。买用 `market_trade`，挂单用 `market_monthly`

参数：无

状态：✅ 已跑通

### `stellaris_market_monthly`

月度挂单 / 撤单：撤单必须给 `order_id`（引擎只删完全吻合的一条）

参数：`resource` string、`action` string（buy / sell）、`amount` number、`price_limit` number、`cancel` boolean、`order_id` number

状态：✅ 已跑通

### `stellaris_market_trade`

即时买卖：`units` 是单位数，**不足一批按一批算**（矿物 100/批，给 10 会成交 100）

参数：`resource` string（必填）、`action` string（buy / sell）（必填）、`units` number（必填）

状态：✅ 已跑通

### `stellaris_player_country`

玩家自己的国家：id、原始引用、显示名。`contacts` 不含自己，游牧又没有星系可查

参数：无

状态：✅ 已跑通

### `stellaris_research_state`

三个研究槽的当前项目与候选（key / 名称 / 层 / 成本）；**候选随时变，选前重读**

参数：无

状态：✅ 已跑通

### `stellaris_researched_techs`

已研究科技表：`level` 是研究次数（循环科技 ×N），`all: true` 列全部；只能读玩家国家

参数：`country` number、`all` boolean

状态：✅ 已跑通

### `stellaris_resources`

帝国资源一张小表：26 项一行一项（库存 / 上限、月净），抬头标**满仓**与**负增长**；`only: ["alloys"]` 只看一项

参数：`only` array<string>

状态：✅ 已跑通

### `stellaris_sectors`

星域一览：每域的首府与殖民地清单（人口 / 规模 / 在建 / 警报数）。**方舟不在星域里**

参数：无

状态：✅ 已跑通

### `stellaris_select_leader_trait`

从**本次可选**的特质里挑一个；不在选项里就回可选项

参数：`leader_id` number（必填）、`trait_key` string（必填）

状态：✅ 已跑通

### `stellaris_select_research`

把一项科技放进研究槽（参数叫 `slot`）

参数：`slot` string（必填）、`tech_key` string（必填）

状态：✅ 已跑通

### `stellaris_set_council_agenda`

换下一个议程（key 取自 `stellaris_government`）；**换掉会丢掉当前进度**

参数：`agenda_key` string

状态：✅ 已跑通

### `stellaris_toggle_edict`

开关一条法令

参数：`edict_key` string、`enabled` boolean

状态：✅ 已跑通

### `stellaris_traditions`

已采纳传统树、飞升天赋、可采纳项

参数：无

状态：✅ 已跑通


## 星图与天体

### `stellaris_arc_furnace_candidates`

电弧熔炉挑星系：按天体数排（`sort: "jumps"` 改按距离），先筛掉已有熔炉、没有熔融星球的；给 `fleet_id` 时引擎逐颗验

参数：`fleet_id` number、`min_bodies` number、`from_system_id` number、`sort` string（bodies / jumps）、`limit` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_build_megastructure`

造巨型结构：天体给 `planet_id`，开放空间给 `system_id` + `toward_system_id`；**默认只印计划，`confirm: true` 才投**

参数：`fleet_id` number（必填）、`type` string（必填）、`system_id` number、`planet_id` number、`toward_system_id` number、`queue` boolean、`confirm` boolean

状态：✅ 已跑通

### `stellaris_build_outpost`

建哨站：星系要勘察完、下命令即扣合金，拒绝原因一次列全。**哨站 0 槽**，要升级才有模块位

参数：`fleet_id` number（必填）、`system_id` number（必填）、`queue` boolean

状态：✅ 已跑通（部分参数未单独验）

### `stellaris_build_station`

采矿 / 研究 / 观测站：只能建在**自家疆域内**；不给 `station` 就先试 mining 再试 research

参数：`fleet_id` number（必填）、`planet_id` number（必填）、`station` string、`queue` boolean

状态：✅ 已跑通

### `stellaris_buildable_megastructures`

这支工程船在这星系能建什么：位置形态、花费、合法 site；没有 site 时给引擎的拒绝原因

参数：`fleet_id` number（必填）、`system_id` number（必填）、`raw` boolean

状态：✅ 已跑通

### `stellaris_bypasses`

虫洞 / 星门清单：类型、所在星系、对端、网络、`usable`。**虫洞没探过就是 false**，探它用 `stellaris_explore_wormhole`

参数：`only` string（wormhole / gateway / lgate / all）、`limit` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_dyson_swarm_candidates`

戴森云挑星系：只吃恒星产出、只能单星（非黑洞 / 中子星 / 脉冲星）；按 1 文物 = 1 战略 = 2 科研 = 8 能量折算排序

参数：`fleet_id` number、`sort` string（value / jumps）、`from_system_id` number、`limit` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_explore_wormhole`

派船探一条虫洞（任意舰船都行，工程船实测也能探完）；不给 `fleet_id` 就挑空闲民用船（科学 → 工程 → 殖民），`queue: false` 可改为顶替指令

参数：`bypass_id` number、`fleet_id` number、`force` boolean、`queue` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_find_path`

问「这支舰队到目标星系走哪、几天」——按舰队自身速度 / FTL / 边检算；路线里会标 `via: bypass`。天数是正常量级；开着「立即移动」到位很快，那是调试开关

参数：`fleet_id` number（必填）、`to_system_id` number（必填）

状态：✅ 已跑通

### `stellaris_find_systems`

找星系 id：`name` 做子串匹配；`purpose` = outpost / deposit / unsurveyed 问「最近的合格星系」。**移动前先来这儿**

参数：`name` string、`purpose` string（outpost / deposit / unsurveyed）、`resource` string、`from_system_id` number、`limit` number

状态：✅ 已跑通

### `stellaris_galaxy_map`

星图：id / 名字 / 归属 / 情报度 / 跳数 / 标记（S 勘察 · C 殖民 · B 基地 · F 本方舰队 · A 异常 · K 宣称 · M 巨构）+ 航道边。**不给 `jumps` 就是全银河**；`center_system_id`、`limit` 可调

参数：`center_system_id` number、`jumps` number、`limit` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_megastructures`

巨型结构（只读）：自己的 + 已知星系的；`owner: "own"` / `type` 可筛，`megastructure_id` 看单条

参数：`megastructure_id` number、`owner` string（all / own）、`type` string、`raw` boolean

状态：✅ 已跑通

### `stellaris_orbit_planet`

让舰队环绕某个天体（跨星系先跃迁）。**研究异常不用先环绕**

参数：`fleet_id` number、`planet_id` number、`queue` boolean

状态：✅ 已跑通

### `stellaris_storms`

宇宙风暴：名字 / 类型 / 圆心 / 半径 / 圈内星系 / 年龄 / 距下一跳。判「殖民地在不在风暴里」直接查圈内列表；路径一列**待测**

参数：`names` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_upgrade_megastructure`

升级巨型结构；同样默认只印计划、`confirm` 才投

参数：`megastructure_id` number（必填）、`type` string（必填）、`confirm` boolean

状态：✅ 已跑通

### `stellaris_use_bypass`

走虫洞 / 星门；`to_system_id` 指定从哪个相连星系出来，**省掉就自己找对端**

参数：`fleet_id` number（必填）、`bypass_id` number（必填）、`to_system_id` number、`queue` boolean

状态：✅ 已跑通


## 舰船与舰队

### `stellaris_build_ship`

正规入队造船：`design_id` + `count` 1–10，逐艘入队，真实合金与工期；**新船自成一支舰队**

参数：`design_id` number（必填）、`count` number、`starbase_id` number、`system_id` number、`species_id` number

状态：✅ 已跑通

### `stellaris_buildable_ships`

船坞现在造得起哪些设计（合金 / 工期）；给 `design_id` 只问一个并带引擎原因。**空列表 = 造不起**，不是造不了

参数：`starbase_id` number、`system_id` number、`design_id` number、`species_id` number

状态：✅ 已跑通

### `stellaris_cancel_ship_orders`

取消一支或多支舰队的全部指令，舰队就停在那儿

参数：`fleet_ids` array<number>（必填）

状态：✅ 已跑通

### `stellaris_create_ship_design`

新建设计：照本国该舰级**第一个**设计的段抄，按 `slots` / `core_components` 换件；投完读回新 id

参数：`ship_size` string（必填）、`name` string（必填）、`slots` array<object>、`core_components` array<string>、`raw` boolean

状态：✅ 已跑通

### `stellaris_delete_ship_design`

删一条设计；编制还在用时先拦（列出舰队），`force: true` 硬删；已造的船不受影响

参数：`design_id` number（必填）、`force` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_disband_fleets`

解散舰队，**船真的没了、不可撤销**

参数：`fleet_ids` array<number>（必填）

状态：✅ 已跑通

### `stellaris_explore`

科研船一次探索：直接投移动 + 勘察，**不问确认**；路线上有敌舰就撤

参数：`targets` number、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_find_ships`

按设计号 / 星系找船（只读），可带 `limit`

参数：`design_id` number、`system_id` number、`limit` number

状态：✅ 已跑通

### `stellaris_fleet_composition`

一支（或全部）军事舰队的真实构成：共几艘、每条编制要几艘、差多少。**只在本地编译的桥上准确**

参数：`fleet_id` number

状态：✅ 已跑通

### `stellaris_fleet_templates`

编制模板库（不依赖某支舰队）：名字、`template_id`、编制总数、现有艘数、每条设计的 quota；兽群靠舰型认，`fauna: true` 只留它们

参数：`fauna` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_fleets`

舰队列表（含借出 / 借入 / 方舟）：每支带 owner / controller 与 `relation`（own · lent · borrowed · ark）；借出的带剩余天数，方舟带 colony_id 与轰炸姿态

参数：`relation` string（all / own / lent / borrowed / ark）

状态：✅ 已跑通

### `stellaris_home_base`

读 / 改舰队母港：只给 `fleet_id` 是读，加 `starbase_id` 是改

参数：`fleet_id` number（必填）、`starbase_id` number

状态：✅ 已跑通

### `stellaris_merge_fleets`

合并两支以上舰队

参数：`fleet_ids` array<number>（必填）

状态：✅ 已跑通

### `stellaris_move_ship`

把任意舰队开到某星系（军用 / 科研 / 工程 / 殖民 / 运输都行）；`queue: true` 排到队尾；回执带目标星系

参数：`fleet_id` number（必填）、`system_id` number（必填）、`queue` boolean

状态：✅ 已跑通

### `stellaris_reinforce_fleet`

按编制补员（投船坞订单）

参数：`fleet_id` number

状态：✅ 已跑通

### `stellaris_set_bombardment_stance`

读 / 改轰炸姿态：不给 `stance` 列出全部档位；稀有的被科技或政策挡，拒绝原文回传；改完过一 tick 生效。**方舟也是舰队，同样适用**

参数：`fleet_id` number、`stance` string、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_set_cloaking`

启动 / 取消隐形；**引擎不拦冷却**，冷却中再启动照样过校验

参数：`fleet_id` number（必填）、`active` boolean（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_set_fleet_template_design`

把编制条目换成另一个设计：**只影响以后的补员**，现有船不动

参数：`fleet_id` number（必填）、`from_design_id` number（必填）、`to_design_id` number（必填）

状态：✅ 已跑通

### `stellaris_set_fleet_template_quota`

改编制里某个设计的目标数量

参数：`fleet_id` number、`design_id` number、`target_quota` number

状态：✅ 已跑通

### `stellaris_set_ship_upgrade_design`

给全队写上「待改装成哪个设计」，不造船也不改装

参数：`fleet_id` number（必填）、`to_design_id` number（必填）

状态：✅ 已跑通

### `stellaris_ship_designs`

设计：`scope: "own"` 本国 / `"all"` 全局（含事件船与兽群）；`design_id` 看详情，`components: true` 连部件一起列

参数：`design_id` number、`scope` string（own / all）、`ship_size` string、`components` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_ship_follow`

跟随另一支舰队；`attack: true` = 跟随并攻击。投完舰队状态立刻变「正在跟随 X」

参数：`fleet_id` number（必填）、`target_fleet_id` number（必填）、`attack` boolean、`queue` boolean

状态：✅ 已跑通

### `stellaris_ship_mia`

让舰队回母港：`return_home` / `emergency_ftl`。**两种都会让舰队失联若干天**（实测 35 / 17 天），不需要在战斗中

参数：`fleet_ids` array<number>（必填）、`type` string（return_home / emergency_ftl）

状态：✅ 已跑通

### `stellaris_ship_orders`

看某支舰队在做什么：当前指令、目标、队列条数，外加一行「隐形 是 / 否」

参数：`fleet_id` number（必填）

状态：✅ 已跑通

### `stellaris_ship_parts`

部件目录：每套的本地化名、`set_key`、变体（键 / 尺寸 / 是否解锁）；`key` 看单套，`size` / `filter` 可筛

参数：`key` string、`unlocked_only` boolean、`size` string、`filter` string、`raw` boolean

状态：✅ 已跑通

### `stellaris_ship_stance`

改遇敌姿态（passive / aggressive / evasive）。**档位分舰种，设错档返回 success 却不生效**——两次调用比 `previous_stance` 才判得准

参数：`fleet_id` number（必填）、`stance` string（passive / aggressive / evasive）（必填）

状态：✅ 已跑通（部分参数未单独验）

### `stellaris_update_ship_design`

改已有设计的部件（段不变、不能改名）；游戏存成新 id 替换旧的，编制自动指过去；已造的船要另走改装

参数：`design_id` number（必填）、`slots` array<object>、`core_components` array<string>、`raw` boolean

状态：✅ 已跑通

### `stellaris_upgrade_fleet`

在指定船坞投改装单（自己取船坞队列）

参数：`fleet_id` number（必填）、`starbase_id` number、`target_design_id` number

状态：✅ 已跑通


## 殖民地 · 物种 · 陆军

### `stellaris_apply_species_template`

应用模板 → **改造特殊项目**（按殖民地，不给就是全部）；基底必须是基底物种、且该种在目标殖民地有人口

参数：`template_species_id` number（必填）、`colony_ids` array<number>、`confirm` boolean

状态：✅ 已跑通

### `stellaris_army_deploy`

读或切「部署到轨道」：开着时**新招的部队直接进运输舰队**；只给 `planet_id` 是读

参数：`planet_id` number、`deploy_in_orbit` boolean

状态：✅ 已跑通

### `stellaris_ascend_colony`

行星飞升升一级：花凝聚力，门槛是**空着的飞升天赋槽**；每级 +25% 特化效果

参数：`planet_id` number、`colony_id` number

状态：✅ 已跑通

### `stellaris_available_district_zones`

每个区划槽位能换成的特化（zone），`include_blocked` 连原因一起给

参数：`planet_id` number、`district_type` string、`include_blocked` boolean

状态：✅ 已跑通

### `stellaris_build_building`

建筑入队；`zone_id` 是槽位 id，不是建筑槽序号。**方舟传 `colony_id`**

参数：`planet_id` number、`colony_id` number、`building_key` string（必填）、`zone_id` number

状态：✅ 已跑通

### `stellaris_build_district`

区划 +1（座数即等级），合金 + 工期

参数：`planet_id` number（必填）、`district_key` string（必填）

状态：✅ 已跑通

### `stellaris_buildable_buildings`

按 zone 槽位列可建建筑；给 `building_key` 则逐 zone 给引擎的拒绝理由。**方舟传 `colony_id`**

参数：`planet_id` number、`colony_id` number、`building_key` string、`zone_id` number

状态：✅ 已跑通

### `stellaris_buildable_districts`

现在能造的区划（含已建座数、花费、工期）；给 `district_key` 只问那一种。**方舟传 `colony_id`**

参数：`planet_id` number、`colony_id` number、`district_key` string

状态：✅ 已跑通

### `stellaris_cancel_construction`

把一个队列项撤掉，**全额退款、即时**

参数：`item_id` number（必填）

状态：✅ 已跑通

### `stellaris_cancel_terraforming`

取消改造项目（不退还已花的天数）

参数：`planet_id` number

状态：✅ 已跑通

### `stellaris_clear_blocker`

清一个地块障碍：入建造队列（180–270 天；若开着 `instant_build` 则几秒完成），清完换成 `swap_type` 或消失。**缺什么只有它会回**

参数：`planet_id` number（必填）、`deposit_id` number（必填）、`deposit_key` string

状态：✅ 已跑通

### `stellaris_colonies`

殖民地清单（行星与方舟通用）：colony_id、类型、名字、人口、稳定、区划 / 岗位 / 人口组数、尺寸，以及**净额前三项**；`detail: true` 再打区划→槽→建筑。**方舟可能同名，用 colony_id 认**

参数：`detail` boolean

状态：✅ 已跑通

### `stellaris_colonize`

殖民一颗行星；`fleet_id` 可省——工具自己挑空闲殖民船并报出选了哪艘；拒绝走引擎原文

参数：`planet_id` number、`fleet_id` number、`queue` boolean

状态：✅ 已跑通

### `stellaris_create_species_template`

建模板（整张特质表）：先按预算校验；**记录基底自动取种子的根**；`no_base` 建无基底独立物种

参数：`base_species_id` number（必填）、`name` string、`traits` array<string>、`allow_variant_source` boolean、`no_base` boolean、`confirm` boolean

状态：✅ 已跑通

### `stellaris_delete_species_template`

删模板（不可逆；有活人口就删不掉）

参数：`species_id` number（必填）、`confirm` boolean

状态：✅ 已跑通

### `stellaris_demolish_district`

**真拆一座区划**：即时、不进队列、不可撤销、无退款。读回滞后一 tick

参数：`planet_id` number、`colony_id` number、`district_key` string（必填）

状态：✅ 已跑通

### `stellaris_disband_army`

解散一支部队，**不可逆**

参数：`planet_id` number（必填）、`army_id` number（必填）

状态：✅ 已跑通

### `stellaris_embark_armies`

把地面上所有突击部队装进**新运输舰队**；守备军拒绝上船

参数：`planet_id` number

状态：✅ 已跑通

### `stellaris_enact_decision`

执行行星决议；**多数不可撤销**，部分有相反决议（人口控制就是）；被拒时回引擎原因

参数：`planet_id` number、`colony_id` number、`decision_key` string（必填）

状态：✅ 已跑通

### `stellaris_fleet_armies`

运输舰队里装的是哪几支陆军（与 `fleet_composition` 互为陆军 / 海军）

参数：`fleet_id` number

状态：✅ 已跑通

### `stellaris_job_capacity`

岗位的**编制**：每条给 `job_key` / 名称 / 阶层 / 编制数。带小数的行不是岗位（会计入 `skipped_non_job`）

参数：`planet_id` number

状态：✅ 已跑通

### `stellaris_job_limit`

给单个岗位上限：超出的立刻转去别的岗位，`limit < 0` 复位到编制。**设成当前值会被引擎拒**

参数：`planet_id` number（必填）、`job_key` string（必填）、`limit` number（必填）

状态：✅ 已跑通

### `stellaris_land_armies`

把运输舰队的部队放到行星上；成功后运输舰队消失

参数：`fleet_id` number（必填）、`planet_id` number（必填）、`queue` boolean

状态：✅ 已跑通

### `stellaris_list_colony_designations`

列出可用的殖民地类型；`for_planet: true` 只列同一大类

参数：`planet_id` number、`filter` string、`effects` boolean、`for_planet` boolean

状态：✅ 已跑通

### `stellaris_modify_species_template`

改模板的名字 / 特质；**引擎会换成新 species id**，投完要重读

参数：`template_species_id` number（必填）、`name` string、`traits` array<string>、`confirm` boolean

状态：✅ 已跑通

### `stellaris_planet_armies`

读一颗殖民地的陆军：总览、逐支部队、可招募目录。**方舟殖民地会回「没有殖民地」**——陆军站在行星上

参数：`planet_id` number、`colony_id` number

状态：✅ 已跑通

### `stellaris_planet_jobs`

岗位的**在岗**（按阶层分组）：在岗 / 编制、等效、加成、上限、是否已优先。行星传 `planet_id`，方舟传 `colony_id`

参数：`planet_id` number、`colony_id` number

状态：✅ 已跑通

### `stellaris_planet_structures`

一块殖民地的完整快照：状态（稳定 / 犯罪 / 舒适 / 住房）→ 逐物种人口 → 产出 / 维护 / 净额 → 修正 → 风暴 → 特征 → 区划 / 槽 / 建筑（带 `zone_id`）→ 建造队列（带 `item_id`）→ 障碍（带 `deposit_id`）。**升级与撤队列要的两个 id 只有它给**。行星传 `planet_id`，方舟传 `colony_id`

参数：`planet_id` number

状态：✅ 已跑通

### `stellaris_planetary_decisions`

一颗殖民地能执行的行星决议：`can_enact` 是引擎逐条判定（只问不做、免费）；`all: true` 连不可执行的也列

参数：`planet_id` number、`colony_id` number、`all` boolean

状态：✅ 已跑通

### `stellaris_prioritize_job`

**切换**某个岗位的优先级（同键再调一次 = 取消）；结果看返回的 `is_prioritized`

参数：`planet_id` number（必填）、`job_key` string（必填）

状态：✅ 已跑通

### `stellaris_recruit_army`

招一支部队：`army_key` 从 `recruitable_armies` 拿。**尽量显式传 `species_id`**（桥的物种回退在 colony 0 上读不到）

参数：`planet_id` number、`colony_id` number、`army_key` string（必填）、`species_id` number

状态：✅ 已跑通

### `stellaris_set_colony_designation`

切换殖民地类型；**引擎不校验合法性**，过月会退回自动——过一个月再看一眼

参数：`planet_id` number（必填）、`key` string（必填）

状态：✅ 已跑通

### `stellaris_set_district_zone`

换区划特化；要等工期，**建完再造建筑就没事**

参数：`planet_id` number（必填）、`district_id` number（必填）、`slot` number（必填）、`zone_key` string（必填）

状态：✅ 已跑通

### `stellaris_set_species_rights`

改某物种的一类权利：先按目录校验类别与取值、讲清这一类的冷却，`confirm: true` 才投

参数：`species_id` number（必填）、`category` string（必填）、`right_value` string（必填）、`confirm` boolean

状态：✅ 已跑通

### `stellaris_species`

物种看板：建国 / 未开智 / 其他物种（带基底的按基底归组），每行带特质数、剩余点数与可选位；`species_id` 走单卡，`mode: "galaxy"` 给全银河

参数：`mode` string、`species_id` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_species_detail`

单个物种：身份与状态、基底、母星、提升者、点数与可选位、每条特质花费、**九类权利各自冷却**

参数：`species_id` number（必填）

状态：✅ 已跑通

### `stellaris_species_modification`

改模板前的预算：剩余点数 / 可选位、当前特质花费、可选特质按 cost 分桶（**目录是全库，没按能不能装筛**）

参数：`species_id` number（必填）、`raw` boolean

状态：✅ 已跑通

### `stellaris_start_terraforming`

开改造（`planet_id` + `target_class`）；缺科技回引擎原文。**付不起会静默失败**，判成败要读 `is_terraforming`

参数：`planet_id` number（必填）、`target_class` string（必填）、`link_index` number

状态：✅ 已跑通

### `stellaris_terraforming`

地貌改造选项：当前星球、进行中的项目（目标 / 剩余 / 百分比）、每个目标的 `link_index` 与 `can_terraform`

参数：`planet_id` number

状态：✅ 已跑通

### `stellaris_upgrade_building`

升级已建建筑：`building_id` + 下一级 key

参数：`planet_id` number、`colony_id` number、`building_id` number（必填）、`upgrade_to_key` string（必填）

状态：✅ 已跑通（部分参数未单独验）

### `stellaris_uplift_species`

**提升未开智物种**：投出 `kind=uplift` 特殊项目并自动启动；默认只印计划，`confirm` 才投

参数：`species_id` number（必填）、`confirm` boolean

状态：✅ 已跑通


## 恒星基地

### `stellaris_buildable_starbase_buildings`

同上，换成**建筑槽**（星港 1、星垒 2…）；能带出「空间站已建造有此建筑」这类引擎原因

参数：`starbase_id` number（必填）、`slot` number、`building_key` string、`include_blocked` boolean

状态：✅ 已跑通

### `stellaris_buildable_starbase_modules`

每个模块槽现在能建 / 能换哪些模块（合金 / 工期）；给 `module_key` 只问一个。**方舟与驿港同样适用**（驿港只有模块槽，2/0 · 4/0 · 6/0）

参数：`starbase_id` number（必填）、`slot` number、`module_key` string、`include_blocked` boolean

状态：✅ 已跑通

### `stellaris_dismantle_starbase`

**只能拆哨站**：已升级的会被拒，还要求该星系没有你的殖民行星。**方舟直接拦**，驿港交给引擎判

参数：`starbase_id` number（必填）

状态：✅ 已跑通

### `stellaris_downgrade_starbase`

**一律降到哨站**（不是降一级）：免费，但装着的模块 / 建筑全部移除且不退款。**方舟与驿港直接拦**

参数：`starbase_id` number（必填）

状态：✅ 已跑通

### `stellaris_queue_starbase_building`

建筑槽正规入队：同样先查再下单，**空槽优先**，被拒时列出每个槽的原因

参数：`starbase_id` number（必填）、`building_key` string（必填）、`slot` number

状态：✅ 已跑通

### `stellaris_queue_starbase_module`

模块正规入队：先查可建列表再挑空槽下单（合金 + 工期，与手点同一条队列）。**驿港的模块进它自己的队列**（已实测），方舟的进殖民地队列

参数：`starbase_id` number（必填）、`module_key` string（必填）、`slot` number

状态：✅ 已跑通

### `stellaris_starbase_details`

单座基地的等级、模块槽、建筑槽（键名 + 本地化名 + 槽号）与各等级槽位上限。**入队前先用它**：满槽会静默失败

参数：`starbase_id` number（必填）

状态：✅ 已跑通

### `stellaris_starbase_queue`

基地的建造队列：每项的键、类别、进度、剩余天数、`item_id`（可喂给 `cancel_construction` 退款）。**方舟多印一段「隐藏队列」**：界面那条是殖民地队列，另一条引擎认、界面不显示

参数：`starbase_id` number（必填）

状态：✅ 已跑通

### `stellaris_starbase_upgrade`

升一级：`dry_run: true` 只报合金 / 工期与拒绝原因（缺合金、缺科技），不开才下单。**不可逆**，合金下单即扣

参数：`starbase_id` number（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_starbases`

本方基地分三组：**方舟**（殖民地载体的舰）、**驿港**（国家驿站网络）、**常规**（星系归属）。每行 id / 等级 / 站体名 / 槽位填充 / 星系；抬头印 `空间站容量 used/capacity`

参数：无

状态：✅ 已跑通


## 外交

### `stellaris_agreements`

附庸协议明细：宗主 → 附庸、预设、起始日、固有忠诚、补贴与每一条离散条款（键与值都翻成中文）。**前提是 hook 抓到过协议**（`install_agreement_capture` + 存一次档）；`hidden: true` 连界面上不显示的条款

参数：`country_id` number、`hidden` boolean

状态：✅ 已跑通

### `stellaris_assign_envoy`

把特使派到某条第一次接触上：不给 `leader_id` 自动挑空闲特使；**引擎不校验职业**，所以工具先拦非特使

参数：`contact_id` number、`leader_id` number、`dry_run` boolean、`reset_cooldown` boolean

状态：✅ 已跑通

### `stellaris_claim_system`

宣称 / 撤回一个星系：能不能宣称由引擎判（影响力代价），拒绝文案原样回；`remove: true` 撤回。宣称是「征服」战争目标的理由

参数：`system_id` number（必填）、`remove` boolean、`count` number

状态：✅ 已跑通

### `stellaris_community`

星海共同体现状：成员 / 议会 / 领袖、台上议案（支持 / 反对 / 弃权）、排队、已通过、剩余天数。内存优先（抓到过对象就 0.15 秒），没抓到才退回归档

参数：`pause` boolean

状态：✅ 已跑通

### `stellaris_contacts`

外交看板：每个国家一行——名称、类型、条约、本方的特使与任务；顶部统计特使去向与空闲。`all: true` 连城邦与掠夺者

参数：`all` boolean、`envoy_only` boolean

状态：✅ 已跑通

### `stellaris_declare_war`

宣战：`goal_key` 必填；`list_goals: true` 只列该目标国**可选**的战争目标。投出去的是**提案**，对方答应后才出现战争

参数：`country_id` number（必填）、`goal_key` string、`list_goals` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_diplo_inbox`

收件箱：对方递来的提案（求和、附庸协议含条款与补贴、各类条约），带 `accept: true/false` 应答一条。**工具先核对收件方是不是本方**——引擎自己不查

参数：`accept` boolean、`id` number、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_diplo_vote`

投本方那一票：求和 / 宣战 / 踢人 / 联邦法律与类型表决都认得，其余标 `unknown` 也照列；`agree` 的含义与联邦表决一致。**AI 关掉就没人再投**，票挂到截止日自动拒绝

参数：`agree` boolean、`id` number、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_diplomatic_actions`

能对某个国家做什么：逐个问引擎，按「可发起 / 被拒（引擎原文）」分组；裸 id 会补标记。宣战要另配目标

参数：`country_id` number（必填）、`raw` boolean

状态：✅ 已跑通

### `stellaris_federation_laws`

一个联邦的法律总表：当前值 / 能换成什么 / 为什么不能；`category` 可收窄，`skip_isvalid: true` 只读不算。**三层判据缺一层就会说谎**（引擎 IsValid 不看资格）

参数：`fed_id` number、`category` string、`skip_isvalid` boolean

状态：✅ 已跑通

### `stellaris_federations`

全银河联邦一览：id、名字、成员数、类型、等级、经验、团结度，以及**当前法律**（默认只列本方所在的，`all_laws: true` 连别国）。读不到每月经验与成员名单

参数：`all_laws` boolean

状态：✅ 已跑通

### `stellaris_first_contacts`

第一次接触：对方、地点、阶段、线索、剩余天数、特使；给 `country_id` 出单条详情（阶段难度、记录 id / 槽）。`all: true` 连别国的也列

参数：`all` boolean、`country_id` number

状态：✅ 已跑通

### `stellaris_form_federation`

组建新联邦 / 指定类型（`list_types: true` 只列引擎接受的）。**邀请加入要过联邦表决**，组建则没有表决

参数：`country_id` number（必填）、`federation_type` string、`list_types` boolean

状态：✅ 已跑通

### `stellaris_offer_peace`

递交求和：`status` 0 维持现状 / 2 达成目标 / **1 我方投降（当场结战）**。记录落在**对方**收件箱，同一对目标只能挂一条；**在联邦里要加 `force: true`**

参数：`war_id` number（必填）、`country_id` number（必填）、`status` number、`dry_run` boolean、`force` boolean

状态：✅ 已跑通

### `stellaris_propose_diplomatic_action`

发起一项外交动作：**自带两道拦截**——`*_diplomacy_view` 是界面占位（投了等于假投送）、`requires_alliance_vote` 的要先过联邦表决。**踢人与邀请加入是例外**，直投后引擎自己起表决

参数：`country_id` number（必填）、`action` string（必填）、`terms` object、`subsidies` object、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_propose_federation_law`

发起联邦法律表决；`list: true` 列全部 16 类 / 73 档并标出「某种联邦专用」。换法律多数通过；**换类型只能走界面**（桥里那条已禁用，手搓会崩）

参数：`law_key` string、`list` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_propose_resolution`

提交议案：先按资格判据拦一次（帝皇 / 监管人 / 「废除 X」要看 X 在不在生效），再问引擎

参数：`type` string（必填）、`target_id` number、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_queued_resolution`

排队中的议案：`support` / `oppose` / `abstain` / `veto`（理事会，移出队列并让对方进提交冷却）/ `freeze`（冻结 1440 天）/ `emergency`（绕开队列与休会）。**没有台上那种 120 天冷却**

参数：`resolution_id` number（必填）、`action` string（support / oppose / abstain / veto / freeze / emergency）（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_resolution_vote`

在台上的议案投票 / 提前结束：`support` / `oppose` / `abstain` / `end_session`；`resolution_id` 省略时取台上那条。**取值 0 赞成、1 反对、2 弃权**——与联邦表决的 1 = 同意正好相反

参数：`action` string（support / oppose / abstain / end_session）（必填）、`resolution_id` number、`vote` number、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_resolutions`

议案清单（按类别）：每条都问引擎能不能提交，理由原样带回；默认只列**现在可提交**的，`all: true` 全列并带理由；`target_id` 给带目标的那批

参数：`all` boolean、`country_id` number、`target_id` number

状态：✅ 已跑通

### `stellaris_set_defender_war_goal`

守方设战争目标：默认 `wg_humiliation`，**别抄攻方那份**；成功要过一 tick 看 `stellaris_wars`

参数：`war_id` number（必填）、`goal_key` string、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_trade_deal`

递交贸易协定：`give` / `take` 一次性，`monthly_*` 月供，`years` 年限；另有 `sensor_charts`（通信同步）、`sensor_link`（传感链接）、`specimens`（标本，即时交割）、`fleets`（借出舰队，有期限）。**空交易被引擎拒**，重复递交会叠成多条待答复提案

参数：`country_id` number（必填）、`give` object、`monthly_give` object、`take` object、`monthly_take` object、`years` number、`sensor_charts` boolean、`sensor_link` boolean、`fleets` array<number>、`specimens` array<string>、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_wars`

战争表：全银河的仗，id、双方、攻守目标、双方疲惫、是否结束；本方参战的会标出来，`mine: true` 只看本方。**疲惫原始值，别当百分数读**

参数：`mine` boolean、`raw` boolean

状态：✅ 已跑通


## 资讯 · 考古 · 遗珍 · 标本

### `stellaris_activate_artifact_action`

启动一条稀有文物行动：拒绝原文带回；`dry_run` 只判定不投

参数：`action_key` string（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_activate_astral_action`

启动一条星界行动：先验后投，拒绝原文回传；**消耗星界丝缕**（按次数递增），部分限次、部分永久

参数：`action_key` string（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_activate_relic`

激活一件遗珍：没持有的键直接拒、不投递；triumph 有期限，代价多为凝聚力，投完进冷却

参数：`relic_key` string（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_advance_first_contact`

按界面的顺序推进一次首触阶段锁（置位 → 投阶段事件 → 自动归零）；`settle` 只做收尾

参数：`country_id` number（必填）、`ope` number、`option` number、`settle` boolean

状态：✅ 已跑通

### `stellaris_alerts`

顶栏警报横幅：id、标题、tooltip

参数：无

状态：✅ 已跑通

### `stellaris_anomalies`

情报日志里**尚未研究**的异常：名字、所在天体、难度、`planet_id` / `system_id`，并标出哪条正被调查

参数：`raw` boolean

状态：✅ 已跑通

### `stellaris_answer_event`

应答任意打开中的事件（不需要窗口）：`ope` 来自 `open_events`，脚本效果真的执行；`dry_run` 只校验

参数：`ope` number（必填）、`option` number（必填）、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_archaeological_sites`

考古遗址全库：id、类型、名称、chapter、线索、难度、剩余、locked、所在天体与**星系归属**、是否可见、正在挖的船；给 `fleet_id` 时逐条问引擎「能不能挖」

参数：`fleet_id` number、`site_id` number、`owner` string（any / unowned / own / foreign）、`include_hidden` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_artifact_actions`

稀有文物行动清单：逐条问引擎；代价多为 `minor_artifacts`；`*_locked` 占位默认跳过

参数：`all` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_astral_actions`

星界行动清单：✓ / ✗ 与拒绝原文（冷却 / 次数用完 / 缺方舟舰 / 丝缕不够）都是引擎给的；另给已解锁、还剩几次、下次可用日（剩余 -1 = 不限次）；未解锁默认藏起来

参数：`all` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_astral_rifts`

星界裂隙全库：id、类型、status、线索、所属国、探索船；给 `fleet_id` 时逐条问引擎能不能进；`rift_id` 看单条

参数：`fleet_id` number、`rift_id` number、`raw` boolean

状态：✅ 已跑通

### `stellaris_event_options`

读一条打开事件的选项：下标、界面文本、`is_valid`、效果提示，并给每条选项一行**判定**（可选 / 不可选（失败子句）/ 判不了）。`filter: false` 只看引擎字段

参数：`ope` number、`id` number、`filter` boolean、`selectable_only` boolean

状态：✅ 已跑通

### `stellaris_event_text`

事件的剧情文本：按 `event_key` 从游戏文件里取标题、描述与每个选项的名字（**不用等它弹出来**）

参数：`event_key` string（必填）、`raw` boolean

状态：✅ 已跑通

### `stellaris_events`

待处理弹窗：id、标题、描述、每个选项及其效果与可用性；抬头一句「为什么停着」（事件数、暂停原因、待处理数、速度）

参数：`raw` boolean

状态：✅ 已跑通

### `stellaris_excavate_site`

投递发掘：`fleet_id` + `site_id`；工具先拦非科研船（引擎不看舰种，收下也不会开挖）

参数：`fleet_id` number（必填）、`site_id` number（必填）、`queue` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_explore_astral_rift`

投裂隙探索：先拦非科研船；**投递后科学官锁死，撤不回来**

参数：`fleet_id` number（必填）、`rift_id` number（必填）、`queue` boolean、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_move_specimen`

发送到库存 / 展览格：展览中可直接入库（门槛是「仓库→展柜」触发的 720 天返回冷却）；移动后拿**新 id**，旧位置留空

参数：`exhibit_id` number（必填）、`to` string（storage / gallery）（必填）

状态：✅ 已跑通

### `stellaris_notifications`

顶栏通知徽标（等着确认的那些）

参数：无

状态：✅ 已跑通

### `stellaris_open_alert`

打开某个警报（左键点横幅；**不消除警报**）；非法 id 回范围提示

参数：`alert_id` number

状态：✅ 已跑通

### `stellaris_open_events`

所有打开中的玩家事件，含**没有经典窗口**的首触 / 裂隙阶段事件；带 `event_key`、`event_id`、`ope`

参数：无

状态：✅ 已跑通

### `stellaris_open_notification`

打开某条通知（左键点一下徽标）

参数：`index` number

状态：✅ 已跑通

### `stellaris_relics`

帝国持有的遗珍：key、名字、`can_activate`（引擎判定 + 拒绝原文）、库的规模；`effects: true` 附被动 / 主动效果，`verbose: true` 附说明

参数：`verbose` boolean、`effects` boolean、`raw` boolean

状态：✅ 已跑通

### `stellaris_research_anomaly`

让科研船研究**指定天体**上的异常（`fleet_id` + `planet_id`）；同星系多条也选得准，不用先环绕

参数：`fleet_id` number（必填）、`planet_id` number（必填）、`queue` boolean

状态：✅ 已跑通

### `stellaris_resolve_event`

选一个选项并关闭弹窗

参数：`window_id` number、`option_index` number

状态：✅ 已跑通

### `stellaris_set_situation_approach`

改局势取向：`approach_key` 从详情里的取向清单拿；引擎校验，可改回

参数：`situation_id` number、`approach_key` string

状态：✅ 已跑通

### `stellaris_situation_details`

单条局势全貌：实时数字 + 阶段效果、每个取向及其效果、完成 / 失败效果（条件判不了的标 `?` 并列出条件）。`all_stages: true` 列全部阶段

参数：`situation_id` number（必填）、`raw` boolean、`all_stages` boolean、`check` boolean

状态：✅ 已跑通

### `stellaris_situations`

局势表：阶段号、阶段区间（界面上的「x 到 y」）、阶段内进度、每月变化、当前取向

参数：`raw` boolean、`player_only` boolean

状态：✅ 已跑通

### `stellaris_special_project_collect`

派舰队研究**需要舰船**的特殊项目（`project_id` 或 `system_id`）；目标点只收星系

参数：`fleet_id` number（必填）、`project_id` number、`system_id` number、`queue` boolean

状态：✅ 已跑通

### `stellaris_special_project_research`

起 / 停**直接研究**类的特殊项目；已在进行的会被引擎拒

参数：`project_id` number（必填）、`cancel` boolean

状态：✅ 已跑通

### `stellaris_special_projects`

特殊项目：`kind`（项目 / 物种改造 / 提升 / 残骸）、剩余天数、要不要船、位置、status（0 未开始 / 1 进行 / 2 已完成）

参数：`raw` boolean

状态：✅ 已跑通

### `stellaris_specimen_exhibit`

激活 / 取消展览：槽没开就先开，开了再切换；**即时生效**

参数：`exhibit_id` number（必填）

状态：✅ 已跑通

### `stellaris_specimens`

博物天枢里的标本：储藏室与三类展览格，每件给标本键 + **发现者**。**持有 = 被哪个数组引用**，发现者与持有者是两个属性

参数：`raw` boolean

状态：✅ 已跑通


## 核心：启动、面板与逃生口

### `stellaris_event_mark`

事件去重：同一个 key 30 分钟内只写一封；`dry_run: true` 只问不记

参数：`key` string（必填）、`title` string、`dry_run` boolean

状态：✅ 已跑通

### `stellaris_inbox`

读面板输入框里最新的那句话（`NEW` / `seen`）；`ack: true` 标已读

参数：`ack` boolean

状态：✅ 已跑通

### `stellaris_launch`

用启动器开游戏（`--continue` 接着上次存档）

参数：`cont` boolean

状态：✅ 已跑通

### `stellaris_letter_write`

往流水账写一条：首行 + 若干正文行，面板显示最近 30 条；`replace: true` 重写整份日志

参数：`text` string（必填）、`replace` boolean

状态：✅ 已跑通

### `stellaris_localize`

用游戏本地化表把 key 翻成当前语言（`job_replicator` → 复制者）

参数：`keys` array<string>、`key` string

状态：✅ 已跑通

### `stellaris_panel`

写 `show.txt` 开合窗口（等同 `Ctrl+Shift+L`）；`log: N` 读面板自己的日志

参数：`show` boolean、`log` number

状态：✅ 已跑通

### `stellaris_state`

**逃生口（慎用）**：走命名管道问局内状态，`method` 可以是**桥的任意方法**（含 `run_console`、`read_memory`、`save_game`、`exit_to_main_menu`），没有白名单；`params` 原样转发。日常别用，只在缺工具时临时开门

参数：`method` string、`params` object

状态：✅ 已跑通

### `stellaris_status`

跑 `stl status`：游戏版本、播放集、装了哪些插件

参数：无

状态：✅ 已跑通


---

共 187 个工具。对应的桥方法与内存细节不在本文件里，见 `stellaris-bridge-patch/README.md`。
