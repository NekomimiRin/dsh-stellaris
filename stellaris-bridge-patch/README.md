# stellaris-mcp 本地改动（桥）

在官方 0.7.0 的 DLL 上扩到 **299 个 IPC 方法**：母港、岗位、殖民地、方舟、恒星基地、外交、物种、巨构……
另外加了一批只读诊断（探针 / 快照 / 扫描）。官方 DLL 备份在 `plugins/stellaris-mcp/stellaris_bridge.dll.orig-0.7.0`，随时可回滚。

> **术语**（按插件 README 的「概念与术语」）：**恒星基地**（starbase）= 五级总称
> （哨站 / 星港 / 星垒 / 星堡 / 擎天堡）；**空间站** = 星港 + 星垒 + 星堡 + 擎天堡，哨站不算。

这份文档是**逆向笔记**：引擎内部怎么走、字段在哪、试过什么不成立。玩的时候要看的东西在
`dsh-stellaris\README.md`（工具口径），两边不重复。

文中提到的 `probes\`、`history\` 与 `tmp-*` 是开发期的一次性脚本，**没有随仓库发布**——它们记录的是过程，
不是用起这套东西的前提；方法层面的逐条记录也没有随仓库发布，结论都写在各节正文里。

**怎么找**：1–5 节是桥本体（装什么、hook、关键偏移、试过不成立的做法、静默崩溃怎么认）；
之后按主体翻（岗位 / 殖民地 / 方舟 / 舰船舰队 / 恒星基地 / 外交 / 物种 / 科研）；
「命令怎么造」看标题里带动作名的那几节；没解决的散在各节末尾与第 4、5 节。

## 目录（按文档顺序）

1. 现在桥里有什么（以 `source\` 为准）
2. 四处 hook
3. 关键偏移（实测，不是推的）
4. 试过但不成立的做法（别再试）
5. 静默崩溃：怎么认、怎么防
6. 找 `CBuildableStarbaseModule` 的 vtable：进展与遗留
7. 引擎容器的正确写法
8. 工具链：python / node 从哪来（PATH 上那个是商店占位）
9. 重建（没有 cmake / git 时用 cl 直编）
10. 脚本怎么分
11. 版本事实
12. 追加：换设计的方法与两个事实
13. 一条教训
14. 热替换 / 编译一条龙
15. 岗位编制与在岗：读法
16. 岗位数据定位：结论与过程
17. 手动切换殖民地设计：已打通
18. 设计对象池会搬家
19. 内存建筑列表为什么会漏：新 zone 未注册
20. 目录结构
21. 逆向工具
22. 工具整理：两组重复功能合并
23. 科研：已研究科技的内存直读（`get_researched_techs`）
24. `get_situation_log` 的 `stage` 与阶段区间
25. `probe_first_contacts` / `get_first_contacts`
26. `probe_leader` / `assign_envoy` / `probe_first_contact_view`
27. 事件选项：非 GUI 推进（`select_open_event_option`）
28. 考古遗址的归属与「能不能挖」探针
29. `get_civilian_fleets` 的领袖映射
30. 星界裂隙的探索与筛选
31. 上游对照（Yidhar/stellaris-mcp）
32. 删掉整堆快照与坏掉的整数扫描
33. 已知未修：`recruit_planet_army` 的物种回退
34. 运输舰队里的陆军构成：已修
35. 飞升天赋：`activate_ascension_perk`
36. 法令目录：改成枚举
37. 守方战争目标：`set_defender_war_goal`
38. 程序化求和提案「自己发」的那条路
39. 踢出联邦成员的提议
40. 手工造 `CVoteForFederationLawAction`，发起联邦法律表决
41. 战争与联邦：逆向要点
42. `probe_community_membership`
43. 星海共同体：逆向要点
44. `get_contacts` 加附庸三字段
45. 附庸协议：hook 抓取与字段
46. 护栏：协议条款写入
47. 附庸协议：**带条款发送**打通
48. 收件箱（incoming_actions）里的动作类对照
49. 附庸关系：真的挂上之后
50. 外交章逆向索引
51. 特使（envoy）：对象层
52. 贸易协定：`propose_trade_deal`
53. 收口：基座 buildable 的 vtable 是这么找到的
54. 银河市场：读写
55. 贸易协定的「AI 接受度」：两处否证
56. 事件选项：IsValid 只判 allow，trigger 由插件三值求值
57. 考古遗址：对象布局与探针
58. 星界裂隙：对象布局与探针
59. 遗珍：持有数组的真实偏移
60. 星界行动：读 + 启动
61. 文物行动（Artifact Actions / 稀有文物）：读 + 启动
62. 标本（大档案馆 / 博物天枢）：国家持有列表从内存读
63. `find_ships` 的船 id 读成 0：`+8` → `+0x18`
64. `find_systems` 加了 `name` 模式：全银河按名字找星系
65. 「局势取向」：`get_situation_approaches` 不接
66. `get_galaxy_map`：取消 12 跳上限，不给 `jumps` = 全银河
67. 虫洞 / 星门：`get_bypasses` 全银河一览
68. 新增但**未验证完**：`get_all_fleets` / `get_fleet_templates`
69. `get_all_designs`：全局 `CShipDesign` 库
70. `get_all_megastructures` 与巨型结构的两种建造位置
71. `find_arc_furnace_systems`：按天体数挑电弧熔炉星系
72. `find_dyson_swarm_systems`：戴森云候选
73. 物种：帝国名单、未开智、点数、九类冷却
74. 隐形：两条命令 + `CFleet+0x1268` 的位
75. 方舟（Nomads）：一切都在 `CColony` 上
76. 行星特征（deposit）：持有者字段在 `+0x30`，不是 SDK 的 `+0x28`
77. 定时修正（timed_modifier）：容器、条目、键名
78. 宇宙风暴：cluster 的偏移
79. 走跃迁通道：`use_bypass` 的对端解析（2026-11-23 修）
80. 方舟与驿港：槽位、队列、清单
81. 站体设计与防御平台
82. 方舟当舰队读（`relation: "ark"`）与轰炸姿态
83. 殖民地：内存布局总表
84. 方舟殖民地：动作命令与字段
85. 科研：科技表的内存布局与定位过程
86. 参考仓库
87. 阶段弹窗与视图：控件树点不到，但路可以绕开
88. 情报日志三类：异常 / 特殊项目的字段与命令

> 改过标题就重跑 `node scripts/toc-bridge.mjs`；`--check` 只检查不改。

## 现在桥里有什么（以 `source\` 为准）

| 方法 | 作用 | 状态 |
|---|---|---|
| `get_fleet_home_base {fleet_id}` | 读母港：`kind`(1 行星 / 2 星港)、`home_id`、`template_id` | 已验证 |
| `set_fleet_home_base {fleet_id, starbase_id}` | 改母港，投 `CSetHomeBaseCommand` | 已验证（飓风 0→32→…来回切过） |
| `get_starbase_pointers` | 只读：`db::CStarbase` 一览（id / 指针 / station） | 已验证（35 座，与存档一致） |
| `probe_fleet_ships {fleet_id}` | 舰船列表、每艘 `0x400..0x4F8` 的字、`0x478/0x4D8` 指向的对象 | 诊断用 |
| `scan_ship_for_values {ship_id, values[]}` | 在船头 `0x2000` 字节里搜给定 u32，报告偏移 | 诊断用 |

**另外还改了 `get_fleets`（原本就有的方法）的计数**：
`total_ships` 改用 `CFleet::ships`（指针 `+0x320`、个数 `+0x32C`），逐设计 `actual_count`
改用 `CShip+0x988`（当前设计）/ `+0x98C`（待升级目标）归组。旧代码读 `+0x4b0`/`+0x108`，给的是假数。

## 四处 hook

桥里有四处 MinHook，都是「引擎自己走到那儿时把指针截下来」——因为这些东西**没有全局库表、
没有 RTTI、内存扫描也找不到**。三张表 + 一处克隆：

| # | 挂在哪 | RVA | 安装函数 | IPC | 谁在用 |
|---|---|---|---|---|---|
| 1 | `CGalacticCommunity` 的 **serializer** | `0x6249E0` | `InstallCommunitySerializerCapture()` | `install_community_capture` / `read_community_capture` | 插件 `stellaris_community` |
| 2 | `CAgreement` 的 **serializer** | `0x7AA310` | `InstallAgreementSerializerCapture()` | `install_agreement_capture` / `get_agreements` | 插件 `stellaris_agreements` |
| 3 | `CTimedModifier` 的 **serializer** | `0x375FF0` | `InstallTimedModifierCapture()` | `install_timed_modifier_capture` / `get_timed_modifier_capture` / `clear_timed_modifier_capture` | **不用挂**：只为当年定位偏移与对账 |
| 4 | `COfferPeaceAction` 的「**模板 → 新动作**」函数 | `0x9A8E80` | `InstallCloneCapture()` | `want_clone`（`probe` 系列） | 求和提案那边找模板用 |

1、2、3 是**序列化器**；4 不是——它是克隆函数（写 vtable `0x23B6B20` 的那处 `lea` 在
`0x9A8EAA`，所属函数 `0x9A8E80..0x9A8EE5`）。

**#3 与另外两个不一样**：共同体的成员、附庸协议的条款**只能**靠 hook 拿（没有库表）；
定时修正则是**日常直接读内存**（`CPlanet+0x280` / `CShip+0x2A0` 的容器，键名在类型对象 `+0x178`），
`install_timed_modifier_capture` 只是当初用来「存一次档、把所有修正连类型指针收下来」找偏移的，
现在**不是必需**——挂上它只多一份 512 条上限的对账样本。

**两条操作上的要点**

1. **`install_*_capture` 之后要存一次档**：存档本身也走 serializer，所以 `save_game`
   就能让 hook 跑一遍，不必手动读档再抓。读档同理也会再跑一次。
2. **重注入 DLL 后指针会丢**：要重挂 hook、再存一次档。`stellaris_agreements` 没抓到协议
   时提示的就是这条。

**为什么非要 hook**（不是懒）

```
CGalacticCommunity  sdk.json 里没有 TPdxRef 库，只能等它自己序列化
CAgreement          同样没有 _pDatabase；对象在池里按 0x180 步长排、空槽 vtable 为 0，
                    枚举只能靠这个 hook，别去扫内存
CTimedModifier      日常读法不靠它；hook 只为一次性定位键名偏移
```

细节在「星海共同体：逆向要点」「附庸协议：hook 抓取与字段」「定时修正」三节。


## 关键偏移（实测，不是推的）

| 找什么 | 位置 |
|---|---|
| 母港（ORBITABLE 联合体：`kind<<32 | id`） | 舰队模板 `+0x70`（**SDK 标的 0x68 是错的**，那是所有模板共享的常量） |
| 模板的设计条目数组 | `+0x28`（指针）、`+0x34`（个数）、每项 `0x560` 字节 |
| 条目的编制数量（quota） | 条目 `+0x558` |
| 舰队舰船列表 | `CFleet+0x320`（元素 `u32` 船 id），个数 `+0x32C` |
| 船的设计 id / 待升级目标 | `CShip+0x988` / `+0x98C`（镜像在 `+0x1BF8`/`+0x1BFC`） |
| `db::CStarbase` | 数组 `+0x18`、容量 `+0x20`、每项 16 字节、对象 `+8`、id `+0x8` |

## 试过但不成立的做法（别再试）

- **`add_design_to_fleet_template`**（往编制里加新舰种）：三种编码（模板指针 / 模板 id × 设计 id 两种打包）
  `IsValid` 全拒且**不给原因**。舰种要进编制，只能靠**造一艘再合并**，让游戏自己写模板
- **`merge_fleets_command`**：SDK 里有（字段 `country` / `fleets` ref_array / `mia_reinforce` / `cancel_orders`），
  但 `fleets` 是 `ref_array<TPdxRef<CFleet>>`，命令构造器只会写标量。手工用 `EngineAlloc` 拼 vector 头
  `{begin,end,cap}` 后调用，**游戏当场崩溃**（命令析构/IsValid 读越界二者之一）。
  结论：**没弄清数组的分配器与真实元素布局之前，不要碰引擎管理的容器**。（后来改用引擎自己的
  `CMergeFleetsCommand` 打通了合并，见 `merge_fleets`；这条留着当反例。）
- **`PersistentNameText()`** 直接调用引擎函数、无 SEH 保护，在未确认偏移上调用会崩（昨晚崩过一次）

## 静默崩溃：怎么认、怎么防

那两次是「进程直接没了」：`crashes\` 目录**没有新 dump**（最后一份还停在 11-18），只有 `logs\time.log`
停在与操作同一分钟。Stellaris 并非每次失败都写 dump，所以「没 dump」不等于不是本桥的问题。

**认法（按顺序）**

1. `stellaris_status` 的 `running` 变 `no`、`Get-Process stellaris` 空
2. `logs\time.log` 的最后一条时间 ≈ 最后那次桥调用——这就是第一嫌疑人
3. 看那一次调用**做了什么**：新建/写引擎对象？调 native 函数？枚举全局数据库？

**两条真实教训**

- **重编脚本先卸载再编译**（2026-11-21 01:45 崩过一次）：`rebuild_reload.py` 老顺序是
  「① eject → ② 编译 → ③ 复制 → ④ 注入」。编译失败就 `exit(1)` 退出，**DLL 已被拔掉、没装回去**，
  游戏带着悬空的 hook 又跑了两分钟才崩。**规则：先编译成功再卸载/替换**（脚本已改；失败时提示「桥仍挂在游戏里」）
- **新探针一次做太多**（同日晚，两次）：`probe_federation_types` 在一个调用里
  ① 枚举 `CFederationTypeDatabase`、② 每个条目造一个动作、③ 写 `+0xB0`、④ 跑 IsValid。
  一调就静默崩，加了 vtable 校验（只认 `CFederationType` RVA `0x2388990`）仍崩。
  `probe_war_goals` 同样是「枚举 + 造 + IsValid」却能跑——说明不是这种模式本身，而是**其中某一步的对象不对**。
  规则：**新对象类型先只读**（只列指针与 vtable，不造对象、不写内存），确认枚举出来的东西是对的，再谈造动作

**两次崩溃的根因（2026-11-21 夜间定案，只读探针找出来的）**

- **`CFederationType` 的 key 在 `+0x20`，不是 `+0x28`**。`+0x28` 是全部 7 个条目**共用的同一个指针**
  （读出来只有 `"H"` 一个字）。旧代码按 `+0x28` 取 key，再交给 `SafeReadPdxString` ——
  它按 PDX 字符串结构去解一块根本不是字符串的内存（读长度、再按长度取字节），
  **这一步就把游戏读崩了**。造动作、写 `+0xB0`、IsValid 都不是凶手
- 只读探针 `probe_federation_type_db` 一跑就看清：7 条、vtable RVA 全是 `0x2388990`、
  key 指针在 `+0x20`、`+0x28` 那 7 个值一模一样。改回 `+0x20` 之后，同一个调用路径（逐类型造动作 + IsValid）
  7 次全部安全返回
- **衍生结论**：`SafeReadPdxString` 看着像「安全版」，其实只保证「传进去的指针可读」，
  不保证**那块内存真的是 PDX 字符串**。长度字段读成天文数字时它会照读，结果就是静默崩。
  **规则：在没确认偏移之前，别用 `SafeReadPdxString`/`SafeReadCStr` 这类「按结构解」的读法；
  先只读探针把指针打印出来，确认它指向的东西再解**

**防护清单（写新探针前过一遍）**

- 读一个指针前先想它可能是野的；用 `SafeReadPtr`/`SafeReadU32`（SEH 包着），别裸解引用
- 写引擎对象前先验 vtable（RVA 与 `base_address_` 相减），拿错对象写进去 = 后面某次 native 调用崩
- 一次只验一条：一个调用里造 N 个对象、各自 IsValid，出错时无法定位是哪条
- 调 native 函数（非命令）前，先确认入参对象真在它该在的容器里（见 `SafeSelectByOpenEvent` 的做法）
- **解结构之前先确认偏移**：同一数据库里不同字段的指针可能全是同一个（共享字符串），
  「看起来能读出字」不等于位置对
- 崩过一次的路径，改动后**先在只读模式跑**，再开放写

## 找 `CBuildableStarbaseModule` 的 vtable：进展与遗留

**目标**：走 `CAddBuildableToQueueCommand` 把基地模块投进建造队列（正规路径的脚本版）。

### 已经确认的

- 序列化器 `0x8C6340` 在 .rdata 里**只有一处引用**：`0x233B580`。
  它所在的簇 `0x233B450..0x233BB18` 是**序列化器 vtable 群**，相邻的还有
  `0x8C8080`（StarbaseUpgradeDesign）、`0x8C48F0`（StarbaseBuilding）、`0x8C7240`（StarbaseUpgrade）
  —— 也就是说基座相关类挤在一起，但**这簇是序列化器，不是对象 vtable**
- 对象 vtable 群在 `0x2397DC8..0x2398298`（Building / District / Zone / UpgradeBuilding / ClearDepositBlocker），
  **`CBuildableStarbaseModule` 的 vtable 不在这段里**（扫了 0x2396000-0x239A000，开/关面板无差异）

### 新增的桥工具（可用，别浪费）

| 方法 | 作用 |
|---|---|
| `scan_memory_for_u64_range {lo, hi, start_addr, max_seconds}` | 扫落在区间内的 qword，分页返回（只报"出现位置"，不代表对象边界） |

IPC 等待上限从 2500ms 改成了 **30000ms**（`ipc_server.cpp`）。

### 差集发现的候选（未确认身份）

关闭模块列表后计数下降的候选 vtable：

```
0x2571148  0x233C228  0x2304418  0x2346D18  0x234A778(-15)  0x234A7F0(-15)
0x2413C60  0x23E4CC8  0x23728C0  0x2376880
```

前 4 个槽都指向 .text，形态像 vtable；但结构探查没能找到“指向模块键名”的指针，
**身份未定**。

### 新进展：指纹法把同族 vtable 全找出来了

用**共享虚函数槽**做指纹（buildable 都继承同一基类，基类实现占用固定槽位）：

```
共享槽: slot2/slot3 = 0x1AF7F0、slot15 = 0x1AF850、slot18 = 0x1AE130、slot19 = 0x15A000
```

拿这组指纹扫整个 .rdata，命中 12 个同族 vtable：

```
已知: 0x237C8E8 Army   0x2397DC8 Zone   0x2397E78 ClearDepositBlocker
      0x2397F28 District  0x23981E8 Building  0x2398298 UpgradeBuilding
未知: 0x2397028  0x2397FD8  0x2398348  0x23983F8  0x23984A8  0x23C7730
```

`CBuildableStarbaseModule` 必是这 6 个之一（另几个应该是 StarbaseBuilding、StarbaseUpgrade、
StarbaseUpgradeDesign、DefensePlatform 等）。

**试过但区分不出来的判据**：

- 结构里找模块键名：6 个都有实例、都读不到键（键可能不是 `+0x20` 明文，或是哈希）
- 槽函数是否引用序列化器（0x8C6340 等）：都没有直接引用
- 槽里是否返回静态字符串：没有这种槽
- 开/关模块列表的实例数差：6 个计数都稳定在 8–10（常驻，不是按列表临时创建）
- 建造队列条目：`CConstructionQueueItem` 只有 `back/queue/paying_country/progress/progress_needed/resources`，
  没有直接的 buildable 指针

### OGAS（原 imperial-auto-governor）给出的关键结论（2026-11-17 查阅）

仓库：[V2t-Nostalgia/OGAS](https://github.com/V2t-Nostalgia/OGAS)（原 Imperial Auto Governor，GPL-3.0）。
他们的思路**不是自己拼 buildable**，而是调用游戏自己的“生产者函数”。

1. **构造函数签名**（Linux 符号 0x2360940）：

```
CAddBuildableToQueueCommand(TPdxRef<CCountry>,
                            TPdxRef<CConstructionQueue>,
                            CPdxScopedPtrImpl<CBuildableBase, false>)
```

   也就是说 buildable 是**调用者构造好、以拥有型指针交进去**的；命令本身不管创建。

2. **正规做法**：`NConstruction::CreateAddBuildableCommand(country, colony, zone, buildingType,
   existingBuilding, bool)`（Linux 0x26d2e90）内部先取队列、再调 `CreateBuildable`，
   由游戏按原生对象状态选择子类（新建/升级/替换/维修）——**vtable 由游戏自己设**，不需要人工去找。

3. **规则链**：`CAddBuildableToQueueCommand::IsValid` → `CConstructionQueue::CanAddItem` →
   buildable 虚槽 `+0x68`（`CanBuild`）。所以 IsValid 通过还要再过队列检查。

4. **4.5.2 已有 46 次 `CBuildableStarbaseModule` 入队记录**（AI 行为捕获），证明这条路走得通；
   他们的 Windows 4.5.2 SDK 里 `add_buildable_to_queue_command` = vtable 0x23c1b60 / size 0x30，
   与本机 4.5.4 的 0x23C7C80 / 0x30 一致（**size 相同，字段在 +0x20/+0x28/+0x2c**）。

5. **序列化标签**（linux_tokens.json）：

```
0x3df7 buildable_starbase_building
0x3df8 buildable_starbase_module     ← 目标
0x3df9 buildable_starbase_upgrade
0x3dfa buildable_defense_platform
```

### 本轮：buildable 构造器普查（0x6BB000-0x6BE000）

用「分配大小 + 写 vtable」的模式扫出来这一段里有 11 个 buildable 构造器：

```
size=0x20  vtable=0x2398088    ← 未知
size=0x28  vtable=0x2398138    ← 未知
size=0x20  vtable=0x23983F8    candD
size=0x28  vtable=0x23984A8    candE
size=0x20  vtable=0x2398298    UpgradeBuilding
size=0x20  vtable=0x2398348    candC
size=0x18  vtable=0x2397F28    District
size=0x28  vtable=0x2397FD8    candB
size=0x20  vtable=0x2397DC8    Zone
size=0x18  vtable=0x2397E78    ClearDepositBlocker
size=0x20  vtable=0x23981E8    Building
```

**注意**：`CBuildableStarbaseModule` 不在这 11 个里（这段是行星建造用的族）。
两个新候选 `0x2398088`、`0x2398138` 值得下一步验证。

### 基座建造的调用链：追到哪儿了（2026-11-17 05:00）

起点：51 个引用 `add_buildable_to_queue_command` vtable（`0x23C7C80`）的站点里，
**`0xBD20CC` 是基座相关的那一个**——它读 `[r12+0x880]`（`CStarbase::build_queue`）。

所在函数 `0xBD1AA0-0xBD2165`（1733 字节）的逻辑：

```
0xBD207A  mov esi, [r12+0x880]        ← 取基座建造队列
0xBD2087  mov ecx, 0x548
0xBD208C  call 0x2028B18              ← EngineAlloc(0x548)
0xBD20A0  call 0xABB360               ← 构造那个 0x548 字节的对象
0xBD20CC  lea rbx, 0x23C7C80          ← 把 AddBuildable 命令 vtable 写到栈上结构
0xBD20E9  call 0x1AEC40               ← 析构辅助（释放数组、重设子对象 vtable）
0xBD2107  call 0xBEDB00               ← 入队/提交
```

`0xABB360`（130 字节）构造的对象：大小 **0x548**，最终 vtable **`0x23A9800`**，
内部还有一个 vtable `0x234A830` 的子对象，字段有 `+0x538/+0x53c/+0x540`。

**顺带遇到但还没定性的 vtable**：

```
0x23A9800   0x548 字节对象（像"建造计划/条目"）
0x23D0A30   0x28 字节对象（0xBEC6B0 构造，带 +8/+0x10/+0x12/+0x14/+0x15/+0x16/+0x18/+0x20/+0x24 字段）
0x234B5F0   同上先设后覆盖的基类 vtable
```

### 05:30 又试了三条，都不通

1. **找“多 token 分派函数”**（一个函数里同时出现 0x3ddf…0x3dfa 多个立即数）：
   全 134906 个函数里**一个都没有**——token 是每个类各自的序列化器单独写的，没有统一分派表
2. **`0xBEDB00` 内部**：是通用临时数组/提交机械（栈上分配、`lea 0x234CE98` 的临时结构），
   没有 buildable 的分配与 vtable 写入
3. **从数据库路径字符串 `starbase_modules` 反查**：字符串在 .rdata 里 3 处
   （RVA 0x2302D7F / 0x2317DA6 / 0x2559E32），但**没有直接 lea 引用**——它被放进某张
   CString 表里间接使用，需要先找到那张表的结构

### 结论与下一步

- **基座模块的 buildable 不在** `0x6BB000-0x6BE000` 那 11 个行星建造族里
- 真正的构造发生在 `0xBEDB00`（567 字节，走虚槽 `+0x40/+0x60/+0x70`）或它下游
- 下一步：反汇编 `0xBEDB00` 的内部调用，找 `lea <vtable>; mov [rax], reg`（分配 ≤0x40 的那处）

### 下一步建议

1. 差集思路已废弃（整堆快照噪音太大），改用**定点读取**：先拿到候选 vtable，
   再用 `scan_memory_for_u64_range` 数实例、用 `read_memory` 验结构
2. 候选身份靠**对象结构**判定：buildable 里应有指向类型对象的指针，其 `+0x20` 是键名；
   用 `read_memory` 顺着指**址**读（注意 `address` 要传数字，传字符串会被当成 0）
3. 或者绕开 vtable：**GUI 点击**已经能完成正规建造（点「+」→ 选模块 → 进队列），
   需要脚本化时直接点坐标即可，不必动引擎命令

## 引擎容器的正确写法

给引擎命令写 `ref_array<T>`（例如 `fleets` / `ships`）时，**不要按 `std::vector` 猜布局**：

```
错误（会崩）:      {begin, end, cap}
正确（CPdxArray）: {?, data +0x08, capacity +0x10, size +0x14}
                  data 必须是引擎堆分配的指针
```

现成辅助函数在 `galaxy_manager.cpp`：

```cpp
SetRefArray(cmd, mf::fleets, ids, &why);   // 内部 EngineAlloc + 写 data/capacity/size
```

**为什么 data 必须来自 `EngineAlloc`**：命令析构时会自己释放这块内存；
用 `new`/`malloc` 分配会被引擎的分配器释放，于是崩溃。

**已验证的命令**：

| 命令 | 字段 | 实测 |
|---|---|---|
| `merge_fleets_command` | `country` / `fleets` ref_array / `mia_reinforce` / `cancel_orders` | 2 支并 1 支 ✓ |
| `disband_fleets_command` | `fleet` ref_array / `country` | 解散新建的护卫舰舰队 ✓ |

两个都是**异步**的：投递后等一两秒再查结果。

## 工具链：python / node 从哪来（PATH 上那个是商店占位）

**`PATH` 里的 `python` 不能用**：它指向
`%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe`，是 **0 字节**的应用商店占位符，
运行直接退 9009。所以“这台机器没有 python”这个观察是对的——只是真身不在 PATH 上。

真正要用的两个，都在 DSH 运行时里：

```
python  %USERPROFILE%\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe   (3.12.14)
node    %USERPROFILE%\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\node\bin\node.exe    (v24.21.0)
```

`node` 根本不在 PATH 上，任何 `node xxx.mjs` 都要写全路径。

**`rebuild_reload.py` / `reload_dll.py` / `inject.py` 只用标准库**（ctypes），
不需要额外装包；跑法就是把上面那个 python.exe 拿来用：

```powershell
& '%USERPROFILE%\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe' rebuild_reload.py
```

**额外装过的两个包**（重跑 `tools/sdk_dumper` 需要，重装 python 运行时会没）：

```
pefile   capstone
```

## 重建（没有 cmake / git 时用 cl 直编）

**2026-11-23 起构建树搬了家**：`D:\Temp` 被清理过一次（cmake 的 share 与 MinHook 的 `src/*.c` 都丢了），
现在固定放在 `D:\build\stl-build2`；依赖改成**本地预置**（`deps\minhook.x64.lib` + json 头文件），
cmake 用 VS Build Tools 自带那份（`...\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`）。

**现在最快的路径**（已封装）：

```
python rebuild_reload.py            # 卸载 → 编译 → 部署到插件目录 → 注入
python rebuild_reload.py --no-build # 只部署 + 注入
```

源码以**构建树** `<构建目录>\src\stellaris-mcp-main` 为准；`source\` 是它的同步副本
（`rebuild_reload.py` 每次编译成功后自动 `robocopy /MIR`，单向：构建树 → 快照，不要手改快照）。
下面的手工命令只在依赖缺失时用。

```powershell
# 四样依赖下到 <构建目录>：cmake-3.30.5、minhook(master)、nlohmann/json v3.11.3、源码 zip
# 根 CMakeLists 加：
#   set(FETCHCONTENT_FULLY_DISCONNECTED ON)
#   set(FETCHCONTENT_SOURCE_DIR_MINHOOK <…>/minhook-master)
#   set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON <…>/json-3.11.3)
cmd /c "vcvars64.bat && cmake -S <源码根> -B build -G \"NMake Makefiles\" -DCMAKE_BUILD_TYPE=Release && cmake --build build"
# 产物 build/stellaris_bridge/stellaris_bridge.dll → plugins/stellaris-mcp/
```

## 脚本怎么分

**日常使用**（本目录根）：

- `rebuild_reload.py` —— 卸载 → 编译 → 部署 → 注入，一条命令
- `reload_dll.py` —— 只做卸载 + 注入（DLL 已经编好）
- `inject.py` —— 只做注入

**探查脚本**（`probes\`）：`mcp-probe.js` 通用 CLI、`snapshot-colony.mjs` / `snapshot-planet.mjs` 快照、
`diff-all.js` 快照对比、`snapshots\` 数据。

**历史记录**（`history\`）：22 个一次性脚本（`apply_*.py` 等），记录每个方法当初怎么加进源码的。

## 版本事实

- 游戏 `Cygnus v4.5.4`（build `0x6AF23393`），SDK 与本机 exe 匹配
- `get_fleets` 的正确计数**只在本地版有效**；换回官方 DLL 会退回假数字
## 追加：换设计的方法与两个事实

多了一个**换设计**的方法与两个相关事实：

| 方法 | 作用 | 状态 |
|---|---|---|
| `set_fleet_template_design {fleet_id, from_design_id, to_design_id}` | 把编制里某条设计改成另一型（写条目 `+0x20+0x530`，即 `+0x550`） | ✅ 已验证：飓风 1552 → 50333052 |
| `set_ship_upgrade_design {fleet_id, to_design_id}` | 给舰队里每艘船写“待升级目标”（`CShip+0x98C`） | ✅ 写入验证；但是否必要见下 |
| `scan_ship_for_values {ship_id, values[]}` | 船头 `0x2000` 字节里搜 u32（设计号定位工具） | ✅（一度被清理脚本误删，已恢复） |

**换设计的正规入口是 `upgrade_fleet` 的两个可选参数**：

```
upgrade_fleet {fleet_id, starbase_id, target_design_id}
→ "CFleetUpgradeDesignCommand posted successfully"
```

只传 `fleet_id` 会被引擎拒（`rejected by the engine`）；补上 `starbase_id` + `target_design_id` 才成功。
所以**不需要**手写 `ship+0x98C`——命令自己带目标设计。

两个语义要分清：
- 改**模板条目**（`set_fleet_template_design`）→ 影响**以后补员造什么**，现有舰船不动
- 改**升级目标**（`upgrade_fleet ... target_design_id`）→ 现有舰船改装，要船坞排期与合金

## 一条教训

用正则批量删代码时**不要用跨行通配**：`drop_unsupported.py` 里 `(?:.*\n)*?` 把两个待删方法之间的
`scan_ship_for_values` 一并吞掉了。以后按函数边界逐个删，删完核对方法表。
## 热替换 / 编译一条龙

`reload_dll.py`（`inject.py` 配套）可**不重启游戏**换 DLL。

**注意**：DLL 被游戏占用时链接器会报 `LNK1104: 无法打开文件 "stellaris_bridge.dll"`，
所以 `rebuild_reload.py` 的顺序是 **先卸载 → 再编译 → 复制到插件目录 → 从插件目录注入**。
注入路径改到插件目录之后，构建产物不再被占用（否则每次都要先手动卸载）。

```
python rebuild_reload.py             # 卸载 → 编译 → 部署 → 注入（约 30 秒）
python rebuild_reload.py --no-build  # 只部署 + 注入
```

模块引用计数是 2，FreeLibrary 要调两次才真正卸载（脚本已循环处理）。

## 岗位编制与在岗：读法

### 结论：在岗和编制都能从内存读，之前“读不到”是偏移写错了

```
colony + 0x38   句柄数组（4 字节一个）
colony + 0x44   条数
每个句柄 handle & 0xFFFFFF → CPopJob 库对象

CPopJob 对象：
  +0x18   岗位类型对象（键名在类型 +0x20，阶层在类型 +0xAA0）
  +0xF8   在岗（×100000）        ← 之前读的是 +0x90，差了 0x68
  +0x100  编制（×100000）        ← 之前读的是 +0x98
  +0x108  加成（×100000）
  +0x110  上限修正（×100000）
```

改成 `+0xF8 / +0x100` 之后，实测与存档逐条一致：

```
planet 14（蜂群）  bio_trophy 3057/4650、mining_drone 1200/1200、agri_drone 650/650、
                   technician_drone 450/450、calculator_physicist 285/285、
                   replicator/coordinator/patrol_drone 200/200、两个 135/135、maintenance_drone 590
                   总在岗 7102
planet 199（帝国 4）foundry 2600/2600、soldier 2600/2600、artisan 600/600、enforcer 600/600、
                   technician 1200/1200 …  总在岗 12495
```

### 同时修掉的第二个 bug：蜂群殖民地 strata 恒为空

输出时只遍历固定的 `ruler / specialist / worker / civilian`，而蜂群的岗位阶层是
`bio_trophy / complex_drone / maintenance_drone / simple_drone`，于是**数字算对了、数组却是空的**。
现在固定四阶层之后会追加其余阶层（按名字排序）。

### 编制表 `planet+0x788`（2026-11-18 定稿）

```
planet + 0x788   指向数组的指针
planet + 0x790   条数
每条 16 字节：{ 数值 × 100000 (i64), 类型 id (u32) }
```

**这张表是混合的**：planet 14 的 63 条里，**只有 11 条是岗位**，另外 52 条属于别的 id 空间
（数值多为 0.04 / 0.5 / -0.1 / -0.75，也有 -91.228 这种）。

**怎么区分**：岗位的 `类型 id` == 该岗位类型对象 `+0x48` 处的 u32（实测 bio_trophy = 41562）。
扫一遍 CPopJob 库（2048 个槽），读每个对象 `+0x18` 的类型，再看类型 `+0x48`，
就能建出 id → 键名 的映射；映射里没有的，就不是岗位。

`get_planet_job_capacity` 已按这个规则过滤，输出 `{job_key, job_name, stratum, workforce}`，
被丢掉的条数放在 `skipped_non_job` 里。实测：

```
planet 14（蜂群）  11 条（跳过 52），合计 8105
  活体陈设 4650、采矿 1200、农业 650、技工 450、物理 285、协调者 200 …
planet 199（帝国 4）9 条（跳过 132），合计 10700
  冶金师 2600、士兵 2600、技工 1200、矿工 1200、农夫 900、保护者 800 …
```

**不要以为“编制表能给出小数岗位”**：建筑与区划给的编制都是整数。
表里那些带小数的行（4.511、2.4、-0.75…）本来就不是岗位，已被过滤掉。
只有当编制来自非建筑、非区划的来源时，才可能出现带小数的岗位编制——属于少见情况。

两个工具真正的区别：

```
get_planet_jobs          读 CPopJob 对象：在岗 / 编制 / 加成 / 上限修正 + 优先级标记
get_planet_job_capacity  读行星编制表：只有编制，但能拿到“表里的原始数值”
```


### 封装的方法

```
get_planet_jobs {planet_id}              在岗/编制/加成/阶层（已修正，任何帝国可用）
get_planet_job_capacity {planet_id}      只读编制表：{workforce, job_ref} + 总计
```

> **记录一次弯路**：2026-11-18 复核对，先用 `+0x90/+0x98` 读到全 0，误判“CPopJob 不存人数”；
> 又只看了编制表前十条（2.4 / 0.5）就断定“0x788 不是编制”。两次都是**取样不足就下结论**。
> 正确做法：拿一个已知真值（这里用存档的 4650）去反查，命中的位置才是答案。

## 岗位数据定位：结论与过程

### 结论

**能从内存读到的**：

| 数据 | 位置 | 验证方式 |
|---|---|---|
| 人口组规模 | `CPopGroup + 0x3D8` | 首都四个组交叉验证（2400 / 1455 / 1700 / 232） |
| 岗位编制（max） | `planet + 0x788`（**指针**）→ 每条 16 字节 `{数值×100000, 类型 id}`，条数在 `+0x790` | 把活体陈设从 1500 改成 3000，内存同步变化。**2026-11-18：已封装 `get_planet_job_capacity`**；注意表里只有一部分是岗位，其余是修正项 |
| 人口组对象解析 | `TPdxRef<CPopGroup>::_pDatabase = 0x311BD28` → `+0x18/+0x20` → `arr[(id & 0xFFFFFF) * 16 + 8]` | 组 id 131 → 对象，`+0x10` 是 id |

> **2026-11-18 推翻**：在岗**是**能从内存读的，在 `CPopJob` 对象 `+0xF8`（编制在 `+0x100`）。
> 当年“读不到”是因为 ① 只扫了 `CColony` / `CPopGroup` / `CPlanet` 三者，**没扫 CPopJob 库**；
> ② 后来读 CPopJob 时偏移写成了 `+0x90/+0x98`。细节见上文「岗位编制与在岗：读法」。
> 下面这几段保留为过程记录，结论以定稿节为准。

**（已过时）**~~读不到的~~：**岗位的“在岗”分配**（例如复制者 100 / 协调者 81 / 猎巡者 62）。
它既不在 `CColony`、`CPopGroup`、`CPlanet` 这三个对象里，也不在它们一层指针可达的 134 个容器里，
甚至不在这三者的缓存里——~~判断是按需计算~~。

**权重也不常驻**（2026-11-16 22:40，玩家提示用 `debugtooltips` 看 Job Weights 后验证）：

`debugtooltips` 打开后，人口组的悬停提示会列出每个岗位的权重，例如

```
(维护子个体) maintenance_drone:    0.08
(复合子个体) replicator:  3198.48 / 3294.43 (incumbent)
(复合子个体) coordinator: 2197.00 / 2262.91 (incumbent)
(复合子个体) patrol_drone: 760.43 /  783.24 (incumbent)
```

拿这些数去人口组本体（0x8000 字节）与 46 个指针目标里搜，**float / double / ×100000 定点三种编码全部 0 命中**。

**因此整条链是**：

```
编制（持久：planet+0x788）
人口（持久：CPopGroup+0x3D8）
岗位优先级 / 特质 / 修正（部分持久）
        ↓ 悬停或每月刷新时现算
      权重 → 分配 → 界面文字
```

~~权重、分配、提示文字三者都算完即弃~~ → **权重确实是现算的**（这点没错），
但**分配结果会落到 `CPopJob` 对象的 `+0xF8`**，并不需要钩函数。

**在岗的值会物化，但没有稳定字段**（2026-11-16 23:30，当时用 `scan_memory_for_int` 实测；该方法已删除，见文末）：

实验：把停泊港的分配改成 **98 / 77 / 68**（定点 9800000 / 7700000 / 6800000），然后

1. 存档前全内存搜这三个值 → 都命中（10 / 8 / 9 处）
2. 触发一次存档 → 数量基本不变（**说明不是存档才写进去的**）
3. 关掉殖民地面板再搜 → 仍然命中（**说明也不是界面计算的临时数据**）

排除桥自己请求缓冲（`0xC09D…`，参数以 int64 存在 json 里，是假命中）之后，真命中集中在

```
0x14DCE5AE7A8 … 0x14DCE5AF888    约 4 KB，三个值都在
```

**但**：

- 用 `read_memory` 打开这段，看到的是**分配器元数据和小整数**（9、15、159、4608、25600…），
  没有“岗位 id + 人数”那样的记录结构
- **每次扫描地址都在变**（值在区内移动），说明这是一块被反复复用的临时缓冲

**结论**：分配的值确实在内存中被物化，但落在**会被反复覆写的复用区**，没有稳定字段可读。
按结构读这条路基本走不通；剩下的只能是**钩住写这块区域的代码**（即计算函数）。

**2026-11-18 更正**：在岗数用 `get_planet_jobs` 直接读（`CPopJob +0xF8`），不需要存档，也不需要钩函数。
存档仍是“一次拿全部殖民地”的捷径，但不再是唯一途径。

## 手动切换殖民地设计：已打通

**引擎命令存在**，桥里也接上了：

```
set_colony_type_command   (CSetColonyTypeCommand)
  colony      +0x20  ref<CColony>
  country     +0x24  ref<CCountry>
  designation +0x28  ptr（设计对象指针，序列化成键字符串）
```

桥里新增 `set_colony_designation {planet_id, key}`：拿行星 → colony id → colony 对象，
再用玩家国家 id 和设计对象指针组命令。**命令通道本身是通的**（编译、注入、调用都没问题）。

**关键发现（快照 diff 得来的）**：`CColony` 的 `+0x100` / `+0x108` 不是字符串，而是
**设计对象的指针**。用 UI 改设计前后各拍一份快照，diff 只有 4 处变化，其中两处就是这两个指针：

```
水培居住站 → 0x14fbbb4cce0
堡垒站     → 0x14fbbb4d170
```

顺着指针读出对象：vtable 在开头，**键名在 `+0x20`**（短键内联、长键存指针）。
这些对象在堆上成片分布，因此可以从殖民地当前的设计指针出发，按 vtable 扫描找到任意键的对象。

**现在的行为**：`set_colony_designation {planet_id, key, designation_ptr?}`

```
只给 key          → 桥自己从 colony+0x108 出发按 vtable 扫描解析（已验证）
给 designation_ptr → 跳过解析，直接投命令（外部先解析时用）
```

实测：停泊港 堡垒站 → 发电站 → 水培居住站，三次都成功，`get_planet_details` 立刻反映。

**已备好的材料**（下次继续时用）：

- 游戏定义文件：`<Steam 库>\steamapps\common\Stellaris\common\colony_types\`
  （7 个文件，共 **101 个设计键**，如 `col_habitat_energy`、`col_capital_machine`）
- `TPdxNullObject<CColonyType>::_pInstance = 0x3119EE8` 可作为另一个入口

**下一步思路**：找数据库自己的 `Find(key)` 成员函数（比手工遍历可靠），
或者从 `SColonizationData` 的默认设计那条线反推对象布局。

## 设计对象池会搬家

`set_colony_designation` / `list_colony_designations` 原来靠「从殖民地当前设计指针出发，
±4 MB 窗口内按 vtable 扫描」。**重开游戏后这个假设失效了**：

```
重启前：窗口内能扫到 101 个设计对象
重启后：同一个窗口 0 个（对象池被分配到很远的地方）
```

改成「限时全堆扫描」也不成——桥的单次调用有 **2.5 秒硬上限**，
从 `0x14000000000` 起扫到池子所在位置需要 3–4 秒，于是报
`Main thread task timed out`；把预算压到 1.8 秒又扫不到。

**修复方向**（当时记的；后来发现整堆扫描没意义，已放弃）：

```
scan_designations { planet_id, start_addr, budget_ms } → { designations, next_addr }
```

调用方循环取页，直到 `next_addr` 不再前进或找到目标键。
关键点：**不要把"池子在锚点附近"当成前提**。

## 内存建筑列表为什么会漏：新 zone 未注册

**现象**：用 `set_district_zone` 新建一个槽位特化，再用 `build_building` 往里建建筑，
内存的建筑列表会**漏掉这座新建筑**，而存档里是有的。

**实测证据**：

```
zone_default    id=80         key=zone_default    挂 5 座建筑
zone_industrial id=16777301   key=zone_industrial 挂 0 座   ← 新建筑不在
占位对象         id=0xFFFFFFFF key=（空）           挂 1 座   ← 新建筑其实在这
```

**根因**：`slot_index` 的参数语义没错（就是 zone id，引擎拿它去查自己的 zone 表），
问题是**新建的 zone 在同一会话里还没注册进引擎的查表**，于是落到 id=-1 的占位对象上。

**决定性验证**：存档 → 载入之后，同一颗星球读出来

```
载入后：6 座 | zone_default(5), zone_industrial(1)    ← 建筑归位
```

载入时所有 zone 会重新注册，占位对象消失。

**与 instant_build 无关**（2026-11-17 00:30 追加验证）：

```
不开 instant_build，让 zone 自然建满 360 天 → 建完
再往里建「资源仓库」→ 内存仍然读不到（zone_minerals 空，总数 5）
存档 → 载入 → 6 座，zone_minerals → building_resource_silo ✓
```

所以触发条件是「**zone 是本次载入之后新建的**」，不是建造速度。
载入时注册过的 zone 都正常；会话中新建的要等下一次载入才算“注册完成”。

**实践规则**：

- **新建 zone 后不要立刻在里面建建筑**；要建就先存档再载入
- 内存读到的建筑数量如果比存档少，先怀疑这个，别怀疑偏移
- 之前"首府/无人机库/民用工业体消失"也是同一原因——那几次都伴随 zone 改造

### 建筑清单要读 zone 自己的数组，不要读 colony+0x58（2026-11-18 修）

`get_planet_details` 原来按 `colony + 0x58` 的建筑 id 表遍历，再用建筑 `+0x18` 的 zone 指针配对。
两个毛病：

1. **新盖的建筑本会话里没写进那张表** → 读不到（档案馆里刚造的研究实验室，`get_buildable_buildings`
   的「已用 1/3」和存档都认，只有它读不出）
2. **表里残留的 `0` 被当成 id 0**（正好是首府）→ 同一个首府被印成两条，看着像多盖了

改成按 **zone 自己的数组**枚举，与「已用 N/M」同源：

```
CZone::buildings = +0x1A0     // ref_array<TPdxRef<CBuilding>>：数据 +0x1A8、条数 +0x1B4、元素 4 字节
元素 = 建筑 id（u32） → TPdxRef<CBuilding>::_pDatabase (0x311BDC0) 的数组 [id&0xFFFFFF]*16+8 取对象
CBuilding：id = +0x8，类型 = +0x20（键在类型对象 +0x20）
```

修后实测：档案馆 1/3（研究实验室 id 16777382，第一格）、政府特区 3/6、重复条目消失。
**别再回头用 colony+0x58 那份表**，它对新建建筑是滞后的。

建筑的**格子号**读 `CBuilding::position`（`sdk::ent::CBuilding::position = +0x1A4`，0 起），
与 GUI 的格子号一致；读不到才退回数组下标。实测（2026-11-18）：政府特区 0/1/2、档案馆 0、
停泊港政府特区 0–4，全部对得上。**注意**：`build_building` 那个 `slot_index` 参数是 **zone id**，
不是格子号——游戏不给玩家选格子，新建筑永远落到最低空位。

**新增方法**：`localize {key | keys[]}`**新增方法**：`localize {key | keys[]}` —— 用游戏自己的本地化表把键翻成当前语言的名字。
实测（中文包）：

```
job_replicator     → 复制者          job_coordinator   → 协调者
job_patrol_drone   → 猎寻者子个体     job_bio_trophy    → 活体陈设
district_hab_housing → 居住站区划
building_organic_sanctuary → 有机庇护所
```

这就是“岗位/建筑/区划 键 → 名字”的映射表——不需要自己维护字典，游戏里换语言它也自动跟着变。

**已删除方法**：`scan_memory_for_int` —— 它坏了（任何值都返回同样的 5 个地址），
2026-11-17 清理时移除；需要扫内存请用 `scan_memory_for_u64_range`（见文末清理记录）
   `colony+0x200`、`colony+0x1A8`、`colony+0x1298`、`*(colony+0x5B8)` 都试过，全是资源/统计的 `(值, id)` 对
2. **模型修正**（玩家指出）：群星某次大更新后岗位逻辑改为**人口组**，一个 232 人的组在内部再分到三个岗位
3. **人口组对象**按 id 解析成功，规模定位到 `+0x3D8`；但组内 0x2000 字节搜遍没有 100/81/62
4. **钩子**：`NPlanetJobs::UpdatePopJobs`、`NColonyUpdate::...InPopJobs`、`CColony::MoveSizeToPopGroup`
   都能抓到调用与参数，但拿到的是“组间迁移量”，不是“组内岗位分配”。
   顺带发现 SDK 注释里 `UpdatePopJobs` 的参数顺序是反的（第一个参数才是殖民地数组）
5. **`planet+0x788` 找到编制表**（1500→3000 验证），这是本轮唯一的正确定位
6. **快照对比定论**：先测“噪声基线”（暂停无操作连拍两份，逐字节相同），
   再把在岗从 100/100/43 改成 100/81/62 —— **139 个快照文件零变化**，彻底排除这三个对象

**方法论**：噪声基线归零后，快照对比就是可靠证据。这套流程（`probes\snapshot-*.mjs` + `diff-all.js`）可复用于任何“某数据存在哪”的问题。

**快照工具链**（`probes/snapshots/`）：`snapshot-colony.mjs`（殖民地+人口组+指针）、`snapshot-planet.mjs`（行星+指针）、`diff-all.js`（按标签逐字段比较）。


## 目录结构

```
stellaris-bridge-patch\
  source\                 ← 完整的当前源码快照（29 个 cpp + 30 个 hpp，来自构建树）
  probes\                 ← 桥的客户端探查脚本（调管道，不参与编译）
    mcp-probe.js               通用 CLI：调任意桥方法
    snapshot-colony.mjs        殖民地 + 人口组 + 46 个指针目标
    snapshot-planet.mjs        行星 + 88 个指针目标
    read-console-strings.mjs   控制台对象的字符串池
    diff-all.js                两份快照逐字段比较
    snapshots\                 快照数据，按标签分目录（保留 base2 / cmp2 / noiseA / noiseB 四批）
  apply_*.py              ← 每个新方法“怎么加进去”的一次性脚本（历史记录，可重放）
  drop_unsupported.py     ← 早前删掉不支持方法的脚本
  restore_scan.py         ← 恢复被误删函数的脚本
  inject.py               ← 远程线程注入
  reload_dll.py           ← 卸载 + 注入（热替换）
  rebuild_reload.py       ← 卸载 → 编译 → 部署 → 注入，一条龙
  README.md               ← 本文件
```

**注意**：早前这里放的是 `galaxy_manager.cpp` 等四个文件的**旧快照**（03:54），
今天新加的 6 个方法只在构建树里，已经过期，已删除并改为整份同步到 `source\`。

**构建树**（真正的编译位置）：`<构建目录>\src\stellaris-mcp-main\stellaris_bridge`
**构建产物**：`…\build-nmake\stellaris_bridge\stellaris_bridge.dll`
**部署位置**：`%USERPROFILE%\Documents\Paradox Interactive\Stellaris\plugins\stellaris-mcp\stellaris_bridge.dll`

**已删除**：`diff-snapshots.js`（工作区根，被 `probes\diff-all.js` 取代且路径失效）、各目录 `__pycache__`、
中间快照批次（locked / after / clean / sanctuary / month / with-sanctuary）。

**已移入 `history\`**：`add-colony-tools.py`（插件加殖民地工具的一次性补丁）、
`inject-design.py` / `inject-design2.py`（早期设计注入，已被 `stellaris-save\design_templates.py` 取代）。



构建目录是临时目录，随时会被清理，所以 `source\` 是它的**同步副本**，不要删。

**同步方向是单向的**：构建树 → `source\`，由 `rebuild_reload.py` 在每次编译成功后自动 `robocopy /MIR`。
`source\` 不是编辑目标；手工改它会在下次重建时被覆盖。
**快照批次说明**（`probes\snapshots\`，2026-11-16 21:15 精简）：

| 标签 | 内容 | 为什么留 |
|---|---|---|
| `noiseA` / `noiseB` | 暂停且无操作，连拍两份 | 噪声基线：**逐字节相同**，证明对比可信 |
| `base2` | 改动前（在岗 100/100/43） | 定论实验的对照组 |
| `cmp2` | 改成 100/81/62 后 | 与 base2 对比 **139 个文件全 0 变化** → 在岗不在这些对象里 |

已删除的批次（locked / after / clean / sanctuary / month / with-sanctuary）都是过程中的中间状态，结论已写进上文。
## 逆向工具

本地补丁版桥新增三个**只读**探针，用来分析运行时内存（都在 `galaxy_manager.cpp`）：

| 方法 | 参数 | 用途 |
|---|---|---|
| `probe_console_slot` | `slot_offset`, `length` | 把 `CConsole+slot` 当指针跟过去，输出可打印 ASCII 与 std::string 头 |
| `scan_memory_for_text` | `needle`, `include_code`, `max_seconds`, `start_addr`, `max_bytes` | 在已提交可读区搜 UTF-8/UTF-16/UTF-32 文本，分页返回 `next_addr` |
| `read_memory` | `address`, `length` | 读任意地址，输出 ASCII 与十六进制 |
| `probe_colony_carriers` | — | 逐个殖民地读 `CColony.carrier`（低 32 位 id、高 32 位类型），区分行星殖民地与方舟（type=1） |
| `scan_edict_arrays` | `max_seconds` | 找可写内存里连续的法令对象指针数组（8/16/24/32/40/48/64 步长都试）；2026-11-18 找 UI 法令列表用的 |

**注意**：IPC 单次等待上限是 **30000 ms**（`ipc_server.cpp`，2026-11-17 从 2500 ms 提上来），
但长扫描仍建议分页（`max_seconds≤2`、`max_bytes≈300MB`，靠 `next_addr` 翻页）——
一次卡住太久会让游戏主线程停顿。

**已确认**：

- `CConsole+0x00` 指向控制台命令的帮助字符串池（描述、参数名、提示文案混排）
- `CConsole+0x38` 是事件系统字符串，`+0x40/+0x48` 是引擎错误日志缓冲
- exe 里硬编码 `"Instant build mode"`（偏移 `0x23E6208`，大写 I）与 `"instant_build"`（`0x23EB280`）
- **打印出来的输出行没有在已扫描的 3 GB 可写内存里留下副本**（ASCII/UTF-16/UTF-32 都搜过），
  所以 `run_console` 的文本大概率不持久，读输出只能靠截图

**踩过的坑**：扫描会命中桥自己的 JSON-RPC 请求缓冲（needle 就在参数里），必须按上下文过滤
（含 `jsonrpc` / `read_memory` 的丢掉）；分页预算用尽时若直接跳到区域末尾，会整块跳过内存。
## 工具整理：两组重复功能合并

**合并的两组**（原来是重复功能）：

| 原工具 | 现在 | 用法 |
|---|---|---|
| `stellaris_event_check` + `stellaris_event_mark` | **`stellaris_event_mark`** | `dry_run:true` = 只判断是否重复；不带则记录 |
| `stellaris_log` + `stellaris_panel` | **`stellaris_panel`** | `log:N` = 读最后 N 行；`show:true/false` = 显隐面板 |

**保留但性质重叠的**（已在描述里区分，不合并）：

| 组 | 区别 |
|---|---|
| `stellaris_state` | 通用转发器，能调任意桥方法；**只作为逃生口**，优先用具体工具 |
| `stellaris_notifications` / `stellaris_alerts` | 徽章队列 vs 警报横幅，是两个系统 |
| `stellaris_colonies` / `stellaris_sectors` | 平铺清单（带警报数）vs 按星域分组 |
| `stellaris_planet_jobs` / 存档侧 `jobs_report.py` | 同一件事的两条来源：内存逐殖民地读 vs 存档一次拿全 |

**动作类工具的验证状态**：以插件端 README 的工具表为准（✅ 已跑通 / ⚠ 未实测 两档，`plugin.test` 会核对两边一致）。
这里不再单独维护——早期那份 14 只的清单到 2026-11 已全部验过，留在这里只会过期。

## 科研：已研究科技的内存直读（`get_researched_techs`）

已研究科技不再靠临时存档。`CCountry + 0x1620` 是 SDK 标的 `tech_status`（`CTechnologyStatus`），
它 `+0x18` 处那个 vector 就是「已研究」列表：

```
tech_status + 0x18    vector 头
tech_status + 0x20    数据指针
tech_status + 0x28    容量（本局 63）
tech_status + 0x2C    条数（本局 46）
每条 0x28 字节：
  +0x08  level（i32）
  +0x10  CTechnology*（键在 +0x20，localize 用 tech_ 前缀）
```

实测 2222.01.01 读出 46 项，与 `stellaris-save/tech_report.py` 从存档 `tech_status` 解析出的键集合
**逐条一致**。桥方法 `get_researched_techs`，插件工具 `stellaris_researched_techs`。

- **别把 `+0x28` 当条数**：它是容量。市场那边的 `IsResourceUnlocked` 就是这么写的（读多了而已，不影响布尔判定），
  要精确条数用 `+0x2C`
- **定位过程**：新增只读探针 `probe_tech_status {offset, length, dump}`——先用 `CTechnologyDatabase`
  （`+0x311B6E8` / 数组 `+0x50` / 条数 `+0x5C` / 键 `+0x20`，本局 679 个）认出全部科技对象，
  再扫 `CCountry` 看哪些 qword 命中它们。全对象只命中一个（`+0x178` 的 `always_available_tech`），
  说明列表不存指针；数组是靠「容量 63 / 条数 46」与存档对账认出来的

> 可复用流程（先找谁在读它 → 桥内探针 → 指针匹配做否证 → 找容器四件套 → 两条来源对集合 → 接线顺序）
> 与这次踩的坑——先在游戏里复现一次、再拿两条独立来源对集合，别靠单点证据定案。

## `get_situation_log` 的 `stage` 与阶段区间

局势（situation）原来只读进度与每月变化，现在多阶段号与**阶段区间**：

```
CSituation::stage = +0x578          // i32，0 起（SDK tok 0x3d27）
CSituation + 0x590                  // 阶段区间数组的容量（本局 10）
CSituation + 0x594                  // 条数（本局 4）
CSituation + 0x598                  // 内联数组，每项 0x10：{begin, end} 两个 int64 定点 ×100000
                                    // 赤字局 (0,25) (25,50) (50,75) (75,100)
                                    // 第 stage 项 = 本阶段区间，就是 UI 的「该阶段在 x 到 y 之间生效」
```

- 数组容量 10，正好占满 `0x598..0x638`，0x638 之后是下一个 vtable —— 边界对得上，不是猜的
- `progress`（`+0x218`）是**本阶段内**的 0–100；`monthly_change`（`+0x220`）为正＝推进、为负＝回落
- **上游的 `+0x588` 在 4.5.4 已失效**：`docs/REVERSE_ENGINEERING_CHEATSHEET.md` 记的是 4.5.1 的
  「`+0x588` 缓存 `{0, 192000000}`」，照抄读出来是 `19917826.85 / 171798.69`。现在在 `+0x598`，
  前面还多了容量/条数两个 u32
- 新只读探针 `probe_situation {situation_id, offset, length}`（hex dump 单个局势对象），定位靠它
- **区间来自运行时缓存**，所以 `total_progress` + `section_weight` 的按比例局势也对得上（缓存里已是绝对值），
  不必解析脚本里的比例与修正
- **没有「阶段名」可读**：阶段标题在 `common/situations/*.txt` 里按 `stage` 序号对，桥不做这层映射

## `probe_first_contacts` / `get_first_contacts`

第一次接触的**阶段**原来读不到——`get_contacts` 只给国家 id、名字、国家类型和一个写死的
`first_contact_in_progress`。现在直接走游戏自己的表：

```
TPdxRef<CFirstContact>::_pDatabase = 0x311BC50        （sdk::glob）
  db + 0x18  槽数组        db + 0x20  容量（本局 1024）
  每槽 16 字节，对象在 +8，id 在 +0x8（sdk::rt::CFirstContact_id）

CFirstContact（sdk::ent，stellaris_sdk.hpp:2178-2195）
  +0x28 location   +0x2C owner   +0x30 country   +0x34 leader
  +0x38 stage（指针，阶段对象的键在 +0x20）      +0x40 date
  +0x48 clues     +0x50 last_roll   +0x54 difficulty   +0x58 days_left(u64)
  +0x60 name      +0x320 status
```

两个方法：

- `probe_first_contacts` —— 只读探针，多给一段 `+0x28..+0x68` 的原始字节，定位用
- `get_first_contacts { all }` —— 可读版：对手国名（`PersistentNameText` 回退本地化）、特使 id、
  `owner_is_player`、**地点**（`location` 走 `CGalacticObject` 表换成星系名，返回 `location` + `system_name`）；
  `all=false`（默认）只留玩家发起的那批。插件工具 `stellaris_first_contacts` 就是它

**实测要点**（逐字段对账的原始记录没有随仓库发布）：

- 表是**全银河**的，且每对国家有两条方向相反的记录（`owner`/`country` 互换），条数 ≠ 接触数
- 界面「阶段 N」= `stage` 键里的数 **+ 1**（`default_stage_1` → 阶段2；同一条推进前后各验一次）
- `+0x54` = 该 stage 的 `difficulty`，与 `common\first_contact` 逐条一致（7/7）
- `+0x50` 是 `last_roll`（1–10，0 = 从没掷过），不是技能；技能是国家常数，不进对象
- `+0x28` 的 `location` 确实是**星系 id**：`get_system { system_id: 646 }` 回「格拉姆」，与 `system_name` 一致；
  `0xFFFFFF` 表示没有地点
- 推进时只有 `stage` 指针、`clues`（清零）、`+0x54`、`status`（1→0）变，其余字节不动
- `id` 不是界面那个数：界面「id」是 `+0x30` 的国家引用（`0x01000004` = 16777220）

## `probe_leader` / `assign_envoy` / `probe_first_contact_view`

三个与首次接触配套的方法。

**`probe_leader { leader_id }`（只读）** —— 解一个领袖的 `CLeader::location`（`+0x110`）与
`council_location`（`+0x120`），每块 16 字节：

```
+0  type        8 = 首次接触（低字节；派上之后整字读到 0x13000008，高字节含义未查）
+4  assignment  EEnvoyAssignment 下标，4 = "first_contact"
+8  id          目标 id —— 首次接触填的是**接触记录 id**，不是国家 id
+0xC position   含义未查（实测 0）
```

**`assign_envoy { leader_id, contact_id, dry_run, reset_cooldown }`** —— 原生 `CAssignLeaderCommand`
（token 0x40AD、size 0x38：`leader` +0x20、`location` +0x24、`reset_cooldown` +0x34）。

两个坑，都实测过：

1. **`leader` 要带高位标记的引用，不是裸 id**。本档特使 62 的引用是 `0x0900003E`（150995006）；
   只传裸 `62` 时 IsValid 通过、Post 也成功，但结果是**把人从原位置解绑、没有派到新位置**。
   桥现在自己去 `CLeader + 0x20`（`sdk::rt::CLeader_id`）读这个值，调用方只给 slot id。
2. **`IsValid` 不校验职业**：科学官（61）能过，行政官（93）被拒但 `reason` 是空串；而且刚挪过的人
   会回「time 360内无法移动」。所以插件层自己拦 `class != envoy`，冷却这种原因原样带回给调用方。
   `reset_cooldown` 写了 +0x34 = 1 **并没有**绕过冷却——那条「无法移动」另有来源，未查。

实测：`{leader 63, contact 4}` → posted，接触 `envoy_id` 62 → 63，leader 62 自动解绑成
`{type 0, assignment 9, id -1}`。

**`probe_first_contact_view { depth }`（只读）** —— 首触视图的 GUI 控件树。原因：视图里真正的按钮
（「了解更多。」）**不在** `view+0xE0` 那个 options_box 里（锁定状态下它是空的，所以 `stellaris_events`
会兜底塞一个假的「关闭」）。探针按 GUI 自己的 hashmap（`+0x6F8` 值数组 / `+0x710` 键数组 /
`+0x71C` 条数，键名在 key + i*48 + 16，文本在控件 `+0x168`）走一遍，并把 `view+0x60..0x400`
的指针当额外根试一遍。实测能读到：`site_name` / `chapter_subtitle` / `title_text`（阶段号）/
`progress_text` / `roll_chance_text` / `difficulty_type_value` / `leader_bonus_value` /
`clues_value` / `leader_name` / 锁定状态文本。

**还没解决**：「了解更多。」这个按钮既不在控件表里，也没有找到它的引擎函数；目前只能按坐标点
（实测 `(419, 672)` 有效，`(419, 655)` 落空）。

## 事件选项：非 GUI 推进（`select_open_event_option`）

```
select_open_event_option { ope, option, dry_run? }
```

`ope` 来自 `list_open_events` 的 `ope` 列（`COpenPlayerEvent*`）。它绕开 `CEventWindow`，直接调

```
RVA 0x8E4550(ope, 选项下标)
```

——这正是 `CEventWindow::PostEventOptionSelection`（RVA `0x1083910`）在找到 open event 之后调用的
那个 worker：窗口只用来按 `[win+0xA1C]` 找 ope，真正的选择与派发都在这里。worker 自己构造
`CSelectEventOptionCommand`（`+0x40` 事件编码取自 `CEvent+0x8`、`+0x44` 选项、`+0x48` open event id、
`+0x4C` 国家，`+0x50…` 由 `ope+0x10` 的 16 字节 scope 拷入），所以效果与点按钮一致。

**实现要点**

- 先校验 ope 真在 `CGameState+0x180` 数组里（野指针进 native call 会崩），再校验下标 < `CEvent+0x5E0` 条数
- `__try` 放在**独立的 helper** `SafeSelectByOpenEvent` 里——函数里有 `std::string`/`nlohmann::json`
  时直接写 `__try` 会 C2712
- 返回 `still_open` 是**投递那一刻**的读数；事件要到下一 tick 才被消费

**实测（2026-11-19 深夜，两次）**

```
grand_archive.2000 / option 0  → 虚空虫 status 1→0、阶段 1→2、线索清零、特使自动挂上、倒计时开始
grand_archive.2005 / option 1  → 虚空虫 status 1→2（建立联络），首都出现「虚空虫研究」特殊项目
```

`first_contact.1` 那类「初次接触通报」投了只从列表消失，不改接触状态。

## 考古遗址的归属与「能不能挖」探针

`get_archaeological_sites` 多三个字段：`owner_id` / `owner_name` / `owner_is_player`——遗址所在**星系的归属**，
直接用 `Owner(galaxy_object)` 读、不经 intel 过滤（`get_system` 对未勘察星系会给 null，拿判「域外」会失真）。
本档 34 处里 16 处归别国、1 处归我方（舍尔比通）、17 处无主。

新方法 **`probe_excavatability { fleet_id }`**（只读）：给每个遗址各建一条
`CExcavateArchaeologicalSiteFleetOrderCommand`，**只跑 IsValid、不 Post**，一次快照 + 一个循环
（不能拿 `excavate_site` 逐个问——它每次都 `Take()`）。返回 `{fleet_id, count, valid_count, rows:[{site_id, valid, reason?}]}`。

`excavate_site` 加了 `dry_run`：只 IsValid、不投递，回 `{success:true, valid, reason?}`。**投递那条路仍未实测。**

实测（2280.09.07 游牧档，34 处，56 ms）：只有 3 处 valid——全是 `chapter == 0` 且星系无主；
域外回「✗ 不能发掘你帝国疆域之外的考古遗址」；被别国占着的回「✗ 该考古遗址已经被其他帝国完全发掘」
（同一处用它自己那艘在挖的船问就 valid，所以这句实际含义是「已经有船占着」）；其余 26 处**不给原因**。
IsValid **不看**舰种与科学家——`allow` 里的 `is_ship_class = shipclass_science_ship` / `exists = leader`
只负责把按钮变灰，不参与命令校验。

## `get_civilian_fleets` 的领袖映射

症状：8 艘科研船里只有 `fleet 1` 有 `leader_name`，其余全空——而实际只有「异类科学船」没上科学官（玩家指出）。

根因：`CLeader` 的 `assignment_target` 是**舰船 id**（`CShip`），不是舰队 id；单舰舰队两者并不相等
（实测 `fleet 776` 的船是 `2665`，领袖「前桅帆奥迭戈」也挂在 `2665` 上）。老代码建表时按 target 存、
查询时拿 `fleet_id` 去比，只有恰好相等的 fleet 1 能命中。

修法：先按船建 `ship_leader_map`，再用 `CFleet::ships`（`+0x320`，个数 `+0xC`）建「舰船 → 舰队」表换回来。
修后 7 艘带科学官的科研船全部能读到，只剩 `100665677 异类科学船` 没有领袖，与玩家的记忆一致。

## 星界裂隙的探索与筛选

`get_astral_rifts` 多一个 `owner_name`（裂隙的所属国：`CAstralRift+0x120` 的引用掩码后取国名）。

**没做出来的**：`system_id` / `system_name` / `system_owner_*`——本想用「各星系对象的 `astral_rifts` 数组反查」，
但建表后一条都没配上（`with system_id: 0`，`rid & 0xFFFFFF` 也试过），说明数组里的值与 `CAstralRift_id`
不是同一套编码，或者那批对象不在 `Snapshot::systems` 里。**待查**；「在哪片疆域」目前只能靠引擎判定回答。

新方法：

```
explore_astral_rift { fleet_id, rift_id, queue?, dry_run? }
  CExploreAstralRiftFleetOrderCommand  token 0x4237 / vtable 0x23BFA60 / size 0x30
  fleet +0x20 / astral_rift +0x24 / queue +0x28
probe_rift_explorability { fleet_id }
  每个裂隙各建一条命令、只跑 IsValid，回 [{rift_id, valid, reason?}]
```

实测（2281.01，游牧）：44 处里只有 **1 处** valid（id 43 `microverse`）；**13 处**回
「✗ 我们在该恒星系的影响力不足，无法确保这次探索的安全。」（定居帝国疆域内、没有研究条约）；
其余 30 处**不给原因**——已结束，别国探索完的裂隙也进不去。

**科学官锁死在引擎层**：正探索裂隙的领袖，`CFireLeaderCommand`（0x2EB3，即 `dismiss_leader` / 工具
`stellaris_dismiss_leader`）会被拒，原话「无法解雇正在探索星界裂隙的领袖」（2026-11-20 实测，leader 161）。
所以裂隙的领袖只有开局能挑，中途换不了人。

## 上游对照（Yidhar/stellaris-mcp）

- 本桥的 DLL 有 **299 个 IPC 方法**，上游 29 个工具的后端全部都在，所以合并只写插件层。
  按处理分支数略少：`open_notification`/`probe_notification`、`enact_decision`/`enact_planetary_decision`、
  `set_job_priority`/`prioritize_job` 三组是同一分支的别名，各算一条。

  按时间追加的清单（细节都在对应小节）：

  - 2026-11-23：`get_our_starbases`（方舟 / 驿港 / 常规三组）、`get_queue`（按 id 直读队列）、
    `get_bombardment_stances` / `set_bombardment_stance`
  - 2026-11-22：`begin_uplifting`、`activate_astral_action`、`probe_grand_archive`；
    `get_discoveries` 加 `probe`（星界行动逐条跑 IsValid）
  - 2026-11-21：`propose_trade_deal`、`get_federation_laws`、`install_agreement_capture`、
    `get_agreements`、`probe_federation_types`
  - 2026-11-20：`declare_war`、`probe_war_goals`、`get_incoming_actions`、`get_pending_votes`、
    `respond_diplo_action`、`vote_diplo_action`、`set_defender_war_goal`、`probe_war`、
    `probe_notification`、`get_wars`、`scan_astral_scar`、`scan_rift_systems`、`probe_system`、
    `probe_astral_rift`、`probe_leader_manager`、`probe_excavatability`、`explore_astral_rift`、
    `probe_rift_explorability`、`get_player_country`；`excavate_site` 加 `dry_run`、
    `get_astral_rifts` 加 `owner_name`、`propose_diplomatic_action` 加 `federation_type`

  插件侧「用了几个」由 `scripts/plugin.test.mjs` 按实参取字面量现算（2026-11-22 之前那版只扫 `rpc(` 行，
  会漏掉 `pipeCall` 与跨行的 `buildAction`，少数 8 个）。
- 上游 `command_builder.hpp` 原话：命令输出「goes to the console and the game logs, not back to the caller」
  → **上游也没解决读控制台输出**，与本项目的结论一致
- 游戏目录下的 `console_history.txt` 只记录**输入的命令**，不含输出
- 上游 `tools/sdk_dumper/` 有一整套函数指纹库（含 `CColony::CalcColonizationProgressPerc`：
  殖民进度 = 殖民地人口 / `COLONY_POPS_REQUIRED`），换游戏版本时可用它重找偏移

## 删掉整堆快照与坏掉的整数扫描

**删掉**：

- `heap_snapshot_take` / `heap_snapshot_diff`（一次性整堆快照）：跑 20 秒会卡主线程，
  且差集被堆的正常变动淹没（一次 22000+ 条变化），没有可用信号
- `heap_snapshot_take_page` / `heap_snapshot_diff_page` / `heap_snapshot_result`（分页版）：差分同样噪音
- `scan_memory_for_int`：**是坏的**——给任何值都返回同样的 5 个地址，78ms 扫完 300MB 不可能，
  留着会骗人

**保留**：

- `scan_memory_for_u64_range {lo, hi}`：只报告"这个值在哪些地址出现"，不假装知道对象边界，
  实测 1 秒扫 315MB，定位 buildable vtable 实例时靠的就是它

**教训**：大范围扫描只有在"问题窄"时才有用（例如已经知道精确的 vtable 值）；
拿它去找"什么东西变了"必然被垃圾噪声淹没。

## 已知未修：`recruit_planet_army` 的物种回退

**症状**：在某颗殖民地（14 号水精之室，colony 0）上招陆军，任何兵种都回
`Cannot be queued (prerequisites not met)`；同一兵种在 54（colony 32）、2174（colony 30）都正常。

**根因**：`OutlinerManager::RecruitArmyJson` 在不给 `species_id` 时这样回退——

```cpp
SafeReadPtr((const void*)((uintptr_t)colony_obj + 0xF68), &sp_arr);   // 物种数组
SafeReadU32((const void*)((uintptr_t)colony_obj + 0xF70), &sp_cnt);   // 数量
if (sp_arr && sp_cnt > 0) SafeReadU32((const void*)((uintptr_t)sp_arr + 8), &target_species);
```

**colony 0 上这块读不到有效物种**（读出来的是不具战斗能力的物种/无效 id），`target_species` 停在
`0xFFFFFFFF` 或一个不可当陆军的物种，引擎于是把整条 `CBuildableArmy` 判成前置不满足——报错文案是笼统的
"prerequisites not met"，跟真正的前置无关，容易误判。

**实测证据**（2026-11-18）：

```
14   colony 0   不传 species → 失败
14   colony 0   species_id=3405774849 → success（colony_id 0、queue_id 1）
2174 colony 30  不传 species → success
54   colony 32  不传 species → success
```

**当前绕法**：插件工具 `stellaris_recruit_army` 自己定物种——先试建国物种，再逐个试帝国物种，
直到入队成功；调用时不必再传 `species_id`。

**彻底修法**（待办）：

1. 回退改成**玩家国家的建国物种**（`CCountry` 的 founder species），或从星球拥有者取主导物种
2. 读不到物种时直接返回 `species unknown`，别再让引擎回那句笼统的前置错误

## 运输舰队里的陆军构成：已修

`GetArmiesJson` 已经把陆军分成 `garrison_armies` 与 `transport_armies`（用 `CArmy::ship` 是否为
`0xFFFFFFFF` 判断），**但 `armies::ToJson` 没把 `ship` 带出来**，所以插件端没法把「哪支舰队装了哪几支陆军」
拼起来（`probe_fleet_ships` 只给 ship_id，`get_armies` 只给 army 列表，两边缺一个键）。

**最小改动**：`ToJson` 里加 `{"ship", army.ship}`；插件端用 `probe_fleet_ships(fleet_id).ships[].ship_id` 与之
join，就能像军事舰队一样列出构成。

**已做**（2026-11-18）：`army_access.cpp` 的 `ToJson` 加了 `{"ship", a.ship}`，重编重注入；
插件端据此写了 `stellaris_fleet_armies`（`get_armies` × `probe_fleet_ships` 做 join），实测「第3运输舰队 2 艘 / 2 支陆军」正确。

## 飞升天赋：`activate_ascension_perk`

`CActivateAscensionPerkCommand`（token `0x383A`，`country +0x20`、`object +0x28`）。
对象从 `CAscensionPerkDatabase`（`base + 0x31195F0`，数组 `+0x50` / 数量 `+0x5C`，每条键在 `+0x20`）按 key 查，
然后 `IsValid` / `Post`——和 `adopt_tradition` 的写法同源。

- 没有空槽时引擎会拒，并回原文：「✗ 需要空的飞升天赋槽位。…」
- 键不存在时回 `Ascension perk 'xxx' not found in database`
- 插件工具：`stellaris_choose_ascension_perk`
- **别用控制台 `effect add_ascension_perk = <key>` 代替**：它能用，但绕过校验——同一个天赋能重复选、槽位为 0 也照样加

## 法令目录：改成枚举

`get_edicts` 原来用**硬编码的 14 个键**，其中 `subsidize_farming` 一类在当前版本并不存在，
名字也只能回键名。现改为枚举 `TGameDatabase_CEdictsDatabase_pInstance`（`base + 0x3119568`，
数组 `+0x50` / 数量 `+0x5C`，每条键在 `+0x20`）——本局 **171 条**，名字用 `edict_<key>` 本地化。

- 生效状态仍是 `country + 0x27F8`（`ref_array<SCountryActiveEdict>`，0x20 字节一条，`edict` 指针在 `+0x10`）
- **全集，没有按每条的 `potential` 过滤**，所以会出现蜂巢/仆从专用的条目
- `toggle_edict` 实测可用（`map_the_stars` 开/关，active 0→1→0）
- **`can_enable` 探针**（2026-11-18 加）：每条建一个 `CAddEdictCommand`（`country +0x50`、`name +0x30`，
  `SetString` 写字符串），只 `IsValid` 不 `Post`；本局 171 条里 115 条可开，单次调用约 270 ms
- **坑**：`CAddEdictCommand::IsValid` 不校验每条法令的 `potential`——蜂巢专用的 `nutritional_plenitude_hive`
  在机械帝国上也能开成功。所以它答的是「命令能不能执行」，不是「这条法令适不适合这个政体」

## 守方战争目标：`set_defender_war_goal`

`CDiploActionSetDefenderWarGoalCommand`（token `0x34E6`，`country +0x20`、`war +0x24`、
`defender_war_goal +0x28`，`kSize 0x48`）。实测 war 50331651（方舟魔法少女 → 普尔扎多尔国度，
我方是守方、守方目标为空）：**空 → `wg_humiliation`，`have_defender_war_goal 0x110` 从 0 变 1**。

### 三层原因，每修一层引擎才多吐一句

| # | 引擎原话 | 真因 | 改法 |
|---|---|---|---|
| ① | 「只有防御方盟主才能设置战争目标。」（只有这一句） | `defender_war_goal` 是内嵌的 `CWarGoal`，**头 8 字节是类级静态指针**；从 0 拼＝把它抹了 | 以本场战争自己的守方槽为模板抄 0x20 字节，再覆盖 type/war/actor/target/win |
| ② | 追加「我们对方舟魔法少女没有任何要求臣服宣战理由。」 | 抄了攻方的 `wg_subjugation`（要求臣服），守方没有这个 CB | 换 `wg_humiliation`：`common/war_goals/00_war_goals.txt` 里它是 `defender_default = yes` |
| ③ | 又只剩第①句 | 命令的 `country` 要**裸 id**，发带标记的 `0x01000036` 被判「不是防御方盟主」 | 传裸 `54`（`my_id`，不 `| 0x01000000`） |

**第①句不是会变的失败原因**，它是这条命令的固定要求文本：拿一场与本方无关的战争（83886080）
去问，它照样出现，后面才追加真正的理由（「已经设置战争目标。」）。**读拒绝文案只看追加的那几句。**

### `CWarGoal` 在内存里的样子（实测，逐字段对上）

```
+0x00  类级静态指针：四份 goal 完全相同 → 0x7FF7989A4130
       （落在 exe 映像内；按 base 0x7FF798000000 算 RVA 0x9A4130。别当数据写，抄现成的）
+0x08  type        CWarGoalType 对象指针（空 goal 时指向 null 实例，非 0）
+0x10  war         引用
+0x14  actor       引用
+0x18  target      引用
+0x1C  win         守方固定 0
+0x1D  accomplished
```

`CWar` 里两段各 0x20 字节：`attacker_war_goal = +0xD0`、`defender_war_goal = +0xF0`。
`SDK` 的 `CWarGoal` 命名空间给的是相对偏移，加上段起点即为绝对位置。

### 两个编码陷阱（见到就照这个查）

- **同一对象图里两种国家引用并存**：`CWarGoal::actor` 里存的是带标记的 `0x01000009`，
  而 `SWarParticipant::country` 读出来是**裸 54**。所以「该传哪种」不能靠推。
  桥现在 `dry_run` 把两种都跑一遍、两条判决一起回（`variants[]`）
- 命令拿到的 `war` 要引用（`50331651` = `0x03000003`，`0x03` 是战争的标记），
  和国家 id 的 `0x01` 前缀不是一回事

### `probe_war` 的两种读法

```
probe_war { war_id, offset, length }                从 war+offset 读，报**绝对**偏移（老行为）
probe_war { war_id, offset, length, follow: true }  把 war+offset 处的值当指针，从那里读，报**相对**偏移
```

参战方数组头是 `{size, cap, ?, data}`：攻方 size 在 `+0x78`、数据指针 `+0x88`；
守方 size 在 `+0x90`、数据指针 `+0xA0`。元素按 `SWarParticipant` 是 0x18 字节
（`country +0x8` / `caller +0xC` / `call_type +0x10` / `fleets_gone_mia +0x14`）。

- **未定案**：守方 size 读到 2，与界面「防守方只有我们一个」不符；`follow` 出来的元素也没能按步长排整齐。
  **别拿这一列当结论**
- 修 `follow` 时踩过一次：把 `follow=false` 的偏移逻辑写丢了，`offset` 被忽略、一律从 `war+0` 读。
  两种模式现在各管各的偏移

## 程序化求和提案「自己发」的那条路

**做法照抄上游 OGAS**（`docs/runtime/LONG_RUNNING_AI_COMMAND_CAPTURE_4_4_6.md`）：他们拿到
`CDiploActionSetDefenderWarGoalCommand` 不是靠逆 UI，而是**挂钩旁听 PostCommand**，等 AI 自己发出来。
照这个思路挂了两处，然后合成一条能自己发的路。

### 为什么不能用「类型」造

`action_offer_peace` 这个键在 exe 里存在（字符串 `0x2327288`），但
`NDiplomacyUtil::CreateDiplomaticAction(type, actor, recipient)` 对它**返回 null** ——
不是 `is_listed = no` 的问题（那个只影响上不上界面），是通用生产者根本不为它造对象。
所以必须自己构造，而构造需要类标识与字段，这两样只能从「一个活的对象」上抄。

### 两个旁听（都在 `command_builder.cpp`）

```
InstallProposeCapture()   Hook 引擎 PostCommand（与 CommandBuilder::Post 走的是同一个函数）
                          凡是 vtable == diplo_action_propose_command 的命令，抄下 +0x20 的动作对象
                          → propose_capture / propose_capture_clear
InstallCloneCapture()     Hook RVA 0x9A8E80（COfferPeaceAction 的「模板→新动作」函数）
                          抄下入参模板与返回对象 → clone_capture / clone_capture_clear
```

实测（界面递交一次）：**clone 的入参、clone 的产出、投递出去的动作，三者 104 字节完全相同**。

### `COfferPeaceAction` 的真身（104 字节 = 0x68）

```
+0x00  vtable                 base + 0x23B6B20     ★
+0x08  基类 vtable            base + 0x2388BA0     （与侮辱动作同一个：CDiplomaticAction）
+0x10  0            +0x18  0
+0x20  基类 actor             （裸 id，本次 54）
+0x24  0            +0x28  0
+0x2C  分配器残留，**不是字段**（ctor 不写；两次捕获读到过 0x247、ASCII "_old"）
+0x30  0 (qword)    +0x38  0
+0x3C  分配器残留，**不是字段**
+0x40  单例指针（本次 0x24736D2B960，**两次读档都一样** → 静态/单例，不是每对象新分配）
+0x48  0 (byte)     +0x4C  0x630225
+0x50  peace_offer 的类级 tag  base + 0x23B6CC8   ★
+0x58  offer.actor   +0x5C offer.recipient   +0x60 offer.war   +0x64 offer.status  ★
```

`★` = 构造时必须写对的三处。`status` 编码（**2026-11-20 21:xx 三个值逐个实测**）：

```
0 = status_quo        维持现状
2 = demand_surrender  达成战争目标 —— 引擎自己的序列化名，**不是 force_demands**
1 = surrender         我方投降 —— 投出去当场结战，提案不留在 incoming_actions
```

**换选项对比（关键证据）**：status_quo 与 force_demands 两份模板**只有 `+0x64` 一位不同**
（`00` → `02`），其余 103 字节完全相同（含 `+0x40` 那个单例指针）。所以构造只需覆写
`+0x20`（基类 actor）与 `+0x58/+0x5C/+0x60/+0x64`。

### 静态还原出来的两个入口

```
0x9A8E80   0x65 字节    void* f(const void* template_0x68)
           分配 0x68 → call 0x9A8D67（基类初始化）→ 写 vtable 与 +0x50 tag
           → 从模板 +0x58/+0x5C/+0x60/+0x64 抄四个字段 → 返回新动作
           这是实际采用的那个（引擎自己的初始化路径，含引用计数）

0xFED350   0x180 字节   f(arg1, cmd_out, int arg3)   ← 一次把动作和命令都造出来
           从 arg1+0x3D8 读引用解析对象 → call 0x9B1D50 把「记录」填在栈上 rsp+0x20（0x68 字节）
           → 造动作 → 在 cmd_out 写 vtable、+0x08=-1、+0x10=0xFFFF0000、[cmd_out+0x20]=动作
           **没解 0x9B1D50 的契约，所以没用它**；知道它存在即可
```

### 桥方法 `try_peace_offer { war_id, country_id, status, dry_run }`

做法：抄本会话捕获的模板 → 覆写 `+0x20/+0x58/+0x5C/+0x60/+0x64` →
调 `0x9A8E80` 得新动作 → `Create(diplo_action_propose_command::kSpec)` →
`Set(0x20, action)` → `IsValid` →（`dry_run: false` 时）`Post`。

**实测（2026-11-20，war 50331651，我方 = country 54）**：

```
dry_run  → is_valid: true
真投     → posted: true
存档证据 → action_offer_peace { peace_offer{status=status_quo actor=54 recipient=0 war=50331651}
             type=propose actor=54 recipient=0 id=1924145348608 waiting_for_vote=no
             timeout="2281.07.06" num_favors=0 }
```

与界面递交产生的记录**逐字段一致**（存档 `dsh-peace-by-bridge-2026-11-20.sav`）。

### 三组样本对比（同选项 / 换战争）

| 对比 | 差异 | 结论 |
|---|---|---|
| status_quo vs force_demands（同一场仗） | 只有 `+0x64`（0 → 2） | 构造只需改 status |
| 我方那场 vs 奎瓦利安集体那场（war 83886080） | `+0x40` **完全相同**；`+0x20/+0x58/+0x5C` 的**国家引用编码不同**（裸 54/0 vs 带标记 `0x01000003`/`0x01000009`）；`+0x4C` 不同（`0x630225` vs 0） | 见下 |

**四份样本（2026-11-20 追加两组）**：

| 样本 | 战争 | 目标类型 | 递交方 → 对方 | `+0x4C` | 编码 |
|---|---|---|---|---|---|
| A | 50331651 | `wg_subjugation` | 54 → 0 | `0x630225` | 裸/裸 |
| B | 83886080 | `wg_conquest` | 3 → 9 | `0` | 带标记/带标记 |
| C | 33554433 | `wg_megacorp_subjugate` | 4 → 2 | `0x630225` | 带标记/**裸** |
| D | 83886080 | `wg_conquest` | 9 → 3 | `0` | 带标记/带标记 |

- **`+0x40` 是类级单例**：四份样本、换国家、换战争、换递交方、跨读档全都一样，可当常量复制
- **`+0x4C` 跟随战争目标类型**：两场「要求臣服型」都是 `0x630225`，同一场征服型战争
  从攻守两侧递交都是 `0` —— 与疲惫、递交方无关
- **国家引用编码逐字段不同，甚至同一份里混搭**（C：actor 带标记、recipient 裸）。
  `BuildPeaceActionFromTemplate` 目前按 actor 的 tag 位统一写两个字段，**遇混搭会写错** —— 待改成逐字段判定
- **实用规则**：**模板就在目标那场战争里采**。这样 `+0x4C`、编码、war 全自动正确，
  现有用法（同一场里采、同一场里发）不受影响

### 消融：模板其实可以不要（2026-11-20 定案）

从全零缓冲开始，逐字段加，`IsValid` 的反应：

| 输入 | 结果 |
|---|---|
| 全零 + `+0x20` actor + offer 四字段 | ✗ `IsValid` 内部访问违例 `0xC0000005` |
| 再加 `+0x40` = `0x24736D2B960` | **✓ 通过** |
| 不写 `+0x4C` | 照样通过 → 无关紧要 |

**`+0x40` 是什么：`action_offer_peace` 这个 `CDiplomaticActionType` 对象本身的地址。**

怎么查出来的：给 `probe_diplomatic_action` 加了 `type_ptr` 输出，它对 `action_offer_peace`
报的 `type_ptr` 正是 `0x24736D2B960` —— 与五份模板里的 `+0x40` 一模一样。
（`.data` 里扫不到这个值，说明它不是静态变量直接持有的，而是类型库里的对象。）

所以最终结论是**构造完全自足**：

```
+0x20  actor（玩家裸 id 或带标记，看模板/参数）
+0x40  action_offer_peace 的类型对象  ← 按 key 从 CDiplomaticActionTypeDatabase 现取
+0x58/+0x5C/+0x60/+0x64  offer 的 actor/recipient/war/status
其余全零
→ 调 RVA 0x9A8E80 → 得到动作 → 包进 propose 命令 → IsValid / Post
```

**补记（2026-11-21）：构造求和动作必须写基类 recipient `+0x24`。** 只用 `+0x20`（actor）时，
`IsValid` 会回**空原因**、发不出去；界面递交的动作里 `+0x24` 是带标记的收件方（如 `0x01000007`）。
旧用例「54 → 0」的收件方恰好是 0，所以一直没暴露。`BuildPeaceAction` 已补这一行。

**「先在界面递交一次采模板」这个前提已经不需要了**（`synth: true`，且 2026-11-20 起就是默认）。
模板路径仍保留作对照（`synth: false`），但它只是历史产物。

剩下的唯一注意点：**同一对目标上不能同时挂着两条** —— 会回「对方目前正在考虑我们提出的另一项提议」。

### 联邦里怎么发：必须 `force`，并由引擎起投票（2026-11-20 实测）

同一份存档里国家引用编码不统一 —— **玩家国家存裸 id（1），别国存带标记的（`0x0100000A`）**。
所以 `probe_country` / `try_peace_offer` 一律按**低 24 位**比较，构造时直接写「内存里存着的 id 原值」，
不自己拼。四处比较（`NewAction`、`ProbeDiplomaticAction`、`TryPeaceOffer`、`ProbeCountryJson`）都已改成 `& 0xFFFFFF`。

**联邦成员发起求和**（灵能魔女 1，war 33554435）：

```
IsValid → ✗「只有攻击方盟主或防御方盟主才能做出提议。」
```

但**界面根本不跑这道校验**（旁听证明它照投 propose 命令）。照它的做法投（`force: true`，
即 `Post(Check::EngineGate)`）之后，引擎自己把它变成联邦投票，存档里：

```
action_offer_peace { peace_offer{status=status_quo actor=1 recipient=16777226 war=33554435}
                     id=1906965479425  waiting_for_vote=yes  timeout="2281.07.01"
                     vote = { {voter=0 vote=neutral}, {voter=1 vote=yes} } }
```

即：发起方自动投赞成，其他成员待表态，全员通过才真正递给对方。私有存档：`dsh-peace-fed-2026-11-20.sav`。

**投票命令实测**（在投票页点「投票支持」被旁听下来）：

```
vtable_rva 0x23BA318 = CDiploActionVoteCommand
+0x24  id    = 0x1BC00000001 = 1906965479425   ← 与存档里那条待回应动议的 id 相同
+0x2C  voter = 0                              ← 投票国（内存里存着的 id 原值）
+0x30  vote  = 1                              ← 1 = 支持
```

**注意 SDK 的偏移对这命令偏了 4 字节**：`id` 实际在 `+0x24`（SDK 标 0x20）、`voter +0x2C`、`vote +0x30`。
旁听现在覆盖三种命令：`propose`（含动作对象）、`vote`、`respond` —— 后两者用 `cmd_capture` 读。

### 弃用记录：模板约束（原「唯一约束」，2026-11-20 已被上面取代）

~~模板必须由界面递交一次抓进内存，且 DLL 重编/重注入后要重抓。~~
保留这段是因为它解释了两个旁听钩子（`propose_capture` / `clone_capture`）为何存在：
它们当初就是为了把模板抄出来。现在构造自足了，但旁听仍有独立价值 ——
任何提议命令投递出去时都能当场抄下来，做对照或抓新类型。

### 记事

- **待回应的提议记在收件方**：存档里是 `CCountry.incoming_actions`（SDK `+0x1950`，
  `ref_array<SIncomingAction>`；隔壁 `rejected_actions +0x1968` / `proposed_actions +0x1980`）。
  所以查「有没有挂着」要看**对方**，不是自己 —— 发件方的动作表、通知表、战争对象都不会变
- `CCountry+0x1950` 读 `{ptr, size, cap}` 的 size 就能当探测器：0 = 没有，1 = 有一条
  （`probe_country { country_id: 0, offset: 6480, length: 16 }`）
- **`CDiploActionRespondCommand { id 0x20, accept 0x28 }`** 要的 `id` 就在那条记录里
  （本次 `1924145348608`）。想「代替某国接受提议」，差的只是把 `incoming_actions` 元素解出来
- 界面递交时的那条 UI 提示「对方目前正在考虑我们提出的另一项提议」是**最可靠的『已提交』信号**
  ——比通知、动作表、战争对象都灵。**但它只对排队型（status 0 / 2）成立**
- **三个 status 都投过（2026-11-20 21:xx，war 50331651）**：0 与 2 留在对方 `incoming_actions`
  （`status=status_quo` / `status=demand_surrender`），1 当场结战（`defenders_surrendered=yes`、`end=yes`，
  攻方 `wg_subjugation` win=yes；我方随即成了对方附庸）。所以**判断「1 有没有成」不能看 incoming_actions
  ——它本来就空**，要看 war 的 `end` 与外交表。样本：`dsh-peacestatus2` / `dsh-peacestatus1`
  （都在 `4_1420271128\`）。`IsValid` 三个值全放行，它不判「这条提案能不能被接受」，也不判输赢
- `status` 的枚举串表在 exe 里是 `accomplished / demand_surrender / surrender / not_set / status_quo`
  这几项紧挨着（文件偏移约 36935528 的 ASCII 区），数值顺序**不能照串表推**——
  0 与 2 的对应是存档读出来的，1 是行为证出来的
- `scan_memory_for_u64_range` / `scan_memory_for_qword` **按时间截断、每次从同一起点**：
  5 秒约 315 MB，`next_addr` 是下一页起点。而游戏堆在 `0x248…` 一带，离默认起点十几 GB，
  所以「只扫了一次没命中」不构成证据。用 `start_addr`/`max_bytes` 限定窗口才靠谱

## 踢出联邦成员的提议

```
动作对象 vtable 0x23B73A0（SDK 里没有类名；求和 0x23B6B20、宣战 0x23B9EB0）
+0x20 actor ｜ +0x24 recipient（被踢的国家，带标记的引用）
+0x30 serial ｜ +0x34 tag   →  id = tag << 32 | serial   （与求和 / 宣战同偏移）
+0x28 = 1（含义未查）｜ +0x38 另一个 u32（实测 0x03CE9700，**不是 tag**）
```

实测（2322.07.01 档，我方提案踢 country 2）：`serial 0`、`tag 886` → **id `3805341024256`**；
用 `vote_diplo_action` 投这个 id，界面里我方那一行 ✔→✘ 立刻翻转——id 对得上。

**坑：`+0x38` 很诱人**。先把它当 tag 会算出 `0x03CE9700 << 32 | 886` ≈ 2.7e17，
超过 2^53，JSON 数字传给桥会被浮点吃掉末几位（变成 …74000），命令照发、引擎找不到 id、**静默无效**。
两个国家各投一次都没反应，才回头发现 tag 读错位置。

**代码（2026-11-21 改成通用）**：`ContactsManager::GetPendingVotesJson` 不再跳过白名单外的动作——

- 三个已知 vtable 给 `kind`：`COfferPeaceAction 0x23B6B20` / `CDeclareWarAction 0x23B9EB0` / 踢人 `0x23B73A0`
- 其余一律 `kind: "unknown"`、`known: false`，并把 `vtable_rva` 一起吐出来
- 每条都读 `+0x20 actor`、`+0x24 recipient`、`+0x30 serial`、`+0x34 tag`（未知类读到的值不保证是 id，标记就是提醒）
- 空表的 note 改成「没有任何国家挂着待处理的提案（proposed_actions 为空）」

**未知类也验过（同日）**：对 country 51 提出「联邦联系」→ 一条 `unknown`、vtable `0x23B7BB0`、
`actor 1 → 51`、`tag 888 / serial 1` → id `3813930958849`；换到 country 0 用它投反对，
界面「给予合作国地位」表决窗里方舟魔法少女那行 ✔→✘。**没抓过 vtable 的类，id 一样读得对、投得动。**

**待查**：`+0x28` 与 `+0x38` 各是什么；表决通过之后引擎怎么处置（成员被踢、联盟条约与停战怎么算）。

## 手工造 `CVoteForFederationLawAction`，发起联邦法律表决

**引擎不造这类动作**：`action_vote_for_federation_law` 在 `common/diplomatic_actions/00_actions.txt` 里是
`auto_accepted = yes`，`NDiplomacyUtil::CreateDiplomaticAction` 对它一律返回 null（换过收件方、换过发起国）。
UI 是自己构造动作对象再塞进 `CDiploActionProposeCommand` 的。

**`propose_capture` 升级**（用来量形状）：抓取 0x100 → 0x200，返回值加

- `qwords`：8 字节对齐的整表（不用再手数 hex）
- `law_hits`：逐个字节偏移试 u64，凡指向 `CFederationLaw`（vtable RVA `0x2388C28`）且 `+0x20` 能读出键的，
  回 `{offset, obj, key}` —— 实测命中 `{offset: 80, key: "treaties_separate_no"}`，**law 在 `+0x50`**

**手工配方**（`ContactsManager::ProposeFederationLaw`，生产者 null 时走这条）

```
EngineAlloc(0x60) → memset 0
 +0x00 vtable       base + 0x23B7050   (CVoteForFederationLawAction)
 +0x08 基类 vtable  base + 0x2388BA0
 +0x20 发起国 id
 +0x40 动作类型对象（action_vote_for_federation_law，键在 +0x20）
 +0x50 法律对象（CFederationLawDatabase 0x3119638 按 key 取）
→ CDiploActionProposeCommand (+0x20 = 动作对象) → IsValid → Post
```

- 大小按 `0x60` 申请（对象至少 `0x58`；`COfferPeaceAction` 是 `0x68`，不是一个类）
- 实测：`IsValid` 通过；真投后多出一条 tag 889 的表决；0/1/2 三票赞成后当场结算；
  存档 `federation_progression.laws` 里 `free_migration_yes` 生效 —— 端到端成立
- `proposed_actions` 元素步长 **0x30**（动作在 +0x08）；按 16/24 扫会把一半条目读成乱码

## 战争与联邦：逆向要点

### 战争

```
CWar 枚举        TPdxRef<CWar> 0x311BDE8
  id +0x20 ｜ name +0x28 ｜ 攻守目标 +0xD0 / +0xF0（内嵌 CWarGoal：
  type +0x8、actor +0x14、target +0x18、win +0x1C）｜ 疲惫 +0x118 / +0x120 ｜ end +0x138
未解：win 的含义（攻方全 1、守方全 0）、疲惫尺度、参战方数组（+0x80 / +0x98，SWarParticipant，步长估 0x10 读到 0）
```

CDeclareWarAction（vtable `0x23B9EB0`）+ CDiploActionProposeCommand（`0x23BA540`）：

```
+0x20 actor ｜ +0x24 recipient（生产者已填）
+0x40 动作类型对象（declare_war）
+0x50 CWarGoal：+0x50 类级静态 0x…9A4130 ｜ +0x58 目标类型对象 ← 唯一要改的
       ｜ +0x60 war / +0x64 actor / +0x68 target 全是 -1 ｜ +0x6C win
+0x70 extra_attacker（ref_array）｜ +0x88 instigator
```

- `NDiplomacyUtil::CreateDiplomaticAction` 对 `action_declare_war` 能造、对 `action_offer_peace` 返回 null
- **造出来的动作交给命令后就不要再手动删**（命令析构会删一次，双重释放 → 静默崩溃，2026-11-20 连崩两次）

### 提案容器与 id

```
CCountry +0x1950 incoming_actions（别人递来的）｜ +0x1980 proposed_actions（我方递出去的）
元素 0x30 字节，动作对象在 +0x08（2026-11-21 量准；曾经按 16 / 24 扫，一半条目读成乱码）
动作 +0x30 serial ｜ +0x34 tag  →  id = tag << 32 | serial
```

- 表决只在**提案方**的 `proposed_actions` 里，被表决方两张表都是空的 → 只能扫全银河
- 通知对象里没有 id，也不是动作指针（±8MB 扫 vtable 0 命中）

### 联邦

```
TPdxRef<CFederation>::_pDatabase 0x311BD48 → db
  db+0x18 slots ／ db+0x20 cap（**按 cap 扫、跳过空槽**；db+0x24 不可靠，本档读到 1 而实际 2 个联邦）
  CFederation +0x08 id ｜ +0x80 progression ｜ +0x90 members ｜ +0xF0 proposed_law_votes_cooldowns
  progression +0x08 等级 ｜ +0x1A0 类型 ｜ +0x1A8 累计经验 ｜ +0x1B0 本级经验
              +0x1B8 base_cohesion ｜ +0x1C0 团结度（经验/团结度都是 ×1e5 定点）
  progression +0x2D0 = 待表决/最近一次表决的记录槽（票通过后不变，别当「当前值」）
CCountry +0x1C38 = 联邦引用：低 32 位 id，**0xFFFFFFFF = 不在联邦**（0 是合法 id，踩过坑）
```

- **成员反向标注**：成员数组（`+0x90`）指向 `.rdata` 的指针表、解不出国家引用；
  改成扫国家表读 `+0x1C38` 按 id 归组（玩家的法子），人数与名单都与界面对上
- **当前法律：内存能读，但不在「表面」**。投票通过那一瞬 `cmd_capture` 只抓到 `CDiploActionVoteCommand`
  （`0x23BA318`），切换是引擎内部改数据；`progression+0x2D0` 只是提案槽。真正的容器是 `progression+0x2E8`
  （见下一节「当前法律：`progression +0x2E8` 的容器」）。存档 `federation_report.py` 留作对账。
- **2026-11-21 晚的阴性复验**（两个法律不同的联邦做对照，#0 自由移民=启用 / #1=不干预）：
  ① `CFederation` 本体 0x280 字节逐 qword diff → 32 处不同，全是 id / 指针 / 成员数 / 经验团结度 /
  `+0xF0` 冷却指针一类，**没有「每个类别一个索引」那种块**；② `progression` 前 0x800 里按
  「qword 是堆指针 → 所指对象 vtable == `CFederationLaw`（`0x7FF798988C28`）」筛 → 候选 52、命中 **0**。
  当时据此判断「当前值既不是 law 对象指针、也不是索引数组」——**这个判断后来被推翻**：
  容器确实在 `progression+0x2E8`（见下一节），只是它的表头不是 law 的 vtable，直接按「qword → law 对象」筛必然 0 命中。
  教训：**先按文档对账，再否定「内存里没有」**——「扫不到」不等于「不存在」。
- **法律/类别对象自带数据库下标**：`CFederationLaw +0x08` = 它在 `CFederationLawDatabase` 里的序号
  （`free_migration_yes` = 48，与 `probe_law_db` 的数组序对得上），键在 `+0x20`（长度在 `+0x28`）。
  若将来证实状态是 bitmask / 索引数组，这个下标就是它要用的索引
- **联邦名** `+0x28` 不是明文；**每月经验**没有存储字段（界面现算）

### 当前法律：`progression +0x2E8` 的容器（2026-11-21 打通）

```
CFederation      +0x80  progression
progression      +0x2E8 → 容器
                    容器 +0x10 起，每 0x18 一槽（表头 0x10 字节，内容未解）
                        槽 +0x00 = CFederationLaw*   空槽全 0，**槽位不连续**
                        槽 +0x08 = 哈希/序号，+0x10 = 键的缓存字节（没用到）
CFederationLaw   +0x08 本库下标（centralization_high = 3、free_migration_yes = 48）
                 +0x20 键字符串指针（PdxString）
progression      +0x2D0 → 一对 law 指针（+0x00 / +0x10）= 待表决 / 最近一次表决
```

- 两个联邦各读到 **13 条**，与存档 `laws=` **逐条相同**（2026-11-21 交叉验证；#1 也核过
  `allow_subjects_to_join_no` / `centralization_minimal` / `research_pact_no`）
- **为什么之前两次零命中**：`probe_federation_laws` 与手工扫都只比「qword 所指对象的 vtable == law」，
  而这个容器的头不是 law 的 vtable——必须**先取 +0x2E8 的指针，再进容器按 0x18 走**
- 读取实现：`ContactsManager::GetFederationLawsJson`（IPC 方法 `get_federation_laws { fed_id }`）。
  逐槽验 vtable（`0x2388C28`）且键非空，**连续 4 个空槽就停**（防止把隔壁分配读进来），上限 0x300
- 例子：fed 0 的槽位是 `0x10, 0x28, 0x40, 0x70, 0x88, 0xB8, 0xD0, 0xE8, 0x118, 0x130, 0x148, 0x160, 0x178`
- 类别归属**不在这里**：law 对象不指回类别对象（类别对象也不指回来，两边前 0x200 都试过）。
  插件层用 `common/federation_laws` 的脚本表补（值块内 `has_any_federation_law_in_category`；
  事件法退到 `on_enact` 里 `<类别>_cooldown` 的修正名）

### 换联邦类型表决：不能手搓（2026-11-21 两次静默崩溃后定论）

动作类 `CVoteForFederationTypeAction` 的实测形状（从 UI 现场抓到，`propose_capture`）：

```
+0x00 vtable RVA 0x23B7548          +0x08 基类 vtable RVA 0x2388BA0（与法律表决同一个基类）
+0x20 发起国（u32） / +0x24 收件方
+0x40 动作类型对象（键 action_vote_for_federation_type）
+0x50 CFederationType*（键在 +0x20，实测 hegemony_federation）
+0x58 名字（UI 写入的是模块内对象 base+0x234A778）
```

**手搓这条会崩，两次都没有 crash dump**：

| 版本 | 构造 | 结果 |
|---|---|---|
| ① | `EngineAlloc(0x60)` + `+0x58` 指向模块静态对象 | IsValid **失败**路径就崩 |
| ② | `EngineAlloc(0x80)` + `+0x58 = 0` | 换「贸易联盟」（IsValid 失败）**安然无恙**；换「霸权」（IsValid **通过**）**又崩** |

结论：这不是单纯的二次释放——**IsValid 通过之后引擎还要用 name 等字段**，而手搓对象里那些字段不完整。
所以 `propose_federation_type` 现在**直接返回错误**（不再构造对象），换类型请走界面；
读取（`get_federations` / `vote_diplo_action`）与投票不受影响。
将来若真要接，唯一稳妥的做法是**把 UI 那份动作连同 name 逐字节复制**，并先确认对象实际尺寸。

### 联邦 flags：四层扫描没找到（2026-11-21，未解决）

- 目标：`has_federation_flag` 读的那份状态。存档里是明文（`flags={ 名字=日期 }`），内存里没找到
- 做法：从 `CFederation`（0x280 全量）与 `progression`（0x400）出发逐层解指针，四层都试过：
  ① qword 直接当字符指针；② qword 当 PdxString 读 `+0` 再当字符指针；
  ③ qword 当节点读 0x40，逐个 qword 当字符指针（346 个候选）
- 结果：只读到 `research_federation`（类型键）与法律 / 舰船设计 / 决议 / 居住区的键，**旗标名一个都没有**
- 判断：旗标大概不以字符串形式存在这两个对象里——要么是按脚本旗标 id 的位图 / 集合，要么挂在全局旗标表。
  下一步两条：① 找脚本旗标注册表（名字→id）再解位图；② 序列化 hook 抓 `CFederation`，在存盘那一刻读 `flags`
- 影响面：只有「第 5 个旗标 `no_federation_fleet_build_right_cooldown`」的 10 天豁免预测读不到，
  插件对那一档写「判不了」，其余 4 个旗标都能从「类型 + 当前法律」推出来

**两个副产物**

1. **`progression +0x2D0` 是「最近表决 / 变动」的历史数组**，不是一对：现在挂着 5 个 law 指针，
   步长 0x10（`+0x00/+0x10/+0x20/+0x30/+0x40`）。2026-11-21 之前记的「一对」是只有两次记录时的样子
2. **扫描脚本必须用单连接、顺序发请求**：一个地址连一次的写法，几百次之后把桥上命名管道实例打光，
后续所有调用直接 `ENOENT \\.\pipe\stellaris_mcp_bridge`（游戏仍在跑）。改成单连接、顺序发请求的版本才稳（脚本未随仓库发布）

### 表决动作（vtable 一览）

| 表决 | vtable RVA | 关键字段 |
|---|---|---|
| 求和 `COfferPeaceAction` | `0x23B6B20` | `+0x58..+0x64` actor/recipient/war/status |
| 宣战 `CDeclareWarAction` | `0x23B9EB0` | `+0x50` 内嵌 CWarGoal，目标类型 `+0x58` |
| 踢出成员 | `0x23B73A0` | `+0x24` 被踢国家 |
| 联邦法律 `CVoteForFederationLawAction` | `0x23B7050` | `+0x50` 法律对象（键在对象 `+0x20`） |
| 联邦类型 `CVoteForFederationTypeAction` | `0x23B7548` | `+0x50` 类型对象（键在对象 `+0x20`） |

- **类型键别读 `+0x28`**：那是所有类型共用的 `.rdata` 指针，只能读出一个 `"H"`——2026-11-21 就因此误判过
- 库：`CFederationTypeDatabase 0x3119538`、`CFederationLawDatabase 0x3119638`、
  `CFederationLawCategoryDatabase 0x3119640`、`CFederationPerkDatabase 0x3119540`；
  法律对象 vtable RVA `0x2388C28`，类型对象 `0x2388990`

### 录制工具（2026-11-21 改进）

- `propose_capture`：抓 0x200 字节，回 `qwords`（8 字节对齐表）与 **`law_hits`**
  （逐字节偏移找指向 `CFederationLaw` 的指针，回 `{offset, obj, key}`）——`law` 的 `+0x50` 就是这么定下来的
- `cmd_capture`：除投票/应答外，**任何** Post 过的命令都进 `any`（vtable RVA + 头 0x40 字节）

## `probe_community_membership`

**目的**：`CCountry` 上没有共同体字段（146 个序列化字段里只有 `federation +0x1C38` /
`associated_federation +0x1C3C` / `overlord` / `subjects`），脚本里的 `is_galactic_community_member`
又是引擎内建触发器（`common/` 里只有使用没有定义），所以想「问引擎」：造
`leave_community_command` / `join_community_command` 只跑 IsValid，看能不能反推成员身份。

**做法**：`probe_community_membership { country_ids: [...] }` → 对每个 id，两种引用编码
（裸 id / `0x01000000|id`）各造 leave 与 join，各跑一次 IsValid，理由原样带回。

**实测（2322.07.01 档，共同体 13 国）**

```
id 0 （我方，监管人）  bare   join ✓  leave ✗「监管人不能脱离星海共同体」
                       tagged join ✗  leave ✓
id 33（成员）          bare   join ✓  leave ✓
id 23（肃正协议，非成员） bare   join ✗  leave ✓
id 5 （冠名熔炉，非成员） bare   join ✗  leave ✓
```

**结论：判不了。** 已知非成员的 leave 也是 ✓，只有我方被拒；而 join 在成员身上还能是 ✓。
这两条命令的 IsValid 判的是「**我方能不能发这条命令**」（监管人不能脱离、已加入的不能再加之类），
不是「那个国家在不在名单里」。payload 里的 `country` 确实传进去了（我方那条理由就是按它给的），
但它不构成成员判据。

要成员名单，仍只有 `galactic_community.members`：现在读存档，拿到实例后读 `CGalacticCommunity +0x180`。
这个探针留着有用——它是「我方能不能退共同体」「能不能拉人」的现成判据。

## 星海共同体：逆向要点

### 对象与实例

```
CGalacticCommunity（serializer RVA 0x6249E0）
  +0x08 community_formed ｜ +0x0C council_established ｜ +0x178 leader
  +0x180 members ｜ +0x198 council ｜ +0x1B0 permanent_council
  +0x1C8 proposed ｜ +0x1E0 passed ｜ +0x1F8 failed
  +0x2D0 voting ｜ +0x2D4 last ｜ +0x2D8 vote_length ｜ +0x2DC days（已过）
  +0x2E0 cooldown ｜ +0x2E4 election ｜ +0x2E8 custodian_term（剩余天数）｜ +0x2EC council_positions
  +0x2F8 imperial_authority（u64，0 = 没有帝皇）
```

**没有库表、没有 RTTI、扫描也找不到**，所以读它靠 hook serializer：

- `MH_CreateHook(base + 0x6249E0, detour)`；detour 只记 RCX（= this）再调原函数，**不碰对象**
- **存档也走 serializer**，所以不必手动读档：`install_community_capture` → `save_game` → `read_community_capture`
- 重注入 DLL 后指针丢（要重挂 + 再存一次档）；**读档会让它自己更新**（载档时 serializer 又跑一次）
- 找实例失败的路子（别重试）：全局表里没有 `TPdxRef<CGalacticCommunity>`；exe 里没有
  `.?AVCGalacticCommunity@@` 的 RTTI；按 `(vote_length, days)`、`(voting,last)=(23,22)`、成员对
  `(33,51)` 等签名扫了约 2 GB 堆，命中的都是别的列表或本插件自己的 JSON 缓冲
  （`scan_memory_for_u64_range` 是**半开区间**，`lo == hi` 永远 0 命中，踩过一次）

**ref_array 的形状（量了半天）**：`{哨兵 +0x00, 数据 +0x08, **cap +0x10, size +0x14**}`——
**cap 在前、size 在后**。先按 +0x10 当 size，`proposed` 多吐了 3 条陈旧值（8750/20000/2000）；
成员/议会两组 cap == size，所以看不出来。条目是 **4 字节引用**（不是指针），值与次序都跟存档一致。

### 议案

```
TPdxRef<CResolution>::_pDatabase 0x311BE08 → db
  db+0x18 slots ／ db+0x20 cap（本档 0x400）；16 字节一条，对象在 +8 —— 与联邦那张表同一套
CResolution  +0x08 id ｜ +0x18 类型对象（**键在 +0x20**）｜ +0x20 提案国 ｜ +0x24 目标
             +0x28 freeze_date ｜ +0x2C pass_date ｜ +0x30 supporters ｜ +0x48 opponents
```

**日期是 u32**：`值 = (年*360 + (月-1)*30 + (日-1)) * 24 + 43800000`——用 2269.01.01、
2274.12.30、2286.12.30、2319.02.01 四个已知值验到完全吻合。

### 类型库

`CResolutionTypeDatabase`（`0x3119690`）→ 逐个类型对象，键在 **`+0x20`**（`CResolutionType`
在 sdk.json 里没有独立实体，偏移是从用法量的）。`CResolutionCategoryDatabase 0x31196B8`、
`GroupDatabase 0x31196C0` 两个库只用过地址，类别名与 type 的归属是靠脚本
`common/resolution_categories/*.txt` 烘的（见插件端 `scripts/resolution_index.py`）。

### 命令表（`tools/sdk_dumper/out/sdk.json`）

| 命令 | token | vtable | 关键字段 |
|---|---|---|---|
| `propose_resolution_command` | `0x3D7A` | `0x23C6990` | `type +0x20`（指向类型对象）/ `country +0x28` / `target +0x2C` |
| `support_proposed_resolution_command` | `0x3D9F` | `0x23C65F0` | `resolution +0x20` / `country +0x24` / `force +0x28` |
| `oppose_proposed_resolution_command` | `0x3DB1` | `0x23C6538` | 同上 |
| `remove_support_for_proposed_resolution_command` | `0x3DAB` | `0x23C6480` | 同上 |
| `cast_vote_command` | `0x3D9E` | `0x23C66A8` | `resolution +0x20` / `country +0x24` / `vote +0x28` / `force +0x2C` |
| `withdraw_proposed_resolution_command` | `0x3DB6` | `0x23C60E8` | `resolution +0x20` / `country +0x24` |
| `veto_resolution_command` | `0x3DB0` | `0x23C6258` | 同上 |
| `freeze_proposed_resolution_command` | `0x3EC9` | `0x23C6030` | 同上 |
| `propose_emergency_measure_command` | `0x3DB5` | `0x23C61A0` | 同上 |
| `end_senate_session_command` | `0x3E79` | `0x23C5F78` | 同上 |
| `pass_resolution_command` / `pass_resolution_type_command` / `fail_resolution_command` | `0x3D7B` / `0x3D7C` / `0x3D7D` | `0x23C68D8` / `0x23C6818` / `0x23C6760` | 结算用 |
| `join_community_command` / `leave_community_command` | `0x3DA2` / `0x3DA3` | `0x23C63C8` / `0x23C6310` | `country +0x20`（join 另有 `has_galactic_emperor +0x24`） |
| `borrow_diplo_weight_for_resolution_command` | `0x2B35` | `0x2398D80` | `resolution +0x20` / `country +0x24` |

### 两层表态别混

```
队列中：support / oppose / remove_support（没有冷却）
台上：  cast_vote（有 120 天改票冷却，按议案计）
```

- **议案一旦上台，队列那三条就用不了**：「你不能支持一项尚未被提出的议案」
- **IsValid 只判 `allow`，不判 `potential`**：`propose_resolution_command` 对
  `resolution_emperor_*`（potential 要帝皇）也回 ✓。plugin 端用烘好的标记 + 实时
  `custodian_term` / `imperial_authority` 过滤
- `potential` 里的 `is_active_resolution = "X"`（「废除 X」那类）同样要自己筛；**极性也要认**
  （`has_galactic_emperor = no` 是「没有帝皇时才存在」，按字面出现判会整反，踩过）
- 同一个国家同时只能挂一条议案：第二条会回「…已经有一项表决中的议案」

### 实测出来的取值与冷却

- **`cast_vote` 的 vote：0 = 赞成、1 = 反对、2 = 弃权**——与联邦表决
  `CDiploActionVoteCommand`（1 = yes、0 = no）**正好相反**。测法：投 1 使我方进入反对名单；
  投 2 使我方退出支持名单又没进反对名单
- **改票冷却 120 天**（`CHANGE_VOTE_COOLDOWN`），**按议案计**：没投过的议案随时能投
- **`veto`**：移出队列，并让该议案进入 **5040 天**的提交冷却（`CATEGORY_VOTE_COOLDOWN` 2520 的两倍），
  对所有国家生效
- **`freeze`**：1440 天不能进参议院（`COMMUNITY_FREEZE_RESOLUTION_MONTHS = 48` 个月）
- **`end_senate_session`**：要会议时间过半，按**基础 `RESOLUTION_VOTE_TIMER` 1440 的一半 = 720 天**算，
  不随 `vote_length` 被压短而变；投出后当场结算（`voting` → -1，议案进 `failed`）
- **`emergency`**：绕过队列与休会，当场把该条放上台并重开会期（`days` 归 0）

### 否证（别重复试）

- **「某个国家在不在共同体」判不了**：`CCountry` 的 146 个序列化字段里没有共同体字段；
  `is_galactic_community_member` 是内建触发器；对每个国家造 `leave_community_command` /
  `join_community_command` 跑 IsValid 也不行——已知非成员（23 肃正行动、5 冠名熔炉）的 leave 照样 ✓，
  这两条命令判的是「我方能不能发」。成员只有 `galactic_community.members` 一处来源
- 上游 `stellaris-mcp-main` 全树搜 `resolution` / `community`：只有使节任务名，没有任何实现

### 相关桥方法

`get_resolutions`（议案全表，带解码日期）、`probe_resolution_types`（逐类型造 propose 命令跑 IsValid）、
`propose_resolution`、`vote_resolution`、`end_senate_session`、`queued_resolution_action`、
`install_community_capture` / `read_community_capture`、`probe_community_membership`（否证用）。

## `get_contacts` 加附庸三字段

`CCountry` 上三个字段，都与存档逐项对上过：

```
+0x2630 overlord       u32 带标记引用 0x01000000|id，0xFFFFFFFF = 没有宗主
+0x2634 subject_date   u32 打包日期（与 CResolution 同一个公式，用 DecodeGameDate 解）
+0x2640 loyalty        u64 定点 ×1e5（存档 -100 → 内存 -10000000）
```

- 实测：country 8（raw 16777224）→ `0x2630 = 0x0100000A`（宗主 10）；释放附庸之后
  country 1 的 `0x2630` 由 `0x00000000` 变 `0xFFFFFFFF`
- **`CCountry +0x2670 subjects` 是空的**：country 8 有宗主、country 10 有附庸，两国读出来都是
  `{哨兵, 0, 0, 0}`。所以「谁的宗主」只能从「谁是谁的附庸」反推，别指望这个 ref_array
- **大坑：`probe_country` 的 offset 上限是 `0x2000`**，超过就**静默返回对象头**（不报错）。
  offset 8192 还读得对、8240 就回头部——0x2630 以上这些字段一直标着「未核对」就是它造成的。
  读这一带必须用 `read_memory`
- `get_contacts` 另给一块 `self`（这张表本来不含自己），字段与 empire 行相同：
  `is_subject / overlord_id / overlord_name / loyalty / subject_date`
- 附庸**类型**（保护国 / 学邦 / 采邑）不在 `CCountry` 里，在协议 `CAgreement` 的
  `agreement_preset`（+0x70 的字符串）——读它要 hook 序列化器，见下一节

## 附庸协议：hook 抓取与字段

**做法与 `CGalacticCommunity` 同一套**：hook `CAgreement` 序列化器（RVA `0x7AA310`），detour 只记
`RCX`（最多 64 条、去重），再存一次档触发序列化（读档也会走一遍）。桥方法两枚：
`install_agreement_capture`、`get_agreements`。

**抓到的东西不是 CAgreement 本体，而是它内部被序列化的那个子对象**（本体 +0x10）。
所以下面这些偏移都是**相对 detour 收到的 `this`**：

```
+0x00 vtable 0x…940170    +0x08 owner ref   +0x0C target ref
+0x10 date_added          +0x14 date_changed  +0x18 active_status
+0x20 …（一段指针区）
+0xC8 agreement_preset → CAgreementPreset，键是它 +0x20 的 PdxString
+0xF0 forced_initial_loyalty  i64 ×1e5（preset_vassal_oppressive_ai = -10000000 → -100）
```

- 怎么发现的：先按 sdk.json 的 `CAgreement.owner +0x18` 读，全是 0；把抓到的对象 dump 出来才看出
  整块字段比本体**提前 0x10**——本体 +0x18 正好是 this +0x08
- **本体布局**（sdk.json，实测对齐）：`+0x18 owner`、`+0x1C target`、`+0x20 date_added`、
  `+0x24 date_changed`、`+0x28 active_status`、`+0x68 term_data`、`+0x108 flags`、`+0x158 variables`；
  vtable RVA `0x23401A8`
- **预设键的读法**：`+0xC8` 指向 `CAgreementPreset`，键在它 `+0x20`。短的（`preset_vassal`，13 字符）
  走 PdxString 的 SSO、就地；长的（`preset_vassal_oppressive_ai`）在堆上——**必须用 `SafeReadPdxString`**，
  直接按 C 字符串读长的那份会得到空
- 实测两条：`10→8 preset_vassal_oppressive_ai / forced -100 / 2262.07.07`、
  `0→1 preset_vassal / forced 0 / 2281.01.02`
- **`preset_vassal_oppressive_ai` 的本地化就是「附庸」**（`$preset_vassal$`）——界面上看不出区别，
  别把「两个都是附庸」当成读错
- **补贴（resource_terms）已解**：协议对象里两套 26 格数组，数据指针在 `+0x90` / `+0xB0`，
  条数在 `+0x98` / `+0xB8` 的低 32 位（两套都 26；哪一套是「现值」还没定，读的时候两套取逐格最大值）。
  每格是 u64 定点 ×1e5，按资源分格——实测：格 0–2 = 基础（能源/矿物/食物）、3–5 = 研究（三系）、
  9–10 = 进阶（消费品/合金）。`6–8` 战略、`11` 凝聚力 未单独验
  - 两次对照：只开基础 15% 时格 0/1/2 各 15000；改成进阶 15% + 研究 30% 之后格 9/10 = 15000、
    格 3/4/5 = 30000
  - **存档里的符号是反的**：宗主给附庸补贴，存档写 `resource_subsidies_advanced=-0.15`、
    `research=-0.3`（内存里是正的 15000 / 30000）；别拿存档的正负去对内存
- **离散条款也解了**（2026-11-21）：`+0x60` 是一个数组，元素 `0x10` 字节、`[key_ptr, value_ptr]` 交替，
  条数在 `+0x68` 的高 32 位（低 32 是容量）。两个指针都指向「键在 `+0x20` 的 PdxString」的对象。
  实测 9 条全读出：`subject_integration / joins_overlord_wars / joins_subject_wars / subject_diplomacy /
  subject_expand / protectorate / subject_loyalty / subject_holdings_limit / subject_sensors`，
  值形如 `subject_can_not_be_integrated`、`joins_overlord_wars_offensive`、`subject_holdings_limit_4`
- **资源条款**：`+0x78` 是同样 0x10 步长的数组，`[key_ptr, i64 定点 ×1e5]`，条数在 `+0x80` 高 32 位（5 条）。
  值是**负数**（宗主给附庸）：`basic -15000`、`strategic -30000`
- 补贴槽位修正（2026-11-21 第三次对照）：**战略 = 格 11–13**（不是 6–8）；`0–2` 基础、`3–5` 研究、
  `9–10` 进阶（消费品/合金）、`14` 大概凝聚力（未验）
- 现场留档：`CAgreement` 对象在池里按 0x180 步长排（空槽 vtable 为 0），没有全局协议库
  （`sdk.json` 里没有 `TPdxRef<CAgreement>::_pDatabase`），所以枚举只能靠这个 hook，别去扫内存



## 护栏：协议条款写入

**事故**：给 `propose_diplomatic_action` 加上「带条款发送」之后，第一次真投（`action_demand_subjugation`
带 terms/subsidies）游戏**无声消失**——`running: no`、`crashes\` 没有新 dump（最后一份还是 11-18）、
`time.log` 停在 09:28 的 `Recalc cached data`、`game.log` 无异常。属于本文件开头那套「静默崩溃」。

**嫌疑（未复现）**：`PatchAgreementTerms` 当时直接按 `action+0xC8`（离散条款）/`action+0xE0`（资源条款）
写内存，**没有任何校验**。两种可能都致命：

1. 生产者造出来的动作若是别的类（或未初始化），那两个偏移不是条款数组，写进去就是踩坏堆
2. 条款数组可能与预设共享（`.rdata` / 引擎只读池），写只读页直接访问违例

**已加的护栏（都落在 `contacts_manager.cpp`）**：

- `PatchAgreementTerms` 先验动作的 vtable RVA ∈ {`0x23CCCF8`（要求臣服）, `0x23CC440`（CProposeAgreementAction）}，
  不是就拒绝
- 条款数组的「容量 / 条数」必须自洽（离散 1–64、资源 1–16，容量 ≥ 条数且 ≤ 256 / 64）
- 写之前一律过 `WritableRange()`（`VirtualQuery` 看 `MEM_COMMIT` + 可写页 + 非 `PAGE_GUARD`），只读页不写
- 新增 `dry_run`：造动作 → 写条款 → **回读**（把 14 条与 5 条读出来返回）→ 删除动作，**不投递**。
  以后改条款先 dry_run 看回读，确认无误再真投

**教训**：写引擎对象和读一样，都要「先确认这块内存是什么」。读错只是拿到垃圾；写错是进程直接消失，
连 dump 都没有——`WritableRange` 这类前置检查不是保险，是必需品。


## 附庸协议：**带条款发送**打通

命令没变，仍是 `diplo_action_propose_command`（0x310B）+ 协议类动作；**难点在动作里的协议是空壳**，
要自己把它建出来再填条款。整条链（全部实测）：

```
① NewAction(生产者)                        造出动作（vtable 0x23CCCF8 / 0x23CC440），协议区全 0
② CAgreement 构造器   RVA 0x1E1A00         在 action+0x58 上构造协议
        （写两个 vtable、owner/target=-1、日期=纪元 0x029C55C0、并在 +0x68 调 term data 构造器 0x7A9280）
③ 设条款   RVA 0x7A9740(container, key, value)
        离散条款：container = term_data+0x00（vtable+0x00/data+0x08/cap+0x10/size+0x14）
                  key 取自 CAgreementTermDatabase 0x3119758，value 取自 CAgreementTermValueDatabase 0x3119750
        资源条款：container = term_data+0x18，key 取自**条款值库**（resource_subsidies_* 不在条款库里），
                  value 直接给 i64 定点 ×1e5（宗主给附庸为负数）
④ 写 owner/target：action+0x70 / +0x74（裸 id）
⑤ 包 CDiploActionProposeCommand → IsValid → Post
```

- **term data 在 action+0xC0**，vtable RVA 是 `0x2304418`（不是 `0x2340418`——这个数差一位会误判成「没构造」）
- 引擎校验照旧生效：对 70 号「要求臣服」被拒「需要关系极佳或50信任度 / 需要建立大使馆…」，说明 IsValid 真跑了
- **实测成功**：对 country 1 发「请求成为附庸」+ 6 条自定义条款 + 基础 10% / 研究 20% 补贴，
  从对方 `incoming_actions` 把动作读回来，6 条条款与 2 条补贴逐条一致；随后由 1 号拒绝，状态干净
- 全程用 `CallGuarded` 包住对引擎函数的调用（SEH），失败只会返回错误，不会带走进程
- 之前踩过的两个死法：**手动删外交模块拥有的动作**（二次释放）与**按固定偏移直写条款数组**——
  后者这次整段废弃，改成调 ③ 那个官方 setter


## 收件箱（incoming_actions）里的动作类对照

```
CCountry +0x1950 incoming_actions：{data +0x00, (cap<<32|size) +0x08}
元素步长 0x10：{type_desc +0x00, action_ptr +0x08}     ← 不是 proposed_actions 那个 0x30
```
动作类（vtable RVA）逐个用真提案验过：

| vtable RVA | 动作类 | 双方字段 |
|---|---|---|
| 0x23B6B20 | `COfferPeaceAction` 求和 | actor +0x20 ｜ recipient 在 +0x58 那组的 `+0x5C` |
| 0x23CCCF8 | 要求臣服（协议类） | **内嵌协议**：+0x70 owner / +0x74 target |
| 0x23CC440 | `CProposeAgreementAction`（协议类） | 同上 |
| 0x23B8FC8 | 商业条约 | actor +0x20 / recipient +0x24 |
| 0x23B8928 | 驿站条约 | 同上 |
| 0x23B8C78 | 互不侵犯条约 | 同上 |
| 0x23B8288 | 防御同盟 | 同上 |
| 0x23B9318 | 研究协定 | 同上 |
| 0x23B4158 | 建立大使馆 | 同上 |
| 0x23B94C0 | 保证独立 | 同上 |

- **条约类的 `+0x58..+0x64` 是求和动作专用的四个字段**，套到条约上读出来是垃圾——别拿它当 recipient
- 协议类的双方只在 `+0x70/+0x74`（内嵌协议）里，`+0x20` 的 actor 对它们不可靠
- **大使馆是一对**：`action_embassy` 只在**还没有大使馆**时可选；已经有的时候工具里只剩 `action_recall_embassy`（召回大使，
  `auto_accepted = yes`，当场生效）。2026-11-21 实测：对 country 1 先发 `recall_embassy`，`action_embassy` 立刻从不
  「不可发」变成可发；再发一条 embassy 并接受，召回按钮回来、建立按钮消失


## 附庸关系：真的挂上之后

- 用桥把「请求成为附庸」发给 1 号、由 1 号接受 → **关系成立**：`stellaris_contacts` 顶部变成
  `我们：灵能魔女 的附庸`，`CCountry +0x2630` 的 overlord 指向 1
- **引擎在接受时会把方向翻过来**：我方发出的提案里写的是 `owner=0（发起方）→ target=1`，
  接受后落成的协议是 `owner=1（宗主）→ target=0（附庸）`。**所以发送时不需要交换参数**，这一点实测钉死
- 落成的协议是「自定义、无预设」（`agreement_preset` 空），条款 = 我方填的 6 条 + 引擎补齐的隐藏项（共 14 条）
- **成为附庸后新增的动作**：`action_ask_to_be_released_subject`（请求解除附庸，vtable `0x23CC7C8`），
  发送方式与普通动作一样（无条款），落在**宗主国的收件箱**；宗主接受即解除（脚本另附 10 年停战）
- 反向那条（宗主释放附庸）是 `action_release_subject`，`auto_accepted = yes`，投出即生效
- 附注：`get_agreements` 里会看到一些 `active_status = 3` 的「幽灵协议」——那是之前测试里被拒/被替换的残留对象，
  只有 `active_status = 0` 的才是当前生效的


## 外交章逆向索引

插件 README 的外交章只说「怎么用」，凡是偏移 / vtable / 构造器 / 试错记录都归这里，按主题对应：

| 想查 | 看这一节 |
|---|---|
| 谁附庸谁、忠诚与起始日 | 「`get_contacts` 加附庸三字段」 |
| 协议对象布局、hook、预设/条款/补贴的读法 | 「附庸协议：hook 抓取与字段」 |
| 带条款发送附庸协议（构造器 + setter + owner/target） | 「附庸协议：**带条款发送**打通」 |
| 真的挂上之后引擎怎么翻方向、请求解除附庸 | 「附庸关系：真的挂上之后」 |
| 收件箱各动作类的 vtable、元素步长、空槽 | 「收件箱（incoming_actions）里的动作类对照」 |
| 求和提案与应答命令、联邦内怎么发 | 「程序化求和提案「自己发」的那条路」「联邦里怎么发」 |
| 宣战动作与战争目标 | 「守方战争目标：`set_defender_war_goal`」与「战争与联邦：逆向要点」 |
| 联邦对象、法律、类型 | 「战争与联邦：逆向要点」「手工造 `CVoteForFederationLawAction`，发起联邦法律表决」 |
| 星海共同体 | 「星海共同体：逆向要点」「`probe_community_membership`」 |
| 静默崩溃与护栏 | 「静默崩溃：怎么认、怎么防」「护栏：协议条款写入」 |


## 特使（envoy）：对象层

特使**也是 `CLeader`**，只是位置落在「外交槽」上；不能雇、不能解雇，数量由帝国修正决定，只能挪。

```
CLeader +0x110  location           16 字节，一块
        +0x120  council_location   同上（议席；特使不用）
location:
    +0x0  type           4 = empire（帝国使者）｜5 = galactic_community｜6 = federation｜8 = first_contact
                         （0 = 无位置；派上首触之后整字读到 0x13000008，高字节含义未查）
    +0x4  assignment     EEnvoyAssignment 下标（任务）
    +0x8  id             目标 id —— empire 是国家 id、first_contact 是**接触记录 id**、federation/共同体填 0
    +0xC  position       含义未查（实测 0）
```

**EEnvoyAssignment 下标**（2026-11-21 补全）：`0` improve_relations ｜ `1` harm_relations ｜
`2` galactic_community ｜ `3` federation ｜ `4` first_contact ｜ `5` strengthen_imperial_authority ｜
`6` undermine_imperial_authority ｜ `7` spy_network ｜ `8` patron_attunement。
（`2` / `3` 这次实测：命令能过、位置真的写上；`5` / `6` 仍缺样本——要**星海帝皇**存在才有这两个任务。）

**⚠ 职业与位置不是一回事（2026-11-21 玩家指出，此前这里写混了）**

| 位置 type | 谁坐 |
|---|---|
| 4 empire（改善 / 损害关系、间谍网、虚境调和） | **特使 envoy** |
| 5 galactic_community、6 federation | **行政官 official** |
| 8 first_contact | 特使 envoy |

拿 envoy 类去派共同体：命令 `IsValid` 与 `Post` 都「成功」，但引擎**静默拒绝**（人没动、也不给 reason）；
换行政官一次就成。本档铁证：三页筏舒克（official）= 共同体、索索毛拉（official）= 联邦代表。
所以插件层不能只按 `slot == "envoy"` 放行——**要按任务查职业**（共同体/联邦 → official，其余 → envoy）。

**指派命令**：`CAssignLeaderCommand`（token `0x40AD`、size `0x38`：`leader +0x20`、`location +0x24`、
`reset_cooldown +0x34`）。三个坑：

- **`leader` 要带高位标记的引用**（本档特使 62 是 `0x0900003E`）。只传裸 id 时 IsValid 与 Post 都成功，
  但结果是**把人解绑、没派到新位置**
- **IsValid 不校验职业**：科学官也能过；行政官被拒时 `reason` 是空串。所以插件层自己拦 `class != envoy`
- **360 天冷却只对特使（envoy）**：刚挪过的特使会回「time 360内无法移动」；行政官没有这限制
  （2026-11-21 实测：三页筏舒克 共同体→联邦→共同体 连挪两次都立即生效）。
  `reset_cooldown` 写 `+0x34 = 1` **并没有**绕过特使那条冷却，
  那条限制另有来源，未查

`probe_leader { leader_id }` 能把上面两块 location 原样打出来，是最快的只读检查。


## 贸易协定：`propose_trade_deal`

`action_offer_trade_deal` 与附庸协议**不是一套东西**：脚本里 `action_type = neutral`，payload 是一个
`CTradeDeal`（两侧资源清单），不是 `CAgreement`。空交易会被引擎拒（「交易不能为空！」）。

**生产者能造，而且把载荷都分配好了**——这是整条路的关键（`probe_diplomatic_action` 回 `created: true`）：

```
CAbstractTradeDealAction  +0x20 actor ｜ +0x24 recipient
                          +0x50 内嵌 CTradeDeal（SDK 标 ref<CTradeDeal>，实际按值；不是指针！）
CTradeDeal   +0x18 first ｜ +0x130 second ｜ +0x248 killed ｜ +0x249 length（**年**）｜ +0x250 date
CTradeDealSide  +0x8 country ｜ +0xC recipient ｜ +0xC0 resources ｜ +0xE0 monthly_resources ｜ +0x106 reserve
    （+0x10 system / +0x28 leaders / +0x40 fleets / +0x70 join_war / +0x88 leave_war 是「顺带项」，本次没用）
每张资源表：+0x00 数据指针 ｜ +0x08 {size=26, cap=26}
    数据 = 26 格 i64 定点（×1e5），**索引 = 资源声明序**：energy=0 / minerals=1 / food=2 /
    research×3 / influence=6 / unity=7 / trade=8 / consumer_goods=9 / alloys=10 / … / entropy_crystals=25
```

**dry_run 实测（2026-11-21）**：生产者造出来的动作里，两侧 `country/recipient` 已经是对的
（0→1、1→0），四张表全是 `size=26 / cap=26`、内容全 0。所以**构造只剩写数值**，不需要分配、不需要构造器，
也不需要复制模板——与附庸协议那次（要自己初始化 term data）不同。

**踩出来的三条**

1. **`length` 的单位是「年」**：界面选 10 年，`+0x249` 读到 `0x0A`。SDK 注释写的是 i32，实际落点在
   `killed(+0x248)` 之后一个字节——按 i32 写 `+0x249` 与实测字节一致
2. **`reserve` 只在发起方那一侧**：UI 递交的那份是 `first.reserve=yes`、second 没有这一行。
   含义（玩家说明）：**给出的资源递交时立即扣除、被拒退回**——不是「保底」也不是「月供预付」。
   最初一版按「谁给东西谁置位」写，结果两侧都给时两边都置了，与 UI 不符；已改成只写 first
3. **重复递交不拦**：同一对国家对、同样的内容连投三次，引擎照收，收件方 `incoming_actions` 里就是三条
   （tag 886/887/888），拒掉才消失。所以工具层要自己防重

**验证**：`alloys 150 + 25/月` 换 `energy 250 + 30/月`、10 年 → 存档里逐条对上
（`first.resources.alloys=150`、`first.monthly.alloys=25`、`second.resources.energy=250`、
`second.monthly.energy=30`、`length=10`、`reserve=yes` 只在 first）。

**通信同步 / 主动传感链接：发起方那侧的两个字节**（2026-11-21 三份样本对位确认）

```
first side +0x104 = sensor_charts（通信同步）      ← 样本 888 只有它为 1
first side +0x105 = sensor_link（主动传感链接）    ← 样本 890 只有它为 1
first side +0x106 = reserve
（三个字节紧挨着：三份样本在动作 +0x168 那个 qword 里依次是
  00 00 00 00 [104] [105] [106] 00，差异只落在这三个字节上）
```

桥里由 `propose_trade_deal` 的两个参数写入（`sensor_charts` / `sensor_link`，只写 first 侧）。
实测两个方向都验过：只开 charts → `+0x104=1`；只开 link → `+0x105=1`；
插件层是 `stellaris_trade_deal { …, sensor_charts: true, sensor_link: true }`。

**标本：已打通（2026-11-21 实测，含三元素样本 + 端到端转移）**

```
first side +0x60 = 数据指针（8 字节一个的 CSpecimen 指针数组）
first side +0x68 = cap（低 32）｜first side +0x6C = size（高 32 那半）
元素：指向 CSpecimen 对象（vtable RVA 0x2382708），键在对象 +0x20、库内下标在 +0x08
      （cenotaph_display=31 / chambered_clay_pot=34 / ancient_kinetic_launcher=12，
       与 common/specimens/specimens.txt 的声明序一致）
数组顺序 = 界面上的顺序
```

**写入方式**：按 key 从 `TGameDatabase<CSpecimenDatabase>::_pInstance = 0x3119500` 查对象（遍历 `ScriptTypes`、
读 `+0x20` 的 PdxString），`CommandBuilder::Get().EngineAlloc(N*8)` 分配指针数组，填指针，
再把 `+0x60/+0x68/+0x6C` 写进 first 侧。**EngineAlloc 与容器析构的分配器一致**——实测接受、结算、
后续存档读回都没崩，标本也确实转移了。

**端到端验证（2026-11-21）**：桥发「1 食物 + `loopmail`」给 country 1 → 用 `stellaris_diplo_inbox` 接受
→ `stellaris-save/specimen_report.py` 显示 country 1 的档案里多了 `loopmail`，country 0 那边少了。
（同一批还验了界面上递交的三标本提案：`cenotaph_display` / `chambered_clay_pot` / `ancient_kinetic_launcher`
三件全部到了接收方。）

**三条要记的**

1. **标本是即时交割**：不留 `monthly_resources` 的交易在接受后**不会生成 `trade_deal` 记录**——
   别拿「`trade_deal={}` 为空」当失败，要去看持有者的档案
2. **`owner=` 是发现者，不是持有者**：查持有者要看各国 `standard_grand_archive_module`
   （`exhibits` 按类别 + `storage_slots`）引用的实例 id。用 `specimen_report.py` 一条命令就能读
3. **同一份标本不能同时在两个待答交易里**：重复投同一样东西时引擎会回
   「…出售了一个在交易中提出的标本」（IsValid 拒），等前面那份结掉再发

**借出舰队：发起方那侧 +0x40（有期限，不是即时交割）**（2026-11-21 用「破浪者 / 飓风」样本对位）

```
动作 +0xA8（= 侧 +0x40）  容器 vtable（引擎填，0x23045F0）
动作 +0xB0（= 侧 +0x48）  数据指针
动作 +0xB8（= 侧 +0x50）  {cap(低 32), size(高 32)}  → 实测 {2,2}
元素：**4 字节的 fleet id**（不是指针！与标本不同）—— 数据区读出来就是 16778563 / 16779394，
      与 get_military_fleets 的 fleet_id 完全一致，顺序 = 界面顺序
```

写入用 `EngineAlloc(N*4)` 填 u32 id，再写 `+0x48` 与 `+0x50`。**协议是有期限的**：`length` 就是年限，
接受后生成 `trade_deal` 记录（和纯资源那份一样），到期自动结束——与标本的「即时交割、不留记录」相反。

实测（2026-11-21）：给 country 70（我方的附属国）发 `fleets: [16778563, 16779394]`、10 年 → 接受后存档：

```
trade_deal = { 0 = { first = { country=0 recipient=50331718 fleets = { 16778563 16779394 } }
                     second = { country=50331718 recipient=0 }
                     length=10 date="2322.07.01" } }
```

**借出期间：`owner` 不变、`controller` 变成借用国**（玩家 2026-11-21 在界面上确认）。舰队对象里
`owner` 与 `controller` 是两个字段，桥里早就在用 `fleet + 0x448` 读 `controller`（围城判定那块），
所以「借给谁了」= 看 `+0x448`：本档破浪者/飓风的 `controller` 已经是 `50331718`（country 70）。
**owner 在 `+0x44C`**（不是 `+0x448` 前一个 4 字节——那是别的字段；2026-11-21 用「country 70 自有舰队
owner 与 controller 都该是 70」这条对照才验出来）。
注意 `get_military_fleets` 只列**玩家自己 owner 的**舰队，借来的舰队不会出现在借用国的这个列表里——
别拿列表空当成没借到，要看 `controller`（或看 UI 的置顶区）。

**IPC 接口**：`propose_trade_deal { country_id, give, monthly_give, take, monthly_take, years, reserve?, sensor_charts?, sensor_link?, specimens?, fleets?, dry_run? }`，
`specimens` 是标本键数组（写 first 侧）；资源数组都是 `[[资源索引, 数量], …]`；资源键 → 索引由插件解析
`common/strategic_resources/00_strategic_resources.txt`（`energy` 是第 0 个、`alloys` 第 10 个，与内存对得上）。

**接受：不需要新命令**（2026-11-21 验）——界面点「接受」投的就是 `CDiploActionRespondCommand`
（`sdk::cmd::diplo_action_respond_command`，vtable RVA **`0x23BA5F8`**、token `0x310C`、`id +0x20`、`accept +0x28`），
与桥里 `respond_diplo_action` 是**同一条命令**：两边的 `cmd_capture` 对比过，`id` 与 `accept` 逐字节相同
（`+0x28 = 1`、`+0x2C = 7` 两边都有；`+0x30` 之后是没初始化的垃圾，不影响）。

- 所以插件层不用加「接受贸易协定」的新工具，用 `stellaris_diplo_inbox { accept: true, id }` 就行
- **判成败看存档**：生效后 `trade_deal={ <id>={ first/second/ length/date } }`（国家段 `trade_deals` 会引用它）；
  收件箱里那条记录**应答后还会挂着**，别拿它判断
- **生效的 deal 只存 `monthly_resources`**：一次性那部分**递交时就已从发起方扣走**（托管），接受时转给对方，
  所以记录里看不到它；实测：发 10 稀有水晶 + 1/月，库存立刻 `21931.16 → 21920.16`（−11 = 10 + 首次月供 1），
  而 `income/expense` 不变（还没接受）。**验一次性只看库存**
- **坑**：那个档里出现过两条 `tag/serial` 完全相同的记录（`action_ptr` 也一样），按 id 应答会分不清；
  第一次「接受没生效」就是撞上它（记录消掉了、deal 没生成），换干净档一次就成


## 收口：基座 buildable 的 vtable 是这么找到的

前面几节记的是一条**绕远的路**（指纹法、构造器普查、OGAS 的线索）。真正定案的是「token getter 法」，
最终 vtable：**module `0x2349600` / building `0x23496B0` / upgrade `0x2349810` / platform `0x3E03`**。

### 内存偏移速查（`CStarbase` / `CShip` / 队列）


CStarbase +0x98    station（站体 id，低 24 位 = CShip 表下标）
CStarbase +0x5E0   modules 数组 / +0x5E8 size（+0x5EC capacity）
CStarbase +0x5F8   buildings 数组 / +0x600 size（+0x604 capacity）
CStarbase +0x880   build_queue（建造）
CStarbase +0x884   shipyard_build_queue（船坞；无船坞 = 0xFFFFFFFF）
CStarbase +0x890   level 对象
等级对象 +0xD8     模块槽数 / +0xF0 建筑槽数
CShip +0x988       设计号          CShip +0x110 持久名
CShip +0x9A0       站内编号        CShip +0x9C0 坐标（+0x20 = 星系 id）
CConstructionQueue +0x20 条目 / +0x28 容量 / +0x2C 条数 / +0x48 并行
CConstructionQueueItem +0x20 所属队列（核对归属用）
buildable: module 0x2349600、building 0x23496B0、upgrade 0x2349810、platform 0x3E03
```

### 怎么找到的（可复用的方法）

> 上面这几步是**静态**定位（反汇编 exe / 找 vtable）。运行时「某个数据存在内存哪」那一类，
> 流程就是四步：先找谁在读它 → 桥内探针 → 指针匹配做否证 → 两条来源对集合。

1. **token getter 法**：每个 buildable 类都有 `mov eax, <token>; ret` 的 GetToken，
   token 用 `out/win_layouts.json` 的 **Windows 编号**（`buildable_starbase_module` = `0x3E01`；
   Linux 的 `0x3DF8` 在 Windows 上无效）
2. getter（模块 `0x208110`、建筑 `0x208060`、升级 `0x208170`）→ 在 .rdata 找指向它的指针
   → 落在 `vtable + 0x80`（槽 16）→ 得到 vtable（模块 `0x2349600`、建筑 `0x23496B0`、升级 `0x2349810`）
3. 顺着 vtable 的 lea 引用找**游戏自己的生产者**（模块 `0xAD2EF0`、建筑 `0x227D00`），它揭示布局：

```
模块/建筑：大小 0x20，{ vtable, 类型@+8, 基地 id@+0x10, 槽@+0x14 }
升级：    { vtable, 目标等级@+8, 基地 id@+0x10 }（无槽）
平台：    { vtable, 内嵌 CShipDesignImplementation@+8 }，设计号在 +0x8+0x530
```

4. `FindDbElementByKey` 取类型指针 → `QueueBuildable()` 投 `CAddBuildableToQueueCommand`

**一个坑**：`QueueBuildable` 原本要求 vtable 的 slot2 与 slot3 都等于基类实现，而模块类**重写了 slot2**，
于是被误判“不是 CBuildable”；放宽成只比 slot3 后通过。


## 银河市场：读写

**读（`get_market`）**——市场对象在 **`g_CurrentGameState + 0xAF8`**（原先记 0xAF0，读到 0 条月度交易，
而那档存档里明明有 `trade_income.monthly_trades.rare_crystals=15`）。

``` 
CMarket  +0x60  monthly_trades 数组指针   +0x6C 条数   +0x70 下一单 id
每项 0x30：SMonthlyTradeData{ trade_data +0x8, amount +0x20, price +0x24, id +0x28 }
           STradeData{ resource, trade_type, country }（在 trade_data 里）
```

怎么找到的：`probe_market`（新增只读探针）按 `CMarket` 的形状扫 `g_CurrentGameState + 0..0x2000`——
候选要求 `+0x60` 是可读指针、`+0x6C` 条数 1..2000、`+0x70` 下一单 id 1..1e6，且**第一单**的国家 id < 0x40000000、
`trade_type ≤ 1`、资源指针能读出像 key 的字符串。全表只命中一个：`offset 2808`（=0xAF8），
它那一条正好是 `{country 0, rare_crystals, type 0, amount 15, id 10}`，与存档对上。

**写**用的是引擎命令（不手搓对象）：

| 动作 | 命令 | 关键字段 |
|---|---|---|
| 即时买 / 卖 | `market_buy_resource_command` / `market_sell_resource_command` | `trade_data{resource, trade_type, country}` + `multiplier`（**批数**） |
| 挂月度单 | `add_monthly_trade_command` | `monthly_trade_data{ trade_data, amount, price, id }`，新单 id 填 -1 由引擎分配 |
| 撤月度单 | `remove_monthly_trade_command` | 同形；**必须与现场那条完全吻合**（resource/type/amount/price/id），所以先读再照抄 |

**批量的坑**：`ExecuteInstantTrade` 里 `batches = units / batch_size`（不足一批按 1 批），
实际成交 `batches × batch_size`。矿物一批 100，`units: 10` 成交 100（实测贸易额 −94.23、矿物 +100）。

## 贸易协定的「AI 接受度」：两处否证

界面在发协定前会显示一个接受度（例：给 country 1 100 暗物质 → 29；给 country 9 → 26）。
**它是引擎实时算的，没找到可读存储**，两次否证：

1. **发送后的 `CAbstractTradeDealAction` 里没有**。同一份内容（first.resources = sr_dark_matter 100，
   无月供、无第二侧内容）分别发给 country 1 / country 9，逐 qword 比对：
   差异只有 `+0x20` 的收件方、`+0x30` 的 tag/serial、以及两侧资源表的分配地址；
   `+0x48` 有个 −25.0 的 float 只在 country 9 那份出现（0 在 country 1），但既不是 26 也不是 29，**与接受度对不上**
2. **贸易窗口对象里也没有**。窗口开关会影响 `idler + 152`（关着是 `0x2585AB8` 一类的 vtable，
   开着换成贸易窗口那一类），据此找到窗口对象；把内容 100 暗物质 → 50 暗物质（接受度 29 → 14），
   前 8 KB 里**只有一处变化**（`+0x10` 两个句柄值），既非 29→14、也搜不到 "29" 或「接受」字符串

结论：接受度是**每帧现算并绘制**的（AI 人格 / 存量资源 / 关系一类输入），不落存储。
要做只能拟合近似模型（AI 资源权重 + 关系信任），样本需求大、误差未知——暂搁。

**副产品**：`idler + 152` = 当前活动窗口指针，开关窗口可观测——以后做界面类探测可以从这里起手。

## 事件选项：IsValid 只判 allow，trigger 由插件三值求值

- 桥 `get_event_options` 只返回 `text` / `is_valid` / `effects`；`is_valid` 是引擎对**选项 `allow`** 的判定，
  **不覆盖 `trigger`**——被 trigger 过滤的选项在引擎里仍然 valid，界面却不显示它。
- 所以「哪些选项真能点」由插件 `stellaris_event_options` 自己求值：解析 `events/*.txt` 里每个选项的
  `allow` 与 `trigger`，做三值判断（真 / 假 / 判不了），输出 ✓ / ✗ / ? 前缀与「判定」行；头部统计三档数量，
  `selectable_only: true` 只列 ✓。
- 判不了的固定几类：`from` / `fromfrom`（站点、舰队、行星作用域）与 `has_country_flag`（flag 列表读不到），
  一律留「判不了」，不猜成真。
- 实例（2322 档，两个考古事件）：
  - `ancrel.14510`：`[2]` 要 `owner: is_spiritualist = yes` → ✗（机械智能）、`[4]` 要
    `origin_evolutionary_predators` → ✗（本档寻宝猎人）、`[3]` 要 `has_country_flag = curator_intro` → ?
  - `ancrel.12040`：`[1]` 要同一 origin → ✗；`[0]`（`is_pacifist = no`）与 `[2]` → ✓
- 这套求值只在插件端，桥不做；所以桥这边只保证「选项原文 + 引擎 allow 判定」准确。

## 考古遗址：对象布局与探针

- 数据库：`TPdxRef<CArchaeologicalSite> _pDatabase = 0x311BC58`
- 字段：`id +0x8`、`index +0x78`（= chapter，已完成章数）、`location +0x98`、`excavator_fleet +0xAC`、
  `events +0xB8`、`type +0xE0`（键在 +0x20）、`clues +0xE8`、`last_roll +0xF0`、`difficulty +0xF4`、
  `days_left +0xF8`（×100000 定点，锁定时不走）、`locked +0x178`
- 探针 `probe_excavatability`：对每处遗址各建一条 `CExcavateArchaeologicalSiteFleetOrderCommand`、
  **只跑 IsValid**（33 处约 56 ms），拒绝原因原样带回
- IsValid 只看疆域与「还有没有章节」，**不看舰种/科学家**（`is_ship_class` / `exists = leader` 在站点类型的
  `allow` 里，只把按钮变灰）；`chapter == stages` 时静默拒绝；「已被其他帝国完全发掘」也可能只是「已有船占着」
- 阶段锁：`locked = true` 由阶段事件的 `immediate` 里 `fromfrom = { set_site_progress_locked = yes }` 写下，
  `stellaris_answer_event` 答完即解除（下一 tick）；弹窗 ↔ 站点靠站点类型脚本的 `stage = { event = … }`
  按 chapter 序号对
- 警报：考古是 27 `alert_site_event`、裂隙是 50；**警报绑在面板上**，用事件推进时面板不刷新，警报会一直悬着。
  刷新不在数据字节里（应答前后快照逐字节相同），在视图自己的刷新槽；考古的刷新槽没找到
  （实测 slot 2 = 重建、4 = 无反应、7 = Hide），2026-11-20 决定先放着

## 星界裂隙：对象布局与探针

- 数据库：`TPdxRef<CAstralRift> _pDatabase = 0x311BD80`
- 字段：`id +0x8`、`name +0x128`、`owner +0x120`、`explorer_fleet +0x180`、`leader +0x184`、
  `type +0x1F0`（键在 +0x20）、`events +0x3E8`、`status +0x410`、`clues +0x414`、
  **origin 星系 `+0xA0` 低 32 位**
- 探针 `probe_rift_explorability`：对每处各建一条 `CExploreAstralRiftFleetOrderCommand`、只跑 IsValid
- 三条死路（2026-11-20 验过）：`+0x80` 的 `coordinate` 是共享常量；`CGalacticObject::astral_rifts +0x540`
  全银河为空（SDK 标着 positional，实测不对）；探索指令 `+0x20` 是噪声（`+0x28` 低 32 位 = 目标 id）
- **origin 怎么找到的**：同一日期的存档里 rift 的 `coordinate` 段写着 `origin=542`；拿 542 回内存，
  rift 对象 `+0xA0` 读出来正是 542。44/44 全部读出，`system_id_source: "rift_origin"`
- status：`0` 从未探索 / `1` 探索中 / `2` locked / `3` 完成 / `4` 中断（领袖死亡，作废）
- 星界细缝：全银河 33 处 `pc_astral_scar`；`split_astral_scar` = `spawn_astral_rift { in_place_of = prev }`
  + `remove_planet = yes`（天体被移除、裂隙在原位生成，所以找不到「已转化」的星体类名）
- 领袖一死裂隙直接作废（status 4、探索船清空），`CFireLeaderCommand` 也会被引擎拒（「无法解雇正在探索
  星界裂隙的领袖」）；`explorer_fleet_id` 带国家位（本档 16777723 → 掩码后 507）

## 遗珍：持有数组的真实偏移

**症状**：`get_discoveries {tab:"relics"}` 对任何国家都回 `held_count 0`。本档 country 0 在存档里
明明有 7 件（`relics = { "r_omnicodex" … }`）、country 1 有 1 件；`play 1` 切过去再读仍是 0。

**旧值**（上游 `Yidhar/stellaris-mcp` 至今用这一对）：`kCountryOwnedRelics = 0x30F0`、
`kCountryOwnedRelicsCount = 0x30FC`。

**真实偏移**（4.5.4 / Cygnus，2026-11-22 用 exe 反汇编定案）：

```
count = [rcx + 0x303C]      data = [rcx + 0x3030]      每个元素 = 8 字节 CRelic*
```

**怎么找到的**（三层，都踩在现成的 vtable 槽上）：

1. `CActivateRelicCommand`（命令 token `0x3CF3`）的 vtable RVA `0x23989E8`；`CCommand::IsValid` 是**槽 8**
   （`command_builder.cpp` 里的 `kSlotIsValid`）→ 读到 IsValid 的 RVA `0x6CAAA0`
2. `0x6CAAA0` 先把命令 `+0x50` 的国家 id 解析成 `CCountry*`，再调 `0x755860`——这个才是
   `CCountry::CanActivateRelic`（旧注释写的 `0x755060` 落在别的函数中间，别照抄）
3. `0x755860` 开头就是那段循环：
   ```
   movsxd rax, dword ptr [rcx + 0x303C]   ; 件数
   mov    rax, qword ptr [rcx + 0x3030]   ; 数据
   cmp    qword ptr [rax], rdi            ; 逐个比 CRelic*
   ```
   与旧值的「data + 计数在 data+0xC」是同一个形状，只是整体前移了 **0xC0**。

**实测对账**（2026-11-22，2322.07.01 档）：

| 国家 | 件数 @0x303C | 首元素 → CRelic 对象 +0x20 | 存档 |
|---|---|---|---|
| country 0 | 7（cap 9 @0x3038） | `r_omnicodex` | 7 件，首项即 `r_omnicodex` |
| country 1 | 1 | `r_time_crystal`（PDX 串 size 14 / cap 15） | 1 件 |

`CRelic` 对象首 qword 是模块指针（`0x7FF7…`），键在 **+0x20** 且是 PDX 串
（`r_omnicodex` size 11、`r_time_crystal` size 14）——与 `CSpecimen` 同一读法。

**顺带两条**

- `get_status.discoveries.can_activate_relic` 是「件数 > 0」，不是「能激活」；逐件的答案要靠
  `CActivateRelicCommand` 的 IsValid（`GetDiscoveriesInfo` 里就是这么做的）
- 遗珍 key **是序列化的**：`country.<id>.relics = { "r_..." }` 在存档里是明文。所以
  「运行时状态、SDK 看不见」只对了一半——SDK 成员表里没有它，但存档写得出来，正好当对账真值

**改的地方**：`discoveries_manager.cpp` 的两个常量与注释；重编注入 2026-11-22 00:1x（模块基址 `0xD02C0000`）。

## 星界行动：读 + 启动

**读**：`get_discoveries { tab: "astral_actions", probe: true }` —— 多了一个 `probe` 参数，为 true 时对数据库里
**每一条**行动各建一条 `CActivateAstralActionCommand`、只跑 IsValid（`CommandBuilder::Create` + `IsValid`，不 Post），
回 `can_activate` 与引擎原文 `blocked_reason`：

```
{"key":"action_astral_boost","name":"星界助推","can_activate":true}
{"key":"action_summon_dimensional_fleet","name":"召唤异次元舰队","can_activate":false,
 "blocked_reason":"✗  该行动还需要842天冷却。"}
```

`potential` **不在 IsValid 里**：界面不显示的那几条（本档 4 条）回 `can_activate: false` 且**没有 reason**，
插件层就是靠「false + 没有 reason」把它们认出来并藏在 `all: false` 的列表外。

**写**：新方法 `activate_astral_action { action_key }`（266 个 IPC 方法）：

```
CActivateAstralActionCommand：token 0x4290 ｜ vtable RVA 0x2398598 ｜ 0x30 字节
  +0x20 astral_action（对象指针，序列化时写成键串） ｜ +0x28 country（ref）
```

做法与 `activate_relic` 同形：按 key 在 `TGameDatabase<CAstralActionsDatabase>::_pInstance = 0x31195E0`
（db +0x50 数组、+0x5C 条数、条目键在 +0x20）里找对象 → 写 `+0x20`/`+0x28` → **先 IsValid、再 Post(EngineGate)**。
IsValid 的拒绝原话直接回给调用方（冷却、次数用完、缺方舟舰、丝缕不够…）。

**实测（2026-11-22，2322.07.01 档）**

| 观察 | 数据 |
|---|---|
| 数据库条数 | 16（脚本也是 16 条 `action_*`）；界面 12 条 |
| 界面 12 条里可启动 | 5（星界助推 / 物理学洞见 / 闪铸超空间中继器 / 并行巡展 / 维度锁） |
| 另 4 条不显示 | 量子弹弓两条（`is_nomadic = no`，本档游牧）、大迁徙跃迁 base（已有 `tech_arkship_exodus_jump`）、超空间中继器 base（已有 `tech_hyper_relays`） |
| 启动 `action_astral_boost` | `Astral action activated`；丝缕 **3800 → 3100**（= 500 + 50×4，按 usage 递增）；随后该行动报「还需要1800天冷却」 |

**`CCountry +0x30F8`**（SDK 标 `astral_rifts`，i32）本档读作 **6**——就是已探索裂隙数，
`unlock_threshold` 数的正是它（脚本里门槛 1–5）。

**使用状态拆开了（2026-11-22 定案）：`CCountry + 0x3100`**

```
+0x3100  哨兵/模块指针（0x7FF7…）
+0x3108  data
+0x3110  cap  = 16
+0x3114  size = 16
元素步长 0x40，索引 = 行动对象 +0x08 的库内下标（= 脚本声明序）
  +0x00  unlockable (u8)         这个行动对本国是否已解锁（潜在条件 + 裂隙数阈值都算在内）
  +0x01  unlocked   (u8)         存档同名字段；写入时点未定（本档 16 条全 0，引擎照样放行可用项）
  +0x04  usages     (i32)        剩余可用次数，-1 = 不限次
  +0x08  next_usable_date (u64)  冷却结束日（游戏日期编码），0 = 从没进过冷却
  +0x20  key (std::string)       键，长度 +0x30、容量 +0x38
```

**怎么定案的**：`CActivateAstralActionCommand`（vtable RVA `0x2398598`）槽 8 = IsValid `0x6CE890`，第一道闸就是
`mov rax,[cmd+0x20]` → `movsxd rcx,[rax+8]` → `shl rcx,6` → `add rcx,[country+0x3108]` → `cmp byte [rcx],0`。
步长 0x40、data 在 +0x3108、首字节是解锁位，全在这一句里。

**存档是同一套字段**：`country.<id>.astral_actions_usage_states_array` 里逐条写着
`unlockable / unlocked / usages / next_usable_date`（偶尔多一条 `is_waiting_for_cooldown_to_start`），
所以对账不用反汇编——存一次档就能核。核对结果（2280.12.15 档）：裂分剩 3/4、`2280.12.15 + 187 天 = 2281.06.22`；
虹吸剩 2/3、`+2802 天 = 2288.09.27`，与引擎 IsValid 回的「该行动还需要 N 天冷却」逐日相同。

**读法落到了 `get_discoveries`**（`tab:"astral_actions"`，不新增方法）：每条行动带上
`index / unlockable / remaining_uses / next_usable_date / next_usable_date_raw / cooldown_days_left`，
顶层多 `usage_states`（= size）与 `usage_states_capacity`。`cooldown_days_left` 是拿 `GameState::ReadDate()`
的序数日跟冷却日相减，过去的一律算 0（那几条 2276 / 2238 的日期就是「早就过了」）。

**还没解**：`is_waiting_for_cooldown_to_start` 在内存里的位置——元素只有 0x40 字节，除上面这几位与
0x20 的键以外没再见到被读的地方；症状是「冷却还没开始」的过渡态，靠引擎 IsValid 回答就够。
另：`unlocked` 那一位由 `0x6CE75B` 一处代码按 `action+0x1B4` 写 1，但什么时候走到那里没抓到。


## 文物行动（Artifact Actions / 稀有文物）：读 + 启动

- 数据库：`TGameDatabase_CArtifactActionsDatabase_pInstance = 0x3119738`（+0x50 数组 / +0x5C 条数，条目键在 +0x20）
- 启动：`CArtifactActionCommand`（token 0x3CFA，vtable `0x2398930`，0x30 字节；`artifact_action` +0x20、country +0x28）
- 探针：`get_artifact_actions {probe:true}` 逐条构建命令、只跑 IsValid（什么都不投）
- 脚本在 `common/artifact_actions/artifact_actions.txt`（28 条，4 条 `*_locked` 占位）；代价在
  `resources { category = artifact_actions cost = { minor_artifacts = N } }`，冷却/次数由各条 `effect` 自己写
- 实测 2322.07.01 档：24 条非占位，12 条可启动；拒绝里 `artifact_analysis` 回「已达到该行动的最大使用次数」，
  其余空原因 = potential 未满足

## 标本（大档案馆 / 博物天枢）：国家持有列表从内存读

游戏里这一套叫**博物天枢**（`grand_archive` 的本地化名）。

**布局（实测，2322.07.01 档）**

```
CCountry + 0x2AB8      -> CGrandArchiveCountryModule*（堆对象）
module + 0x40          storage_slots：flat ref_array {data, cap(u32), size(u32)}，元素 = u32 的 CExhibit 引用
module + 0x28          -> 3 个「展览格」包装对象，每个 0x18：{vtable(0x7FF7989045F0), data, cap, size}
                          index 0 = aesthetic_wonder / 1 = historical_item / 2 = xeno_geology（与存档顺序一致）
CExhibit               id +0x10、exhibit_state +0x18、specimen 指针 +0x28、owner +0x1A0（发现者 ref，掩码 0xFFFFFF）
CSpecimen              vtable RVA 0x2382708、键在 +0x20（PDX 串）
```

**引用编码**：数组元素是 u32；存档里写 `0x01000000 | id`，内存里也见过带标记的，统一 `& 0xFFFFFF`。
注意 `CExhibit+0x20` 不是标本（那里是模块里的一个静态指针），真正的标本对象指针在 **+0x28**。

**探针**：`probe_grand_archive { country_id, decode }` —— 先扫 country+0..0x3FF0 的 `{data, cap, size}` 形状，
对第一个元素跑 `TPdxRef<CExhibit>`（`_pDatabase = 0x311BDB0`；slots +0x18 / cap +0x20，
`slots + id*16 + 8` 取对象，对象 +0x10 的 id 必须与引用一致）验证；命中且元素里 ≥2 个不同合法 id 的就是模块。
`decode: true` 按上面布局解出储藏室与三个展览格。

**实测对账（country 0）**

- 储藏室 7 件：45/51/52/53/54/64/72 —— 与存档 `storage_slots` 逐项相同
- 展览格 9/9/6：id 与存档 `exhibits` 三类逐项相同（存档的 16777220 / 16777216 / 16777281 就是
  `0x01000000|4`、`|0`、`|65`，掩码后一致）
- **发现者 ≠ 持有者**：持有 = 被哪个数组引用（储藏室 / 某一类展览格），发现者 = `CExhibit+0x1A0`

**状态与空位**

- `CExhibit+0x18` 低字节 = 槽位（`0` 库存 / `1` 未开槽 / `2` 已开槽），`+0x19` = `activated`；
  **state=2 且 activated=1 才是「展览中」**，其余（state 1 或 activated 0）都是「未展览」
- 每个展柜 9 个位置，未展览的也占位；满位时「发送到展览格」被引擎拒绝（无原因）
- 移走时旧位置留一条空位（标本键为空），**空位保留原状态**；下一件从仓库发往同类展柜的标本
  会拿走这个空位并继承状态（原来展览中 → 直接展览；原来未展览 → 要先 unlock）

**动作命令（2026-11-22 录制）**

- **激活展览** = `CUnlockExhibitCommand`（token 0x433C，vtable `0x239F7C0`，0x30 字节；
  country +0x20、`exhibition_index` +0x24、`exhibit_index` +0x28）
- **取消展览** = `CToggleActivateExhibitCommand`（0x433D，vtable `0x23C5B88`，同样字段）
- 两个 index 是「第几类展柜（0/1/2）」与「该类格内第几个」，不是 exhibit id
- `state=1` 时 toggle 被 IsValid 拒（无原因）；先 unlock（state 1→2）才能 toggle
- **移动**：`CMoveSpecimenToStorageCommand`（0x435F）/ `CMoveSpecimenFromStorageCommand`（0x4360），
  country +0x20、exhibit +0x24；引用一般用裸 id，**id 0 要写 `0x01000000`**（裸 0 = 空引用，
  IsValid 静默失败）
- 展览中可直接入库，不需要先取消展览；门槛是**每件标本的返回库存冷却**——
  **由「仓库 → 展柜」触发、持续 720 天**（展柜 → 仓库不触发；引擎回「在 N 天内（日期）无法返回库存」），
  对应 `SOwnedSpecimen+0x8C` 的 `storage_cooldown_date`
- 移动后标本拿**新 id**、旧位置留空位（读工具跳过空位）

**还没解**：module+0x60 有一份 110 项数组（含别国 owner 的 id），像「见过的/曾拥有的」总表，不是持有列表


## `find_ships` 的船 id 读成 0：`+8` → `+0x18`

**症状**：`find_ships {limit:5}` 回五条，`ship_id` 全是 0，`fleet_id` 干脆没有；名字、设计号、星系都对。

**病因**：`OutlinerManager::FindShipsJson` 从 `obj + 8` 读 id（那是槽位内的某个字段，值恒 0），
而 `CShip` 的 id 在 **`+0x18`**（`sdk::rt::CShip_id`，`probe_fleet_ships` 一直用的就是这个）。

**修法**：改成 `SafeReadU32(obj + sdk::rt::CShip_id, &ship_id)`。重编注入后复测
`find_ships {limit:5}` 回 `[0]…[4]`，与 `probe_fleet_ships {fleet_id:1}` 报的 `ship_id: 1` 对上。

**顺带回答一个常问的**：`find_ships` **不出 `fleet_id`**——船对象上没有「属于哪支舰队」的回指，
舰队→船是 `CFleet+0x320` 的 id 数组。要按舰队找船，用 `probe_fleet_ships {fleet_id}` 或
`stellaris_fleet_composition`；`find_ships` 的用处是反过来：「这个设计号的船都在哪些星系」。

## `find_systems` 加了 `name` 模式：全银河按名字找星系

**为什么要加**：插件一直缺「按名字查星系 id」这条路——`get_galaxy_map` 只给**中心 3 跳**的邻域
（本档 19 行），`get_system` 要 id 本身，`probe_system` 也是。于是 `move_fleet` 之前那一步只能靠扫。

**改法**（`GalaxyManager::FindSystems`）：多一个可选 `name`

```
find_systems { name: "监牢" }                       → { mode: "name", query, from_system_id, systems: [...] }
find_systems { name: "监", limit: 20, from_system_id: 109 }
```

- 比较的是 `SystemName(id, obj)` 的小写子串（`std::tolower` 逐字节，中文按 UTF-8 原样比）
- 仍是**从 `from` 算跳数**（默认首都），排序按跳数；`limit` 夹在 1–50
- 返回 `{systems, count, mode, query, from_system_id}`；`purpose` 那条路不变（返回 `{purpose, from_system_id, systems, …}`）
- 名字是**本地化后的显示名**，所以搜 "a" 在中文档里自然 0 命中——要按内部键搜得另开一条

**实测**（2280.12.15 档）：`name:"监牢"` 回两个——535（我方，0 跳）、109（country 1，37 跳），
与 `get_galaxy_map` 的 center 一致。

## 「局势取向」：`get_situation_approaches` 不接

桥里有两个读取向的入口：`get_situation_log`（列表里带 `current_approach`）与 `get_situation_approaches`
（按 situation_id 现读）。后者走内存：`CSituation+0x208` → 类型，类型 `+0x68` → approach 块，
`CSituation+0x210` → 当前选中的那个，再逐个试 `+0x18` 的 PDX 串。

**实测（2280.12.15 档，situation 21「运行储备」）**：

```
get_situation_approaches { situation_id: 21 } → { "success": false, "error": "no approach found for this situation" }
```

而脚本 `common/situations/*.txt` 里这条有四个取向（`…_stringent_budget` / `…_balanced` / `…_blazing` / `…_carnival`），
`validate_situation_approach` 逐个问引擎也都能答（含「不可用 + 缺的条件」）。所以插件端走的是
**脚本清单 + validate** 这条路，`get_situation_approaches` 保持不接；要真修它，得先弄清 `+0x210` 之后
那些块的迭代方式（`+0x68` 指向的容器形状与 64 次的试读都不对）。

顺带：`probe_situation` / `probe_situation_type` 是只读探针（给偏移吐原始字），也不做成工具。

## `get_galaxy_map`：取消 12 跳上限，不给 `jumps` = 全银河

**原来**：`GetMapJson(center, jumps)` 里 `jumps = std::clamp(jumps, 0, kMaxJumps)`，而 `kMaxJumps = 12`
（`galaxy_manager.cpp:28`），默认 `jumps = 3`。所以它只能给「某中心 12 跳以内的邻域图」，
再怎么调也拿不到全图——实测传 `jumps: 50` 仍然只回 148 行、最远 12 跳。

**现在**（玩家定的形状：保留 `jumps`，取消上限，不给就读全银河）：

```
int jumps = params.value("jumps", -1);       // IPC：不给 = -1
const bool whole_galaxy = jumps < 0;
jumps = whole_galaxy ? kHardJumpCap : std::clamp(jumps, 0, kHardJumpCap);   // kHardJumpCap = 1<<20，纯防呆
返回里 {"jumps", whole_galaxy ? -1 : jumps}
```

- `jumps < 0` → BFS 走满全银河；`jumps: 0` 只有中心；`jumps: N` 就是 N 跳球
- `kMaxJumps = 12` 这个常量还在，但 `GetMapJson` 不再用它（留给别处参照用）

**实测（2280.12.15 档）**

```
不给 jumps    → 197 个星系、286 条航线、最远 22 跳、JSON 12 KB
jumps: 3     → 13 个星系
jumps: 50    → 197 个（不再被 12 截断）
```

**行格式**：`columns = [id, name, x, y, owner_id, intel, jumps, flags]`，`flags` 里
`S` 已勘察 / `C` 已殖民 / `B` 有基地 / `F` 有我的舰队 / `A` 有未研究异常 / `K` 我宣称过 / `M` 巨构。
插件端 `stellaris_galaxy_map` 就按 `columns` 解，不写死列序。

## 虫洞 / 星门：`get_bypasses` 全银河一览

**为什么加**：`use_bypass` / `explore_bypass` 都要 bypass id，而普通读取（`get_system` 的 `bypasses` 段、
`get_galaxy_map`）都不给全表——想派科研船探虫洞时根本不知道 id。这条一加，`stellaris_bypasses` 就有了数据源。

**做法**（`GalaxyManager::GetBypassesJson`）：

```
位置      扫所有星系的 CGalacticObject::bypasses（+0x5A8）反推「这条 bypass 站在哪个星系」
形态      CBypass +0x18 linked_to（ref，虫洞成对）｜+0x20 connections / +0x38 active_connections（星门成网）
类型      CBypass +0x50 → 类型对象，键在 +0x28（kBypassTypeKey）
状态      +0x68 active ｜ +0x58 owner ｜ +0x6C lock_country ｜ +0x70 lock_remaining_days
可用性    CCountry + 0x34A0 usable_bypasses（ref_array，按 id & 0xFFFFFF）
          另有 +0x3430 seen_bypasses（「见过」不等于「能走」）
```

- 返回 `{count, usable_count, wormhole_count, gateway_count, bypasses[]}`；每行 `id / type / type_name /
  system{id,name} / linked_to + linked_system / active / connections / active_connections / usable / seen /
  locked_by + lock_days`
- 类型键实测有：`wormhole`、`starlit_wormhole`、`gateway`、`lgate`（L-星门）、`shroud_tunnel`（虚境通道）、
  `relay_bypass`（中继）
- **`wormhole_count` 只按名字粗分**（含 gateway 的算星门，其余算虫洞类）——要精确就按 `type` 自己判

**实测（2227.01.01 测试档，26 条）**

```
虫洞类 16（天然虫洞 8 对，每对 linked_to 互指；另有 L-星门 2、虚境通道 1、中继 1）
星门   10 口，connections 把十口连成一张网（active_connections 全空 = 还没起用）
usable 0 / seen 0 —— 这个档一条虫洞都没探过
```

**注意**：IPC 里 `get_bypasses` 与 `get_galaxy_map` 共用同一个 else-if 分支（避免再加深嵌套，那个文件顶到过 C1061）。

## 新增但**未验证完**：`get_all_fleets` / `get_fleet_templates`

两只都编译进桥了，但偏移没验完，**插件端都没用它们**（模板工具改用 `get_fleets`，见下）：

- `get_all_fleets { type_filter, foreign_only, limit }`：想列全银河舰队（含太空兽群）。
  第一版手写遍历没抄 `fleets::AllIds` 的两道保险（`cap < 1e6`、`(id & 0xFFFFFF) == 槽位号`），
  cap 一脏就跑到天边，卡死任务线程；后来改成走 `fleets::AllIds` 才安全。
- `get_fleet_templates`：读 `CFleetTemplate` 数据库。能枚举模板、能读条目（设计 id 在条目 +0x550、quota 在 +0x558），
  但 **「模板 → 舰队」的引用偏移不对**：读出来的 fleet_id 是 2831/704 这种，与 `get_fleets` 的
  `template_id`（134217871…）对不上，加「只要本国」过滤后会返回 0 条。**下次先量这个偏移再加过滤。**

**已知可用的替代**：`get_fleets` 本来是按国家的模板向量（`CCountry.fleet_template_manager +0x8+0x8/+0x14`）
枚举的，每行就是模板（`template_id` / `total_quota` / `designs[]`），所以插件端 `stellaris_fleet_templates`
直接用它——**不需要新桥方法**。兽群识别要 `design_id → ship_size`，而 `get_ship_designs {design_id}` 对部分
设计返回空（例：白色恐惧那条 251661707），这条链还没通。

## `get_all_designs`：全局 `CShipDesign` 库

```
TPdxRef<CShipDesign>::_pDatabase = 0x311B680   → slots +0x18 / cap +0x20，对象 slot*16+8
id        槽位自洽：读 CShipDesign_id 并要求 (id & 0xFFFFFF) == 槽位号
name      CShipDesign::name + CPersistentName::key（PdxString）
ship_size CShipDesign_stages → 第一个 growth stage → CShipGrowthStage_ship_size → +0x20 的键
段/部件    reuse ship_designer 的 designs::Sections / SlotsOf / Installed / Cores（与 GetShipDesigns 同法）
```

**为什么必须读全局库**：`get_ship_designs` 只走本国的 `ship_design_collection`，而且只列
`Designable` 的舰型——事件船（白色恐惧 `NAME_Derelict`，251661707）与兽群的部分设计在那里查不到。

本档实测（2319）：表容量 7168、可读 **4196** 条设计，其中兽群舰型 **145** 条
（`voidworms_*` / `space_whale_*` / `crystal_ship_*` / `cutholoid_*`），耗时约 0.4 秒。

**坑**：插件里不能对同一管道 `Promise.all` 并发两问——`get_fleets` 与 `get_all_designs` 并发时
后者会拿到空结果（表现为 ship_size 全 null），必须串行 await。

## `get_all_megastructures` 与巨型结构的两种建造位置

```
TPdxRef<CMegaStructure>::_pDatabase = 0x311BD40 → slots +0x18 / cap +0x20，对象 slot*16+8
id 自洽：(id & 0xFFFFFF) == 槽位号；owner 在 CMegaStructure::owner，星系在 coordinate.origin
升级信息在 SMegaStructureUpgrade（upgrade_to 的键 / progress 定点天数 / halted）
可见性同 get_megastructure：自己的，或所在星系 intel >= 2
```

本档实测：表里 559 个，可见 550 个。

**建造位置两种形态**（`CMegaStructureType::build_type`）：

```
kBuildAtPlanet             → 给 CPlanet 的 id
kBuildInsideGravityWell / kBuildOutsideGravityWell → 给 system + toward 邻星，
                             x/y 由 OpenSpacePoint() 按建造菜单同一算法算（不要自己编坐标）
```

`get_buildable_megastructures` 会把合法 site 列出来：天体型 `{planet_id, name}`，
开放空间型 `{toward_system_id, toward}`；两种情况都先用 `IsValid` 过滤，所以「列出来 = 现在能投」。

## `find_arc_furnace_systems`：按天体数挑电弧熔炉星系

电弧熔炉的收益看**星系内天体数量**（本地化文案：揭露星系内每颗行星、卫星和小行星上规模 1 的矿物，
开采站产出 +25%），所以这只方法按 `s.planets[system]` 的条数排序，并套用脚本的两条前置：

```
system_has_arc_furnace = no                 → 先扫 CMegaStructure 库，把已有 arc_furnace 的星系剔掉
any_system_planet = { is_planet_class = pc_molten }  → 天体里至少一颗 class 键是 pc_molten
```

给了 `fleet_id` 时，对每个候选星系的每颗熔融星球跑 `BuildMegaCommand(...).IsValid()`，**用引擎的话**
判定能不能投，`site_planet_id` 就是可投的那颗；不给了只做脚本筛选。

实测（本档 2319）：天体最多的几个（克根/监牢 24，西什马/伊西斯 23）因游牧政体的
「驿道要有驿港或驿垒」全部投不了；真正可投的是**军市一**（天体 20、熔融 7，planet 5083）。

## `find_dyson_swarm_systems`：戴森云候选

脚本前置（`18_dyson_swarm.txt`）：`system_has_dyson_swarm = no`、`is_multiple_star = no`、
`NOT is_star_class = sc_black_hole / sc_neutron_star / sc_pulsar`；效果是
`station_gatherers_produces_mult` 与 `station_researchers_produces_mult`，即**只放大恒星自身产出**。

实现要点：

```
恒星判定用天体类键（CPlanet::planet_class）：pc_f_star / pc_m_star 这类 _star 结尾的，
        外加 pc_black_hole / pc_neutron_star / pc_pulsar；单星 = 恰好一个这种天体
恒星产出读它身上的 deposit（CDeposit 的 deposit_holder 指回该天体），键形如 d_energy_3 / d_physics_2
折算：能量×1 + 科研×4 + 战略×8 + 文物×8 + 矿物×1（1 文物 = 1 战略 = 2 科研 = 8 能量）
```

**注意**：`CGalacticObject::star_class(0x558)` 是**星系**上的字段，不是 CPlanet 的——第一版照它写编译不过，
换成天体类键才通。给了 `fleet_id` 时对恒星天体跑 `BuildMegaCommand(...).IsValid()`，可投行带 `site_planet_id`。

实测（2319）：候选 200 个（单星、非黑洞/中子星/脉冲星、还没有戴森云），可投 8 个；
值最高的佩斯奇（B型恒星、科研 9、值 36）在 25 跳外且驿港条件不满足，**值排序必须配合可投分组才有用**。

## 物种：帝国名单、未开智、点数、九类冷却

**帝国模式的名单换了来源**（`get_species`）。原来自己拼「建国 + 模板池 + 有专属权利的种」，
用默认权利的和未开智的都会漏。改成读 `CCountry::owned_species_refs`（`+0x3470`，
`ref_array<TPdxRef<CSpecies>>`，CPdxArray 头 `{data +0x8, cap +0x10, size +0x14}`）——
**物种界面用的同一张表**。存档对账（2319.07）：country 0 =
`{23 27 81 82 83 115 143 169 176 193 206 218 279 283}`；现档读出来 18 条、未开智 3。

**未开智 / 已提升 的判据**（脚本层，跨版本稳）

- 未开智：特质里带 `trait_presapient_*`（脚本里这些特质都 `sapient = no`）**且没有** `trait_uplifted`
- 已提升：有 `trait_uplifted`。**提升后仍保留未开智特质**——只按特质判会把已提升的误判成未开智
  （本档 290/291 就是这种）
- `CSpecies::home_planet (+0x28)`、`uplifter (+0x30)` 是两个 ref，`0xFFFFFFFF` = 无；输出取低 24 位
- `CSpecies+0x53C` 有 `sapient` / `immortal` / `can_be_modified` / `inherit_parent_rights` 四个 bool_cond
  挤在同一偏移（SDK 标 `[check: sentinel]` / `[check: positional]`），**没有拿它当判据**，用的是特质

**模板 vs 亚种**：`CCountry::species_templates_ref`（`+0x3458`）是**模板池**（还没应用的模板）。
`in_template_pool` 为真 = 模板；带 `base_ref` 却不在池里 = 已经存在的实体亚种（本档 23 / 27 / 291）。
本档池里 9 条，与存档的 `species_templates_ref` 逐条对上。

**每物种点数**：`NSpeciesModification::HasFreeSpeciesTraitPoints(country, species, &points, &picks)`，
每物种一次（18 条约 18 ms）。`get_species` 因此带 `points_free / picks_free`；`points_free` 可以为负
（模板花超，本档 279/283 是 -3）。

**九类权利各自的冷却**：每类读自己的
`CSpeciesRightsCountryConfiguration_changed_*`，再换算成天。只给一个最大值会误导——本档实测
模板 2202 天、米沙的移民控制 3600 天。

**应用模板（apply）**

```
命令 create_species_mod_special_project：country +0x20 / template +0x24 / species +0x28（ref_array<SSpeciesColonyPair>）
pair 元素 SSpeciesColonyPair：vtable +0，species +0x8、planet +0xC（ref<CColony>，**殖民地 id**），步长 0x10
```

- 实测要通过：**template 的 base 必须是基底物种，且该种在目标殖民地有人口**
- 2319 档的四次拒投都对上了：291（base 290 是别国提升的，本国没 290 的人口）、
  335（base 215 本身是模板）、333（base 是 built species，无人口）全拒；
  「赛凡特（4）→ 模板 336 → 殖民地 56」投成，项目 `kind=species_modification`
- **对模板/亚种再建模板**：游戏会把新物种当**独立物种**（335 的 `base_ref=215` 指向一个模板；
  它缺 `name_data` 是因为源物种 215 本身就没有，不是套娃的锅），应用也会被拒。界面只对基底物种提供「建模板」
- 新参数 `tag_refs`（默认 false）：把 pair 里的 species/colony 写成 `0x01000000|id`，
  留给以后排查 ref 编码的开关；本轮没用到
- 取消：`CCancelProjectCommand` 对这类项目有效，状态回「未开始」，**情报日志条目保留**

**顺带修掉一个桥 bug**：`SpeciesManager::GetPlayerCountry()` 原来直接读国家库第 0 槽（country 0），
于是 `play N` 之后物种/权利/模板命令仍作用在 country 0 上（应用当然找不到人口）。
改成 `GameState::Get().GetPlayerCountry()`，与其余 manager 一致。

**建模板：种子（`species`）与记录基底（`base_ref`）是两回事（2026-11-22 深夜实测）**

- 命令 `CCountryCreateSpeciesModTemplate` 的 `species`（+0x24）必须是**真实物种**：
  写 `0xFFFFFFFF` 会被引擎拒，文案只有 `Species template creation rejected`，**不给原因**
- 新物种记录里的 `base_ref` 才是「记录基底」，三种落点：

  ```
  指向根物种（base_ref 为空的那个）→ 真亚种，界面挂在该根下
  指向亚种/模板                  → 游戏显示成独立物种（base_ref 里仍留着那个指针）
  0xFFFFFFFF                     → 无基底的独立物种，但**仍在模板池里**
  ```

- 桥新参数：`root_base_id`（记录基底；`0xFFFFFFFF` = 用种子）、`no_base`（把记录基底置空）
- 实测（country 0）：

  ```
  种子 115（自身 base=3791650817）→ 记录 base 3791650817 → 与 115 同级
  种子 291（base=290）            → 记录 base 290       → 与 291 同级
  种子 291 + root_base_id=291     → 独立物种（337）
  种子 291 + no_base              → 独立物种、base 空、仍在池里（340）
  ```

- 建国种那条要留意：`115` 自己就是 `base_ref=3791650817` 的亚种（mod 的开局设置），
  所以「对建国种建模板」= 记录到 3791650817 名下，而不是 115 名下

**全银河普查：建国种也可以是亚种（2026-11-22）**

方法：解析存档 `countries` 块里的 `founder_species_ref`，再拿 `species_db` 的 `base_ref` 判形态、
数 `pop_groups` 里各物种的人口组。

```
60 个有创始物种的国家：根 49｜亚种 11
11 个亚种创始人的根：全部还在（没有孤立 base）
机械/蜂巢 6 个：真正玩出来的机械智能是 country 0 与 1，两个创始人都是亚种
    0：founder 115 → base 3791650817（根 0 pop groups，115 有 8）
    1：founder 215 → base 3          （根 0 pop groups，215 有 110）
    另 4 个是事件机械（机械校准器 / Model-16 …），创始人是根
```

结论：人口全部迁到模板后，**根记录不消失，只是空掉**（0 pop groups），`founder_species_ref` 改指模板。
所以 `is_founder` 只说明「谁被指为建国」，不说明「谁是根」；插件端列表给建国行标了「真根 …」，
单卡里也写明了这一层。

**提升（uplift）：`CBeginUpliftingCommand`（2026-11-22 接）**

```
token 0x2F78 ／ vtable RVA 0x2399A70 ／ factory 0x6D33F0 ／ size 0x28
country +0x20（ref<CCountry>）、species +0x24（ref<CSpecies>）
```

- 桥方法 `begin_uplifting { species_id, dry_run }`：先自查「未开智」（特质带 `trait_presapient_*` 且没有
  `trait_uplifted`），再跑 IsValid；`dry_run` 只校验、不投
- **与「应用模板」不同**：应用（`create_species_mod_special_project`）投出即开始；uplift 只是**建了项目**，
  必须再 `research_project { project_id }` 启动，否则它挂 30 天就过期（2026-11-22 玩家指出、实测确认）
- **IsValid 不查资源**：本档 81 / 82 都通过，但真投要花社会学研究，并生成 `kind=uplift` 的特殊项目
- **真投两次的教训（2026-11-22）**
  - `[345]`：只投、没启动研究 → 没有「进行中」，30 天走完被清掉，物种没变、研究点没扣
  - `[347]`：投出后 `research_project` 启动，显示「进行中」；投出当天正好是 2320.01.01 结算日，
    研究点又高 → **结算时当场完成**，项目记录随即清空
- 完成的表现：**新建一个物种记录**（`base_ref` 为空 = 新的根），`trait_uplifted` 加上、`uplifter` 记本国
  （本档 `347 韦提里西乌斯`，uplifter country 0）；旧的未开智记录（81）还在，但离开本国的物种表、人口 0
- 所以：**uplift = 建项目 + 启动研究 + 等月度结算**，三段缺一不可；项目完成后从情报日志里消失
  （不像残骸类留「已完成」），别把「记录没了」当成失败
- 同一时间只能有一个修饰/提升项目，引擎会回「我们已经在开展一项修饰或提升物种的项目」
- 第二次实测（`[350]` 提升 82，2320.02.02 → 03.02）：工具自动启动成功；跨 03.01 结算后完成。
  社会学研究 **300,764 → 257,345**（扣 43,419，第一次看到真扣）；旧种 82 的 pop groups 归 0、
  新种 348 拿到 2 组。**物种表有延迟**：完成当天 `stellaris_species` 里还没有 348，跑到 03.09 才出现
- 插件工具 `stellaris_uplift_species` 已改成「投完自动找那条 `kind=uplift` 并启动」，现在标 ✅



## 隐形：两条命令 + `CFleet+0x1268` 的位

**命令**（SDK 里都有，`sdk::cmd::` 下）

```
activate_cloaking      CActivateFleetCloakingCommand
  token 0x4210  vtable 0x23BFD40  factory 0xA772A0  size 0x28
  fleet +0x20   ref<CFleet>

deactivate_cloaking    CDeactivateFleetCloakingCommand
  token 0x4211  vtable 0x23BFC88  factory 0xA772F0  size 0x30
  fleet +0x20   ref<CFleet>
  forced +0x28  ref<CCountry>   ← 谁逼你解除；自愿解除送 0xFFFFFFFF（空引用），IsValid 通过
```

桥方法 `set_cloaking { fleet_id, active, dry_run }`（`GalaxyManager::SetCloaking`）。
**`IsValid` 只看舰种/装置**：工程船会被拒（且不给原因），但**冷却中的舰队照样过校验** ——
「能投」不等于「会生效」，跟法令那条一个道理。

**状态位：`CFleet + 0x1268` 一个 dword**

定法：同一支舰队（fleet 1）取消隐形前后，整个对象逐字节比 —— **只有 6 个字节变**：

```
+0x1268   0x0004A602 → 0x0006C602     XOR 0x26000 → bit13 / bit14 / bit17
+0x12C0   0x029C55C0 → 0x03CE8650     cloaking_cooldown_start_date（SDK 标的 +0x12C0 是对的）
```

| 位 | 掩码 | 含义 | 证据 |
|---|---|---|---|
| 13 | `0x2000` | **is_cloaked** | 隐形时置 1、解除后清 0 |
| 14 | `0x4000` | **cloaking_cooldown** | 解除后置 1；**120 天到期后自己清 0**（0x6C602 → 0x48602） |
| 17 | `0x20000` | **dirty_cloaking_strength** | 状态一变就置 1，强度重算完自己清 0（ISS金鳍 0x2A602 → 0xA602） |

四位是 2026-11-22 下午「把时间跑过 120 天」之后定死的（2322.07.03 → 2322.11.13）：

```
解除瞬间   0x6C602   bit14 ✔ bit17 ✔
冷却到期   0x48602   两位都清
再激活     0x6A602   bit13 ✔ bit17 ✔（无 bit14 = 不在冷却）
同一支隐形久了的船 0xA602  只剩 bit13（bit17 已被重算掉）
```

四支交叉验证（读 `+0x1268` 的 dword）：

```
fleet 1  隐形时      0x4A602      fleet 33554864 隐形  0x4A602   ← 完全相同
fleet 1  解除后      0x6C602      fleet 2502  已解除  0x6C602   ← 完全相同
fleet 2  工程船      0x158602     bit13/14/17 全 0
```

存档侧同一件事：舰队块 `properties={ … is_cloaked=yes … }` 是**按名字写的明文**，
`is_cloaked` 只在真时出现；解除后换成 `cloaking_cooldown=yes` + `dirty_cloaking_strength=yes`，
另加 fleet 层的 `cloaking_cooldown_start_date="2322.07.03"`。两边能逐条对上。

桥方法 `probe_fleet_flags { fleet_id, offset=0x1268, length=0x100 }`：回舰队基址 + 一串 u32
（**注意 `find` 回来的是 CFleet 基址，偏移要自己加**）。

## 方舟（Nomads）：一切都在 `CColony` 上

**结论**：方舟和行星的殖民地结构完全相同——同一个 `CColony`，只是**载体**不同：

```
行星   CPlanet + 0xE0  → CColony
方舟   CShip   + 0x100 → CColony      （CColonyCarrier 内嵌在两边的同一个相对位置，+0x40）
```

方舟**没有 CPlanet**，所以以前所有「planet_id → CPlanet → 字段」的读法在它上面全失效；
但改成「colony → 字段」之后两边通用。

### 实测出来的 `CColony` 布局

数组一律 `{vtable 8 / data 8 / cap 4 / count 4}` 共 0x18 字节：

| 字段 | 结构起点 | count 在 | colony 0 实测 |
|---|---|---|---|
| `pop_groups` | `0x18` | `0x2C` | 5 |
| `pop_jobs` | `0x30` | `0x44` | 37 |
| `districts` | `0x80` | `0x94` | 4 |
| `num_sapient_pops` | `0xFC8` | — | 52749 |
| `stability` | `0xFB8` | — | 10000000（×1e5 = 100.0%） |
| **飞升等级** | `0x110` | — | i32；方舟同样适用（本档三条方舟 = 5） |
| `stability` | `0xFB8` | — | 10000000（×1e5 = 100.0%） |

**SDK 陷阱（必记）**：`CColony::pop_jobs = 0x44`、`districts = 0x94`、`CDistrict::zones = 0x3C`
标的都是**条数**的位置，不是结构起点。**结构起点 = SDK 值 − 0x14**。照 SDK 直接读会把计数当指针。

### 往下的链（和行星一模一样）

```
colony.districts → CDistrict { type 0x20、zones 0x30（count 0x3C）、level 0x1B0 }
                 → CZone     { type 0x20、buildings 0x1A0（data 0x1A8、count 0x1B4） }
                 → CBuilding { type 0x20 }
```

### 两只探针

```
probe_colony { colony_id, offset, length }
    carrier（低 4 id / 高 4 类型）+ 名字（行星走 CPlanet::name，方舟走 CShip::name +0x110）
    + ship_colony_ref 自检 + planet_size（CShip +0x170）+ 一段 u32 窗口

probe_colony_structures { colony_id }
    整棵树：carrier、pop_groups / pop_jobs / districts 三个计数、num_sapient_pops、stability，
    以及每个 district（id / type / 中文名 / level）→ 每个 zone slot（zone 键 + 中文名）→ 每座建筑
```

实测（colony 0「ISS水精之室」，`science_arkship_tier_3`）：

```
pop_groups 5 · pop_jobs 37 · 区划 4 · 人口 52749 · 稳定 100%
district_ark_city     level 39  居住甲板   zone_default / zone_research_physics / zone_research_society
district_ark_generator level 1  反应堆阵列 zone_ark_reactor_physics
district_ark_basic     level 1  方舟甲板   zone_ark_waystation_trade
district_ark_military  level 1  军械工程   zone_ark_shipwright
```

四条方舟全部读通（colony 0 / 106 / 147 / 164），人口 52749 / … / 47845，与存档的
`pop_groups` 求和逐条对得上。

**已接进工具**：`stellaris_colonies` / `stellaris_planet_structures` 按 `carrier_type` 分支（planet → 行星对象，
ship → 方舟那条路），对上方舟时传 `colony_id`。

## 行星特征（deposit）：持有者字段在 `+0x30`，不是 SDK 的 `+0x28`

**特征不是持有者对象上的字段。** 方舟舰的存档块里 0 条 deposit/blocker，CShip 也没有这类成员 ——
特征待在独立的 `CDeposit` 表里，用 `deposit_holder` 指回持有者（存档写作 `{ type, id }`，
`type=2` 是舰，也就是方舟）。

**内存布局实测**（拿 `d_treasure_planet`「珍宝馆」验的）：

```
CDeposit
  +0x08  id                      33555075 等
  +0x18  type (ptr → 键在 +0x20)
  +0x28  ✗ 一个共享指针 —— SDK 标的 deposit_holder 在这里，读出来一串 deposit 同值
  +0x30  ✔ 真正的持有者：低 4 字节 type、高 4 字节 id
```

`dump_words` 抓到的原始窗口（deposit 33555075）：

```
+0x00 vtable ... +0x10 0x2000283(id) ... +0x30  2  0x1000DCA(=16780584)
```

`type=2 / id=16780584` 与存档里那条 `deposit_holder={type=2 id=16780584}` 逐字对上。

**桥方法** `probe_deposits { key, holder_type, holder_id, limit, dump_words }`：
扫 `CDeposit` 表（`sdk::db::CDeposit`，mgr +0x18 数组 / +0x20 容量 / 每槽 16 字节、对象在 +8），
按键片段和/或持有者过滤，回 id / 键 / 中文名 / 持有者 / 对象指针 / 可选原始窗口。

实测：4 条方舟（ship 0 / 4436 / 67110257 / 16780584）各有 **8 个珍宝馆**，与存档的 32 个逐条对上。

**飞升命令本身就吃 colony**：`CIncreasePlanetaryAscensionTierCommand` 的字段是 `ascend::colony`，
所以 `AscendColonyJson` 加个 `colony_id` 参数就支持方舟了（实测 5 → 6，回读 `colony+0x110` 同步）。

**殖民地队列明细**：`probe_colony_structures` 的 `queue` 直接用 `ExtractConstructionQueue(qid)`
（恒星基地队列 `GetStarbaseQueueJson` 用的同一把），所以每项带 `item_id`，`cancel_construction` 直接可用。


`probe_colony_structures { colony_id }` 现在也**顺带把特征带出来**（`features` / `features_count` /
`feature_holder_type`）：按 colony 的 carrier **完整 32 位** id 去 deposit 表比持有者，方舟 type=2、
行星按 planet id。**不要掩码到 24 位** —— 船 id 会超过，掩了会和行星 id 撞车（实测撞出过一颗行星的电弧熔炉）。
**id 之外还要比类型**：方舟 0 的 id 就是 `0`，只按 id 比会把行星 0 的 deposit 也算进来
（实测多算了一处「星界丝缕 +2」）。规则：carrier 是船要求 holder `type == 2`，是行星则排除 type 2。

方舟 0 实测 11 个特征：`d_treasure_planet` ×8、`d_nanites_medium`（纳米机器人复制器欧米茄）、
`d_living_metal_medium`（活体金属仓）、`d_virtual_power_medium`（虚拟发电站）各 1。

**行星的持有者 `type = 0`（方舟是 2；测试档现造 2 个珍宝馆验的）。行星也走这条**，而且**旧的 `ExtractPlanetaryFeatures`（入口是 CPlanet）会漏**：
planet 39 的 `planetary_features.summary.total_features` 读成 0，同一颗在 deposit 表里却有
1 个「星界丝缕 +2」（colony 1，`play 1` 实测）。所以插件侧的行星分支额外从 deposit 表补一段。

## 定时修正（timed_modifier）：容器、条目、键名

```
行星 CPlanet + 0x280 · 方舟 CShip + 0x2A0   CTimedModifierCollection
             （相对容器起点）+0x08 数据 / +0x10 容量 / +0x14 条数
条目 步长 0x28  +0x08 modifier 类型指针 · +0x10 days · +0x20 clear_on_owner_change
键名 类型对象 +0x178 的 CPdxString（长度 +0x188；≤15 内联 SSO，否则前 8 字节为字符指针）
```

**怎么定位的（可复用）**：SDK 里没有“加修正”的函数地址。先扫 `.text` 找引用 0x280/0x288/0x294 的
函数（167 个），再与常量 100000 取交集（101 个）——判别力不够。于是改**hook 序列化器**：
`CTimedModifier::CSerializer`（RVA 0x375FF0）在存档时对每条定时修正各走一遍，所以「存一次档」就能
把当前所有修正连同类型指针收齐（实测 512 条 / 60 种）；再对每种类型对象扫可读字符串，在 `+0x178`
命中键名。

桥方法：`install_timed_modifier_capture` / `get_timed_modifier_capture` / `clear_timed_modifier_capture`。

实测：`stellaris_planet_structures { colony_id: 1 }` → 严阵以待 `storm_hunkered_down_modifier` 永久、
展出的艺术纪念碑 `artist_monument_erected` 永久。**方舟实测**：容器在 `CShip + 0x2A0`（与行星的 0x280 不同，2026-11-23 用「扫一段对象找
{data, cap, count} 且首条 type+0x178 能解出键名」的语义扫描定下来的）。三条方舟实测 3 / 1 / 0 条，
与界面一致。

## 宇宙风暴：cluster 的偏移

内存里能直接读到「风暴现在在哪、半径多大、圈内哪些星系」，不必插值、不必读存档。

```
风暴表     base + 0x311BE90 是一次间接：读出的指针才是 mgr
           mgr + 0x18 数组指针 · mgr + 0x20 容量（本档 2048）
           槽步长 16，指针在 +0x08（空槽为 0）
风暴对象   +0x20 id · +0x2C path_index · +0x30 age · +0x3CC days_till_next
cluster    +0x180 objects 数组指针 · +0x188 容量 · +0x18C 条数（u32 系统 id）
           +0x198 x · +0x1A0 y · +0x1A8 visual_height · +0x1B0 origin · +0x1B8 radius
           坐标与半径都是 i64 定点，**÷100000** 得坐标单位（364.15845 / 30.25742 / 48.3835）
```

实测四条（与存档逐值一致）：

```
16777218 托尔巴吉拉戈 (364.16, 30.26) r=48.38  13 个星系
16777220 哈斯克约诺特 (-315.56, 61.76) r=49.51  7 个
16777221 托尔巴伊纳里姆 (-216.90, -10.60) r=33.05  3 个
16777223 托尔巴瓦斯库 (-19.62, 289.77) r=34.97  5 个
```

**SDK 的 `path = 0x348` 在这版上不对**：那里读出来是 `vtable + 两个字符串指针 + 长度 3 + 颜色`，
沿两个指针读到的内存以 345 / 109 开头紧接着是英文字符串，不是系统 id 数组；
在风暴对象前 0x1000 字节里也找不到 345、494 相邻的序列。所以 `probe_storms` 里
`current_system = path_data[path_index]`（stride 4）对 index > 0 就是错的——哈斯克约诺特读成 0 即此。
**判「星系在不在风暴里」别用 path：用 cluster.objects。** path 的偏移待重新定位。

## 走跃迁通道：`use_bypass` 的对端解析（2026-11-23 修）

`UseBypass` 以前要求调用方给 `to_system`，不给就直接拿 `0xFFFFFFFF` 去查星系表，
回报 **`Unknown system id: 4294967295`**——工具描述里写的「省略就自己找」从来没有实现。

现在省掉 `to_system` 时会自己找对端：取这条 bypass 的 `active_connections` 与 `linked_to`，
扫所有星系的 bypass 列表，看哪条与它相连，就用那条所在的星系做目标。
（实测：虫洞 56 省略 `to_system` → 自动解析到 `destination_bypass_id 55`、`to_system 339 博班克`。）


## 方舟与驿港：槽位、队列、清单

方舟（arkship）与驿港（waystation）都是普通 `CStarbase`，住在同一张 `db::CStarbase` 表里，
**等级键**区分：`starbase_level_{civilian,science,military}_arkship_{1,2,3}` / `starbase_level_waystation_{1,2,3}`
（`common/starbase_levels/06_arkship_levels.txt` / `05_waystation_levels.txt`）。

### 槽位条数在 +0xDC / +0xF4，不是 +0xD8 / +0xF0

等级对象里的槽位是 `CPdxArray`：`{vtable, data, capacity, size}`。模块在 `+0xC8`（data `+0xD0` /
capacity **`+0xD8`** / size **`+0xDC`**），建筑在 `+0xE0`（data `+0xE8` / capacity **`+0xF0`** / size **`+0xF4`**）。

旧代码读的是 **capacity**。常规基地两者相等，所以从没露出来；方舟上不等：

```
科研方舟 III   模块 capacity 9 / size 7   建筑 capacity 13 / size 12   （脚本 6+spinal_mount_1 = 7、8+lower_1..4 = 12）
军事方舟 II    模块 4 / 4                 建筑 capacity 9 / size 8
```

照 capacity 读会多出幽灵槽（`get_buildable_starbase_modules` 会为它们各跑一轮 IsValid，全是「可建 0 项」）；
往幽灵槽投模块，引擎收下、扣钱、什么也不建。**已装数组本身也可能比槽位多**（方舟实测 size 9 / 槽 7），
所以 `get_starbase_details` 的显示上限也改成「槽位条数」，已装数量按「键不为空」数。

### 队列：一条对外的（殖民地队列），一条隐藏的（`+0x880`）

```
CShip     + 0x104  → 殖民地队列     ← 界面显示的那条：建筑、陆军、方舟升级、**模块**都排这里
CStarbase + 0x880  → 隐藏队列       ← 引擎认、界面从不使用（投进去会造出来，但玩家看不到）
```

**证据**（2026-11-23，玩家指出「正常情况下都是用殖民地队列造模块」，随后 PostCommand 旁听证实）：

- 界面造模块时投的是 `CAddBuildableToQueueCommand`（vtable `0x23C7C80`），捕获字节里 **`+0x2C` = 6829**，
  即殖民地队列；模块落在 6829，类型 `starbase_module`
- 早先往 `+0x880`（6828）投过：引擎照收、照造（`instant_build` + 1 tick 后槽 0 的工程港真的换成停泊区），
  但界面上**看不到这条队列**——所以它是「隐藏队列」，不是正常玩法路径
- **队列由命令 `+0x2C` 决定，引擎不重定向**：先前以为「传哪条都落到 `+0x880`」，其实是
  `QueueStarbaseModuleJson` 先读了 `+0x880` 才退回载体队列，把自家 bug 当成了引擎行为

`CStarbase + 0x884`（船坞队列）在方舟上是 `0xFFFFFFFF`——造船不走它，走 `CStarbase::GetShipsBuildQueueRef`
（就是 `stellaris_buildable_ships` 用的那个，回载体队列）。

### 驿港：清单在国家的 waystation 网络里

```
CCountry + 0x2E68      waystation_networks  ref_array<TPdxRef<CWaystationNetwork>>
                       {vtable, data +0x08, cap +0x10, size +0x14}，元素是 4 字节 id
db::CWaystationNetwork（base + 0x311BDC8）  槽 16 字节、指针 +0x08、id +0x08
网络对象 + 0x330       waystations  ref_array<TPdxRef<CStarbase>>   元素也是 4 字节 id
         + 0x348       子网络            + 0x360 名字
```

本档：1 个网络、13 条驿港，`stats.starbases.used` 也是 13——**方舟不进这个计数器**
（脚本里 arkship 等级写了 `starbase_capacity_used = 1`，实测没算）。
驿港**不把星系算成本国的**（`get_galaxy_map` 里 owner 仍是别人），但可以在里面造巨构；
所以「本方有哪些基地」不能只看星系归属，游牧档上那条判据恒为 0 座。

- **常规那组按等级键剔除方舟 / 驿港 / 主宰（2026-11-23 修）**：第三组用「星系归属」列，
  会把停在我方星系里的别国方舟、驿港、主宰一起带进来（定居档实测 35 条里混了 4 条方舟 + 十几条驿港）。
  现在 `get_our_starbases` 第三组跳过 `level` 含 `arkship` / `waystation` / `juggernaut` 的条目：
  方舟与驿港各有自己的分组，主宰是舰不是基地。修后同一份档 122 → 75 条，游牧档仍是 4 / 27 / 0。

- **驿港只有模块槽**（2/0、4/0、6/0，建筑槽恒 0）：模块入队走同一条 `CAddBuildableToQueueCommand`，
- **驿港模块入队（2026-11-23 实测）**：走的是它**自己的建造队列**（`CStarbase.build_queue +0x880` 指向的
  独立队列对象，这次是 6980；方舟才是殖民地队列 6829，隐藏队列 6828 与这里无关）。
  投一条 `waystation_hydroponics_module`：`get_queue` 显示「队列 6980 · 1 项」，`instant_build` 下一 tick 进槽。

### id 里会混进标记位（两处，都会咬人）

| 字段 | 见过 | 处理 |
|---|---|---|
| `CStarbase.station`（站体） | 裸 `1393` 与 `0x04000000|1393` 都出现过（tier 2 的方舟） | 一律 `& 0xFFFFFF` 后查 CShip 表 |
| starbase 自己的 id（`+0x08`） | 裸 `144` 与 `0x01000000|144 = 16777360` 并存 | 报**对象自己的 id**；按 id 扫描时两边都掩码 24 位再比 |

第二条是 2026-11-23 踩出来的：驿港 144 的 id 实际是 16777360，`get_starbase_upgrade` 按裸 id 扫
找不到它（`starbase 144 not found`），但它明明在表里。六个按 id 扫 starbase 的函数都改成了掩码比较。

### 升级链：跳过了 next_level，直接硬编码

`CStarbaseLevelType` 没有可用的 `next_level` 字段偏移（SDK 里只有数据库 RVA），所以
`get_starbase_upgrade` 仍用一张表；2026-11-23 补齐：

```
waystation_1 → 2 → 3
{civilian,science,military}_arkship_1 → 2 → 3      military_arkship_champions_forge 无下一级
orbital_ring_tier_1 → 2 → 3      deep_space_citadel_1 → 2 → 3
```

实测 `get_starbase_upgrade { starbase_id: 16777360, dry_run: true }` → `waystation_2 → waystation_3`、
合金 225 / 360 天。

### 两个新 IPC 方法

| 方法 | 作用 |
|---|---|
| `get_our_starbases` | 三组清单：`arks`（殖民地 carrier 是舰 → 在 CStarbase 表里找 `station & 0xFFFFFF`）、`waystations`（上面的网络链）、`starbases`（星系归属 = 玩家）。每组一行：id / 等级 / 槽位 / 已装数 / 站体名 / 星系 |
| `get_queue { queue_id }` | 按 id 直读一条队列（`ExtractConstructionQueue` + 并行数）。诊断用：方舟两条队列、投递落点都靠它 |

**顺带记一条编译坑**：`ipc_server.cpp` 那条巨大的 `if (method == ...) else if` 链已经贴着 MSVC 的
块嵌套上限，**再加一个分支就 C1061（编译器限制：块嵌套太深）**。新方法要么并进已有的同类分支，
要么另起小函数——2026-11-23 的两次编译失败都是这个。

## 站体设计与防御平台

恒基的「武器配置」就是站体那条 `CShipDesign`，`probe_design_sections` 读三层：

```
核心（cores）   stage +0x30 数组 / +0x3C 个数      反应堆、雷达、火控、FTL 拦截器
段（sections）  section +0x50 数组 / +0x5C 个数    武器、装甲、护盾、舰载机
组件键         组件模板 +0x1B0，再交给 localize     MEDIUM_RED_LASER → 中型红激光
```

两个容易读错的地方：

- **核心层不在段里**：`SENSOR_1`（雷达系统）只在这条数组里能读到
- **按已装组件枚举，别按模板槽数**：有些段模板槽数是 0、却装着组件，按槽数会整段漏掉

`get_ship_designs` 对**基地设计**返回 `count: 0`（基地设计不进玩家设计库），所以基地只能走 `probe_design_sections`。

**防御平台是 `CShip`**，锚在星系上：三份 8KB 快照（有平台 / 拆掉 / 拆掉过 tick）逐字节相同，
说明平台不写在 `CStarbase` 对象里；`CStarbase::orbitals(+0x638)` 也是空的，站体里搜不到平台指针。

```
CShip + 0x988  设计号        CShip + 0x110 持久名
CShip + 0x9A0  站内编号（1 起，同站第几座）
CShip + 0x9C0  坐标（+0x20 = origin 星系 id）
```

**没解**：面板把平台列进恒星基地 UI 走的是哪条查询。不影响读数据（`find_ships` 扫 `CShip` 表就够）。

## 方舟当舰队读（`relation: "ark"`）与轰炸姿态

**方舟是一支 `shipclass_starbase` 的舰队**（`get_system` 的 `fleets[]` 里带 stance 那种）。模板驱动的
军事舰队列表看不到它，所以 2026-11-23 起 `get_military_fleets` 多扫一遍舰队库，把它按 `relation: "ark"`
列出来（玩家定：它有战力、能移动、能设姿态与轰炸姿态，按军用口径读最合适），并带上 `colony_id` /
`station_ship_id` / `bombardment_stance`。

**怎么认**（踩过两次）：

```
CShip + 0x100   是 **colony 引用**（低 4 字节 colony id、高 4 字节 type），不是指针
CColony + 0xF78 carrier：低 4 字节 id、高 4 type（1 = 舰）
```

只看 `+0x100` 非空会把 13 条驿港也当成方舟（驿站那艘船这一格也有值）；必须**回链核对**：
那个 colony 的 carrier 要等于这艘船（id 相同、type = 1）。

**轰炸姿态（内部名 ground support stance）**

```
档位库     TGameDatabase_CBombardmentStanceDatabase（0x31195D8）
           条目 +0x50（8 字节指针）、+0x5C（条数），键在对象 +0x20
           本档 13 档：selective / indiscriminate / raiding / armageddon / pox / plundering …
舰队字段   **CFleet + 0x458**（指向档位对象）。SDK 标的 0x968 在这一版不对 ——
           2026-11-23 拿两艘设了不同档的方舟逐字节 diff 才定下来
命令       CSetFleetGroundSupportStanceCommand（token 0x2DDC、vtable RVA 0x23C0320、
           size 0x30：fleet +0x20、stance +0x28）——界面设姿态投的就是它（PostCommand 旁听抓到）
```

改完**过一 tick 才生效**：当场回读仍是旧值。实测方舟 0 `selective → indiscriminate → selective` 两趟都通；
`armageddon` 被引擎拒（科技/政策没到）。

**顺手一条**：通用舰船动作对方舟同样成立——`move_ship` 把方舟连同殖民地一起搬走（实测 147 从萨奇斯搬到
奥拉斯再搬回，模块 / 建筑 / 队列全原样），`orbit_planet` / `ship_stance` / `cancel_ship_orders` 也都受理；
`set_cloaking` 被拒（方舟没有隐形装置，引擎不给原因）。

## 殖民地：内存布局总表

插件端只写「怎么用」，偏移一律在这里。行星与方舟共用同一个 `CColony`，差别只在载体与
两张行星专属表（编制表、`CPlanet` 那侧的位置）。

```
载体                CPlanet + 0xE0 → CColony      （行星）
                    CShip   + 0x100 → CColony      （方舟；CColonyCarrier 在两边同一相对位置 +0x40）
CColony.carrier     + 0xF78，8 字节：低 4 字节 id、高 4 字节 type（0 行星 / 1 舰）
殖民地列表          CCountry::owned_planets：数据 +0x08、条数 +0x14（元素是 colony id）
```

### 状态数值（×1e5 定点，行星方舟通用）

| 字段 | 偏移 |
|---|---|
| 稳定 | `CColony + 0xFB8` |
| 犯罪 / 格式塔反常度（同一字段） | `+0xFC0` |
| 舒适 / 舒适用量 | `+0xFA8` / `+0xFB0` |
| 住房 / 住房用量 / 住房余量 | `+0xF88` / `+0xF90` / `+0xF98` |
| 可就业人口（sapient pops） | `+0xFC8` |
| 飞升等级 | `+0x110` |

行星侧也可以用 `get_planet_details` 的 `kpi`（已封装，值同源）。

### 产出 / 维护 / 净（三张资源表）

```
CColony + 0xE80 produced · + 0xEA0 upkeep · + 0xEC0 net
  字段是「表对象指针」：表对象 +0x00 数据指针、+0x0C 条数
  条目 i64 定点，÷100000
资源名顺序：CStrategicResourceDatabase（base + 0x3159BF0）→ +0x14 条数、+0x08 指针数组；
            每个资源对象 +0x30 是键名（CPdxString，≤16 字符内联）
```

### 逐物种人口

```
CColony + 0xF68 数组指针 · + 0xF70 条数
  条目步长 12：+0x00 人数（u32）、+0x08 物种 id（完整 id）
物种名：base + 0x311BD58 取库（+0x18 数组 / +0x20 容量，槽 16、指针 +8）
  对象 +0x10 是 id（校验低 24 位）、+0x60 是 CPersistentName.key
```

> 2026-11-23 用 `get_planet_details.population_breakdown`（同一份数据、行星侧已验证）对过 planet 39 的 9 种，逐条一致。

### 岗位

```
CColony + 0x38 数组指针 · + 0x44 条数（元素 4 字节 handle，handle & 0xFFFFFF 是槽号）
CPopJob 库：base + 0x311BDA8 → +0x18 数组 / +0x20 容量，槽 16、指针 +8
CPopJob 对象
  +0xF8   在岗（i64 ÷1e5）
  +0x100  编制
  +0x108  加成
  +0x110  上限定（玩家设的上限；-1 = 没设）
  +0x18   岗位类型对象 → +0x20 键名；+0xaa0 → 阶层对象 → +0x20 阶层键名
```

**行星专属的编制表**（方舟没有 `CPlanet`）：`CPlanet + 0x788` 指针 / `+0x790` 条数，
每条 16 字节 `{数值 × 100000 (i64), 类型 id (u32)}`——表里混着非岗位的修正项，按类型 id 过滤，
细节见「岗位编制与在岗」一节。

> 方舟 0 实测（2026-11-23，玩家给的界面数值逐项对上）：活体陈设 15472/18750、
> 复制者 0/0（编制 400、上限定 0）、维护子 1/∞（等效 1.9，界面显示 2）、总在岗 45112。

### 修正 / 特征 / 队列（原有小节，这里只列入口）

| 找什么 | 位置 |
|---|---|
| 修正容器 | 行星 `CPlanet + 0x280` · 方舟 `CShip + 0x2A0`（条目 0x28、键名在类型对象 `+0x178`） |
| 地块特征（deposit） | 持有者类型 +0x30（行星 `type=0`、舰 `type=2`），见「行星特征」一节 |
| 建造队列 | 行星 `CPlanet + 0xE4` · 方舟 `CShip + 0x104`（队列 id；明细走 `ExtractConstructionQueue`） |
| 方舟所在星系 | `CShip + 0x9C0 + CCelestialCoordinate::origin` |

## 方舟殖民地：动作命令与字段

方舟的动作命令大多吃 **`carrier`** 而不是 planet id —— 与 `CColony::carrier`（+0xF78）同一个编码：

| 命令 | 吃的字段 | 方舟怎么给 |
|---|---|---|
| 行星决议 `CEnactDecisionCommand` | `carrier`（8 字节：低 4 id、高 4 type） | 直接抄 `CColony::carrier`：方舟是 `{ship_id, 1}`（colony 0 → `0x1_00000000`） |
| 飞升 `CIncreasePlanetaryAscensionTierCommand` | `colony` | colony id，行星方舟通用 |
| 拆区划 `CDestroyDistrictCommand` | `colony` | colony id |
| 建造/取消（区划、建筑、队列） | 队列在**载体**上：`CPlanet + 0xE4` / `CShip + 0x104` | 按 carrier 取队列 id |

**陆军建造**（`CBuildableArmy`）：`+0x08` army_type 指针、`+0x10` species、`+0x18` colony、
**`+0x20` starbase 必须留 `0xFFFFFFFF`**（填基地 id 反而被拒）。常规部队要带主族，特殊部队
（`machine_assault_3` 巨型战体）species 必须是 `0xFFFFFFFF`——引擎自己那条就是这样。

**地块特征（deposit）**：不在持有者对象上，而在 `CDeposit` 表里；持有者在 **`CDeposit + 0x30`**
（低 4 字节 type、高 4 字节 id，`type=2` 是舰）。SDK 标的 `+0x28` 是共享指针，照它读永远是同一个值。

**两个实现坑**（改桥时必看）：

1. **IPC 没转发 `colony_id`**：方法签名加了参数、函数体也改了，但 `ipc_server.cpp` 的分支还在传
   `planet_id`，于是走了 else 分支拿到 `planet 0`
2. **JS 的 falsy 判断**：`if (!args?.colony_id)` 会把 colony 0 当成“没给”；凡 id 都用
   `typeof x === 'number'` 判

**方舟舰的识别**（补记，2026-11-23）：`find_ships` 里能看到方舟（本档 `ISS水精之室`，ship id 1、
design 184549385），但**这个 design 不在 `get_ship_designs` 那张表里**——方舟另有一套设计，
所以别想从设计表按 `ship_size` 反查方舟，只能从 `CColony.carrier` 这条链走。

## 科研：科技表的内存布局与定位过程

**已研究列表**（`CTechnologyStatus`，SDK 的 `tech_status`）：

```
CCountry + 0x1620            CTechnologyStatus
  +0x18                      vector 头
  +0x20                      数据指针
  +0x28                      容量（本局 63）
  +0x2C                      条数（本局 46，与存档 tech_status 的条数一致）
每条 0x28 字节：
  +0x08  level（i32，已研究次数；循环科技读真实值）
  +0x10  CTechnology*（键在科技对象 +0x20，与 get_research_state 用的是同一个字符串）
```

实测 2222.01.01：内存读出 46 项，与存档解析出的 46 个键**集合完全相同**（顺序不同：内存按写入顺序、
存档按键名）。代价从「存盘 + 解析约 2 秒」降到 **21 ms**。

**循环科技**（2026-11-21 补测，此前「`level` 只见过 1」的说法作废）：`+0x08` 就是已研究次数。
本档 351 项里有 12 项循环科技（能量射速 ×89、能量伤害 ×62、舰载机伤害 ×40、地块能量 ×39、
舰载机射速 ×35、地块矿物 ×15，另 command_limit / naval_cap / starbase_capacity / building_cost /
building_time 各 ×5、lcluster_clue ×3）。

**定位过程（可复用）**：先做只读探针 `probe_tech_status {offset, length, dump}`——用
`CTechnologyDatabase`（`+0x311B6E8`，数组 `+0x50`、条数 `+0x5C`、键在 `+0x20`）把 679 个科技对象
逐一认出来，再扫 `CCountry` 的 0..0x3000，看哪几个 qword 落在这些对象上。结果全对象只有一个，
且在 `+0x178`（`always_available_tech`）——**说明已研究列表不是存指针的**；真正的数组在 `+0x18`，
靠「容量 63、条数 46」与存档对账才认出来。教训同岗位那次：**先拿已知真值去反查**，别指着一堆指针猜结构。

> 市场那边 `IsResourceUnlocked` 早就在走这张表（0x28 步长、指针在 +0x10，都对），
> 只把 `+0x28`（容量）当成条数——读多几条而已，不影响判定。

完整流程与踩过的坑：先找谁在读它 → 桥内探针 → 指针匹配做否证 → 找容器四件套 → 两条来源对集合 → 接线顺序。

## 参考仓库

**桥的本体来源**：**[Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp)**（MIT）——
本桥就是它改的。上游 29 个工具（科研、舰队、领袖、事件、物种…）全在，
`stellaris_bridge.dll` 的 IPC/命令构造/主线程队列/UI 钩子都是它的骨架。

最值得抄的是它的 `tools/sdk_dumper/`：

```
dump.py           一条命令重生成 stellaris_sdk.hpp（约 1.5 分钟）
win_extract.py    从 exe 里恢复偏移；token 名 → Windows 编号的重映射
anchors.py        buildable vtable 的定位算法（token getter + 删除析构断言）
```

**换游戏版本时**：`& $python tools/sdk_dumper/dump.py --skip-linux`（`$python` 见本机环境备忘录），
但注意 `globals.py` 需要 `source/stellaris_4.5_source.cpp`，缺了会少一批 `glob`/`db` 常量。

**[Yidhar/stellaris-Launcher](https://github.com/Yidhar/stellaris-Launcher)**（`stl`）：
插件规范与加载器——本机的 `stl.exe` 就是它，插件清单/playset 的格式看这里。

**原生命令研究的宝库**：**[V2t-Nostalgia/OGAS](https://github.com/V2t-Nostalgia/OGAS)**
（原 Imperial Auto Governor，GPL-3.0）。它不自己拼 buildable，而是**调用游戏自己的生产者函数**，
这条思路是关键转折。必看几份：

| 文件 | 内容 |
|---|---|
| `docs/runtime/NATIVE_COMMAND_PRODUCER_DISCOVERY_4_5_2.md` | 五类领域（科研/建造/移动/交易/外交）的生产链，含 `NConstruction::CreateAddBuildableCommand` 的完整契约 |
| `docs/runtime/HUMAN_AI_COMMAND_DISCOVERY_4_4_6.md` | AI 实际提交的 61 种命令及其构造器签名；含 46 次 `CBuildableStarbaseModule` 入队记录 |
| `services/stellaris_native_runtime/command_producers.cpp` | 生产者调用的实现（含 `ordinary_buildable_vtable` 校验），可照抄的 ABI 处理 |
| `.../versions/generated/stellaris_4_5_2_windows_sdk.hpp` | 4.5.2 的 Windows 命令表，可与本机 4.5.4 交叉比对 |
| `.../tools/sdk_recovery/vendor/linux_tokens.json` | token 名表（`buildable_starbase_module` 在这里） |

**注意**：GPL-3.0，参考思路可以，别直接搬进本项目的 MIT 桥。

**[V2t-Nostalgia/OGAS-research-evidence](https://github.com/V2t-Nostalgia/OGAS-research-evidence)**
带签名的证据归档，原始日志/哈希在那里。**[TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)**
——上游用它钩 `IDXGISwapChain::Present`。

**这次从它们身上拿到的三件事**：

```
1. token getter 法：mov eax,<token>;ret 在 vtable 槽 16 → 定位 buildable vtable
2. Windows token 要重新编号：buildable_starbase_module 是 0x3E01（Linux 的 0x3DF8 不存在）
3. 别自己拼 buildable——先找游戏的生产者函数，它会顺手把 vtable 和字段都填好
```

## 阶段弹窗与视图：控件树点不到，但路可以绕开

三类阶段弹窗（首触 / 考古 / 裂隙）是同族的自定义视图，不是事件窗：

```
CArchaeologyView   vtable RVA 0x2417A38   idler+0xC68   构造 0x1135C60
CFirstContactView  vtable RVA 0x2418238   idler+0xC90
裂隙视图（类名未定） vtable RVA 0x2418A98   idler+0xCD0
```

- 每类**只有一个实例**，按需复用；`list_views` 可列出（`+0x6D0 & 0xFFFFFF` = 当前绑定的对象 id，
  考古视图上就是遗址 id）
- **不是 `CEventWindow`**：`stellaris_events` 恒为空；按 `+0x9E8/+0x98/+0xB8` 签名扫 1000+ 个可达对象 0 命中
- **选项按钮不在视图的窗口树里**：视图名下 9 个窗口只有面板控件（site_name / 难度 / 线索 / 日志 / 阶段），
  剧情窗（图片 + 正文 + 选项按钮）不可达；控件文本也不落对象（绘制时按本地化键取）
- **`CSelectEventOptionCommand` 手填用不了**：`event`/`scope` 要真实指针，喂整数直接
  `IsValid failed: exception 0xC0000005`——但它**不用手填**：把 `ope` 交给 `0x8E4550`，
  引擎自己把那 0x1A0 字节填满
- **视图里的「关闭」按钮 = 写 `CFirstContact + 0xC0`**
- `common\button_effects` 只有 2 个文件，无考古/首触相关条目
- 结论：**视图控件树里没有可点的入口**；但「选选项」已经不需要视图——走事件那条

**未解**：`events` 数组（`+0x310`）的语义；阶段之间怎么串（数据文件里没有 `next_stage`）。

> 逐条对账、界面原始记录、两次快照对比、非 GUI 的全部死路，都是当时的工作笔记（未随仓库发布）；
> 非 GUI 路的解出过程与两次实测同样没有随仓库发布，结论见本节。

## 情报日志三类：异常 / 特殊项目的字段与命令

**异常**：`CCountryEventManager.anomalies`（`+0xF0`，数组是**行星引用**），逐条读行星的 `CPlanet::anomaly`
（`+0x4B0`）——行星只在异常尚未研究时挂着它，研究完就变成空对象、自动从列表消失。

```
名字（DISTAR_ELECTRO_CAT 这类键）   anomaly 对象 +0x20，经 localisation 取名
难度（1–10 档）                    anomaly 对象 +0x60
所在天体 / 星系                     行星对象与其 coordinate.origin
```

「正在调查」**不在异常对象里**：对比过十个实例，`+0x60` 是难度（2–6 分布）、`+0x30` 是递增序号，
而进度从 0% 涨到 4% 之后对象 `0x00–0xE0` 逐字节未变。它记在舰队状态字符串背后：

```
CFleet + 0x2C8   当前指令对象
                   +0x18 = 指令 token（research_anomaly_order = 0x2CF0）
                   +0x28 = 目标天体 id
                 （无指令时指向一个共享的空占位对象，token 0）
CFleet + 0x2D8   排队中的指令数组 data
CFleet + 0x2E4   条数 —— 就是状态里的「+N命令」
```

依据：反汇编 `CFleet::BuildOrdersString`（RVA `0xC7B960`），它对 `[rbx+0x2D8]` 与 `[rbx+0x2E4]`
做 `rdi … rdi+rax*8` 的循环，每条 `call [vtable+0x128]`。探针 `probe_fleet_orders` 就是把 `+0x2C8` 这层读出来。

`research_anomaly` 命令：`CResearchAnomalyFleetOrderCommand`（token `0x2CF0`，`fleet +0x20` / `planet +0x24` /
`queue +0x28`）。星系版 `research_anomalies` 是 `CFleetResearchAnomaliesCommand`（`fleet/system/queue`），
没有天体字段——引擎挑的是**舰队当前环绕那颗天体**上的异常（实测：梅萨特罗斯 BI/CIa 两条，环绕哪颗就研究哪颗，
所以星系版没有接成工具；插件用的是点名天体的那一版（`stellaris_research_anomaly`）。

**特殊项目**：`CCountryEventManager.special_project`（`+0x60`）→ `CSpecialProjectInstance`。

```
id          +0x1C
days_left   +0x20      -1 = 不限时
项目定义    +0x28      键在 +0x18（如 AMOEBAS_PACIFICATION_PROJECT）
位置        +0x38      空间引用 {vtable, type +8, id +0xC}
status      +0x1C8
残骸        +0x30、species / convert_to（残骸类、物种改造类用）
```

两条命令：`CResearchProjectCommand`（`0x2D8F`，`country +0x20` / `special_project +0x24`）、取消
`CCancelProjectCommand`（`0x31AC`）；需舰船的走 `CCollectDataFleetOrderCommand`（`0x31F5`：`fleet +0x20` /
`galactic_object +0x24` / `id +0x28` / `queue +0x2C`）。
