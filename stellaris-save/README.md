# stellaris-save

从**存档**读游戏状态，不碰游戏进程。

## 什么时候读存档

内存侧已经能直接读母港、恒星基地、船坞与岗位（`stellaris_home_base` / `stellaris_starbases` /
`stellaris_starbase_details` / `stellaris_planet_jobs`）。**存档不是唯一路径**，它的长处是另外三样：

- **一次拿全**：全银河的舰队、每个国家的设计、所有殖民地，都在一个文件里
- **不需要游戏在跑**：翻历史档，看两次保存之间发生了什么
- **能与内存对账**：同一件事两条来源各读一遍，对不上就说明有一边读错了

代价也明确：数据是**保存那一刻**的；要看现状得先存一次档（写临时档约 2 秒）。

存档里这些都是明文：

```
fleet_template={
  0={
    fleet=6
    home_base={ orbitable={ starbase=0 } }     ← 母港
  }
}
starbase_mgr={ starbases={ 0={ level=... type=... modules={ 0=shipyard ... } station=0 } } }
galactic_object={ 106={ name={ key="监牢" } starbases={ 0 } } }
```

读它没有副作用、不会崩，也不依赖游戏进程。

## 现在能回答什么

```
python save_report.py
```

```
存档：…\save games\_1449518320\2203.01.12.sav
版本：Cygnus v4.5.4   日期：2203.01.12

                    舰队    id    模板  母港
  MOL4_FLEET_Hurricane     6     0  监牢（starbase_level_starport／sshipyard，船坞 1）
    MOL4_FLEET_Tsunami   250  16777220  监牢（…，船坞 1）
  ART1_FLEET_Urtaka-Sa    10     1  SPEC_Themlar_system（…）
```

- 每支舰队的**模板 id** 与**母港**（星港所在星系、等级、类型、船坞数）
- 舰队名是本地化 key（`MOL4_FLEET_Hurricane` 就是“飓风”）；显示名要查游戏本地化
- 星港的**显示名**（“监牢-A 空间站”）是运行时按名字模板拼的，存档里只有模板与变量；
  要还原得实现 `STARBASE_STATION_NAME_FORMAT_*` 的拼装，暂未做

## 结构

| 文件 | 作用 |
|---|---|
| `paradox_script.py` | 按**区段切片**读存档：`section()` / `blocks()` / `value()` / `sub()`，不做整棵解析 |
| `save_report.py` | 上面那份报告的组装：模板 → 母港 → 星港 → 星系 |
| `tech_report.py` / `colony_report.py` / `jobs_report.py` / `queue_report.py` / `design_templates.py` / `zone_housing.py` | 各自一份报表 |
| `federation_report.py` | **联邦现状**：每个联邦的类型 / 经验 / 团结度 / 当前 12–17 条法律（`laws` 是明文键值） |
| `community_report.py` | 星海共同体：成员 / 议会 / 台上议案 / 队列 / 已通过（插件在内存抓不到对象时回退到它） |
| `colony_effects.py` | 殖民地产出的逐项效果（插件 `effects: true` 调它） |
| `subject_report.py` | 附庸关系与协议条款 |
| `specimen_report.py` | **谁真的持有哪些标本**（`--country N` / `--all` / `--json`）。**关键**：标本实例块里的 `owner=` 是**发现者**；持有者要看各国 `standard_grand_archive_module`（`exhibits` + `storage_slots`）引用的实例 id——用错这个字段会误判成「交易没交割」 |

为什么不整棵解析：9 MB 的 gamestate 嵌套太深，朴素解析器会撞 Python 的递归上限，而且
解析失败就是全盘失败。切片法只取需要的几个顶层区段，坏一个字段只损失一个字段。

## 扩展点（下次要加东西就照这几个位置改）

1. **新区段**：`ps.section(text, '国家名')`，例如 `country`、`ships`、`construction`
2. **新字段**：`ps.value(block, 'key')`（单值）或 `ps.sub(block, 'key')`（子块）
3. **新报表**：在 `save_report.py` 里加一个函数，像 `starbase_label()` 那样
4. **舰队别的属性**：`ship_class`、`fleet_size`、`movement`、`current_order` 都在 `fleet` 块里

候选的下一个报表：

- 每支舰队的**航速/当前位置**（`fleet.movement`）
- 星港的**模块与建筑清单**（`starbase.modules` / `buildings`）
- 舰船设计 → 组件的完整列表（`ship_design` + `ship_design_implementation`）
- 全局资源与产出的存档快照（用于和桥的数据对照）

## 联邦现状（`federation_report.py`）

```
python federation_report.py            # 最新存档，可读文本
python federation_report.py --raw      # JSON（给工具用）
python federation_report.py 路径.sav
```

存档里的形状（`federation=` 段）：

```
federation= { 0= { name={ key="%ADJ%" variables={…"Pan-Galactic"…"Information"…"Federation"…} }
                    federation_progression=
                    { federation_type="research_federation"  experience=9713.32  cohesion=89.6
                      laws={ law_category_succession_term=succession_term_years_40
                             law_category_centralization=centralization_high … } } } }
```

**存档读法律的定位**：投票通过那一瞬 `cmd_capture` 只抓到 `CDiploActionVoteCommand`（vtable `0x23BA318`）——
**切换法律不走命令系统**，是引擎内部直接改数据，所以抓不到「谁改了、改成什么」。
但**当前值是能从内存读的**：`federation_progression` 里那个法律容器（`+0x2E8`），
桥的 `get_federation_laws { fed_id }` 与插件的 `stellaris_federations` / `stellaris_federation_laws` 都走它。

那这个脚本还有用——**两条路互为对账**，而且它能一次列出全银河所有联邦（内存那条要逐联邦问）；
存档里的 `laws` 是明文键值，最直观。早期试过的 `progression+0x2D0` 是「待表决 / 最近一次表决」的记录槽，
不是当前值——那一版的结论是「读不到」，后来才找到真正的容器。

- 输出：每个联邦的 id / 名字（`name` 段里的 key 串拼出来，如 `Pan-Galactic Information Federation`）/
  类型 / 经验 / 团结度，以及 12–17 条法律（类别名译成中文，值保留脚本键）
- 名字是拼的：相邻的 `key=` 串按顺序接起来，`%ADJ%` 之类的占位符跳过
- 已知：`law_category_challenge_type` 没有本地化键（游戏自己返回原键）

## 注意

- 只读：脚本不会写存档
- 版本：存档顶部有 `version="Cygnus v4.5.4"`，结构若变，`ps.section()` 取不到就是空串，不会崩
- 存档是 zip：`.sav` 里含 `gamestate` 与 `meta`；解压后的明文也可以直接喂给它

## 舰船设计模板库（10 条）

**这是「换区段」用的骨架**：段留着、可选部件一条不写、核心落初始档。局内想换成哪套配置，
再由 `create_ship_design` / `update_ship_design` 按科技把部件填进去。

模板文件在 `templates/`，一条一个 `.txt`，`manifest.json` 是索引。
（`templates/legacy/` 里是旧的九份 mod 原设计——满配、有部件——只作配置参考，不在 manifest 里。）

**清单以 `manifest.json` 为准**（id / 名字 / 舰级）；每条模板一个 `.txt`，`legacy/` 下那九份只作配置参考。

- 文件名 `<设计id>_<名字>.txt`，`manifest.json` 里是 id / 名字 / 舰级
- 船的本体部件 = 0（可选槽全空）；兽群 7 / 4 / 6（本体器官必须留，只清 `mutation_section` 的 `ANY_1..6`）
- 这些块**全部是游戏自己写的**（在舰船设计器里存的空设计），只搬不改

## 注入存档：格式上踩过的坑

```
python design_templates.py inject <目标存档> <输出存档> <模板...>
```

1. **id 必须用游戏分配过的那个**。原块的 id 一律不动。
   自己造的 id（`max(taken)+1`、`0x0E000071`、`0x15001300`）全部被拒，报
   `Malformed token: yes` / `Unexpected token: role`，然后崩溃。把同一条块**只换 id** 就会崩，
   **只换区段名**（id 不动）就没事 —— 这是最容易走错的一步。
2. **名字（key）在同一个国家里不能重**。撞了就加后缀（`空匣泰坦二`）。
   字面量名字要带 `literal=yes`（游戏自己写的时候就是这样）。
3. **必须同时登记进玩家国的 `ship_design_collection`**，否则设计在库里但列表里看不见。
4. **插入位置**：`ship_design` 段最后一个设计之后、段尾 `}` 之前。
   注意别插到一串 `<id>=none` 的空位后面 —— 有的存档段尾就是这种空洞（2322 那档）。
5. **块首不能带空行**。旧代码用 `^\s*<id>` 取块，`\s` 会把换行也吃掉，导出的块前面多一个空行；
   已改 `^[ \t]*`。
6. **空槽合法**（游戏自己就这么写），但兽群例外：本体段部件必须留，`upgrades_to` 也不能丢。
7. 存盘前自检：花括号平衡、id 全局唯一、key 全局唯一、块在段里、id 在列表里。

**验证结果**

| 场景 | 结果 |
|---|---|
| 开局档（2200.01.01）注入 10 条原块（id 不动） | ✅ 载入正常，13 条设计在列表里，区段与空槽都对 |
| 同一份档里再复制一条、**只换 id** | ❌ 崩在 `literal=yes` |
| 只换某条现成设计的区段名（id 不动） | ✅ 载入正常，槽位跟着新区段变 |

**所以**：跨存档搬模板 = 注原块（id 不动）；局内换段 = 改 `template="..."` 那一行。
两条都别碰 id。