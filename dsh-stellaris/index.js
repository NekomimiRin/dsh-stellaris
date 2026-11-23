/**
 * dsh-stellaris — the Stellaris side of the harness.
 *
 * Registers 187 `stellaris_*` tools on ctx.tools — most are thin pass-throughs to a
 * bridge method of the same name; the rest have a hand-written execute (explore,
 * fleet_composition, home_base, colonies, list_colony_designations, set_colony_designation,
 * merge_fleets, disband_fleets, queue_starbase_module, panel, inbox, the letter tools
 * and the build/district group).
 * 工具数只认 `scripts/plugin.test.mjs` 的输出，别再往这行手抄一个数。
 * 存档读法（colony_report / jobs_report / tech_report）已从插件移除，只在 README「存档审计（手动）」里留脚本用法。
 * The tail of this file is the auto-check that runs with the game heartbeat.
 *
 * The game-side pieces (stellaris_guiexpand + dsh_panel.dll) are ordinary
 * Stellaris plugins; this package only talks to the launcher and to files.
 */
import { execFile } from 'node:child_process'
import { appendFileSync, rmSync, existsSync, readFileSync, readdirSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { promises as fs } from 'node:fs'
import net from 'node:net'
import { homedir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

/** Cordis 诊断用的插件名，也是 cordis.patch.yml 里 insert 的 name。 */
export const name = 'dsh-stellaris'

/** 等 tools / schedule / sessions 就绪再 apply。 */
export const inject = ['tools', 'schedule', 'sessions']

/* 每个模块实例一个短 id（pid + 4 位随机）：日志里带上它，就能看出两条 tick 到底来自
   一个实例还是两个。模块级常量，重复 apply 落到不同模块实例时它会不同。 */
const INSTANCE_ID = `${process.pid}.${Math.random().toString(36).slice(2, 6)}`

const DEFAULTS = {
	stlPath: path.join(process.env.LOCALAPPDATA ?? '', 'Programs', 'Stellaris Launcher', 'stl.exe'),
	pluginDir: path.join(homedir(), 'Documents', 'Paradox Interactive', 'Stellaris', 'plugins', 'dsh-panel'),
	pipe: '\\\\.\\pipe\\stellaris_mcp_bridge',
}

/* 自动化那条提醒：由面板的心跳文件驱动，游戏在就建，游戏走就删 */
const AUTO_PROMPT = [
	'这是一条由游戏心跳唤起的自动检查：',
	'先看指令：调 stellaris_inbox；若返回 NEW，那是玩家在游戏里的输入框对我说的话。',
	'看得懂就照做（派船、选科研、看状态这类；不确定或有风险时，只在面板里回问，不要动手），然后调 stellaris_inbox(ack:true) 标记已读。',
	'先读 plugins\\dsh-panel\\auto.txt，内容是 0 就什么也不做、简短结束。',
	'是 1 时调用 stellaris_state（method=get_active_events）；没有弹窗就什么也不做。',
	'去重：对每个弹窗的事件 key 调 stellaris_event_mark（dry_run: true 只判断）；返回 repeat 的跳过。',
	'要写就用 stellaris_letter_write 写一句 1–3 句、40 字内的话，',
	'写完调 stellaris_event_mark 记下这个 key（30 分钟内再来同一条就不再写）。',
].join('')
const AUTO_TITLE = '事件弹窗时写便笺'
const AUTO_EVERY_SECONDS = 120
/** 同一个事件 key 在这么久之内不再重复写第二封 */
const AUTO_EXPIRE_MINUTES = 30

function run(exe, args, timeoutMs = 60000) {
	return new Promise((resolve) => {
		execFile(exe, args, { timeout: timeoutMs, windowsHide: true }, (error, stdout, stderr) => {
			const text = `${stdout ?? ''}${stderr ?? ''}`.trim()
			resolve({ ok: !error, code: error?.code ?? 0, text })
		})
	})
}

/** One JSON-RPC call on the stellaris-mcp bridge pipe (newline delimited). */
function pipeCall(pipe, method, params, timeoutMs = 8000) {
	return new Promise((resolve, reject) => {
		const sock = net.connect(pipe)
		let buf = ''
		let settled = false
		const finish = (fn, value) => {
			if (settled) return
			settled = true
			clearTimeout(timer)
			try { sock.destroy() } catch {}
			fn(value)
		}
		const timer = setTimeout(
			() => finish(reject, new Error(`no answer from ${pipe} (is stellaris-mcp loaded and the game running?)`)),
			timeoutMs,
		)
		sock.on('connect', () => sock.write(JSON.stringify({ jsonrpc: '2.0', id: 1, method, params }) + '\n'))
		sock.on('data', (d) => {
			buf += d.toString('utf8')
			const i = buf.indexOf('\n')
			if (i < 0) return
			const line = buf.slice(0, i).trim()
			try { finish(resolve, JSON.parse(line)) } catch (e) { finish(reject, e) }
		})
		sock.on('error', (e) => finish(reject, e))
	})
}

/**
 * 桥报告的 military_power 只有面板值的约 1/3.05（三组数据实测）
 * （285→93.47、56→18.21、109→35.70）。这里只**附加**一个换算字段，
 * 不覆盖原值：原值是什么仍然看得见。
 */
const POWER_SCALE = 3.05

export function withDisplayPower(value, seen = new WeakSet()) {
	if (Array.isArray(value)) {
		for (const item of value) withDisplayPower(item, seen)
		return value
	}
	if (!value || typeof value !== 'object' || seen.has(value)) return value
	seen.add(value)
	for (const [key, raw] of Object.entries(value)) {
		if (typeof raw === 'number' && /^(military_power|total_military_power)$/.test(key)) {
			value[`estimated_display_power`] = Number((raw * POWER_SCALE).toFixed(2))
		} else if (raw && typeof raw === 'object') {
			withDisplayPower(raw, seen)
		}
	}
	return value
}

/** ToolRuntime 要求每个工具声明 output：schema 描述返回值，render 把它变成给模型看的段落。 */
const textOutput = {
	schema: {
		type: 'object',
		properties: { content: { type: 'array', items: {} } },
		required: ['content'],
		additionalProperties: false,
	},
	render(_args, value) {
		const parts = Array.isArray(value?.content) ? value.content : []
		const text = parts.map((part) => (typeof part?.text === 'string' ? part.text : '')).join('\n')
		return [{ type: 'text', text }]
	},
}

/** A text-only result, which is all these tools ever produce. */
const text = (value) => ({ content: [{ type: 'text', text: value }] })

/** seen-events.json 与插件同目录：记录每个事件 key 上一次写信的时间 */
const seenEventsPath = path.join(path.dirname(fileURLToPath(import.meta.url)), 'seen-events.json')

function readSeenEvents() {
	try {
		const parsed = JSON.parse(readFileSync(seenEventsPath, 'utf8'))
		return Array.isArray(parsed?.seen) ? parsed.seen : []
	} catch {
		return []
	}
}

/** 管道返回的是 JSON-RPC 信封，这里只要 result */
async function rpc(pipe, method, params) {
	const answer = await pipeCall(pipe, method, params)
	return answer?.result ?? answer
}

/** 星系里有没有不是自己的舰队——探索时遇到就要避开 */
function hostileIn(system) {
	const fleets = Array.isArray(system?.fleets) ? system.fleets : []
	return fleets.some((fleet) => fleet && fleet.own === false)
}

/** 把一个星系读成探索逻辑要的形状 */
function summarise(system, id) {
	return {
		id,
		name: system.name,
		surveyed: Boolean(system.surveyed),
		owner: system.owner_name || '',
		hostile: hostileIn(system),
		fleets: (system.fleets ?? []).map((fleet) => ({
			id: fleet.id,
			name: fleet.name,
			own: fleet.own !== false,
			class: fleet.ship_class,
		})),
		neighbours: (system.hyperlanes ?? []).map((lane) => ({ id: lane.to, name: lane.name, length: lane.length })),
	}
}

export function apply(ctx, config = {}) {
	/* 恒星基地等级的键 → 中文（面板上的叫法，见 README「术语规范」） */
	const LEVEL_CN = {
		starbase_level_outpost: '哨站',
		starbase_level_starport: '星港',
		starbase_level_starhold: '星垒',
		starbase_level_starfortress: '星堡',
		starbase_level_citadel: '擎天堡',
	}
	const cnOf = (key) => LEVEL_CN[key] ?? key
	/* 启动时留一行痕迹：插件到底有没有被 loader 装上，一看这个文件就知道 */
	const here = path.dirname(fileURLToPath(import.meta.url))
	const marker = path.join(here, 'loaded.log')
	const mark = (line) => {
		try {
			appendFileSync(marker, `${new Date().toISOString()} ${line}\n`, 'utf8')
		} catch {}
	}
	try { mark(`enter (tools=${typeof ctx.tools})`) } catch {}

	const stl = config.stlPath || DEFAULTS.stlPath
	const dir = config.pluginDir || DEFAULTS.pluginDir
	const letter = path.join(dir, 'letters.txt')
	const logfile = path.join(dir, 'dsh-panel.log')
	const disposers = []

	// 这些工具桥端存在、插件已接，但没有在本局实测过。描述里必须带上这一句——
// 运行时模型读的是 description，看不到 README 里的 ⚠ 列。
const UNVERIFIED = new Set([
	// 2026-11-23：最后四只（ship_follow / ship_mia / use_bypass / find_path）已实测，集合清空
])

const add = (definition) => {
	const described = UNVERIFIED.has(definition.name)
		? `${definition.description} (Not verified in-game yet: try it on a test save first.)`
		: definition.description
	return disposers.push(ctx.tools.register({ ...definition, description: described }))
}

	/* ---- 与上游 Yidhar/stellaris-mcp 对齐的工具（后端方法本就在 174 个里，这里只做接线） ---- */

	/** 直接把参数转发给同名桥方法，返回 JSON。 */
	const bridgeTool = (name, method, description, properties) =>
		add({
			name,
			description,
			parameters: { type: 'object', properties: properties ?? {}, additionalProperties: false },
			output: textOutput,
			execute: async (args) => {
				try {
					const answer = await pipeCall(config.pipe || DEFAULTS.pipe, method, args ?? {})
					const result = withDisplayPower(answer?.result ?? answer)
					return text(JSON.stringify(result))
				} catch (error) {
					return text(`bridge: ${error.message}`)
				}
			},
		})

	/* —— 只读（后端已逐个冒烟验证） —— */
	/* 事件弹窗 + 一句「为什么停着」：抬头取自 get_attention（pause_reason 只有三种：
	   player 手动暂停 / event 有事件窗口 / popup 有弹窗消息），正文仍是 get_active_events 的原始 JSON。 */
	add({
		name: 'stellaris_events',
		description:
			'Pending event windows: event id, title, description and each option with its validity flag (bridge get_active_events). The header also says why the game is paused — from get_attention: pause_reason player (you paused it), event (an event window holds it) or popup (a message set to pause), plus how many events are waiting and the current speed. Use stellaris_event_options / stellaris_answer_event on an open window, stellaris_open_events for the ones with no classic window.',
		parameters: {
			type: 'object',
			properties: { raw: { type: 'boolean', description: 'return only the bridge JSON (no header)' } },
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const events = await rpc(pipe, 'get_active_events', {})
				const att = await rpc(pipe, 'get_attention', {}).catch(() => null)
				const list = Array.isArray(events) ? events : Array.isArray(events?.events) ? events.events : []
				if (args?.raw === true) return text(JSON.stringify(events))
				const reason = { player: '玩家手动暂停', event: '有事件窗口', popup: '有弹窗消息' }[att?.pause_reason] ?? att?.pause_reason ?? '?'
				const state = att?.paused ? `已暂停：${reason}` : `运行中（速度 ${att?.speed ?? '?'}）`
				const waiting = Array.isArray(att?.pending_events) ? att.pending_events.length : 0
				return text(
					`事件 ${list.length} 个｜${state}｜待处理弹窗 ${waiting} 个` +
						`${att?.resumes_when_closed === true ? '｜关掉窗口会自动继续' : ''}\n` +
						JSON.stringify(events),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	bridgeTool('stellaris_notifications', 'get_notifications',
		'Top-bar notification badges waiting to be acknowledged.')
	bridgeTool('stellaris_alerts', 'get_alerts',
		'Top-bar alert banners (CAlertIconsWindow): alert ids, titles and tooltips.')
	bridgeTool('stellaris_government', 'get_government',
		'Government form, authority, civics, council seats and agenda progress.')

	/* ---- 国策（国民理念）：读 + 改（2026-11-22 接；改的那只先读后投，默认要 confirm）---- */
	add({
		name: 'stellaris_civics',
		description:
			'Read the empire\'s **civics** (bridge get_civics): the civics currently adopted (key / localised name / civic-point cost), the civic-point budget (total / used / free) and the reform status (whether a reform is possible now, its unity cost and the stockpile). Pass civic_key to ask about ONE civic: the bridge then answers whether it can be adopted and why not (already adopted, not enough free civic points). **Effects are not in memory** — with civic_key the tool parses common/governments/civics/*.txt (top-level modifier block, potential, custom_tooltip) and resolves @constants from the same file and common/scripted_variables, plus the civic description through the engine\'s localisation; `effects: true` prints the effect of every current civic. Modifier names are localised through the engine (tries mod_<key>, then MOD_<KEY>, then the raw key; strips $VALUE$ / [This.Get…] placeholders). Values are raw game values (0.2 = +20%). Changing civics is stellaris_change_civics.',
		parameters: {
			type: 'object',
			properties: {
				civic_key: { type: 'string', description: 'ask about one civic key, e.g. civic_machine_servitor' },
				effects: { type: 'boolean', description: 'also print every current civic\'s effect, parsed from common/governments/civics' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const params = {}
				if (typeof args?.civic_key === 'string' && args.civic_key) params.civic_key = args.civic_key
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_civics', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				/* 修正值（modifier）的本地化名。引擎里同一概念有两套键：
				   mod_<key>（如 mod_refugee_attraction）与全大写 <KEY>（如 MOD_PLANET_POP_ASSEMBLY_MULT），
				   两种都试，都没有就退回原始键。文案里可能带 [This.GetXxxColor] / $VALUE$ 这类占位符，清掉。 */
				const cleanLoc = (text) => {
					let t = String(text ?? '')
					t = t.replace(/\[[^\]]*\]/g, ' ')
					t = t.replace(/\$[A-Za-z0-9_$.]+?\$/g, ' ')
					t = t.replace(/\s+/g, ' ').trim()
					t = t.replace(/\s+的/g, '的')
					// 「X X」这种整段重复（$VALUE$ 被拼了两遍）只留一份
					const half = Math.floor(t.length / 2)
					if (t.length > 6 && t.slice(0, half).trim() === t.slice(half).trim()) t = t.slice(0, half).trim()
					// 「growth_node 生长节点经验获得」这种前缀变量名去掉
					t = t.replace(/^[A-Za-z][A-Za-z0-9_]*\s+(?=[\u4e00-\u9fa5])/, '')
					return t.trim()
				}
				const renderModifiers = async (mods) => {
					if (!Array.isArray(mods) || !mods.length) return ''
					const keys = []
					for (const m of mods) {
						keys.push('mod_' + m.key, 'MOD_' + m.key.toUpperCase(), m.key.toUpperCase(), m.key)
					}
					const loc = await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys }).catch(() => null)
					const names = loc?.names ?? {}
					const pick = (m) => {
						const a = names['mod_' + m.key]
						if (a && a !== 'mod_' + m.key) return cleanLoc(a)
						const b = names['MOD_' + m.key.toUpperCase()]
						if (b && b !== 'MOD_' + m.key.toUpperCase()) return cleanLoc(b)
						const c = names[m.key.toUpperCase()]
						if (c && c !== m.key.toUpperCase()) return cleanLoc(c)
						return null
					}
					return mods
						.map((m) => {
							const label = pick(m)
							return label ? label + ' ' + m.value + '（' + m.key + '）' : m.key + ' ' + m.value
						})
						.join('｜')
				}
				const pts = d?.civic_points ?? {}
				const reform = d?.reform ?? {}
				const lines = [
					`国策 ${Array.isArray(d?.civics) ? d.civics.length : 0} 条｜国策点 ${pts.used ?? '?'} / ${pts.total ?? '?'}（余 ${pts.free ?? '?'}）`,
				]
				for (const c of d?.civics ?? []) {
					lines.push(`  ${c.name}（${c.key}）｜占 ${c.cost ?? '?'} 点`)
					if (args?.effects === true) {
						const eff = civicEffects(c.key)
						if (eff?.modifiers.length) lines.push('      效果：' + (await renderModifiers(eff.modifiers)))
					}
				}
				lines.push(
					reform.can_reform
						? `改革：可以（${reform.reform_cost_unity ?? '?'} unity，库存 ${reform.unity_stockpile ?? '?'}）`
						: `改革：现在不行${reform.reason ? `（${reform.reason}）` : ''}`,
				)
				if (d?.civic && typeof d.civic === 'object') {
					const c = d.civic
					const why = Array.isArray(c.blocked_by) && c.blocked_by.length ? '｜' + c.blocked_by.join('；') : ''
					const req = c.requirements_met === false ? '｜前置条件不满足' : ''
					lines.push('查 ' + (c.name ?? c.key) + '（' + c.key + '）：' + (c.can_adopt ? '可采纳' : '不可采纳') + req + why)
					// 效果来自游戏文件（内存里没有）
					const eff = civicEffects(c.key)
					if (eff) {
						const desc = await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: [`${c.key}_desc`] }).catch(() => null)
						const descText = desc?.names?.[`${c.key}_desc`]
						if (descText && descText !== `${c.key}_desc`) lines.push(`  说明：${descText}`)
						if (eff.modifiers.length) lines.push('  效果：' + (await renderModifiers(eff.modifiers)))
						if (eff.tooltips.length) lines.push(`  另见：${eff.tooltips.join('、')}`)
						if (eff.potential.length) lines.push(`  前置：${eff.potential.join(' ')}`)
					} else {
						lines.push('  （游戏文件里没找到这条国策的定义）')
					}
				}
				if (Array.isArray(d?.available_civics) && d.available_civics.length) {
					lines.push(`可采纳 ${d.available_civics.length} 条：`)
					for (const c of d.available_civics.slice(0, 20)) {
						lines.push(`  ${c.name ?? c.key}（${c.key}）${c.cost !== undefined ? `｜${c.cost} 点` : ''}${c.can_adopt === false ? '｜✗ ' + (c.reason ?? '不可') : ''}`)
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_change_civics',
		description:
			'Change the empire\'s civics through a government reform (bridge change_civics { add, remove }, both arrays of civic keys). This SPENDS unity — the bridge reports the reform cost and the stockpile — and the game may put the reform on cooldown, so this tool first reads get_civics and refuses when a civic is already adopted, a key is unknown to the current list, or the free civic points do not cover the additions. **Nothing is posted unless confirm:true** (dry_run:true is the same thing); with confirm it posts once and reports the engine\'s answer verbatim.',
		parameters: {
			type: 'object',
			properties: {
				add: { type: 'array', items: { type: 'string' }, description: 'civic keys to adopt' },
				remove: { type: 'array', items: { type: 'string' }, description: 'civic keys to drop' },
				confirm: { type: 'boolean', description: 'post the reform (otherwise only the checks and the cost are shown)' },
				dry_run: { type: 'boolean', description: 'only run the checks, post nothing' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const addList = Array.isArray(args?.add) ? args.add.filter((k) => typeof k === 'string' && k) : []
				const removeList = Array.isArray(args?.remove) ? args.remove.filter((k) => typeof k === 'string' && k) : []
				if (!addList.length && !removeList.length) return text('要给 add 或 remove（都是国策键的数组）')
				const now = await rpc(pipe, 'get_civics', {})
				if (now?.error) return text(bridgeFail(now))
				const have = new Set((now.civics ?? []).map((c) => c.key))
				const pts = now.civic_points ?? {}
				const reform = now.reform ?? {}
				const problems = []
				for (const k of removeList) if (!have.has(k)) problems.push(`remove 里的 ${k} 并没有采纳`)
				for (const k of addList) if (have.has(k)) problems.push(`add 里的 ${k} 已经采纳`)
				const freeGuess = (pts.free ?? 0) + removeList.length
				if (addList.length > freeGuess) problems.push(`国策点不够：余 ${pts.free ?? 0}，去掉 ${removeList.length} 条后最多再上 ${freeGuess} 条，但要加 ${addList.length} 条`)
				const head = `现状：${(now.civics ?? []).map((c) => `${c.name}（${c.key}）`).join(' / ') || '（无）'}｜点数 ${pts.used ?? '?'}/${pts.total ?? '?'}`
				const plan = `计划：去掉 ${removeList.join('、') || '（无）'}，加上 ${addList.join('、') || '（无）'}｜改革费 ${reform.reform_cost_unity ?? '?'} unity（库存 ${reform.unity_stockpile ?? '?'}）`
				if (problems.length) return text(`${head}\n${plan}\n✗ 不投：${problems.join('；')}`)
				if (args?.confirm !== true || args?.dry_run === true) {
					return text(`${head}\n${plan}\n（未投：要真改请加 confirm: true）`)
				}
				const res = await rpc(pipe, 'change_civics', { add: addList, remove: removeList })
				if (res?.success === false || res?.error) return text(`${head}\n${plan}\n引擎拒绝：${bridgeFail(res)}`)
				return text(`${head}\n${plan}\n已投改革${res?.spent_unity !== undefined ? `，花费 ${res.spent_unity} unity` : ''}\n用 stellaris_civics 复核当前国策`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	bridgeTool('stellaris_traditions', 'get_traditions',
		'Adopted tradition trees, ascension perks, unlocked traditions and adoptable trees.')
	/* ---- 法令：目录 + 三值判定（potential 能判真/判假，其余一律 unknown）----
	   读游戏文件 common/edicts/*.txt 解析 potential / prerequisites；只判拿得到的事实。 */
	const EDICT_CACHE = { dir: null, defs: null }
	const EDICT_GAME_DIRS = [
		'E:\\SteamLibrary\\steamapps\\common\\Stellaris',
		'D:\\SteamLibrary\\steamapps\\common\\Stellaris',
		'C:\\Program Files (x86)\\Steam\\steamapps\\common\\Stellaris',
	]
	/* 国策的效果在内存里没有，只能从游戏文件读：common/governments/civics/*.txt
	   一个国策块里通常是 potential（前置）+ modifier（数值）+ custom_tooltip / description（文案键）。
	   @开头的常量（如 @gestalt_civic_node_xp_rate）在本文件顶部的 @name = 值 里定义，读的时候顺带解析。 */
	const CIVIC_DIRS = EDICT_GAME_DIRS.map((d) => d + '\\common\\governments\\civics')
	const SCRIPTVAR_DIRS = EDICT_GAME_DIRS.map((d) => d + '\\common\\scripted_variables')
	const CIVIC_CACHE = { defs: null, consts: null }
	/* @常量可能定义在别处：civic 文件里的 @gestalt_civic_node_xp_rate 实际在
	   common/scripted_variables/05_scripted_variables_paragon.txt，所以要单独扫一遍 */
	const scriptVarConsts = () => {
		if (CIVIC_CACHE.consts) return CIVIC_CACHE.consts
		const map = new Map()
		for (const dir of SCRIPTVAR_DIRS) {
			let files = []
			try {
				files = readdirSync(dir).filter((f) => f.endsWith('.txt'))
			} catch {
				continue
			}
			for (const file of files) {
				let text = ''
				try {
					text = readFileSync(path.join(dir, file), 'utf8')
				} catch {
					continue
				}
				for (const m of text.matchAll(/^\s*@([A-Za-z0-9_]+)\s*=\s*([-\w.]+)\s*(?:#.*)?$/gm)) {
					if (!map.has(m[1])) map.set(m[1], m[2])
				}
			}
		}
		CIVIC_CACHE.consts = map
		return map
	}
	const civicDefs = () => {
		if (CIVIC_CACHE.defs) return CIVIC_CACHE.defs
		const defs = new Map()
		for (const dir of CIVIC_DIRS) {
			let files = []
			try {
				files = readdirSync(dir).filter((f) => f.endsWith('.txt'))
			} catch {
				continue
			}
			for (const file of files) {
				let text = ''
				try {
					text = readFileSync(path.join(dir, file), 'utf8')
				} catch {
					continue
				}
				const consts = new Map()
				for (const m of text.matchAll(/^\s*@([A-Za-z0-9_]+)\s*=\s*([-\w.]+)\s*(?:#.*)?$/gm)) consts.set(m[1], m[2])
				for (const m of text.matchAll(/^\s*([a-z0-9_]+)\s*=\s*\{/gm)) {
					const key = m[1]
					if (defs.has(key)) continue
					const body = braceBlock(text, m.index + m[0].length - 1)
					if (body) defs.set(key, { body, consts, file })
				}
			}
		}
		CIVIC_CACHE.defs = defs
		return defs
	}
	/* 只取**顶层**子块：国策块里 nested 的 modifier（比如 ai_weight 里的）不能算效果 */
	const topChildren = (body) => {
		const out = new Map()
		let i = 0
		const skipWs = () => {
			while (i < body.length && /\s/.test(body[i])) i += 1
			while (i < body.length && body[i] === '#') {
				while (i < body.length && body[i] !== '\n') i += 1
				while (i < body.length && /\s/.test(body[i])) i += 1
			}
		}
		while (i < body.length) {
			skipWs()
			if (i >= body.length) break
			const m = /^([A-Za-z0-9_.]+)\s*=\s*/.exec(body.slice(i))
			if (!m) {
				i += 1
				continue
			}
			const name = m[1]
			i += m[0].length
			if (body[i] === '{') {
				const inner = braceBlock(body, i)
				if (inner === null) break
				let depth = 0
				let j = i
				for (; j < body.length; j += 1) {
					if (body[j] === '{') depth += 1
					else if (body[j] === '}') {
						depth -= 1
						if (depth === 0) {
							j += 1
							break
						}
					}
				}
				if (!out.has(name)) out.set(name, inner)
				i = j
			} else {
				let j = body.indexOf('\n', i)
				if (j < 0) j = body.length
				if (!out.has(name)) out.set(name, body.slice(i, j).trim())
				i = j
			}
		}
		return out
	}
	const resolveConst = (v, consts) => {
		if (typeof v !== 'string' || !v.startsWith('@')) return v
		return consts.get(v.slice(1)) ?? scriptVarConsts().get(v.slice(1)) ?? v
	}
	const civicEffects = (key) => {
		const def = civicDefs().get(key)
		if (!def) return null
		const children = topChildren(def.body)
		const num = (v) => {
			const raw = resolveConst(v, def.consts)
			const n = Number(raw)
			if (!Number.isFinite(n)) return String(raw).replace(/^"|"$/g, '')
			return n > 0 ? `+${n}` : String(n)
		}
		const modifiers = []
		const modBody = children.get('modifier')
		if (modBody) {
			for (const m of modBody.matchAll(/([A-Za-z0-9_]+)\s*=\s*(@?[-\w."]+)/g)) modifiers.push({ key: m[1], value: num(m[2]) })
		}
		const potential = []
		const potBody = children.get('potential')
		if (potBody) {
			for (const line of String(potBody).split('\n')) {
				const t = line.replace(/#.*$/, '').trim()
				if (t) potential.push(t)
			}
		}
		const tooltips = []
		for (const [name, val] of children) {
			if (name === 'custom_tooltip' && typeof val === 'string') tooltips.push(val)
		}
		return { modifiers, potential, tooltips, file: def.file }
	}
	const T3 = (v) => (v === true ? 'true' : v === false ? 'false' : 'unknown')
	const and3 = (a, b) => (a === 'false' || b === 'false' ? 'false' : a === 'unknown' || b === 'unknown' ? 'unknown' : 'true')
	const or3 = (a, b) => (a === 'true' || b === 'true' ? 'true' : a === 'unknown' || b === 'unknown' ? 'unknown' : 'false')
	const not3 = (a) => (a === 'unknown' ? 'unknown' : a === 'true' ? 'false' : 'true')
	const braceBlock = (s, start) => {
		let depth = 0
		for (let i = start; i < s.length; i += 1) {
			if (s[i] === '{') depth += 1
			else if (s[i] === '}') {
				depth -= 1
				if (depth === 0) return s.slice(start + 1, i)
			}
		}
		return null
	}
	const splitClauses = (body) => {
		const out = []
		let i = 0
		while (i < body.length) {
			while (i < body.length && /[\s#]/.test(body[i])) {
				if (body[i] === '#') while (i < body.length && body[i] !== '\n') i += 1
				i += 1
			}
			if (i >= body.length) break
			const m = /^[A-Za-z_][A-Za-z0-9_.]*/.exec(body.slice(i))
			if (!m) { i += 1; continue }
			const name = m[0]
			i += name.length
			while (i < body.length && /\s/.test(body[i])) i += 1
			if (body[i] !== '=') continue
			i += 1
			while (i < body.length && /\s/.test(body[i])) i += 1
			if (body[i] === '{') {
				const blk = braceBlock(body, i)
				if (blk === null) break
				out.push({ name, block: blk })
				i += blk.length + 2
			} else {
				const v = /^[^\s}]+/.exec(body.slice(i))
				out.push({ name, value: v ? v[0].replace(/"/g, '') : '' })
				i += v ? v[0].length : 0
			}
		}
		return out
	}
	const evalFacts = (name, value, facts) => {
		const want = value === 'yes' || value === 'true'
		switch (name) {
			// 依据（游戏文件 common/scripted_triggers/00_scripted_triggers.txt）：
			//   is_machine_empire = { has_authority = auth_machine_intelligence }
			//   is_hive_empire    = { has_authority = auth_hive_mind }
			//   is_gestalt        = { has_ethic = ethic_gestalt_consciousness }
			//   is_wilderness_empire = { has_origin = origin_wilderness }
			//   has_comms_with_alien_empire = any_relation(has_communications) 且 country_type ∈ default/fallen/awakened
			//   is_nomadic 是引擎内建，common 里没有定义 → 一律 unknown
			case 'is_machine_empire': return T3((facts.authority === 'auth_machine_intelligence') === want)
			case 'is_hive_empire': return T3((facts.authority === 'auth_hive_mind') === want)
			case 'is_gestalt': return T3(facts.ethics.has('ethic_gestalt_consciousness') === want)
			case 'is_wilderness_empire': return T3((facts.origin === 'origin_wilderness') === want)
			case 'has_comms_with_alien_empire': return T3(facts.comms === want)
			case 'has_authority': return T3(facts.authority === value)
			case 'has_ethic': return T3(facts.ethics.has(value))
			case 'has_origin': return T3(facts.origin === value)
			case 'is_nomadic': return facts.nomadic === null ? 'unknown' : T3(facts.nomadic === want)
			case 'has_technology': return facts.techs ? T3(facts.techs.has(value)) : 'unknown'
			case 'has_tradition':
			case 'has_active_tradition': return T3(facts.traditions.has(value))
			case 'always': return T3((value === 'yes') === want)
			// 全 DLC：has_*_dlc 一律按“拥有”判
			case 'is_pacifist': case 'is_militarist': case 'is_spiritualist': case 'is_materialist':
			case 'is_xenophile': case 'is_xenophobe': case 'is_authoritarian': case 'is_egalitarian': {
				const ethic = {
					is_pacifist: 'ethic_pacifist', is_militarist: 'ethic_militarist',
					is_spiritualist: 'ethic_spiritualist', is_materialist: 'ethic_materialist',
					is_xenophile: 'ethic_xenophile', is_xenophobe: 'ethic_xenophobe',
					is_authoritarian: 'ethic_authoritarian', is_egalitarian: 'ethic_egalitarian',
				}[name]
				return T3(facts.ethics.has(ethic) === want)
			}
			default:
				if (/^has_[a-z0-9_]*dlc$/.test(name)) return T3(want)
				// 具体飞升天赋：has_ascension_perk = ap_xxx（get_traditions 的 ascension_perks.adopted）
				// 注意：这里的 value 是键名而不是 yes/no，不能套布尔公式，否则「没有这个天赋」会被判成真
				if (name === 'has_ascension_perk') {
					if (!facts.perks) return 'unknown'
					if (value === 'yes' || value === 'no') return T3((value === 'yes') === want)
					return T3(facts.perks.has(value))
				}
				// 飞升路径（引擎内建触发器）：灵能这一条由国民理念「天选」(civic_chosen)
				// 或等位的「先知网络」(civic_machine_chosen) 提供
				if (/^has_(psionic|cybernetic|synthetic|genetic|purity)_ascension$/.test(name)) {
					const kind = /^has_(\w+)_ascension$/.exec(name)[1]
					const perks = facts.perks ?? new Set()
					const byPerk = [...perks].some((p) => p.includes(kind))
					const byCivic = kind === 'psionic' && (facts.civics ?? []).some((c) => c === 'civic_chosen' || c === 'civic_machine_chosen')
					if (byPerk || byCivic) return T3(want)
					return perks.size ? T3(!want) : 'unknown'
				}
				if (name === 'is_individual_machine') {
					return facts.civics?.includes('civic_machine_assimilator') === true ? T3(want) : 'unknown'
				}
				return 'unknown'
		}
	}
	const evalClause = (c, facts) => {
		if (c.block !== undefined) {
			if (c.name === 'OR') return splitClauses(c.block).reduce((acc, x) => or3(acc, evalClause(x, facts)), 'false')
			if (c.name === 'AND') return evalBody(c.block, facts)
			if (c.name === 'NOT') return not3(evalBody(c.block, facts))
			if (c.name === 'NOR') return not3(splitClauses(c.block).reduce((acc, x) => or3(acc, evalClause(x, facts)), 'false'))
			if (c.name === 'NAND') return not3(evalBody(c.block, facts))
			// custom_tooltip { fail_text = "..."  <条件> } —— 剥掉 fail_text 后按里面的条件判
			if (c.name === 'custom_tooltip') {
				return evalBody(c.block.replace(/fail_text\s*=\s*("[^"]*"|[^\s}]+)/g, ''), facts)
			}
			// 事件/选项作用域块：owner、root 是国家 —— 用同一份事实往下判；from/fromfrom 是遗址/舰队，读不到
			if (c.name === 'owner' || c.name === 'root') return evalBody(c.block, facts)
			return 'unknown'
		}
		return evalFacts(c.name, c.value, facts)
	}
	const clauseText = (c) => (c.block !== undefined ? `${c.name} { … }` : `${c.name} = ${c.value}`)
	/** 找出第一条判为 false 的子句原文（给界面复核用）；作用域块往里钻一层，找不到就给整段 */
	const blockVerdict = (body, facts) => {
		const txt = String(body || '').trim()
		if (!txt) return { state: 'true', failed: '' }
		const state = evalBody(txt, facts)
		if (state !== 'false') return { state, failed: '' }
		const findFalse = (src) => {
			for (const c of splitClauses(src)) {
				if (evalClause(c, facts) !== 'false') continue
				// owner / root / AND 这几种块只是作用域或与，真正卡住的是里面那条
				if (c.block !== undefined && (c.name === 'owner' || c.name === 'root' || c.name === 'AND')) {
					const inner = findFalse(c.block)
					if (inner) return `${c.name}: ${inner}`
				}
				return clauseText(c)
			}
			return null
		}
		return { state, failed: findFalse(txt) ?? txt.replace(/\s+/g, ' ').slice(0, 160) }
	}
	const evalBody = (body, facts) => splitClauses(body).reduce((acc, c) => and3(acc, evalClause(c, facts)), 'true')
	const findGameDir = async () => {
		if (EDICT_CACHE.dir) return EDICT_CACHE.dir
		const cands = config.gameDir ? [config.gameDir, ...EDICT_GAME_DIRS] : EDICT_GAME_DIRS
		for (const d of cands) {
			try {
				await fs.access(path.join(d, 'common', 'edicts'))
				EDICT_CACHE.dir = d
				return d
			} catch {}
		}
		return null
	}
	const loadEdictDefs = async () => {
		if (EDICT_CACHE.defs) return EDICT_CACHE.defs
		const dir = await findGameDir()
		if (!dir) return null
		const edir = path.join(dir, 'common', 'edicts')
		const defs = new Map()
		for (const f of await fs.readdir(edir)) {
			if (!f.endsWith('.txt')) continue
			const txt = await fs.readFile(path.join(edir, f), 'utf8')
			const re = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\{/gm
			let m
			while ((m = re.exec(txt))) {
				const blk = braceBlock(txt, txt.indexOf('{', m.index))
				if (blk === null) continue
				const pot = /potential\s*=\s*\{/.exec(blk)
				const pre = /prerequisites\s*=\s*\{/.exec(blk)
				defs.set(m[1], {
					potential: pot ? braceBlock(blk, blk.indexOf('{', pot.index)) : '',
					prereq: pre ? braceBlock(blk, blk.indexOf('{', pre.index)) : '',
				})
			}
		}
		EDICT_CACHE.defs = defs
		return defs
	}
	/** prerequisites 是一串科技键；有科技集就逐条判，没有就 unknown */
	const prereqState = (body, facts) => {
		const text = String(body || '').trim()
		if (!text) return 'true'
		const keys = [...text.matchAll(/"([^"]+)"/g)].map((m) => m[1])
		const list = keys.length ? keys : [...text.matchAll(/[A-Za-z_][A-Za-z0-9_]*/g)].map((m) => m[0])
		if (!list.length) return 'true'
		if (!facts.techs) return 'unknown'
		return list.every((k) => facts.techs.has(k)) ? 'true' : 'false'
	}

	/** 传统会被 tradition_swap 改名（如 workplace_motivators → drone_network，gestalt 专用） */
	const TRAD_SWAPS = { list: null }
	const loadTraditionSwaps = async () => {
		if (TRAD_SWAPS.list) return TRAD_SWAPS.list
		const dir = await findGameDir()
		if (!dir) return null
		const tdir = path.join(dir, 'common', 'traditions')
		const out = []
		try {
			for (const f of await fs.readdir(tdir)) {
				if (!f.endsWith('.txt')) continue
				const txt = await fs.readFile(path.join(tdir, f), 'utf8')
				const re = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\{/gm
				let m
				while ((m = re.exec(txt))) {
					const blk = braceBlock(txt, txt.indexOf('{', m.index))
					if (!blk || !blk.includes('tradition_swap')) continue
					const sw = /tradition_swap\s*=\s*\{/.exec(blk)
					if (!sw) continue
					const swBody = braceBlock(blk, blk.indexOf('{', sw.index))
					if (!swBody) continue
					const nm = /name\s*=\s*([A-Za-z_][A-Za-z0-9_]*)/.exec(swBody)
					const tg = /trigger\s*=\s*\{/.exec(swBody)
					if (!nm) continue
					const trig = tg ? braceBlock(swBody, swBody.indexOf('{', tg.index)) : ''
					out.push({ key: m[1], swap: nm[1], trigger: trig || '' })
				}
			}
		} catch {}
		TRAD_SWAPS.list = out
		return out
	}

	const edictFacts = async (pipe) => {
		const safe = async (m) => { try { return await rpc(pipe, m) } catch (e) { return { __error: String(e && e.message) } } }
		const gov = await safe('get_government')
		// 传统一次读空会让所有 has_tradition 判成 false（2026-11-19 踩过），失败或空集时重试一次
		let trad = await safe('get_traditions')
		const tradEmpty = (t) => !t || t.__error || ((t.unlocked_traditions ?? []).length === 0 && (t.adopted_trees ?? []).length === 0)
		if (tradEmpty(trad)) {
			await new Promise((r) => setTimeout(r, 300))
			const retry = await safe('get_traditions')
			if (!tradEmpty(retry)) trad = retry
		}
		const contacts = await safe('get_contacts')
		if (gov && gov.__error) console.error('get_government:', gov.__error)
		if (trad && trad.__error) console.error('get_traditions:', trad.__error)
		if (contacts && contacts.__error) console.error('get_contacts:', contacts.__error)
		const authority = gov?.authority ?? ''
		const origin = gov?.origin ?? ''
		const ethics = new Set((gov?.ethics ?? []).map((e) => e.key ?? e))
		const contactTypes = (contacts?.empires ?? []).map((e) => e.country_type)
		const comms = contactTypes.some((t) => t === 'default' || t === 'fallen_empire' || t === 'awakened_fallen_empire')
		const civics = (gov?.civics ?? []).map((c) => c.key ?? c)
		// 飞升天赋（供 has_*_ascension 类触发器判）
		const perks = new Set((trad?.ascension_perks?.adopted ?? []).map((p) => p.key ?? p))
		const traditions = new Set([
			...(trad?.unlocked_traditions ?? []).map((t) => t.key),
			...(trad?.adopted_trees ?? []).map((t) => t.key),
		])
		let nomadic = null
		try {
			const carriers = await rpc(pipe, 'probe_colony_carriers')
			if (carriers && typeof carriers.ship_carriers === 'number') nomadic = carriers.ship_carriers > 0
		} catch {}
		// tradition_swap：gestalt 等条件下传统会改名，按当前事实把 swap 名并进集合
		try {
			const swaps = await loadTraditionSwaps()
			if (swaps && swaps.length) {
				const base = { authority, origin, ethics, comms, traditions, techs: null, nomadic: null, contacts: contactTypes.length }
				for (const s of swaps) {
					if (!traditions.has(s.key)) continue
					if (!s.trigger || evalBody(s.trigger, base) === 'true') { traditions.delete(s.key); traditions.add(s.swap) }
				}
			}
		} catch {}
		let techs = null
		try {
			const tr = await researchedTechs(pipe)
			if (tr && tr.techs) techs = tr.techs
		} catch {}
		return { authority, origin, ethics, comms, civics, traditions, techs, nomadic, perks, contacts: contactTypes.length }
	}

	/* ---- 事件选项的脚本筛选（2026-11-20）----
	   引擎的 is_valid 只查选项的 allow，不查它的 trigger：被 trigger 过滤掉的选项照样报 true，
	   界面上却根本不显示（第一次接触 first_contact.15、考古 ancrel.14510 都踩过）。
	   这里照「法令」那套做法，把事件脚本里每条 option 的 trigger 找出来做三值求值。
	   作用域：选项 trigger 常写成 owner = { … }，那是国家作用域，用与法令同一份事实判；
	   from / fromfrom（遗址、舰队）读不到，一律 unknown。 */
	const EVENT_INDEX = { data: null }
	const loadEventIndex = async () => {
		if (EVENT_INDEX.data) return EVENT_INDEX.data
		const dir = await findGameDir()
		if (!dir) return null
		const edir = path.join(dir, 'events')
		let files = []
		try {
			files = (await fs.readdir(edir)).filter((f) => f.endsWith('.txt'))
		} catch {
			return null
		}
		const map = new Map()
		for (const f of files) {
			let txt = ''
			try {
				txt = await fs.readFile(path.join(edir, f), 'utf8')
			} catch {
				continue
			}
			// namespace = ancrel 这样的声明决定 id 的前缀；一个文件可以声明好几个
			for (const m of txt.matchAll(/^\s*namespace\s*=\s*([A-Za-z_][A-Za-z0-9_]*)/gm)) {
				if (!map.has(m[1])) map.set(m[1], [])
				map.get(m[1]).push(f)
			}
		}
		EVENT_INDEX.data = { edir, files, map }
		return EVENT_INDEX.data
	}
	/** 事件脚本里那几条 option（按出现顺序）：{ name, trigger, allow }；找不到给 null */
	const loadEventOptions = async (eventKey) => {
		const idx = await loadEventIndex()
		if (!idx) return null
		const key = String(eventKey ?? '')
		if (!key.includes('.')) return null
		const ns = key.split('.')[0]
		const cands = [...(idx.map.get(ns) ?? []), ...idx.files.filter((f) => !(idx.map.get(ns) ?? []).includes(f))]
		const needle = new RegExp(`\\bid\\s*=\\s*${key.replace(/\./g, '\\.')}(?![0-9A-Za-z_])`)
		const optionsIn = (blk) => {
			const out = []
			const re = /(?:^|\n)\s*option\s*=\s*\{/g
			let om
			while ((om = re.exec(blk))) {
				const body = braceBlock(blk, blk.indexOf('{', om.index))
				if (body === null) continue
				const trig = /trigger\s*=\s*\{/.exec(body)
				const allow = /allow\s*=\s*\{/.exec(body)
				const nm = /\bname\s*=\s*"?([A-Za-z_][A-Za-z0-9_.]*)"?/.exec(body)
				out.push({
					name: nm ? nm[1] : '',
					trigger: trig ? braceBlock(body, body.indexOf('{', trig.index)) ?? '' : '',
					allow: allow ? braceBlock(body, body.indexOf('{', allow.index)) ?? '' : '',
				})
			}
			return out
		}
		for (const f of cands) {
			let txt = ''
			try {
				txt = await fs.readFile(path.join(idx.edir, f), 'utf8')
			} catch {
				continue
			}
			txt = txt.replace(/#[^\n]*/g, '') // 注释里也会写 id，先剥掉
			// 同一个 id 可能出现多次：先是一句 `fleet_event = { id = … }` 的转发，然后才是真定义。
			// 所以逐个试，取第一块**带 option** 的；都没有才退回第一块。
			const scan = new RegExp(needle.source, 'g')
			let best = null
			let m
			while ((m = scan.exec(txt))) {
				const open = txt.lastIndexOf('{', m.index) // 事件块的开括号一定在 id 之前
				if (open < 0) continue
				const blk = braceBlock(txt, open)
				if (blk === null) continue
				const cand = { file: f, options: optionsIn(blk) }
				if (!best) best = cand
				if (cand.options.length) return cand
			}
			if (best) return best
		}
		return null
	}

	/* ---- 内阁议程：脚本判定 potential + allow ----
	   引擎 CSetCouncilAgendaCommand::IsValid 基本只卡 potential；allow 不满足时命令会被接受、
	   过一个月又静默清空（2026-11-19 实测：军事准备/增长修订被清，校正二分法保留）。
	   所以这里照 common/council_agendas/*.txt 自己判一遍，把 allow 也算进去。 */
	const AGENDA_CACHE = { defs: null }
	/** 议程代价宏（@base_agenda_cost 等）的真实数值：读 common/scripted_variables，
	 *  再补 common/council_agendas 各文件顶部的宏定义（@base_agenda_cost 就定义在那里）。
	 *  这样换版本/换档都准，不把 7000/3500 写死。 */
	const AGENDA_VARS = { vars: null }
	const loadScriptedVars = async () => {
		if (AGENDA_VARS.vars) return AGENDA_VARS.vars
		const dir = await findGameDir()
		if (!dir) return null
		const vars = new Map()
		const re = /^\s*@([A-Za-z0-9_.]+)\s*=\s*(-?[0-9.]+)/gm
		for (const sub of [['common', 'scripted_variables'], ['common', 'council_agendas']]) {
			try {
				const d = path.join(dir, ...sub)
				for (const f of await fs.readdir(d)) {
					if (!f.endsWith('.txt')) continue
					const txt = await fs.readFile(path.join(d, f), 'utf8')
					for (const m of txt.matchAll(re)) vars.set(m[1], Number(m[2]))
				}
			} catch {}
		}
		AGENDA_VARS.vars = vars
		return vars
	}
	/** 代价档：base / short / ascension / nomadic + 实际数值 */
	const costClass = (raw, vars) => {
		const s = String(raw || '').trim()
		if (!s) return { cls: '', value: null }
		if (s.startsWith('@')) {
			const name = s.slice(1)
			const value = vars ? (vars.get(name) ?? null) : null
			const kind = /ascension/i.test(name) ? '飞升'
				: /short/i.test(name) ? '短'
					: /nomadic/i.test(name) ? '游牧'
						: '基础'
			return { cls: kind, value }
		}
		if (/^(value:|@)/.test(s)) return { cls: '公式', value: null }
		const n = Number(s)
		return { cls: Number.isFinite(n) ? '基础' : '公式', value: Number.isFinite(n) ? n : null }
	}
	const loadAgendaDefs = async () => {
		if (AGENDA_CACHE.defs) return AGENDA_CACHE.defs
		const dir = await findGameDir()
		if (!dir) return null
		const adir = path.join(dir, 'common', 'council_agendas')
		const defs = new Map()
		try {
			for (const f of await fs.readdir(adir)) {
				if (!f.endsWith('.txt')) continue
				const txt = await fs.readFile(path.join(adir, f), 'utf8')
				const re = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\{/gm
				let m
				while ((m = re.exec(txt))) {
					const blk = braceBlock(txt, txt.indexOf('{', m.index))
					if (blk === null) continue
					// 只认真正的议程定义：必须自带 agenda_cost（origin_* 之类会被误收）
					if (!/agenda_cost\s*=/.test(blk)) continue
					const cost = /agenda_cost\s*=\s*([^\s}]+)/.exec(blk)
					const pot = /potential\s*=\s*\{/.exec(blk)
					const alw = /(^|\n)\s*allow\s*=\s*\{/.exec(blk)
					defs.set(m[1], {
						key: m[1],
						cost: cost ? cost[1] : '',
						potential: pot ? braceBlock(blk, blk.indexOf('{', pot.index)) : '',
						allow: alw ? braceBlock(blk, blk.indexOf('{', alw.index)) : '',
					})
				}
			}
		} catch {}
		AGENDA_CACHE.defs = defs
		return defs
	}
	add({
		name: 'stellaris_agenda_options',
		description:
			'Council agenda availability judged from the game scripts, not from the engine probe. CSetCouncilAgendaCommand::IsValid basically only checks `potential`, so an agenda whose `allow` block is unmet is still accepted by set_council_agenda and then silently cleared at the next monthly tick (measured 2026-11-19). This tool parses common/council_agendas/*.txt and evaluates both `potential` and `allow` with the same three-valued evaluator the edicts tool uses: facts we can read (authority, ethics, origin, civics, traditions, always, has_*_dlc → all DLC owned) are decided true/false, anything else stays unknown and is never treated as true. `custom_tooltip { fail_text = ... }` is unwrapped for evaluation and its raw text is reported so the condition can be checked by hand. Output: what is actually selectable, what is silently reverted (with the failing clause), and what could not be judged. raw:true returns the per-agenda JSON including raw potential/allow bodies.',
		parameters: {
			type: 'object',
			properties: {
				raw: { type: 'boolean', description: 'return per-agenda JSON instead of the summary text' },
				all: { type: 'boolean', description: 'also list agendas whose potential is false' },
			},
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const gov = await rpc(pipe, 'get_government')
				if (!gov || gov.error) return text(bridgeFail(gov))
				const facts = await edictFacts(pipe)
				const defs = await loadAgendaDefs()
				const listed = new Set((gov.available_agendas ?? []).map((a) => a.key))
				const names = new Map((gov.available_agendas ?? []).map((a) => [a.key, a.name]))
				const keys = defs ? [...new Set([...defs.keys(), ...listed])] : [...listed]
				const vars = await loadScriptedVars()
				const rows = keys.map((k) => {
					const def = defs ? defs.get(k) : null
					const pot = def && def.potential ? blockVerdict(def.potential, facts) : { state: def ? 'true' : 'unknown', failed: '' }
					const alw = def && def.allow ? blockVerdict(def.allow, facts) : { state: def ? 'true' : 'unknown', failed: '' }
					const cc = costClass(def?.cost, vars)
					return {
						key: k,
						name: names.get(k) ?? '',
						cost: def?.cost ?? '',
						cost_class: cc.cls,
						cost_value: cc.value,
						listed: listed.has(k),
						potential: pot.state,
						allow: alw.state,
						allow_failed: alw.failed,
						potential_body: def?.potential ?? '',
						allow_body: def?.allow ?? '',
					}
				})
				if (args?.raw === true) return text(JSON.stringify({ current: gov.agenda, facts: { authority: facts.authority, origin: facts.origin, ethics: [...facts.ethics] }, agendas: rows }, null, 2))
				// 分类：引擎清单 = potential 过 且 不在冷却（CSetCouncilAgendaCommand::IsValid 自带冷却过滤）
				//      脚本只需再判 allow —— allow=false 的命令会被接受、下个月又被清空
				const verdict = (r) => {
					if (r.key === (gov.agenda?.key ?? '')) return 'current' // 正在跑的那个不在可用清单里，属正常
					if (!r.listed) {
						if (r.potential === 'false') return 'no_potential'
						if (r.potential === 'unknown') return 'unknown_potential'
						return 'cooldown' // potential 通过却不在清单里 → 冷却中
					}
					if (r.allow === 'false') return 'reverted'
					if (r.allow === 'unknown') return 'unknown_allow'
					return 'selectable'
				}
				for (const r of rows) r.verdict = verdict(r)
				const selectable = rows.filter((r) => r.verdict === 'selectable')
				const reverted = rows.filter((r) => r.verdict === 'reverted')
				const cooldown = rows.filter((r) => r.verdict === 'cooldown')
				const noPot = rows.filter((r) => r.verdict === 'no_potential')
				const unknown = rows.filter((r) => r.verdict === 'unknown_allow')
				const unknownPot = rows.filter((r) => r.verdict === 'unknown_potential')
				// 缺名字的补本地化（议程名键是 council_agenda_<key>_name）
				const missing = [...selectable, ...reverted, ...cooldown].filter((r) => !r.name).map((r) => r.key)
				if (missing.length) {
					try {
						const loc = await rpc(pipe, 'localize', { keys: missing.map((k) => `council_agenda_${k}_name`) })
						for (const r of [...selectable, ...reverted, ...cooldown]) {
							if (r.name) continue
							const name = loc?.names?.[`council_agenda_${r.key}_name`]
							if (typeof name === 'string' && name && name !== `council_agenda_${r.key}_name`) r.name = name
						}
					} catch {}
				}
				const cur = gov.agenda ?? {}
				const tag = (r) => (r.key === cur.key ? '（当前）' : '')
				// 代价档：短条（3500）跑满只要一半，值得在列表里直接标出来
				const costTag = (r) => {
					if (r.cost_value == null) return ''
					return `  〔${r.cost_class}·${r.cost_value}${r.cost_value <= 3500 ? '·短条' : ''}〕`
				}
				const lines = [
					`当前议程：${cur.name || '（无）'}${cur.key ? `（${cur.key}，${cur.progress} / ${cur.cost}）` : ''}`,
					`可选 ${selectable.length}｜会被静默回滚 ${reverted.length}｜冷却中 ${cooldown.length}｜potential 不通过 ${noPot.length}｜potential 判不了 ${unknownPot.length}｜allow 判不了 ${unknown.length}` +
						(defs ? '' : '（找不到游戏目录，全部按 unknown 处理）'),
				]
				lines.push('真正可选（后面是代价档）：')
				for (const r of selectable) lines.push(`  ${r.name || r.key}  ${r.key}${tag(r)}${costTag(r)}`)
				if (reverted.length) {
					lines.push('会被静默回滚（引擎接受、下个月清空——allow 不满足）：')
					for (const r of reverted) lines.push(`  ${r.name || r.key}  ${r.key}   ← ${r.allow_failed}${tag(r)}${costTag(r)}`)
				}
				if (cooldown.length) {
					lines.push('冷却中／已用过（potential 通过、但引擎不给——按 COUNCIL_AGENDA_COOLDOWN=30 年算）：')
					for (const r of cooldown) lines.push(`  ${r.name || r.key}  ${r.key}${costTag(r)}`)
				}
				if (unknownPot.length) {
					lines.push('potential 判不了（条件读不到，例如 NAND 里的科技/剧情标志）：')
					for (const r of unknownPot.slice(0, 20)) lines.push(`  ${r.name || r.key}  ${r.key}${costTag(r)}`)
					if (unknownPot.length > 20) lines.push(`  …（另 ${unknownPot.length - 20} 条）`)
				}
				if (unknown.length) {
					lines.push('allow 判不了（引擎清单里有，但条件读不到——选之前建议手动确认）：')
					for (const r of unknown.slice(0, 20)) lines.push(`  ${r.name || r.key}  ${r.key}${costTag(r)}`)
					if (unknown.length > 20) lines.push(`  …（另 ${unknown.length - 20} 条）`)
				}
				if (args?.all === true && noPot.length) {
					lines.push('potential 不通过：')
					for (const r of noPot.slice(0, 30)) lines.push(`  ${r.name || r.key}  ${r.key}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_edicts',
		description:
			'Active edicts plus a three-valued availability check. The bridge enumerates the engine CEdictsDatabase and probes each entry with CAddEdictCommand::IsValid (nothing is posted) — but that probe ignores the per-edict potential, so it over-reports. This tool therefore also parses the game files (common/edicts/*.txt) and evaluates each potential with a small three-valued evaluator: facts we can read (authority → is_machine_empire / is_hive_empire / is_gestalt, civics → is_nomadic, traditions, contact count) are decided true/false, everything else stays unknown — unknown is never treated as true. A prerequisites block (tech list) is decided from the researched-tech set (read from memory). Output lists what is actually available (potential true, no prerequisites) and separately the ones whose potential passes but prerequisites could not be checked. all:true also lists the ones potential rejects.',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: 'also list edicts whose potential is false' },
				available_only: { type: 'boolean', description: 'default true; false returns the raw bridge JSON' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const d = await rpc(pipe, 'get_edicts')
				if (!d || d.error) return text(bridgeFail(d))
				if (args?.available_only === false) return text(JSON.stringify(d))
				const facts = await edictFacts(pipe)
				const defs = await loadEdictDefs()
				const rows = (d.available_edicts ?? []).map((e) => {
					const def = defs ? defs.get(e.key) : null
					const pot = defs && def ? (def.potential ? evalBody(def.potential, facts) : 'true') : 'unknown'
					const pre = defs && def ? prereqState(def.prereq, facts) : 'unknown'
					return { key: e.key, name: e.name, active: e.is_active === true, pot, pre }
				})
				const active = (d.active_edicts ?? []).map((e) => `${e.name}(${e.key})`)
				const ok = rows.filter((r) => r.pot === 'true' && r.pre === 'true' && !r.active)
				const maybe = rows.filter((r) => r.pot === 'true' && r.pre === 'unknown' && !r.active)
				const preFail = rows.filter((r) => r.pot === 'true' && r.pre === 'false' && !r.active)
				const no = rows.filter((r) => r.pot === 'false')
				const unk = rows.filter((r) => r.pot === 'unknown' && !r.active)
				const lines = [
					`生效 ${active.length} 条${active.length ? '：' + active.join('、') : ''}`,
					`判定可用 ${ok.length} 条｜前置科技不满足 ${preFail.length} 条｜前置判不了 ${maybe.length} 条｜potential 判不了 ${unk.length} 条｜potential 不通过 ${no.length} 条` +
						(defs ? '' : '（找不到游戏目录，全部按 unknown 处理）'),
				]
				for (const r of ok) lines.push(`  ${r.name}  ${r.key}`)
				if (maybe.length) {
					lines.push('潜在通过、前置未判定：')
					for (const r of maybe.slice(0, 20)) lines.push(`  ${r.name}  ${r.key}`)
					if (maybe.length > 20) lines.push(`  …（另 ${maybe.length - 20} 条）`)
				}
				if (args?.all === true && unk.length) {
					lines.push('potential 判不了（条件读不到，例如 is_nomadic / has_technology）：')
					for (const r of unk.slice(0, 12)) lines.push(`  ${r.name}  ${r.key}`)
					if (unk.length > 12) lines.push(`  …（另 ${unk.length - 12} 条）`)
				}
				if (args?.all === true && no.length) {
					lines.push('potential 不通过：')
					for (const r of no.slice(0, 20)) lines.push(`  ${r.name}  ${r.key}`)
					if (no.length > 20) lines.push(`  …（另 ${no.length - 20} 条）`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 领袖总表（2026-11-20 改为按职业分组）——
	   数据还是桥的 get_leaders；这里只做呈现：按 行政官 / 指挥官 / 科学官 / 特使 分组，每行给等级、位置、
	   可选特质标记。raw:true 退回原始 JSON。 */
	const SLOT_CN = { ruler: '统治者', unassigned: '闲置', ship: '舰船', governor: '总督', army: '陆军', empire: '帝国使者', federation: '联邦', galactic_community: '共同体', first_contact: '首触', spy_network: '间谍网', shroud: '虚境' }
	add({
		name: 'stellaris_leaders',
		description:
			'Hired leaders and the recruitment pool, grouped by class (行政官 / 指挥官 / 科学官 / 特使). Each line: name, ' +
			'level, where they serve (location slot + target id), how many traits they carry, and a 可选特质 marker when the ' +
			'engine currently offers a pick (trait_options / trait_upgrade_options non-empty — select one with ' +
			'stellaris_select_leader_trait). Raw fields behind it: location { slot, target_id, council_seat_id?, task? }, ' +
			'is_councilor, traits[] and the trait option lists. slot is the main location type and target_id means whatever ' +
			'that slot means — for ship it is the CShip id, NOT the fleet id; for governor the colony id; for empire a country ' +
			'id; a council seat shows up as slot unassigned plus location.council_seat_id. Use raw:true for the JSON, or ' +
			'stellaris_leader_assignments for "where can this one go, and why not".',
		parameters: {
			type: 'object',
			properties: {
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
				class: { type: 'string', description: 'only this class key (official / commander / scientist / envoy)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'get_leaders', {})
				if (args?.raw === true) return text(JSON.stringify(r))
				const hired = Array.isArray(r?.hired_leaders) ? r.hired_leaders : []
				const pool = Array.isArray(r?.pool_candidates) ? r.pool_candidates : []
				if (!hired.length && !pool.length) return text('没有领袖数据')

				const only = args?.class ? String(args.class) : ''
				const groups = new Map()
				for (const l of hired) {
					const key = String(l.class ?? '?')
					if (only && key !== only) continue
					if (!groups.has(key)) groups.set(key, [])
					groups.get(key).push(l)
				}
				const order = ['official', 'commander', 'scientist', 'envoy']
				const keys = [...groups.keys()].sort((a, b) => {
					const ia = order.indexOf(a), ib = order.indexOf(b)
					return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.localeCompare(b)
				})
				const unspent = hired.filter((l) => l?.has_unspent_trait_points).length
				const shownCount = only ? hired.filter((l) => String(l.class ?? '') === only).length : hired.length
				const lines = [
					`已雇 ${shownCount} 名` +
						(only ? `（${only}）` : '：' + [...groups.entries()].map(([k, v]) => `${v[0]?.class_name ?? k} ${v.length}`).join(' · ')) +
						`｜招募池 ${pool.length}｜可选特质 ${unspent}`,
				]
				for (const k of keys) {
					const list = groups.get(k).sort((a, b) => Number(b.level) - Number(a.level) || Number(a.id) - Number(b.id))
					lines.push('', `【${list[0]?.class_name ?? k}】${list.length} 名`)
					for (const l of list) {
						const loc = l.location ?? {}
						const seat = loc.council_seat_id !== undefined && Number(loc.council_seat_id) !== 0xffffffff
							? `议会席位 ${loc.council_seat_id}`
							: `${SLOT_CN[loc.slot] ?? loc.slot ?? '?'}` +
								(loc.target_id !== undefined && Number(loc.target_id) !== 0xffffffff ? ` ${loc.target_id}` : '') +
								(loc.slot === 'empire' && loc.task ? `（${loc.task}）` : '')
						const flags = []
						if (l.has_unspent_trait_points) flags.push(`可选特质 ${l.trait_selections_available}`)
						if (l.has_unspent_trait_points === false && (l.trait_options ?? []).length) flags.push('可选特质')
						lines.push(
							`  ${String(l.name ?? l.key ?? l.id).padEnd(10)} lv${String(l.level ?? '?').padEnd(2)}` +
								` ${seat}` +
								`  特质 ${(l.traits ?? []).length}` +
								(flags.length ? `  ★ ${flags.join('/')}` : '') +
								`  （id ${l.id}）`,
						)
						// 有可选时把选项列出来：trait_options 是「本次可选」，trait_upgrade_options 是「可花点升级」
						// （subclass_* 键就是等级 4 的专职方向，同一个命令选）
						const opts = (l.trait_options ?? []).map((t) => `${t.name || t.key}（${t.key}）`)
						const ups = (l.trait_upgrade_options ?? []).map((t) => `升级 ${t.name || t.key}（${t.key}）`)
						const all = [...opts, ...ups]
						if (all.length) lines.push(`      可选：${all.join(' / ')}`)
					}
				}
				if (pool.length && !only) {
					lines.push('', `【招募池】${pool.length} 名`)
					for (const p of pool) {
						lines.push(`  ${String(p.name ?? p.id).padEnd(10)} ${p.class_name ?? p.class ?? ''} lv${p.level ?? '?'}  （id ${p.id}）`)
					}
				}
				lines.push('', '选特质：stellaris_select_leader_trait { leader_id, trait_key }；查能派到哪：stellaris_leader_assignments')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_leader_assignments',
		description:
			'Every slot ONE leader could be posted to, and the engine\'s answer for each — the probe to read before ' +
			'stellaris_assign_leader. Bridge method get_leader_assignments. Returns current { slot, target_id, ' +
			'council_position_id? } — a leader holding a council seat reports slot unassigned, and the seat is named from ' +
			'council[] (its id is a tagged ref such as 16777276, which is also what assign_leader wants for slot council) — ' +
			'the council seats with can_assign plus the reason when one is taken, every candidate slot with can_assign and ' +
			'a reason when refused (governor slots are the player\'s colonies — for a nomad those are ark ships, so the ' +
			'name is the ship name; ship slots give fleet ids), and a ' +
			'refused summary counting the ones the engine will not take with its own text (e.g. 「现在不允许改变该舰队的司令。」). ' +
			'Measured 2026-11-20 on 星帆杰斯格: current ship 33559036, six free science ships listed, 7 refusals without a ' +
			'reason, 1 "already serves here", 1 fleet-commander lock.',
		parameters: {
			type: 'object',
			properties: { leader_id: { type: 'number', description: 'leader id from stellaris_leaders' } },
			required: ['leader_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const leaderId = Number(args?.leader_id)
			if (!Number.isFinite(leaderId)) return text('leader_id is required（stellaris_leaders 的 id）')
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'get_leader_assignments', { leader_id: leaderId })
				if (r?.error) return text(`失败：${r.error.message || JSON.stringify(r.error)}`)
				const cur = r.current ?? {}
				const council = Array.isArray(r.council) ? r.council : []
				// 议席不在主位置里：current.slot 会是 unassigned，真正的席位在 council_position_id。
				// 拿它去 council 列表里换名字——名字能换到，id 是带标记的引用（0x0100000C 这种）。
				const seatId = Number(cur.council_position_id)
				const seat = Number.isFinite(seatId) && seatId !== 0xffffffff
					? council.find((c) => Number(c.target_id) === seatId)
					: null
				const taskNamesHere = cur.task ? await envoyTaskNames(config.pipe || DEFAULTS.pipe) : null
				const curText = seat
					? `议会席位 ${seat.name || '?'}（id ${seatId}）`
					: `${cur.slot ?? '—'}${cur.target_id !== undefined && cur.target_id !== 0xffffffff ? ` ${cur.target_id}` : ''}` +
						(cur.task ? `｜${taskLabel(taskNamesHere, cur.task)}` : '')
				const lines = [
					`${r.name ?? '?'}（${r.class ?? '?'}，id ${r.leader_id ?? leaderId}）`,
					`  当前      ${curText}`,
				]
				if (council.length) {
					lines.push('  议会席位  ' + council.map((c) => `${c.name}${c.can_assign ? '（可派）' : `（${c.reason ?? '不可派'}）`}`).join(' / '))
				}
				const slots = Array.isArray(r.slots) ? r.slots : []
				if (slots.length) {
					const opened = slots.filter((s) => s.can_assign).length
					lines.push(`  候选槽位  ${slots.length} 个（可派 ${opened}）：`)
					for (const s of slots) {
						const mark = s.can_assign ? '✓' : '✗'
						const why = s.can_assign ? '' : `  ${s.reason ?? ''}`
						lines.push(`    ${mark} ${s.slot} ${s.target_id}${s.name ? ` ${s.name}` : ''}${why}`)
					}
				}
				const refused = Array.isArray(r.refused) ? r.refused : []
				if (refused.length) {
					lines.push('  被拒合计  ' + refused.map((x) => `${x.count} × ${x.reason}`).join('；'))
				}
				lines.push('', 'ship 的 target_id 是**舰队 id**（这是能派过去的目标），而 current.target_id 是**舰船 id**——同一条输出两套编号')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 选领袖特质（2026-11-20 接）——
	   桥端 select_leader_trait → CAddTraitFromPoolCommand；桥自己会先检查该特质是否在「本次可选」里。 */
	add({
		name: 'stellaris_select_leader_trait',
		description:
			'Pick one of the traits the engine currently offers a leader (bridge select_leader_trait → ' +
			'CAddTraitFromPoolCommand). The bridge checks the key against the leader\'s current trait_options / ' +
			'trait_upgrade_options and refuses anything not offered, returning the offered list. Read those lists with ' +
			'stellaris_leaders first: has_unspent_trait_points / trait_selections_available are derived from the lists ' +
			'(measured 2026-11-20: the engine counter reads 0 even for leaders the UI offers a pick to).',
		parameters: {
			type: 'object',
			properties: {
				leader_id: { type: 'number', description: 'leader id from stellaris_leaders' },
				trait_key: { type: 'string', description: 'one of trait_options[].key or trait_upgrade_options[].key' },
			},
			required: ['leader_id', 'trait_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const leaderId = Number(args?.leader_id)
			const traitKey = String(args?.trait_key ?? '')
			if (!Number.isFinite(leaderId)) return text('leader_id is required（stellaris_leaders 的 id）')
			if (!traitKey) return text('trait_key is required（trait_options / trait_upgrade_options 里的 key）')
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'select_leader_trait', { leader_id: leaderId, trait_key: traitKey })
				if (r?.error) {
					const offered = Array.isArray(r.error.offered)
						? '\n  本次可选：' + r.error.offered.map((t) => `${t.name || t.key}（${t.key}）`).join(' / ')
						: ''
					return text(`失败：${r.error.message || JSON.stringify(r.error)}${offered}`)
				}
				return text(`已投递：领袖 ${leaderId} ← ${traitKey}\n${JSON.stringify(r)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 物种（2026-11-22 会话 17 重做）----
	   以前是桥的裸转发：一大坨 JSON，22 条里 8 条同名「大魔女」，未开智物种根本不在里面。
	   桥现在读 CCountry::owned_species_refs（+0x3470，物种界面用的就是这张表），未开智的
	   也在列表里；它们带 is_pre_sapient（还没提升，不能增长也不能改造）与 home_planet_id，
	   提升过的带 is_uplifted + uplifter_country_id。 */
	const speciesTraits = (s) => (Array.isArray(s?.traits) ? s.traits : []).map((t) => t?.name || t?.key || '?')
	// 单个物种的卡片：身份与权利走 get_species，点数与特质花费走 get_species_modification_info。
	// 两条都是只读，合计约 50 ms。列表里的 species_id 与 stellaris_species_detail 共用这一份。
	const speciesCard = async (pipe, id) => {
		const one = await rpc(pipe, 'get_species', { species_id: id })
		const s = Array.isArray(one?.species) ? one.species[0] : null
		if (!s) return null
		let mod = null
		try {
			const m = await rpc(pipe, 'get_species_modification_info', { species_id: id })
			if (m?.success) mod = m
		} catch {}
		const lines = []
		const tag = s.is_pre_sapient
			? '未开智（不能增长、不能改造；要先提升才成为正式物种）'
			: s.is_founder ? '建国' : s.in_template_pool ? (s.is_template ? '模板' : '新物种') : s.is_uplifted ? '已提升' : s.is_template ? '亚种' : '物种'
		lines.push(`${s.name}（${s.species_id}）｜${tag}｜${s.class || '?'}`)
		if (typeof s.base_species_id === 'number' && s.base_species_id !== 4294967295) lines.push(`  基础种：${s.base_species_name || '?'}（${s.base_species_id}）`)
		// 机械帝国的建国种常常本身就是亚种（全族更新过模板）：人口都在这个亚种上，根记录还在但没人
		if (s.is_founder && s.is_template && typeof s.base_species_id === 'number' && s.base_species_id !== 4294967295) {
			lines.push(`  建国说明：它是亚种，真正的根是 ${s.base_species_name || '?'}（${s.base_species_id}）——根记录还在，人口已全部迁到这个亚种（本档实测）`)
		}
		if (s.is_template) lines.push(`  模板状态：${s.in_template_pool ? '在模板池（还没应用，改它不碰人口）' : '已存在（实体亚种）'}`)
		if (typeof s.home_planet_id === 'number') lines.push(`  母星：行星 ${s.home_planet_id}`)
		if (typeof s.uplifter_country_id === 'number') lines.push(`  提升者：country ${s.uplifter_country_id}${s.uplifter_country_id === 0 ? '（我们）' : '（不是我们）'}`)
		if (mod) {
			lines.push(`  点数 ${mod.points_total}｜已用 ${mod.points_used}｜剩余 ${mod.points_free}${s.is_pre_sapient ? '（提升后才能用）' : ''}`)
			lines.push(`  可选位 ${mod.picks_total}｜已用 ${mod.picks_used}｜剩余 ${mod.picks_free}`)
			const traits = Array.isArray(mod.current_traits) ? mod.current_traits : []
			lines.push(`  特质 ${traits.length}：${traits.map((t) => `${t.name}（${Number(t.cost) >= 0 ? '+' : ''}${t.cost}）`).join('、') || '（无）'}`)
		} else {
			lines.push(`  点数：剩余 ${s.points_free ?? 0}｜可选位 ${s.picks_free ?? 0}`)
			lines.push(`  特质 ${speciesTraits(s).length}：${speciesTraits(s).join('、') || '（无）'}`)
		}
		const r = s.rights || {}
		const cooldowns = r.cooldowns || {}
		const rightPairs = [
			['公民权', 'citizenship', r.citizenship?.name],
			['生活水平', 'living_standards', r.living_standards?.name],
			['兵役', 'military_service', r.military_service?.name],
			['奴隶', 'slavery_type', r.slavery_type?.name],
			['净化', 'purge_type', r.purge_type?.name],
			['人口控制', 'population_controls', r.population_controls?.name],
			['殖民控制', 'colonization_controls', r.colonization_controls?.name],
			['移民控制', 'migration_controls', r.migration_controls?.name],
			['亚种整合', 'subspecies_integration', r.subspecies_integration?.name],
		]
		const rightBits = rightPairs.filter(([, , v]) => v).map(([label, key, v]) => {
			const cd = Number(cooldowns[key] ?? 0)
			return `${label} ${v}${cd > 0 ? `（冷却 ${cd} 天）` : ''}`
		})
		const rightsScope = r.is_specific === true ? '' : '（帝国默认，没有单独配）'
		if (rightBits.length) lines.push(`  权利${rightsScope}：${rightBits.join('｜')}`)
		else lines.push('  权利：这一条没有权利配置（未开智提升后才有；模板沿用基底）')
		return lines.join('\n')
	}
	add({
		name: 'stellaris_species',
		description:
			'Species of the empire or the galaxy: id, name, portrait class, traits, rights. Empire mode reads the ' +
			"country's own species list (CCountry::owned_species_refs) — the same table the species screen shows — so " +
			'pre-sapients are included too: is_pre_sapient means still pre-sapient (no growth, no modification until ' +
			'uplifted), with home_planet_id; is_uplifted + uplifter_country_id mark a species that was uplifted (it keeps ' +
			'its presapient traits). Templates carry base_species_id — several templates can share one name. species_id ' +
			'prints one species in detail; mode:"galaxy" lists the galaxy (331 species). Read-only.',
		parameters: {
			type: 'object',
			properties: {
				mode: { type: 'string', description: '"empire" (default) or "galaxy"' },
				species_id: { type: 'number', description: 'one species id; omit for the list' },
				raw: { type: 'boolean', description: 'return the raw bridge JSON' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const mode = args?.mode === 'galaxy' ? 'galaxy' : 'empire'
				const params = { mode }
				if (typeof args?.species_id === 'number') params.species_id = args.species_id
				const d = await rpc(pipe, 'get_species', params)
				if (d?.error) return text(`bridge: ${d.error}`)
				if (args?.raw === true) return text(JSON.stringify(d))
				const all = Array.isArray(d?.species) ? d.species : []
				const lines = []

				// 单条详情：与 stellaris_species_detail 用同一份卡片
				if (typeof args?.species_id === 'number') {
					const card = await speciesCard(pipe, args.species_id)
					return text(card || `没有物种 ${args.species_id}`)
				}

				if (mode === 'galaxy') {
					const pre = all.filter((s) => s.is_pre_sapient)
					const up = all.filter((s) => s.is_uplifted)
					lines.push(`全银河 ${Number(d?.species_count ?? all.length)} 个物种｜未开智 ${pre.length}｜已提升 ${up.length}（这里只列这两种，其余看 raw）`)
					for (const s of pre) lines.push(`  未开智 ${String(s.species_id).padStart(4)} ${s.name}｜${s.class || '?'}｜母星 ${s.home_planet_id ?? '?'}`)
					for (const s of up) lines.push(`  已提升 ${String(s.species_id).padStart(4)} ${s.name}｜${s.class || '?'}｜提升者 country ${s.uplifter_country_id ?? '?'}`)
					return text(lines.join('\n'))
				}

				const n = Number(d?.species_count ?? all.length)
				const pre = all.filter((s) => s.is_pre_sapient)
				const up = all.filter((s) => !s.is_pre_sapient && s.is_uplifted)
				const founder = all.filter((s) => s.is_founder)
				const variants = all.filter((s) => !s.is_founder && !s.is_pre_sapient && s.is_template)
				const restNoBase = all.filter((s) => !s.is_founder && !s.is_pre_sapient && !s.is_template)
				const tpl = variants.filter((s) => s.in_template_pool === true).length
				const sub = variants.length - tpl
				const pts = all.filter((s) => Number(s.points_free) > 0 && !s.is_pre_sapient).length
				lines.push(`帝国物种 ${n} 条｜未开智 ${pre.length}｜模板 ${tpl}｜亚种 ${sub}｜已提升 ${up.length}｜有点数 ${pts}`)
				lines.push('模板 = 还没应用，改它不碰人口；亚种 = 已经存在的实体。★ 后面是这个变体还剩几点（未开智的单列）')
				const budget = (s) => {
					const p = Number(s.points_free ?? 0)
					const k = Number(s.picks_free ?? 0)
					if (s.is_pre_sapient) return p > 0 ? `★${p} 点（提升后可用）` : ''
					if (p < 0) return `⚠ 点数为负（${p}）`
					if (p > 0) return k > 0 ? `★${p} 点 / ${k} 选` : `★${p} 点（没有可选位）`
					if (k > 0) return `${k} 选`
					return ''
				}
				const row = (s, indent, withBase) => {
					const tag = s.is_pre_sapient ? '未开智' : s.is_founder ? '建国' : s.in_template_pool ? (s.is_template ? '模板' : '新物种') : s.is_uplifted ? '已提升' : s.is_template ? '亚种' : '物种'
					const bits = [`${indent}${String(s.species_id).padStart(4)} ${padName(s.name, 12)}`, padName(tag, 4), `特质 ${String(speciesTraits(s).length).padStart(2)}`]
					const b = budget(s)
					if (b) bits.push(b)
					if (s.is_uplifted && typeof s.uplifter_country_id === 'number') bits.push(`提升者 country ${s.uplifter_country_id}`)
					if (withBase && s.is_template && typeof s.base_species_id === 'number') bits.push(`${s.is_founder ? '真根' : '基'} ${s.base_species_name || s.base_species_id}（${s.base_species_id}）`)
					if (typeof s.home_planet_id === 'number') bits.push(`母星 ${s.home_planet_id}`)
					return bits.join('｜')
				}
				const section = (title, rows, withBase = false) => {
					if (!rows.length) return
					lines.push('')
					lines.push(title)
					for (const s of rows) lines.push(row(s, '  ', withBase))
				}
				section('建国', founder, true)
				section('未开智（不能增长、不能改造，提升后才算正式物种）', pre)
				// 其余物种都放在这一节：没有基底的直接一行，带基底的按基底归组。
				// 「变体」不是另一类东西——只是这一族改出来的模板/亚种多于一个，才需要并排看。
				if (restNoBase.length || variants.length) {
					lines.push('')
					lines.push('其他物种（带基底的按基底归组；同一物种有多个模板/亚种时并排放，各变体剩余点数不同）')
					for (const s of restNoBase) lines.push(row(s, '  ', true))
					const byBase = new Map()
					for (const s of variants) {
						const k = String(s.base_species_id)
						if (!byBase.has(k)) byBase.set(k, [])
						byBase.get(k).push(s)
					}
					for (const [baseId, rows] of [...byBase.entries()].sort((a, b) => Number(a[0]) - Number(b[0]))) {
						lines.push(`  基底：${rows[0]?.base_species_name || '?'}（${baseId}）`)
						for (const s of rows) lines.push(row(s, '    ', false))
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* 单卡：列表里看中哪一条，再要它的细节——特质（带花费）、点数、九类权利各自的冷却。
	   与列表里传 species_id 用的是同一份卡片；可选特质目录不在这里，去看 species_modification。 */
	add({
		name: 'stellaris_species_detail',
		description:
			'One species in full: identity and status (pre-sapient / uplifted / template-in-pool / existing subspecies), ' +
			'base species, home planet, uplifter, trait budget (points and picks total/used/free), every current trait with ' +
			'its engine cost, and the nine rights categories each with its OWN cooldown (a category can be changed once its ' +
			'days reach 0; the species screen allows one change per category per policy period). rights.is_specific=false ' +
			'means the values shown are the empire default, not configured for this species. For the catalogue of traits you ' +
			'could pick, use stellaris_species_modification. Read-only.',
		parameters: {
			type: 'object',
			properties: {
				species_id: { type: 'number', description: 'species id (see stellaris_species)' },
			},
			required: ['species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.species_id)
				if (!Number.isFinite(id)) return text('需要 species_id')
				const card = await speciesCard(pipe, id)
				return text(card || `没有物种 ${id}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* 改模板之前先看这一条：还剩几点、可选几点、当前特质各花多少、目录里有什么。
	   目录是「全部特质 × 这一物种的引擎花费」，没有按 potential/allow 过滤——能不能真装上由引擎判。 */
	add({
		name: 'stellaris_species_modification',
		description:
			'Trait budget for ONE species before creating/modifying a template (bridge get_species_modification_info): ' +
			'points_total / used / free, picks_total / used / free, the current traits with their engine cost, and the ' +
			'available trait catalog with a cost per trait. NOTE: the catalog is every trait in the game with its cost for ' +
			'this species — it is NOT filtered by potential/allow, so a name here can still be refused by the engine. ' +
			'Read-only.',
		parameters: {
			type: 'object',
			properties: {
				species_id: { type: 'number', description: 'species id (see stellaris_species)' },
				raw: { type: 'boolean', description: 'return the raw bridge JSON' },
			},
			required: ['species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.species_id)
				if (!Number.isFinite(id)) return text('需要 species_id')
				const d = await rpc(pipe, 'get_species_modification_info', { species_id: id })
				if (args?.raw === true) return text(JSON.stringify(d))
				if (!d?.success) return text(`bridge: ${d?.error || '读不到这一条'}`)
				const cur = Array.isArray(d.current_traits) ? d.current_traits : []
				const pre = cur.some((t) => String(t.key || '').includes('presapient'))
				const lines = []
				lines.push(`${d.name}（${d.species_id}）｜${pre ? '带未开智特质' : d.is_template ? '模板' : '物种'}｜${d.class || '?'}`)
				lines.push(`  点数 ${d.points_total}｜已用 ${d.points_used}｜剩余 ${d.points_free}`)
				lines.push(`  可选点 ${d.picks_total}｜已用 ${d.picks_used}｜剩余 ${d.picks_free}`)
				lines.push(`  当前特质：${cur.map((t) => `${t.name}（${Number(t.cost) >= 0 ? '+' : ''}${t.cost}）`).join('、') || '（无）'}`)
				const preTraits = cur.filter((t) => String(t.key || '').includes('presapient')).map((t) => t.name)
				if (preTraits.length) lines.push(`  未开智特质：${preTraits.join('、')}（提升后保留，每个花 0 点）`)
				const cat = Array.isArray(d.available_traits_catalog) ? d.available_traits_catalog : []
				const buckets = new Map()
				for (const t of cat) {
					const c = Number(t.cost ?? 0)
					if (!buckets.has(c)) buckets.set(c, new Set())
					buckets.get(c).add(t.name || t.key)
				}
				lines.push(`  可选特质目录 ${cat.length} 条（全部特质 × 这一物种的花费，没有按能不能装筛）`)
				for (const c of [...buckets.keys()].sort((a, b) => a - b)) {
					const names = [...buckets.get(c)]
					const head = `   ${c > 0 ? '+' : ''}${c} 点（${names.length}）`
					const show = names.slice(0, 8).join('、')
					lines.push(`${head}：${show}${names.length > 8 ? ` …另 ${names.length - 8} 条` : ''}`)
				}
				lines.push(`  剩余 ${d.points_free} 点：花费 ≤ ${d.points_free} 的才配得上；负花费的会腾点数`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* ---- 物种模板：四条写命令（2026-11-22 会话 17）----
	   桥里早就写了 create/modify/delete/apply，但一次都没在游戏里跑过。这里统一口径：
	   默认只印计划，confirm:true 才投；create/modify 先按 get_species_modification_info 把
	   特质花费算一遍（超预算、认不出的键直接拒），删与应用不可逆，一律要 confirm。 */
	const speciesBudgetCheck = async (pipe, speciesId, traits) => {
		const mod = await rpc(pipe, 'get_species_modification_info', { species_id: speciesId })
		if (!mod?.success) return { error: `读不到 ${speciesId} 的点数：${mod?.error || '未知原因'}` }
		const have = new Map((mod.current_traits ?? []).map((t) => [t.key, Number(t.cost ?? 0)]))
		const cat = new Map((mod.available_traits_catalog ?? []).map((t) => [t.key, Number(t.cost ?? 0)]))
		let sum = 0
		let picks = 0
		const unknown = []
		const parts = []
		for (const k of traits) {
			const c = cat.has(k) ? cat.get(k) : have.has(k) ? have.get(k) : null
			if (c === null) {
				unknown.push(k)
				continue
			}
			sum += c
			if (c !== 0) picks++
			parts.push(`${k}（${c >= 0 ? '+' : ''}${c}）`)
		}
		return { mod, sum, picks, unknown, parts }
	}
	add({
		name: 'stellaris_create_species_template',
		description:
			'Create a species modification template from a species (bridge create_species_template { base_species_id, name?, traits[] }). ' +
			'traits is the COMPLETE trait list of the template: a planet-preference trait is inherited from the source when none is ' +
			'given, but every other trait you leave out is absent from the template. The tool first reads the species\' trait budget ' +
			'(get_species_modification_info), refuses an unknown trait key, a total above points_total or more non-zero traits than ' +
			'picks_total, then prints the plan against the budget. **Nothing is posted unless confirm:true.** The template lands in the ' +
			'template pool and shows up at once in stellaris_species; applying it to colonies is stellaris_apply_species_template. ' +
			'HOW THE BASE WORKS (measured 2026-11-22): the game files a subspecies under its ROOT species. Give it a root and you get ' +
			'a real subspecies; point the base at a template/subspecies instead and the game shows an INDEPENDENT species (and apply ' +
			'refuses it). So when base_species_id is itself a subspecies/template (e.g. the founder 115, whose own base is ' +
			'3791650817), the tool records the new species under that root automatically — the seed still supplies class, portrait and ' +
			'traits. allow_variant_source:true keeps the old behaviour (base = the source itself) for experiments; no_base:true makes ' +
			'a standalone species (base_ref 0xFFFFFFFF).',
		parameters: {
			type: 'object',
			properties: {
				base_species_id: { type: 'number', description: 'the seed species (a root species, or a subspecies/template — its root is used as the recorded base)' },
				name: { type: 'string', description: 'template name; omit to inherit the base name' },
				traits: { type: 'array', items: { type: 'string' }, description: 'complete trait key list, e.g. ["trait_intelligent","trait_adaptive"]' },
				allow_variant_source: { type: 'boolean', description: 'record the source itself as the base instead of its root (experimental; the game shows that as an independent species)' },
				no_base: { type: 'boolean', description: 'create a STANDALONE species: seed the traits/class from base_species_id but leave its base_ref at 0xFFFFFFFF (experiment)' },
				confirm: { type: 'boolean', description: 'post the command (otherwise only the plan is printed)' },
			},
			required: ['base_species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const baseId = Number(args?.base_species_id)
				if (!Number.isFinite(baseId)) return text('需要 base_species_id')
				const traits = Array.isArray(args?.traits) ? args.traits.filter((k) => typeof k === 'string' && k) : []
				const one = await rpc(pipe, 'get_species', { species_id: baseId })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${baseId}`)
				const lines = [`种子：${s.name}（${s.species_id}）${s.is_pre_sapient ? '｜未开智' : ''}${s.is_template ? `｜它本身是模板/亚种（基底 ${s.base_species_name || '?'}（${s.base_species_id}））` : ''}`]
				if (s.is_pre_sapient) return text(`${lines.join('\n')}\n✗ 不投：未开智物种要先提升，才能做基因改造`)
				// 游戏的规矩（2026-11-22 实测）：新模板记在「根物种」名下，不是记在源物种名下。
				// 对根物种建模板 → 真亚种；对模板/亚种再建一层（base 指向模板）→ 游戏当成独立物种，
				// 应用也会被拒。所以默认把 base 归一到根物种；allow_variant_source 可以把源物种自己当 base。
				let rootId = s.species_id
				let rootName = s.name
				if (s.is_template && args?.allow_variant_source !== true) {
					let cur = s
					for (let hop = 0; hop < 3 && cur?.is_template; hop++) {
						rootId = cur.base_species_id
						rootName = cur.base_species_name || '?'
						const up = await rpc(pipe, 'get_species', { species_id: rootId })
						cur = Array.isArray(up?.species) ? up.species[0] : null
						if (cur) rootName = cur.name
					}
				}
				if (traits.length) {
					const b = await speciesBudgetCheck(pipe, baseId, traits)
					if (b.error) return text(`${lines.join('\n')}\n✗ ${b.error}`)
					if (b.unknown.length) return text(`${lines.join('\n')}\n✗ 不投：认不出这些特质键 ${b.unknown.join('、')}（目录见 stellaris_species_modification）`)
					lines.push(`计划：特质 ${traits.length} 条｜${b.parts.join('、')}`)
					lines.push(`预算：共花 ${b.sum} 点（总 ${b.mod.points_total}）｜非零 ${b.picks} 个（可选位 ${b.mod.picks_total}）`)
					if (b.sum > Number(b.mod.points_total)) return text(`${lines.join('\n')}\n✗ 不投：超预算 ${b.sum - Number(b.mod.points_total)} 点`)
					if (b.picks > Number(b.mod.picks_total)) return text(`${lines.join('\n')}\n✗ 不投：非零特质 ${b.picks} 个，可选位只有 ${b.mod.picks_total} 个`)
				} else {
					lines.push('计划：不换特质（沿用基底的特质），只起名字')
				}
				const name = typeof args?.name === 'string' ? args.name : ''
				if (name) lines.push(`名字：${name}`)
				const noBase = args?.no_base === true
				const useRoot = !noBase && s.is_template && args?.allow_variant_source !== true
				if (useRoot) lines.push(`记录基底：${rootName}（${rootId}）—— 亚种记在根物种名下，源物种只当种子`)
				if (noBase) lines.push(`新物种：base_ref = 0xFFFFFFFF（独立物种，不是亚种）；特质与立绘取自 ${s.name}`)
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真建请加 confirm: true）`)
				const res = await rpc(pipe, 'create_species_template', {
					base_species_id: baseId,
					name,
					traits,
					no_base: noBase,
					root_base_id: useRoot ? rootId : 4294967295,
				})
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				return text(`${lines.join('\n')}\n已投：${noBase ? '建独立物种' : '建模板'}\n用 stellaris_species 复核`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_modify_species_template',
		description:
			'Change an existing template\'s name and/or traits (bridge modify_species_template { template_species_id, name?, traits[] }). ' +
			'traits again REPLACES the whole trait set (a preference is inherited when none is given). The engine swaps the template for ' +
			'a new species id, so re-read stellaris_species afterwards. Same budget pre-check as create (points_total / picks_total); ' +
			'**nothing is posted unless confirm:true**.',
		parameters: {
			type: 'object',
			properties: {
				template_species_id: { type: 'number', description: 'the template species id (stellaris_species)' },
				name: { type: 'string', description: 'new name; omit to keep the current one' },
				traits: { type: 'array', items: { type: 'string' }, description: 'complete trait key list; omit to keep the current traits' },
				confirm: { type: 'boolean', description: 'post the command (otherwise only the plan is printed)' },
			},
			required: ['template_species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.template_species_id)
				if (!Number.isFinite(id)) return text('需要 template_species_id')
				const traits = Array.isArray(args?.traits) ? args.traits.filter((k) => typeof k === 'string' && k) : []
				const one = await rpc(pipe, 'get_species', { species_id: id })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${id}`)
				const lines = [`模板：${s.name}（${s.species_id}）${s.in_template_pool ? '｜在模板池' : '｜已存在的实体（改它等于改这个亚种）'}`]
				if (s.is_pre_sapient) return text(`${lines.join('\n')}\n✗ 不投：未开智物种不能改造`)
				if (traits.length) {
					const b = await speciesBudgetCheck(pipe, id, traits)
					if (b.error) return text(`${lines.join('\n')}\n✗ ${b.error}`)
					if (b.unknown.length) return text(`${lines.join('\n')}\n✗ 不投：认不出这些特质键 ${b.unknown.join('、')}`)
					lines.push(`计划：特质 ${traits.length} 条｜${b.parts.join('、')}`)
					lines.push(`预算：共花 ${b.sum} 点（总 ${b.mod.points_total}）｜非零 ${b.picks} 个（可选位 ${b.mod.picks_total}）`)
					if (b.sum > Number(b.mod.points_total)) return text(`${lines.join('\n')}\n✗ 不投：超预算 ${b.sum - Number(b.mod.points_total)} 点`)
					if (b.picks > Number(b.mod.picks_total)) return text(`${lines.join('\n')}\n✗ 不投：非零特质 ${b.picks} 个，可选位只有 ${b.mod.picks_total} 个`)
				} else {
					lines.push('计划：不换特质，只改名')
				}
				const name = typeof args?.name === 'string' ? args.name : ''
				if (name) lines.push(`名字：${name}`)
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真改请加 confirm: true）`)
				const res = await rpc(pipe, 'modify_species_template', { template_species_id: id, name, traits })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				return text(`${lines.join('\n')}\n已投：改模板（引擎会换成新的 species id，记得重读）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_delete_species_template',
		description:
			'Delete one species template (bridge delete_species_template { species_id }). Irreversible; the engine refuses when the ' +
			'template is not ours or living pops of it still exist. **Nothing is posted unless confirm:true.**',
		parameters: {
			type: 'object',
			properties: {
				species_id: { type: 'number', description: 'the template species id (stellaris_species)' },
				confirm: { type: 'boolean', description: 'post the delete (otherwise only the plan is printed)' },
			},
			required: ['species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.species_id)
				if (!Number.isFinite(id)) return text('需要 species_id')
				const one = await rpc(pipe, 'get_species', { species_id: id })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${id}`)
				const lines = [`将删除：${s.name}（${s.species_id}）${s.in_template_pool ? '｜在模板池' : '｜不在模板池（可能是实体亚种）'}`]
				if (s.is_founder) return text(`${lines.join('\n')}\n✗ 不投：这是建国物种`)
				if (s.is_pre_sapient) return text(`${lines.join('\n')}\n✗ 不投：未开智物种不是模板`)
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：删除不可逆，要真删请加 confirm: true）`)
				const res = await rpc(pipe, 'delete_species_template', { species_id: id })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				return text(`${lines.join('\n')}\n已删除`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_apply_species_template',
		description:
			'Apply a template to colonies (bridge apply_species_template { template_species_id, colony_ids[] }). This does NOT change ' +
			'pops at once: it starts a species-modification SPECIAL PROJECT (society research, shows up in the intel log as ' +
			'kind=species_modification). colony_ids are colony ids; omit to apply to every colony of the empire. Template and colony ids ' +
			'come from stellaris_species / stellaris_colonies. **Nothing is posted unless confirm:true.**',
		parameters: {
			type: 'object',
			properties: {
				template_species_id: { type: 'number', description: 'the template species id (stellaris_species)' },
				colony_ids: { type: 'array', items: { type: 'number' }, description: 'colony ids; omit for every colony' },
				confirm: { type: 'boolean', description: 'post the order (otherwise only the plan is printed)' },
			},
			required: ['template_species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.template_species_id)
				if (!Number.isFinite(id)) return text('需要 template_species_id')
				const colonyIds = Array.isArray(args?.colony_ids) ? args.colony_ids.map(Number).filter((n) => Number.isFinite(n)) : []
				const one = await rpc(pipe, 'get_species', { species_id: id })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${id}`)
				const lines = [`模板：${s.name}（${s.species_id}）｜基底 ${s.base_species_name || '?'}（${s.base_species_id}）`]
				if (s.is_pre_sapient) return text(`${lines.join('\n')}\n✗ 不投：未开智物种不能改造`)
				lines.push(colonyIds.length ? `目标：${colonyIds.length} 个殖民地（${colonyIds.join('、')}）` : '目标：全部殖民地（不给 colony_ids）')
				lines.push('注意：投出的是「改造特殊项目」，不是立刻改完；项目在情报日志里，要花社会学研究')
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真投请加 confirm: true）`)
				const res = await rpc(pipe, 'apply_species_template', { template_species_id: id, colony_ids: colonyIds })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				return text(`${lines.join('\n')}\n已投：改造项目（影响 ${res?.colonies_affected ?? colonyIds.length} 个殖民地）\n用 stellaris_special_projects 查 kind=species_modification`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* 提升：把未开智物种变成正式物种（桥 begin_uplifting → CBeginUpliftingCommand，token 0x2F78）。
	   投出的是特殊项目（社会学研究 + 工期），不是立刻完成；默认只印计划。 */
	add({
		name: 'stellaris_uplift_species',
		description:
			'Uplift a pre-sapient species (bridge begin_uplifting { species_id } → CBeginUpliftingCommand, token 0x2F78). ' +
			'The tool refuses anything that is not pre-sapient (no trait_presapient_* trait, or already trait_uplifted). ' +
			'Uplifting starts a SPECIAL PROJECT (society research + build time; it shows up in the intel log as kind=uplift) — ' +
			'it does not finish at once, so check stellaris_special_projects afterwards. After posting it also starts the project ' + ' (research_project on the new kind=uplift entry) — without that the project just expires after 30 days. **Nothing is posted unless confirm:true.**',
		parameters: {
			type: 'object',
			properties: {
				species_id: { type: 'number', description: 'pre-sapient species id (stellaris_species)' },
				confirm: { type: 'boolean', description: 'post the order (otherwise only the plan is printed)' },
			},
			required: ['species_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.species_id)
				if (!Number.isFinite(id)) return text('需要 species_id')
				const one = await rpc(pipe, 'get_species', { species_id: id })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${id}`)
				const lines = [`${s.name}（${s.species_id}）｜${s.class || '?'}`]
				if (!s.is_pre_sapient) {
					return text([...lines,
						s.is_uplifted ? '✗ 已经提升过了' : '✗ 不是未开智物种（没有未开智特质）',
						'只有未开智物种可以提升'].join('\n'))
				}
				if (typeof s.home_planet_id === 'number') lines.push(`母星：行星 ${s.home_planet_id}`)
				lines.push(`点数 ${s.points_free ?? 0} / 可选 ${s.picks_free ?? 0}（提升时能花）`)
				lines.push('代价：提升是特殊项目（社会学研究 + 工期），不是立刻完成；投完在情报日志看 kind=uplift')
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真提升请加 confirm: true）`)
				const res = await rpc(pipe, 'begin_uplifting', { species_id: id })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				// 投出只是「建了项目」；不启动研究的话它挂 30 天就过期（2026-11-22 玩家指出，
				// 实测：没启动的项目没有「进行中」、研究点不扣、到期消失）。这里顺手启动它。
				let started = null
				try {
					const log = await readSituationLog(true)
					const list = Array.isArray(log?.special_projects) ? log.special_projects : []
					const cand = list
						.filter((p) => p && p.kind === 'uplift' && Number(p.status) !== 1 && Number(p.status) !== 2)
						.filter((p) => !s.name || String(p.name || '').includes(s.name))
						.sort((a, b) => Number(b.id) - Number(a.id))
					if (cand.length) {
						const st = await rpc(pipe, 'research_project', { project_id: cand[0].id })
						if (st && st.success !== false) started = cand[0].id
					}
				} catch {}
				const tail = started
					? `已投并启动：提升项目 ${started}\n用 stellaris_special_projects 复核（应有「进行中」，研究点每月 1 日结算）`
					: '已投：提升项目（**没有自动启动成功**——去 stellaris_special_projects 找 kind=uplift、用 stellaris_special_project_research 启动，否则 30 天后会过期）'
				return text(`${lines.join('\n')}\n${tail}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	const fix2 = (v) => (typeof v === 'number' ? Math.round(v * 100) / 100 : v)
	const signed2 = (v) => (typeof v === 'number' ? `${v >= 0 ? '+' : ''}${fix2(v)}` : String(v))

	// 情报日志的三段共用一个桥方法 get_situation_log；工具按“看哪一段”拆开，
	// 这样名字里就写着内容（找异常时不用记得它还叫 situation log）。
	const readSituationLog = async (playerOnly) => {
		const pipe = config.pipe || DEFAULTS.pipe
		return (await rpc(pipe, 'get_situation_log', { player_only: playerOnly })) || {}
	}
	add({
		name: 'stellaris_situations',
		description:
			'Active situations only — the stage-driven long-running states (deficits, plagues, crises) from the intel log. Each gives stage (the 0-based CSituation::stage index at CSituation+0x578), the stage\'s absolute range (stage_min / stage_max, cached inline at CSituation+0x598 as {begin,end} int64 fixed points — the same numbers the UI prints as "this stage is in effect between x and y"), the progress inside that stage (0-100), the monthly change and the current approach. For special projects use stellaris_special_projects; for the anomalies still unresearched use stellaris_anomalies. raw:true returns the bridge JSON unchanged.',
		parameters: {
			type: 'object',
			properties: {
				raw: { type: 'boolean', description: 'return the raw bridge JSON instead of the table' },
				player_only: { type: 'boolean', description: "default true; false also lists other empires' situations" },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const r = await readSituationLog(args?.player_only !== false)
				if (args?.raw === true) return text(JSON.stringify(r))
				const sits = r?.situations ?? []
				if (!sits.length) return text('局势 0 条')
				const lines = [`局势 ${sits.length} 条`]
				for (const s of sits) {
					const range =
						typeof s.stage_min === 'number' && typeof s.stage_max === 'number' && s.stage_max > s.stage_min
							? `（${fix2(s.stage_min)}–${fix2(s.stage_max)}）`
							: ''
					lines.push(
						`  [${s.id}] ${s.name || s.key}  阶段 ${s.stage ?? '?'}${range}｜进度 ${fix2(s.progress)}｜每月 ${signed2(s.monthly_change)}｜${s.current_approach_name || s.current_approach || '—'}`,
					)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_anomalies',
		description:
			'Anomalies the empire has discovered but not researched yet — the 「异常」 section of the intel log. Read from the per-country CCountryEventManager.anomalies (a list of planet refs) and then the planet\'s own anomaly object: a planet keeps its anomaly until it is researched, after which it holds a null object and drops off this list, so this is "still to do", not "all anomalies seen". Each row gives the anomaly name plus the planet it sits on. NOT included here: archaeological sites and astral rifts (galaxy-level databases — see the archaeology/rift tools).',
		parameters: {
			type: 'object',
			properties: { raw: { type: 'boolean', description: 'return the raw bridge JSON instead of the table' } },
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const r = await readSituationLog(true)
				if (args?.raw === true) return text(JSON.stringify(r))
				const anomalies = r?.anomalies ?? []
				if (!anomalies.length) return text('异常 0 条（没有待研究的异常）')
				// 「正在调查」不记在 anomaly 对象上（对比十个实例：+0x60 是难度档、+0x30 是序号），
				// 也不可靠名字匹配（同星系可能同名天体）。真正的答案在 CFleet +0x2C8 的当前指令上：
				//   +0x18 = 指令 token（research_anomaly_order = 0x2CF0）、+0x28 = 目标天体 id
				// 所以先用 status 里出现「正在调查」的船挑出候选，再用 probe_fleet_orders 拿到 planet id。
				const pipe = config.pipe || DEFAULTS.pipe
				let ships = []
				try {
					const cf = await rpc(pipe, 'get_civilian_fleets')
					ships = cf?.science_ships ?? []
				} catch {}
				const busy = new Map() // planet_id -> "船名 N%"
				// research_anomaly_order 的命令 token；普通调查（survey）也是「正在调查…」，靠 token 区分
				const TOKEN_RESEARCH_ANOMALY = 0x2CF0
				for (const s of ships) {
					const st = String(s.status || '')
					if (!st.includes('正在调查')) continue
					const pct = /[：:]\s*(\d+)%/.exec(st)?.[1]
					let target = 0
					try {
						const pf = await rpc(pipe, 'probe_fleet_orders', { fleet_id: s.fleet_id })
						const cur = pf?.current_order
						// 只认「研究异常」这条指令；survey 等其他指令一律不算
						if (cur?.has_order && Number(cur.token) === TOKEN_RESEARCH_ANOMALY) target = Number(cur.target) || 0
					} catch {}
					if (!target) continue // 拿不到或不是研究异常 → 宁可不标，避免标错
					busy.set(target, `${s.name || s.fleet_id}${pct ? ` ${pct}%` : ''}`)
				}
				const lines = [`异常 ${anomalies.length} 条（待研究）`]
				for (const a of anomalies) {
					// planet_id 给 stellaris_research_anomaly，system_id 给桥里的星系版命令
					const who = busy.get(Number(a.planet_id))
					const diff = a.difficulty ? `  难度${a.difficulty}` : ''
					lines.push(
						`  ${a.name || a.key}＠${a.planet_name || a.planet_id}${diff}  planet=${a.planet_id}  system=${a.system_id ?? '?'}` +
							(who ? `  ← ${who} 正在调查` : ''),
					)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_special_projects',
		description:
			'Special projects in the intel log — the 「特殊项目」 section. Two kinds matter: needs_ship (the project sits at a location — planet / fleet / system / debris / megastructure — and a science ship must be sent there; start it with stellaris_collect_data) and direct research (no location; start it with stellaris_special_project_research). days_left >= 0 means a deadline, otherwise it is unlimited. status: 0 not started (or discarded from the log), 1 in progress (a science ship is working on it, or research points are being spent), 2 COMPLETED — the project has been finished, its permanent effect stays and the entry remains in the intel log; other projects disappear from the list when they finish (verified with the user 2026-11-19). Starting a project that is not 0 is refused by the engine. Each row also gives kind (project / species_modification / uplift / debris) and the location (type 2 planet / 3 fleet / 4 system / 5 debris / 6 megastructure / 7 wormhole / 9 astral rift); fleet locations resolve to the fleet name.',
		parameters: {
			type: 'object',
			properties: { raw: { type: 'boolean', description: 'return the raw bridge JSON instead of the table' } },
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const r = await readSituationLog(true)
				if (args?.raw === true) return text(JSON.stringify(r))
				const projects = r?.special_projects ?? []
				if (!projects.length) return text('特殊项目 0 条')
				// 位置类型（ESpatialObjectType）：2 行星 / 3 舰队 / 4 星系 / 5 残骸 / 6 巨型结构 / 7 天然虫洞 / 9 星界裂隙
				const SPATIAL = { 2: '行星', 3: '舰队', 4: '星系', 5: '残骸', 6: '巨型结构', 7: '天然虫洞', 9: '星界裂隙' }
				// status（2026-11-19 由用户逐条核对确认）：
				//   0 未开始（或从这个记录里丢弃了）／1 进行中／2 已完成，永久效果生效且条目仍留在日志里
				const STATUS = { 1: '进行中', 2: '已完成' }
				// 定义层告诉我们这个项目要哪种船（science_ship / constructor / …）
				let shipNeeds = new Map()
				try {
					shipNeeds = await loadProjectShipNeeds()
				} catch {}
				const fmt = (p) => {
					// 两类区分：要不要派舰船（needs_ship）与有无限时（days_left）
					let how = '直接研究'
					if (p.needs_ship) {
						const kind = SPATIAL[p.location_type] || `类型${p.location_type}`
						const where = p.location_name ? `「${p.location_name}」` : ` ${p.location_id}`
						const classes = shipNeeds.get(p.key)
						const who = classes
							? `（需${[...classes].map((c) => SHIP_CLASS_NAMES[c] || c).join('／')}）`
							: ''
						how = `需舰船 ${kind}${where}${who}`
					}
					const left = typeof p.days_left === 'number' && p.days_left >= 0 ? `剩 ${p.days_left} 天` : '不限时'
					const st = STATUS[p.status] ?? (p.status ? `status=${p.status}` : '')
					return `  [${p.id}] ${p.name || p.key}（${p.kind}）｜${how}｜${left}${st ? `｜${st}` : ''}`
				}
				// 进行中排最前，未开始居中，已完成收在末尾单独一段（已完成=过去的事，别和待办混在一起）
				const running = projects.filter((p) => p.status === 1)
				const todo = projects.filter((p) => !p.status)
				const done = projects.filter((p) => p.status === 2)
				const other = projects.filter((p) => p.status && p.status !== 1 && p.status !== 2)
				const lines = [`特殊项目 ${projects.length} 条（进行中 ${running.length}／未开始 ${todo.length}／已完成 ${done.length}）`]
				for (const p of running) lines.push(fmt(p))
				for (const p of todo) lines.push(fmt(p))
				for (const p of other) lines.push(fmt(p))
				if (done.length) {
					lines.push('── 已完成（永久效果已生效，条目仍留在情报日志）──')
					for (const p of done) lines.push(fmt(p))
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_open_events',
		description:
			'Player events that are currently open, from the global CGameState+0x180 array (COpenPlayerEvent). Unlike stellaris_events this also lists events with NO classic window — the first-contact / astral-rift stage popups live here, which is why they were once believed to have no non-GUI route. Each row gives id (COpenPlayerEvent id), event_key, event_id (the value at CEvent+0x8, which is what the select command wants), options, ope (the COpenPlayerEvent address; its +0x10 is the CEventScope to copy) and has_window (whether an event window at idler+0x170 points at it).',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'list_open_events')
				if (!r || r.error) return text(`bridge: ${(r && r.error) || 'no response'}`)
				const events = r.events ?? []
				if (!events.length) return text('没有打开中的玩家事件')
				const lines = [`打开中的玩家事件 ${events.length} 条（有经典窗口的 ${events.filter((e) => e.has_window).length} 条）`]
				for (const e of events) {
					lines.push(
						`  [${e.id}] ${e.event_key}  event_id=${e.event_id}  选项${e.options}  ope=${e.ope}${e.has_window ? '  有窗口' : ''}`,
					)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* —— 事件文本：从 events/*.txt 取标题 / 叙述 / 选项名（2026-11-22）——
	   事件窗口开着时 stellaris_event_options 能读选项文本，但标题与叙述不在内存里；
	   这里按 event_key 找脚本块，再用引擎 localize 出中文。 */
	const findEventBlock = (text, key) => {
		const esc = key.replace(/\./g, "\\.")
		// 定义块：astral_rift_event = { \n id = KEY … } / country_event = { id = KEY … }
		const defRe = new RegExp("(?:^|\\n)[ \\t]*[A-Za-z_][A-Za-z0-9_]*\\s*=\\s*\\{\\s*\\n[ \\t]*id\\s*=\\s*" + esc + "\\b", "m")
		let m = defRe.exec(text)
		if (!m) return null
		const brace = text.indexOf("{", m.index)
		let depth = 0
		let endIdx = brace
		for (let i = brace; i < text.length; i += 1) {
			if (text[i] === "{") depth += 1
			else if (text[i] === "}") {
				depth -= 1
				if (depth === 0) {
					endIdx = i
					break
				}
			}
		}
		return text.slice(brace, endIdx + 1)
	}
	add({
		name: 'stellaris_event_text',
		description:
			'Show the STORY text of one event from the game files: title, description and each option\'s name, localised ' +
			'through the engine. Bridge offers no such read (the open-event window exposes option text only), so this parses ' +
			'events/*.txt: it finds the block that declares `id = <event_key>` (country_event / ship_event / astral_rift_event / …) ' +
			'and pulls `title`, `desc` (plain key or `desc = { text = … }`) and the `name` of every `option = { … }`. Works for ' +
			'any event key, including rift stage events like astral_rift.3120, whether or not it is currently open. ' +
			'pass `raw: true` to also dump the matched script block.',
		parameters: {
			type: 'object',
			properties: {
				event_key: { type: 'string', description: 'event key, e.g. astral_rift.3120 or ancrel.14510' },
				raw: { type: 'boolean', description: 'also print the raw script block' },
			},
			required: ['event_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const key = String(args?.event_key ?? '')
			if (!key) return text('event_key is required')
			const pipe = config.pipe || DEFAULTS.pipe
			let hit = null
			try {
				for (const root of EDICT_GAME_DIRS) {
					const dir = path.join(root, 'events')
					if (!existsSync(dir)) continue
					for (const name of readdirSync(dir)) {
						if (!name.endsWith('.txt')) continue
						const full = path.join(dir, name)
						const body = findEventBlock(readFileSync(full, 'utf8'), key)
						if (body) {
							hit = { file: name, body }
							break
						}
					}
					if (hit) break
				}
			} catch {}
			if (!hit) return text(`没有在 events/*.txt 里找到 id = ${key} 的事件块（键可能写错，或事件不属于 events 目录）`)
			const unq = (s) => s.replace(/^"|"$/g, "")
			const pick = (re) => {
				const m = re.exec(hit.body)
				return m ? unq(m[1]) : ""
			}
			const title = pick(/^[ \t]*title\s*=\s*"?([A-Za-z0-9_.]+)"?/m)
			// desc 可能是普通键，也可能是带 trigger 的多个 desc = { text = … } 变体
			const descs = []
			const plain = /^[ \t]*desc\s*=\s*"?([A-Za-z0-9_.]+)"?[ \t]*$/m.exec(hit.body)
			if (plain) descs.push({ key: unq(plain[1]), conditional: false })
			const descRe = /^[ \t]*desc\s*=\s*\{/gm
			let dm
			while ((dm = descRe.exec(hit.body))) {
				const brace = hit.body.indexOf("{", dm.index)
				let depth = 0
				let end = brace
				for (let i = brace; i < hit.body.length; i += 1) {
					if (hit.body[i] === "{") depth += 1
					else if (hit.body[i] === "}") {
						depth -= 1
						if (depth === 0) {
							end = i
							break
						}
					}
				}
				const chunk = hit.body.slice(brace, end + 1)
				const tx = /^[ \t]*text\s*=\s*"?([A-Za-z0-9_.]+)"?/m.exec(chunk)
				if (tx) descs.push({ key: unq(tx[1]), conditional: /^[ \t]*trigger\s*=\s*\{/m.test(chunk) })
			}
			const optionNames = []
			const optRe = /^[ \t]*option\s*=\s*\{/gm
			let om
			while ((om = optRe.exec(hit.body))) {
				const brace = hit.body.indexOf("{", om.index)
				let depth = 0
				let end = brace
				for (let i = brace; i < hit.body.length; i += 1) {
					if (hit.body[i] === "{") depth += 1
					else if (hit.body[i] === "}") {
						depth -= 1
						if (depth === 0) {
							end = i
							break
						}
					}
				}
				const chunk = hit.body.slice(brace, end + 1)
				const nm = /^[ \t]*name\s*=\s*"?([A-Za-z0-9_.]+)"?/m.exec(chunk)
				optionNames.push(nm ? unq(nm[1]) : "")
			}
			const keys = [title, ...descs.map((d) => d.key), ...optionNames].filter(Boolean)
			let loc = {}
			try {
				const r = await rpc(pipe, 'localize', { keys })
				loc = r?.names ?? {}
			} catch {}
			const L = (k) => (k && loc[k] && loc[k] !== k ? loc[k] : k || '（无）')
			const lines = [`事件 ${key}（events\\${hit.file}）`]
			lines.push(` 标题：${L(title)}${title ? `（${title}）` : ""}`)
			if (!descs.length) lines.push(" 叙述：（脚本里没有 desc）")
			descs.forEach((d, i) => {
				const tag = descs.length > 1 ? ` [叙述 ${i + 1}${d.conditional ? "，条件变体" : ""}]` : ""
				lines.push(` 叙述${tag}：${L(d.key)}${d.key ? `（${d.key}）` : ""}`)
			})
			if (!optionNames.length) lines.push(' 选项：（脚本里没有 option 块）')
			optionNames.forEach((k, i) => lines.push(` [${i}] ${L(k)}${k ? `（${k}）` : ''}`))
			if (args?.raw === true) {
				lines.push('')
				lines.push(hit.body)
			}
			return text(lines.join('\n'))
		},
	})

	add({
		name: 'stellaris_answer_event',
		description:
			'Answer ONE option of an open player event, without the GUI and without a window — the engine\'s own path. It hands the COpenPlayerEvent pointer (the ope column of stellaris_open_events) to the same worker the option button calls (RVA 0x8E4550), so the example engine builds the whole CSelectEventOptionCommand itself and the option\'s script effects really run (verified 2026-11-19: a first-contact stage lock advanced and its enable_special_project spawned the project). Works for popups that have no CEventWindow at all — first contact (grand_archive.*, first_contact.*), and any other open event, including archaeology/astral-rift events if their popup has one (ancrel.* do show up in stellaris_open_events). Not first-contact-only. Use dry_run first to see event_key and the option count. The event is consumed on the next tick: read state again after a moment, still_open from the post itself means nothing.',
		parameters: {
			type: 'object',
			properties: {
				ope: { type: 'number', description: 'the ope column from stellaris_open_events (COpenPlayerEvent address)' },
				option: { type: 'number', description: '0-based option index; the event has `options` of them' },
				dry_run: { type: 'boolean', description: 'only validate the pointer/index and report event_key, do not post' },
			},
			required: ['ope', 'option'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'select_open_event_option', {
					ope: args.ope,
					option: args.option,
					dry_run: args.dry_run === true,
				})
				if (!r) return text('bridge: no response')
				if (r.error) return text(`失败：${r.error.message || JSON.stringify(r.error)}`)
				if (args.dry_run === true) {
					const list = (r.option_list ?? [])
						.map((o) => `  [${o.index}]${o.is_valid ? '' : '（不可选）'} ${o.text}\n      ${String(o.effects || '').replace(/\s+/g, ' ').trim()}`)
						.join('\n')
					return text(`可以投（未执行）：${r.event_key} 共 ${r.options} 个选项，投的是 ${r.option}\n${list}`)
				}
				if (r.success !== true) return text('投递失败')
				return text(
					`已投：${r.event_key} 的选项 ${r.option}（事件会在下一 tick 被消费，读状态前先让时间走一步）`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_event_options',
		description:
			'Read the options of ONE open player event without posting anything: for each option the index, the text the game ' +
			'would show, the effect tooltip of the hover box, and — unless filter:false — a 判定 line from the event script. ' +
			'That last part matters because is_valid only checks the option\'s allow block, NOT its trigger: an option filtered by ' +
			'a trigger (first_contact.15\'s "abduct a specimen", ancrel.14510\'s spiritualist/predator options) still reports ' +
			'is_valid true while the game never shows it. The filter parses events/*.txt (namespace → file index), takes each ' +
			'option\'s trigger and evaluates it three-valued against the same facts the edicts tool uses (authority, ethics, ' +
			'civics, origin, traditions, perks, techs): 可选 / 不可选（失败的子句）/ 判不了（条件原文）. owner = { … } blocks are ' +
			'country scope and are evaluated; from / fromfrom (site, fleet) and has_country_flag stay unknown, never true.',
		parameters: {
			type: 'object',
			properties: {
				ope: { type: 'number', description: 'the ope column from stellaris_open_events (preferred)' },
				id: { type: 'number', description: 'or the COpenPlayerEvent id from stellaris_open_events, e.g. 77' },
				filter: { type: 'boolean', description: 'evaluate each option\'s trigger from the event script (default true)' },
				selectable_only: { type: 'boolean', description: 'only list options whose verdict is 可选' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'get_event_options', {
					ope: args.ope ?? 0,
					id: args.id ?? 0xffffffff,
				})
				if (!r) return text('bridge: no response')
				if (r.error) return text(`失败：${r.error.message || JSON.stringify(r.error)}`)
				const wantFilter = args?.filter !== false
				let script = null
				let facts = null
				if (wantFilter) {
					try {
						script = await loadEventOptions(r.event_key)
						if (script) facts = await edictFacts(pipe)
					} catch {
						script = null
					}
				}
				const verdict = (i) => {
					const opt = script?.options?.[i]
					if (!opt) return { state: 'unknown', line: '' }
					const allow = blockVerdict(opt.allow, facts)
					if (allow.state === 'false') return { state: 'false', line: `\n   判定：不可选（allow：${allow.failed}）` }
					const tr = blockVerdict(opt.trigger, facts)
					if (tr.state === 'true') return { state: 'true', line: '\n   判定：可选' }
					if (tr.state === 'false') return { state: 'false', line: `\n   判定：不可选（${tr.failed}）` }
					const raw = String(opt.trigger || '').replace(/\s+/g, ' ').trim()
					return { state: 'unknown', line: `\n   判定：判不了（${raw ? raw.slice(0, 120) : '无 trigger'}）` }
				}
				const onlySelectable = args?.selectable_only === true
				const rows = (r.option_list ?? []).map((o) => ({ o, v: verdict(o.index) }))
				const counts = { true: 0, false: 0, unknown: 0 }
				for (const row of rows) counts[row.v.state] += 1
				const list =
					rows
						.filter((row) => !onlySelectable || row.v.state === 'true')
						.map(({ o, v }) => {
							const mark = v.state === 'true' ? '✓ ' : v.state === 'false' ? '✗ ' : '? '
							return (
								`[${o.index}]${mark}${o.is_valid ? '' : '（引擎标 allow 不过）'} ${o.text}\n   ` +
								String(o.effects || '').replace(/\s+/g, ' ').trim() +
								v.line
							)
						})
						.join('\n') || '（没有判定为可选的选项）'
				const notes = []
				if (wantFilter && !script) notes.push(`找不到 ${r.event_key} 的脚本，未判（可用 filter:false 关掉这段）`)
				if (script && script.options.length !== (r.option_list ?? []).length) {
					notes.push(`脚本 ${script.options.length} 条 / 事件 ${(r.option_list ?? []).length} 条，判定按下标对，可能错位`)
				}
				if (!wantFilter) notes.push('filter:false —— 只读引擎字段，没看脚本 trigger')
				return text(
					`${r.event_key}（open_event_id ${r.open_event_id}，ope ${r.ope}）· ${rows.length} 个选项${script ? `（可选 ${counts.true} / 不可选 ${counts.false} / 判不了 ${counts.unknown}）` : ''}\n${list}` +
						(notes.length ? `\n（${notes.join('；')}）` : ''),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_advance_first_contact',
		description:
			'Advance a locked first contact the way the UI button does — three steps in the right order. Measured 2026-11-19/20: the click sets CFirstContact+0xC0 = 1 (which is what clears alert 38 and the locked pane), posts the stage event, and the stage transition resets +0xC0 back to 0. Doing only the post leaves the alert behind, so call this instead of stellaris_answer_event for first contacts. Give country_id (stellaris_first_contacts), ope and option (stellaris_open_events). Finish with settle:true after the tick if that stage completed the contact (status 2): completed contacts are normally left with +0xC0 = 0, and the stage transition only resets it when there is a further stage. settle:true touches nothing else — it writes 0 only when status != 1 and the byte is not already 0.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'counterparty country id from stellaris_first_contacts' },
				ope: { type: 'number', description: 'the ope column of that contact\'s stage event (stellaris_open_events)' },
				option: { type: 'number', description: '0-based option index of that event' },
				settle: { type: 'boolean', description: 'true = only normalise +0xC0 back to 0 when the contact is no longer waiting (call after the tick)' },
			},
			required: ['country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'advance_first_contact_stage', {
					country_id: args.country_id,
					ope: args.ope ?? 0,
					option: args.option ?? 0,
					settle: args.settle === true,
				})
				if (!r) return text('bridge: no response')
				if (r.error) return text(`失败：${r.error.message || r.error}`)
				if (args.settle === true) {
					if (r.changed === false) return text(`收尾：无需改动（status ${r.status}，+0xC0 已是 ${r.flag}）`)
					return text(`收尾：+0xC0 ${r.flag_before} → ${r.flag_after}（status ${r.status}）`)
				}
				const posted = r.posted ?? {}
				return text(
					`已按 UI 的顺序推进：+0xC0 ${r.flag_before} → ${r.flag_set}，事件 ${posted.event_key ?? '?'} 的选项 ${posted.option ?? '?'} 已投\n` +
						`过一 tick 再读 ${args.country_id}；若它这一下完成了（status 2），再调一次 settle:true 把 +0xC0 归 0`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	// 项目定义里「需要什么船」：common/special_projects/*.txt 的 requirements / triggered_requirement
	// 里写 `is_ship_class = shipclass_X` 或 `shipclass_X = 1`（X = science_ship / constructor / …）
	const SHIP_CLASS_NAMES = {
		shipclass_science_ship: '科研船',
		shipclass_constructor: '工程船',
		shipclass_transport: '运输船',
		shipclass_military: '军事舰',
	}
	let PROJECT_SHIPS_CACHE = null
	const loadProjectShipNeeds = async () => {
		if (PROJECT_SHIPS_CACHE) return PROJECT_SHIPS_CACHE
		const map = new Map()
		PROJECT_SHIPS_CACHE = map
		const dir = await findGameDir()
		if (!dir) return map
		let files = []
		try {
			files = (await fs.readdir(path.join(dir, 'common', 'special_projects'))).filter((f) => f.endsWith('.txt'))
		} catch {
			return map
		}
		for (const f of files) {
			let txt = ''
			try {
				txt = await fs.readFile(path.join(dir, 'common', 'special_projects', f), 'utf8')
			} catch {
				continue
			}
			// 按 special_project = { 切块，每块里取 key 与舰种
			const re = /special_project\s*=\s*\{/g
			let m
			while ((m = re.exec(txt))) {
				const blk = braceBlock(txt, txt.indexOf('{', m.index))
				if (blk === null) continue
				// 键有两种写法：带引号 key = "X"，和不带引号 key = X
				const key = /key\s*=\s*"?([A-Za-z0-9_]+)"?/.exec(blk)?.[1]
				if (!key) continue
				const classes = new Set()
				for (const mm of blk.matchAll(/is_ship_class\s*=\s*(shipclass_\w+)/g)) classes.add(mm[1])
				for (const mm of blk.matchAll(/(?:^|\s)(shipclass_\w+)\s*=\s*\d+/g)) classes.add(mm[1])
				if (classes.size) map.set(key, classes)
			}
		}
		return map
	}
	add({
		name: 'stellaris_special_project_collect',
		description:
			'Send a fleet to research ONE located special project (needs_ship = true ones). Posts CCollectDataFleetOrderCommand (token 0x31F5: fleet +0x20 / galactic_object +0x24 / id +0x28 / queue +0x2C). Two modes: give project_id and the engine finds where it is (galactic_object = -1), or give system_id to research every located project of yours in that system. The target argument is a STAR SYSTEM, never a planet/fleet/debris — the project\'s own location only tells you where it sits. Measured 2026-11-19: collect_data { fleet_id: 776, project_id: 11 } put the science ship straight into 「外星日志（0%）」. Requires the right ship class: the definition\'s requirements block names it (common/special_projects/*.txt, e.g. is_ship_class = shipclass_constructor), and the engine refuses a wrong fleet — its reason is returned verbatim. For direct-research projects (no location) use stellaris_special_project_research instead.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id (science ship / constructor as the project requires)' },
				project_id: { type: 'number', description: 'id from stellaris_special_projects (then the engine finds the location)' },
				system_id: { type: 'number', description: 'alternative: research every located project in this system' },
				queue: { type: 'boolean', description: 'append instead of replacing the fleet queue' },
			},
			required: ['fleet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			if (args.project_id == null && args.system_id == null) return text('需要 project_id 或 system_id 之一')
			try {
				const r = await rpc(pipe, 'collect_data', {
					fleet_id: args.fleet_id,
					project_id: args.project_id ?? 0xffffffff,
					system_id: args.system_id ?? 0xffffffff,
					queue: args.queue === true,
				})
				if (!r || r.success === false) return text(`引擎拒绝：${(r && r.error) || '未知原因'}`)
				return text(`已下研究指令：project ${r.project_id ?? args.project_id ?? '-'}（fleet ${r.fleet_id ?? args.fleet_id}）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_special_project_research',
		description:
			'Start (or cancel) a DIRECT-RESEARCH special project — the ones with no location, e.g. 安抚变形虫 / 量身定制外交 / 信念之源. This posts CResearchProjectCommand (token 0x2D8F: country +0x20 / special_project +0x24), or CCancelProjectCommand (0x31AC) with cancel:true. Projects that need a ship (needs_ship in stellaris_special_projects) are NOT started here — use stellaris_collect_data with a science ship. The engine refuses to start a project that is already in progress; when its IsValid gives no reason the refusal is reported without one.',
		parameters: {
			type: 'object',
			properties: {
				project_id: { type: 'number', description: 'id from stellaris_special_projects' },
				cancel: { type: 'boolean', description: 'cancel the project instead of starting it' },
			},
			required: ['project_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'research_project', { project_id: args.project_id, cancel: args.cancel === true })
				if (!r || r.success === false) return text(`引擎拒绝：${(r && r.error) || '未知原因'}`)
				return text(args.cancel === true ? `已取消项目 ${args.project_id}` : `已启动项目 ${args.project_id}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})	/* ---- 局势详情：定义来自 common/situations/*.txt，实时数值来自桥 ----
	 * 桥只有「阶段号 / 阶段区间 / 进度 / 每月变化 / 当前取向」，阶段效果与取向效果是脚本里的
	 * modifier 块（含按 potential 条件生效的 triggered_modifier）。这里解析脚本，potential 用
	 * 三值逻辑判（判不了的按「条件性」标注，不假装是真）。
	 */
	const stripScriptComment = (text) => {
		// 整份文本按引号状态逐字符去注释（字符串里的 # 不算注释）
		let out = ''
		let inStr = false
		for (let i = 0; i < text.length; i++) {
			const c = text[i]
			if (c === '"') inStr = !inStr
			if (c === '#' && !inStr) {
				while (i < text.length && text[i] !== '\n') i++
				out += '\n'
				continue
			}
			out += c
		}
		return out
	}

	/** 字符级 Stellaris 脚本解析：key = value / key = { … }，重复键（approach）收成数组
	 *  按行猜测会在大括号跨行的写法（section_weight、total_progress 等）上失准，这里一律走括号配对 */
	const parseScriptText = (text) => {
		const src = stripScriptComment(text)
		let i = 0
		// 键可以是 owner? / NOT / if 之类——除空白、=、花括号外的任意串
		const isKeyChar = (c) => !/[\s={}]/.test(c)

		const skipSpace = () => {
			while (i < src.length && /\s/.test(src[i])) i++
		}

		const parseValue = () => {
			const start = i
			let inStr = false
			while (i < src.length) {
				const c = src[i]
				if (c === '"') inStr = !inStr
				if (!inStr && (c === '\n' || c === '}')) break
				i++
			}
			return src.slice(start, i).trim()
		}

		const parseBlock = () => {
			const node = { values: {}, blocks: {} }
			for (;;) {
				skipSpace()
				if (i >= src.length) break
				if (src[i] === '}') {
					i++
					break
				}
				if (!isKeyChar(src[i])) {
					i++
					continue
				}
				const ks = i
				while (i < src.length && isKeyChar(src[i])) i++
				const key = src.slice(ks, i)
				skipSpace()
				if (src[i] !== '=') {
					// 不是赋值（如 `value < 0` 这类比较），把本行剩余当作值收下，别让 '<' 被当成键
					const rest = parseValue()
					if (rest) (node.values[key] ||= []).push(`${rest}`)
					continue
				}
				i++
				skipSpace()
				if (src[i] === '{') {
					i++
					const child = parseBlock()
					;(node.blocks[key] ||= []).push(child)
					continue
				}
				const val = parseValue()
				if (val !== '') (node.values[key] ||= []).push(val)
			}
			return node
		}

		return parseBlock()
	}

	/** 把文件头部的 @变量 代进整棵树（数值、字符串都替） */
	const substituteVars = (node, vars) => {
		for (const [k, list] of Object.entries(node.values)) {
			node.values[k] = list.map((v) => v.replace(/@([A-Za-z0-9_]+)/g, (m, n) => vars['@' + n] ?? m))
		}
		for (const list of Object.values(node.blocks)) for (const child of list) substituteVars(child, vars)
	}

	const SITUATION_CACHE = { at: 0, blocks: null, triggers: null }
	const loadSituationDefs = async () => {
		if (SITUATION_CACHE.blocks && Date.now() - SITUATION_CACHE.at < 60000) return SITUATION_CACHE
		const dir = await findGameDir()
		if (!dir) return { error: '找不到游戏目录（index.js 的 EDICT_GAME_DIRS / config.gameDir）' }
		const folder = path.join(dir, 'common', 'situations')
		const merged = {}
		let files = []
		try {
			files = readdirSync(folder).filter((f) => f.endsWith('.txt'))
		} catch (error) {
			return { error: `读不到 ${folder}: ${error.message}` }
		}
		for (const f of files) {
			let tree
			try {
				tree = parseScriptText(readFileSync(path.join(folder, f), 'utf8'))
			} catch {
				continue
			}
			const vars = {}
			for (const [k, v] of Object.entries(tree.values)) if (k.startsWith('@')) vars[k] = v[v.length - 1]
			substituteVars(tree, vars)
			for (const [k, list] of Object.entries(tree.blocks)) {
				if (!k.startsWith('situation')) continue
				;(merged[k] ||= []).push(...list)
			}
		}
		// 脚本化触发器（allow 里引用的 situation_*_cooldown_trigger 之类）单独收一份，用来展开条件
		const triggers = {}
		const tFolder = path.join(dir, 'common', 'scripted_triggers')
		try {
			for (const f of readdirSync(tFolder).filter((x) => x.endsWith('.txt'))) {
				const tree = parseScriptText(readFileSync(path.join(tFolder, f), 'utf8'))
				for (const [k, list] of Object.entries(tree.blocks)) triggers[k] = list[0]
			}
		} catch {}
		SITUATION_CACHE.blocks = merged
		SITUATION_CACHE.triggers = triggers
		SITUATION_CACHE.at = Date.now()
		return SITUATION_CACHE
	}

	/** 三值判定：按 potential 的树结构求值（AND / OR / NOT / NOR / NAND 都认）
	 *  之前把叶子全拉平再 AND，遇到 `NOT = { … }` 会把条件判反——旅客满意度那 12 条就是这么被误杀的 */
	const evalCondNode = (node, facts) => {
		if (!node) return { v: 'true', text: '' }
		const leaf = (k, val) => {
			const raw = String(val).trim()
			const have = facts[k]
			let v
			if (have === undefined || have === null) v = 'unknown'
			else if (typeof have === 'boolean') v = have === (raw === 'yes') ? 'true' : 'false'
			else v = String(have) === raw ? 'true' : 'false' // 字符串事实（如 current_situation_approach）按等值比
			return { v, text: `${k} = ${raw}` }
		}
		const join = (items, mode) => {
			let v = mode === 'or' ? 'false' : 'true'
			const texts = []
			for (const it of items) {
				v = mode === 'or' ? or3(v, it.v) : and3(v, it.v)
				if (it.text) texts.push(it.text)
			}
			return { v, text: texts.join(mode === 'or' ? ' | ' : ' & ') }
		}

		const parts = []
		for (const [k, list] of Object.entries(node.values ?? {})) parts.push(leaf(k, list[0]))
		for (const [k, list] of Object.entries(node.blocks ?? {})) {
			const kids = list.map((child) => evalCondNode(child, facts))
			if (k === 'OR' || k === 'NOR') {
				const j = join(kids, 'or')
				parts.push(k === 'NOR' ? { v: not3(j.v), text: `NOR(${j.text})` } : { v: j.v, text: `OR(${j.text})` })
			} else if (k === 'NOT') {
				const j = join(kids, 'and')
				parts.push({ v: not3(j.v), text: `NOT(${j.text})` })
			} else if (k === 'NAND') {
				const j = join(kids, 'and')
				parts.push({ v: not3(j.v), text: `NAND(${j.text})` })
			} else {
				// owner? / exists / if / limit 之类的包裹，语义上按 AND 汇总
				for (const kid of kids) parts.push(kid)
			}
		}
		return join(parts, 'and')
	}

	/** 把脚本化触发器展开成可读条件（$PARAM$ 用调用处的参数替换） */
	const renderCond = (node, params, depth = 0) => {
		if (!node || depth > 6) return ''
		const sub = (s) => String(s).replace(/\$([A-Za-z0-9_]+)\$/g, (m, n) => params[n] ?? m)
		const WRAPPERS = new Set(['custom_tooltip', 'target', 'owner', 'root', 'from', 'limit', 'trigger', 'this'])
		const parts = []
		for (const [k, list] of Object.entries(node.values ?? {})) {
			if (k === 'fail_text') continue
			parts.push(`${k} = ${sub(list[0])}`)
		}
		for (const [k, list] of Object.entries(node.blocks ?? {})) {
			const key = sub(k)
			for (const child of list) {
				const inner = renderCond(child, params, depth + 1)
				if (WRAPPERS.has(key)) {
					if (inner) parts.push(inner)
				} else if (key === 'NOT') {
					parts.push(`NOT(${inner})`)
				} else if (['OR', 'AND', 'NOR', 'NAND'].includes(key)) {
					parts.push(`${key}(${inner})`)
				} else {
					parts.push(inner ? `${key}（${inner}）` : key)
				}
			}
		}
		return parts.join(' & ')
	}

	const fmtModValue = (key, raw) => {
		const n = Number(raw)
		if (!Number.isFinite(n)) return String(raw)
		// 无量纲倍率（|v| ≤ 1）按百分比显示；整数型修正（如 pop_purge_speed = 2）保持原样
		const pct = Math.abs(n) <= 1
		const v = pct ? Math.round(n * 1000) / 10 : n
		return `${v >= 0 ? '+' : ''}${v}${pct ? '%' : ''}`
	}

	const modLabel = (key) => `MOD_${key.toUpperCase()}`
	const modLabelAlt = (key) => `mod_${key}`

	add({
		name: 'stellaris_situation_details',
		description:
			'One situation in full: the live stage / stage range / progress / monthly change / current approach (from the bridge), plus the definitions parsed out of common/situations/*.txt — every stage effect (modifier blocks, with triggered_modifier rows filtered by their potential three-valued: unknown ones are marked ?), each approach and its own modifiers, and the completion / failure tooltip. Definitions come from the game files, live numbers from memory; nothing is written.',
		parameters: {
			type: 'object',
			properties: {
				situation_id: { type: 'number', description: 'situation id from stellaris_situation_log' },
				raw: { type: 'boolean', description: 'also dump the parsed definition as JSON' },
				all_stages: { type: 'boolean', description: 'list the effects of every stage, not just the current one' },
				check: {
					type: 'boolean',
					description:
						'default true: ask the engine whether each approach can be selected right now (IsValid only, nothing is posted) and show its rejection reason',
				},
			},
			additionalProperties: false,
			required: ['situation_id'],
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const id = Number(args?.situation_id)
			if (!Number.isFinite(id)) return text('需要一个 situation_id（先用 stellaris_situation_log 看）')
			let log
			let gov
			try {
				log = await rpc(pipe, 'get_situation_log', { player_only: true })
				gov = await rpc(pipe, 'get_government').catch(() => null)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
			const live = (log?.situations ?? []).find((s) => s.id === id)
			if (!live) return text(`没有 id=${id} 的局势（stellaris_situation_log 里确认一下）`)

			const loaded = await loadSituationDefs()
			if (loaded?.error) return text(loaded.error)
			const blocks = loaded.blocks
			const scriptedTriggers = loaded.triggers ?? {}
			const def = blocks?.[live.key]?.[0]
			if (!def) return text(`脚本里找不到局势定义：${live.key}（可能是 mod 加的，或键名不同）`)

			const authority = gov?.authority
			const facts = {
				is_machine_empire: authority === 'auth_machine_intelligence',
				is_hive_empire: authority === 'auth_hive_mind',
				is_gestalt: authority === 'auth_machine_intelligence' || authority === 'auth_hive_mind',
				country_uses_bio_ships: null,
				// 这条我们能判：取向条件在脚本里写成 current_situation_approach = <键>
				current_situation_approach: live.current_approach ?? null,
			}

			// stages in declaration order（排除夹在 stages 里的非阶段子块）
			const NON_STAGE = new Set([
				'modifier',
				'triggered_modifier',
				'potential',
				'hidden_effect',
				'allow',
				'on_start',
				'on_select',
				'on_monthly',
				'on_progress_complete',
				'on_fail',
				'ai_weight',
			])
			const stageNodes = def.blocks?.stages?.[0]?.blocks ?? {}
			const stageNames = Object.keys(stageNodes).filter((n) => !NON_STAGE.has(n))
			const stageRows = stageNames.map((name) => {
				const node = stageNodes[name][0]
				const plain = []
				for (const m of node.blocks?.modifier ?? []) {
					for (const [k, list] of Object.entries(m.values)) plain.push([k, list[0]])
				}
				const triggered = []
				for (const tm of node.blocks?.triggered_modifier ?? []) {
					const cond = tm.blocks?.potential?.[0]
					const res = evalCondNode(cond, facts)
					const mods = []
					for (const [k, list] of Object.entries(tm.values)) {
						if (k === 'potential') continue
						mods.push([k, list[0]])
					}
					triggered.push({ verdict: res.v, mods, condition: res.text })
				}
				return { name, end: node.values?.end?.[0], plain, triggered }
			})

			// 当前阶段名：脚本里 current_stage = <阶段名> 这类条件据此可判
			facts.current_stage = stageRows[live.stage ?? 0]?.name ?? null

			const approaches = (def.blocks?.approach ?? []).map((node) => {
				const name = node.values?.name?.[0] ?? '?'
				const mods = []
				for (const m of node.blocks?.modifier ?? []) {
					for (const [k, list] of Object.entries(m.values)) mods.push([k, list[0]])
				}
				const allowNode = node.blocks?.allow?.[0]
				// allow 里常用脚本化触发器（situation_xxx_cooldown_trigger = { APPROACH = support }），
				// 名字本身就是条件，参数跟在后面；判不了，但应当原样告诉人
				const conds = []
				if (allowNode) {
					const WRAP = new Set([
						'if',
						'else',
						'else_if',
						'AND',
						'OR',
						'NOT',
						'limit',
						'trigger',
						'hidden_effect',
						'custom_tooltip',
					])
					const walkCond = (n) => {
						for (const [k, list] of Object.entries(n?.values ?? {})) {
							if (k === 'fail_text') continue
							conds.push(`${k} = ${list[0]}`)
						}
						for (const [k, list] of Object.entries(n?.blocks ?? {})) {
							for (const child of list) {
								if (WRAP.has(k)) {
									walkCond(child)
									continue
								}
								const params = Object.fromEntries(
									Object.entries(child.values ?? {}).map(([kk, vv]) => [kk, vv[0]]),
								)
								const trig = scriptedTriggers[k]
								if (trig) {
									conds.push(`${k} → ${renderCond(trig, params)}`)
									continue
								}
								const pstr = Object.entries(params).map(([kk, vv]) => `${kk} = ${vv}`)
								conds.push(pstr.length ? `${k}（${pstr.join('，')}）` : k)
							}
						}
					}
					walkCond(allowNode)
					for (let ci = conds.length - 1; ci >= 0; ci--) if (conds.indexOf(conds[ci]) !== ci) conds.splice(ci, 1)
				}
				const failTexts = []
				const walkFail = (n) => {
					if (!n) return
					for (const ft of n.values?.fail_text ?? []) failTexts.push(ft.replace(/^"|"$/g, ''))
					for (const list of Object.values(n.blocks ?? {})) for (const c of list) walkFail(c)
				}
				walkFail(allowNode)
				// resources = { cost = { unity = 100 } upkeep = { sr_zro = 3 } }
				const res = node.blocks?.resources?.[0]
				const fmtPairs = (n) =>
					Object.entries(n?.values ?? {})
						.map(([k, v]) => `${k} ${v[0]}`)
						.join('，')
				const cost = fmtPairs(res?.blocks?.cost?.[0])
				const upkeep = fmtPairs(res?.blocks?.upkeep?.[0])
				const allowRes = allowNode ? evalCondNode(allowNode, facts) : { v: 'true', text: '' }
				return {
					name,
					mods,
					hasAllow: !!allowNode,
					allowVerdict: allowRes.v,
					conds,
					cost,
					upkeep,
					failTexts,
				}
			})

			// 引擎校验：每个取向现在能不能选——只跑 IsValid、不投命令（玩家 01:45 提的路子）
			if (args?.check !== false) {
				for (const a of approaches) {
					// 当前取向不再问：它已经在生效，命令只是「再选一次」（要再付一次代价），
					// 标成不可用会让人误以为当前取向坏了
					if (a.name === live.current_approach) continue
					try {
						const v = await rpc(pipe, 'validate_situation_approach', { situation_id: id, approach_key: a.name })
						if (v && v.valid !== undefined) {
							a.engineValid = !!v.valid
							a.engineReason = String(v.reason ?? '').replace(/\s+/g, ' ').trim()
						}
					} catch {}
				}
			}

			const finisherKeys = []
			for (const wrapper of ['on_progress_complete', 'on_fail']) {
				const w = def.blocks?.[wrapper]?.[0]
				const raw = w?.values?.custom_tooltip?.[0]
				const ct = raw ? raw.replace(/^"|"$/g, '').trim() : undefined
				if (ct) finisherKeys.push([wrapper, ct])
			}

			// 完成 / 失败时真正会执行的东西：custom_tooltip 只是提示，块里还有事件与效果
			const COND_BLOCKS = new Set(['potential', 'limit', 'trigger', 'allow', 'ai_weight'])
			// 这些块只是容器，往里递归；其余块当成「一个效果 + 它的参数」整体打印，
			// 否则会出现 TECH = … / PROGRESS = … / type = … 这种不知道属于谁的行
			const CONTAINERS = new Set([
				'hidden_effect',
				'immediate',
				'effect',
				'if',
				'else_if',
				'else',
				'random',
				'random_list',
				'while',
				'switch',
				'custom_tooltip',
			])
			const collectFinish = (node, acc) => {
				if (!node) return acc
				// 标量值本身就是效果（set_country_flag = … / save_event_target_as = …），照原样收
				for (const [k, list] of Object.entries(node.values ?? {})) {
					if (COND_BLOCKS.has(k) || k === 'custom_tooltip' || k === 'fail_text') continue
					acc.push(`${k} = ${list[0]}`)
				}
				for (const [k, list] of Object.entries(node.blocks ?? {})) {
					if (COND_BLOCKS.has(k)) continue
					for (const child of list) {
						if (/_event$/.test(k)) {
							const id = child.values?.id?.[0]
							acc.push(id ? `事件 ${id}` : k)
							continue
						}
						// 有子块 → 这是作用域/容器（target / owner / every_owned_species / hidden_effect …），
						// 往里递归；只有标量参数 → 这才是「一个效果」，整体打印成 key（参数…）
						if (Object.keys(child.blocks ?? {}).length > 0) {
							collectFinish(child, acc)
							continue
						}
						const params = Object.entries(child.values ?? {}).map(([kk, vv]) => `${kk} = ${vv[0]}`)
						acc.push(params.length ? `${k}（${params.join('，')}）` : k)
					}
				}
				return acc
			}
			const finishEffects = {}
			for (const wrapper of ['on_progress_complete', 'on_fail']) {
				const w = def.blocks?.[wrapper]?.[0]
				const acc = collectFinish(w, [])
				if (acc.length) finishEffects[wrapper] = [...new Set(acc)]
			}

			// localise everything we are about to print
			const keys = new Set()
			for (const row of stageRows) {
				for (const [k] of row.plain) {
					keys.add(modLabel(k))
					keys.add(modLabelAlt(k))
				}
				for (const t of row.triggered) {
					for (const [k] of t.mods) {
						keys.add(modLabel(k))
						keys.add(modLabelAlt(k))
					}
				}
			}
			for (const a of approaches) {
				keys.add(a.name)
				for (const [k] of a.mods) {
					keys.add(modLabel(k))
					keys.add(modLabelAlt(k))
				}
			}
			for (const [, ct] of finisherKeys) keys.add(ct)
			let names = {}
			try {
				const loc = await rpc(pipe, 'localize', { keys: [...keys] })
				names = loc?.names ?? loc?.localized ?? {}
			} catch {}

			// 本地化串里还有两类游戏内标记：£energy£ 这类图标、§Y…§! 这类颜色，
			// 以及 [calculator_physicist.GetNamePlural] 这种运行时解析的引用（用 job_<x>_plural 补）
			const brackets = new Set()
			for (const v of Object.values(names)) {
				for (const m of String(v).matchAll(/\[([A-Za-z0-9_]+)\.GetNamePlural\]/g)) brackets.add(m[1])
			}
			if (brackets.size) {
				const extra = [...brackets].map((x) => `job_${x}_plural`)
				try {
					const loc2 = await rpc(pipe, 'localize', { keys: extra })
					Object.assign(names, loc2?.names ?? loc2?.localized ?? {})
				} catch {}
			}

			const pretty = (s) =>
				String(s)
					.replace(/\[([A-Za-z0-9_]+)\.GetNamePlural\]/g, (m, x) => {
						const t = names[`job_${x}_plural`]
						return t && t !== `job_${x}_plural` ? t : m
					})
					.replace(/£[A-Za-z0-9_]+£/g, '')
					.replace(/§[A-Za-z!]/g, '')
					// 图标被桥换成资源名后会重复一次：「建筑能量币 能量币维护费」→「建筑能量币维护费」
					.replace(/([\u4e00-\u9fa5]{2,4})\s+\1/g, '$1')
					.replace(/\s{2,}/g, ' ')
					.trim()

			const label = (key) => {
				const n = names[key]
				if (!n || n === key) return null
				const text = pretty(n)
				// 有些本地化串整句就是一个运行时访问器，例如 [GetPassengerSatisfactionMutinyOutcome]
				const acc = text.match(/^\[([A-Za-z0-9_]+)\]$/)
				if (acc) return `（运行时生成：${acc[1]}）`
				return text
			}
			const modName = (k) => label(modLabel(k)) || label(modLabelAlt(k))

			const fmtRow = ([k, v]) => {
				const nice = modName(k)
				return `  ${nice ? `${nice}: ` : ''}${fmtModValue(k, v)}${nice ? '' : `   [${k}]`}`
			}

			const curIdx = live.stage ?? 0
			const lines = []
			lines.push(`${live.name || live.key}（${live.key}）  id ${live.id}`)
			lines.push(
				`  阶段 ${curIdx}（${live.stage_min ?? '?'}–${live.stage_max ?? '?'}）｜进度 ${live.progress ?? '?'}｜每月 ${live.monthly_change >= 0 ? '+' : ''}${live.monthly_change}｜取向 ${live.current_approach ?? '—'}`,
			)

			const wanted = args?.all_stages === true ? stageRows : stageRows.slice(curIdx, curIdx + 1)
			for (const row of wanted) {
				const shown = row.triggered.filter((t) => t.verdict !== 'false')
				const fixed = row.plain.length
				const cond = shown.filter((t) => t.verdict === 'unknown').reduce((n, t) => n + t.mods.length, 0)
				const total = fixed + shown.reduce((n, t) => n + t.mods.length, 0)
				// end 缺失或不是数字（脚本变量如 @ShroudForgedAcceptanceThreshold）时，
				// 当前阶段回落到桥给的实时区间，其它阶段显示 ?
				const endOk = row.end && /^-?\d+(\.\d+)?$/.test(String(row.end).trim())
				const upper =
					(endOk ? row.end : null) ??
					(row.name === stageRows[curIdx]?.name ? `${live.stage_min ?? '?'}–${live.stage_max ?? '?'}` : '?')
				lines.push(
					`阶段 ${row.name}（到 ${upper}）：效果 ${total} 条` +
						(cond ? `（其中 ${cond} 条附带条件，未判定；游戏界面按条件取舍）` : ''),
				)
				for (const p of row.plain) lines.push(fmtRow(p))
				for (const t of shown) {
					// 条件判不了时，条件只印一次，下面的条目缩进——比每条都挂一长串条件好读
					const unknown = t.verdict === 'unknown'
					if (unknown) lines.push(`  ? 条件：${t.condition}`)
					for (const m of t.mods) lines.push(unknown ? `    ${fmtRow(m).trimStart()}` : fmtRow(m))
				}
			}

			lines.push('取向：')
			for (const a of approaches) {
				const active = a.name === live.current_approach ? '（当前）' : ''
				const nice = label(a.name)
				// 优先用引擎 IsValid 的结论；没问到时退回脚本条件的三值判定。
				// 当前取向一律不加「可用/不可用」——它已经在生效
				const avail =
					active
						? ''
						: a.engineValid === true
						? '（可选）'
						: a.engineValid === false
							? '（不可用）'
							: !a.hasAllow
								? ''
								: a.allowVerdict === 'false'
									? '（不可用）'
									: a.allowVerdict === 'unknown'
										? '（可用性未判定）'
										: ''
				const head = `  ${nice || a.name}${active}${avail}`
				if (!a.mods.length) {
					lines.push(`${head} —— 无效果`)
				} else {
					lines.push(head)
					for (const m of a.mods) lines.push(fmtRow(m))
				}
				if (a.engineValid === false && a.engineReason) {
					lines.push(`      引擎说：${a.engineReason}`)
				}
				if (a.hasAllow && a.allowVerdict !== 'true') {
					if (a.conds.length) lines.push(`      条件：${a.conds.join('；')}`)
					for (const ft of a.failTexts) lines.push(`      需要：${label(ft) || ft}`)
				}
				if (a.cost || a.upkeep) {
					const bits = []
					if (a.cost) bits.push(`启动 ${a.cost}`)
					if (a.upkeep) bits.push(`维护 ${a.upkeep}`)
					lines.push(`      代价：${bits.join('｜')}`)
				}
			}

			const done = finisherKeys.find(([w]) => w === 'on_progress_complete')
			const fail = finisherKeys.find(([w]) => w === 'on_fail')
			if (done && fail && done[1] === fail[1]) {
				lines.push(`完成/失败效果：${label(done[1]) || done[1]}`)
			} else {
				if (done) lines.push(`完成效果：${label(done[1]) || done[1]}`)
				if (fail) lines.push(`失败效果：${label(fail[1]) || fail[1]}`)
			}
			// custom_tooltip 只说了「会怎样」，这里补上「具体执行什么」
			for (const [wrapper, tag] of [
				['on_progress_complete', '完成时'],
				['on_fail', '失败时'],
			]) {
				const acc = finishEffects[wrapper]
				if (!acc || !acc.length) continue
				lines.push(`${tag}会触发：`)
				for (const item of acc) lines.push(`  · ${item}`)
			}

			if (args?.raw === true) {
				lines.push('', JSON.stringify({ live, stages: stageRows, approaches, finisherKeys }, null, 1))
			}
			return text(lines.join('\n'))
		},
	})
	/* —— 外交看板（2026-11-20）——
	   以「国家」为主体，而不是以特使为主体：谁认识、签了什么条约、我们在他那儿有没有特使、在做什么。
	   数据是 get_contacts × get_leaders 的 join：特使的 location.slot == 'empire' 时 target_id 就是对方国家。 */
	const ENVOY_SLOT_CN = { empire: '帝国', galactic_community: '共同体', federation: '联邦', first_contact: '首触' }
	// 使者的任务键 → 中文名，走游戏本地化（improve_relations → 改善关系、patron_attunement → 与虚境实体调和…）
	let ENVOY_TASK_NAMES = null
	const envoyTaskNames = async (pipe) => {
		if (ENVOY_TASK_NAMES) return ENVOY_TASK_NAMES
		const keys = [
			'improve_relations', 'harm_relations', 'spy_network', 'galactic_community',
			'patron_attunement', 'strengthen_imperial_authority', 'undermine_imperial_authority',
		]
		const map = new Map()
		try {
			const r = await rpc(pipe, 'localize', { keys })
			for (const [k, v] of Object.entries(r?.names ?? {})) map.set(k, v)
		} catch {}
		ENVOY_TASK_NAMES = map
		return map
	}
	const taskLabel = (names, task) => (task ? `${names.get(String(task)) ?? task}（${task}）` : '?')
	const CONTACT_TYPE_CN = {
		default: '普通',
		fallen_empire: '失落帝国',
		awakened_fallen_empire: '觉醒帝国',
		enclave: '城邦',
		mindwarden_enclave: '城邦',
		dormant_marauders: '掠夺者',
		primitive: '前超光速',
		faction: '派系',
	}
	add({
		name: 'stellaris_contacts',
		description:
			'Diplomacy board: who we know, what treaties we have, and where OUR envoys stand — one row per country. ' +
			'Bridge side is get_contacts (country_id, country_type, name, treaties) joined with get_leaders, so a country ' +
			'whose envoy sits there shows the envoy name and task (location.slot "empire", target_id = that country). ' +
			'A summary counts the envoys: how many are posted to which kind of slot and how many are idle — envoy slots are ' +
			'independent of the leader system (they cannot be hired or dismissed; the count comes from empire modifiers). ' +
			'Measured 2026-11-20: this save has 50 contacts, envoys on 2 (improve_relations), 1 (improve_relations) and ' +
			'6 (spy_network). Default lists only empire-type contacts; all:true adds enclaves, marauders, primitives, ' +
			'guardians and factions. ' +
			'NEW 2026-11-21: each row carries the contract — "◀ X 的附庸（忠诚、起始日）" for a subject, ' +
			'"▲ A、B 的宗主" for an overlord; the bridge reads CCountry+0x2630 (overlord ref, 0xFFFFFFFF = none), ' +
			'+0x2634 (subject_date, packed date) and +0x2640 (loyalty, u64 ×1e5). The subject TYPE ' +
			'(保护国/学邦/采邑) lives in the agreement, not in the country struct, so it is omitted until the ' +
			'agreement hook is in place.',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: 'also list non-empire contacts (enclaves, marauders, primitives, guardians, factions)' },
				envoy_only: { type: 'boolean', description: 'only countries that currently have one of our envoys' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const contacts = await rpc(pipe, 'get_contacts', {})
				if (contacts?.error) return text(`bridge: ${contacts.error}`)
				const leaders = await rpc(pipe, 'get_leaders', {})
				const hired = Array.isArray(leaders?.hired_leaders) ? leaders.hired_leaders : []

				// 特使位置 → 按「槽类型 + target」索引
				const envoys = hired.filter((l) => String(l?.class ?? '') === 'envoy')
				const taskNames = await envoyTaskNames(pipe)
				const bySlot = (slot) => envoys.filter((l) => String(l?.location?.slot ?? '') === slot)
				const empireEnvoy = new Map()
				for (const e of bySlot('empire')) empireEnvoy.set(Number(e.location.target_id), e)

				const rows = Array.isArray(contacts?.empires) ? contacts.empires : []
				// 协议（hook 抓到的 CAgreement）：只用来给「附庸」补类型——保护国 / 学邦 / 戍邦…
				// 没抓到就退回泛称「附庸」，不报错。键是 owner->target 的国家 id。
				const agreementByPair = new Map()
				let agreementCount = null
				try {
					const agr = await rpc(pipe, 'get_agreements', {})
					if (agr?.captured) {
						const list = Array.isArray(agr.agreements) ? agr.agreements : []
						agreementCount = list.length
						for (const a of list) {
							agreementByPair.set(`${Number(a.owner_id)}->${Number(a.target_id)}`, {
								preset: String(a.agreement_preset || ''),
								subsidies: a.subsidies ?? null,
							})
						}
					}
				} catch {}
				const presetNames = new Map()
				const presetKeys = [...new Set([...agreementByPair.values()].map((a) => a.preset).filter(Boolean))]
				if (presetKeys.length) {
					try {
						const r = await rpc(pipe, 'localize', { keys: presetKeys })
						for (const [k, v] of Object.entries(r?.names ?? {})) presetNames.set(k, v)
					} catch {}
				}
				const subjectType = (ownerId, subjectId) => {
					const key = agreementByPair.get(`${Number(ownerId)}->${Number(subjectId)}`)?.preset
					if (!key) return '附庸'
					return presetNames.get(key) ?? key.replace(/^preset_/, '')
				}
				// 补贴：从协议的资源条款读（基础 / 进阶 / 研究 / 战略 / 凝聚力），只列非零的
				const subsidyText = (ownerId, subjectId) => {
					const sub = agreementByPair.get(`${Number(ownerId)}->${Number(subjectId)}`)?.subsidies
					if (!sub) return ''
					const parts = [
						['基础', sub.basic], ['进阶', sub.advanced], ['研究', sub.research],
						['战略', sub.strategic], ['凝聚力', sub.unity],
					]
						.filter(([, v]) => Number(v) > 0)
						.map(([label, v]) => `${label} ${Math.round(Number(v) * 100)}%`)
					return parts.length ? `补贴 ${parts.join(' / ')}` : ''
				}
				// 契约（附庸/宗主）：桥 get_contacts 的 overlord_id / loyalty / subject_date 给的是「当我们是附庸时」那一侧，
				// 宗主那一侧只能从别的国家的 overlord 字段反推——所以两边都从这里拼。
				const subjectsOf = new Map()
				for (const r of rows) {
					const ov = Number(r?.overlord_id ?? -1)
					if (r?.is_subject !== true || ov < 0) continue
					if (!subjectsOf.has(ov)) subjectsOf.set(ov, [])
					subjectsOf.get(ov).push(r)
				}
				const contractOf = (r) => {
					const parts = []
					if (r?.is_subject === true) {
						const ov = r.overlord_name || `country ${r.overlord_id}`
						const type = subjectType(r.overlord_id, r.country_id)
						const extra = [
							Number.isFinite(Number(r.loyalty)) ? `忠诚 ${r.loyalty}` : '',
							r.subject_date ? `自 ${r.subject_date}` : '',
							subsidyText(r.overlord_id, r.country_id),
						].filter(Boolean).join('，')
						parts.push(`◀ ${ov} 的${type}${extra ? `（${extra}）` : ''}`)
					}
					const subs = subjectsOf.get(Number(r.country_id)) ?? []
					if (subs.length) {
						parts.push(
							`▲ ${subs.map((s) => `${s.name ?? `country ${s.country_id}`}（${subjectType(r.country_id, s.country_id)}）`).join('、')} 的宗主`,
						)
					}
					return parts.join('｜')
				}
				const isEmpire = (r) => ['default', 'awakened_fallen_empire'].includes(String(r.country_type))
				let list = rows.filter((r) => Number(r.country_id) !== 0)
				if (args?.all !== true) list = list.filter(isEmpire)
				if (args?.envoy_only === true) list = list.filter((r) => empireEnvoy.has(Number(r.country_id)))

				const posted = envoys.filter((e) => String(e?.location?.slot ?? '') !== 'unassigned')
				const idle = envoys.length - posted.length
				const slotCount = {}
				for (const e of posted) {
					const s = String(e.location.slot)
					slotCount[s] = (slotCount[s] ?? 0) + 1
				}
				const slotText = Object.entries(slotCount).map(([s, n]) => `${ENVOY_SLOT_CN[s] ?? s} ${n}`).join(' / ')

				const lines = [
					`特使 ${envoys.length} 名：已派 ${posted.length}${slotText ? `（${slotText}）` : ''} · 空闲 ${idle}`,
					'（使节位由帝国修正决定，不能雇、不能解雇；位置见 stellaris_leader_assignments）',
					'',
				]
				const selfInfo = contacts?.self
				const ourSubjects = subjectsOf.get(Number(selfInfo?.country_id ?? 0)) ?? []
				const totalPairs = rows.filter((r) => r?.is_subject === true).length + (selfInfo?.is_subject === true ? 1 : 0)
				const ourText = ourSubjects.length
					? ourSubjects.map((s) => `${s.name}（${subjectType(selfInfo?.country_id ?? 0, s.country_id)}）`).join('、')
					: selfInfo?.is_subject === true
						? `${selfInfo.overlord_name || `country ${selfInfo.overlord_id}`} 的${subjectType(selfInfo.overlord_id, selfInfo.country_id)}`
						: '没有附庸、也没有宗主'
				lines.push(
					`附庸-宗主 ${totalPairs} 组 · 我们：${ourText}`,
					agreementCount == null
						? '（类型未解：桥没抓到协议——先 install_agreement_capture，再存一次档；抓到前一律写「附庸」）'
						: `（类型来自协议：抓到 ${agreementCount} 条 CAgreement）`,
					`${args?.all === true ? '全部联系对象' : '普通帝国'} ${list.length} 个`,
				)
				const sortKey = (r) => (empireEnvoy.has(Number(r.country_id)) ? 0 : 1) * 1e6 + Number(r.country_id)
				list.sort((a, b) => sortKey(a) - sortKey(b))
				for (const r of list) {
					const t = r.treaties ?? {}
					const pacts = [
						t.commercial_pact ? '商贸' : '',
						t.migration_pact ? '移民' : '',
						t.research_agreement ? '研究' : '',
					].filter(Boolean).join('/')
					const e = empireEnvoy.get(Number(r.country_id))
					const envoyText = e ? `${e.name ?? e.id}｜${taskLabel(taskNames, e.location.task)}` : '—'
					const contract = contractOf(r)
					lines.push(
						`  ${String(r.country_id).padStart(3)}  ${(r.name ?? '?').padEnd(12)}` +
							`  ${CONTACT_TYPE_CN[r.country_type] ?? r.country_type ?? '?'}` +
							`  ${pacts || '—'}` +
							`  特使 ${envoyText}` +
							(contract ? `  ｜ ${contract}` : ''),
					)
				}
				lines.push('', '派/挪特使：stellaris_assign_leader { leader_id, slot: "envoy", target_id: <国家 id>, task }')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	// 协议条款里引擎本地化缺项 / 错项的几个键（2026-11-21 实测：protectorate、has_access、
	// subject_loyalty、naval_capacity 都没有像样的中文，naval_capacity 甚至被译成「附属」）
	// 游戏里 term_data 有 hidden = yes 的条款：界面不显示，但存档与内存里都有（2026-11-21 实测：
	// protectorate / subject_loyalty / has_access / subject_xenophobe / subject_pompous / naval_capacity）
	const AGREEMENT_HIDDEN = { keys: null }
	const hiddenTermKeys = async () => {
		if (AGREEMENT_HIDDEN.keys) return AGREEMENT_HIDDEN.keys
		const keys = new Set()
		try {
			const fs = await import('node:fs')
			const dir = EDICT_GAME_DIRS.map((d) => `${d}\\common\\agreement_terms`).find((d) => fs.existsSync(d))
			if (dir) {
				for (const name of fs.readdirSync(dir)) {
					if (!name.endsWith('.txt')) continue
					const text = fs.readFileSync(`${dir}\\${name}`, 'utf8')
					const re = /^\s*([a-z_0-9]+)\s*=\s*\{/gm
					let m
					while ((m = re.exec(text))) {
						const open = text.indexOf('{', m.index)
						const body = braceBlock(text, open)
						if (body && /^\s*hidden\s*=\s*yes/m.test(body)) keys.add(m[1])
					}
				}
			}
		} catch {}
		AGREEMENT_HIDDEN.keys = keys
		return keys
	}

	const AGREEMENT_TERM_CN = {
		protectorate: '保护国',
		subject_is_protectorate: '是保护国',
		subject_is_not_protectorate: '不是保护国',
		has_access: '通行权',
		yes_access: '有通行权',
		subject_loyalty: '忠诚度',
		subject_loyalty_effects: '受忠诚度影响',
		subject_pompous: '傲慢',
		subject_is_pompous: '是傲慢的',
		subject_xenophobe: '排外主义',
		subject_is_xenophobe: '是排外主义',
		naval_capacity: '海军容量',
		naval_cap_unmodified: '不修正',
	}

	/* —— 协议明细（桥 get_agreements，2026-11-21 接）——
	   前提：先 install_agreement_capture，再存一次档（读档也会走序列化）。 */
	add({
		name: 'stellaris_agreements',
		description:
			'Subject agreements in full (bridge get_agreements): who is whose subject, the agreement preset ' +
			'(preset_protectorate / preset_vassal / preset_vassal_oppressive_ai …), dates, forced initial loyalty, ' +
			'resource subsidies (basic / research / advanced = consumer goods + alloys / strategic / unity) and every ' +
			'discrete term (integration, diplomacy, expansion, war joining, sensors, holdings limit …). ' +
			'Needs the agreement hook: stellaris_state { method: "install_agreement_capture" }, then one save_game ' +
			'(loading a save also serialises every agreement). Measured 2026-11-21: terms arrive as keys such as ' +
			'joins_overlord_wars_offensive and the game localises them (进攻); an empty answer means the hook has not ' +
			'seen a save yet.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'only agreements where this country is the overlord or the subject' },
				hidden: { type: 'boolean', description: 'also list hidden terms (hidden = yes in the game files; the diplomacy UI never shows them)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const agr = await rpc(pipe, 'get_agreements', {})
				if (agr?.error) return text(`bridge: ${agr.error}`)
				if (agr?.captured !== true) {
					return text(
						`桥还没抓到协议${agr?.hint ? `（${agr.hint}）` : ''}\n` +
							'抓法：stellaris_state { method: "install_agreement_capture" }，再 save_game 一次（读档也会走序列化）',
					)
				}
				const list = Array.isArray(agr.agreements) ? agr.agreements : []
				const names = new Map()
				try {
					const c = await rpc(pipe, 'get_contacts', {})
					for (const r of c?.empires ?? []) {
						names.set(Number(r.country_id), r.name)
						// 宗主国不一定在我们的通讯录里（可能没有联络），但附庸那一行带着它的名字
						if (r?.is_subject === true && Number(r.overlord_id) >= 0 && r.overlord_name) {
							names.set(Number(r.overlord_id), r.overlord_name)
						}
					}
					if (c?.self) {
						names.set(Number(c.self.country_id), c.self.name)
						if (c.self.overlord_name) names.set(Number(c.self.overlord_id), c.self.overlord_name)
					}
				} catch {}
				const nameOf = (id) => names.get(Number(id)) ?? `country ${id}`
				let rows = list
				if (Number.isFinite(Number(args?.country_id))) {
					const cid = Number(args.country_id)
					rows = rows.filter((a) => Number(a.owner_id) === cid || Number(a.target_id) === cid)
				}
				if (!rows.length) return text('没有匹配的协议')
				// 预设、条款键与值一次性翻成中文（引擎本地化；没有的就显示原键）
				const locKeys = new Set()
				for (const a of rows) {
					if (a.agreement_preset) locKeys.add(String(a.agreement_preset))
					for (const t of a.terms ?? []) {
						locKeys.add(String(t.key))
						if (t.value) locKeys.add(String(t.value))
					}
				}
				const cn = new Map()
				if (locKeys.size) {
					try {
						const r = await rpc(pipe, 'localize', { keys: [...locKeys] })
						for (const [k, v] of Object.entries(r?.names ?? {})) cn.set(k, v)
					} catch {}
				}
				const label = (k) => AGREEMENT_TERM_CN[String(k)] ?? cn.get(String(k)) ?? String(k)
				const pct = (v) => `${Math.round(Number(v) * 1000) / 10}%`
				const lines = [`协议 ${rows.length} 条（hook 已抓到）`]
				for (const a of rows) {
					lines.push('', `#${a.owner_id} ${nameOf(a.owner_id)} → #${a.target_id} ${nameOf(a.target_id)}`)
					const forced = Number(a.forced_initial_loyalty ?? 0)
					lines.push(
						`   ${label(a.agreement_preset) || '（未知预设）'} ｜ 自 ${a.date_added || '?'}` +
							(forced ? ` ｜ 固有忠诚 ${forced}` : ''),
					)
					const sub = a.subsidies ?? {}
					const subParts = [
						['基础', sub.basic],
						['进阶', sub.advanced],
						['研究', sub.research],
						['战略', sub.strategic],
						['凝聚力', sub.unity],
					]
						.filter(([, v]) => Number(v) > 0)
						.map(([n, v]) => `${n} ${pct(v)}`)
					if (subParts.length) lines.push(`   补贴：${subParts.join(' / ')}`)
					const hiddenSet = await hiddenTermKeys()
					let hiddenCount = 0
					for (const t of a.terms ?? []) {
						const isHidden = hiddenSet.has(String(t.key))
						if (isHidden) hiddenCount += 1
						if (isHidden && args?.hidden !== true) continue
						lines.push(
							`   ${label(t.key)}：${t.value ? label(t.value) : '—'}${isHidden ? '（隐藏）' : ''}`,
						)
					}
					if (hiddenCount && args?.hidden !== true) {
						lines.push(`   （另有 ${hiddenCount} 条隐藏条款，界面不显示；加 hidden: true 看）`)
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_player_country',
		description:
			'The player\'s own country: id, raw reference and display name (bridge get_player_country, which runs the same ' +
			'CountryName() path as the site/system owner_name fields). Useful because get_contacts never lists you and a ' +
			'nomad owns no system, so get_system cannot answer it either. Measured 2026-11-20: this save returns ' +
			'{country_id 0, name 方舟魔法少女} — country 1 is a different empire (灵能魔女).',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'get_player_country', {})
				if (r?.error) return text(`bridge: ${r.error}`)
				return text(`玩家国家：${r?.name || '（读不到名字）'}｜country_id ${r?.country_id}｜raw_ref ${r?.raw_ref}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 外交动作（桥 get_diplomatic_actions，2026-11-20 接）——
	   注意：桥要的是「带标记的国家引用」（0x01000000|id），传裸 id 会回 No empire with country id N。
	   这里自带补标记，并把每个动作的 IsValid 结论摆出来；宣战与贸易协定单列一段说明它们缺什么。 */
	let DIPLO_NAMES = null
	// 这些 action 键本身没有本地化（localize 原样返回键），所以写一张小对照表；表外的原样显示
	const DIPLO_CN = {
		action_improve_relation: '改善关系',
		action_harm_relation: '损害关系',
		action_build_spy_network: '建立间谍网',
		action_declare_war: '宣战',
		action_make_claims_diplomacy_view: '宣称星系',
		action_offer_trade_deal: '提出贸易协定',
		action_make_rival: '设为宿敌',
		action_form_commercial_pact: '商业条约',
		action_form_waystation_pact: '驿站条约',
		action_form_research_agreement: '研究协定',
		action_form_migration_pact: '移民条约',
		action_offer_federation_association_status: '提出联邦联系',
		action_invite_to_federation: '邀请加入联邦',
		action_demand_subjugation: '要求臣服',
		action_ask_to_become_subjugated: '请求成为附庸',
		action_insult: '侮辱',
		action_recall_embassy: '召回大使',
		action_offer_peace_diplomacy_view: '提出和平',
		action_support_independence: '支持独立',
		action_embassy: '建立大使馆',
		action_propose_secret_fealty: '提议秘密效忠',
		// 2026-11-21 补：联邦与附庸这一批原来只显示原始键
		action_kick_from_federation: '踢出联邦成员',
		action_leave_federation: '退出联邦',
		action_ask_to_join_federation: '申请加入联邦',
		action_ask_to_leave_federation: '请求离开联邦',
		action_ask_for_federation_association_status: '请求联邦联系',
		action_end_federation_association_status: '结束联邦联系',
		action_vote_for_federation_law: '联邦法律表决',
		action_vote_for_federation_type: '联邦类型表决',
		action_negotiate_existing_agreement: '商谈现有协议',
		action_release_subject: '释放附庸',
		action_ask_to_be_released_subject: '请求解除附庸',
		action_integrate_subject: '整合附庸',
		action_cancel_subject_integration: '取消整合',
		action_break_waystation_pact: '终止驿站条约',
	}
	const diploNames = async () => DIPLO_NAMES
	const rawCountryRef = (id) => (Number(id) >= 0x01000000 ? Number(id) : 0x01000000 + Number(id))
	add({
		name: 'stellaris_diplomatic_actions',
		description:
			'What we can propose to ONE empire right now (bridge get_diplomatic_actions, which builds every ' +
			'CDiplomaticActionType and asks the engine\'s propose command IsValid for each — nothing is posted). ' +
			'IMPORTANT: the bridge wants the tagged country reference (0x01000000|id); a bare id answers ' +
			'"No empire with country id N", so this tool tags it for you. Rows are grouped 可发起 / 被拒 with the engine\'s ' +
			'own reason. Two actions get a callout: action_declare_war needs a war goal chosen first (not wired yet), and ' +
			'action_offer_trade_deal needs items (an empty trade is refused). Measured 2026-11-20 toward 诺力佳统治集团 ' +
			'(16777222): 8 of 17 proposable — improve_relation / harm_relation / build_spy_network / commercial_pact / ' +
			'waystation_pact / research_agreement / federation_association / recall_embassy.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'target country id (bare id is fine; the tag is added for you)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			required: ['country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.country_id)
			if (!Number.isFinite(id)) return text('country_id is required（stellaris_contacts 里的国家 id）')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'get_diplomatic_actions', { country_id: rawCountryRef(id) })
				if (args?.raw === true) return text(JSON.stringify(r))
				if (r?.error) return text(`bridge: ${r.error}`)
				const actions = Array.isArray(r?.actions) ? r.actions : []
				if (!actions.length) return text('没有动作（对方可能没有通讯）')
				const label = (key) => `${DIPLO_CN[String(key)] ?? key}（${key}）`
				// 宣战与贸易协定单独成段（玩家 2026-11-20 定）：它们被拒的理由是“缺参数”，不是“不能做”，
				// 混在被拒里会被误读成“打不了 / 谈不了”。
				const CALLOUT = new Set(['action_declare_war', 'action_offer_trade_deal'])
				// 1) `*_diplomacy_view` 是「打开某个界面」的占位动作（common/diplomatic_actions 里只有 is_listed = yes），
				//    投它不会真的发出任何提议——2026-11-20 拿求和试过，引擎把状态写成“考虑中”，实际什么也没发生。
				// 2) 这些动作在脚本里带 requires_alliance_vote = yes：真流程要先进联邦投票，桥直投 propose 会跳过投票。
				const VIEW = (key) => /_diplomacy_view$/.test(String(key))
				const VOTE = new Set([
					'action_declare_war', 'action_offer_federation_association_status', 'action_revoke_federation_association_status',
					'action_invite_to_federation', 'action_vote_for_federation_law', 'action_vote_for_federation_type',
					'action_kick_from_federation',
				])
				const real = actions.filter((a) => !VIEW(a.action))
				const views = actions.filter((a) => VIEW(a.action))
				const rest = real.filter((a) => !CALLOUT.has(String(a.action)))
				const ok = rest.filter((a) => a.can_propose)
				const no = rest.filter((a) => !a.can_propose)
				const okTotal = real.filter((a) => a.can_propose).length
				const lines = [
					`对 country ${id}（raw ${rawCountryRef(id)}）：可发起 ${okTotal} / 共 ${real.length}` +
						(views.length ? `（另有 ${views.length} 个界面占位动作，不算）` : ''),
					'',
					'【可发起】',
				]
				for (const a of ok) lines.push(`  ✓ ${label(a.action)}${VOTE.has(String(a.action)) ? '  ← 需要先过联邦投票' : ''}`)
				if (no.length) {
					lines.push('', '【被拒】')
					for (const a of no) lines.push(`  ✗ ${label(a.action)}  ${String(a.reason ?? '').replace(/\n/g, ' / ')}`)
				}
				const war = actions.find((a) => String(a.action) === 'action_declare_war')
				const trade = actions.find((a) => String(a.action) === 'action_offer_trade_deal')
				// 玩家 2026-11-20 定：宣战与贸易协定不进「被拒」列，而是按原因分三类——
				//   可发：引擎放行
				//   待议：缺的是“内容”（没战争目标 / 空交易）——凑齐就能发
				//   不可用：条件不满足（停战冷却、已交战等）——凑也发不了
				const whyOf = (a) => String(a.reason ?? '').replace(/\n/g, ' / ').trim()
				// 先判“不可用”，再判“待议”：宣战在停战冷却里时，理由会写两条（停战协议 + 尚未设置战争目标），
				// 只看后者会误判成待议，所以停战/交战这类硬条件优先。
				const classify = (a, pendingPat) => {
					if (!a) return { tag: '—', text: '这张表里没有' }
					if (a.can_propose) return { tag: '可发', text: '现在就可以' }
					const why = whyOf(a)
					if (/停战|交战|无法宣战/.test(why)) return { tag: '不可用', text: why }
					if (pendingPat.test(why)) return { tag: '待议', text: why }
					return { tag: '不可用', text: why }
				}
				const warState = classify(war, /尚未设置战争目标/)
				const tradeState = classify(trade, /交易不能为空/)
				lines.push('', '【宣战 / 贸易协定（单独两类）】')
				lines.push(`  宣战      ${warState.tag}：${warState.text}`)
				lines.push(`  贸易协定  ${tradeState.tag}：${tradeState.text}`)
				const notes = []
				if (warState.tag === '待议') notes.push('宣战要先设战争目标（界面上那一步还没接线），而且 script 里它带 requires_alliance_vote，真流程还有一道联邦投票')
				if (warState.tag === '不可用' && /停战/.test(warState.text)) notes.push('这是停战协议在冷却，不是“没准备好”')
				if (tradeState.tag === '待议') notes.push('贸易协定要带内容——空交易会被引擎直接拒')
				if (notes.length) lines.push(`  （${notes.join('；')}）`)
				if (views.length) {
					lines.push('', '【界面占位动作——投了也不会真发】')
					for (const a of views) lines.push(`  ⊘ ${label(a.action)}  ${String(a.reason ?? '').replace(/\n/g, ' / ')}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 发起外交动作（桥 propose_diplomatic_action，2026-11-20 接）——
	   真投递：桥走 CDiploActionProposeCommand::Post（先 IsValid、再投），拒绝原因原样带回。
	   action 取 stellaris_diplomatic_actions 里的键（如 action_offer_peace_diplomacy_view）。 */
	add({
		name: 'stellaris_propose_diplomatic_action',
		description:
			'Propose ONE diplomatic action at a country (bridge propose_diplomatic_action → CDiploActionProposeCommand; ' +
			'the command runs IsValid and then posts, and its own refusal reason is returned verbatim). action is a key from ' +
			'stellaris_diplomatic_actions (e.g. action_offer_peace_diplomacy_view, action_form_research_agreement, ' +
			'action_improve_relation). The bridge wants the tagged country reference — this tool adds the 0x01000000 tag for ' +
			'you. Auto-accepted actions apply at once; others (peace offers, pacts) are answered by the other empire on its ' +
			'own time, so re-run stellaris_diplomatic_actions after a few ticks to see the outcome. Measured 2026-11-20: ' +
			'action_offer_peace_diplomacy_view toward 伊瓦尔人主体群 (while at war, as the war leader) was posted. ' +
			'Federation votes: action_kick_from_federation and action_invite_to_federation ARE allowed — measured ' +
			'2026-11-21, posting them makes the engine create the federation vote itself (notification + vote window + ' +
			'proposed_actions record). invite is two-stage: our vote first, then the invitee accepts via respond_diplo_action. ' +
			'action_ask_to_join_federation needs no vote on our side — it lands in the TARGET\'s incoming_actions. ' +
			'The other requires_alliance_vote actions (federation association, law/type votes, war declaration) are still ' +
			'refused here as fake delivery.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'target country id (bare id is fine; the tag is added for you)' },
				action: { type: 'string', description: 'action key, e.g. action_offer_peace_diplomacy_view' },
				terms: {
					type: 'object',
					description:
						'subject-agreement terms to write into the embedded CAgreement (protocol actions only, e.g. ' +
						'action_demand_subjugation / action_ask_to_be_subjugated): key = term key, value = term value key, ' +
						'e.g. {"subject_holdings_limit": "subject_holdings_limit_0", "joins_overlord_wars": ' +
						'"joins_overlord_wars_all", "subject_diplomacy": "subject_can_do_diplomacy"}. Measured 2026-11-21: ' +
						'the UI sends exactly this way — one command, the terms ride in the embedded agreement.',
				},
				subsidies: {
					type: 'object',
					description:
						'resource subsidies as fractions (0.15 = 15%): basic / advanced (consumer goods + alloys) / research / ' +
						'strategic / unity. Written as i64 fixed ×1e5, negative when the overlord pays the subject (the sign the engine uses).',
				},
				dry_run: {
					type: 'boolean',
					description: 'build the action, write the terms, read them back and DO NOT post (protocol actions only)',
				},
			},
			required: ['country_id', 'action'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.country_id)
			const action = String(args?.action ?? '')
			if (!Number.isFinite(id)) return text('country_id is required')
			if (!action) return text('action is required（取自 stellaris_diplomatic_actions 的键）')
			// `*_diplomacy_view` 只是“打开某个界面”的占位动作，投出去不会有真提议。
			// 2026-11-20 拿 action_offer_peace_diplomacy_view 连投三次：引擎把它写成“考虑中”，
			// 但没有通知、没有联邦投票、对方也没收到——玩家指出这是假投送。
			if (/_diplomacy_view$/.test(action)) {
				return text(
					`拒投：${DIPLO_CN[action] ?? action}（${action}）是**界面占位动作**，不是真动作。\n` +
						'它在 common/diplomatic_actions 里只有 is_listed = yes——真实流程是先打开对应界面（和平、索赔等），\n' +
						'由界面组一个 deal 再发；桥的 propose 只投了动作本体，跳过通知与联邦投票，所以对方收不到。',
				)
			}
			if (action === 'action_declare_war') {
				return text(
					'宣战不走这个工具：它需要一个带 CWarGoal 的 CDeclareWarAction，空动作会被引擎以「尚未设置战争目标」拒掉。\n' +
						'用 stellaris_declare_war { country_id, goal_key }（2026-11-20 打通）。',
				)
			}
			// 踢人与邀请加入联邦是例外（都 2026-11-21 实测）：它们虽然也带 requires_alliance_vote，
			// 但桥直投之后引擎会自己走表决——或者（邀请、且我们还没有联邦时）直接开一场联邦成立邀请，不是假投送。
			if (action === 'action_kick_from_federation' || action === 'action_invite_to_federation') {
				const isKick = action === 'action_kick_from_federation'
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'propose_diplomatic_action', {
					country_id: rawCountryRef(id),
					action,
					...(args?.terms && typeof args.terms === 'object' ? { terms: args.terms } : {}),
					...(args?.subsidies && typeof args.subsidies === 'object' ? { subsidies: args.subsidies } : {}),
					...(args?.dry_run === true ? { dry_run: true } : {}),
				})
				if (r?.success === false || r?.error) return text(`发起失败：${r?.error ?? JSON.stringify(r)}`)
				if (isKick) {
					return text(
						`已发起：踢出联邦成员（${action}）→ country ${id}\n` +
							'**这条会自己走联邦表决**（实测 2026-11-21：通知、投票窗、proposed_actions 记录都会出现），不是假投送。\n' +
							'接着用 stellaris_diplo_vote 看 id（kind = kick_from_federation）并投自己那一票；多数通过才踢得掉，' +
							'到期没投完会自动拒绝。',
					)
				}
				return text(
					`已发起：邀请加入联邦（${action}）→ country ${id}\n` +
						'要分两种身份看（都 2026-11-21 实测）：\n' +
						'· 我们**已经在联邦里**：先起我方联邦表决（挂在我们自己的 proposed_actions，窗口「邀请加入联邦」）；' +
						'票过了才落进对方 `incoming_actions`，它点头才入盟。\n' +
						'· 我们**还没有联邦**（对方也是独立国）：这条就是**组建联邦**——不表决，直接落进对方 `incoming_actions`' +
						'（`get_incoming_actions`），它接受即成立一个两成员的联邦。\n' +
						'接着用 stellaris_diplo_vote 看有没有待表决；没有就去 `get_incoming_actions` 看对方那条邀请的 id。',
				)
			}
			const VOTE = new Set([
				'action_declare_war', 'action_offer_federation_association_status', 'action_revoke_federation_association_status',
				'action_vote_for_federation_law', 'action_vote_for_federation_type',
			])
			if (VOTE.has(action)) {
				return text(
					`暂不投：${DIPLO_CN[action] ?? action}（${action}）在脚本里带 requires_alliance_vote = yes。\n` +
						'真流程要先发起联邦投票、等成员通过再递给对方；桥目前只会直投 propose，会跳过投票（等于假投送）。\n' +
						'这条要先接投票，再放行。',
				)
			}
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'propose_diplomatic_action', {
					country_id: rawCountryRef(id),
					action,
				})
				if (r?.success === false || r?.error) {
					return text(`发起失败：${r?.error ?? JSON.stringify(r)}`)
				}
				if (r?.dry_run === true) {
					const patched = r?.patched ?? {}
					const rows = (patched.terms ?? []).map((t) => `   ${t.key}：${t.value}`)
					const subs = (patched.resource_terms ?? [])
						.filter((t) => Number(t.value) !== 0)
						.map((t) => `${t.key} ${Math.round(Math.abs(Number(t.value)) * 1000) / 10}%`)
					return text(
						`dry_run：${DIPLO_CN[action] ?? action} → country ${id}｜条款已写入（没有投递）\n` +
							(rows.length ? `${rows.join('\n')}\n` : '（没有条款可回读）\n') +
							(subs.length ? `补贴：${subs.join(' / ')}` : '补贴：无'),
					)
				}
				return text(
					`已发起：${DIPLO_CN[action] ?? action}（${action}）→ country ${id}\n` +
						`${r?.message ?? ''}\n` +
						'（自动接受的动作立刻生效；求和、条约这类要等对方回应——过几个 tick 再跑 stellaris_diplomatic_actions 看结果）',
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 宣称星系（2026-11-22 接线；桥这条 2026-11-21 已实测过）——
	   桥 claim_system：CAddSystemClaimCommand / CRemoveSystemClaimCommand，按界面的写法
	   （country + system + claims 数 + 当天日期）投。IsValid 覆盖 CGalacticObject::IsClaimableBy
	   与影响力代价（CCountry::CalcClaimCost），拒绝时是引擎原话。投完自己回读一次 player_claims。 */
	add({
		name: 'stellaris_claim_system',
		description:
			'Claim ONE system for your empire, or take claims back. Bridge method claim_system posts ' +
			'CAddSystemClaimCommand / CRemoveSystemClaimCommand the way the UI does (country + system + claims count + ' +
			'today\'s date). The command runs IsValid first — CGalacticObject::IsClaimableBy plus the influence cost ' +
			'(CCountry::CalcClaimCost) — so a refusal carries the engine\'s own text (not enough influence, the system is ' +
			'not claimable, ...). remove:true removes claims; without count it takes back everything you hold there. ' +
			'Measured 2026-11-21: system 123 奥菲洛斯 claim 0 → 1 and remove 1 → 0, both applied at once. A claim is what ' +
			'gives the 「征服」 casus belli: common/war_goals/00_war_goals.txt has wg_conquest = { casus_belli = cb_claim } ' +
			'and cb_claim requires has_claim = FROM, so winning a normal war then takes every claimed system, occupied or ' +
			'not. Some empires cannot claim at all (the UI shows 「游牧民无法宣称恒星系」) — then the engine refuses. ' +
			'Do NOT use action_make_claims_diplomacy_view: that is only the placeholder that opens the claims UI.',
		parameters: {
			type: 'object',
			properties: {
				system_id: { type: 'number', description: 'system id (get_system / find_systems)' },
				remove: { type: 'boolean', description: 'remove claims instead of adding them (default: all you hold there)' },
				count: { type: 'number', description: 'how many claims to add or remove (default 1 to add / all to remove)' },
			},
			required: ['system_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const systemId = Number(args?.system_id)
			if (!Number.isFinite(systemId)) return text('system_id is required（get_system / find_systems 的 id）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				let before = null
				try { before = await rpc(pipe, 'get_system', { system_id: systemId }) } catch { before = null }
				const countArg = Number(args?.count)
				const result = await rpc(pipe, 'claim_system', {
					system_id: systemId,
					remove: args?.remove === true,
					count: Number.isFinite(countArg) ? countArg : 0,
				})
				if (result?.success === false || result?.error) {
					const held = result?.player_claims ?? before?.player_claims
					const where = before?.owner_name ? `｜属于 ${before.owner_name}` : ''
					return text(`失败：${result?.error ?? '未知原因'}${held !== undefined ? `（这里现有宣称 ${held}）` : ''}${where}`)
				}
				let after = null
				try { after = await rpc(pipe, 'get_system', { system_id: systemId }) } catch { after = null }
				const name = result?.system ?? before?.name ?? after?.name ?? `system ${systemId}`
				const from = result?.player_claims_before ?? before?.player_claims
				const to = after?.player_claims
				const owner = after?.owner_name ?? before?.owner_name
				const verb = args?.remove === true ? '撤回' : '宣称'
				const delta = from !== undefined && from !== null && to !== undefined && to !== null
					? ` · 我们的宣称 ${from} → ${to}`
					: ''
				return text(
					`${verb}成功：${name}（system ${systemId}${owner ? `，${owner}` : ''}）· count ${result?.count ?? countArg ?? 1}${delta}\n` +
						(result?.message ?? '') +
						(args?.remove === true ? '' : '\n之后对这个国家的「征服」战争目标就成立了（cb_claim 要 has_claim = FROM）'),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 战争表（桥 get_wars，2026-11-20 新增）——
	   注意：桥枚举的是全银河的 CWar 库，**不是只管我们自己的仗**——所以这里标出「我们」的，
	   并给 mine: true 只看我方参战的。战争疲惫是原始定长值，除以 100 才是界面上的百分数。 */
	add({
		name: 'stellaris_wars',
		description:
			'Every war in the galaxy from the CWar table (bridge get_wars, 2026-11-20): id, both sides, war goals ' +
			'(attacker_war_goal / defender_war_goal: type, actor, target, win), war exhaustion on both sides and whether ' +
			'it has ended. This lists ALL wars, not only ours — the tool marks the ones we fight in and mine:true filters to ' +
			'those. Exhaustion is the raw fixed-point value; divide by 100 for the percentage the UI shows. Measured ' +
			'2026-11-20: 4 wars in the galaxy, ours is 灵能魔女 vs 伊瓦尔人主体群 (wg_conquest vs wg_humiliation).',
		parameters: {
			type: 'object',
			properties: {
				mine: { type: 'boolean', description: 'only wars the player country fights in' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const [w, me] = [await rpc(pipe, 'get_wars', {}), await rpc(pipe, 'get_player_country', {})]
				if (args?.raw === true) return text(JSON.stringify(w))
				if (w?.error) return text(`bridge: ${w.error}`)
				const myId = Number(me?.country_id)
				const wars = Array.isArray(w?.wars) ? w.wars : []
				const involvesMe = (x) => {
					if (!Number.isFinite(myId)) return false
					// 只看 actor 会漏：守方目标没设时 actor 为空，而我们可能正是攻方目标里的 target。
					const ids = [
						x?.attacker_war_goal?.actor_id, x?.attacker_war_goal?.target_id,
						x?.defender_war_goal?.actor_id, x?.defender_war_goal?.target_id,
					].map((v) => Number(v))
					return ids.includes(myId)
				}
				const shown = args?.mine === true ? wars.filter(involvesMe) : wars
				const line = (x, tag) => {
					const ag = x.attacker_war_goal ?? {}
					const dg = x.defender_war_goal ?? {}
					const who = (g) => `${g.actor_name ?? '?'} → ${g.target_name ?? '?'}`
					return [
						`  ${tag} id ${x.id}  ${who(ag)}`,
						`      攻方目标 ${ag.type ?? '—'}（win=${ag.win ? 1 : 0}）｜守方目标 ${dg.type ?? '—'}（win=${dg.win ? 1 : 0}）`,
						`      疲惫原始值 ${x.attacker_exhaustion} / ${x.defender_exhaustion}（尺度未定，别当百分数读）｜${x.ended ? '已结束' : '进行中'}`,
					].join('\n')
				}
				const mine = shown.filter(involvesMe)
				const others = shown.filter((x) => !involvesMe(x))
				const lines = [`银河战争 ${wars.length} 场（我们参与 ${wars.filter(involvesMe).length}）`]
				if (args?.mine !== true && mine.length) {
					lines.push('', '【我们的】')
					for (const x of mine) lines.push(line(x, '★'))
				}
				if (args?.mine === true) {
					if (!mine.length) return text('我们没有在打仗')
					lines.length = 0
					lines.push(`我们的战争 ${mine.length} 场`, '')
					for (const x of mine) lines.push(line(x, '★'))
					return text(lines.join('\n'))
				}
				if (others.length) {
					lines.push('', '【其他（别国之间的仗，全银河）】')
					for (const x of others) lines.push(line(x, '·'))
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 防御方设战争目标（桥 set_defender_war_goal，2026-11-20 打通）——
	   用法只有三条：目标默认 wg_humiliation（守方的 defender_default）、别抄攻方那份、
	   成功要看一 tick 之后的 stellaris_wars。
	   内存布局与三层原因（CWarGoal 头 8 字节的类级静态、country 为什么要裸 id）都是桥端的事，
	   记在 stellaris-bridge-patch\README.md 的同名一节里，这里不重复。 */
	add({
		name: 'stellaris_set_defender_war_goal',
		description:
			'Set the DEFENDER war goal on a war we are fighting (bridge set_defender_war_goal → ' +
			'CDiploActionSetDefenderWarGoalCommand). Use it when stellaris_wars shows an empty 守方目标 and we are on ' +
			'the defending side. goal_key defaults to wg_humiliation, the defender default in common/war_goals — do not ' +
			'copy the attacker goal: wg_subjugation needs a casus belli the defender usually lacks, and the engine says ' +
			'so explicitly. dry_run only runs IsValid and lists both country-reference encodings; the bare id is the one ' +
			'the engine accepts, and the bridge already sends that. Success shows up a tick later in stellaris_wars — ' +
			'IsValid passing is not the same as the state having changed. Bridge internals (CWarGoal layout) are in ' +
			'stellaris-bridge-patch\\README.md.',
		parameters: {
			type: 'object',
			properties: {
				war_id: { type: 'number', description: 'war id from stellaris_wars (e.g. 50331651)' },
				goal_key: {
					type: 'string',
					description: 'war goal type key; default wg_humiliation. Others live in common/war_goals/*.txt',
				},
				dry_run: { type: 'boolean', description: 'only run the engine IsValid, post nothing' },
			},
			required: ['war_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const warId = Number(args?.war_id)
			const goalKey = String(args?.goal_key ?? '').trim() || 'wg_humiliation'
			const dryRun = args?.dry_run === true
			if (!Number.isFinite(warId)) return text('war_id is required（取自 stellaris_wars）')
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'set_defender_war_goal', {
					war_id: warId,
					goal_key: goalKey,
					dry_run: dryRun,
				})
				if (r?.error && r?.success !== true) {
					const base = `设置失败：${r.error}`
					if (!dryRun) return text(base)
				}
				if (dryRun) {
					const lines = [`校验 war ${warId} ／ 目标 ${goalKey}（只跑 IsValid，没投递）`]
					const vs = Array.isArray(r?.variants) ? r.variants : []
					for (const v of vs) {
						const tag = v.country_field === 'bare' ? '裸 id  ' : '带标记'
						lines.push(
							`  ${tag} ${v.country_ref}  ${v.ok ? '✓ 通过' : '✗ ' + String(v.reason ?? '').replace(/\n+/g, ' ').trim()}`,
						)
					}
					if (!vs.length && r?.error) lines.push(`  ✗ ${r.error}`)
					lines.push('', '（引擎要的是裸 id；带标记那条被判「不是防御方盟主」是已知的固定要求文本）')
					return text(lines.join('\n'))
				}
				if (r?.success === true) {
					return text(
						`已设置守方战争目标：${goalKey}（war ${warId}）\n` +
							'过一两个 tick 再跑 stellaris_wars，「守方目标」应当变成它；' +
							'内存侧 have_defender_war_goal 会从 0 变 1',
					)
				}
				return text(`设置失败：${r?.error ?? JSON.stringify(r)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 递交求和提案（桥 try_peace_offer，2026-11-20 打通）——
	   用法：war_id + country_id 要写清楚；status 0 维持现状 / 2 达成战争目标（都排队等答复）、
	   1 我方投降（当场结战，别乱发）；同一对目标不能同时挂两条 0/2。
	   三个值 2026-11-20 21:xx 逐个实测过（war 50331651），详见插件 README「发求和提案」。
	   桥侧构造已经完全自足（不需要先在界面递交一次采模板），细节在桥端 README。 */
	add({
		name: 'stellaris_offer_peace',
		description:
			'Offer peace in one war (bridge try_peace_offer → a native COfferPeaceAction wrapped in ' +
			'CDiploActionProposeCommand). status: 0 = status quo (default), 2 = we force our war goals, ' +
			'1 = WE surrender. Measured 2026-11-20 in war 50331651: 0 and 2 are recorded on the RECIPIENT ' +
			'(CCountry.incoming_actions) as a pending offer — the save writes status=status_quo / status=demand_surrender ' +
			'respectively — and the other empire answers on its own time; the war panel then shows ' +
			'"对方目前正在考虑我们提出的另一项提议", the reliable "it was sent" signal. 1 does NOT queue: the enemy ' +
			'accepts at once and the war ends the same frame (save: defenders_surrendered=yes, end=yes, their war goal ' +
			'is won), so send 1 only when losing the war is what you mean. IsValid passes for all three — it does not ' +
			'check whether you can win. Only one pending offer per pair: a second 0/2 attempt is refused with that same ' +
			'line, so reload a pre-offer save to retry. dry_run only runs IsValid. Peace needs no federation vote; a ' +
			'rejection starts a 360-day cooldown (ACTION_MIN_DAYS_REJECTED_PEACE). In a federation post with force: true — ' +
			'the engine refuses an ordinary member, yet posting creates the federation vote the UI itself creates.',
		parameters: {
			type: 'object',
			properties: {
				war_id: { type: 'number', description: 'war id from stellaris_wars (e.g. 50331651)' },
				country_id: { type: 'number', description: 'the other side, bare country id (e.g. 0)' },
				status: {
					type: 'number',
					description: '0 = status quo (default), 1 = WE surrender (the war ends at once), 2 = force our war goals',
				},
				dry_run: { type: 'boolean', description: 'only run the engine IsValid, post nothing' },
				force: {
					type: 'boolean',
					description:
						'post even when IsValid refuses (what the UI does). Needed when we are in a federation: the engine ' +
						'answers 「只有攻击方盟主或防御方盟主才能做出提议」 for an ordinary member, yet posting anyway creates ' +
						'the federation vote (waiting_for_vote=yes) — the UI takes exactly this route.',
				},
			},
			required: ['war_id', 'country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const warId = Number(args?.war_id)
			const target = Number(args?.country_id)
			const status = Number.isFinite(Number(args?.status)) ? Number(args.status) : 0
			const dryRun = args?.dry_run === true
			const force = args?.force === true
			if (!Number.isFinite(warId)) return text('war_id is required（取自 stellaris_wars）')
			if (!Number.isFinite(target)) return text('country_id is required（对方国家，裸 id）')
			const NAME = { 0: '维持现状', 1: '我方投降（当场结战）', 2: '达成战争目标' }
			if (![0, 1, 2].includes(status)) {
				return text(`status 只有三个值：0 维持现状 / 1 我方投降（当场结战）/ 2 达成战争目标（收到 ${args.status}）`)
			}
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'try_peace_offer', {
					war_id: warId,
					country_id: target,
					status,
					dry_run: dryRun,
					force,
				})
				if (dryRun) {
					const ok = r?.is_valid === true
					return text(
						`校验 war ${warId} → country ${target}（${NAME[status] ?? status}，只跑 IsValid）\n` +
							`  ${ok ? '✓ 通过' : '✗ ' + String(r?.reason ?? r?.error ?? '').replace(/\n+/g, ' ').trim()}\n` +
							(ok ? '\n去掉 dry_run 就是真投' : ''),
					)
				}
				if (r?.success === true && r?.posted === true) {
					if (status === 1) {
						return text(
							`已递交投降提案：war ${warId} → country ${target}（我方投降）\n` +
								'这一档不排队：对方无条件接受，战争当场结束，对方的战争目标直接判胜' +
								'（存档里 defenders_surrendered=yes、end=yes）。结果看 stellaris_wars 的「已结束」' +
								'与外交表——我方通常就此成了对方的附庸',
						)
					}
					return text(
						`已递交求和提案：war ${warId} → country ${target}（${NAME[status] ?? status}）\n` +
							'记录落在**对方**身上（incoming_actions），存档里写的是 status_quo / demand_surrender。' +
							'过一会儿若看到「对方目前正在考虑我们提出的另一项提议」，那就是确实递出去了；' +
							'同一对目标不能同时挂两条',
					)
				}
				const why = r?.reason ?? r?.error
				if (why) return text(`递交失败：${String(why).replace(/\n+/g, ' ').trim()}`)
				return text(`递交失败：${JSON.stringify(r)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 收到的外交提案 / 联邦投票（2026-11-20 打通；桥 get_incoming_actions + respond_diplo_action + vote_diplo_action）
	   读法：CCountry+0x1950 → 元素 +0x08 → CDiplomaticAction；当下样本都是求和提案。
	   id = tag<<32 | serial（动作对象 +0x30 一对 u32，低位 serial、高位 tag）。
	   **坑**：respond 的 IsValid 不校验「应答方是不是收件方」，所以这里先比 offer.recipient 与我们自己。 */
	const readInbox = async (pipe) => {
		const r = await rpc(pipe, 'get_incoming_actions', {})
		if (r?.success !== true) return { error: r?.error ?? 'get_incoming_actions 失败', raw: r }
		return { items: Array.isArray(r?.items) ? r.items : [], size: Number(r?.size ?? 0), raw: r }
	}
	const readPendingVotes = async (pipe) => {
		const r = await rpc(pipe, 'get_pending_votes', {})
		if (r?.success !== true) return { error: r?.error ?? 'get_pending_votes 失败', raw: r }
		return { items: Array.isArray(r?.items) ? r.items : [], count: Number(r?.count ?? 0), raw: r }
	}
	const inboxLines = async (pipe, inbox) => {
		const out = [
			`待回应 ${inbox.size} 条`,
			'（id = tag<<32 | serial；一条记录只能应答一次）',
		]
		for (const it of inbox.items) {
			const o = it.offer ?? {}
			if (it.kind === 'propose_agreement') {
				// 协议提案：双方在内嵌协议里（桥已给出 actor/recipient 与条款）
				const keys = []
				if (it.agreement_preset) keys.push(String(it.agreement_preset))
				for (const t of it.terms ?? []) {
					keys.push(String(t.key))
					if (t.value) keys.push(String(t.value))
				}
				const cn = new Map()
				if (keys.length) {
					try {
						const r = await rpc(pipe, 'localize', { keys: [...new Set(keys)] })
						for (const [k, v] of Object.entries(r?.names ?? {})) cn.set(k, v)
					} catch {}
				}
				const label = (k) => AGREEMENT_TERM_CN[String(k)] ?? cn.get(String(k)) ?? String(k)
				const preset = it.agreement_preset ? label(it.agreement_preset) : '自定义（无预设）'
				out.push(
					`  id ${it.id_candidate}（tag ${it.tag} / serial ${it.serial}）｜附庸协议提案｜${it.actor} → ${it.recipient}`,
					`      ${preset}`,
				)
				const terms = (it.terms ?? []).map((t) => `${label(t.key)}：${t.value ? label(t.value) : '—'}`)
				if (terms.length) out.push(`      条款：${terms.join('；')}`)
				const SUB_CN = { basic: '基础', advanced: '进阶', research: '研究', strategic: '战略', unity: '凝聚力' }
				const subs = (it.resource_terms ?? [])
					.filter((t) => Number(t.value) !== 0)
					.map((t) => {
						const k = String(t.key).replace('resource_subsidies_', '')
						return `${SUB_CN[k] ?? k} ${Math.round(Math.abs(Number(t.value)) * 1000) / 10}%`
					})
				out.push(`      补贴：${subs.length ? subs.join(' / ') : '无'}`)
				continue
			}
			const INBOX_KIND_CN = {
				offer_peace: '求和提案',
				propose_agreement: '附庸协议提案',
				form_commercial_pact: '商业条约',
				form_waystation_pact: '驿站条约',
				form_non_aggression_pact: '互不侵犯条约',
				form_defensive_pact: '防御同盟',
				form_research_agreement: '研究协定',
				embassy: '建立大使馆',
				guarantee_independence: '保证独立',
				ask_to_be_released_subject: '请求解除附庸',
			}
			// 桥还没给这一类起名：贸易协定的动作类是 0x23B58E8（2026-11-21 实测，与 propose_capture 抓到的一致）
			// 桥还没给这两类起名（2026-11-21 实测）：
			//   贸易协定 = 0x23B58E8；请求离开联邦（action_ask_to_leave_federation）= 0x23B7BE8
			const TRADE_DEAL_VT = 0x23b58e8
			const ASK_LEAVE_FED_VT = 0x23b7be8
			const vt = Number(it.action_vtable_rva)
			const kindText =
				vt === TRADE_DEAL_VT
					? '贸易协定提案'
					: vt === ASK_LEAVE_FED_VT
						? '请求离开联邦（霸权：成员向主席请求）'
						: (INBOX_KIND_CN[it.kind] ?? `未识别的动作类（vtable 0x${vt.toString(16)}）`)
			const who =
				it.recipient !== undefined && Number(it.recipient) !== -1
					? `${it.actor} → ${it.recipient}`
					: `actor ${it.actor ?? '?'}`
			out.push(
				`  id ${it.id_candidate}（tag ${it.tag} / serial ${it.serial}）｜${kindText}` +
					(it.kind === 'offer_peace' ? `｜${o.status_name ?? '?'}｜war ${o.war}｜${who}` : `｜${who}`),
			)
		}
		// 同一个 id 出现多次时提醒：应答按 id 匹配，会分不清是哪一条（2026-11-21 实测过，
		// 引擎自己的收件箱里出现过两条 tag/serial 完全相同的记录）
		const idCount = new Map()
		for (const it of inbox.items) {
			const k = String(it.id_candidate)
			idCount.set(k, (idCount.get(k) || 0) + 1)
		}
		const dup = [...idCount].filter(([, n]) => n > 1).map(([k, n]) => `${k}×${n}`)
		if (dup.length) {
			out.push('', `⚠ 有重复 id：${dup.join('、')}——应答按 id 匹配，可能打到错的那条；必要时先拒掉多余的再答`)
		}
		return out
	}

	// —— 组建联邦：类型筛选（2026-11-21） ——
	// 依据 common/federation_types/*.txt 的 potential / allow：potential 全是 host_has_dlc = Federations
	// （本机全有），imperial_loyalists 是 potential = always = no；allow 只有三类判据——伦理 / 公民 / 传统。
	// **引擎的 IsValid 不看这些**（2026-11-21 实测：7 种全放行），所以筛选只能在这里自己做。
	const FEDERATION_TYPE_RULES = {
		default_federation: (f) => (f.civics.has('civic_barbaric_despoilers') ? '野蛮掠夺者不能组建联邦' : null),
		trade_federation: (f) => {
			if (f.civics.has('civic_barbaric_despoilers')) return '野蛮掠夺者不能组建联邦'
			if (f.megacorp || f.civics.has('civic_merchant_guilds') || f.trads.has('tr_mercantile_federations_finish')) return null
			return '需要 megacorp / 商人行会公民，或完成商业联邦传统'
		},
		research_federation: (f) => {
			if (f.civics.has('civic_barbaric_despoilers')) return '野蛮掠夺者不能组建联邦'
			if (f.materialist || f.machine || f.trads.has('tr_discovery_federations_finish')) return null
			return '需要唯物 / 机械智能，或完成探索联邦传统'
		},
		military_federation: (f) =>
			f.militarist || f.trads.has('tr_unyielding_federations_finish') ? null : '需要军国，或完成筑防联邦传统',
		hegemony_federation: (f) =>
			f.civics.has('civic_machine_servitor') ||
			f.authoritarian ||
			f.trads.has('tr_domination_federations_finish') ||
			f.trads.has('tr_domination_federations_wilderness_finish')
				? null
				: '需要威权 / 失控机仆，或完成支配联邦传统',
		spiritualist_federation: (f) => {
			if (f.civics.has('civic_barbaric_despoilers')) return '野蛮掠夺者不能组建联邦'
			if (f.spiritualist || f.trads.has('tr_harmony_federations_finish')) return null
			return '需要唯心，或完成和谐联邦传统'
		},
		imperial_loyalists_federation: () => 'potential = always = no：这种联邦不能由玩家组建',
	}
	const fedTypeVerdict = (key, f) => (FEDERATION_TYPE_RULES[key] ? FEDERATION_TYPE_RULES[key](f) : '未知类型')
	const federationFacts = async (pipe) => {
		const gov = (await rpc(pipe, 'get_government')) ?? {}
		const trad = (await rpc(pipe, 'get_traditions')) ?? {}
		const ethics = new Set((gov.ethics ?? []).map((x) => x.key))
		const civics = new Set((gov.civics ?? []).map((x) => x.key))
		const trads = new Set((trad.unlocked_traditions ?? []).map((x) => x.key))
		const authority = String(gov.authority ?? '')
		const government = String(gov.government_type ?? '')
		return {
			ethics,
			civics,
			trads,
			authority,
			government,
			machine: /^auth_machine/.test(authority) || /^gov_machine/.test(government),
			megacorp: authority === 'auth_megacorp',
			materialist: ethics.has('ethic_materialist'),
			spiritualist: ethics.has('ethic_spiritualist'),
			militarist: ethics.has('ethic_militarist'),
			authoritarian: ethics.has('ethic_authoritarian'),
		}
	}

	add({
		name: 'stellaris_federations',
		description:
			'All federations in the galaxy, read straight from the engine (bridge get_federations; no command, nothing ' +
			'written). Offsets measured 2026-11-21 against the UI: id (+0x08), name (+0x28), member count (+0xA0), ' +
			'progression (+0x80) → type (+0x1A0), level (+0x08), cumulative experience (+0x1A8), in-level experience ' +
			'(+0x1B0), cohesion (+0x1C0). Experience and cohesion are fixed point ×100000 (UI 1313 and 89 read as ' +
			'131332000 / 8960000). NOT stored anywhere: the monthly experience the UI shows — it is derived, so this ' +
			'tool cannot report it. NEW 2026-11-21: the CURRENT LAWS are now readable too (bridge get_federation_laws): ' +
			'progression +0x2E8 points at a container of CFederationLaw* (0x10 header, then 0x18-byte slots, empty slots ' +
			'are zero), one entry per law category, cross-checked 13/13 against two saves. The tool prints the laws of ' +
			'our own federation(s) with localised "category = value" (category from common/federation_laws, names from ' +
			'the engine) plus the pending/last-voted pair; all_laws: true adds every federation.',
		parameters: {
			type: 'object',
			properties: {
				all_laws: { type: 'boolean', description: 'true = 别的联邦也列当前法律（默认只列我们所在的那个）' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'get_federations')
				if (r?.success === false || r?.error) return text(`bridge: ${r?.error ?? JSON.stringify(r)}`)
				const items = Array.isArray(r?.items) ? r.items : []
				if (!items.length) return text('银河里没有联邦')
				let myId = -1
				try {
					const me = await rpc(pipe, 'get_player_country')
					myId = Number(me?.country_id ?? -1)
				} catch {}
				const showAll = args?.all_laws === true
				const typeNames = await localizeKeys(pipe, items.map((f) => f.type))
				const lines = [`银河联邦 ${r.count} 个`]
				for (const f of items) {
					const name = String(f.name ?? '').trim() || '（无名）'
					const mids = Array.isArray(f.member_ids) && f.member_ids.length ? `（country ${f.member_ids.join('/')}）` : ''
					const mine = Array.isArray(f.member_ids) && f.member_ids.some((x) => (Number(x) & 0xffffff) === myId)
					lines.push(
						`  #${f.index} id ${f.id}「${name}」｜${locName(typeNames, f.type) || '?'}｜等级 ${f.level}｜` +
							`团结度 ${Number(f.cohesion).toFixed(1)}｜经验 ${Number(f.experience_level).toFixed(1)}` +
							`（累计 ${Number(f.experience_total).toFixed(1)}）｜成员 ${f.members}${mids}`,
					)
					if (!mine && !showAll) continue
					// 当前法律：桥读容器，这边补中文与「值 → 类别」
					let laws = null
					try {
						laws = await rpc(pipe, 'get_federation_laws', { fed_id: Number(f.id) })
					} catch {}
					if (!laws || laws.success === false) {
						lines.push(`      当前法律读不到：${laws?.error ?? '桥没有返回'}`)
						continue
					}
					const rows = Array.isArray(laws.laws) ? [...laws.laws] : []
					if (!rows.length) {
						lines.push('      当前法律：容器是空的（这一版可能没这个结构）')
						continue
					}
					rows.sort((a, b) => Number(a.law_index) - Number(b.law_index))
					const lawMap = await loadFederationLawMap()
					const names = await localizeKeys(pipe, rows.flatMap((x) => [x.key, lawMap.get(x.key)]))
					lines.push(`      ${mine ? '我们在这个联邦里' : '不在我们联邦里'} · 当前法律 ${rows.length} 条：`)
					for (const row of rows) {
						const cat = lawMap.get(row.key)
						const label = cat
							? `${locName(names, cat)} = ${locName(names, row.key)}`
							: locName(names, row.key)
						lines.push(`        ${label}${cat ? '' : `（${row.key}）`}`)
					}
					const pend = Array.isArray(laws.pending) ? laws.pending.filter((x) => x.key) : []
					if (pend.length) {
						const pn = await localizeKeys(pipe, pend.flatMap((x) => [x.key, lawMap.get(x.key)]))
						const shown = pend.map((x) => {
							const cat = lawMap.get(x.key)
							return cat ? `${locName(pn, cat)} → ${locName(pn, x.key)}` : locName(pn, x.key)
						})
						lines.push(`      （最近表决过：${shown.join(' / ')}）`)
					}
				}
				lines.push('', '每月经验没有存储字段（界面现算），这里读不到')
				return text(lines.join('\n'))
			} catch (e) {
				return text(`bridge: ${String(e?.message ?? e)}`)
			}
		},
	})

	/* —— 一个联邦的法律总表：当前 / 能换成什么 / 为什么不能（2026-11-21）——
	   三层判据，缺一层就会说谎：
	   ① potential（脚本三值：能判的 has_federation_type / has_federation_law / always / host_has_dlc，
	      4 个旗标由「类型 + 当前法律」推出来；第 5 个没有 setter，保持 unknown）
	   ② required_centralization 对当前集权档（unlocks_centralization）
	   ③ 只对前两层放行的候选跑引擎 IsValid（allow：同类冷却 / 交战中 / 已是该值）——省掉一半调用 */
	add({
		name: 'stellaris_federation_laws',
		description:
			'One federation\'s whole law table: what is in force, what it could be changed to, and why the rest is ' +
			'not available. Three layers, because the engine alone is not enough (measured 2026-11-21): ' +
			'① potential is evaluated from common/federation_laws — the vocabulary is tiny (has_federation_type, ' +
			'has_federation_flag, has_federation_law, host_has_dlc, always, wrapped in NOT/AND/OR) and four of the ' +
			'five federation flags are derived from the federation type and the current laws (special_federation, ' +
			'enable_federation_cooldowns, challenge_type_no_cooldown, no_federation_fleet). Only special_federation ' +
			'ever appears in a potential block — the other four live in on_enact and only decide whether a cooldown ' +
			'is added after enacting, so they never affect availability; the fifth ' +
			'(no_federation_fleet_build_right_cooldown) is a 10-day timed flag for exactly that and is not probed ' +
			'back (a flag being set and a category never having been changed look identical from outside). ' +
			'② required_centralization is compared with the level of the centralization law ' +
			'in force. ③ only the survivors reach the engine: propose_federation_law with dry_run (IsValid) covers ' +
			'the allow block — category cooldown, being at war, already having that value. IsValid alone is NOT a ' +
			'filter: centralization_imperial_loyalists passes it although its potential says imperial-loyalists ' +
			'only, and a cooldown refusal comes back with an empty reason. Current values come from ' +
			'get_federation_laws (memory). fed_id defaults to the federation we are in; category narrows the run ' +
			'(substring of the key or the Chinese name); skip_isvalid: true gives a fast read without the engine check.',
		parameters: {
			type: 'object',
			properties: {
				fed_id: { type: 'number', description: '联邦 id（省略 = 我们所在的那个）' },
				category: { type: 'string', description: '只看某一类：键或中文名的片段，如 succession / 继任' },
				skip_isvalid: { type: 'boolean', description: 'true = 不跑引擎校验（快，但看不出冷却）' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const feds = await rpc(pipe, 'get_federations')
				const items = Array.isArray(feds?.items) ? feds.items : []
				if (!items.length) return text('银河里没有联邦')
				let target = null
				if (args?.fed_id !== undefined) {
					target = items.find((f) => Number(f.id) === Number(args.fed_id))
				} else {
					let myId = -1
					try {
						const me = await rpc(pipe, 'get_player_country')
						myId = Number(me?.country_id ?? -1)
					} catch {}
					target = items.find((f) => Array.isArray(f.member_ids) && f.member_ids.some((x) => (Number(x) & 0xffffff) === myId))
				}
				if (!target) return text('找不到这个联邦：不带 fed_id 时要我们在某个联邦里（用 stellaris_federations 看 id）')
				const cur = await rpc(pipe, 'get_federation_laws', { fed_id: Number(target.id) })
				if (!cur || cur.success === false) return text(`当前法律读不到：${cur?.error ?? '桥没有返回'}`)
				const current = new Set((cur.laws || []).map((x) => x.key))
				const db = await rpc(pipe, 'probe_law_db', {})
				if (!db || db.success === false) return text(`法律库读不到：${db?.error ?? '桥没有返回'}`)
				const defs = await loadFederationLawDefs()
				const fedType = String(target.type || '')
				const { flags, flagKnown } = deriveFederationFlags(fedType, current)
				const facts = { fedType, laws: current, flags, flagKnown }
				let level = null
				for (const k of current) {
					const d = defs.get(k)
					if (d?.category === 'law_category_centralization' && d.unlocksCentralization != null) level = d.unlocksCentralization
				}
				const catKeys = (db.law_categories || []).map((c) => c.key)
				const vals = (db.laws || []).map((l) => l.key)
				const groups = new Map(catKeys.map((c) => [c, []]))
				const orphans = []
				for (const v of vals) {
					const c = defs.get(v)?.category
					if (c && groups.has(c)) groups.get(c).push(v)
					else orphans.push(v)
				}
				const names = await localizeKeys(pipe, [...catKeys, ...vals])
				const filter = args?.category ? String(args.category) : ''
				const skip = args?.skip_isvalid === true
				const counts = { ok: 0, cur: 0, no: 0, unk: 0 }
				const lines = [
					`联邦 #${target.id}「${String(target.name ?? '').trim() || '（无名）'}」${locName(names, fedType) || fedType}` +
						` · 成员 ${target.members} · ${catKeys.length} 类 / ${vals.length} 档` +
						(level != null ? ` · 集权档 ${level}` : ''),
				]
				if (!defs.size) lines.push('（没读到 common/federation_laws → potential 一律按判不了处理）')
				for (const cat of catKeys) {
					const catName = locName(names, cat)
					if (filter && !(cat.includes(filter) || String(catName).includes(filter))) continue
					const list = groups.get(cat) || []
					if (!list.length) continue
					const curVal = list.find((v) => current.has(v))
					lines.push('', `  ${catName}（${cat}）${curVal ? ` · 当前：${locName(names, curVal)}` : ' · 当前：无（这一类现在没有值）'}`)
					for (const v of list) {
						const d = defs.get(v) || {}
						const vName = locName(names, v)
						if (current.has(v)) {
							counts.cur += 1
							lines.push(`    ·  ${padName(vName)}${v}   ← 当前`)
							continue
						}
						const pot = evalLawPotential(d.potential, facts)
						if (pot === 'false') {
							counts.no += 1
							lines.push(`    ✗  ${padName(vName)}${v}   ${lawPotentialHint(d.potential, facts) || 'potential 不过'}`)
							continue
						}
						if (pot === 'unknown') {
							counts.unk += 1
							lines.push(`    ?  ${padName(vName)}${v}   判不了（potential 里有判不了的旗标）`)
							continue
						}
						if (d.requiredCentralization != null && level != null && level < d.requiredCentralization) {
							counts.no += 1
							lines.push(`    ✗  ${padName(vName)}${v}   需要联邦集权 ≥ ${d.requiredCentralization}（现在 ${level}）`)
							continue
						}
						if (skip) {
							lines.push(`       ${padName(vName)}${v}   （未跑引擎校验）`)
							continue
						}
						let r = null
						try {
							r = await rpc(pipe, 'propose_federation_law', { law_key: v, dry_run: true })
						} catch {}
						if (r?.success === true) {
							counts.ok += 1
							// 生效后的冷却预测（只看 on_enact 的 if/else_if 链）
							const cd = lawCooldownAfterEnact(d, facts)
							const cdText = cd?.category
								? `   ｜生效后：${locName(names, `law_category_${cd.category}`)} 类冷却 ${cd.days ?? '?'} 天`
								: cd?.none
									? '   ｜生效后：免这类冷却'
									: cd?.unknown
										? '   ｜生效后冷却：判不了'
										: ''
							lines.push(`    ✓  ${padName(vName)}${v}${cdText}`)
						} else {
							counts.no += 1
							const why = String(r?.reason ?? '').replace(/\n+/g, ' ').trim() ||
								'引擎拒绝（无原因：多半是同类冷却 / 交战中）'
							lines.push(`    ✗  ${padName(vName)}${v}   ${why}`)
						}
					}
				}
				if (orphans.length) {
					lines.push('', `  未归类 ${orphans.length} 个（脚本里没读到类别引用）`)
					for (const v of orphans) lines.push(`       ${padName(locName(names, v))}${v}`)
				}
				lines.splice(1, 0, `可换 ${counts.ok} · 当前 ${counts.cur} · 不可用 ${counts.no} · 判不了 ${counts.unk}`)
				lines.push(
					'',
					'判据：① potential（脚本，unknown 不当 true）② required_centralization 对集权档 ③ IsValid（allow：冷却 / 交战 / 已是该值）',
					'✓ 可换 ｜ · 当前 ｜ ✗ 不可用 ｜ ? 判不了',
					'引擎「无原因」的拒绝多半是同类冷却（改过某类之后，同类候选整类都会被拒，直到冷却结束）；交战中的联邦也会整类被拒',
				)
				return text(lines.join('\n'))
			} catch (e) {
				return text(`bridge: ${String(e?.message ?? e)}`)
			}
		},
	})

	add({
		name: 'stellaris_form_federation',
		description:
			'⚠ UNVERIFIED — MAY CRASH THE GAME. 2026-11-21 night: both the type listing and the typed invite took the game ' +
			'down (no crash dump); the type-database enumeration is still suspect. Do NOT call this until a read-only ' +
			'database probe has been validated. Kept in the catalog so the wiring is not lost. ' +
			'Form a NEW federation with an independent empire — this is action_invite_to_federation under the hood ' +
			'(2026-11-21 measured: no vote when neither side has a federation; the invite lands in the target\'s ' +
			'incoming_actions and its acceptance founds a two-member federation). ' +
			'federation_type picks the type for the new federation: the bridge writes CInviteToFederationAction::' +
			'federation_type (+0xB0) from CFederationTypeDatabase before posting — default_federation is what the ' +
			'engine fills in when you leave it empty. list_types: true asks the engine which types it would accept for ' +
			'this pair (one IsValid per type, nothing posted) — types differ by ethic / tradition / civic requirements ' +
			'and only the engine can judge them. NOTE: the type only matters when founding (neither side in a ' +
			'federation); if we already have one this is an ordinary invite and the type is irrelevant.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'target country id (bare id is fine)' },
				federation_type: {
					type: 'string',
					description: 'type key for the new federation, e.g. trade_federation（省略 = default_federation）',
				},
				list_types: { type: 'boolean', description: 'true = 只列出引擎接受哪些类型，不投递' },
			},
			required: ['country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.country_id)
			if (!Number.isFinite(id)) return text('country_id is required')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				// 2026-11-21：**引擎的 IsValid 不查类型资格**（7 种全放行，实测），所以筛选只能照
				// common/federation_types 的 potential/allow 自己判：判据只有伦理 / 公民 / 传统三种，
				// facts 从 get_government 与 get_traditions 取。imperial_loyalists 是 potential = always = no。
				const facts = await federationFacts(pipe)
				const verdict = (key) => fedTypeVerdict(key, facts)
				if (args?.list_types === true) {
					const lines = ['组建联邦：按 common/federation_types 的 allow 逐条判（引擎 IsValid 对 7 种全放行，不能当筛子）']
					for (const key of Object.keys(FEDERATION_TYPE_RULES)) {
						const why = verdict(key)
						lines.push(why ? `  ✗ ${key}  ${why}` : `  ✓ ${key}`)
					}
					lines.push(
						'',
						`我们：权威 ${facts.authority || '?'} ｜ 伦理 ${[...facts.ethics].join(',') || '无'} ｜ 公民 ${[...facts.civics].join(',')}`,
						'用 federation_type 指定一个 ✓ 的再投；省略 = default_federation',
					)
					return text(lines.join('\n'))
				}
				const chosen = args?.federation_type ? String(args.federation_type) : 'default_federation'
				const why = verdict(chosen)
				if (why) return text(`不能选 ${chosen}：${why}\n先用 list_types: true 看哪些 ✓`)
				const r = await rpc(pipe, 'propose_diplomatic_action', {
					country_id: rawCountryRef(id),
					action: 'action_invite_to_federation',
					...(chosen !== 'default_federation' ? { federation_type: chosen } : {}),
				})
				if (r?.success === false || r?.error) return text(`发起失败：${r?.error ?? JSON.stringify(r)}`)
				return text(
					`已发起：组建/邀请联邦 → country ${id}` +
						(args?.federation_type ? `（类型 ${args.federation_type}）` : '（类型用引擎默认）') +
						'\n还没联邦时：不表决，落进对方 incoming_actions，对方接受即成立；' +
						'已有联邦时：先我方表决。之后用 get_incoming_actions / stellaris_diplo_vote 看后续。',
				)
			} catch (e) {
				return text(`bridge: ${String(e?.message ?? e)}`)
			}
		},
	})

	add({
		name: 'stellaris_diplo_inbox',
		description:
			'Pending diplomatic offers addressed to us — the whole inbox, not only peace (bridge get_incoming_actions → ' +
			'CCountry+0x1950 → the action object). Renamed from stellaris_peace_inbox on 2026-11-21 because it now also ' +
			'reads subject-agreement proposals (kind propose_agreement) and federation invites (kind unknown, vtable ' +
			'0x23B71F8 / 0x23B7898), and it will show any other action class that lands here. ' +
			'With no arguments it only lists them; pass accept: true / false to answer one via CDiploActionRespondCommand. ' +
			'Measured 2026-11-20: a peace offer shows as kind offer_peace with actor/recipient/war/status and an id ' +
			'(tag<<32 | serial); answering a decline moved the record from incoming_actions to rejected_actions. ' +
			'CAREFUL: the engine does NOT check that the responder is the recipient, so this tool compares the recipient ' +
			'with our own country first (item.recipient for agreement proposals, offer.recipient for peace offers). ' +
			'Accepting a 「要求我们投降」 offer (status demand_surrender) means losing the war — use dry_run first when unsure. ' +
			'NEW 2026-11-21: subject-agreement proposals (action_demand_subjugation / action_ask_to_be_subjugated, ' +
			'vtable 0x23CCCF8 / 0x23CC440) are read as kind propose_agreement: the two sides come from the embedded ' +
			'agreement (+0x70 owner / +0x74 target — the +0x20 actor is wrong for this class), and the tool prints the ' +
			'preset, every term (localised) and the resource subsidies. Measured 2026-11-21: a proposal we sent with ' +
			'six custom terms + 10%/20% subsidies read back exactly, and declining it here went through.',
		parameters: {
			type: 'object',
			properties: {
				accept: { type: 'boolean', description: 'true = 接受 / false = 拒绝；省略则只列出来（不投递）' },
				id: { type: 'number', description: '要应答的 id（省略则用第一条）' },
				dry_run: { type: 'boolean', description: '只跑 IsValid，不投递' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const inbox = await readInbox(pipe)
				if (inbox.error) return text(`bridge: ${inbox.error}`)
				if (!inbox.items.length) return text('现在没有待回应的外交提案')
				const lines = await inboxLines(pipe, inbox)
				if (args?.accept === undefined) {
					lines.push('', '要应答就带 accept: true（接受）/ false（拒绝）；先 dry_run 更稳')
					return text(lines.join('\n'))
				}
				const pick =
					args?.id !== undefined
						? inbox.items.find((x) => Number(x.id_candidate) === Number(args.id))
						: inbox.items[0]
				if (!pick) return text(`没有 id ${args.id} 的待回应提案`)
				const me = await rpc(pipe, 'get_player_country', {})
				const myId = Number(me?.country_id ?? -1)
				const recipient = Number(pick?.recipient ?? pick?.offer?.recipient ?? -1)
				// 收件方可能是带标记的引用（0x01000007），比自己时要掩掉高 8 位
				if ((recipient & 0xffffff) !== (myId & 0xffffff)) {
					return text(`这条提案的收件方是 ${recipient}，而我们自己是 ${myId}——不能替别人应答（引擎的 IsValid 不拦这个）`)
				}
				const r = await rpc(pipe, 'respond_diplo_action', {
					id: Number(pick.id_candidate),
					accept: args.accept === true,
					dry_run: args?.dry_run === true,
				})
				if (r?.dry_run === true) {
					return text(
						`校验：${args.accept === true ? '接受' : '拒绝'} id ${pick.id_candidate}\n` +
							`  ${r?.is_valid === true ? '✓ 通过' : '✗ ' + String(r?.reason ?? '').replace(/\n+/g, ' ').trim()}`,
					)
				}
				if (r?.success === true && r?.posted === true) {
					const status = pick?.offer?.status_name
					const warn =
						args.accept === true && status === 'demand_surrender'
							? '\n注意：这条是「要求我们投降」，接受就是认输（会弹战败事件）'
							: ''
					return text(`已应答（${args.accept === true ? '接受' : '拒绝'}）id ${pick.id_candidate}${warn}`)
				}
				return text(`应答失败：${String(r?.reason ?? r?.error ?? JSON.stringify(r)).replace(/\n+/g, ' ').trim()}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 联邦法律：值 → 类别，以及名字（2026-11-21）——
	   「某一档值属于哪一类」只有脚本里有：common/federation_laws 一类一档，每个值块内带
	   `has_any_federation_law_in_category = law_category_*`。内存里没有这条边——CFederationLaw 与
	   CFederationLawCategory 互不指向（2026-11-21 两种扫法都是零命中），所以这条归属只能解析文件。
	   名字一律问引擎本地化：law_category_* / 值键 / federation_type 键都能翻，且是界面措辞。 */
	const LAW_MAP_CACHE = { map: null }
	const LAW_DEF_CACHE = { defs: null }
	/**
	 * 脚本里每个值档的完整定义：类别、potential、required_centralization、unlocks_centralization。
	 * 一次读完缓存；`loadFederationLawMap` 就是它的「值 → 类别」投影。
	 */
	const loadFederationLawDefs = async () => {
		if (LAW_DEF_CACHE.defs) return LAW_DEF_CACHE.defs
		const defs = new Map()
		const dir = await findGameDir()
		if (!dir) return defs
		const ldir = path.join(dir, 'common', 'federation_laws')
		let files = []
		try {
			files = await fs.readdir(ldir)
		} catch {
			return defs
		}
		for (const f of files) {
			if (!f.endsWith('.txt') || f === '00_example.txt') continue
			let txt
			try {
				txt = await fs.readFile(path.join(ldir, f), 'utf8')
			} catch {
				continue
			}
			const re = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\{/gm
			let m
			const cdVar = /^@cooldown\s*=\s*(\d+)/m.exec(txt)
			const cooldownDefault = cdVar ? Number(cdVar[1]) : null
			while ((m = re.exec(txt))) {
				const key = m[1]
				const blk = braceBlock(txt, txt.indexOf('{', m.index))
				if (blk === null) continue
				// 类别：首选 allow 里的 has_any_federation_law_in_category；
				// 事件法（15_event_laws.txt）的 allow 是空的，退到 on_enact 里 `<类别>_cooldown` 的修正名
				const direct = /has_any_federation_law_in_category\s*=\s*(law_category_[A-Za-z0-9_]+)/.exec(blk)
				const viaCd = direct ? null : /federation_law_category_([a-z0-9_]+)_cooldown/.exec(blk)
				const category = direct ? direct[1] : viaCd ? `law_category_${viaCd[1]}` : null
				const potAt = /potential\s*=\s*\{/.exec(blk)
				const potential = potAt ? braceBlock(blk, blk.indexOf('{', potAt.index)) : null
				const rc = /required_centralization\s*=\s*\{\s*base\s*=\s*(-?\d+)/.exec(blk)
				const unl = /unlocks_centralization\s*=\s*(-?\d+)/.exec(blk)
				// on_enact 只用来预测「生效后会不会上冷却」（2026-11-21）
				const oneAt = /on_enact\s*=\s*\{/.exec(blk)
				const onEnact = oneAt ? braceBlock(blk, blk.indexOf('{', oneAt.index)) : null
				defs.set(key, {
					category,
					potential,
					requiredCentralization: rc ? Number(rc[1]) : null,
					unlocksCentralization: unl ? Number(unl[1]) : null,
					onEnact,
					cooldownDefault,
				})
			}
		}
		LAW_DEF_CACHE.defs = defs
		return defs
	}
	const loadFederationLawMap = async () => {
		if (LAW_MAP_CACHE.map) return LAW_MAP_CACHE.map
		const defs = await loadFederationLawDefs()
		const map = new Map()
		for (const [k, d] of defs) if (d.category) map.set(k, d.category)
		LAW_MAP_CACHE.map = map
		return map
	}
	const localizeKeys = async (pipe, keys) => {
		const list = [...new Set(keys)].filter(Boolean)
		if (!list.length) return {}
		try {
			const answer = await rpc(pipe, 'localize', { keys: list })
			return answer?.names ?? {}
		} catch {
			return {}
		}
	}
	/** 引擎没翻出来时会原样返回键，那就当没有 */
	const locName = (names, key) => {
		if (!key) return ''
		const v = names?.[key]
		return v && v !== key ? v : key
	}
	/** 联邦类型专用档的标记（按值的后缀认；名字取引擎本地化，2026-11-21 核对） */
	const FED_TYPE_CN = {
		trade: '贸易联盟',
		research: '研究合作组织',
		martial: '军事同盟',
		hegemony: '霸权',
		imperial_loyalists: '帝制派',
	}
	/** `has_federation_type = X` 里的类型键 → 界面名（同上，引擎实测） */
	const FED_TYPE_BY_KEY = {
		default: '星系联盟',
		trade: '贸易联盟',
		research: '研究合作组织',
		military: '军事同盟',
		hegemony: '霸权',
		spiritualist: '神圣契约',
		imperial_loyalists: '帝制派',
	}
	const fedTypeHint = (key) => {
		const m = /_(trade|research|martial|hegemony|imperial_loyalists)(_|$)/.exec(String(key || ''))
		return m ? `（${FED_TYPE_CN[m[1]]}专用）` : ''
	}
	/** 中文按两格算，清单才排得齐 */
	const dispWidth = (s) =>
		[...String(s)].reduce(
			(n, ch) => n + (/[\u1100-\u115f\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe30-\ufe4f\uff00-\uff60\uffe0-\uffe6]/.test(ch) ? 2 : 1),
			0,
		)
	const padName = (s, w = 16) => s + ' '.repeat(Math.max(1, w - dispWidth(s)))

	/* —— 法律的 potential / 中央集权门槛：三值判定（2026-11-21）——
	   potential 的词表很小（全库扫过只有这几个）：
	     has_federation_type / has_federation_flag / has_federation_law / host_has_dlc / always
	   外面套 NOT / AND / OR / NAND。判不了的**一律 unknown，绝不当 true**。
	   5 个旗标里 4 个能从「联邦类型 + 当前法律」推出来（第 5 个 no_federation_fleet_build_right_cooldown
	   在脚本里只有 remove、没有 set，保留为判不了）。 */
	const EVAL3_AND = (a, b) => (a === 'false' || b === 'false' ? 'false' : a === 'unknown' || b === 'unknown' ? 'unknown' : 'true')
	const EVAL3_OR = (a, b) => (a === 'true' || b === 'true' ? 'true' : a === 'unknown' || b === 'unknown' ? 'unknown' : 'false')
	const EVAL3_NOT = (a) => (a === 'unknown' ? 'unknown' : a === 'true' ? 'false' : 'true')
	/** 把 potential 体切成 [{key, value}] / [{key, block}]，只认这两种写法 */
	const splitLawClauses = (body) => {
		const out = []
		// 键要连大写一起认：`NOT = { … }` 里的 NOT 全大写，只写 [a-z_]+ 会把整层否定吃掉，
		// 「NOT = { has_federation_flag = special_federation }」就会被读成「需要 special_federation」（2026-11-21 踩过）
		const re = /([A-Za-z_]+)\s*=\s*/g
		let m
		while ((m = re.exec(body))) {
			const key = m[1]
			const at = re.lastIndex
			if (body[at] === '{') {
				const inner = braceBlock(body, at)
				if (inner === null) break
				out.push({ key, block: inner })
				re.lastIndex = at + inner.length + 2
			} else {
				const vm = /^[^\s}]+/.exec(body.slice(at))
				out.push({ key, value: vm ? vm[0] : '' })
			}
		}
		return out
	}
	const evalLawClause = (c, facts) => {
		if (c.block !== undefined) {
			const vals = splitLawClauses(c.block).map((x) => evalLawClause(x, facts))
			const all = vals.reduce(EVAL3_AND, 'true')
			if (c.key === 'NOT' || c.key === 'NAND') return EVAL3_NOT(all)
			if (c.key === 'AND') return all
			if (c.key === 'OR') return vals.reduce(EVAL3_OR, 'false')
			return 'unknown'
		}
		const v = c.value
		switch (c.key) {
			case 'always':
				return v === 'yes' ? 'true' : v === 'no' ? 'false' : 'unknown'
			case 'has_federation_type':
				return facts.fedType ? (v === facts.fedType ? 'true' : 'false') : 'unknown'
			case 'has_federation_law':
				return facts.laws ? (facts.laws.has(v) ? 'true' : 'false') : 'unknown'
			case 'has_federation_flag':
				if (!facts.flags || !facts.flagKnown?.has(v)) return 'unknown'
				return facts.flags.has(v) ? 'true' : 'false'
			case 'host_has_dlc':
				return 'true' // 本机全 DLC（2026-11-21 核对）
			default:
				return 'unknown'
		}
	}
	const evalLawPotential = (body, facts) => {
		if (!body) return 'true'
		return splitLawClauses(body).reduce((acc, c) => EVAL3_AND(acc, evalLawClause(c, facts)), 'true')
	}
	/** 第一条不成立的子句翻成人话；否定式要翻过来（`NOT = { X }` 不成立 ⇔ X 成立） */
	const explainLawClause = (c, facts, negated) => {
		if (c.block !== undefined) {
			const isNot = c.key === 'NOT' || c.key === 'NAND'
			const underNot = negated !== isNot
			const inner = splitLawClauses(c.block)
			if (isNot) {
				// 否定块不成立 = 里面（总体上）成立：拿第一条「为真」的来说明
				for (const ic of inner) {
					if (evalLawClause(ic, facts) === 'true') return explainLawClause(ic, facts, underNot)
				}
			} else {
				for (const ic of inner) {
					if (evalLawClause(ic, facts) !== 'true') return explainLawClause(ic, facts, underNot)
				}
			}
			return inner.length ? explainLawClause(inner[0], facts, underNot) : ''
		}
		const v = c.value
		const cn = (k) => FED_TYPE_BY_KEY[String(k).replace(/_federation$/, '')] || k
		switch (c.key) {
			case 'always':
				return v === 'no' ? 'always = no（这一档永远不可选）' : ''
			case 'has_federation_type':
				return negated ? `不适用于${cn(v)}` : `只有${cn(v)}`
			case 'has_federation_law':
				return negated ? `不能是 ${v}（当前正是它）` : `需要当前是 ${v}`
			case 'has_federation_flag':
				return negated ? `不能带旗标 ${v}` : `需要旗标 ${v}`
			case 'host_has_dlc':
				return negated ? '' : '需要 DLC'
			default:
				return `判不了（${c.key}）`
		}
	}
	const lawPotentialHint = (body, facts) => {
		if (!body) return ''
		for (const c of splitLawClauses(body)) {
			if (evalLawClause(c, facts) !== 'true') return explainLawClause(c, facts, false)
		}
		return ''
	}
	/**
	 * 生效后会不会给本类上冷却（2026-11-21）——这是 `enable_federation_cooldowns` / `*_no_cooldown`
	 * 那几个旗标唯一的作用，正好拿来预测：
	 *   if = { limit = { has_federation_flag = no_xxx_cooldown }        ← 豁免分支（清旗标）
	 *          remove_federation_flag = no_xxx_cooldown }
	 *   else_if = { limit = { has_federation_flag = enable_federation_cooldowns }
	 *               add_modifier = { modifier = federation_law_category_<类>_cooldown days = @cooldown } }
	 * 链上第一条成立的 if 说了算。返回 { category, days } / { none: true } / { unknown: true } / null。
	 * 注意：这是按文档顺序走「if/else_if 链」的近似，够用于现行脚本（冷却分支总在链首）。
	 */
	const lawCooldownAfterEnact = (d, facts) => {
		if (!d?.onEnact) return null
		const seq = []
		const re = /\b(else_)?if\s*=\s*\{/g
		let m
		while ((m = re.exec(d.onEnact))) {
			const blk = braceBlock(d.onEnact, d.onEnact.indexOf('{', m.index))
			if (!blk) continue
			const limAt = /limit\s*=\s*\{/.exec(blk)
			const limit = limAt ? braceBlock(blk, blk.indexOf('{', limAt.index)) : null
			const cd = /modifier\s*=\s*federation_law_category_([a-z0-9_]+)_cooldown/.exec(blk)
			const daysM = /days\s*=\s*@?([A-Za-z0-9_]+)/.exec(blk)
			const removes = /remove_federation_flag\s*=\s*([a-z0-9_]+)/.exec(blk)
			seq.push({
				limit,
				category: cd ? cd[1] : null,
				removes: removes ? removes[1] : null,
				days: daysM ? (/^\d+$/.test(daysM[1]) ? Number(daysM[1]) : d.cooldownDefault) : d.cooldownDefault,
			})
		}
		for (const x of seq) {
			const st = x.limit ? evalLawPotential(x.limit, facts) : 'true'
			if (st === 'unknown') return { unknown: true }
			if (st !== 'true') continue
			if (x.category) return { category: x.category, days: x.days }
			if (x.removes && /_cooldown$/.test(x.removes)) return { none: true }
		}
		return null
	}
	/** 4 个旗标从「类型 + 当前法律」推出来（脚本里每个类型的 set/remove 成对） */
	const deriveFederationFlags = (fedType, laws) => {
		const flags = new Set()
		const flagKnown = new Set([
			'special_federation',
			'enable_federation_cooldowns',
			'challenge_type_no_cooldown',
			'no_federation_fleet',
		])
		if (fedType === 'imperial_loyalists_federation') flags.add('special_federation')
		if (fedType && fedType !== 'imperial_loyalists_federation') flags.add('enable_federation_cooldowns')
		if (laws?.has('succession_type_challenge')) flags.add('challenge_type_no_cooldown')
		if (laws?.has('fleet_contribution_none')) flags.add('no_federation_fleet')
		return { flags, flagKnown }
	}

	add({
		name: 'stellaris_diplo_vote',
		description:
			'Cast OUR vote on a pending diplomatic vote — a federation vote on a peace offer, a war declaration, ' +
			'or on kicking a member out of our federation ' +
			'(bridge get_pending_votes → vote_diplo_action → CDiploActionVoteCommand, id +0x20 / voter +0x28 / ' +
			'vote +0x2C — the same offsets as the SDK). ' +
			'With no arguments it only lists what is up for a vote; pass agree: true / false to vote. ' +
			'ANY pending proposal shows up: the five known kinds (COfferPeaceAction 0x23B6B20, CDeclareWarAction ' +
			'0x23B9EB0, kick 0x23B73A0, federation-law 0x23B7050, federation-type 0x23B7548) get a name — law and ' +
			'type votes also get a localised label (「联邦集权 → 高（centralization_high）」, category from ' +
			'common/federation_laws, names from the engine), with the raw key kept in brackets so it can be copied ' +
			'straight into stellaris_propose_federation_law. Everything else is listed as an unknown action class with its ' +
			'vtable RVA — measured 2026-11-21 on a federation-association offer (「给予合作国地位」), whose id read ' +
			'from +0x30/+0x34 still worked with the vote command. The proposal lives in the PROPOSER ' +
			'country\'s +0x1980 (proposed_actions); the voting member\'s own ' +
			'incoming_actions and proposed_actions are both empty (measured 2026-11-20), so the bridge scans every ' +
			'country for a pending action and reports holder_country + the id. Only our own vote is cast — ' +
			'the other members are AI and vote by themselves; with the AI turned off nobody else ever votes, ' +
			'so the vote just sits until its deadline and is auto-rejected. vote: 1 = 同意, 0 = 拒绝 (neutral is just "not voted"). ' +
			'Measured 2026-11-20: once every member had voted yes, the engine forwarded the offer to the opponent as a ' +
			'NEW incoming action (new id, waiting_for_vote=no) — the war ends when that side accepts it. ' +
			'A war DECLARATION is different: once every member votes yes the war simply starts — the target ' +
			'gets no proposal to accept or refuse (measured 2026-11-20: country 0 declared on 7 and the war appeared ' +
			'immediately after member 1 voted yes).',
		parameters: {
			type: 'object',
			properties: {
				agree: { type: 'boolean', description: 'true = 同意 / false = 拒绝；省略则只列出来' },
				id: { type: 'number', description: '要投票的 id（省略则用第一条）' },
				dry_run: { type: 'boolean', description: '只跑 IsValid，不投递' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const pending = await readPendingVotes(pipe)
				if (pending.error) return text(`bridge: ${pending.error}`)
				if (!pending.items.length) return text('现在没有待处理的提案（proposed_actions 为空）')
				// 法律表决要「类别 → 值」、类型表决要类型名：都问引擎本地化（键本身也留着，方便照抄去发起）
				const lawVotes = pending.items.filter((x) => x.kind === 'vote_for_federation_law' && x.law_key)
				const typeVotes = pending.items.filter((x) => x.kind === 'vote_for_federation_type')
				const lawMap = lawVotes.length ? await loadFederationLawMap() : null
				const names = await localizeKeys(pipe, [
					...lawVotes.flatMap((x) => [x.law_key, lawMap?.get(x.law_key)]),
					...typeVotes.map((x) => x.federation_type_key),
				])
				const lawLabel = (key) => {
					if (!key) return '?'
					const cat = lawMap?.get(key)
					return cat ? `${locName(names, cat)} → ${locName(names, key)}` : locName(names, key)
				}
				const lines = [`待处理 ${pending.count} 条（记录在提案方的 proposed_actions 里）`]
				for (const it of pending.items) {
					const o = it.offer ?? {}
					const what = it.known === false
						? `未识别的动作类（vtable 0x${Number(it.vtable_rva ?? 0).toString(16)}）` +
							` · country ${it.actor} → ${o.recipient}` +
							`${it.id_candidate ? '' : '（读不到 id）'}`
						: it.kind === 'declare_war'
							? `宣战 · 理由 ${it.goal_key || '?'} · 目标 ${o.recipient}`
							: it.kind === 'kick_from_federation'
								? `踢出联邦 · 被踢的是 country ${o.recipient}`
								: it.kind === 'vote_for_federation_law'
									? `联邦法律表决 · ${lawLabel(it.law_key)}（${it.law_key || '?'}）`
									: it.kind === 'vote_for_federation_type'
										? `联邦类型表决 · ${locName(names, it.federation_type_key)}（${it.federation_type_key || '?'}）`
										: `求和 · ${o.status_name ?? '?'} · war ${o.war}`
					lines.push(`  id ${it.id_candidate}（tag ${it.tag} / serial ${it.serial}）｜提案方 country ${it.holder_country}｜${what}`)
				}
				if (args?.agree === undefined) {
					lines.push('', '要投票就带 agree: true（同意）/ false（拒绝），并**把 id 写上**')
					return text(lines.join('\n'))
				}
				// 有多条待表决时**必须指定 id**：默认取第一条会在不知不觉中把票投给别的提案
				// （2026-11-21 实测：漏了 id，一票投给别的记录，宣战提案永远差一票、战争没出现）
				if (args?.id === undefined && pending.items.length > 1) {
					return text(
						`现在有 ${pending.items.length} 条待表决，请用 id 指定要投哪一条（不指定会把票投给第一条，很容易投错）`,
					)
				}
				const pick =
					args?.id !== undefined
						? pending.items.find((x) => Number(x.id_candidate) === Number(args.id))
						: pending.items[0]
				if (!pick) return text(`没有 id ${args.id} 的待表决提案`)
				const r = await rpc(pipe, 'vote_diplo_action', {
					id: Number(pick.id_candidate),
					vote: args.agree === true ? 1 : 0,
					dry_run: args?.dry_run === true,
				})
				if (r?.dry_run === true) {
					return text(
						`校验：${args.agree === true ? '同意' : '拒绝'} id ${pick.id_candidate}\n` +
							`  ${r?.is_valid === true ? '✓ 通过' : '✗ ' + String(r?.reason ?? '').replace(/\n+/g, ' ').trim()}`,
					)
				}
				if (r?.success === true && r?.posted === true) {
					return text(
						`已投出我们这一票（${args.agree === true ? '同意' : '拒绝'}）id ${pick.id_candidate}｜voter ${r?.voter}`,
					)
				}
				return text(`投票失败：${String(r?.reason ?? r?.error ?? JSON.stringify(r)).replace(/\n+/g, ' ').trim()}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 发起联邦法律表决（桥 propose_federation_law，2026-11-21 打通）——
	   引擎没有「提交法律提案」这条命令；桥是手工摆一个 CVoteForFederationLawAction（法律对象在 +0x50），
	   再包成普通的外交提案投出去，落进我们自己的 proposed_actions，由成员表决（法律多数、类型全体）。
	   这一层补的是「名字」与「哪个值属于哪一类」：前者问引擎本地化，后者只存在于 common/federation_laws。 */
	add({
		name: 'stellaris_propose_federation_law',
		description:
			'Propose a federation law change (bridge propose_federation_law). The engine has no "submit law" command, ' +
			'so the bridge hand-builds CVoteForFederationLawAction (law object at +0x50) and posts it as an ordinary ' +
			'diplo-action proposal: it lands in OUR proposed_actions, the members vote (law changes: majority; type ' +
			'changes are a different action and need unanimity) and the new value is written when the vote settles. ' +
			'Names and the value-to-category mapping are added here: the mapping is parsed from ' +
			'common/federation_laws/*.txt (each file is one category and every value block carries ' +
			'has_any_federation_law_in_category = law_category_* — memory has no such link, measured 2026-11-21), and ' +
			'the display names come from the engine localisation, so the output matches the UI wording ' +
			'(联邦集权 → 高). With list: true it prints the whole catalogue grouped by category (16 categories / 73 ' +
			'values) and posts nothing; values that only exist for one federation type are marked. ' +
			'dry_run only runs IsValid. Measured 2026-11-21: free_migration_yes posted and the save then showed ' +
			'自由移民 = 启用.',
		parameters: {
			type: 'object',
			properties: {
				law_key: { type: 'string', description: '值键，如 free_migration_yes / centralization_high（list: true 时不用给）' },
				list: { type: 'boolean', description: 'true = 只列「类别 → 可选值」，不投递' },
				dry_run: { type: 'boolean', description: '只跑 IsValid，不投递' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const lawMap = await loadFederationLawMap()
				if (args?.list === true) {
					const db = await rpc(pipe, 'probe_law_db', {})
					if (!db || db.success === false) return text(`bridge: ${db?.error ?? 'probe_law_db 没有返回'}`)
					const cats = (db.law_categories ?? []).map((x) => x.key)
					const vals = (db.laws ?? []).map((x) => x.key)
					if (!cats.length || !vals.length) return text('桥没有列出法律库（probe_law_db 空）')
					const names = await localizeKeys(pipe, [...cats, ...vals])
					const byCat = new Map(cats.map((c) => [c, []]))
					const orphans = []
					for (const v of vals) {
						const c = lawMap.get(v)
						if (c && byCat.has(c)) byCat.get(c).push(v)
						else orphans.push(v)
					}
					const lines = [
						`联邦法律：${cats.length} 类 / ${vals.length} 档` +
							`（名字取自引擎本地化；归属取自 common/federation_laws${lawMap.size ? '' : '——没读到脚本，归属可能缺'}）`,
					]
					for (const [cat, list] of byCat) {
						lines.push('', `  ${locName(names, cat)}（${cat}）`)
						for (const v of list) lines.push(`    ${padName(locName(names, v) + fedTypeHint(v))}${v}`)
					}
					if (orphans.length) {
						lines.push('', '  未归类的值（脚本里没读到类别引用）')
						for (const v of orphans) lines.push(`    ${padName(locName(names, v) + fedTypeHint(v))}${v}`)
					}
					lines.push(
						'',
						'投的时候只给 law_key；带「（<类> 联邦专用）」的档只在那种联邦里有意义。',
						'投完用 stellaris_diplo_vote 看表决（法律多数通过；票通过后引擎直接改数据，不产生命令）。',
					)
					return text(lines.join('\n'))
				}
				const key = args?.law_key ? String(args.law_key) : ''
				if (!key) return text('要给 law_key；不知道有哪些就先带 list: true')
				const cat = lawMap.get(key)
				const names = await localizeKeys(pipe, [key, cat])
				const label = cat ? `${locName(names, cat)} → ${locName(names, key)}` : locName(names, key)
				// 引擎没这条本地化时 label 就是键本身，别写成「键（键）」
				const shown = label === key ? key : `${label}（${key}）`
				const r = await rpc(pipe, 'propose_federation_law', {
					law_key: key,
					...(args?.dry_run === true ? { dry_run: true } : {}),
				})
				if (r?.dry_run === true) {
					const why = String(r?.reason ?? '').replace(/\n+/g, ' ').trim()
					return text(
						`校验：${shown}\n  ${r?.success === true ? '✓ IsValid 通过' : '✗ ' + (why || '引擎没给原因')}`,
					)
				}
				if (r?.success !== true) {
					const why = String(r?.error ?? r?.reason ?? JSON.stringify(r)).replace(/\n+/g, ' ').trim()
					return text(
						`投递失败：${shown}\n  ${why}\n` +
							(why.includes('未知法律键') ? '先用 list: true 看有哪些键' : ''),
					)
				}
				return text(
					`已发起：${shown}\n` +
						'表决落在我们的 proposed_actions 里。用 stellaris_diplo_vote 看票数、投我们那一票；' +
						'票通过后引擎直接改数据（不产生命令），要看结果用 federation_report.py 读存档。',
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 贸易协定（桥 propose_trade_deal，2026-11-21 抓现场后接线）——
	   引擎的生产者能造 action_offer_trade_deal，而且**已经把内嵌 CTradeDeal 与两侧的 26 格资源表都分配好**
	   （dry_run 实测：两侧 country/recipient 已填好、四张表 size=26/cap=26 全 0），所以只需按索引写数值、
	   设 length（单位是**年**）与发起方的 reserve，再包 propose 投递——与附庸条款那次「先抓现场再构造」同路。
	   资源索引 = common/strategic_resources/00_strategic_resources.txt 的顶层声明序
	   （energy=0 / minerals=1 / … / consumer_goods=9 / alloys=10 / … / entropy_crystals=25），这边的工具层解析。 */
	const RES_ORDER_CACHE = { map: null }
	const loadResourceOrder = async () => {
		if (RES_ORDER_CACHE.map) return RES_ORDER_CACHE.map
		const map = new Map()
		const dir = await findGameDir()
		if (!dir) return map
		let txt = ''
		try {
			txt = await fs.readFile(path.join(dir, 'common', 'strategic_resources', '00_strategic_resources.txt'), 'utf8')
		} catch {
			return map
		}
		const re = /^([a-z_0-9]+)\s*=\s*\{/gm
		let m
		let idx = 0
		while ((m = re.exec(txt))) {
			map.set(m[1], idx)
			idx += 1
		}
		RES_ORDER_CACHE.map = map
		return map
	}

	add({
		name: 'stellaris_trade_deal',
		description:
			'Offer a resource trade deal to one empire (bridge propose_trade_deal). The engine producer builds ' +
			'action_offer_trade_deal WITH the embedded CTradeDeal and all four 26-slot resource tables already ' +
			'allocated (measured 2026-11-21 with a UI-sent sample: the action at +0x50 holds two sides with ' +
			'country/recipient filled and size=26/cap=26 tables), so this only writes the amounts, the duration and the ' +
			'reserve flag, then posts. Resource tables are 26 fixed i64 slots in 1e5 fixed point, indexed by the ' +
			'DECLARATION ORDER of common/strategic_resources/00_strategic_resources.txt (energy=0, minerals=1, food=2, ' +
			'research x3, influence=6, unity=7, trade=8, consumer_goods=9, alloys=10, ... entropy_crystals=25); the tool ' +
			'parses that file so callers pass plain resource keys. The duration field (+0x249) is in YEARS (a 10-year ' +
			'deal reads 10). One-off amounts are escrowed the moment the offer is sent and returned if the other side ' +
			'refuses — that is what the reserve byte on the proposer side means. give/take are what WE give / what we ' +
			'ASK for, monthly_* are the per-month parts. dry_run builds the action, reports the table pointers and does ' +
			'not write or post. Measured 2026-11-21: alloys 150 + 25/month for energy 250 + 30/month posted, and the ' +
			'save showed exactly those numbers with reserve only on our side.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: '对方国家（裸 id）' },
				give: { type: 'object', description: '我们给的一次性资源，如 {"alloys": 150}' },
				monthly_give: { type: 'object', description: '我们给的月供，如 {"alloys": 25}' },
				take: { type: 'object', description: '向对方要的一次性资源，如 {"energy": 250}' },
				monthly_take: { type: 'object', description: '向对方要的月供，如 {"energy": 30}' },
				years: { type: 'number', description: '协定年限（单位：年，默认 10；界面最小 10）' },
				sensor_charts: { type: 'boolean', description: '通信同步（把我们看到的星图/情报共享给对方，侧 +0x104）' },
				sensor_link: { type: 'boolean', description: '主动传感链接（共享传感器数据，侧 +0x105）' },
				fleets: {
					type: 'array',
					items: { type: 'number' },
					description: '借出的舰队 id（get_military_fleets 的 fleet_id）；只有宗主对附属国可用，且是有期限的（years）',
				},
				specimens: {
					type: 'array',
					items: { type: 'string' },
					description: '给的标本键（如 chambered_clay_pot，见 common/specimens/specimens.txt）；顺序就是 UI 顺序',
				},
				dry_run: { type: 'boolean', description: 'true = 只造动作并回读表格状态，不写不投' },
			},
			required: ['country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.country_id)
			if (!Number.isFinite(id)) return text('country_id is required')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const order = await loadResourceOrder()
				if (!order.size) return text('读不到 common/strategic_resources/00_strategic_resources.txt（资源索引表）')
				const toPairs = (obj, label) => {
					const out = []
					for (const [key, amount] of Object.entries(obj || {})) {
						const idx = order.get(key)
						if (idx === undefined) return { error: `${label}：未知资源键 ${key}` }
						const n = Number(amount)
						if (!Number.isFinite(n)) return { error: `${label}：${key} 的数量不是数字` }
						out.push([idx, n])
					}
					return { pairs: out }
				}
				const give = toPairs(args?.give, '我们给')
				if (give.error) return text(give.error)
				const monthlyGive = toPairs(args?.monthly_give, '我们月供')
				if (monthlyGive.error) return text(monthlyGive.error)
				const take = toPairs(args?.take, '向对方要')
				if (take.error) return text(take.error)
				const monthlyTake = toPairs(args?.monthly_take, '向对方月供')
				if (monthlyTake.error) return text(monthlyTake.error)
				const fleetIds = Array.isArray(args?.fleets) ? args.fleets.map(Number).filter(Number.isFinite) : []
				const specimenKeys = Array.isArray(args?.specimens) ? args.specimens.filter((s) => typeof s === 'string') : []
				const hasSensor = args?.sensor_charts === true || args?.sensor_link === true
				if (![give, monthlyGive, take, monthlyTake].some((x) => x.pairs.length) &&
					!fleetIds.length && !specimenKeys.length && !hasSensor) {
					return text('空交易会被引擎拒（「交易不能为空！」）——至少给点资源、标本、舰队或通信/传感中的一样')
				}
				const years = args?.years !== undefined ? Number(args.years) : 10
				const sensors = {
					...(args?.sensor_charts === true ? { sensor_charts: true } : {}),
					...(args?.sensor_link === true ? { sensor_link: true } : {}),
				}
				const r = await rpc(pipe, 'propose_trade_deal', {
					country_id: id,
					give: give.pairs,
					monthly_give: monthlyGive.pairs,
					take: take.pairs,
					monthly_take: monthlyTake.pairs,
					years,
					...sensors,
					...(specimenKeys.length ? { specimens: specimenKeys } : {}),
					...(fleetIds.length ? { fleets: fleetIds } : {}),
					...(args?.dry_run === true ? { dry_run: true } : {}),
				})
				if (r?.success === false) return text(`发起失败：${String(r?.error ?? JSON.stringify(r)).replace(/\n+/g, ' ').trim()}`)
				const fmt = (pairs) => (pairs.length ? pairs.map(([i, n]) => `${[...order].find(([, v]) => v === i)?.[0] ?? i}=${n}`).join(' + ') : '—')
				const sensorText = [
					args?.sensor_charts === true ? '通信同步' : '',
					args?.sensor_link === true ? '主动传感链接' : '',
				].filter(Boolean).join(' / ')
				const lines = [
					`贸易协定 → country ${id}｜${years} 年`,
					`  我们给：${fmt(give.pairs)}${monthlyGive.pairs.length ? `（每月 ${fmt(monthlyGive.pairs)}）` : ''}`,
					`  向对方要：${fmt(take.pairs)}${monthlyTake.pairs.length ? `（每月 ${fmt(monthlyTake.pairs)}）` : ''}`,
				]
				if (sensorText) lines.push(`  附带：${sensorText}`)
				if (fleetIds.length) lines.push(`  借出舰队：${fleetIds.join(' / ')}（期限 ${years} 年）`)
				if (specimenKeys.length) lines.push(`  标本：${specimenKeys.join(' / ')}`)
				if (r?.dry_run === true) {
					const state = (s) => `${s?.resources?.table === '空' ? '表空' : `表 ${s?.resources?.size}/${s?.resources?.cap}`}`
					lines.push('', `dry_run：动作已造、未写未投｜我方 ${state(r?.first)}｜对方 ${state(r?.second)}`)
					return text(lines.join('\n'))
				}
				if (r?.posted !== true) return text(`${lines.join('\n')}\n未投递：${String(r?.error ?? r?.note ?? '未知原因')}`)
				lines.push('', '已递交：表决前给出的一次性资源会被扣住，对方拒绝则退回；接受后按月结算')
				return text(lines.join('\n'))
			} catch (e) {
				return text(`bridge: ${String(e?.message ?? e)}`)
			}
		},
	})

	/* —— 宣战（桥 declare_war，2026-11-20 打通）——
	   用引擎自己的生产者 NDiplomacyUtil::CreateDiplomaticAction 造 CDeclareWarAction——
	   它对 action_declare_war **能造**（对 action_offer_peace 返回 null，那是求和必须自己组的原因），
	   造出来的对象已带好 vtable/动作类型/内嵌 CWarGoal/空 extra_attacker 与 actor/recipient，
	   所以只需换 +0x58 的战争目标类型，再包进 propose 命令。**不需要抓模板。**
	   投出去的是提案：落在我们自己的 proposed_actions，对方答应后才出现战争。 */
	add({
		name: 'stellaris_declare_war',
		description:
			'Declare war on one empire (bridge declare_war). It builds a CDeclareWarAction with the engine\'s own ' +
			'NDiplomacyUtil::CreateDiplomaticAction — that producer DOES build action_declare_war (it returns null only ' +
			'for action_offer_peace, which is why peace had to be synthesised) — then patches the embedded CWarGoal\'s ' +
			'type pointer (+0x58) from goal_key and wraps it in CDiploActionProposeCommand. No UI template needed. ' +
			'The declaration lands in OUR proposed_actions as a pending proposal and a war only appears once the other ' +
			'side answers it. goal_key is required (the producer\'s default goal is not a usable casus belli): ' +
			'wg_subjugation = 要求附庸, wg_conquest, wg_humiliation … dry_run only runs IsValid. Measured 2026-11-20: ' +
			'valid for country 7 and 4 with wg_subjugation; country 4 + wg_conquest returns an empty reason (that goal ' +
			'is simply not offered there); posting to country 7 added one proposed_actions entry and no war. ' +
			'With list_goals: true it only lists the war goals that target actually accepts (bridge probe_war_goals: ' +
			'one IsValid per CWarGoalType, names from war_goal_<key>) — run that first when unsure which goal works.',
		parameters: {
			type: 'object',
			properties: {
				country_id: { type: 'number', description: 'target country id（裸 id）' },
				goal_key: { type: 'string', description: 'war goal key：wg_subjugation = 要求附庸、wg_tribute = 要求朝贡…（list_goals 时不用给）' },
				list_goals: { type: 'boolean', description: '只列出这个国家**可选的**战争目标（逐个问引擎，带中文名）' },
				dry_run: { type: 'boolean', description: '只跑 IsValid，不投递' },
			},
			required: ['country_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const target = Number(args?.country_id)
			if (!Number.isFinite(target)) return text('country_id is required（目标国家，裸 id）')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				if (args?.list_goals === true) {
					const g = await rpc(pipe, 'probe_war_goals', { country_id: target, all: false })
					if (g?.success !== true) return text(`bridge: ${g?.error ?? 'probe_war_goals 失败'}`)
					const items = Array.isArray(g?.items) ? g.items : []
					if (!items.length) return text(`对 country ${target}：没有任何可用的战争目标（共 ${g?.total ?? '?'} 种都不过）`)
					const lines = [`对 country ${target} 可用的战争目标 ${g?.valid_count} 个（共 ${g?.total} 种）`]
					for (const it of items) lines.push(`  ✓ ${it.name ?? it.key}（${it.key}）`)
					lines.push('', '要宣战就带 goal_key 再跑一次（可先加 dry_run）')
					return text(lines.join('\n'))
				}
				if (!args?.goal_key) return text('goal_key is required（先用 list_goals: true 看这个国家接受哪些）')
				const r = await rpc(pipe, 'declare_war', {
					country_id: target,
					goal_key: String(args?.goal_key ?? ''),
					dry_run: args?.dry_run === true,
				})
				if (r?.dry_run === true) {
					return text(
						`校验：宣战 → country ${target}（${r?.goal_key || '沿用模板的理由'}，只跑 IsValid）\n` +
							`  ${r?.is_valid === true ? '✓ 通过' : '✗ ' + String(r?.reason ?? '').replace(/\n+/g, ' ').trim()}` +
							(r?.is_valid === true ? '\n去掉 dry_run 就会投出提案（落在我们自己的 proposed_actions，等联邦表决；票过即开战）' : ''),
					)
				}
				if (r?.success === true && r?.posted === true) {
					// 投完之后看实际结果：独立国立刻开战；联邦成员会先起投票（2026-11-21 实测两种都见过）
					let tail = ''
					try {
						const [w, pv, me] = [
							await rpc(pipe, 'get_wars', {}),
							await rpc(pipe, 'get_pending_votes', {}),
							await rpc(pipe, 'get_player_country', {}),
						]
						const myId = Number(me?.country_id)
						const atWar = (Array.isArray(w?.wars) ? w.wars : []).some((x) => {
							const g = x?.attacker_war_goal ?? {}
							return x?.ended !== true && Number(g.actor_id) === myId && (Number(g.target_id) & 0xffffff) === target
						})
						const pending = (Array.isArray(pv?.items) ? pv.items : []).find(
							(it) => it?.kind === 'declare_war' && (Number(it?.offer?.recipient) & 0xffffff) === target,
						)
						if (atWar) tail = '\n→ **已经开战了**（独立国不经表决，投出即生效）'
						else if (pending) tail = `\n→ 已进联邦表决：id ${pending.id_candidate}，票过即开战（投票时务必带这个 id）`
						else tail = '\n→ 战争没有出现、也没有待表决记录：换个理由再试（`wg_subjugation` 最稳）'
					} catch {
						// 读不到就只报投递成功，不猜
					}
					return text(`已投出宣战：→ country ${target}（${r?.goal_key || '?'}）${tail}`)
				}
				return text(`宣战失败：${String(r?.reason ?? r?.error ?? JSON.stringify(r)).replace(/\n+/g, ' ').trim()}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 第一次接触（桥 get_first_contacts，2026-11-19 接）——
	   阶段用本项目实测的换算：界面「阶段 N」= stage_key 里的数 + 1（stage_1 → 阶段2）。
	   剩余 = days_left ÷ 100000；地点是 location 指向的星系。技能不输出：它是国家常数，不进对象。 */
	const fcStageNumber = (key) => {
		const m = /_stage_(\d+)$/.exec(String(key ?? ''))
		return m ? Number(m[1]) + 1 : null
	}
	const fcStageText = (key) => {
		const k = String(key ?? '')
		if (!k) return '?'
		const n = fcStageNumber(k)
		if (n !== null) return `阶段${n}`
		if (k.includes('_end_stage')) return '结束阶段'
		return k
	}
	const fcDays = (v) => {
		const n = Number(v)
		if (!Number.isFinite(n)) return '?'
		return (n / 100000).toFixed(1).replace(/\.0$/, '')
	}
	const fcHasEnvoy = (v) => v != null && Number(v) !== 4294967295
	// 只有「status 0 且有特使」这一种是本局实测过的「进行中」（桑达尔、萨斯维尔）。
	// status 2 的也会挂着特使（已结束、特使没撤），所以不能拿特使单独判进行。
	const fcState = (r) => {
		const s = Number(r.status)
		if (s === 0) return fcHasEnvoy(r.envoy_id) ? '进行' : '待办'
		return `status ${Number.isFinite(s) ? s : '?'}`
	}

	add({
		name: 'stellaris_first_contacts',
		description:
			'第一次接触（读 CFirstContact 表）。不给 country_id 时列一览：对方、地点、阶段、线索、剩余天数、特使；' +
			'给了 country_id 只出那一条的详情，多列阶段难度、stage_key、记录 id/槽。' +
			'阶段＝界面阶段号（stage_key 里的数＋1）；剩余＝days_left÷100000；地点＝location 指向的星系；' +
			'status 0＝待办或自己正在跑（未锁定）；status 1＝一个阶段跑完、锁定等待处理；status 2＝已建立联络（完成），这一档仍挂着特使引用，别拿特使单独判；status 3＝对方完成（我方中止）。' +
			'status：0 进行/待办、1 过渡、2 已建立联络、3 未定。' +
			'技能不输出——它是国家常数，各国不同，接触对象里没有这个字段。' +
			'all: true 连别国的接触一起列（这张表是全银河的，每对国家两条相反记录）。',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: "also list other countries' first contacts" },
				country_id: { type: 'number', description: "only this counterparty's contact(s), in detail" },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const want = Number(args?.country_id)
				const detail = Number.isFinite(want)
				const all = detail ? true : args?.all === true
				const data = await rpc(config.pipe || DEFAULTS.pipe, 'get_first_contacts', { all })
				let rows = Array.isArray(data?.contacts) ? [...data.contacts] : []
				if (detail) {
					const mine = rows.filter((r) => r.owner_is_player && Number(r.country_id) === want)
					rows = mine.length ? mine : rows.filter((r) => Number(r.country_id) === want)
				}
				if (!rows.length) return text(detail ? `没有 country ${want} 的第一次接触记录` : '没有第一次接触记录')

				if (detail) {
					const out = []
					if (!rows.some((r) => r.owner_is_player)) out.push('（我方没有这条，下面是其它国家的记录）')
					for (const r of rows) {
						const owner = r.owner_is_player ? '我方发起' : `country ${r.owner_id} 发起`
						const state = fcState(r)
						out.push(`${r.country_name || `country ${r.country_id}`}（country ${r.country_id}，${owner}）`)
						out.push(`  地点      ${r.system_name || '—'}`)
						out.push(`  阶段      ${fcStageText(r.stage_key)}   ← ${r.stage_key || '?'}`)
						out.push(`  线索      ${r.clues}`)
						out.push(`  阶段难度  ${r.difficulty}   （阶段自带；界面＝这个＋对方加密）`)
						out.push(`  剩余      ${fcDays(r.days_left)} 天   （days_left ${r.days_left}）`)
						// status 2（已建立联络）上留着的特使是结束没清的引用，不能当「在派人」看
						const envoyText = !fcHasEnvoy(r.envoy_id)
							? '—'
							: Number(r.status) === 0
								? String(r.envoy_id)
								: `${r.envoy_id}（已结束，引用残留）`
						out.push(`  特使      ${envoyText}`)
						out.push(`  status    ${r.status}${state === '进行' || state === '待办' ? `（${state}）` : ''}`)
						out.push(`  记录      id ${r.id} · 槽 ${r.slot} · 发起方 ${r.owner_id}`)
						out.push('')
					}
					return text(out.join('\n').trimEnd())
				}

				rows.sort((a, b) =>
					Number(fcHasEnvoy(b.envoy_id)) - Number(fcHasEnvoy(a.envoy_id)) ||
					(fcStageNumber(b.stage_key) ?? 0) - (fcStageNumber(a.stage_key) ?? 0) ||
					String(a.country_name || '').localeCompare(String(b.country_name || ''), 'zh'))

				const running = rows.filter((r) => fcState(r) === '进行').length
				const waiting = rows.filter((r) => fcState(r) === '待办').length
				const lines = [
					`${all ? '全部' : '我方'}第一次接触 ${rows.length} 条：进行 ${running} · 待办 ${waiting} · 其它 ${rows.length - running - waiting}`,
					'',
				]
				for (const r of rows) {
					const flag = fcState(r)
					lines.push(
						`  ${flag}  ${r.country_name || `country ${r.country_id}`}` +
						`｜${fcStageText(r.stage_key)}｜线索 ${r.clues}｜剩余 ${fcDays(r.days_left)} 天` +
						`｜特使 ${Number(r.status) === 0 && fcHasEnvoy(r.envoy_id) ? r.envoy_id : '—'}` +
						`｜${r.system_name || '—'}｜country ${r.country_id}`
					)
				}
				lines.push('', '阶段＝界面阶段号（键里的数＋1）；剩余＝days_left÷100000；地点＝location 指向的星系')
				lines.push(`口径＝当前玩家国家（country ${data?.player_country_id ?? '?'}；用 play 换国家后会跟着变）`)
				lines.push('status：0＝待办或自己正在跑（未锁定）；1＝一个阶段跑完、锁定等待处理')
				lines.push('        2＝已建立联络（完成，特使那列不显示）；3＝对方完成（我方中止）')
				lines.push('要看某一条的细节（阶段难度、stage_key、记录 id/槽）就带上 country_id')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 派特使（2026-11-19 实测定案）——
	   桥的 assign_envoy 走 CAssignLeaderCommand + CLeaderLocation{type 8, assignment 4, id=contact id}。
	   两个坑：① leader 字段要带高位标记的引用（桥内部自己从 CLeader+0x20 读），② 引擎 IsValid 不校验职业，
	   科学官也会通过、行政官被拒时 reason 还是空的 —— 所以这里先自己确认 class == envoy。 */
	add({
		name: 'stellaris_assign_envoy',
		description:
			'把一名特使派到某条第一次接触上（原生 CAssignLeaderCommand；接触 id 从 stellaris_first_contacts 的详情里拿）。' +
			'不给 leader_id 时自动挑一名空闲特使（class = envoy 且 unassigned）。' +
			'引擎的 IsValid 不校验职业、有时还不给原因，所以这里先自己拦一次，只让特使过去。' +
			'dry_run: true 只校验不派。派完会回读那条接触记的特使 id。',
		parameters: {
			type: 'object',
			properties: {
				contact_id: { type: 'number', description: 'first contact record id (see stellaris_first_contacts)' },
				leader_id: { type: 'number', description: 'envoy leader id, tagged or bare; omit to pick a free envoy' },
				dry_run: { type: 'boolean', description: 'only validate, do not post' },
				reset_cooldown: { type: 'boolean', description: 'skip the engine cooldown ("time 360内无法移动"); testing only' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const contactId = Number(args?.contact_id)
				if (!Number.isFinite(contactId)) return text('contact_id is required（先用 stellaris_first_contacts 看详情里的「记录 id」）')

				const slot = (id) => Number(id) & 0xffffff
				const leaders = await rpc(pipe, 'get_leaders')
				const hired = Array.isArray(leaders?.hired_leaders) ? leaders.hired_leaders : []
				if (!hired.length) return text('读不到已雇领袖（get_leaders 返回空）')

				let target = null
				const want = Number(args?.leader_id)
				if (Number.isFinite(want)) {
					target = hired.find((l) => slot(l.id) === slot(want)) || null
					if (!target) return text(`没有 id 为 ${slot(want)} 的已雇领袖`)
				} else {
					target = hired.find((l) => l.class === 'envoy' && l.assignment_type === 'unassigned') || null
					if (!target) return text('没有空闲的特使（class = envoy 且 unassigned）——先看看是不是都派出去了')
				}

				if (target.class !== 'envoy') {
					return text(`${target.name} 是「${target.class_name || target.class}」，不是特使。引擎的 IsValid 会放行，这里拦下`)
				}

				const res = await rpc(pipe, 'assign_envoy', {
					leader_id: slot(target.id),
					contact_id: contactId,
					dry_run: args?.dry_run === true,
					reset_cooldown: args?.reset_cooldown === true,
				})
				const lines = [`特使 ${target.name}（leader ${slot(target.id)}，id_ref ${target.id}）→ 接触 ${contactId}`]
				if (res?.can_assign === false) {
					lines.push(`引擎拒绝：${res.reason ? res.reason : '（没有给原因）'}`)
					return text(lines.join('\n'))
				}
				if (args?.dry_run === true) {
					lines.push('dry_run：只校验，没有派')
					return text(lines.join('\n'))
				}
				if (res?.posted !== true) {
					lines.push(`没有派上：${res?.error || JSON.stringify(res)}`)
					return text(lines.join('\n'))
				}

				const after = await rpc(pipe, 'get_first_contacts', { all: false })
				const row = (after?.contacts || []).find((c) => Number(c.id) === contactId)
				const now = row ? Number(row.envoy_id) : 4294967295
				lines.push(`已派。接触现在记的特使：${now === 4294967295 ? '（无，可能要吃一个 tick）' : now}`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* ---- 帝国资源（2026-11-22）----
	   get_status 里什么都有，但一次要读整包 JSON（contacts / council / fleets / outliner …）。
	   玩的时候最常问的只是「合金/影响力/星界丝缕还剩多少、谁满了」，所以单独给一张小表：
	   一行一项（名字本地化、库存/上限、月净），抬头标出满仓与负增长的资源。 */
	add({
		name: 'stellaris_resources',
		description:
			'Your empire\'s resources in one compact read (bridge get_status → resources): stockpile, max + capped ' +
			'where a cap exists, and the monthly net. The header calls out which resources are capped (being wasted) and ' +
			'which are negative. only:["alloys"] asks about a single resource. Names are localised through the engine. ' +
			'Read-only.',
		parameters: {
			type: 'object',
			properties: {
				only: { type: 'array', items: { type: 'string' }, description: 'resource keys to show, e.g. ["astral_threads", "alloys"]' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const d = await rpc(pipe, 'get_status')
				if (d?.error) return text(`bridge: ${d.error}`)
				if (d?.in_game === false) return text('游戏在菜单里——载入存档后才有日期与资源')
				const all = d?.resources && typeof d.resources === 'object' ? d.resources : {}
				const askOnly = Array.isArray(args?.only) && args.only.length > 0
				const keys = askOnly ? args.only.map(String) : Object.keys(all)
				let names = {}
				try {
					const loc = await rpc(pipe, 'localize', { keys })
					names = loc?.names ?? {}
				} catch {}
				const label = (k) => (names[k] && names[k] !== k ? names[k] : k)
				const num = (v) => Math.round(Number(v ?? 0) * 100) / 100
				const lines = []
				if (!askOnly) {
					const capped = Object.keys(all).filter((k) => all[k]?.capped === true)
					const negative = Object.keys(all).filter((k) => Number(all[k]?.net ?? 0) < 0)
					lines.push(`帝国资源 ${Object.keys(all).length} 项（${d?.date?.formatted ?? '?'}${d?.is_paused ? '，暂停' : ''}）`)
					lines.push(`  满仓 ${capped.length} 项${capped.length ? `：${capped.map(label).join(' / ')}` : ''}`)
					lines.push(`  负增长 ${negative.length} 项${negative.length ? `：${negative.map(label).join(' / ')}` : ''}`)
					lines.push('')
				}
				for (const k of keys) {
					const r = all[k]
					if (!r) {
						lines.push(`  ${k}：没有这一项`)
						continue
					}
					const bits = [`${padName(label(k), 12)}${num(r.stockpile)}`]
					if (r.max !== undefined && r.max !== null) bits.push(`/ ${num(r.max)} 上限`)
					bits.push(`▸ ${Number(r.net ?? 0) >= 0 ? '+' : ''}${num(r.net)}`)
					if (r.capped === true) bits.push('★满')
					lines.push(`  ${bits.join(' ')}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 银河市场：读 + 买卖（即时 / 月度）2026-11-21 接线 ----
	   读：桥 get_market（市场对象在 g_CurrentGameState+0xAF8，2026-11-21 修正 0xAF0 → 0xAF8，
	       否则月度交易一律读到 0 条）。
	   写：即时买卖走 market_buy_resource_command / market_sell_resource_command；
	       月度挂单/撤单走 add_monthly_trade_command / remove_monthly_trade_command（撤单要 order_id）。 */
	add({
		name: 'stellaris_market',
		description:
			'Galactic market: fee, tradable resources with buy/sell unit prices, and YOUR monthly orders ' +
			'(order_id, buy/sell, resource, amount, max unit price). Read-only; use stellaris_market_trade for ' +
			'instant trades and stellaris_market_monthly for recurring ones. Bridged from get_market.',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const d = await rpc(pipe, 'get_market')
				if (d?.error) return text(`市场：${d.error}`)
				const fee = Number(d?.market_fee_percent ?? 0)
				const lines = [
					`银河市场${d?.is_galactic_market ? '' : '（尚未成立银河市场）'}　手续费 ${fee}%　结算用 ${d?.settlement_currency ?? 'trade'}`,
				]
				const res = Array.isArray(d?.resources) ? d.resources : []
				lines.push('', `可交易 ${res.length} 种（买价 / 卖价 为单位价，批量为一次下单量）`)
				for (const r of res) {
					lines.push(
						`  ${padName(String(r.localized_name ?? r.key), 10)}买 ${r.buy_price_per_unit}　卖 ${r.sell_price_per_unit}` +
							`　批量 ${r.batch_size}　库存 ${Math.round(Number(r.stockpile ?? 0))}`,
					)
				}
				const orders = Array.isArray(d?.monthly_trades) ? d.monthly_trades : []
				lines.push('', `我的月度交易 ${orders.length} 笔`)
				for (const o of orders) {
					const side = o.action === 'buy' ? '买' : o.action === 'sell' ? '卖' : String(o.action)
					const cap = Number(o.max_unit_price) > 0 ? `，限价 ${o.max_unit_price}` : ''
					lines.push(`  #${o.order_id}　${side} ${o.amount} ${o.localized_name ?? o.resource}${cap}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text('市场：' + error.message)
			}
		},
	})

	add({
		name: 'stellaris_market_trade',
		description:
			'Buy or sell a resource on the galactic market IMMEDIATELY (bridge market_trade → ' +
			'market_buy_resource_command / market_sell_resource_command). units is the amount traded, not the ' +
			'number of batches; the engine prices it and applies the market fee. ' +
			'BATCH ROUNDING: a partial batch is rounded up to a whole batch (minerals = 100/批, alloys = 25/批), ' +
			'so units 10 for minerals actually trades 100 — check batch_size in stellaris_market first. ' +
			'For a recurring order use stellaris_market_monthly.',
		parameters: {
			type: 'object',
			properties: {
				resource: { type: 'string', description: '资源键，如 energy / minerals / alloys / rare_crystals' },
				action: { type: 'string', enum: ['buy', 'sell'], description: '买 / 卖' },
				units: { type: 'number', description: '数量（不是批数）' },
			},
			required: ['resource', 'action', 'units'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const resource = String(args?.resource ?? '')
			const action = String(args?.action ?? '')
			const units = Number(args?.units)
			if (!resource) return text('resource is required')
			if (action !== 'buy' && action !== 'sell') return text('action must be buy or sell')
			if (!Number.isFinite(units) || units <= 0) return text('units must be a positive number')
			try {
				const r = await rpc(pipe, 'market_trade', { resource, action, units })
				const head = `${action === 'buy' ? '买入' : '卖出'} ${units} ${resource}`
				// 桥的回执里带了「实际成交多少、几批」，比只回一行有用：不足一批按一批算。
				return text(r?.success ? `${head} → ${r.message}` : `${head} 失败：${String(r?.message ?? '引擎没有给原因')}`)
			} catch (error) {
				return text('市场交易：' + error.message)
			}
		},
	})

	add({
		name: 'stellaris_market_monthly',
		description:
			'Create or cancel a MONTHLY market order (bridge set_monthly_trade → add_monthly_trade_command / ' +
			'remove_monthly_trade_command). Create: give resource + action + amount (+ optional price_limit, the max ' +
			'unit price; 0 = no limit). Cancel: give cancel:true + order_id (from stellaris_market) — the bridge ' +
			'copies the live order because the engine only removes an exact match.',
		parameters: {
			type: 'object',
			properties: {
				resource: { type: 'string', description: '资源键（撤单时不用给）' },
				action: { type: 'string', enum: ['buy', 'sell'], description: '买 / 卖（撤单时不用给）' },
				amount: { type: 'number', description: '每月数量' },
				price_limit: { type: 'number', description: '单位限价（0 = 不限）' },
				cancel: { type: 'boolean', description: 'true = 撤销 order_id 那条' },
				order_id: { type: 'number', description: '要撤销的月度交易 id（stellaris_market 里看得到）' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const cancel = args?.cancel === true
			const params = { cancel }
			if (cancel) {
				const oid = Number(args?.order_id)
				if (!Number.isFinite(oid)) return text('撤单要给 order_id（stellaris_market 里能看到）')
				params.order_id = oid
			} else {
				const resource = String(args?.resource ?? '')
				const action = String(args?.action ?? '')
				const amount = Number(args?.amount)
				if (!resource) return text('resource is required')
				if (action !== 'buy' && action !== 'sell') return text('action must be buy or sell')
				if (!Number.isFinite(amount) || amount <= 0) return text('amount must be a positive number')
				params.resource = resource
				params.action = action
				params.amount = amount
				params.price_limit = Number(args?.price_limit ?? 0)
			}
			try {
				const r = await rpc(pipe, 'set_monthly_trade', params)
				return text(r?.success ? `已执行：${r.message}` : `失败：${String(r?.message ?? '引擎没有给原因')}`)
			} catch (error) {
				return text('月度交易：' + error.message)
			}
		},
	})

	bridgeTool('stellaris_sectors', 'get_sectors', 'Sectors and their colonies.')
	bridgeTool('stellaris_armies', 'get_armies', 'Garrison and transport armies of the empire.')

	/* —— 动作（后端方法在，但插件层还没跑过，属未验证） —— */
	bridgeTool('stellaris_resolve_event', 'resolve_event',
		'Pick an option in a modal event window and dismiss it.',
		{
			window_id: { type: 'number', description: 'event window id' },
			option_index: { type: 'number', description: '0-based option index' },
		})
	bridgeTool('stellaris_open_notification', 'open_notification',
		'Left-click a top-bar notification badge to open its modal.',
		{ index: { type: 'number', description: 'badge index' } })
	bridgeTool('stellaris_open_alert', 'open_alert',
		'Click a top-bar alert banner to open its view.',
		{ alert_id: { type: 'number', description: 'alert id from stellaris_alerts' } })
	/* 注意：桥的这个方法参数叫 area，不是 slot —— 这里做一次映射 */
	const AREA_INDEX = { physics: 0, society: 1, engineering: 2 }

	const areaOf = (args) => {
		const a = args?.area ?? args?.slot
		if (typeof a === 'number') return a
		if (typeof a === 'string') {
			if (a in AREA_INDEX) return AREA_INDEX[a]
			const n = Number(a)
			if (Number.isFinite(n)) return n
		}
		return 0
	}

	/* 科研状态：三个领域各自的候选与当前项目 */
	add({
		name: 'stellaris_research_state',
		description:
			'Research state: for each area (physics / society / engineering) the current project and the candidate list with keys, names, tier and cost. Use the keys with stellaris_select_research.',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			try {
				const answer = await pipeCall(config.pipe || DEFAULTS.pipe, 'get_research_state', {})
				const state = answer?.result ?? answer
				const lines = []
				for (const key of ['physics', 'society', 'engineering']) {
					const area = state?.[key]
					if (!area) continue
					const cur = area.current || {}
					// 槽被特殊项目占用时只有 is_researching=true、key/name 为空。
					// 具体是哪个项目读不出来（槽里指向的是项目定义，不是实例），而且多个项目可能同时
					// status=1，所以这里只说“被占用”，不猜名字——要查是哪个，用 stellaris_special_projects。
					let what = '（空）'
					if (cur.is_researching) {
						what = cur.name
							? `${cur.name}（${cur.key}，${cur.cost}）`
							: '被特殊项目占用（是哪个项目见 stellaris_special_projects 的 status=1 条目）'
					}
					lines.push(`${key}：${what}`)
					for (const c of area.candidates || []) {
						lines.push(`    ${c.name}  ${c.key}  T${c.tier}  ${c.cost}`)
					}
					lines.push('')
				}
				return text(lines.join('\n').trim() || '（没有读到科研状态）')
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_select_research',
		description: 'Assign a technology to a research slot (physics | society | engineering).',
		parameters: {
			type: 'object',
			properties: {
				slot: { type: 'string', description: 'physics | society | engineering' },
				tech_key: { type: 'string', description: 'technology key from get_research_state' },
			},
			required: ['slot', 'tech_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const answer = await pipeCall(config.pipe || DEFAULTS.pipe, 'select_research', {
					area: areaOf(args),
					tech_key: args?.tech_key,
				})
				return text(JSON.stringify(answer?.result ?? answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_cancel_research',
		description:
			'Cancel the technology being researched in a slot. The slot goes empty and the tech returns to the candidate pool with its progress kept. An empty slot is not wasteful: that field keeps banking research points, and once you pick a project they are released at double speed until the stockpile runs out.',
		parameters: {
			type: 'object',
			properties: {
				slot: { type: 'string', description: 'physics | society | engineering' },
			},
			required: ['slot'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const answer = await pipeCall(config.pipe || DEFAULTS.pipe, 'cancel_research', { area: areaOf(args) })
				return text(JSON.stringify(answer?.result ?? answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	bridgeTool('stellaris_adopt_tradition', 'adopt_tradition',
		'Adopt a tradition tree or unlock a tradition perk.',
		{ tradition_key: { type: 'string', description: 'tradition key' } })
	bridgeTool('stellaris_choose_ascension_perk', 'activate_ascension_perk',
		'Pick an ascension perk by key (bridge activate_ascension_perk, CActivateAscensionPerkCommand). This is the validated path: with no free slot the engine refuses and says why ("需要空的飞升天赋槽位…"); one perk per finished tradition tree. perk keys live in common/ascension_perks/00_*.txt (e.g. ap_world_shaper, ap_one_vision). Do not use the console `effect add_ascension_perk` for this — it bypasses the slot check and lets you take duplicates.',
		{ perk: { type: 'string', description: 'ascension perk key, e.g. ap_world_shaper' } })
	bridgeTool('stellaris_toggle_edict', 'toggle_edict',
		'Turn an edict on or off.',
		{
			edict_key: { type: 'string', description: 'edict key' },
			enabled: { type: 'boolean', description: 'true to enable' },
		})
	/* —— 雇 / 解（2026-11-20 接成正式工具）——
	   hire → CHireLeaderCommand；dismiss → CFireLeaderCommand(0x2EB3)。
	   两个都先把「这个人是谁」查出来，失败时把引擎原话带回去。 */
	add({
		name: 'stellaris_hire_leader',
		description:
			'Hire one leader from the recruitment pool (bridge hire_leader → CHireLeaderCommand). candidate_id comes from ' +
			'stellaris_leaders → the 招募池 section. The tool first looks the candidate up so the reply names them, then posts ' +
			'and reports the engine\'s reason verbatim when it refuses. Costs energy credits / unity as the game prices it.',
		parameters: {
			type: 'object',
			properties: { candidate_id: { type: 'number', description: 'candidate id from stellaris_leaders (the 招募池 list)' } },
			required: ['candidate_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.candidate_id)
			if (!Number.isFinite(id)) return text('candidate_id is required（stellaris_leaders 的招募池 id）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				let who = ''
				try {
					const ls = await rpc(pipe, 'get_leaders', {})
					const c = (ls?.pool_candidates ?? []).find((x) => Number(x.id) === id)
					if (c) who = `${c.name ?? id}（${c.class_name ?? c.class ?? '?'} lv${c.level ?? '?'}）`
				} catch {}
				const r = await rpc(pipe, 'hire_leader', { candidate_id: id })
				if (r?.error) return text(`雇佣失败：${r.error.message || JSON.stringify(r.error)}`)
				return text(`已雇佣：${who || r?.name || id}${r?.class ? `（${r.class}）` : ''}\n${JSON.stringify(r)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_dismiss_leader',
		description:
			'Dismiss a hired leader (bridge dismiss_leader → CFireLeaderCommand, token 0x2EB3). This is PERMANENT — the leader ' +
			'is gone, not returned to the pool. Refusals measured 2026-11-20: the ruler ("Cannot dismiss the ruler of the ' +
			'empire", blocked by the bridge) and a leader exploring an astral rift ("无法解雇正在探索星界裂隙的领袖", refused by ' +
			'the engine). A successful dismissal is reported by the bridge; the tool names the leader before posting.',
		parameters: {
			type: 'object',
			properties: { leader_id: { type: 'number', description: 'leader id from stellaris_leaders' } },
			required: ['leader_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.leader_id)
			if (!Number.isFinite(id)) return text('leader_id is required（stellaris_leaders 的 id）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				let who = ''
				try {
					const ls = await rpc(pipe, 'get_leaders', {})
					const l = (ls?.hired_leaders ?? []).find((x) => Number(x.id) === id)
					if (l) who = `${l.name ?? id}（${l.class_name ?? l.class ?? '?'} lv${l.level ?? '?'}）`
				} catch {}
				const r = await rpc(pipe, 'dismiss_leader', { leader_id: id })
				if (r?.error) return text(`解雇失败：${r.error.message || JSON.stringify(r.error)}`)
				return text(`已解雇：${who || id}\n${JSON.stringify(r)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_assign_leader',
		description:
			'Assign a hired leader to a council seat, a fleet (ship command), a planet (governor), an army, or an envoy post. ' +
			'The bridge posts CAssignLeaderCommand (token 0x40AD, size 0x38: leader ref at +0x20, a 16-byte CLeaderLocation at ' +
			'+0x24 {type, assignment, id, position}). slot decides the location type and what target_id means: ' +
			'ship = FLEET id (the bridge resolves FleetLeaderShip), governor = PLANET id or COLONY id (a colony id is tried ' +
			'first — a nomad ark colony is a ship and has no CPlanet::colony link), ' +
			'army = army id, council = seat id from stellaris_leader_assignments, envoy = COUNTRY id plus task ' +
			'(improve_relations / harm_relations / spy_network / patron_attunement / galactic_community / ' +
			'strengthen_imperial_authority / undermine_imperial_authority). The engine validates on post and its reason is ' +
			'returned verbatim; when it refuses without one the bridge says so instead of inventing a cause. ' +
			'GESTALT EMPIRES: replacing a sitting council member is refused by this tool even though the engine would accept ' +
			'it (measured 2026-11-20: a level-2 scientist displaced a level-9 councilor; the UI does not allow that, so the ' +
			'tool blocks it to keep the save clean). ' +
			'Measured 2026-11-20 — the first-contact envoy path used the same command successfully, but this slot-based entry ' +
			'had never been run; the old wiring sent slot_type instead of slot, so it always failed.',
		parameters: {
			type: 'object',
			properties: {
				leader_id: { type: 'number', description: 'leader id (stellaris_leaders)' },
				slot: { type: 'string', enum: ['council', 'ship', 'governor', 'army', 'envoy'], description: 'where to send them' },
				target_id: { type: 'number', description: 'fleet id / planet id / army id / seat id / country id, depending on slot' },
				task: { type: 'string', description: 'envoy only: improve_relations | harm_relations | spy_network | patron_attunement | federation | galactic_community | strengthen_imperial_authority | undermine_imperial_authority（galactic_community / federation 要行政官 official，其余要特使 envoy）' },
			},
			required: ['leader_id', 'slot', 'target_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const leaderId = Number(args?.leader_id)
			const targetId = Number(args?.target_id)
			const slot = String(args?.slot ?? '')
			if (!Number.isFinite(leaderId)) return text('leader_id is required（stellaris_leaders 的 id）')
			if (!Number.isFinite(targetId)) return text('target_id is required（按 slot 给舰队/行星/陆军/席位/国家 id）')
			if (!slot) return text('slot is required：council | ship | governor | army | envoy')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				// 格式塔的内阁席位禁止替换（玩家 2026-11-20 定）：引擎的 CAssignLeaderCommand 会照收，
				// 实测能把 9 级意识节点顶成 unassigned，但界面不允许，强行换可能留下坏状态。
				if (slot === 'council') {
					let gestalt = false
					try {
						const gov = await rpc(pipe, 'get_government', {})
						const auth = String(gov?.authority ?? '')
						const ethics = (gov?.ethics ?? []).map((e) => e.key ?? e)
						gestalt = auth === 'auth_machine_intelligence' || auth === 'auth_hive_mind' || ethics.includes('ethic_gestalt_consciousness')
					} catch {
						// 读不到政体就放行，让引擎回答
					}
					if (gestalt) {
						let holder = null
						try {
							const ls = await rpc(pipe, 'get_leaders', {})
							holder = (ls?.hired_leaders ?? []).find(
								(l) => Number(l?.location?.council_seat_id) === targetId && Number(l.id) !== leaderId,
							)
						} catch {}
						if (holder) {
							return text(
								`拒绝：席位 ${targetId} 现在是「${holder.name ?? holder.id}」（id ${holder.id}）。\n` +
									'格式塔的内阁席位不允许替换——引擎会收下这条命令，但界面里做不到，强行换可能留下坏状态。',
							)
						}
					}
				}
				const r = await rpc(pipe, 'assign_leader', {
					leader_id: leaderId,
					slot,
					target_id: targetId,
					task: args?.task ? String(args.task) : '',
				})
				if (r?.error) {
					const msg = r.error.message || JSON.stringify(r.error)
					// 使者有指派冷却（虚境调和那档的说明写着 360 天内不能再指派），而引擎拒绝这类情况时不吐原因
					const hint = slot === 'envoy' && /without a reason/i.test(msg)
						? '\n（使者有指派冷却——虚境调和那档的本地化写着 360 天内无法重新指派；引擎遇到这类情况不给原因。' +
							'刚才这次没有生效，位置没变。）'
						: ''
					return text(`派任失败：${msg}${hint}`)
				}
				return text(
					`已投递：领袖 ${leaderId} → ${slot} ${targetId}` +
						(args?.task ? `（task ${args.task}）` : '') +
						'\n引擎会在执行命令时再校验一次；拒绝原因原样带回\n' +
						JSON.stringify(r),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* 改权利：先按 get_species 的目录校验类别与取值，并把这一类的冷却讲清楚。
	   一类一改、改完就进冷却（本档实测 2202 天，九类各自独立），所以默认只印计划，
	   confirm:true 才投；桥自己还会再跑一遍 potential/allow 与冷却的检查。 */
	add({
		name: 'stellaris_set_species_rights',
		description:
			'Set one species right (bridge set_species_rights { species_id, category, right_value }). category is one of the nine ' +
			'categories the bridge lists (citizenship, living_standards, military_service, slavery_type, purge_type, ' +
			'population_controls, colonization_controls, migration_controls, subspecies_integration); right_value must be one of the ' +
			'keys listed for that category in get_species.available_rights_catalog. The tool refuses a value the catalogue does not ' +
			'have and a category still on cooldown, prints current -> new, and **nothing is posted unless confirm:true**. Changing one ' +
			'category starts that category\'s own cooldown (the cooldowns differ per category — 2202 and 3600 days both seen in ' +
			'this save — and the nine are independent). The bridge itself re-checks the right\'s potential/allow before posting, ' +
			'so a refusal may still come from the engine.',
		parameters: {
			type: 'object',
			properties: {
				species_id: { type: 'number', description: 'species id (see stellaris_species)' },
				category: { type: 'string', description: 'right category, e.g. living_standards' },
				right_value: { type: 'string', description: 'right value key from that category, e.g. living_standard_utopian' },
				confirm: { type: 'boolean', description: 'post the change (otherwise only the plan is printed)' },
			},
			required: ['species_id', 'category', 'right_value'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const id = Number(args?.species_id)
				const category = typeof args?.category === 'string' ? args.category : ''
				const value = typeof args?.right_value === 'string' ? args.right_value : ''
				if (!Number.isFinite(id) || !category || !value) return text('需要 species_id、category、right_value')
				const one = await rpc(pipe, 'get_species', { species_id: id })
				const s = Array.isArray(one?.species) ? one.species[0] : null
				if (!s) return text(`没有物种 ${id}`)
				const r = s.rights || {}
				const catalog = one?.available_rights_catalog || {}
				const entries = Array.isArray(catalog[category]) ? catalog[category] : null
				if (!entries) return text(`没有这一类权利：${category}\n可选类别：${Object.keys(catalog).join('、')}`)
				const pick = entries.find((e) => e.key === value)
				if (!pick) return text(`这一类里没有 ${value}\n这一类可选：${entries.map((e) => `${e.name}（${e.key}）`).join('、')}`)
				const cur = r[category]?.name || '（无）'
				const curKey = r[category]?.key || ''
				const cd = Number((r.cooldowns || {})[category] ?? 0)
				const lines = [
					`${s.name}（${s.species_id}）｜${category}`,
					`现状：${cur}${curKey ? `（${curKey}）` : ''}${r.is_specific ? '' : '｜帝国默认，没有单独配'}`,
				]
				if (cd > 0) return text(`${lines.join('\n')}\n✗ 不投：这一类还在冷却，还要 ${cd} 天`)
				if (curKey === value) return text(`${lines.join('\n')}\n＝ 已经是这个值，不用改`)
				lines.push(`改为：${pick.name}（${pick.key}）`)
				lines.push('代价：改完这一类进冷却（九类各自独立；本档实测 2202 天与 3600 天都见过）')
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真改请加 confirm: true）`)
				const res = await rpc(pipe, 'set_species_rights', { species_id: id, category, right_value: value })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${res?.error || '无原因'}`)
				return text(`${lines.join('\n')}\n已投。用 stellaris_species_detail 复核（这一类应变成新值并带冷却）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	bridgeTool('stellaris_reinforce_fleet', 'reinforce_fleet',
		'Dispatch shipbuilding orders to fill a fleet template deficit.',
		{ fleet_id: { type: 'number', description: 'fleet id' } })
	bridgeTool('stellaris_orbit_planet', 'orbit_planet',
		'Send a fleet to orbit one celestial body. Same system: the order string turns into "正在进入<planet>轨道" at once and back to "无指令" a tick later; cross-system: it shows "正在前往<system>星系" first. Verified 2026-11-19. Orbiting is what orbital bombardment would need too, but the bridge cannot set a bombardment stance. (For anomaly research you do NOT need to orbit first — use stellaris_research_anomaly, which takes the body directly.)',
		{
			fleet_id: { type: 'number', description: 'fleet id (science ships included)' },
			planet_id: { type: 'number', description: 'celestial body id, e.g. from stellaris_anomalies' },
			queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
		})
	add({
		name: 'stellaris_research_anomaly',
		description:
			'Research the anomaly on ONE celestial body with a science ship — just (fleet_id, planet_id), the ids stellaris_anomalies prints. This posts CResearchAnomalyFleetOrderCommand (token 0x2CF0: fleet +0x20 / planet +0x24 / queue +0x28), which names the body outright, so it works whether or not the ship already orbits it and picks the right anomaly when a system has several. Measured 2026-11-19: pointing it at 梅萨特罗斯-BI (4038) while the ship was busy elsewhere switched the order straight to "正在调查梅萨特罗斯-BI" and queued the rest of the system ("+2命令"); and a second ship asked to research the same body is refused by the engine with 「已有另一艘舰船正在执行或已完成此命令。」, which is returned verbatim. The older system-wide command is research_anomalies in the bridge (fleet + system) — it picks whatever the ship orbits, so prefer this one.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'science ship fleet id (see stellaris_civilian_fleets)' },
				planet_id: { type: 'number', description: 'celestial body id, from stellaris_anomalies' },
				queue: { type: 'boolean', description: 'append instead of replacing the fleet queue' },
			},
			required: ['fleet_id', 'planet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const r = await rpc(pipe, 'research_anomaly', {
					fleet_id: args.fleet_id,
					planet_id: args.planet_id,
					queue: args.queue === true,
				})
				// 引擎的拒绝（例如同一异常已有船在做）会把原因放在 error 字符串里，原样转述
				if (!r || r.success === false) return text(`引擎拒绝：${(r && r.error) || '未知原因'}`)
				return text(`已下研究指令：${r.planet || r.planet_id}（fleet ${r.fleet_id}${r.queued ? '，已排队' : ''}）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	bridgeTool('stellaris_set_fleet_template_quota', 'set_fleet_template_quota',
		'Change the target quota of one ship design in a fleet template.',
		{
			fleet_id: { type: 'number', description: 'fleet id' },
			design_id: { type: 'number', description: 'ship design id' },
			target_quota: { type: 'number', description: 'desired ship count' },
		})
	bridgeTool('stellaris_set_situation_approach', 'set_situation_approach',
		'Change the approach of an active situation. The approach_key comes from stellaris_situation_details (its 取向 list); the bridge posts CSetSituationApproachCommand and the engine validates it. Changing an approach is reversible: set it back the same way.',
		{
			situation_id: { type: 'number', description: 'situation id from stellaris_situation_log' },
			approach_key: { type: 'string', description: 'approach key, e.g. deficit_approach_cut_science_investment' },
		})
	/* —— 星界裂隙（2026-11-20）——
	   读 get_astral_rifts（现在带 owner_name）；给 fleet_id 时调 probe_rift_explorability，
	   让引擎逐条回答「这艘船能不能进去」。游牧的规则（无主 / 有研究条约的定居帝国疆域）
	   就落在这一条上：被拒的会明说「✗ 我们在该恒星系的影响力不足…」，其余多半是已经结束的。
	   注意：一旦投递，科学官就锁死（不像考古能随时离开）。 */
	const riftStatus = (s) => ({ 0: '未开始', 1: '探索中' })[Number(s)] ?? `status ${s}`
	const riftLine = (r, v) => {
		const bits = [`id ${r.id} ${r.type || '（剧情未指定）'}`, riftStatus(r.status), `线索 ${r.clues}`]
		if (r.system_name) {
			const who = r.system_owner_name ? r.system_owner_name : '无主'
			bits.push(`星系 ${r.system_name}（${who}）`)
		}
		if (r.owner_name) bits.push(`所属 ${r.owner_name}`)
		if (r.explorer_fleet_id !== undefined) bits.push(`探索船 ${r.explorer_fleet_id}`)
		if (r.leader_id !== undefined && r.explorer_fleet_id === undefined) bits.push(`领袖 ${r.leader_id}（残留）`)
		const reason = (v?.reason ?? '').trim()
		if (v && !v.valid && reason) bits.push(reason)
		const mark = v ? (v.valid ? '✓' : '✗') : '·'
		return `  ${mark} ${bits.join('｜')}`
	}

	add({
		name: 'stellaris_astral_rifts',
		description:
			'All astral rifts in the galaxy, from the CAstralRift table. Each row: id, type key (empty while the rift is ' +
			'unexplored — stage 1 spins idle and the story chain is only assigned at stage 2, so every untouched rift looks ' +
			'identical), status (0 unstarted, 1 being explored; other values are rifts already finished), clues, the country ' +
			'that owns/explored it (owner_name) and the exploring fleet / its leader (leader_id lingers after the dig ends). ' +
			'Pass fleet_id and every rift is also put to the engine: probe_rift_explorability builds one ' +
			'CExploreAstralRiftFleetOrderCommand per rift and only runs IsValid — nothing is posted. Measured 2026-11-20 in this ' +
			'nomad save: of 44 rifts exactly 1 was valid, 12 were refused with 「✗ 我们在该恒星系的影响力不足，无法确保这次探索的安全。」 ' +
			'(a settled empire\'s system, no research agreement) and the rest came back false with no text (already finished). ' +
			'Rifts that another empire has finished cannot be entered again. WARNING: once an exploration order is posted the ' +
			'scientist is locked to that rift — unlike archaeology there is no leaving, so pick the ship first.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'science ship fleet id; ask the engine which rifts it may enter (IsValid only)' },
				rift_id: { type: 'number', description: 'one rift in detail' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const data = await rpc(pipe, 'get_astral_rifts', {})
				if (data?.error) return text(`bridge: ${data.error}`)
				let rifts = Array.isArray(data?.rifts) ? [...data.rifts] : []
				if (!rifts.length) return text('没有星界裂隙')

				const fleetId = Number(args?.fleet_id)
				const probe = Number.isFinite(fleetId) ? await rpc(pipe, 'probe_rift_explorability', { fleet_id: fleetId }) : null
				const verdict = new Map()
				if (Array.isArray(probe?.rows)) for (const row of probe.rows) verdict.set(Number(row.rift_id), row)

				if (args?.raw === true) return text(JSON.stringify({ rifts, probe }, null, 2))

				const wantId = Number(args?.rift_id)
				const detail = Number.isFinite(wantId)
				if (detail) rifts = rifts.filter((r) => Number(r.id) === wantId)
				if (!rifts.length) return text(detail ? `没有 id ${wantId} 的裂隙` : '没有星界裂隙')

				// 有判定时把可探索的排前面，其次按 status、id
				rifts.sort((a, b) => {
					const va = verdict.get(Number(a.id))?.valid ? 0 : 1
					const vb = verdict.get(Number(b.id))?.valid ? 0 : 1
					return va - vb || Number(a.status) - Number(b.status) || Number(a.id) - Number(b.id)
				})

				const lines = []
				if (probe) {
					const ok = rifts.filter((r) => verdict.get(Number(r.id))?.valid).length
					const reasons = new Map()
					for (const r of rifts) {
						const v = verdict.get(Number(r.id))
						if (!v || v.valid) continue
						const why = (v.reason ?? '').trim() || '（引擎没给原因）'
						reasons.set(why, (reasons.get(why) ?? 0) + 1)
					}
					lines.push(`裂隙 ${rifts.length} 处（船 ${fleetId}）：可探索 ${ok} · 拒绝 ${rifts.length - ok}`)
					for (const [why, n] of [...reasons.entries()].sort((a, b) => b[1] - a[1])) lines.push(`    ${n} × ${why}`)
				} else {
					const st = (n) => rifts.filter((r) => Number(r.status) === n).length
					const exploring = rifts.filter((r) => r.explorer_fleet_id !== undefined).length
					lines.push(`裂隙 ${rifts.length} 处：未开始 ${st(0)} · 探索中 ${exploring} · 其它 ${rifts.length - st(0) - exploring}`)
				}
				lines.push('')
				for (const r of rifts) lines.push(riftLine(r, verdict.get(Number(r.id))))
				lines.push('')
				lines.push('能不能进：给 fleet_id 才问引擎（只 IsValid、不投递）——游牧只进「无主」或「有研究条约的定居帝国疆域」')
				lines.push('别国探索完的裂隙进不去；type 空是正常的（第一阶段空转，第二阶段才指定剧情链）')
				lines.push('警告：一旦投递，科学官就锁在这条裂隙上，撤不回来——先挑好船')

				if (detail) {
					const r = rifts[0]
					lines.push('')
					lines.push(`id ${r.id} · status ${r.status} · 线索 ${r.clues} · 事件 ${Array.isArray(r.events) ? r.events.length : 0} 条` +
						`｜所属 ${r.owner_name || '无'}｜探索船 ${r.explorer_fleet_id ?? '—'}｜领袖 ${r.leader_id ?? '—'}`)
					lines.push(`星系 ${r.system_name || '?'}（id ${r.system_id ?? '?'}，${r.system_owner_name || '无主'}）` +
						`← CAstralRift+0xA0 的 origin 星系`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 投递星界裂隙探索 ——
	   桥端 explore_astral_rift：CExploreAstralRiftFleetOrderCommand（fleet +0x20 / astral_rift +0x24 / queue +0x28）。
	   和考古一样先拦非科研船；不一样的是投递之后科学官锁死，所以把话说在返回值里。 */
	add({
		name: 'stellaris_explore_astral_rift',
		description:
			'Send a science ship into ONE astral rift (rift_id from stellaris_astral_rifts). Posts ' +
			'CExploreAstralRiftFleetOrderCommand (token 0x4237: fleet +0x20 / astral_rift +0x24 / queue +0x28). This tool first ' +
			'reads get_civilian_fleets and only lets a science ship through (the engine IsValid does not check the ship class). ' +
			'dry_run: true only validates. IRREVERSIBLE IN A DIFFERENT WAY THAN ARCHAEOLOGY: once the order is posted the ' +
			'scientist is locked to that rift and cannot be freed — there is no "leave the site" here, so choose the ship (and ' +
			'its leader) before posting. Measured 2026-11-20: on this nomad save 43 of 44 rifts were refused, the 12 with ' +
			'「✗ 我们在该恒星系的影响力不足…」 being settled empires\' space without a research agreement.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'science ship fleet id (get_civilian_fleets → science_ships)' },
				rift_id: { type: 'number', description: 'rift id from stellaris_astral_rifts' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing its orders' },
				dry_run: { type: 'boolean', description: 'only run the engine IsValid, do not post' },
			},
			required: ['fleet_id', 'rift_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const fleetId = Number(args?.fleet_id)
			const riftId = Number(args?.rift_id)
			if (!Number.isFinite(fleetId)) return text('fleet_id is required（用 get_civilian_fleets 的 science_ships）')
			if (!Number.isFinite(riftId)) return text('rift_id is required（stellaris_astral_rifts 的 id；0 也是合法值）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				let shipName = ''
				try {
					const fleets = await rpc(pipe, 'get_civilian_fleets', {})
					const ships = Array.isArray(fleets?.science_ships) ? fleets.science_ships : []
					const ship = ships.find((f) => Number(f.fleet_id) === fleetId)
					if (!ship) {
						const ids = ships.map((f) => `${f.fleet_id} ${f.name ?? ''}`.trim()).join('、')
						return text(`fleet ${fleetId} 不是科研船，不投——引擎的 IsValid 不看舰种。现有科研船：${ids || '（无）'}`)
					}
					shipName = ship.name ?? ''
				} catch {
					// 读不到就放行，让引擎回答
				}
				const result = await rpc(pipe, 'explore_astral_rift', {
					fleet_id: fleetId,
					rift_id: riftId,
					queue: args?.queue === true,
					dry_run: args?.dry_run === true,
				})
				if (args?.dry_run === true) {
					if (result?.valid) return text(`可探索：船 ${fleetId}${shipName ? ` ${shipName}` : ''} → 裂隙 ${riftId}（dry run，未投递）`)
					return text(`不可探索：${(result?.reason ?? '').trim() || result?.error || '引擎没给原因'}`)
				}
				if (result?.success === false || result?.error) return text(`投递失败：${result?.error ?? '未知原因'}`)
				return text(
					`已投递：船 ${fleetId}${shipName ? ` ${shipName}` : ''} → 裂隙 ${riftId}\n` +
					`注意：这名科学官从此锁在裂隙 ${riftId} 上，撤不回来\n` +
					JSON.stringify(result),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 星界行动（2026-11-22）——
	   读：桥 get_discoveries {tab:"astral_actions", probe:true} —— 每条都跑一次
	       CActivateAstralActionCommand 的 IsValid（只校验、不投递），所以「能不能用 + 引擎原因」现成。
	   写：桥 activate_astral_action {action_key}（2026-11-22 新加，token 0x4290）。
	   代价与限次在脚本 common/astral_actions/astral_planes_actions.txt（activation_cost / usages）。 */
	const ASTRAL_CACHE = { defs: null, vars: null }
	const ASTRAL_RES_CN = {
		influence: '影响力',
		unity: '凝聚力',
		energy: '能量币',
		minerals: '矿物',
		food: '食物',
		alloys: '合金',
		consumer_goods: '消费品',
		rare_crystals: '稀有水晶',
		exotic_gases: '异星天然气',
		volatile_motes: '挥发性微粒',
		sr_dark_matter: '暗物质',
		sr_living_metal: '活体金属',
		sr_zro: '泽洛',
		minor_artifacts: '稀有文物',
	}
	const loadAstralVars = () => {
		if (ASTRAL_CACHE.vars) return ASTRAL_CACHE.vars
		const vars = new Map()
		for (const root of EDICT_GAME_DIRS) {
			const dir = path.join(root, 'common', 'scripted_variables')
			if (!existsSync(dir)) continue
			for (const file of readdirSync(dir)) {
				if (!file.endsWith('.txt')) continue
				const text = readFileSync(path.join(dir, file), 'utf8')
				for (const m of text.matchAll(/^@([A-Za-z0-9_]+)\s*=\s*(-?\d+)\s*$/gm)) vars.set(m[1], Number(m[2]))
			}
			break
		}
		ASTRAL_CACHE.vars = vars
		return vars
	}
	const loadAstralDefs = () => {
		if (ASTRAL_CACHE.defs) return ASTRAL_CACHE.defs
		const defs = new Map()
		for (const root of EDICT_GAME_DIRS) {
			const full = path.join(root, 'common', 'astral_actions', 'astral_planes_actions.txt')
			if (!existsSync(full)) continue
			const text = readFileSync(full, 'utf8')
			for (const m of text.matchAll(/^([A-Za-z][A-Za-z0-9_]*)\s*=\s*\{/gm)) {
				const body = braceBlock(text, text.indexOf('{', m.index))
				if (body) defs.set(m[1], body)
			}
			break
		}
		ASTRAL_CACHE.defs = defs
		return defs
	}
	const astralMeta = (key) => {
		const body = loadAstralDefs().get(key)
		const out = { costText: null, multiplier: false, uses: null, upgrade: null }
		if (!body) return out
		const acm = body.match(/activation_cost\s*=\s*\{/)
		const acb = acm ? braceBlock(body, body.indexOf('{', acm.index)) ?? '' : ''
		const threads = []
		const others = []
		let pos = 0
		for (let guard = 0; guard < 10; guard += 1) {
			const rest = acb.slice(pos)
			const cm = /cost\s*=\s*\{/.exec(rest)
			if (!cm) break
			const open = pos + rest.indexOf('{', cm.index)
			const cb = braceBlock(acb, open) ?? ''
			pos = open + cb.length + 2
			const mul = /multiplier\s*=\s*([^\n}]+)/.exec(cb)
			if (mul) {
				out.multiplier = true
				const base = /BASE\|(\d+)/.exec(mul[1])
				const inc = /INCREASE\|(\d+)/.exec(mul[1])
				const max = /MAX\|(\d+)/.exec(mul[1])
				out.scaled = {
					base: base ? Number(base[1]) : null,
					inc: inc ? Number(inc[1]) : null,
					max: max ? Number(max[1]) : null,
				}
			}
			const t = /astral_threads\s*=\s*(@?[A-Za-z0-9_]+)/.exec(cb)
			if (t) {
				let v = t[1]
				if (v.startsWith('@')) {
					const resolved = loadAstralVars().get(v.slice(1))
					v = resolved === undefined ? v : resolved
				}
				const n = Number(v)
				if (Number.isFinite(n)) threads.push(n)
			}
			for (const mm of cb.matchAll(/(influence|unity|energy|minerals|food|alloys|consumer_goods|rare_crystals|exotic_gases|volatile_motes|sr_dark_matter|sr_living_metal|sr_zro|minor_artifacts)\s*=\s*(\d+)/g)) {
				others.push(`${ASTRAL_RES_CN[mm[1]] ?? mm[1]} ${mm[2]}`)
			}
		}
		const uniq = [...new Set(threads)]
		if (uniq.length) {
			if (out.scaled && out.scaled.base !== null) {
				out.costText = `${out.scaled.base} 丝缕起${out.scaled.inc ? `，每次 +${out.scaled.inc}` : ''}${out.scaled.max ? `（上限 ${out.scaled.max}）` : ''}`
			} else {
				out.costText = `${uniq.join(' / ')} 丝缕`
			}
		}
		if (others.length) out.costText = `${out.costText ?? '无丝缕'} ＋ ${others.join('、')}`
		const uses = /^\s*usages\s*=\s*(\d+)/m.exec(body)
		const upgrade = /^\s*upgrade\s*=\s*([A-Za-z0-9_]+)/m.exec(body)
		out.uses = uses ? Number(uses[1]) : null
		out.upgrade = upgrade ? upgrade[1] : null
		return out
	}
	const astralClean = (s) => String(s ?? '').trim().replace(/^✗\s*/u, '')
	const astralMetaText = (key) => {
		const m = astralMeta(key)
		const bits = [m.costText ?? '代价见脚本', m.uses ? `限 ${m.uses} 次` : '无限次']
		return bits.join('｜')
	}

	add({
		name: 'stellaris_astral_actions',
		description:
			'The astral actions your empire can see, with the engine\'s own verdict on each. The bridge method ' +
			'get_discoveries {tab:"astral_actions", probe:true} builds one CActivateAstralActionCommand per action and only ' +
			'runs IsValid (nothing is posted), so can_activate and the refusal text (cooldown, uses left, missing ark ' +
			'ship, ...) are the engine\'s. Cost and usage limits are parsed from ' +
			'common/astral_actions/astral_planes_actions.txt (activation_cost in astral threads, usages N = limited, no ' +
			'usages = unlimited). Actions whose potential fails are hidden in the UI and come back can_activate:false with ' +
			'NO reason — this tool hides those too unless all:true. Usage states (unlocked / remaining uses / next usable ' +
			'date) are read straight from CCountry+0x3100, the same fields the save writes as ' +
			'astral_actions_usage_states_array: remaining -1 = unlimited, and the date is the end of the cooldown (it ' +
			'matched the engine\'s own "还需要 N 天" day for day). Measured 2026-11-22: the script has 16 actions, the UI ' +
			'shows 12 (nomadic hides the quantum-catapult pair; having tech_arkship_exodus_jump / tech_hyper_relays hides ' +
			'two base variants), and of the 12 five were activatable. Use stellaris_activate_astral_action to fire one.',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: 'also list the actions hidden by potential (they always fail IsValid with no reason)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const d = await rpc(pipe, 'get_discoveries', { tab: 'astral_actions', probe: true })
				if (d?.error) return text(`bridge: ${d.error}`)
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const actions = Array.isArray(d?.astral_actions?.actions) ? d.astral_actions.actions : []
				if (!actions.length) return text('没有星界行动（数据库为空？）')
				const hidden = actions.filter((a) => !a.can_activate && !a.blocked_reason)
				const visible = actions.filter((a) => a.can_activate || a.blocked_reason)
				const list = args?.all === true ? [...visible, ...hidden] : visible
				// 限次与冷却：桥现在从 CCountry+0x3100 直读（存档里叫 astral_actions_usage_states_array）。
				// 脚本的 usages 是「总共几次」，这里给的是「还剩几次」；-1 = 不限次。
				const astralUsageText = (a) => {
					const bits = []
					if (Number.isInteger(a.remaining_uses)) {
						const cap = astralMeta(a.key).uses
						bits.push(a.remaining_uses < 0 ? '剩余 不限' : `剩余 ${a.remaining_uses}${cap ? `/${cap}` : ''}`)
					}
					if (Number.isInteger(a.cooldown_days_left) && a.cooldown_days_left > 0 && a.next_usable_date) {
						bits.push(`下次可用 ${a.next_usable_date}（${a.cooldown_days_left} 天）`)
					}
					return bits.length ? `｜${bits.join('｜')}` : ''
				}
				const lines = [
					`星界行动 ${visible.length} 条（脚本共 ${actions.length} 条；另有 ${hidden.length} 条未解锁）`,
				]
				for (const a of list) {
					if (!a.can_activate && !a.blocked_reason) {
						const why = a.unlockable === false ? '潜在条件或裂隙数阈值未到' : '引擎没给原因'
						lines.push(`  · ${a.name}（${a.key}）${astralUsageText(a)}｜界面不显示（${why}）`)
						continue
					}
					const mark = a.can_activate ? '✓' : '✗'
					// 引擎的拒绝语里带着「该行动还需要 N 天冷却」与「剩余次数：3/4」，与上面那两列重复，只留别的部分
					const whyText = astralClean(a.blocked_reason)
						.replace(/该行动还需要\d+天冷却。/u, '')
						.replace(/\s*剩余次数：\d+\/\d+/u, '')
						.replace(/\n+/g, ' ')
						.trim()
					const why = a.can_activate || !whyText ? '' : `｜${whyText}`
					lines.push(`  ${mark} ${a.name}（${a.key}）｜${astralMetaText(a.key)}${astralUsageText(a)}${why}`)
				}
				lines.push('')
				lines.push('「剩余 / 下次可用」读自 CCountry+0x3100（存档同名字段 astral_actions_usage_states_array）；剩余 -1 = 不限次')
				lines.push('能不能启动是引擎对 CActivateAstralActionCommand 跑 IsValid 的答案；启动用 stellaris_activate_astral_action')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_activate_astral_action',
		description:
			'Fire ONE astral action. Bridge method activate_astral_action looks the action up in CAstralActionsDatabase and ' +
			'posts CActivateAstralActionCommand (token 0x4290, vtable RVA 0x2398598, 0x30 bytes: astral_action +0x20 object ' +
			'pointer, country +0x28). IsValid runs first, so a refusal comes back with the engine\'s own text — cooldown, ' +
			'uses exhausted, missing requirement, not enough astral threads. dry_run:true only reads the probe and posts ' +
			'nothing. Cost/limit come from the script (activation_cost in astral threads; usages N = limited to N uses, ' +
			'no usages = unlimited). Measured 2026-11-22: action_astral_boost fired successfully (its cost is scaled — 500 base ' +
			'+ 50 per use; this call actually spent 700 threads, 3800 → 3100 — and the action then reported a 1800-day ' +
			'cooldown); folding_hyperspace and dimensional_shifting were refused as already used ("该行动只能使用一次").',
		parameters: {
			type: 'object',
			properties: {
				action_key: { type: 'string', description: 'action key from stellaris_astral_actions, e.g. action_astral_boost' },
				dry_run: { type: 'boolean', description: 'only report whether the engine would allow it; post nothing' },
			},
			required: ['action_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const key = String(args?.action_key ?? '').trim()
			if (!key) return text('action_key is required（先用 stellaris_astral_actions 看键）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				if (args?.dry_run === true) {
					const d = await rpc(pipe, 'get_discoveries', { tab: 'astral_actions', probe: true })
					if (d?.error) return text(`bridge: ${d.error}`)
					const a = (d?.astral_actions?.actions ?? []).find((x) => String(x?.key) === key)
					if (!a) return text(`没有 ${key} 这条星界行动`)
					if (a.can_activate) return text(`可启动：${a.name}（${key}）｜${astralMetaText(key)}（dry run，未投递）`)
					const why = astralClean(a.blocked_reason).replace(/\n+/g, ' ')
					return text(`不可启动：${a.name}（${key}）｜${why || 'potential 未过（界面不显示）'}（dry run，未投递）`)
				}
				const r = await rpc(pipe, 'activate_astral_action', { action_key: key })
				if (r?.success === false) return text(`启动失败：${r?.message ?? '未知原因'}`)
				return text(`${r?.message ?? `已启动 ${key}`}\n（${astralMetaText(key)}；能不能用、扣没扣丝缕，过一 tick 用 stellaris_resources 核对）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 文物行动（Artifact Actions / 稀有文物）：读 + 启动（2026-11-22）——
	   桥 get_artifact_actions {probe:true} 对每条构建 CArtifactActionCommand 只跑 IsValid；
	   代价从 common/artifact_actions/artifact_actions.txt 的 cost 解析（usually minor_artifacts）。 */
	const ARTIFACT_COST_CACHE = { map: null }
	const loadArtifactCosts = () => {
		if (ARTIFACT_COST_CACHE.map) return ARTIFACT_COST_CACHE.map
		const map = new Map()
		for (const root of EDICT_GAME_DIRS) {
			const full = path.join(root, 'common', 'artifact_actions', 'artifact_actions.txt')
			if (!existsSync(full)) continue
			const text = readFileSync(full, 'utf8')
			for (const m of text.matchAll(/^([A-Za-z][A-Za-z0-9_]*)\s*=\s*\{/gm)) {
				const body = braceBlock(text, text.indexOf('{', m.index))
				if (!body) continue
				const costBlock = /cost\s*=\s*\{([^}]*)\}/.exec(body)
				const cost = costBlock ? /([a-z_]+)\s*=\s*(\d+)/.exec(costBlock[1]) : null
				map.set(m[1], cost ? { resource: cost[1], amount: Number(cost[2]) } : null)
			}
			break
		}
		ARTIFACT_COST_CACHE.map = map
		return map
	}
	const ARTIFACT_RES_CN = { minor_artifacts: '稀有文物' }
	// 先驱者「xxx 的秘密」一局只能做一次，而且 potential 要 *_world_found；默认不列，需要时直接启动
	const ARTIFACT_HIDDEN_KEYS = new Set([
		'artifact_irassian_research',
		'artifact_yuht_research',
		'artifact_inetian_research',
		'artifact_adakkaria_research',
		'artifact_vultaum_research',
		'artifact_cybrex_research',
		'artifact_league_research',
		'artifact_baol_research',
		'artifact_zroni_research',
	])
	add({
		name: 'stellaris_artifact_actions',
		description:
			'Artifact actions (稀有文物行动) of the empire, with the engine\'s own verdict. Bridge method ' +
			'get_artifact_actions {probe:true} walks TGameDatabase_CArtifactActionsDatabase (28 script entries), and for ' +
			'each builds a CArtifactActionCommand and runs IsValid only (nothing is posted). Data comes from ' +
			'common/artifact_actions/artifact_actions.txt: cost is usually minor_artifacts, cooldown lives in the effect\'s ' +
			'add_modifier. Entries whose key ends in _locked are potential-hidden placeholders (name ？？？？？) and are skipped ' +
			'unless all:true. The nine precursor “xxx 的秘密” actions (irassian / yuht / inetian / adakkaria / vultaum / ' +
			'cybrex / league / baol / zroni research) are hidden by default — they are once-per-game and gated by ' +
			'`*_world_found` country flags; fire one directly with stellaris_activate_artifact_action when needed. ' +
			'`artifact_precursor_clue` (发现先驱者洞见) is NOT in that set and still lists. A refusal with an empty ' +
			'reason means potential is unmet (IsValid does not cover it). Use ' +
			'stellaris_activate_artifact_action to fire one. raw:true returns the bridge JSON.',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: 'also list the _locked placeholders and refused actions' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const d = await rpc(pipe, 'get_artifact_actions', { probe: true })
				if (d?.error || d?.success === false) return text(`bridge: ${d?.error ?? '读取失败'}`)
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const costs = loadArtifactCosts()
				const rows = Array.isArray(d?.actions) ? d.actions : []
				const isLocked = (r) => /_locked$/.test(String(r?.key ?? ''))
				const shown = args?.all === true ? rows : rows.filter((r) => !isLocked(r))
				const listed = shown.filter((r) => !ARTIFACT_HIDDEN_KEYS.has(String(r?.key ?? '')))
				const avail = listed.filter((r) => r?.can_activate === true)
				const refused = listed.filter((r) => r?.can_activate !== true)
				const costText = (r) => {
					const c = costs.get(String(r?.key ?? ''))
					if (!c) return ''
					return `${ARTIFACT_RES_CN[c.resource] ?? c.resource} ${c.amount}`
				}
				const lines = [`文物行动（稀有文物）· 可用 ${avail.length} / ${listed.length} 条`]
				lines.push('')
				lines.push(' 可用')
				if (!avail.length) lines.push('   （暂无）')
				for (const r of avail) {
					const c = costText(r)
					lines.push(`   ${r.name ?? r.key}（${r.key}）${c ? '｜' + c : ''}`)
				}
				lines.push('')
				lines.push(` 不可用 ${refused.length}`)
				if (!refused.length) lines.push('   （无）')
				for (const r of refused) {
					const why = r?.reason && String(r.reason).trim() ? String(r.reason).trim().split('\n')[0] : '条件不足（potential 未满足）'
					lines.push(`   ${r.name ?? r.key}（${r.key}）｜${why}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	add({
		name: 'stellaris_activate_artifact_action',
		description:
			'Fire ONE artifact action (稀有文物行动). Bridge method activate_artifact_action looks the key up in ' +
			'TGameDatabase_CArtifactActionsDatabase, writes artifact_action +0x20 / country +0x28, runs IsValid and posts ' +
			'CArtifactActionCommand (token 0x3CFA). The engine\'s refusal (cost, cooldown, requirement) comes back verbatim; ' +
			'a locked placeholder key is refused by potential with no reason. Cost is minor_artifacts per the script, so ' +
			'read stellaris_artifact_actions (or stellaris_resources) first.',
		parameters: {
			type: 'object',
			properties: {
				action_key: { type: 'string', description: 'action key from stellaris_artifact_actions' },
				dry_run: { type: 'boolean', description: 'only run IsValid (via get_artifact_actions probe), post nothing' },
			},
			required: ['action_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const key = String(args?.action_key ?? '')
			if (!key) return text('action_key is required')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				if (args?.dry_run === true) {
					const d = await rpc(pipe, 'get_artifact_actions', { probe: true })
					const row = (d?.actions ?? []).find((x) => String(x?.key ?? '') === key)
					if (!row) return text(`未知行动：${key}`)
					const why = row?.reason && String(row.reason).trim() ? String(row.reason).trim() : ''
					return text(row?.can_activate === true ? `可以启动：${row.name ?? key}` : `不能启动：${why || 'potential 未满足（IsValid 不覆盖）'}`)
				}
				const r = await rpc(pipe, 'activate_artifact_action', { key })
				if (r?.success === false) return text(`失败：${r?.message ?? '未知原因'}`)
				return text(r?.message ?? `${key} 已启动`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* —— 遗珍（2026-11-22）——
	   桥 get_discoveries {tab:"relics"}：读帝国自己持有的遗珍（CCountry +0x3030 数据 / +0x303C 件数），
	   逐件给出名字、说明与「能不能激活」——后者是桥拿 CActivateRelicCommand 对引擎跑 IsValid 的答案，
	   不是自己推的，所以被挡的会带引擎原话（例：「✗ 目前没有殖民地有concept_treasure_vault」）。
	   激活见下面的 stellaris_activate_relic。 */
	/* 遗珍的被动/主动效果：游戏脚本里写着，中文名问引擎。
	   passive = triggered_country_modifier 块（potential 条件 + 数值修正 / custom_tooltip）
	   active  = active_effect 块（custom_tooltip + add_modifier；hidden_effect 里是冷却，单独标注） */
	const RELIC_COND_CN = {
		is_robot_empire: '机器人帝国',
		is_machine_empire: '机械帝国',
		is_nomadic: '游牧',
		is_wilderness_empire: '荒野',
		has_origin: '起源',
	}
	/* 脚本里的修正字段与引擎的 localisation 键不总是同名（2026-11-22 全库对过：85 个字段里只有这 4 个例外）。
	   值为引擎的 canonical 键，值写 null 表示没找到、按原样显示字段名。 */
	const RELIC_FIELD_ALIAS = {
		all_technology_research_speed: 'MOD_COUNTRY_ALL_TECH_RESEARCH_SPEED',
		science_ship_survey_speed: 'MOD_SHIP_SCIENCE_SURVEY_SPEED',
		species_leader_exp_gain: 'MOD_LEADER_SPECIES_EXP_GAIN',
		monthly_loyalty_from_organic_subjects: 'MOD_MONTHLY_LOYALTY_GAIN_FROM_ORGANIC_SUBJECTS',
	}
	const RELIC_CACHE = { defs: null, vars: null }
	const loadRelicDefs = () => {
		if (RELIC_CACHE.defs) return RELIC_CACHE
		for (const root of EDICT_GAME_DIRS) {
			const dir = path.join(root, 'common', 'relics')
			if (!existsSync(dir)) continue
			const defs = new Map()
			for (const file of ['relics.txt', 'grand_archive_relics.txt']) {
				const full = path.join(dir, file)
				if (!existsSync(full)) continue
				const text = readFileSync(full, 'utf8')
				for (const m of text.matchAll(/^([A-Za-z][A-Za-z0-9_]*)\s*=\s*\{/gm)) {
					const body = braceBlock(text, text.indexOf('{', m.index))
					if (body) defs.set(m[1], body)
				}
			}
			const vars = new Map()
			const varFile = path.join(root, 'common', 'scripted_variables', '00_scripted_variables.txt')
			if (existsSync(varFile)) {
				for (const m of readFileSync(varFile, 'utf8').matchAll(/^@([A-Za-z0-9_]+)\s*=\s*(-?\d+)\s*$/gm)) vars.set(m[1], Number(m[2]))
			}
			RELIC_CACHE.defs = defs
			RELIC_CACHE.vars = vars
			return RELIC_CACHE
		}
		RELIC_CACHE.defs = new Map()
		RELIC_CACHE.vars = new Map()
		return RELIC_CACHE
	}
	const relicDropHidden = (blk) => {
		let out = blk
		for (let guard = 0; guard < 40; guard += 1) {
			const h = out.match(/hidden_effect\s*=\s*\{/)
			if (!h) break
			const open = out.indexOf('{', h.index)
			const inner = braceBlock(out, open)
			if (!inner) break
			out = out.slice(0, h.index) + out.slice(open + inner.length + 2)
		}
		return out
	}
	const relicEffects = (key) => {
		const { defs, vars } = loadRelicDefs()
		const body = defs.get(key)
		if (!body) return null
		const numeric = (raw) => {
			const t = String(raw).trim()
			const v = /^@([A-Za-z0-9_]+)$/.exec(t)
			if (v) return vars.has(v[1]) ? vars.get(v[1]) : null
			const n = Number(t)
			return Number.isFinite(n) ? n : null
		}
		const fmt = (field, raw) => {
			const n = numeric(raw)
			if (n === null) return null
			const pct = !field.endsWith('_add') && Math.abs(n) < 1
			const sign = n > 0 ? '+' : ''
			return pct ? `${sign}${Math.round(n * 1000) / 10}%` : `${sign}${n}`
		}
		const passive = []
		for (const m of body.matchAll(/triggered_country_modifier\s*=\s*\{/g)) {
			const blk = braceBlock(body, body.indexOf('{', m.index))
			if (!blk) continue
			const cond = []
			const pm = blk.match(/potential\s*=\s*\{/)
			if (pm) {
				const pb = braceBlock(blk, blk.indexOf('{', pm.index)) ?? ''
				for (const c of pb.matchAll(/(is_[a-z_]+|has_origin)\s*=\s*([A-Za-z0-9_]+)/g)) {
					const label = RELIC_COND_CN[c[1]] ?? c[1]
					cond.push(c[2] === 'no' ? `${label}（否）` : c[2] === 'yes' ? label : `${label} ${c[2]}`)
				}
			}
			const mods = []
			for (const line of blk.split('\n')) {
				const mm = /^([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(-?[0-9.]+)\s*$/.exec(line.trim())
				if (!mm || mm[1] === 'mult') continue
				const v = fmt(mm[1], mm[2])
				if (v) mods.push({ field: mm[1], text: v })
			}
			const tip = /^\s*custom_tooltip\s*=\s*([A-Za-z0-9_]+)/m.exec(blk)
			if (mods.length || tip) passive.push({ cond: cond.join('、'), mods, tip: tip ? tip[1] : null })
		}
		const active = { tips: [], mods: [], cooldown: false }
		const am = body.match(/active_effect\s*=\s*\{/)
		if (am) {
			const rawBlk = braceBlock(body, body.indexOf('{', am.index)) ?? ''
			active.cooldown = /relic_activation_cooldown/.test(rawBlk)
			const blk = relicDropHidden(rawBlk)
			for (const mm of blk.matchAll(/^\s*custom_tooltip\s*=\s*([A-Za-z0-9_]+)/gm)) {
				if (!/^relic_triumph_cooldown/.test(mm[1]) && !active.tips.includes(mm[1])) active.tips.push(mm[1])
			}
			for (const mm of blk.matchAll(/add_modifier\s*=\s*\{/g)) {
				const mb = braceBlock(blk, blk.indexOf('{', mm.index)) ?? ''
				const nm = /modifier\s*=\s*"([^"]+)"/.exec(mb)
				const dm = /days\s*=\s*([^\s}]+)/.exec(mb)
				if (nm) active.mods.push({ name: nm[1], days: dm ? numeric(dm[1]) : null })
			}
		}
		return { passive, active }
	}

	const relicName = (r) => {
		const key = String(r?.key ?? '')
		const name = String(r?.name ?? '')
		return name && name !== key ? `${name}（${key}）` : key
	}
	const relicLine = (r) => {
		const bits = [relicName(r)]
		// 引擎的拒绝文案自带一个「✗」，别再叠一个
		const why = String(r?.blocked_reason ?? '').trim().replace(/^✗\s*/u, '')
		if (r?.can_activate) bits.push('✓ 可激活')
		else bits.push(`✗ ${why || '不可激活（引擎没给原因）'}`)
		return `  ${bits.join('｜')}`
	}

	add({
		name: 'stellaris_relics',
		description:
			'Relics your empire owns — the bridge method get_discoveries {tab:"relics"}, which reads the country\'s own ' +
			'relic array (CCountry +0x3030 data / +0x303C count; those offsets were corrected 2026-11-22 — the old ' +
			'0x30F0/0x30FC pair reads 0 for every country on 4.5.4, so this list was empty until now). Cross-checked against ' +
			'the save: a country\'s relic keys are serialized as `relics = { "r_..." }` in its country block. Each row gives ' +
			'the key, the localised name and can_activate — can_activate is the engine\'s own answer: the bridge builds a ' +
			'CActivateRelicCommand for that relic and runs IsValid without posting, so a blocked relic carries the engine\'s ' +
			'reason (e.g. 「✗ 目前没有殖民地有concept_treasure_vault」). total_galaxy_relics is the size of the relic database ' +
			'(68 = relics.txt 47 + grand_archive_relics.txt 21), not something only you can see. effects: true adds the ' +
			'passive effect (triggered_country_modifier: numeric modifiers localised through the engine + custom_tooltip ' +
			'text + its potential as a condition) and the triumph effect (active_effect: custom_tooltip lines and ' +
			'add_modifier names with their duration; the cooldown in hidden_effect is summarised). Those texts are parsed ' +
			'from common/relics/*.txt, not read from memory. Activation is a separate ' +
			'tool, stellaris_activate_relic. Measured 2026-11-22 in this save: 7 relics held, 6 ' +
			'activatable, 星国图志 blocked. Use verbose:true for each relic\'s description.',
		parameters: {
			type: 'object',
			properties: {
				verbose: { type: 'boolean', description: 'also print each relic\'s description' },
				effects: { type: 'boolean', description: 'also print the passive effect and the triumph (active) effect, parsed from common/relics/*.txt' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const data = await rpc(pipe, 'get_discoveries', { tab: 'relics' })
				if (data?.error) return text(`bridge: ${data.error}`)
				if (args?.raw === true) return text(JSON.stringify(data, null, 2))
				const box = data?.relics ?? {}
				const held = Array.isArray(box.held_relics) ? box.held_relics : []
				const total = box.total_galaxy_relics ?? '?'
				if (!held.length) {
					return text(`遗珍 0 件（银河库内 ${total} 种）\n这个帝国还没拿到遗珍——考古、事件、贸易都会给`)
				}
				const ready = held.filter((r) => r?.can_activate).length
				// effects: true —— 从脚本读被动/主动，中文名一次问引擎（mod_<字段> 与 MOD_<字段大写> 两种键都试）
				let eff = null
				let names = {}
				if (args?.effects === true) {
					eff = new Map()
					const want = new Set()
					for (const r of held) {
						const key = String(r?.key ?? '')
						const e = relicEffects(key)
						if (!e) continue
						eff.set(key, e)
						for (const p of e.passive) {
							for (const m of p.mods) {
								want.add(`mod_${m.field}`)
								want.add(`MOD_${m.field.toUpperCase()}`)
								if (RELIC_FIELD_ALIAS[m.field]) want.add(RELIC_FIELD_ALIAS[m.field])
							}
							if (p.tip) want.add(p.tip)
						}
						for (const t of e.active.tips) want.add(t)
						for (const m of e.active.mods) want.add(m.name)
					}
					if (want.size) {
						try {
							const loc = await rpc(pipe, 'localize', { keys: [...want] })
							names = loc?.names ?? {}
						} catch {
							names = {}
						}
					}
				}
				const tr = (key) => {
					const hit = names[key]
					return hit && hit !== key ? hit : key
				}
				const fieldName = (field) => {
					const alias = RELIC_FIELD_ALIAS[field]
					if (alias) {
						const hit = names[alias]
						if (hit && hit !== alias) return hit
					}
					const lower = names[`mod_${field}`]
					if (lower && lower !== `mod_${field}`) return lower
					const upperKey = `MOD_${field.toUpperCase()}`
					const upper = names[upperKey]
					if (upper && upper !== upperKey) return upper
					return field
				}
				const lines = [`遗珍 ${held.length} 件（银河库内 ${total} 种）· 可激活 ${ready}`]
				for (const r of held) {
					lines.push(relicLine(r))
					const e = eff?.get(String(r?.key ?? ''))
					if (e) {
						const flat = (s) => String(s).replace(/\n+/g, ' ').trim()
						const passiveTexts = e.passive
							.map((p) => {
								const bits = p.mods.map((m) => `${fieldName(m.field)} ${m.text}`)
								if (p.tip) bits.push(flat(tr(p.tip)))
								const body = bits.filter(Boolean).join('；')
								return p.cond ? `${body}（条件：${p.cond}）` : body
							})
							.filter(Boolean)
						if (passiveTexts.length) lines.push(`      被动 ${passiveTexts.join(' ／ ')}`)
						const activeBits = []
						for (const t of e.active.tips) activeBits.push(flat(tr(t)))
						for (const m of e.active.mods) activeBits.push(`${tr(m.name)}${m.days ? `（${m.days} 天）` : ''}`)
						if (e.active.cooldown) activeBits.push('此后遗珍启用冷却')
						if (activeBits.length) lines.push(`      主动 ${activeBits.filter(Boolean).join(' ／ ')}`)
					}
					if (args?.verbose === true) {
						const desc = String(r?.description ?? '').trim()
						if (desc) lines.push(`      ${desc.replace(/\n+/g, '\n      ')}`)
					}
				}
				if (args?.verbose !== true) lines.push('（加 verbose: true 看每件的说明）')
				lines.push('')
				lines.push(args?.effects === true
					? '被动/主动读自 common/relics/*.txt；能不能激活是引擎对 CActivateRelicCommand 跑 IsValid 的答案，激活用 stellaris_activate_relic'
					: '被动/主动读自 common/relics/*.txt（加 effects: true 显示）；能不能激活是引擎对 CActivateRelicCommand 跑 IsValid 的答案，激活用 stellaris_activate_relic')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 激活遗珍（2026-11-22）——
	   桥 activate_relic → CActivateRelicCommand（token 0x3CF3 / vtable RVA 0x23989E8 / 0x58 字节；
	   relic 是 +0x30 的字符串、country +0x50、human +0x54）。桥先核持有（本国遗珍数组里有没有这个键），
	   没有就直接回「Your empire does not possess the relic ...」，不会投。
	   dry_run 只问 get_discoveries 那一层的 can_activate，不投递。 */
	add({
		name: 'stellaris_activate_relic',
		description:
			'Activate the triumph effect of a relic your empire owns. Posts CActivateRelicCommand (token 0x3CF3, vtable ' +
			'RVA 0x23989E8, 0x58 bytes: relic string +0x30, country +0x50, human +0x54) through the bridge method ' +
			'activate_relic. The bridge first checks the country\'s own relic array (CCountry +0x3030/+0x303C — the same ' +
			'list stellaris_relics reads) and refuses keys you do not hold without posting. The triumph effect is temporary ' +
			'and the engine writes its own cooldown modifier (relic_activation_cooldown), so activating has a price: unity ' +
			'per the relic script (common/relics/relics.txt resources.cost). dry_run: true only reads can_activate from ' +
			'stellaris_relics and posts nothing. Measured 2026-11-22: 7 relics held / 6 activatable; 星国图志 is refused by ' +
			'the engine for lack of a treasure-vault colony.',
		parameters: {
			type: 'object',
			properties: {
				relic_key: { type: 'string', description: 'relic key from stellaris_relics, e.g. r_dragon_trophy' },
				dry_run: { type: 'boolean', description: 'only report whether the engine would allow it; post nothing' },
			},
			required: ['relic_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const key = String(args?.relic_key ?? '').trim()
			if (!key) return text('relic_key is required（先用 stellaris_relics 看键）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				if (args?.dry_run === true) {
					const data = await rpc(pipe, 'get_discoveries', { tab: 'relics' })
					if (data?.error) return text(`bridge: ${data.error}`)
					const held = Array.isArray(data?.relics?.held_relics) ? data.relics.held_relics : []
					const relic = held.find((r) => String(r?.key) === key)
					if (!relic) return text(`没有持有 ${key}（dry run，未投递）`)
					if (relic.can_activate) return text(`可激活：${relicName(relic)}（dry run，未投递）`)
					const why = String(relic.blocked_reason ?? '').trim().replace(/^✗\s*/u, '')
					return text(`不可激活：${relicName(relic)}｜${why || '引擎没给原因'}（dry run，未投递）`)
				}
				const result = await rpc(pipe, 'activate_relic', { relic_key: key })
				if (result?.success === false) return text(`激活失败：${result?.message ?? '未知原因'}`)
				return text(`${result?.message ?? `已激活 ${key}`}\n（triumph 效果有期限，引擎会写同类冷却；代价按 common/relics 的 cost）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 博物天枢（标本）：内存直读（2026-11-22）——
	   桥 probe_grand_archive { decode: true }：持有 = 储藏室 + 三类展览格（每类一个数组）。
	   每件带标本键与「发现者」（CExhibit+0x1A0）；名字问引擎 localize。 */
	const GALLERY_CN = ['美学大观', '星海往事', '异星奇景']
	const SPECIMEN_RARITY_CACHE = { map: null }
	const loadSpecimenRarity = () => {
		if (SPECIMEN_RARITY_CACHE.map) return SPECIMEN_RARITY_CACHE.map
		const map = new Map()
		for (const root of EDICT_GAME_DIRS) {
			const full = path.join(root, 'common', 'specimens', 'specimens.txt')
			if (!existsSync(full)) continue
			const text = readFileSync(full, 'utf8')
			for (const m of text.matchAll(/^([A-Za-z][A-Za-z0-9_]*)\s*=\s*\{/gm)) {
				const body = braceBlock(text, text.indexOf('{', m.index))
				const r = body && /RARITY\s*=\s*([a-z_]+)/.exec(body)
				map.set(m[1], r ? r[1] : '')
			}
			break
		}
		SPECIMEN_RARITY_CACHE.map = map
		return map
	}
	const SPECIMEN_TYPE_CACHE = { map: null }
	const loadSpecimenTypes = () => {
		if (SPECIMEN_TYPE_CACHE.map) return SPECIMEN_TYPE_CACHE.map
		const map = new Map()
		for (const root of EDICT_GAME_DIRS) {
			const full = path.join(root, 'common', 'specimens', 'specimens.txt')
			if (!existsSync(full)) continue
			const text = readFileSync(full, 'utf8')
			for (const m of text.matchAll(/^([A-Za-z][A-Za-z0-9_]*)\s*=\s*\{/gm)) {
				const body = braceBlock(text, text.indexOf('{', m.index))
				const t = body && /\btype\s*=\s*([a-z_]+)/.exec(body)
				map.set(m[1], t ? t[1] : '')
			}
			break
		}
		SPECIMEN_TYPE_CACHE.map = map
		return map
	}
	add({
		name: 'stellaris_specimens',
		description:
			'The specimens (标本) your empire holds in the Grand Archive — 博物天枢 — read straight from memory by the ' +
			'bridge probe probe_grand_archive {decode:true}. Holding is "which array references the exhibit": the storage ' +
			'room (module+0x40) or one of the three gallery arrays (module+0x28 -> 3 wrappers, aesthetic_wonder / ' +
			'historical_item / xeno_geology, in that order). Each exhibit resolves to its specimen key and to its ' +
			'DISCOVERER (CExhibit+0x1A0) — holder and discoverer are different things: a traded specimen keeps the ' +
			'original discoverer but is held by whoever references it. Names are localised through the engine. Measured ' +
			'2026-11-22 in this save: 31 held (storage 7 + gallery 9/9/6), every id matching the save\'s ' +
			'standard_grand_archive_module. Rows are grouped by the specimen type from common/specimens/specimens.txt ' +
			'(aesthetic_wonder / historical_item / xeno_geology), each category with its own 展柜 and 仓库 block. raw:true ' +
			'returns the bridge JSON. The save-side specimen_report.py still lists ' +
			'the full per-country table and is the fallback.',
		parameters: {
			type: 'object',
			properties: {
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const d = await rpc(pipe, 'probe_grand_archive', { decode: true })
				if (d?.error || d?.success === false) return text(`bridge: ${d?.error ?? '探针失败'}`)
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const all = [
					...(Array.isArray(d?.storage) ? d.storage : []),
					...(Array.isArray(d?.galleries) ? d.galleries.flatMap((g) => g?.rows ?? []) : []),
				].filter((r) => String(r?.specimen ?? '') !== '')
				if (!all.length) return text('博物天枢：没有读到持有中的标本（帝国可能还没拿到，或探针没定位到模块）')
				const keys = [...new Set(all.map((r) => String(r?.specimen ?? '')).filter(Boolean))]
				let names = {}
				try {
					const loc = await rpc(pipe, 'localize', { keys })
					names = loc?.names ?? {}
				} catch {}
				const label = (k) => (names[k] && names[k] !== k ? names[k] : k)
				const who = (r) => (Number(r?.owner ?? 0) === 0 ? '我们' : `country ${Number(r?.owner)}`)
				// 2026-11-22 实测：state 0 = 库存；state 2 且 activated 1 = 展览中；其余（state 1 / 或 activated 0）= 未展览
				const stateOf = (r) => (Number(r?.state) === 0 ? '库存' : Number(r?.state) === 2 && Number(r?.activated) === 1 ? '展览中' : '未展览')
				const rarities = loadSpecimenRarity()
				const RARITY_CN = { common: '普通', rare: '稀有', epic: '史诗', exceptional: '卓绝' }
				const rarityText = (r) => RARITY_CN[rarities.get(String(r?.specimen ?? '')) ?? ''] ?? ''
				// 按标本类型分三类：类型读 common/specimens/specimens.txt 的 type
				const types = loadSpecimenTypes()
				const typeOrder = ['aesthetic_wonder', 'historical_item', 'xeno_geology']
				const galleries = (Array.isArray(d?.galleries) ? d.galleries : []).map((g, i) => ({
					index: Number(g?.index ?? i),
					rows: (g?.rows ?? []).filter((r) => String(r?.specimen ?? '') !== ''),
				}))
				const storageRows = (Array.isArray(d?.storage) ? d.storage : []).filter((r) => String(r?.specimen ?? '') !== '')
				const cats = typeOrder.map((type, i) => ({
					title: GALLERY_CN[i] ?? type,
					gallery: galleries.find((g) => g.index === i)?.rows ?? [],
					storage: storageRows.filter((r) => (types.get(String(r?.specimen)) ?? '') === type),
				}))
				const other = storageRows.filter((r) => !typeOrder.includes(types.get(String(r?.specimen)) ?? ''))
				if (other.length) cats.push({ title: '未归类', gallery: [], storage: other })
				const shownCats = cats.filter((c) => c.gallery.length || c.storage.length)
				const galleryCount = shownCats.reduce((n, c) => n + c.gallery.length, 0)
				const storageCount = shownCats.reduce((n, c) => n + c.storage.length, 0)
				const lines = [`博物天枢（标本）· 持有 ${galleryCount + storageCount} 件（展柜 ${galleryCount} / 仓库 ${storageCount}）`]
				for (const c of shownCats) {
					lines.push('')
					lines.push(` ${c.title}｜展柜 ${c.gallery.length} / 仓库 ${c.storage.length}`)
					if (c.gallery.length) {
						lines.push('   展柜')
						for (const r of c.gallery) {
							lines.push(`     ${label(String(r?.specimen ?? ''))}（${r?.specimen ?? '?'}）#${r?.id ?? '?'}｜${stateOf(r)}${rarityText(r) ? '｜' + rarityText(r) : ''}｜发现者：${who(r)}`)
						}
					}
					if (c.storage.length) {
						lines.push('   仓库')
						for (const r of c.storage) {
							lines.push(`     ${label(String(r?.specimen ?? ''))}（${r?.specimen ?? '?'}）#${r?.id ?? '?'}｜库存${rarityText(r) ? '｜' + rarityText(r) : ''}｜发现者：${who(r)}`)
						}
					}
				}
				lines.push('')
				lines.push('持有 = 被哪个数组引用（储藏室 / 某一类展览格）；发现者 = CExhibit+0x1A0，交易换手不会改它')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 博物天枢（标本）的动作：展览开关 / 收发库存（2026-11-22 实测）——
	   激活展览 = CUnlockExhibitCommand（0x433C）；取消展览 = CToggleActivateExhibitCommand（0x433D）——
	   桥里的 toggle_exhibit 按当前状态自己选。发送到库存/展览格 = CMoveSpecimenToStorageCommand（0x435F）/
	   CMoveSpecimenFromStorageCommand（0x4360）；exhibit 引用用**裸 id**（带 0x01000000 会读错条目）。
	   规则：state 1 先 unlock 才能 toggle；展览中不能直接入库（先取消展览）；移动后有返回库存冷却，
	   而且标本会拿新 id——动作后都要再读一次 stellaris_specimens。 */
	add({
		name: 'stellaris_specimen_exhibit',
		description:
			'Put ONE specimen on display, or take it off. Bridge method toggle_exhibit reads the exhibit\'s current ' +
			'state (CExhibit+0x18: 0 storage, 1 in gallery but not displayed, 2 displayed) and posts the command the UI ' +
			'uses: state 1 -> CUnlockExhibitCommand (token 0x433C, country +0x20 / exhibition_index +0x24 / ' +
			'exhibit_index +0x28 — the two indices are [gallery 0/1/2, position in that gallery]); state 2 -> ' +
			'CToggleActivateExhibitCommand (0x433D, same fields). State semantics (measured 2026-11-22): +0x18 low byte = slot ' +
			'(0 storage / 1 slot not opened -> unlock / 2 slot open -> toggle); on display only when state == 2 AND the ' +
			'+0x19 activated byte is 1 (a bare activated=1 with state 1 is still 未展览). The change shows up at once — an ' +
			'two bytes, not real lag.',
		parameters: {
			type: 'object',
			properties: {
				exhibit_id: { type: 'number', description: 'exhibit id from stellaris_specimens, e.g. 63' },
			},
			required: ['exhibit_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.exhibit_id)
			if (!Number.isFinite(id)) return text('exhibit_id is required（用 stellaris_specimens 的 #id）')
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'toggle_exhibit', { exhibit_id: id })
				if (r?.success === false) return text(`操作失败：${r?.message ?? '未知原因'}`)
				return text(`${r?.message ?? `已切换 #${id}`}\n（即时生效；要核对就再读一次 stellaris_specimens）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_move_specimen',
		description:
			'Move ONE specimen between the gallery and the storage room — bridge method move_specimen, which posts ' +
			'CMoveSpecimenToStorageCommand (token 0x435F) or CMoveSpecimenFromStorageCommand (0x4360); fields are country ' +
			'+0x20 and exhibit ref +0x24. A displayed specimen CAN be moved straight to storage (measured 2026-11-22: ' +
			'#16 知识目录 went 展览中 → 库存 in one call), so there is no cancel-first rule; the real gate is a per-specimen ' +
			'storage cooldown (720 days, started when a specimen moves from storage into a gallery), and the engine answers ' +
			'with a date. Exhibit id 0 needs the 0x01000000 tag ' +
			'(a bare 0 reads as a null ref and IsValid fails with no reason); other ids use the bare value. Moving gives ' +
			'the specimen a NEW id and leaves an empty placeholder in the old array (the read tool skips those), so re-read ' +
			'stellaris_specimens afterwards.',
		parameters: {
			type: 'object',
			properties: {
				exhibit_id: { type: 'number', description: 'exhibit id from stellaris_specimens' },
				to: { type: 'string', enum: ['storage', 'gallery'], description: 'storage = 发送到库存；gallery = 发送到展览格' },
			},
			required: ['exhibit_id', 'to'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const id = Number(args?.exhibit_id)
			const to = String(args?.to ?? '')
			if (!Number.isFinite(id)) return text('exhibit_id is required')
			if (to !== 'storage' && to !== 'gallery') return text('to must be "storage" or "gallery"')
			try {
				const r = await rpc(config.pipe || DEFAULTS.pipe, 'move_specimen', {
					exhibit_id: id,
					to_storage: to === 'storage',
				})
				if (r?.success === false) return text(`移动失败：${r?.message ?? '未知原因'}`)
				return text(`${r?.message ?? `已移动 #${id}`}\n（换了位置会拿到新 id，再读一次 stellaris_specimens）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})
	/* —— 考古遗址（2026-11-20）——
	   列表走桥的 get_archaeological_sites（星系归属直接从星系对象读，不经 intel 过滤）；
	   给 fleet_id 时再调 probe_excavatability，让引擎自己对每个遗址跑一次 IsValid——
	   「能不能挖」不自己拼规则，照 RE-PLAYBOOK 的「先问引擎」。 */
	const siteDays = (raw) => {
		const n = Number(raw ?? 0) / 100000
		return Number.isFinite(n) ? String(Math.round(n * 10) / 10) : '?'
	}
	const siteOwner = (site) => {
		if (site.owner_is_player === true) return '我方'
		if (site.owner_id === undefined) return '无主'
		return site.owner_name || `country ${site.owner_id}`
	}
	const siteOwnerRank = (site) => (site.owner_id === undefined ? 0 : site.owner_is_player === true ? 1 : 2)

	add({
		name: 'stellaris_archaeological_sites',
		description:
			'Archaeological sites, from the galaxy-level CArchaeologicalSite table. Each row: id, type key and name, ' +
			'chapter (raw +0x78 index — semantics not settled, do not read it as progress), clues, difficulty, ' +
			'days_left (÷100000), locked, the planet/system it sits on and that system\'s owner (read straight from the ' +
			'galaxy object, so it is not gated by intel), visible_to_player and the excavating fleet id. ' +
			'Pass fleet_id and every site is also put to the engine: probe_excavatability builds one ' +
			'CExcavateArchaeologicalSiteFleetOrderCommand per site and only runs IsValid — nothing is posted. ' +
			'Measured 2026-11-20: a site inside another empire\'s borders comes back 「✗ 不能发掘你帝国疆域之外的考古遗址」, ' +
			'a finished one 「✗ 该考古遗址已经被其他帝国完全发掘」, and untouched sites (chapter 0) in unowned space are valid; ' +
			'IsValid does NOT look at the fleet\'s ship class or scientist, so every science ship gets the same verdicts ' +
			'(the 舰船/科学家 check is in the site type\'s allow block, which only greys the button out). Sites whose type has an ' +
			'origin-gated potential are refused with no text at all. owner filters any|unowned|own|foreign; ' +
			'include_hidden also lists sites the player cannot see.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'science ship fleet id (stellaris_state get_civilian_fleets); ask the engine which sites it may dig' },
				site_id: { type: 'number', description: 'one site in detail (also dumps its event table)' },
				owner: { type: 'string', enum: ['any', 'unowned', 'own', 'foreign'], description: 'filter by who owns the site\'s system (default any)' },
				include_hidden: { type: 'boolean', description: 'also list sites with visible_to_player false' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const data = await rpc(pipe, 'get_archaeological_sites', {})
				if (data?.error) return text(`bridge: ${data.error}`)
				let sites = Array.isArray(data?.sites) ? [...data.sites] : []
				if (!sites.length) return text('没有考古遗址')

				const fleetId = Number(args?.fleet_id)
				const probe = Number.isFinite(fleetId) ? await rpc(pipe, 'probe_excavatability', { fleet_id: fleetId }) : null
				const verdict = new Map()
				if (Array.isArray(probe?.rows)) for (const row of probe.rows) verdict.set(Number(row.site_id), row)

				if (args?.raw === true) return text(JSON.stringify({ sites, probe }, null, 2))

				const wantId = Number(args?.site_id)
				const detail = Number.isFinite(wantId)
				const ownerFilter = String(args?.owner ?? 'any')
				if (detail) sites = sites.filter((s) => Number(s.id) === wantId)
				else if (ownerFilter === 'unowned') sites = sites.filter((s) => s.owner_id === undefined)
				else if (ownerFilter === 'own') sites = sites.filter((s) => s.owner_is_player === true)
				else if (ownerFilter === 'foreign') sites = sites.filter((s) => s.owner_id !== undefined && s.owner_is_player !== true)

				const hidden = sites.filter((s) => s.visible_to_player === false)
				if (!detail && args?.include_hidden !== true) sites = sites.filter((s) => s.visible_to_player !== false)
				if (!sites.length) return text(detail ? `没有 id ${wantId} 的遗址` : '这个筛选下没有遗址')

				sites.sort((a, b) => {
					const va = verdict.get(Number(a.id))?.valid ? 0 : 1
					const vb = verdict.get(Number(b.id))?.valid ? 0 : 1
					return va - vb ||
						siteOwnerRank(a) - siteOwnerRank(b) ||
						Number(a.difficulty) - Number(b.difficulty) ||
						Number(a.id) - Number(b.id)
				})

				const mark = (s) => {
					const v = verdict.get(Number(s.id))
					if (!v) return siteOwnerRank(s) === 0 ? '·' : '-'
					return v.valid ? '✓' : '✗'
				}
				const line = (s) => {
					const v = verdict.get(Number(s.id))
					const bits = [
						`${mark(s)} id ${s.id} ${s.name || s.type || '?'}`,
						`${s.system_name || '—'}（${siteOwner(s)}）`,
						`难度 ${s.difficulty}`,
						`章 ${s.chapter}`,
						`线索 ${s.clues}`,
						`剩 ${siteDays(s.days_left)} 天`,
					]
					if (s.excavator_fleet_id !== undefined) bits.push(`在挖（船 ${s.excavator_fleet_id}）`)
					if (s.locked) bits.push('锁定')
					if (s.visible_to_player === false) bits.push('未见')
					const reason = (v?.reason ?? '').trim()
					if (v && !v.valid && reason) bits.push(reason)
					return `  ${bits.join('｜')}`
				}

				const lines = []
				if (probe) {
					const ok = sites.filter((s) => verdict.get(Number(s.id))?.valid).length
					const reasons = new Map()
					for (const s of sites) {
						const v = verdict.get(Number(s.id))
						if (!v || v.valid) continue
						const r = (v.reason ?? '').trim() || '（引擎没给原因）'
						reasons.set(r, (reasons.get(r) ?? 0) + 1)
					}
					lines.push(`遗址 ${sites.length} 处（船 ${fleetId}）：可挖 ${ok} · 拒绝 ${sites.length - ok}`)
					for (const [r, n] of [...reasons.entries()].sort((a, b) => b[1] - a[1])) lines.push(`    ${n} × ${r}`)
				} else {
					const unowned = sites.filter((s) => s.owner_id === undefined).length
					const own = sites.filter((s) => s.owner_is_player === true).length
					const digging = sites.filter((s) => s.excavator_fleet_id !== undefined).length
					lines.push(`遗址 ${sites.length} 处：无主 ${unowned} · 我方 ${own} · 他国 ${sites.length - unowned - own} · 在挖 ${digging}`)
					if (hidden.length) lines.push(`    （另有 ${hidden.length} 处你看不见：include_hidden: true 才列）`)
				}
				lines.push('')
				for (const s of sites) lines.push(line(s))
				lines.push('')
				lines.push('能不能挖：给 fleet_id 才问引擎（只 IsValid、不投递）；没给就只列数据')
				lines.push('「域外」＝遗址所在星系归别国；没有原因的拒绝多半是已挖完（chapter == 该类型 stages）')
				lines.push('章＝已完成章数（CArchaeologicalSite+0x78）；等于该类型 stages 即已挖完')

				if (detail) {
					const s = sites[0]
					lines.push('')
					lines.push(`id ${s.id} · ${s.type || '?'} · 天体 ${s.planet_id ?? s.location_id} · 星系 ${s.system_id ?? '—'}` +
						`｜归属 ${s.owner_id === undefined ? '无主' : `country ${s.owner_id}（${s.owner_name || '?'}）`}`)
					try {
						const ev = await rpc(pipe, 'probe_site_events', { site_id: Number(s.id) })
						const rows = Array.isArray(ev?.events) ? ev.events : []
						if (rows.length) {
							lines.push(`  事件表 ${rows.length} 条：`)
							for (const e of rows) lines.push(`    #${e.i} expired ${e.expired} · event ${e.event_id} · index ${e.index}`)
						} else lines.push('  事件表：空')
					} catch (error) {
						lines.push(`  事件表读不到：${error.message}`)
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* —— 投递考古发掘 ——
	   桥端 excavate_site：CExcavateArchaeologicalSiteFleetOrderCommand（fleet +0x20 / site +0x24 / queue +0x28）。
	   引擎的 IsValid 不看舰种与科学家（站点类型的 allow 只负责把按钮变灰），拿工程船投也会被收下却永远不开挖，
	   所以这里先看一眼 get_civilian_fleets，只让科研船过去。 */
	add({
		name: 'stellaris_excavate_site',
		description:
			'Post an excavation order: a science ship digs ONE archaeological site (site_id from ' +
			'stellaris_archaeological_sites). The bridge builds CExcavateArchaeologicalSiteFleetOrderCommand ' +
			'(fleet +0x20 / site +0x24 / queue +0x28). This tool first reads get_civilian_fleets and only lets a ' +
			'science ship through, because the engine\'s IsValid does not look at the ship class or the scientist at all ' +
			'(is_ship_class / exists = leader live in the site type\'s allow block and only grey the button out) — a ' +
			'constructor would have its order accepted and never dig. dry_run: true only validates and posts nothing. ' +
			'Measured 2026-11-20 on site 33 (unowned system, chapter 0): posting flipped the ship to ' +
			'「发掘考古遗址中。」 and days_left began counting; when the first chapter finished the site went ' +
			'locked = true and extreme_frontiers.1300 opened, and stellaris_answer_event cleared the lock and started ' +
			'the next chapter. queue: true appends instead of replacing the fleet\'s orders.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'science ship fleet id (get_civilian_fleets → science_ships)' },
				site_id: { type: 'number', description: 'site id from stellaris_archaeological_sites (0 is a valid id)' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing its orders' },
				dry_run: { type: 'boolean', description: 'only run the engine IsValid, do not post' },
			},
			required: ['fleet_id', 'site_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const fleetId = Number(args?.fleet_id)
			const siteId = Number(args?.site_id)
			if (!Number.isFinite(fleetId)) return text('fleet_id is required（用 get_civilian_fleets 的 science_ships）')
			if (!Number.isFinite(siteId)) return text('site_id is required（stellaris_archaeological_sites 的 id；0 也是合法值）')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				let shipName = ''
				try {
					const fleets = await rpc(pipe, 'get_civilian_fleets', {})
					const ships = Array.isArray(fleets?.science_ships) ? fleets.science_ships : []
					const ship = ships.find((f) => Number(f.fleet_id) === fleetId)
					if (!ship) {
						const ids = ships.map((f) => `${f.fleet_id} ${f.name ?? ''}`.trim()).join('、')
						return text(`fleet ${fleetId} 不是科研船，不投——引擎的 IsValid 不看舰种，收下也不会开挖。现有科研船：${ids || '（无）'}`)
					}
					shipName = ship.name ?? ''
				} catch {
					// 列表读不到就放行，让引擎自己回答
				}
				const result = await rpc(pipe, 'excavate_site', {
					fleet_id: fleetId,
					site_id: siteId,
					queue: args?.queue === true,
					dry_run: args?.dry_run === true,
				})
				if (args?.dry_run === true) {
					if (result?.valid) return text(`可挖：船 ${fleetId}${shipName ? ` ${shipName}` : ''} → 遗址 ${siteId}（dry run，未投递）`)
					return text(`不可挖：${(result?.reason ?? '').trim() || result?.error || '引擎没给原因'}`)
				}
				if (result?.success === false || result?.error) return text(`投递失败：${result?.error ?? '未知原因'}`)
				return text(
					`已投递：船 ${fleetId}${shipName ? ` ${shipName}` : ''} → 遗址 ${siteId}\n` +
					'下一 tick 船会变「发掘考古遗址中。」；一段走完遗址会 locked = true 并挂阶段事件，用 stellaris_answer_event 应答\n' +
					JSON.stringify(result),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	bridgeTool('stellaris_launch_council_agenda', 'launch_council_agenda',
		'Launch the council agenda that is running. When it has finished (is_ready) this posts CFinishAgendaCommand; pass early=true to launch before the bar fills, which posts CEarlyFinishAgendaCommand and costs unity (the engine prices it — 60000 unity in one 4.5.4 test — and refuses with its own reason when you cannot pay). After launching, pick the next one with stellaris_set_council_agenda.',
		{ early: { type: 'boolean', description: 'launch before the bar is full, paying unity' } })
	bridgeTool('stellaris_set_council_agenda', 'set_council_agenda',
		'Set the next council agenda to run. The agenda_key comes from stellaris_government (its available_agendas list, already filtered by the engine\'s own IsValid) — or from stellaris_agenda_options when you also want the `allow` block checked, since an agenda whose allow is unmet is accepted here and then silently cleared at the next monthly tick. WARNING: switching while an agenda is still running DISCARDS all of its progress, and it does not come back when you switch back (measured 2026-11-19: 5049 → 0). The reply carries discarded_progress plus a warning when that happens; abandoning is sometimes intended, so it is not blocked.',
		{ agenda_key: { type: 'string', description: 'agenda key from stellaris_government, e.g. agenda_build_relations' } })

	add({
		name: 'stellaris_status',
		description:
			'What the Stellaris launcher sees: installed game build, the active playset, its mods and the DLL plugins. Use it before launching the game.',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			const r = await run(stl, ['status'])
			return text(r.text || `stl exited with ${r.code}`)
		},
	})

	add({
		name: 'stellaris_state',
		description:
			'ESCAPE HATCH — live state from the game through the stellaris-mcp bridge. `method` is forwarded to the bridge as-is with NO allowlist, so it can reach any bridge method, including run_console, read_memory, save_game and exit_to_main_menu. `params` is forwarded verbatim as the method arguments, so parameterised methods work too — it is a general-purpose door, not a sandbox. Use it only for what no dedicated tool covers, and prefer a dedicated tool when one exists.',
		parameters: {
			type: 'object',
			properties: {
				method: {
					type: 'string',
					description: 'a pipe method: get_status (default), get_research_state, get_government, get_fleets, ...',
				},
				params: {
					type: 'object',
					description: 'arguments for that method, forwarded as-is (e.g. {"planet_id": 14}); omit for no-argument methods',
				},
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const answer = await pipeCall(
					config.pipe || DEFAULTS.pipe,
					args?.method || 'get_status',
					args?.params && typeof args.params === 'object' ? args.params : {},
				)
				const result = withDisplayPower(answer.result ?? answer)
				return text(JSON.stringify(result))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_inbox',
		description:
			'The command channel: is there a new line from the player in the in-game input box? Returns the text, when it was sent, and whether it is new since the last acknowledgement. Pass ack=true to mark the current one as read.',
		parameters: {
			type: 'object',
			properties: { ack: { type: 'boolean', description: 'mark the current reply as read' } },
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const replyPath = path.join(dir, 'reply.txt')
			const statePath = path.join(here, 'inbox.json')
			let text0 = ''
			let mtime = 0
			try {
				text0 = (await fs.readFile(replyPath, 'utf8')).trim()
				mtime = (await fs.stat(replyPath)).mtimeMs
			} catch (error) {
				return text(`no reply yet at ${replyPath}: ${error.message}`)
			}
			let handled = 0
			try { handled = Number(JSON.parse(readFileSync(statePath, 'utf8')).mtimeMs) || 0 } catch {}
			const isNew = mtime > handled
			if (args?.ack) {
				try {
					writeFileSync(
						statePath,
						JSON.stringify({ mtimeMs: mtime, text: text0, at: new Date().toISOString() }, null, 2) + '\n',
						'utf8',
					)
				} catch (error) {
					return text(`could not write ${statePath}: ${error.message}`)
				}
			}
			const at = new Date(mtime).toLocaleString('zh-CN')
			return text(`${isNew ? 'NEW' : 'seen'}: ${text0}\n(${at}${args?.ack ? ', marked read' : ''})`)
		},
	})

	add({
		name: 'stellaris_letter_write',
		description:
			'Speak into the in-game panel in letter form: appends one entry (a first line plus optional following lines) to the shared chat log. The panel shows the last 30 entries, oldest first, scrollable. Set replace=true to rewrite the whole log.',
		parameters: {
			type: 'object',
			properties: {
				text: { type: 'string', description: 'what to say, UTF-8; newlines are kept inside this one entry' },
				replace: { type: 'boolean', description: 'replace the whole chat log instead of appending' },
			},
			required: ['text'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const chat = path.join(dir, 'chat.log')
			const raw = String(args?.text ?? '').replace(/\r\n/g, '\n').replace(/^\n+|\n+$/g, '')
			if (!raw) return text('nothing to say')
			const now = new Date()
			const stamp = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`
			const entry = `[${stamp}] 助手 ${raw}`
			try {
				await fs.mkdir(dir, { recursive: true })
				let body = ''
				if (!args?.replace) {
					try { body = await fs.readFile(chat, 'utf8') } catch {}
				}
				const entries = body
					.split(/\n{2,}/)
					.map((item) => item.trim())
					.filter(Boolean)
				entries.push(entry)
				await fs.writeFile(chat, entries.slice(-200).join('\n\n') + '\n', 'utf8')
				await fs.writeFile(letter, raw, 'utf8')
				return text(`said: ${entry.split('\n')[0]}`)
			} catch (error) {
				return text(`could not write ${chat}: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_explore',
		description:
			'Queue exploration for the science ship: walk outwards from where it is, skip any system that holds a hostile fleet, and queue move+survey orders for up to `targets` unsurveyed systems in one go. It dispatches those orders immediately without asking for confirmation — cancel with a fleet-order tool if the route is wrong. If a hostile fleet shares the ship system it cancels orders and withdraws instead. Pass dry_run to only report the plan.',
		parameters: {
			type: 'object',
			properties: {
				targets: { type: 'number', description: 'how many systems to queue (default 3, max 12)' },
				dry_run: { type: 'boolean', description: 'report the plan without issuing any order' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const dry = args?.dry_run === true
			const want = Math.max(1, Math.min(12, Number(args?.targets ?? 3) || 3))
			try {
				const civilians = await rpc(pipe, 'get_civilian_fleets')
				const ships = civilians?.science_ships ?? []
				if (!ships.length) return text('没有科学船')
				const ship = ships[0]

				/* 先定位：扫到它在的星系就停 */
				const overview = await rpc(pipe, 'get_galaxy_overview')
				const total = Number(overview?.systems_total ?? 250) + 40
				let here = null
				for (let id = 0; id < total && !here; id += 1) {
					let system
					try { system = await rpc(pipe, 'get_system', { system_id: id }) } catch { continue }
					if (!system || !system.name) continue
					const summary = summarise(system, id)
					if (summary.fleets.some((fleet) => fleet.id === ship.fleet_id && fleet.own)) here = summary
				}
				if (!here) return text(`找不到「${ship.name}」所在的星系`)

				const cache = new Map([[here.id, here]])
				const load = async (id) => {
					if (cache.has(id)) return cache.get(id)
					let system
					try { system = await rpc(pipe, 'get_system', { system_id: id }) } catch { return null }
					if (!system || !system.name) return null
					const summary = summarise(system, id)
					cache.set(id, summary)
					return summary
				}

				const neighboursOf = async (system) => {
					const result = []
					for (const lane of system.neighbours) {
						const next = await load(lane.id)
						if (next) result.push(next)
					}
					return result
				}

				const around = await neighboursOf(here)
				const safeAround = around.filter((system) => !system.hostile)

				/* 一、同星系有敌对舰队：撤，哪怕正在勘察 */
				if (here.hostile) {
					if (!safeAround.length) return text(`「${here.name}」里有敌对舰队，邻接星系也都不安全，先原地不动`)
					safeAround.sort((a, b) => (a.surveyed === b.surveyed ? 0 : a.surveyed ? -1 : 1))
					const refuge = safeAround[0]
					if (!dry) {
						await rpc(pipe, 'cancel_fleet_orders', { fleet_ids: [ship.fleet_id] })
						await rpc(pipe, 'move_fleet', { fleet_id: ship.fleet_id, system_id: refuge.id })
					}
					return text(`回避：${here.name} 有敌对舰队，${dry ? '打算' : '已'}让「${ship.name}」撤往 ${refuge.name}（${refuge.id}）`)
				}

				/* 二、由近及远走一圈，跳过有敌对舰队的星系，凑够 targets 个未勘察目标 */
				const plan = []
				const seen = new Set([here.id])
				let frontier = [here]
				let hops = 0
				while (plan.length < want && frontier.length && hops < 8) {
					const next = []
					for (const system of frontier) {
						for (const neighbour of await neighboursOf(system)) {
							if (seen.has(neighbour.id)) continue
							seen.add(neighbour.id)
							if (neighbour.hostile) continue           /* 有敌对舰队：不进规划 */
							if (!neighbour.surveyed && plan.length < want) plan.push(neighbour)
							next.push(neighbour)
						}
					}
					frontier = next
					hops += 1
				}

				if (!plan.length) return text(`${here.name} 周围没有可勘察的星系（邻居 ${around.length} 个，安全 ${safeAround.length} 个）`)

				if (!dry) {
					let first = true
					for (const target of plan) {
						await rpc(pipe, 'move_fleet', {
							fleet_id: ship.fleet_id,
							system_id: target.id,
							queue: !first,
						})
						await rpc(pipe, 'survey', { fleet_id: ship.fleet_id, system_id: target.id, queue: !first })
						first = false
					}
				}

				const names = plan.map((system) => `${system.name}(${system.id})`).join(' → ')
				return text(
					`${dry ? '打算' : '已'}让「${ship.name}」依次勘察 ${plan.length} 个星系：${names}` +
						`；跳过有敌对舰队的 ${around.length - safeAround.length} 个邻居`,
				)
			} catch (error) {
				return text(`explore: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_event_mark',
		description:
			'Event dedupe: record that this event key was written now (so a repeat within 30 minutes is skipped), or pass dry_run=true to only ask whether it is a repeat. Keeps entries for a day.',
		parameters: {
			type: 'object',
			properties: {
				key: { type: 'string', description: 'event key, e.g. unique.130' },
				title: { type: 'string', description: 'what the event was called, for the log' },
				dry_run: {
					type: 'boolean',
					description: 'true = only report whether this key is a repeat, do not record it',
				},
			},
			required: ['key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const key = String(args?.key ?? '').trim()
			if (!key) return text('no key given')

			/* dry_run：只判断是否重复，不写入 */
			if (args?.dry_run === true) {
				const seen = readSeenEvents()
				const entry = seen.find((item) => item.key === key)
				if (!entry) return text(`${key}: new (never written)`)
				const at = Date.parse(entry.at ?? '')
				const minutes = Number.isFinite(at) ? (Date.now() - at) / 60000 : Infinity
				const verdict = minutes < AUTO_EXPIRE_MINUTES ? 'repeat' : 'new again (older than 30 min)'
				return text(`${key}: ${verdict}; last written ${entry.at ?? '(unknown)'} (${minutes.toFixed(1)} min ago)`)
			}

			const now = new Date()
			const seen = readSeenEvents().filter((item) => item.key !== key)
			seen.push({ key, title: args?.title ?? '', at: now.toISOString() })
			const kept = seen
				.filter((item) => {
					const at = Date.parse(item.at ?? '')
					return Number.isFinite(at) && Date.now() - at < 24 * 3600 * 1000
				})
				.slice(-100)
			try {
				writeFileSync(seenEventsPath, JSON.stringify({ seen: kept }, null, 2) + '\n', 'utf8')
			} catch (error) {
				return text(`could not write ${seenEventsPath}: ${error.message}`)
			}
			return text(`marked ${key} at ${now.toISOString()} (${kept.length} entries kept)`)
		},
	})

	add({
		name: 'stellaris_home_base',
		description:
			'Read or change a fleet\'s home base. The home base is where the fleet regroups: after a defeat that sends it MIA (emergency FTL), it returns there. Call with only fleet_id to read; add starbase_id to move it. Needs the locally built bridge (see stellaris-bridge-patch): the shipped 0.7.0 has no such method.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id (from get_military_fleets / get_civilian_fleets)' },
				starbase_id: { type: 'number', description: 'target starbase id; omit to only read' },
			},
			required: ['fleet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const fleetId = Number(args?.fleet_id)
			if (!Number.isFinite(fleetId)) return text('fleet_id is required')
			const starbaseId = args?.starbase_id === undefined ? undefined : Number(args.starbase_id)
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				if (starbaseId === undefined) {
					const now = await rpc(pipe, 'get_fleet_home_base', { fleet_id: fleetId })
					if (now?.error) return text(`读不到：${now.error}`)
					return text(
						`${now.fleet}（id ${fleetId}）的母港：${now.home_kind_name} ${now.home_id}（模板 ${now.template_id}）`,
					)
				}
				const before = await rpc(pipe, 'get_fleet_home_base', { fleet_id: fleetId })
				const result = await rpc(pipe, 'set_fleet_home_base', { fleet_id: fleetId, starbase_id: starbaseId })
				if (result?.success === false) return text(`引擎拒绝：${result.error}`)
				const after = await rpc(pipe, 'get_fleet_home_base', { fleet_id: fleetId })
				return text(
					`${before?.fleet ?? fleetId} 的母港：${before?.home_id ?? '?'} → ${after?.home_id ?? '?'}` +
						`（${after?.home_kind_name ?? '?'}）；战败紧急跃迁后它会回到这里集结`,
				)
			} catch (error) {
				return text(`home base: ${error.message}`)
			}
		},
	})

	/* ---- 列出所有殖民地设计 ---- */
	add({
		name: 'stellaris_list_colony_designations',
		description:
			'List every colony designation the game has loaded (key + localized name + optional static effects parsed from the game files). Use it to pick a key for stellaris_set_colony_designation.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet used as the scan anchor (default 14)' },
				filter: { type: 'string', description: 'only show keys containing this text, e.g. "habitat"' },
				effects: { type: 'boolean', description: 'also parse static effects from the game files' },
				for_planet: {
					type: 'boolean',
					description:
						'only list designations of this colony\'s own family (habitat → col_habitat_*, ringworld → col_ring_*, ordinary planet → no prefix), derived from its district type',
				},
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const planetId = args?.planet_id ?? 14
			const filter = typeof args?.filter === 'string' ? args.filter : ''
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'list_colony_designations', { planet_id: planetId })
				if (!answer?.success) return text(`失败：${JSON.stringify(answer)}`)
				let rows = answer.designations ?? []
				if (filter) rows = rows.filter((r) => String(r.key).includes(filter))

				if (args?.for_planet) {
					/* 用殖民地的区划类型推"家族前缀"：
					   district_hab_* → col_habitat_*；district_ring_* → col_ring_*；普通 → 无前缀 */
					const detail = await rpc(config.pipe || DEFAULTS.pipe, 'get_planet_details', { planet_id: planetId }).catch(() => null)
					let family = ''
					for (const d of detail?.districts ?? []) {
						const t = String(d.type || '')
						if (t.startsWith('district_hab_')) { family = 'col_habitat_'; break }
						if (t.startsWith('district_ring_')) { family = 'col_ring_'; break }
					}
					rows = rows.filter((r) => {
						const key = String(r.key)
						if (family) return key.startsWith(family)
						return !key.startsWith('col_habitat_') && !key.startsWith('col_ring_') && !key.startsWith('col_wilderness_') && !key.startsWith('col_nomad_')
					})
				}

				let effectsMap = {}
				if (args?.effects && rows.length) {
					const python = config.python || 'C:/Users/user/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe'
					const script =
						config.effectsScript ||
						path.join(DEFAULTS.workspace || 'C:/Users/user/Documents/deepseek-harness/default-workspace', 'stellaris-save', 'colony_effects.py')
					const keys = rows.map((r) => r.key)
					const out = await new Promise((resolve) => {
						execFile(python, [script, ...keys], { windowsHide: true, timeout: 120000, encoding: 'utf8' }, (error, stdout) => {
							resolve(error ? '' : stdout)
						})
					})
					for (const line of out.split('\n')) {
						const i = line.indexOf(' → ')
						if (i > 0) effectsMap[line.slice(0, i)] = line.slice(i + 3)
					}
				}

				const lines = rows.map((r) => {
					const eff = effectsMap[r.key]
					return `${r.name}  ${r.key}${eff ? '\n    ' + eff : ''}`
				})
				return text(`${rows.length} 个设计（共 ${answer.count}）\n\n` + lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 岗位编制（内存读，planet+0x788） ---- */
	add({
		name: 'stellaris_job_capacity',
		description:
			'Read a colony\'s job capacity table straight from memory (planet +0x788): one row per job with its workforce cap, job key, localised name and stratum. Job caps from buildings and districts are whole numbers; the fractional rows in the raw table are not jobs at all (they are modifier/contribution bookkeeping) and are dropped, with the count reported in skipped_non_job. Only a cap coming from a non-building, non-district source could ever be fractional. Works for any empire. Use stellaris_colony_jobs (save-based) when you also need who is actually working.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {

			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'get_planet_job_capacity', {
					planet_id: args.planet_id,
				})
				return text(JSON.stringify(answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 岗位在岗（内存读，CPopJob 对象 +0xF8/+0x100） ---- */
	/* 与 stellaris_job_capacity 的分工：那只是「编制表」（planet+0x788，只有上限），
	   这只是「每个岗位实际站了几个人」——在岗 / 编制 / 等效 / 加成，按阶层分组。 */
	add({
		name: 'stellaris_planet_jobs',
		description:
			'Live job assignment of a colony, read from memory (CPopJob objects): per stratum, every job with its current workforce (cur, 在岗), its capacity (max, 编制), the effective workforce (eff, 等效人数 after modifiers) and the modifier bonus (加成, can be negative). Give planet_id for a planet colony, or **colony_id for a Nomads ark** — the ark has no CPlanet, so the ark path reads CColony +0x38 directly (same CPopJob fields; the planet-only capacity table does not exist there, capacity comes from CPopJob +0x100). Jobs with neither workforce nor capacity are skipped. A job without a cap (e.g. maintenance drones) reports max = -1.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark (no CPlanet)' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			if (typeof args?.colony_id === 'number') {
				try {
					const pc = await probeColony(pipe, args.colony_id)
					if (!pc?.ptr) return text(`读不到殖民地 ${args.colony_id} 的指针`)
					const rows = await readColonyJobs(pipe, Number(pc.ptr))
					if (!rows?.length) return text(`殖民地 ${args.colony_id} 没有可读的岗位`)
					const strata = [...new Set(rows.map((r) => r.stratum))].sort()
					const sKeys = [...new Set(strata.map((s) => 'pop_cat_' + s))]
					const sNames = sKeys.length ? ((await rpc(pipe, 'localize', { keys: sKeys }).catch(() => null))?.names ?? {}) : {}
					const total = rows.reduce((n, r) => n + r.cur, 0)
					const lines = [`殖民地 ${args.colony_id}（${pc.name ?? '?'}）· 总在岗 ${total.toFixed(0)}`]
					for (const s of strata) {
						const group = rows.filter((r) => r.stratum === s).sort((a, b) => b.cur - a.cur)
						const sName = sNames['pop_cat_' + s] && sNames['pop_cat_' + s] !== 'pop_cat_' + s ? sNames['pop_cat_' + s] : s
						lines.push('')
						lines.push(`  [${sName}] ${s} · 在岗 ${group.reduce((n, r) => n + r.cur, 0).toFixed(0)}`)
						for (const r of group) {
							const eff = r.cur + r.bonus
							/* 玩家设了上限时，界面显示的就是「上限」，编制另注在后 */
							const capVal = r.limit >= 0 ? r.limit.toFixed(0) : r.max < 0 ? '∞' : r.max.toFixed(0)
							const capTxt =
								r.limit >= 0 && Math.abs(r.limit - r.max) > 0.5 ? `（编制 ${r.max < 0 ? '∞' : r.max.toFixed(0)}）` : ''
							lines.push(
								`    ${r.name} ${r.key}  ${r.cur.toFixed(0)}/${capVal}${capTxt}  等效 ${eff.toFixed(1)}（加成 ${r.bonus >= 0 ? '+' : ''}${r.bonus.toFixed(1)}）`,
							)
						}
					}
					return text(lines.join('\n'))
				} catch (error) {
					return text(`殖民地岗位：${error.message}`)
				}
			}
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id（行星）或 colony_id（方舟）')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_planet_jobs', { planet_id: args.planet_id })
				if (d?.success !== true) return text(bridgeFail(d))
				const strata = Array.isArray(d.strata) ? d.strata : []
				const lines = [`行星 ${d.planet_id} · 总在岗 ${d.total_workforce ?? 0}`]
				for (const s of strata) {
					lines.push('')
					lines.push(`  [${s.stratum_name ?? s.stratum_key}] ${s.stratum_key} · 在岗 ${s.total_workforce ?? 0}`)
					for (const j of s.jobs ?? []) {
						const bonus = typeof j.bonus_workforce === 'number' ? (j.bonus_workforce >= 0 ? `+${j.bonus_workforce}` : `${j.bonus_workforce}`) : '?'
						const cap = j.workforce_limit && j.workforce_limit !== j.max_workforce ? ` 上限 ${j.workforce_limit}` : ''
						lines.push(
							`    ${j.job_name ?? j.job_key} ${j.job_key}  ${j.current_workforce}/${j.max_workforce}` +
								`  等效 ${j.effective_workforce}（加成 ${bonus}）${cap}${j.is_prioritized ? ' · 已优先' : ''}`,
						)
					}
				}
				if (!strata.length) lines.push('  （这座殖民地没有可读的岗位）')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 岗位优先级 / 上限（桥的 prioritize_job / set_job_workforce_limit） ---- */
	add({
		name: 'stellaris_prioritize_job',
		description:
			'Toggle one job\'s priority on a colony (prioritize_job). Calling it again with the same job CLEARS the priority — it is a toggle, not a set. The answer says whether the job ends up prioritized; stellaris_planet_jobs shows the same as is_prioritized. job_key is the bare key (e.g. mining_drone, replicator).',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				job_key: { type: 'string', description: 'job key without the job_ prefix, e.g. mining_drone' },
			},
			required: ['planet_id', 'job_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' || !args?.job_key) return text('需要 planet_id 和 job_key')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'prioritize_job', {
					planet_id: args.planet_id,
					job_key: args.job_key,
				})
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`${args.job_key} 的优先已${d.is_prioritized ? '开启' : '取消'}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_job_limit',
		description:
			'Cap the workforce of ONE job on a colony (set_job_workforce_limit): pops above the cap are pushed to other jobs (usually maintenance drones) right away, and come back when the cap is raised. limit < 0 resets to the job\'s capacity; a value above the capacity is clamped to it. Setting the value it already has is REFUSED by the engine ("Workforce limit cannot be changed") — lower it first, then reset. The answer carries max_workforce (the real capacity) and workforce_limit (what was applied).',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				job_key: { type: 'string', description: 'job key without the job_ prefix, e.g. mining_drone' },
				limit: { type: 'number', description: 'new cap; negative resets to the job capacity' },
			},
			required: ['planet_id', 'job_key', 'limit'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' || !args?.job_key || typeof args?.limit !== 'number') {
				return text('需要 planet_id、job_key 和 limit')
			}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'set_job_workforce_limit', {
					planet_id: args.planet_id,
					job_key: args.job_key,
					limit: args.limit,
				})
				if (d?.success !== true) return text(bridgeFail(d))
				return text(
					`${args.job_key} 上限 → ${d.workforce_limit}（编制 ${d.max_workforce ?? '?'}）` +
						`${args.limit < 0 ? '（已复位）' : ''}\n${JSON.stringify(d)}`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 建站：采矿 / 研究 / 观测站（桥的 build_station） ---- */
	add({
		name: 'stellaris_build_station',
		description:
			'Order a construction ship to build an orbital station on one celestial body (bridge build_station). station is "mining" (mineral or energy deposits), "research" (physics / society / engineering deposits) or "observation" (only on a body that holds a pre-FTL civilisation). **The body must lie inside your own borders — the game refuses stations outside them.** Read the deposits first with get_system {system_id} (only SURVEYED bodies list deposits) or find_systems {purpose:"deposit", resource:"minerals"}; planet_id is the body id from planets[].id, NOT a colony id. Omit station and the bridge tries mining, then research, returning each refusal in reasons. queue: true appends the order to that fleet\'s queue.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id (see get_civilian_fleets)' },
				planet_id: { type: 'number', description: 'celestial body id from get_system planets[].id' },
				station: { type: 'string', description: '"mining" | "research" | "observation"; omit to try mining then research' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of dropping its other orders' },
			},
			required: ['fleet_id', 'planet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.planet_id !== 'number') {
				return text('需要 fleet_id 和 planet_id')
			}
			const params = { fleet_id: args.fleet_id, planet_id: args.planet_id }
			if (args.station) params.station = args.station
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'build_station', params)
				if (d?.success !== true) {
					const lines = [bridgeFail(d)]
					for (const [kind, why] of Object.entries(d?.reasons ?? {})) {
						lines.push(`  ${kind}: ${String(why).trim()}`)
					}
					return text(lines.join('\n'))
				}
				return text(
					`${d.station === 'mining' ? '采矿站' : d.station === 'research' ? '研究站' : d.station} 命令已投：天体 ${d.planet_id}` +
						`${d.queued ? '（排队）' : ''}\n${JSON.stringify(d)}`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 建哨站：工程船的舰队命令（桥的 build_outpost） ---- */
	add({
		name: 'stellaris_build_outpost',
		description:
			'Order a construction ship to build an outpost in a system (fleet order via the bridge: build_outpost). The system must be fully surveyed and the alloys are paid when the order is posted; the bridge lists the engine\'s reasons together (e.g. "提拉莫尔星系没有被完全调查。" plus "✗ 缺少合金 76") instead of one at a time. The ship does not need to be in the target system already — the engine validates survey and alloys first. queue: true appends the order to that fleet\'s queue. An outpost has 0 module slots: upgrade it before it can build ships.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id (see get_civilian_fleets)' },
				system_id: { type: 'number', description: 'target system id' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of dropping its other orders' },
			},
			required: ['fleet_id', 'system_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.system_id !== 'number') {
				return text('需要 fleet_id 和 system_id')
			}
			const params = { fleet_id: args.fleet_id, system_id: args.system_id }
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'build_outpost', params)
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`建哨站命令已投：${d.system ?? d.system_id}${d.queued ? '（排队）' : ''}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地模块入队（正规路径，CAddBuildableToQueueCommand） ---- */
	add({
		name: 'stellaris_queue_starbase_module',
		description:
			'Queue a starbase module through the normal construction queue (native CAddBuildableToQueueCommand with a CBuildableStarbaseModule): it costs resources and takes build time, unlike the instant `set_starbase_module` script effect. It first asks stellaris_buildable_starbase_modules where this module can actually go, then queues it in the first empty slot whose probe accepted it (or in `slot` when that slot is among them) — so a full starbase is refused with the engine\'s own list instead of silently charging alloys. The engine alone would only refuse a PENDING task in the slot: a queue into an already-built slot returns success, charges the alloys and builds nothing, which is why the probe comes first.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
				module_key: { type: 'string', description: 'module key, e.g. "shipyard"' },
				slot: { type: 'number', description: 'prefer this module slot (0-based); omit to take the first slot that accepts the module' },
			},
			required: ['starbase_id', 'module_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number' || !args?.module_key) {
				return text('需要 starbase_id 和 module_key')
			}
			const pipe = config.pipe || DEFAULTS.pipe
			const wantSlot = typeof args.slot === 'number' ? args.slot : null

			// 先读可造列表：它已经按引擎的条件筛过，且只列空槽（已装模块的槽不会出现在 buildable 里）
			let list
			try {
				const params = { starbase_id: args.starbase_id, module_key: args.module_key, include_blocked: true }
				if (wantSlot !== null) params.slot = wantSlot
				list = await rpc(pipe, 'get_buildable_starbase_modules', params)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
			if (list?.success !== true) return text(`失败：${list?.error ?? JSON.stringify(list ?? {})}`)

			const slots = Array.isArray(list.slots) ? list.slots : []
			// 已装的槽也能换别的模块（实测过），所以 occupied 不参与筛选，只看探针认不认；
			// 但**空槽优先**：没点名槽位时别把已装的东西悄悄换掉
			const candidates = slots.filter((s) => (s.buildable ?? []).length > 0)
			candidates.sort((a, b) => (a.occupied === b.occupied ? a.slot - b.slot : a.occupied ? 1 : -1))
			if (!candidates.length) {
				const reasons = []
				for (const s of slots) {
					for (const b of s.blocked ?? []) reasons.push(`槽 ${s.slot} ✗ ${b.reason}`)
				}
				const head = wantSlot !== null
					? `槽 ${wantSlot} 建不了 ${args.module_key}：`
					: `${args.module_key} 在所有空槽都建不了：`
				return text(`${head}${reasons.length ? '\n  ' + [...new Set(reasons)].join('\n  ') : ''}`)
			}

			const target = wantSlot !== null ? (candidates.find((s) => s.slot === wantSlot) ?? candidates[0]) : candidates[0]
			if (wantSlot !== null && target.slot !== wantSlot) {
				// 指定的槽不行、但别的槽可以——按“选一个可造的”处理，并说明换了槽
				const note = `（槽 ${wantSlot} 不可建，改到槽 ${target.slot}）`
				try {
					const answer = await rpc(pipe, 'queue_starbase_module', {
						starbase_id: args.starbase_id,
						slot: target.slot,
						module_key: args.module_key,
					})
					return text(`${JSON.stringify(answer)}\n${note}`)
				} catch (error) {
					return text(`bridge: ${error.message}`)
				}
			}
			try {
				const answer = await rpc(pipe, 'queue_starbase_module', {
					starbase_id: args.starbase_id,
					slot: target.slot,
					module_key: args.module_key,
				})
				return text(JSON.stringify(answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地：每个空槽能建哪些模块（桥的 get_buildable_starbase_modules） ---- */
	add({
		name: 'stellaris_buildable_starbase_modules',
		description:
			'Which modules each free slot of a starbase accepts right now, with cost and build days — one block per slot; occupied slots just report what is installed. Pass module_key to ask about one module (it then also reports the engine reason when it is refused) and slot to look at a single slot. The probe only validates, nothing is queued. Use it before stellaris_queue_starbase_module: queueing into an occupied slot returns success, charges the alloys and silently builds nothing.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
				slot: { type: 'number', description: 'only this module slot (0-based); omit for all' },
				module_key: { type: 'string', description: 'ask about one module, e.g. "shipyard"' },
				include_blocked: { type: 'boolean', description: 'also list modules the engine refuses, with the reason' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			const params = { starbase_id: args.starbase_id }
			if (typeof args.slot === 'number') params.slot = args.slot
			if (args.module_key) params.module_key = args.module_key
			if (args.include_blocked) params.include_blocked = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_starbase_modules', params)
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const lines = [`${d.level}（starbase ${d.starbase_id}）· ${d.module_slots} 个模块槽`]
				for (const s of d.slots ?? []) {
					const open = s.buildable ?? []
					const cur = s.occupied ? [s.current_name, s.current_key].filter(Boolean).join(' ') : ''
					if (args.module_key) {
						// 单模块模式：给「能建 / 不能建 + 原因」；已装的槽能换别的模块
						if (open.length) {
							const b = open[0]
							const tail = cur ? `（替换 ${cur}）` : ''
							lines.push(`  槽 ${s.slot}  可建  ${[b.name, b.key].filter(Boolean).join(' ')}  ${priceText(b)}${tail}`)
						} else {
							lines.push(`  槽 ${s.slot}  ✗ ${s.blocked?.[0]?.reason ?? '不可建'}`)
						}
						continue
					}
					lines.push(cur ? `  槽 ${s.slot}  已装 ${cur}；可换 ${open.length} 项：` : `  槽 ${s.slot}  空着，可建 ${open.length} 项：`)
					for (const b of open) lines.push(`      ${b.name} ${b.key}  ${priceText(b)}`)
					for (const b of s.blocked ?? []) lines.push(`      ✗ ${b.name} ${b.key}  ${b.reason}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地：每个建筑槽能建哪些建筑（桥的 get_buildable_starbase_buildings） ---- */
	add({
		name: 'stellaris_buildable_starbase_buildings',
		description:
			'Which buildings each building slot of a starbase accepts right now, with cost and build days — one block per slot; occupied slots also list what can replace the installed building. Pass building_key to ask about one building (it then reports the engine reason when refused, e.g. "空间站已建造有此建筑。" for a second copy) and slot to look at one slot. The probe only validates, nothing is queued. Starbase buildings are the slots after the module slots: outpost 0, starport 1, starhold 2, star fortress 3, citadel 4.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
				slot: { type: 'number', description: 'only this building slot (0-based); omit for all' },
				building_key: { type: 'string', description: 'ask about one building, e.g. "crew_quarters"' },
				include_blocked: { type: 'boolean', description: 'also list buildings the engine refuses, with the reason' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			const params = { starbase_id: args.starbase_id }
			if (typeof args.slot === 'number') params.slot = args.slot
			if (args.building_key) params.building_key = args.building_key
			if (args.include_blocked) params.include_blocked = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_starbase_buildings', params)
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const lines = [`${d.level}（starbase ${d.starbase_id}）· ${d.building_slots} 个建筑槽`]
				for (const s of d.slots ?? []) {
					const open = s.buildable ?? []
					const cur = s.occupied ? [s.current_name, s.current_key].filter(Boolean).join(' ') : ''
					if (args.building_key) {
						if (open.length) {
							const b = open[0]
							const tail = cur ? `（替换 ${cur}）` : ''
							lines.push(`  槽 ${s.slot}  可建  ${[b.name, b.key].filter(Boolean).join(' ')}  ${priceText(b)}${tail}`)
						} else {
							lines.push(`  槽 ${s.slot}  ✗ ${s.blocked?.[0]?.reason ?? '不可建'}`)
						}
						continue
					}
					lines.push(cur ? `  槽 ${s.slot}  已装 ${cur}；可换 ${open.length} 项：` : `  槽 ${s.slot}  空着，可建 ${open.length} 项：`)
					for (const b of open) lines.push(`      ${b.name} ${b.key}  ${priceText(b)}`)
					for (const b of s.blocked ?? []) lines.push(`      ✗ ${b.name} ${b.key}  ${b.reason}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地升级（CAddBuildableToQueueCommand + CBuildableStarbaseUpgrade） ---- */
	add({
		name: 'stellaris_starbase_upgrade',
		description:
			'Upgrade a starbase one level (outpost → starport → starhold → star fortress → citadel) through the normal construction queue — the same CBuildableStarbaseQueue path as modules/buildings, but the buildable is CBuildableStarbaseUpgrade and it carries only the TARGET LEVEL (no slot). dry_run=true only asks the engine: it reports the alloys and build days, or the refusal reason (e.g. "缺少合金 1232 / 工程学 星堡"), and spends nothing. An upgrade is NOT reversible: the alloys are paid when queued.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
				dry_run: { type: 'boolean', description: 'only validate and report cost/days, do not queue (recommended first)' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			const dry = args.dry_run === true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, dry ? 'get_starbase_upgrade' : 'queue_starbase_upgrade', {
					starbase_id: args.starbase_id,
				})
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const head = `${cnOf(d.current_level)} → ${cnOf(d.next_level ?? '')}`
				if (d.can_upgrade !== true) {
					// 引擎的拒绝是多条并列（✗ 未满足 / ✓ 已满足），原样保留标记、逐行输出
					const reasons = String(d.reason ?? '不能升级')
						.split('\n')
						.map((l) => l.trim())
						.filter(Boolean)
						.map((l) => `  ${l}`)
					return text([head, ...reasons].join('\n'))
				}
				const cost = `合金 ${d.cost?.alloys ?? '?'} · ${d.build_days ?? '?'} 天`
				return text(dry ? `${head}\n  可升  ${cost}（dry run，未下单）` : `${head}\n  已下单  ${cost}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地降级（原生 CDowngradeStarbaseCommand） ---- */
	add({
		name: 'stellaris_downgrade_starbase',
		description:
			'Downgrade a starbase with the native CDowngradeStarbaseCommand (token 0x3AE4, no GUI, no selection needed). It does NOT step down one level: from ANY level it drops straight to an OUTPOST (0 module slots, 0 building slots), and an outpost cannot be downgraded further — the engine refuses with "此空间站无法再进一步降级。". Unlike the upgrade this IS a real command, not a queued buildable, and it costs nothing. DESTRUCTIVE: everything installed — every module and building — is removed and NOT refunded. The command lands after a tick — read stellaris_starbase_details again only then.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			// 方舟基地不能降级（脚本 downgrade_potential = always = no），驿港也没有「降一级」这回事。
			// 先读等级再决定要不要投，省得投出一条会毁掉模块的命令。
			try {
				const det = await rpc(config.pipe || DEFAULTS.pipe, 'get_starbase_details', {
					starbase_id: args.starbase_id,
				})
				const lvl = String(det?.level ?? '')
				if (lvl.includes('arkship')) {
					return text(`starbase ${args.starbase_id} 是方舟基地（${lvl}），不能降级：脚本里 downgrade_potential 恒为 no`)
				}
				if (lvl.includes('waystation')) {
					return text(
						`starbase ${args.starbase_id} 是驿港（${lvl}）——没有「降一级」，它本来就是独立的一条链；要拆走 stellaris_dismantle_starbase`,
					)
				}
			} catch {}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'downgrade_starbase', {
					starbase_id: args.starbase_id,
				})
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				return text(`已下降级命令（starbase ${args.starbase_id}）：会直接降到哨站，装着的模块与建筑全部移除且不退款；走一 tick 后再读详情才会变`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 拆毁哨站（原生 CDismantleStarbaseCommand） ---- */
	add({
		name: 'stellaris_dismantle_starbase',
		description:
			'Dismantle a starbase with the native CDismantleStarbaseCommand (token 0x3A55, no GUI, no selection needed). Only an OUTPOST can be dismantled: an upgraded starbase is refused with "你不能直接拆解已升级的空间站。", and so is one in a system that holds one of your colonized planets ("你不能拆解此恒星基地，因为你在此恒星系有殖民行星。"). Nothing is refunded and the station is gone for good. The command lands after a tick — read stellaris_starbase_details again only then. To get rid of an upgraded starbase, downgrade it to an outpost first, then dismantle.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			// 方舟基地没有「拆」这回事：拆掉等于删掉殖民地。驿港可以拆（waystation_1 脚本里带
			// can_always_dismantle，其余由引擎判），所以只拦方舟。
			try {
				const det = await rpc(config.pipe || DEFAULTS.pipe, 'get_starbase_details', {
					starbase_id: args.starbase_id,
				})
				const lvl = String(det?.level ?? '')
				if (lvl.includes('arkship')) {
					return text(`starbase ${args.starbase_id} 是方舟基地（${lvl}）——它同时是殖民地，拆掉等于毁殖民地，不执行`)
				}
			} catch {}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'dismantle_starbase', {
					starbase_id: args.starbase_id,
				})
				if (d?.success !== true) {
					// 引擎的拒绝理由也是多条并列，逐行照抄
					const reasons = String(d?.error ?? JSON.stringify(d ?? {}))
						.split('\n')
						.map((l) => l.trim())
						.filter(Boolean)
						.map((l) => `  ${l}`)
					return text(['拆不了：', ...reasons].join('\n'))
				}
				return text(`已下拆毁命令（starbase ${args.starbase_id}）：走一 tick 后再读详情，这座基地会消失，不退款`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地建筑入队（CAddBuildableToQueueCommand + CBuildableStarbaseBuilding） ---- */
	add({
		name: 'stellaris_queue_starbase_building',
		description:
			'Queue a starbase BUILDING (the slots after the module slots) through the normal construction queue. It first asks stellaris_buildable_starbase_buildings where this building can go, then queues it in the first slot whose probe accepted it (or in `slot` when that slot is among them); one the engine refuses everywhere — e.g. a second crew_quarters, which answers "空间站已建造有此建筑。" — is refused here without spending alloys. Keys: crew_quarters, hydroponics_bay, resource_silo, target_uplink_computer, communications_jammer, disruption_field ... (ask the buildable tool for the live list).',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
				building_key: { type: 'string', description: 'building key, e.g. "hydroponics_bay"' },
				slot: { type: 'number', description: 'prefer this building slot (0-based); omit to take the first slot that accepts it' },
			},
			required: ['starbase_id', 'building_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number' || !args?.building_key) {
				return text('需要 starbase_id 和 building_key')
			}
			const pipe = config.pipe || DEFAULTS.pipe
			const wantSlot = typeof args.slot === 'number' ? args.slot : null

			let list
			try {
				const params = { starbase_id: args.starbase_id, building_key: args.building_key, include_blocked: true }
				if (wantSlot !== null) params.slot = wantSlot
				list = await rpc(pipe, 'get_buildable_starbase_buildings', params)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
			if (list?.success !== true) return text(`失败：${list?.error ?? JSON.stringify(list ?? {})}`)

			const slots = Array.isArray(list.slots) ? list.slots : []
			const candidates = slots.filter((s) => (s.buildable ?? []).length > 0)
			// 空槽优先：没点名槽位时先把空着的填掉，别把已装的建筑悄悄换掉
			candidates.sort((a, b) => (a.occupied === b.occupied ? a.slot - b.slot : a.occupied ? 1 : -1))
			if (!candidates.length) {
				const reasons = []
				for (const s of slots) {
					for (const b of s.blocked ?? []) reasons.push(`槽 ${s.slot} ✗ ${b.reason}`)
				}
				const head = wantSlot !== null
					? `槽 ${wantSlot} 建不了 ${args.building_key}：`
					: `${args.building_key} 在所有建筑槽都建不了：`
				return text(`${head}${reasons.length ? '\n  ' + [...new Set(reasons)].join('\n  ') : ''}`)
			}

			const target = wantSlot !== null ? (candidates.find((s) => s.slot === wantSlot) ?? candidates[0]) : candidates[0]
			const note = wantSlot !== null && target.slot !== wantSlot ? `（槽 ${wantSlot} 不可建，改到槽 ${target.slot}）` : ''
			try {
				const answer = await rpc(pipe, 'queue_starbase_building', {
					starbase_id: args.starbase_id,
					slot: target.slot,
					building_key: args.building_key,
				})
				return text(note ? `${JSON.stringify(answer)}\n${note}` : JSON.stringify(answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地建造队列（桥的 get_starbase_queue） ---- */
	add({
		name: 'stellaris_starbase_queue',
		description:
			'Read the starbase construction queue: one line per item with its key, kind (module / building / upgrade / ship), progress, total and remaining days, plus the runtime item_id. That item_id can be passed to stellaris_cancel_construction for a full refund. The queue holds what is PENDING — the installed modules/buildings are in stellaris_starbase_details instead.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_starbase_queue', {
					starbase_id: args.starbase_id,
				})
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const label = {
					starbase_module: '模块',
					starbase_building: '建筑',
					starbase_upgrade: '升级',
					ship: '舰船',
					defense_platform: '防御平台',
					colony_ship: '殖民船',
					clear_blocker: '清除障碍',
					district: '区划',
					building: '建筑',
					zone_specialization: '区划特化',
				}
				const lines = [`starbase ${d.starbase_id}`]
				const section = (title, block) => {
					if (!block || typeof block !== 'object' || !('count' in block)) return
					lines.push(`  ${title}（队列 ${block.queue_id} · 并行 ${block.simultaneous ?? '?'}）· ${block.count} 项`)
					if (!block.count) {
						lines.push('    （空）')
						return
					}
					let n = 0
					for (const it of block.items ?? []) {
						n += 1
						const name = it.type === 'starbase_upgrade' ? cnOf(it.key)
							: it.type === 'defense_platform' ? '防御平台'
							: it.item_name || it.key
						const pct = Math.round(it.progress_percent ?? 0)
						lines.push(
							`    ${n}. ${[name, it.key].filter(Boolean).join(' ')}  ${label[it.type] ?? it.type} · ${it.progress}/${it.total_days} 天` +
								`（余 ${it.remaining_days} 天，${pct}%）  item_id ${it.item_id}`,
						)
					}
				}
				if (d.is_arkship === true) {
					// 界面显示的只有殖民地那条（模块也排在这里）；6828 那条是隐藏队列，引擎认、UI 不显示
					section('方舟建造队列（界面显示这条：建筑 / 陆军 / 升级 / 模块）', d.ark)
					section('隐藏队列（starbase 自带，界面不显示）', d.ark_module)
				} else {
					section('建造', d.build)
					section('船坞', d.shipyard)
				}
				if (lines.length === 1) lines.push('  （队列不可读）')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地详情（桥的 get_starbase_details，对应殖民地的 get_planet_details） ---- */
	add({
		name: 'stellaris_starbase_details',
		description:
			'Read one starbase: level, its module slots and building slots, each with the installed key and localised name. Both lists are indexed by slot (empty slots come back with an empty key, so holes are real), and they only hold what is BUILT — a pending queue item is not listed. module_slots / building_slots are read from the starbase level: outpost 0/0, starport 2/1, starhold 4/2, star fortress 6/3, citadel 6/4. It also reports whether the next level can be upgraded right now (target level, alloys, build days, or the engine refusal reasons) — that part is a read-only probe. Use it BEFORE stellaris_queue_starbase_module: queueing into a slot that already holds a module returns success, charges the alloys, and silently builds nothing.',
		parameters: {
			type: 'object',
			properties: {
				starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases (or get_system.starbase.id)' },
			},
			required: ['starbase_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.starbase_id !== 'number') return text('需要 starbase_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_starbase_details', {
					starbase_id: args.starbase_id,
				})
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const slots = (used, total) => (typeof total === 'number' && total >= 0 ? `${used}/${total}` : `${used}/?`)
				const levelCn = {
					starbase_level_outpost: '哨站',
					starbase_level_starport: '星港',
					starbase_level_starhold: '星垒',
					starbase_level_starfortress: '星堡',
					starbase_level_citadel: '擎天堡',
				}
				const cn = (key) => levelCn[key] ?? key
				const headName = d.station_name
					? `${d.station_name}（${d.level_name || cn(d.level)}）`
					: d.level_name || cn(d.level)
				const lines = [
					`${headName} · starbase ${d.starbase_id} · ${d.level}`,
					`  模块 ${slots(d.module_count ?? 0, d.module_slots)}  建筑 ${slots(d.building_count ?? 0, d.building_slots)}`,
				]
				const list = (label, items) => {
					lines.push(`  ${label}：`)
					if (!items?.length) {
						lines.push('    （空）')
						return
					}
					for (const it of items) {
						if (!it.key) {
							lines.push(`    ${it.slot}  （空）`)
							continue
						}
						lines.push(`    ${it.slot}  ${[it.name, it.key].filter(Boolean).join(' ')}`)
					}
				}
				list('模块', d.modules)
				list('建筑', d.buildings)
				// 升级可用性：只让引擎校验一次，不排队、不扣钱
				try {
					const up = await rpc(config.pipe || DEFAULTS.pipe, 'get_starbase_upgrade', {
						starbase_id: args.starbase_id,
					})
					if (up?.success === true) {
						const head = `${cn(d.level)} → ${cn(up.next_level ?? '')}`
						if (up.can_upgrade === true) {
							lines.push(`  升级  可  ${head}  合金 ${up.cost?.alloys ?? '?'} · ${up.build_days ?? '?'} 天`)
						} else {
							// 引擎给的是多条并列（✗ 未满足 / ✓ 已满足），照原样逐行显示，不去掉标记
							lines.push(`  升级  不可  ${head}`)
							for (const reason of String(up.reason ?? '不能升级').split('\n')) {
								if (reason.trim()) lines.push(`      ${reason.trim()}`)
							}
						}
					}
				} catch {
					// 拿不到升级信息就只少这一行，不影响主数据
				}
				// 舰船配置（设计里的核心组件 + 各段组件）；只读，失败就少这一段
				try {
					const designId = d.station_design_id
					if (typeof designId === 'number' && designId !== 0xffffffff && designId >= 0) {
						const cfg = await rpc(config.pipe || DEFAULTS.pipe, 'probe_design_sections', { design_id: designId })
						if (cfg?.success === true) {
							const keys = new Set()
							for (const c of cfg.cores ?? []) if (c.component) keys.add(c.component)
							for (const s of cfg.sections ?? []) for (const slot of s.slots ?? []) if (slot.component) keys.add(slot.component)
							const loc = keys.size
								? await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: [...keys] }).catch(() => null)
								: null
							const nameOf = (k) => loc?.names?.[k] ?? k
							lines.push(`  配置（design ${designId}）`)
							const cores = (cfg.cores ?? []).map((c) => nameOf(c.component)).filter(Boolean)
							if (cores.length) lines.push(`    核心：${cores.join('、')}`)
							for (const s of cfg.sections ?? []) {
								const slots = s.slots ?? []
								if (!slots.length) continue
								const order = []
								const count = {}
								for (const slot of slots) {
									const k = slot.component || '(空)'
									if (!(k in count)) { count[k] = 0; order.push(k) }
									count[k] += 1
								}
								lines.push(`    段 ${s.index}：` + order.map((k) => `${nameOf(k)}×${count[k]}`).join('、'))
							}
						}
					}
				} catch {
					// 读不到配置就不显示这一段
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 恒星基地一览（桥的 get_our_starbases，对应殖民地的 stellaris_colonies） ---- */
	add({
		name: 'stellaris_starbases',
		description:
			'Overview of our own starbases in three groups (bridge get_our_starbases). ARKS — the Nomads ark base is a `CStarbase` whose station is the ark ship, i.e. the colony carrier, so it carries the ark module grid and building slots (and its colony_id is given). WAYSTATIONS (驿港) — the Nomads territorial station: 2/4/6 module slots, no building slots, does not make its system owned by us, yet megastructures may be built there; they live in CCountry+0x2E68 waystation_networks. ORDINARY starbases — settled empires, still decided by the system owner (a nomad has none). Each row: id, level, station name, module/building fill, system. Use stellaris_starbase_details for one starbase in full.',
		parameters: { type: 'object', properties: {}, additionalProperties: false },
		output: textOutput,
		execute: async () => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const d = await rpc(pipe, 'get_our_starbases', {})
				if (d?.success !== true) return text(`失败：${d?.error ?? JSON.stringify(d ?? {})}`)
				const fill = (used, cap) => (typeof cap === 'number' && cap >= 0 ? `${used}/${cap}` : `${used}/?`)
				let capLine = ''
				try {
					const st = await rpc(pipe, 'get_status')
					const sb = st?.stats?.starbases
					if (sb && typeof sb.used === 'number') capLine = `｜空间站容量 ${sb.used}/${sb.capacity}`
				} catch {}
				const total = (d.ark_count ?? 0) + (d.waystation_count ?? 0) + (d.starbase_count ?? 0)
				const lines = [
					`恒星基地 ${total} 座（方舟 ${d.ark_count ?? 0} / 驿港 ${d.waystation_count ?? 0} / 常规 ${d.starbase_count ?? 0}）${capLine}`,
				]
				if (!total) {
					lines.push('  （无）')
					return text(lines.join('\n'))
				}
				const line = (s, tail = '') =>
					`  id ${String(s.starbase_id).padEnd(4)} ${(s.level_name || cnOf(s.level)).padEnd(8)} · ` +
					`${(s.name || '?').padEnd(10)} 模块 ${fill(s.module_count, s.module_slots)}` +
					(s.building_slots > 0 ? `  建筑 ${fill(s.building_count, s.building_slots)}` : '') +
					`  星系 ${s.system_name || '?'}${tail}`
				if (d.arks?.length) {
					lines.push('  方舟（模块 + 建筑槽；与殖民地同一个载体）')
					for (const s of d.arks) lines.push(line(s, `  殖民地 ${s.colony_id}`))
				}
				if (d.waystations?.length) {
					lines.push('  驿港（对应定居帝国的恒星基地；只有模块槽，可造巨构）')
					for (const s of d.waystations) lines.push(line(s))
				}
				if (d.starbases?.length) {
					lines.push('  常规（星系归属 = 我们）')
					for (const s of d.starbases) lines.push(line(s))
				}
				lines.push('  （这些 id 可直接给 stellaris_starbase_details / _queue / queue_starbase_module）')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 解散舰队（引擎命令 CDisbandFleetsCommand） ---- */
	add({
		name: 'stellaris_disband_fleets',
		description:
			'Disband one or more of your fleets with the native CDisbandFleetsCommand (no GUI, no selection needed). Fleet ids come from get_military_fleets / get_civilian_fleets. This destroys the ships — unlike merging.',
		parameters: {
			type: 'object',
			properties: {
				fleet_ids: { type: 'array', items: { type: 'number' }, description: 'fleet ids to disband' },
			},
			required: ['fleet_ids'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const ids = Array.isArray(args?.fleet_ids) ? args.fleet_ids.filter((n) => typeof n === 'number') : []
			if (!ids.length) return text('需要至少一个 fleet_id')
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'disband_fleets', { fleet_ids: ids })
				return text(JSON.stringify(answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 合并舰队（引擎命令，不走 GUI） ---- */
	add({
		name: 'stellaris_merge_fleets',
		description:
			'Merge two or more of your fleets into one using the native CMergeFleetsCommand (no GUI clicking). Fleet ids come from stellaris_state (method=get_military_fleets) or the outliner.',
		parameters: {
			type: 'object',
			properties: {
				fleet_ids: { type: 'array', items: { type: 'number' }, description: 'two or more fleet ids to merge' },
			},
			required: ['fleet_ids'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const ids = Array.isArray(args?.fleet_ids) ? args.fleet_ids.filter((n) => typeof n === 'number') : []
			if (ids.length < 2) return text('需要至少两个 fleet_id')
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'merge_fleets', { fleet_ids: ids })
				return text(JSON.stringify(answer))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 舰队动作（不改构成）：姿态 / 跟随 / 紧急撤离 / 跃迁通道 / 寻路 / 找船 ----
	   2026-11-22 接入；桥方法早就在（README「舰船」章的表里逐条记着实测状态）。 */
	add({
		name: 'stellaris_ship_stance',
		description:
			'Set one fleet\'s engagement stance (bridge set_fleet_stance { fleet_id, stance }). Legal values depend on ship class: civilian fleets (science / construction / colony / transport) take passive or evasive, military fleets take passive or aggressive — the other one is accepted with success:true and then silently does nothing. The reply carries previous_stance, so the honest check is: set it twice and see whether previous_stance shows what you asked for.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				stance: { type: 'string', enum: ['passive', 'aggressive', 'evasive'], description: 'passive / aggressive / evasive' },
			},
			required: ['fleet_id', 'stance'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || !args?.stance) return text('需要 fleet_id 和 stance')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'set_fleet_stance', { fleet_id: args.fleet_id, stance: args.stance })
				if (d?.success !== true) return text(bridgeFail(d))
				const stuck = d.previous_stance && d.previous_stance === args.stance
				return text(
					`舰队 ${args.fleet_id}：${d.previous_stance} → ${d.stance_name ?? d.stance}（${d.stance}）` +
						`${stuck ? '｜已经是这一档，等于没动' : ''}\n${JSON.stringify(d)}`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* 轰炸姿态（内部名 ground support stance）：引擎自己的 CSetFleetGroundSupportStanceCommand。
	   2026-11-23 打通：字段在 CFleet+0x458（SDK 标的 0x968 在这一版不对），档位对象在
	   TGameDatabase_CBombardmentStanceDatabase；界面也是投这条命令（PostCommand 旁听抓到，vtable 0x23C0320）。
	   方舟舰也是舰队，所以这条对它同样适用。 */
	add({
		name: 'stellaris_set_bombardment_stance',
		description:
			'Read the options for, or set, a fleet\'s **bombardment stance** (internal name "ground support stance") ' +
			'through the engine\'s own CSetFleetGroundSupportStanceCommand. Call it without stance to list the ' +
			'stance keys this game has loaded (e.g. selective / indiscriminate / raiding / armageddon — the exotic ' +
			'ones are gated by tech or policy and the engine will refuse them). A refusal comes back with the ' +
			'engine reason; dry_run only validates. **Ark ships are fleets too**, so this works on them as well. ' +
			'The change is processed on the next tick — read it back with stellaris_fleets afterwards.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id (stellaris_fleets / get_system)' },
				stance: { type: 'string', description: 'stance key; omit to list what is available' },
				dry_run: { type: 'boolean', description: 'only validate, post nothing' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				if (typeof args?.stance !== 'string' || !args.stance) {
					const d = await rpc(pipe, 'get_bombardment_stances')
					if (d?.success !== true) return text(bridgeFail(d))
					const keys = (d.stances ?? []).map((s) => s.key)
					return text(`可选轰炸姿态 ${d.total ?? keys.length} 档：\n  ${keys.join(' / ')}\n（给 stance 才会去设；dry_run 只校验）`)
				}
				if (typeof args?.fleet_id !== 'number') return text('需要 fleet_id（只列档位时可以不传 stance）')
				const d = await rpc(pipe, 'set_bombardment_stance', {
					fleet_id: args.fleet_id,
					stance: args.stance,
					dry_run: args?.dry_run === true,
				})
				if (d?.success !== true) return text(bridgeFail(d))
				const head = args?.dry_run === true
					? `舰队 ${d.fleet_id}：可设为 ${d.stance}（dry run，未投）`
					: `舰队 ${d.fleet_id}：${d.previous || '?'} → ${d.stance}`
				return text(`${head}\n（过了下一 tick 才生效；用 stellaris_fleets 回读）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* 隐形：两条原生命令 + flags 位。2026-11-22 接，位是当日在局内定出来的（见桥端 README）。
	   is_cloaked 在 CFleet+0x1268 的 bit13（0x2000）：隐形时置 1，解除后清 0；
	   解除后 bit14 / bit17 会置起来（cloaking_cooldown 与 dirty_cloaking_strength）。 */
	add({
		name: 'stellaris_set_cloaking',
		description:
			'Activate or cancel one fleet\'s cloaking (bridge set_cloaking { fleet_id, active, dry_run }). ' +
			'active:true → CActivateFleetCloakingCommand (token 0x4210); false → CDeactivateFleetCloakingCommand ' +
			'(token 0x4211, whose forced country ref is sent empty, i.e. a voluntary decloak). The engine\'s own ' +
			'IsValid runs before anything is posted. It does check the ship class / cloaking device (a constructor is ' +
			'refused), but it does NOT check the cooldown — a fleet still cooling down passes IsValid. The reply ' +
			'carries the flag word at CFleet+0x1268 before the call; ' +
			'is_cloaked is bit 0x2000 of it. dry_run:true only validates. Reading the current state: ' +
			'stellaris_ship_orders prints a 隐形 line.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				active: { type: 'boolean', description: 'true = activate cloaking, false = cancel it' },
				dry_run: { type: 'boolean', description: 'only run IsValid, post nothing' },
			},
			required: ['fleet_id', 'active'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.active !== 'boolean') return text('需要 fleet_id 和 active（true / false）')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'set_cloaking', {
					fleet_id: args.fleet_id,
					active: args.active,
					dry_run: args.dry_run === true,
				})
				if (d?.success !== true) return text(bridgeFail(d))
				const word = Number(d.flags_before ?? d.flags ?? 0)
				const cloaked = (word & 0x2000) !== 0
				const dir = d.direction === 'activate' ? '启动隐形' : '取消隐形'
				if (d.dry_run === true) {
					return text(
						`舰队 ${args.fleet_id}｜${dir}：引擎校验通过（dry_run，未投递）｜投前 flags 0x${word.toString(16)}，隐形=${cloaked ? '是' : '否'}`,
					)
				}
				return text(
					`舰队 ${args.fleet_id}｜${dir} 已投递｜投前 flags 0x${word.toString(16)}（隐形=${cloaked ? '是' : '否'}）\n` +
						`读现在的状态用 stellaris_ship_orders（一行 隐形）。冷却 120 天，冷期内再启动会被引擎拒。`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_ship_follow',
		description:
			'Order a fleet to follow another one (bridge follow_fleet { fleet_id, target_fleet_id, attack, queue }). attack:true means follow AND attack. queue:true appends instead of replacing the fleet\'s current orders. This one has never been run in-game — watch the fleet\'s status after one tick.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'the fleet that follows' },
				target_fleet_id: { type: 'number', description: 'the fleet to follow' },
				attack: { type: 'boolean', description: 'true = follow and attack' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
			},
			required: ['fleet_id', 'target_fleet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.target_fleet_id !== 'number') return text('需要 fleet_id 和 target_fleet_id')
			const params = { fleet_id: args.fleet_id, target_fleet_id: args.target_fleet_id }
			if (args.attack === true) params.attack = true
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'follow_fleet', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`舰队 ${args.fleet_id} 开始跟随 ${args.target_fleet_id}${params.attack ? '（并攻击）' : ''}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_ship_mia',
		description:
			'Send fleets home the hard way (bridge fleet_mia { fleet_ids, type }): type return_home = normal return, emergency_ftl = emergency jump. Both put the fleet MIA for a while (measured 2026-11-23: return_home 35 days, emergency_ftl 17 days in that save); it works even without a battle. fleet_ids is an array.',
		parameters: {
			type: 'object',
			properties: {
				fleet_ids: { type: 'array', items: { type: 'number' }, description: 'fleet ids' },
				type: { type: 'string', enum: ['return_home', 'emergency_ftl'], description: 'return_home (default) or emergency_ftl' },
			},
			required: ['fleet_ids'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const ids = Array.isArray(args?.fleet_ids) ? args.fleet_ids.filter((n) => typeof n === 'number') : []
			if (!ids.length) return text('需要至少一个 fleet_id')
			const params = { fleet_ids: ids, type: args?.type === 'emergency_ftl' ? 'emergency_ftl' : 'return_home' }
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'fleet_mia', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`${ids.length} 支舰队 → ${params.type}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_use_bypass',
		description:
			'Send a fleet through a wormhole / gateway (bridge use_bypass { fleet_id, bypass_id, to_system_id, queue }). bypass_id is the CBypass object; to_system_id picks which connected system to exit at when the network has several. Omitting to_system_id makes the bridge find the counterpart itself (measured 2026-11-23; before that it failed with Unknown system id 4294967295).',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				bypass_id: { type: 'number', description: 'bypass id (wormhole / gateway)' },
				to_system_id: { type: 'number', description: 'exit system id; omit to let the bridge pick' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
			},
			required: ['fleet_id', 'bypass_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.bypass_id !== 'number') return text('需要 fleet_id 和 bypass_id')
			const params = { fleet_id: args.fleet_id, bypass_id: args.bypass_id }
			if (typeof args.to_system_id === 'number') params.to_system_id = args.to_system_id
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'use_bypass', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`舰队 ${args.fleet_id} 走通道 ${args.bypass_id}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})


	add({
		name: 'stellaris_find_path',
		description:
			'Ask how a given fleet would travel to a system (bridge find_path { fleet_id, to_system_id }): the route and the estimated days, computed from that fleet\'s own position, speed, FTL type and border access. Use it before move_fleet on a long trip.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id (route and travel time are fleet-specific)' },
				to_system_id: { type: 'number', description: 'destination system id' },
			},
			required: ['fleet_id', 'to_system_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.to_system_id !== 'number') return text('需要 fleet_id 和 to_system_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'find_path', { fleet_id: args.fleet_id, to_system_id: args.to_system_id })
				if (d?.error) return text(bridgeFail(d))
				const path = Array.isArray(d?.path) ? d.path.join(' → ') : null
				const days = d?.estimated_days ?? d?.days ?? d?.eta_days
				return text(`舰队 ${args.fleet_id} → 星系 ${args.to_system_id}${days !== undefined ? `，约 ${days} 天` : ''}${path ? `\n  ${path}` : ''}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_find_ships',
		description:
			'Read-only: find ships across the empire by design and/or system (bridge find_ships { design_id?, system_id?, limit? }). Handy when you know a design id from the fleet template and want to know where those hulls actually are. limit 1–2000 (default 200).',
		parameters: {
			type: 'object',
			properties: {
				design_id: { type: 'number', description: 'ship design id' },
				system_id: { type: 'number', description: 'only ships in this system' },
				limit: { type: 'number', description: 'max ships to list (default 200, capped 2000)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const params = {}
			if (typeof args?.design_id === 'number') params.design_id = args.design_id
			if (typeof args?.system_id === 'number') params.system_id = args.system_id
			if (typeof args?.limit === 'number') params.limit = args.limit
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'find_ships', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const ships = Array.isArray(d?.ships) ? d.ships : []
				const lines = [`找到 ${d?.count ?? ships.length} 艘`]
				for (const s of ships.slice(0, 60)) {
					const fleet = typeof s.fleet_id === 'number' ? `｜舰队 ${s.fleet_id}` : ''
					lines.push(`  [${s.ship_id}] ${s.name ?? '?'}｜设计 ${s.design_id ?? '?'}｜${s.system_name ?? `星系 ${s.system_id ?? '?'}`}${fleet}`)
				}
				if (ships.length > 60) lines.push(`  …另有 ${ships.length - 60} 艘（给 limit 收窄，或看 raw）`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 换设计 / 改装（改的是编制条目或船身上的升级目标，不造船） ---- */
	add({
		name: 'stellaris_set_fleet_template_design',
		description:
			'Point a fleet template entry at a different design (bridge set_fleet_template_design { fleet_id, from_design_id, to_design_id }). This changes what FUTURE reinforcements build — ships already in the fleet are untouched. For those use stellaris_set_ship_upgrade_design + stellaris_upgrade_fleet.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				from_design_id: { type: 'number', description: 'design id currently in the template' },
				to_design_id: { type: 'number', description: 'design id to build from now on' },
			},
			required: ['fleet_id', 'from_design_id', 'to_design_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.from_design_id !== 'number' || typeof args?.to_design_id !== 'number') {
				return text('需要 fleet_id、from_design_id、to_design_id')
			}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'set_fleet_template_design', {
					fleet_id: args.fleet_id,
					from_design_id: args.from_design_id,
					to_design_id: args.to_design_id,
				})
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`编制条目 ${args.from_design_id} → ${args.to_design_id}（舰队 ${args.fleet_id}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_set_ship_upgrade_design',
		description:
			'Mark what every ship in a fleet should upgrade INTO (bridge set_ship_upgrade_design { fleet_id, to_design_id }). It only writes the ships\' pending-upgrade field — nothing happens until stellaris_upgrade_fleet posts the refit at a shipyard with spare capacity.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				to_design_id: { type: 'number', description: 'design id to refit into' },
			},
			required: ['fleet_id', 'to_design_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.to_design_id !== 'number') return text('需要 fleet_id 和 to_design_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'set_ship_upgrade_design', { fleet_id: args.fleet_id, to_design_id: args.to_design_id })
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`舰队 ${args.fleet_id} 的待升级目标 → ${args.to_design_id}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_upgrade_fleet',
		description:
			'Post the refit order for a fleet at a shipyard (bridge upgrade_fleet { fleet_id, starbase_id?, target_design_id? }). The bridge takes the shipyard build queue out of the starbase itself. Pass target_design_id to write the ships\' upgrade target in the same call (otherwise the bridge uses what set_ship_upgrade_design already wrote). Measured 2026-11-16: ship 1464 went 50333052 → 1552 and finished in seconds under instant_build.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id' },
				starbase_id: { type: 'number', description: 'starbase id (its shipyard queue is used)' },
				target_design_id: { type: 'number', description: 'design id to refit into' },
			},
			required: ['fleet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number') return text('需要 fleet_id')
			const params = { fleet_id: args.fleet_id }
			if (typeof args.starbase_id === 'number') params.starbase_id = args.starbase_id
			if (typeof args.target_design_id === 'number') params.target_design_id = args.target_design_id
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'upgrade_fleet', params)
				if (d?.error) return text(bridgeFail(d))
				return text(`改装指令已投（舰队 ${args.fleet_id}${typeof args.starbase_id === 'number' ? `，基地 ${args.starbase_id}` : ''}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 舰船通用：移动 / 取消指令 / 读指令队列 / 设计列表 / 找星系（2026-11-22 接）
	   命名约定：改的是**某一支舰队的动作**、民用也能用 → 名字带 ship；军用舰队专有 → 带 fleet。 */
	add({
		name: 'stellaris_move_ship',
		description:
			'Move any fleet to a system (bridge move_fleet { fleet_id, system_id, queue }) — military, science, construction, colony or transport. queue:true appends to the queue instead of replacing the current orders, so several moves can be chained. This is the plain move; stellaris_explore is the science-ship policy that also surveys and dodges hostiles, and stellaris_orbit_planet parks on a body.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id (military or civilian)' },
				system_id: { type: 'number', description: 'destination system id (stellaris_find_systems can look it up)' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
			},
			required: ['fleet_id', 'system_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.system_id !== 'number') return text('需要 fleet_id 和 system_id')
			const params = { fleet_id: args.fleet_id, system_id: args.system_id }
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'move_fleet', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`舰队 ${args.fleet_id} → 星系 ${args.system_id}${params.queue ? '（排到队尾）' : ''}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_cancel_ship_orders',
		description:
			'Cancel every queued order of one or more fleets (bridge cancel_fleet_orders { fleet_ids }). The fleet stops where it is — what you want when it is already flying somewhere you changed your mind about.',
		parameters: {
			type: 'object',
			properties: {
				fleet_ids: { type: 'array', items: { type: 'number' }, description: 'fleet ids whose orders to cancel' },
			},
			required: ['fleet_ids'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const ids = Array.isArray(args?.fleet_ids) ? args.fleet_ids.filter((n) => typeof n === 'number') : []
			if (!ids.length) return text('需要至少一个 fleet_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'cancel_fleet_orders', { fleet_ids: ids })
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`${ids.length} 支舰队的指令已取消（${ids.join(', ')}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_ship_orders',
		description:
			'Read what a fleet is doing (bridge probe_fleet_orders { fleet_id }): the current order (has_order / token / target) and every queued order. token is the engine\'s order class id — 0x2CE4 is a plain move (whose target reads 0; the destination system is not stored in that field), 0x2CF0 a research-anomaly order, 0x4236 / 0x4237 the astral-rift ones — and target is the object id the order points at (planet / site) when the order has one. It also prints a 隐形 line read from CFleet+0x1268 (is_cloaked = bit 0x2000; the cooldown / dirty bits 0x4000 / 0x20000 come back too), which works for civilian ships as well — that is why the cloaking read lives here rather than in the military fleet list.',
		parameters: {
			type: 'object',
			properties: { fleet_id: { type: 'number', description: 'fleet id' } },
			required: ['fleet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number') return text('需要 fleet_id')
			try {
				const pipe = config.pipe || DEFAULTS.pipe
				const d = await rpc(pipe, 'probe_fleet_orders', { fleet_id: args.fleet_id })
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				const cur = d?.current_order ?? {}
				const lines = [
					`舰队 ${d.fleet_id}（${d.name ?? '?'}）｜当前 ${cur.has_order ? `指令 0x${Number(cur.token).toString(16).toUpperCase()} → 目标 ${cur.target}` : '无指令'}`,
					`队列 ${d.orders_count ?? 0} 条`,
				]
				// 隐形：+0x1268 的 flags 字（位表见工具说明与桥端 README）
				try {
					const f = await rpc(pipe, 'probe_fleet_flags', { fleet_id: args.fleet_id, offset: 0x1268, length: 8 })
					if (f?.success === true && Array.isArray(f.words)) {
						const word = Number(f.words[0] ?? 0) >>> 0
						const cloaked = (word & 0x2000) !== 0
						const cooldown = (word & 0x4000) !== 0
						const dirty = (word & 0x20000) !== 0
						lines.push(
							`隐形 ${cloaked ? '是' : '否'}` +
								`${cooldown ? '｜冷却中' : ''}${dirty ? '｜强度待重算' : ''}` +
								`（flags 0x${word.toString(16)}）`,
						)
					}
				} catch {
					/* 读不到就只报指令，不因为这个失败 */
				}
				for (const [i, o] of (Array.isArray(d?.orders) ? d.orders : []).entries()) {
					lines.push(`  ${i + 1}. ${typeof o === 'object' ? JSON.stringify(o) : o}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_ship_designs',
		description:
			'Ship designs, two scopes. scope:"own" (default) = the designs this country owns (bridge get_ship_designs): design_id / name / ship_size — these are the ids stellaris_build_ship and stellaris_set_ship_upgrade_design want. scope:"all" = the **global** design table (bridge get_all_designs), which also covers event ships (NAME_Derelict…) and every space-fauna design; ship_size narrows it (e.g. "crystal_ship_small_blue", "corvette"). Pass design_id to dump ONE design: its sections with the component installed in each slot plus the core components (reactor / FTL / thrusters / sensors / combat computer); components:true adds the section/component breakdown in list mode for scope "all". raw:true returns the bridge JSON.',
		parameters: {
			type: 'object',
			properties: {
				design_id: { type: 'number', description: 'one design id: print it in full instead of listing' },
				scope: { type: 'string', enum: ['own', 'all'], description: 'own (this country) or all (the global design table)' },
				ship_size: { type: 'string', description: 'scope "all": only this hull key (substring match)' },
				components: { type: 'boolean', description: 'scope "all" list mode: include each design\'s sections/core components' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const scope = args?.scope === 'all' ? 'all' : 'own'
			const detailId = typeof args?.design_id === 'number' ? args.design_id : null
			/* 两种来源的段/槽形状不同，统一成 {label, slots[{slot, name, key}]} + cores[{key, name}] */
			const normalise = (d) => {
				const sections = (Array.isArray(d?.sections) ? d.sections : []).map((sec) => ({
					label: sec.section ?? sec.name ?? '',
					slots: (Array.isArray(sec.slots) ? sec.slots : []).map((sl) => ({
						slot: sl.slot ?? sl.slot_name ?? '',
						key: sl.component_key ?? '',
						name: sl.component_name || sl.component_key || '(空)',
					})),
				}))
				const cores = (Array.isArray(d?.core_components) ? d.core_components : []).map((c) => ({
					key: c.component_key ?? '',
					name: c.component_name ?? c.component_key ?? '(空)',
				}))
				return { sections, cores }
			}
			const renderDetail = (d) => {
				const { sections, cores } = normalise(d)
				const head = `设计 ${d?.design_id ?? '?'}｜${d?.name ?? '?'}｜${d?.ship_size ?? '?'}`
				const lines = [head]
				if (d?.designable === false) lines.push('  （不可定制：不是本国的可编辑设计）')
				for (const sec of sections) {
					lines.push(`  段 ${sec.label || '?'}：`)
					for (const sl of sec.slots) {
						lines.push(`     [${sl.slot || '?'}] ${sl.name}${sl.key ? `（${sl.key}）` : ''}`)
					}
					if (!sec.slots.length) lines.push('     （无槽位）')
				}
				if (cores.length) {
					lines.push('  核心：')
					for (const c of cores) lines.push(`     ${c.name}${c.key ? `（${c.key}）` : ''}`)
				}
				if (!sections.length && !cores.length) lines.push('  （没有段/部件信息）')
				return lines
			}
			try {
				if (scope === 'own') {
					const params = {}
					if (detailId !== null) params.design_id = detailId
					const d = await rpc(pipe, 'get_ship_designs', params)
					if (d?.error) return text(bridgeFail(d))
					if (args?.raw === true) return text(JSON.stringify(d, null, 2))
					const designs = Array.isArray(d?.designs) ? d.designs : []
					if (detailId !== null) {
						const one = designs.find((x) => x.design_id === detailId) ?? designs[0]
						if (!one) return text(`设计 ${detailId} 不在本国设计列表里`)
						return text(renderDetail(one).join('\n'))
					}
					const lines = [`本国设计 ${d?.count ?? designs.length} 条`]
					for (const x of designs) lines.push(`  [${x.design_id}] ${x.name ?? '?'}｜${x.ship_size ?? '?'}`)
					lines.push('（银河里的事件船 / 兽群设计用 scope: "all"）')
					return text(lines.join('\n'))
				}
				const params = { limit: 5000 }
				if (typeof args?.ship_size === 'string' && args.ship_size) params.ship_size = args.ship_size
				if (args?.components === true || detailId !== null) params.components = true
				const d = await rpc(pipe, 'get_all_designs', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const designs = Array.isArray(d?.designs) ? d.designs : []
				if (detailId !== null) {
					const one = designs.find((x) => x.design_id === detailId)
					if (!one) return text(`全局设计库里没找到 ${detailId}（加 ship_size 过滤会把别的排掉）`)
					return text(renderDetail(one).join('\n'))
				}
				// 全局库的名字是本地化键，批量翻一次（翻译不出就留键）
				const keys = [...new Set(designs.map((x) => x.name).filter(Boolean))]
				const names = keys.length
					? ((await rpc(pipe, 'localize', { keys }).catch(() => null))?.names ?? {})
					: {}
				const label = (x) => {
					const hit = names[x.name]
					return hit && hit !== x.name ? `${hit}（${x.name}）` : x.name ?? '?'
				}
				const lines = [`全局设计 ${d?.count ?? designs.length} 条${args?.ship_size ? `（舰型含「${args.ship_size}」）` : ''}｜表容量 ${d?.table_capacity ?? '?'}`]
				const shown = designs.slice(0, 60)
				for (const x of shown) {
					lines.push(`  [${x.design_id}] ${label(x)}｜${x.ship_size ?? '?'}${x.designable ? '' : '｜不可定制'}`)
					if (args?.components === true) {
						const { sections, cores } = normalise(x)
						for (const sec of sections) {
							lines.push(`        ${sec.label}: ${sec.slots.map((sl) => sl.name).join(' / ') || '（无槽位）'}`)
						}
						if (cores.length) lines.push(`        核心: ${cores.map((c) => c.name).join(' / ')}`)
					}
				}
				if (designs.length > shown.length) lines.push(`  （另有 ${designs.length - shown.length} 条没印）`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 舰船部件目录（桥 get_ship_design_catalog / get_component_details）---- */
	add({
		name: 'stellaris_ship_parts',
		description:
			'The ship-part **catalog** (bridge get_ship_design_catalog): every component set with its localised name, set_key and variants (component_key / name / size / unlocked). That answers "what can this hull mount". Pass key (a set_key or a component_key) to get ONE set in detail from get_component_details — each variant with its power and type. unlocked_only:false also lists variants the empire has not unlocked; size filters the variants shown (small / medium / large / standard).',
		parameters: {
			type: 'object',
			properties: {
				key: { type: 'string', description: 'set_key or component_key to detail, e.g. DARK_MATTER_DEFLECTOR' },
				unlocked_only: { type: 'boolean', description: 'list only unlocked variants (default true)' },
				size: { type: 'string', description: 'only variants of this size, e.g. small / medium / large' },
				filter: { type: 'string', description: 'list mode: only sets whose key or name contains this text' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const sizeFilter = typeof args?.size === 'string' && args.size ? args.size.toLowerCase() : null
			const keepSize = (v) => !sizeFilter || String(v?.size ?? '').toLowerCase() === sizeFilter
			try {
				if (typeof args?.key === 'string' && args.key) {
					const d = await rpc(pipe, 'get_component_details', { key: args.key })
					if (d?.error) return text(bridgeFail(d))
					if (args?.raw === true) return text(JSON.stringify(d, null, 2))
					const variants = (Array.isArray(d?.variants) ? d.variants : []).filter(keepSize)
					const lines = [`${d?.localized_name ?? '?'}（${d?.set_key ?? args.key}）｜变体 ${d?.total_variants ?? variants.length}`]
					for (const v of variants) {
						const parts = [v.name ?? v.component_key, v.size, v.type, v.power !== undefined ? `power ${v.power}` : '', v.is_unlocked === false ? '未解锁' : '']
						lines.push(`  [${v.component_key}] ${parts.filter(Boolean).join('｜')}`)
					}
					if (!variants.length) lines.push('  （这个尺寸没有变体）')
					return text(lines.join('\n'))
				}
				const params = { unlocked_only: args?.unlocked_only !== false }
				const d = await rpc(pipe, 'get_ship_design_catalog', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const sets = Array.isArray(d?.components) ? d.components : []
				const needle = typeof args?.filter === 'string' ? args.filter.toLowerCase() : ''
				const lines = [
					`部件套 ${sets.length} 组｜舰型 ${Array.isArray(d?.ship_sizes) ? d.ship_sizes.length : '?'} 种（${(d?.ship_sizes ?? []).slice(0, 8).join(' / ')}）`,
				]
				let shown = 0
				for (const set of sets) {
					const variants = (Array.isArray(set.variants) ? set.variants : []).filter(keepSize)
					if (!variants.length) continue
					if (needle && !(`${set.set_key ?? ''} ${set.localized_name ?? ''}`.toLowerCase().includes(needle))) continue
					lines.push(`  ${set.localized_name ?? '?'}（${set.set_key ?? '?'}）：${variants.map((v) => v.name ?? v.component_key).join(' / ')}`)
					shown += 1
					if (shown >= 60) break
				}
				if (!shown) lines.push('  （没有匹配的部件套）')
				lines.push('给 key 看单组细节（含 power / type）')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 舰船设计器：写（2026-11-23 接；读的两只在上面）----
	   走引擎原生命令：create / update → CCreateOrUpdateShipDesignCommand，delete → CRemoveShipDesignCommand。
	   桥的 create 没有「原型」参数——原型固定是本国该舰级的**第一个**设计，段照它抄，只换槽里的部件与核心。
	   所以换段的路子只有一条：先让某个同舰级设计带着想要的段（空匣模板或存档注入），再来改。 */
	const designErrorText = (r) => {
		const e = r?.error
		if (typeof e === 'string') return e
		if (e && typeof e === 'object') return e.message ?? JSON.stringify(e)
		return JSON.stringify(r ?? {})
	}
	const findDesignByName = async (pipe, name) => {
		const d = await rpc(pipe, 'get_ship_designs', {}).catch(() => null)
		const list = Array.isArray(d?.designs) ? d.designs : []
		return list.find((x) => x.name === name) ?? null
	}
	const designEditsOf = (args) => ({
		slots: (Array.isArray(args?.slots) ? args.slots : []).filter((s) => s && typeof s.component_key === 'string'),
		cores: (Array.isArray(args?.core_components) ? args.core_components : []).filter((k) => typeof k === 'string'),
	})
	const slotEditsSchema = {
		type: 'array',
		description: 'slot edits — each one picks a slot (section_index/section_name + slot_index/slot_name) and the component to put in it',
		items: {
			type: 'object',
			properties: {
				section_index: { type: 'number', description: '0-based section index from stellaris_ship_designs' },
				section_name: { type: 'string', description: 'or the section key, e.g. BATTLESHIP_BOW_M2S4' },
				slot_index: { type: 'number', description: '0-based slot index inside that section' },
				slot_name: { type: 'string', description: 'or the slot key, e.g. SMALL_GUN_01 / SMALL_UTILITY_1' },
				component_key: { type: 'string', description: 'component to install, e.g. SMALL_RED_LASER (keys from stellaris_ship_parts)' },
			},
			required: ['component_key'],
			additionalProperties: false,
		},
	}
	/* 「装不进去」时补一句：槽的类型来自段模板，同名槽在不同段里可能不是一类东西 */
	const withFitHint = (msg) =>
		/does not fit slot/.test(msg)
			? `${msg}\n  （槽的类型由段模板的 component_slot.template 决定，见 common/section_templates。` +
				'例如 CORVETTE_MID_S2PD1 的 SMALL_GUN_01 是 point_defence_turret（点防槽），' +
				'同段的 02/03 才是 small_turret——部件没错，是槽不对）'
			: msg

	add({
		name: 'stellaris_create_ship_design',
		description:
			'Create ONE new ship design from a design of the same hull (bridge create_ship_design → CCreateOrUpdateShipDesignCommand). The prototype is your FIRST design of that hull — its sections and core components are copied — so you change components inside existing slots: give slots (section + slot + component_key) and/or core_components. A design identical to the prototype is refused by the game (it would replace it); a name already in use is refused (use stellaris_update_ship_design). The new id appears in stellaris_ship_designs (this tool reads it back and prints it); then stellaris_build_ship { design_id } builds it. Verified in-game 2026-11-23.',
		parameters: {
			type: 'object',
			properties: {
				ship_size: { type: 'string', description: 'hull key of the design to make, e.g. corvette / destroyer / battleship' },
				name: { type: 'string', description: 'new, unique design name' },
				slots: slotEditsSchema,
				core_components: {
					type: 'array',
					items: { type: 'string' },
					description: 'required-component keys to swap, e.g. CORVETTE_FISSION_REACTOR / HYPER_DRIVE_1',
				},
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			required: ['ship_size', 'name'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const shipSize = typeof args?.ship_size === 'string' ? args.ship_size.trim() : ''
			const name = typeof args?.name === 'string' ? args.name.trim() : ''
			if (!shipSize) return text('要给 ship_size（如 corvette / battleship）——原型取本国该舰级的第一个设计')
			if (!name) return text('要给 name：新的、不与已有设计重名（重名改用 stellaris_update_ship_design）')
			const { slots, cores } = designEditsOf(args)
			if (!slots.length && !cores.length) {
				return text('slots 与 core_components 至少改一处：与原型完全相同的设计会被游戏当成「替换已有设计」而拒掉')
			}
			try {
				const r = await rpc(pipe, 'create_ship_design', { ship_size: shipSize, name, slots, core_components: cores })
				if (r?.success !== true) return text(`失败：${withFitHint(designErrorText(r))}`)
				if (args?.raw === true) return text(JSON.stringify(r, null, 2))
				const back = await findDesignByName(pipe, name)
				const lines = [`已投：新设计「${name}」· ${shipSize}${r?.message ? `（${r.message}）` : ''}`]
				lines.push(
					back
						? `  读回：[${back.design_id}] ${back.name}｜${back.ship_size} —— 要造就 stellaris_build_ship { design_id: ${back.design_id} }`
						: '  读回：这一下还没进列表（命令排在主线程队列），下一次用 stellaris_ship_designs 看 id',
				)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_update_ship_design',
		description:
			'Change the components of ONE of your designs (bridge update_ship_design → CCreateOrUpdateShipDesignCommand). Same slots / core_components shape as stellaris_create_ship_design; the sections stay as they are. Renaming is refused — the game matches the design it replaces by name, so to rename, create a new design. The game saves the result as a NEW design id that replaces the old one AND points fleet templates that used it at the new design; ships already built keep flying with the old one until you refit them (stellaris_set_ship_upgrade_design + stellaris_upgrade_fleet). Verified in-game 2026-11-23.',
		parameters: {
			type: 'object',
			properties: {
				design_id: { type: 'number', description: 'design id from stellaris_ship_designs (must be one of yours)' },
				slots: slotEditsSchema,
				core_components: {
					type: 'array',
					items: { type: 'string' },
					description: 'required-component keys to swap',
				},
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			required: ['design_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			if (typeof args?.design_id !== 'number') return text('要给 design_id（本国设计，stellaris_ship_designs 里那一列）')
			const { slots, cores } = designEditsOf(args)
			if (!slots.length && !cores.length) return text('slots 与 core_components 至少改一处，否则没有变化')
			try {
				const r = await rpc(pipe, 'update_ship_design', { design_id: args.design_id, slots, core_components: cores })
				if (r?.success !== true) return text(`失败：${withFitHint(designErrorText(r))}`)
				if (args?.raw === true) return text(JSON.stringify(r, null, 2))
				const back = r?.name ? await findDesignByName(pipe, r.name) : null
				const lines = [`已投：改「${r?.name ?? `设计 ${args.design_id}`}」${r?.message ? `（${r.message}）` : ''}`]
				if (back && back.design_id !== args.design_id) {
					lines.push(`  新 id：[${back.design_id}]（旧的 ${args.design_id} 已被它替换，舰队编制会被游戏指到新的）`)
				} else if (back) {
					lines.push(`  读回：仍是 [${back.design_id}]`)
				}
				lines.push('  已造的船不会自己换装：要换走 stellaris_set_ship_upgrade_design + stellaris_upgrade_fleet')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_delete_ship_design',
		description:
			'Delete ONE of your designs (bridge delete_ship_design → CRemoveShipDesignCommand). Irreversible, but only the design goes: ships already built keep flying with it. It first checks the fleet templates: if any of your fleets still carries that design in its quota it refuses (those templates would point at a missing design) — pass force:true to go ahead anyway. Verified in-game 2026-11-23.',
		parameters: {
			type: 'object',
			properties: {
				design_id: { type: 'number', description: 'design id from stellaris_ship_designs' },
				force: { type: 'boolean', description: 'delete even when a fleet template still carries it' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			required: ['design_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			if (typeof args?.design_id !== 'number') return text('要给 design_id')
			const id = args.design_id
			try {
				const d = await rpc(pipe, 'get_ship_designs', {}).catch(() => null)
				const mine = (Array.isArray(d?.designs) ? d.designs : []).find((x) => x.design_id === id)
				if (d && !mine) return text(`设计 ${id} 不在本国设计列表里（也许已经删了，或那是别国的）`)
				const f = await rpc(pipe, 'get_fleets', {}).catch(() => null)
				const holders = []
				for (const fleet of Array.isArray(f?.fleets) ? f.fleets : []) {
					for (const e of Array.isArray(fleet?.designs) ? fleet.designs : []) {
						if (e?.design_id === id) holders.push(`${fleet.name}（编制 ${e.target_quota ?? '?'} / 实际 ${e.actual_count ?? '?'}）`)
					}
				}
				if (holders.length && args?.force !== true) {
					return text(
						`没有删：还有 ${holders.length} 支舰队的编制里带着它——${holders.join('、')}\n` +
							'删掉以后这些编制会指向一个不存在的设计。先把编制换掉（stellaris_set_fleet_template_design），' +
							'或加 force: true 硬删',
					)
				}
				const r = await rpc(pipe, 'delete_ship_design', { design_id: id })
				if (r?.success !== true) return text(`失败：${designErrorText(r)}`)
				if (args?.raw === true) return text(JSON.stringify(r, null, 2))
				const after = await rpc(pipe, 'get_ship_designs', {}).catch(() => null)
				const still = (Array.isArray(after?.designs) ? after.designs : []).some((x) => x.design_id === id)
				return text(
					`已投：删除设计 ${id}${mine ? `「${mine.name}」` : ''}${r?.message ? `（${r.message}）` : ''}\n` +
						(still ? '  读回：还在列表里（命令要下一 tick 才落地）' : '  读回：已经不在列表里'),
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_find_systems',
		description:
			'Look up a system id before stellaris_move_ship / build_outpost. Two modes: name:"监牢" matches every system name in the galaxy by case-insensitive substring (Chinese names include the whole string), listing id / name / owner / jumps from your capital; purpose:"outpost"|"deposit"|"unsurveyed" asks the bridge for the nearest systems that fit (optional resource:"minerals", from_system_id, limit 1–50).',
		parameters: {
			type: 'object',
			properties: {
				name: { type: 'string', description: 'system name (or part of it) to search for' },
				purpose: { type: 'string', enum: ['outpost', 'deposit', 'unsurveyed'], description: 'outpost / deposit / unsurveyed' },
				resource: { type: 'string', description: 'with purpose deposit: e.g. minerals / physics' },
				from_system_id: { type: 'number', description: 'measure jumps from here (default: your capital)' },
				limit: { type: 'number', description: 'max rows (default 10, capped 50)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const params = {}
			if (args?.name) params.name = String(args.name)
			if (args?.purpose) params.purpose = args.purpose
			if (args?.resource) params.resource = args.resource
			if (typeof args?.from_system_id === 'number') params.from_system_id = args.from_system_id
			if (typeof args?.limit === 'number') params.limit = args.limit
			if (!params.name && !params.purpose) return text('给 name（按名字找）或 purpose（outpost / deposit / unsurveyed）')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'find_systems', params)
				if (d?.error) return text(bridgeFail(d))
				const rows = Array.isArray(d) ? d : Array.isArray(d?.systems) ? d.systems : []
				const head = params.name
					? `按名字「${params.name}」找到 ${rows.length} 个星系（跳数从 ${d?.from_system_id ?? params.from_system_id ?? '首都'} 算）`
					: `按用途 ${d?.purpose ?? params.purpose} 找到 ${rows.length} 个星系（从 ${d?.from_system_id ?? '首都'} 起）`
				const lines = [head]
				for (const r of rows) {
					const owner = r.owner_id === null || r.owner_id === undefined ? '无主' : `country ${r.owner_id & 0xffffff}`
					const jt = (r.jumps ?? 0) >= 9999 ? '不可达（另一簇）' : `${r.jumps} 跳`
					lines.push(`  ${String(r.id).padEnd(5)} ${r.name}｜${jt}｜${owner}｜intel ${r.intel ?? '?'}`)
				}
				if (!rows.length) lines.push('  （没有匹配）')
				const skipped = d?.skipped && typeof d.skipped === 'object' ? Object.entries(d.skipped) : []
				if (skipped.length) {
					lines.push('  被排除的原因：')
					for (const [why, n] of skipped) lines.push(`    ${why} ×${n}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 全银河 / 邻域星图：get_galaxy_map（2026-11-22 取消 12 跳上限，不给 jumps = 全银河） ---- */
	add({
		name: 'stellaris_galaxy_map',
		description:
			'The galaxy map around a center system (bridge get_galaxy_map { center_system_id, jumps }): every system\'s id / name / owner / intel / jumps-from-center / flags, plus the hyperlane edges. **Omit jumps for the WHOLE galaxy** (the default is now -1 = unlimited; it used to be clamped to a 12-jump neighbourhood); give jumps:N for an N-jump ball around center_system_id (default centre: your capital). Flags: S surveyed, C colonized, B starbase, F your fleet present, A unresearched anomaly, K claimed by you, M megastructure. limit caps the printed rows; raw:true returns the bridge JSON.',
		parameters: {
			type: 'object',
			properties: {
				center_system_id: { type: 'number', description: 'centre system id (default: your capital)' },
				jumps: { type: 'number', description: 'radius in jumps; omit = whole galaxy' },
				limit: { type: 'number', description: 'max rows to print (default 40)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const params = {}
			if (typeof args?.center_system_id === 'number') params.center_system_id = args.center_system_id
			if (typeof args?.jumps === 'number') params.jumps = args.jumps
			try {
				const d = await rpc(pipe, 'get_galaxy_map', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d))
				const me = await rpc(pipe, 'get_player_country', {}).catch(() => null)
				const myId = typeof me?.country_id === 'number' ? me.country_id : null
				const cols = Array.isArray(d?.columns) ? d.columns : ['id', 'name', 'x', 'y', 'owner_id', 'intel', 'jumps', 'flags']
				const rows = (Array.isArray(d?.rows) ? d.rows : []).map((r) => Object.fromEntries(cols.map((c, i) => [c, r[i]])))
				const sameOwner = (a, b) => typeof a === 'number' && typeof b === 'number' && (a & 0xffffff) === (b & 0xffffff)
				const mine = rows.filter((r) => myId !== null && sameOwner(r.owner_id, myId)).length
				const others = rows.filter((r) => r.owner_id !== null && r.owner_id !== undefined && !sameOwner(r.owner_id, myId)).length
				const limit = typeof args?.limit === 'number' ? Math.max(1, args.limit) : 40
				const shown = [...rows].sort((a, b) => (a.jumps ?? 0) - (b.jumps ?? 0)).slice(0, limit)
				const scope = d?.jumps === -1 || d?.jumps === undefined ? '全银河' : `${d.jumps} 跳以内`
				const farthest = rows.reduce((m, r) => ((r.jumps ?? 0) < 9999 ? Math.max(m, r.jumps ?? 0) : m), 0)
				const lines = [
					`${scope} ${rows.length} 个星系（从 ${d.center_system_id} 算跳数，最远 ${farthest} 跳）｜我们 ${mine} / 别国 ${others} / 无主 ${rows.length - mine - others}`,
				]
				for (const r of shown) {
					const jumpsText = (r.jumps ?? 0) >= 9999 ? '不可达（另一簇）' : `${r.jumps} 跳`
					const who =
						r.owner_id === null || r.owner_id === undefined ? '无主' : sameOwner(r.owner_id, myId) ? '我们' : `country ${r.owner_id & 0xffffff}`
					lines.push(`  ${String(r.id).padEnd(5)} ${r.name}｜${jumpsText}｜${who}｜${r.flags ?? ''}`)
				}
				if (rows.length > shown.length) lines.push(`  另有 ${rows.length - shown.length} 个（加 limit，或 raw: true 拿全量）`)
				lines.push('标记：S 已勘察 / C 已殖民 / B 有基地 / F 有我的舰队 / A 有未研究异常 / K 我宣称过 / M 巨构')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 宇宙风暴（2026-11-23 接；内存直读，不解存档）----	   风暴表：base+0x311BE90 是一次间接（读出的指针才是 mgr）→ mgr+0x18 数组 / +0x20 容量，
	   槽步长 16、指针在 +0x08。cluster 段：+0x180 星系数组 / +0x188 容量 / +0x18C 条数，
	   位置 +0x198 x / +0x1A0 y / +0x1A8 高度 / +0x1B0 origin / +0x1B8 半径（i64 定点，÷100000）。
	   path 的偏移未定（SDK 的 +0x348 在 4.5.4 上读出来不是 id 数组），所以路径一列标「待测」；
	   判「星系在不在风暴里」用 cluster 的星系表，不用路径。 */
	add({
		name: 'stellaris_storms',
		description:
			'Live cosmic storms, read straight from memory (get_module_base + read_memory — no save parsing). For each storm: name, type, the **real centre** (x, y) and **radius** in galaxy-coordinate units, and the systems inside it (`cluster.objects`). This is what decides whether a colony has a storm modifier: look the colony\'s system up in circle, no geometry needed. Also gives path_index / age / days_till_next; the path array itself is NOT located yet (SDK offset +0x348 is wrong on 4.5.4) so the route is marked 待测. names:false skips resolving system names (one get_galaxy_map call). raw:true returns the JSON.',
		parameters: {
			type: 'object',
			properties: {
				names: { type: 'boolean', description: 'resolve the systems inside each storm to names (default true; one galaxy_map call)' },
				raw: { type: 'boolean', description: 'return the collected JSON instead of the table' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const readMem = async (address, length) => {
				if (!address || !length) return null
				const r = await rpc(pipe, 'read_memory', { address, length })
				if (!r?.success || typeof r.hex !== 'string') return null
				const bytes = r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16))
				return Buffer.from(bytes)
			}
			/* CPdxString：大小 +0x10、容量 +0x18；容量 < 16 内联，否则 +0x00 是字符指针 */
			const readPdx = async (addr) => {
				const b = await readMem(addr, 0x40)
				if (!b || b.length < 0x20) return ''
				const size = Number(b.readBigUInt64LE(0x10))
				if (!size || size > 256) return ''
				const cap = Number(b.readBigUInt64LE(0x18))
				if (cap < 16) return b.subarray(0, Math.min(size, 63)).toString('utf8')
				const ptr = Number(b.readBigUInt64LE(0))
				if (!ptr) return ''
				const d = await readMem(ptr, Math.min(size, 255) + 1)
				return d ? d.toString('utf8').replace(/\0.*$/, '') : ''
			}
			const fixed = (b, off) => Number(b.readBigInt64LE(off)) / 100000
			try {
				const base = Number((await rpc(pipe, 'get_module_base'))?.base)
				if (!base) return text('读不到 get_module_base（桥没加载？）')
				const holder = await readMem(base + 0x311BE90, 8)
				const mgr = holder ? Number(holder.readBigUInt64LE(0)) : 0
				if (!mgr) return text('风暴表指针为空：游戏还没进局？')
				const head = await readMem(mgr, 0x60)
				if (!head) return text('读不到风暴表头')
				const arr = Number(head.readBigUInt64LE(0x18))
				const cap = head.readUInt32LE(0x20)
				const storms = []
				for (let off = 0; off < cap; off += 512) {
					const n = Math.min(cap - off, 512)
					const chunk = await readMem(arr + off * 16, n * 16)
					if (!chunk) break
					for (let k = 0; k * 16 + 8 <= chunk.length; k += 1) {
						const st = Number(chunk.readBigUInt64LE(k * 16 + 8))
						if (!st) continue
						const b = await readMem(st, 0x400)
						if (!b || b.length < 0x1C0) continue
						const id = b.readUInt32LE(0x20)
						if (!id) continue
						const typePtr = Number(b.readBigUInt64LE(0x340))
						const typeKey = typePtr ? await readPdx(typePtr + 0x20) : ''
						const nameKey = await readPdx(st + 0x1D8 + 0x18)
						const objsPtr = Number(b.readBigUInt64LE(0x180))
						const objsCount = b.readUInt32LE(0x18C)
						let objects = []
						if (objsPtr && objsCount > 0 && objsCount < 512) {
							const ob = await readMem(objsPtr, objsCount * 4)
							if (ob) for (let j = 0; j < objsCount; j += 1) objects.push(ob.readUInt32LE(j * 4))
						}
						storms.push({
							id,
							type: typeKey,
							name_key: nameKey,
							x: fixed(b, 0x198),
							y: fixed(b, 0x1A0),
							radius: fixed(b, 0x1B8),
							path_index: b.readInt32LE(0x2C),
							age: b.readInt32LE(0x30),
							days_till_next: b.readInt32LE(0x3CC),
							objects,
						})
					}
				}
				if (!storms.length) return text('没读到风暴（本档可能没有激活的风暴）')
				const keys = [...new Set(storms.flatMap((s) => [s.name_key, s.type]).filter(Boolean))]
				const names = keys.length ? ((await rpc(pipe, 'localize', { keys }).catch(() => null))?.names ?? {}) : {}
				const L = (k) => (k && names[k] && names[k] !== k ? names[k] : k || '?')
				let systemName = new Map()
				if (args?.names !== false) {
					const g = await rpc(pipe, 'get_galaxy_map', {}).catch(() => null)
					const rows = Array.isArray(g?.rows) ? g.rows : []
					systemName = new Map(rows.map((r) => [r[0], r[1]]))
				}
				for (const s of storms) s.name = L(s.name_key)
				if (args?.raw === true) return text(JSON.stringify(storms.map((s) => ({ ...s, name: undefined })), null, 2))
				const lines = [`宇宙风暴 ${storms.length} 条（内存直读，路径一列待测）`]
				for (const s of storms) {
					lines.push(
						`  ${s.name}｜${s.type || '?'}｜中心 ${s.x.toFixed(2)}, ${s.y.toFixed(2)}｜半径 ${s.radius.toFixed(2)}` +
							`｜路径索引 ${s.path_index}（待测）｜年龄 ${s.age} 天｜距下一跳 ${s.days_till_next} 天`,
					)
					const shown = s.objects.map((id) => (systemName.get(id) ? `${systemName.get(id)}(${id})` : id)).join(' / ')
					lines.push(`    圈内 ${s.objects.length} 个星系：${shown || '（空）'}`)
				}
				lines.push('判断某殖民地是否在风暴里：把它的星系 id 放进上面某条的「圈内」列表即可')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 全银河的虫洞 / 星门（2026-11-22 接；只读）----
	   位置靠「每个星系的 bypass 数组」反推；「已探明 / 可用」读国家的 usable_bypasses / seen_bypasses。
	   虫洞是成对的（linked_to 一对），星门是网状（active_connections 连通块）——渲染时按这两种形状分组。 */
	add({
		name: 'stellaris_bypasses',
		description:
			'List every bypass in the galaxy (bridge get_bypasses): natural wormholes (one-to-one pairs), gateways (a network), L-gates and shroud tunnels — each with the system it sits in, its type, active/locked state, and whether YOU can use it (`usable`, from CCountry.usable_bypasses). **Wormholes must be explored before a fleet may enter them**: an unexplored wormhole comes back usable:false, and the way to open it is stellaris_explore_wormhole { bypass_id, fleet_id } (any ship class works, not science ships only). Pathfinding already routes fleets through a bypass once it is usable, so stellaris_use_bypass is only for forcing a specific exit. raw:true returns the bridge JSON.',
		parameters: {
			type: 'object',
			properties: {
				only: { type: 'string', enum: ['wormhole', 'gateway', 'lgate', 'all'], description: 'wormhole / gateway / lgate / all（默认 all）' },
				limit: { type: 'number', description: 'max rows to print per family (default 30)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_bypasses', {})
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d))
				const rows = Array.isArray(d?.bypasses) ? d.bypasses : []
				const sysName = (s) => s?.name || (s?.id !== undefined ? `星系 ${s.id}` : '?')
				const isGateway = (r) => String(r.type || '').includes('gateway')
				const only = args?.only ?? 'all'
				const isLgate = (r) => String(r.type || '').includes('lgate')
				let worms = rows.filter((r) => !isGateway(r))
				if (only === 'lgate') worms = worms.filter(isLgate)
				if (only === 'wormhole') worms = worms.filter((r) => !isLgate(r))
				const gates = rows.filter(isGateway)
				const usableOf = (list) => list.filter((r) => r.usable).length
				const head =
					only === 'lgate'
						? `bypass ${rows.length} 条：L-星门 ${worms.length}（可用 ${usableOf(worms)}）`
						: only === 'gateway'
							? `bypass ${rows.length} 条：星门 ${gates.length}（可用 ${usableOf(gates)}）`
							: `bypass ${rows.length} 条：虫洞类 ${worms.length}（可用 ${usableOf(worms)}）｜星门 ${gates.length}（可用 ${usableOf(gates)}）`
				const lines = [head]
				const cap = typeof args?.limit === 'number' ? Math.max(1, args.limit) : 30
				const byType = new Map()
				for (const r of rows) byType.set(r.type, (byType.get(r.type) ?? 0) + 1)
				lines.push(`  按类型：${[...byType.entries()].map(([t, n]) => `${t} ${n}`).join(' / ')}`)

				let pairCount = 0
				if (only !== 'gateway' && worms.length) {
					// 成对的只印一次：id < linked_to 的那一头，打印「A ↔ B」
					const printed = new Set()
					const pairs = []
					for (const r of worms) {
						const key = [r.id, r.linked_to ?? -1].sort((a, b) => a - b).join(':')
						if (printed.has(key)) continue
						printed.add(key)
						pairs.push(r)
					}
					lines.push('  虫洞 / 特殊通道：')
					pairCount = pairs.length
					for (const r of pairs.slice(0, cap)) {
						const peer = r.linked_to !== undefined ? (worms.find((x) => x.id === r.linked_to) ?? null) : null
						const far = r.linked_system ?? peer?.system
						const isWh = String(r.type || '').includes('wormhole')
						const tail = r.usable ? '已探明，可直接走' : isWh ? '未探明 ← 派任意一艘船（stellaris_explore_wormhole）' : '未启用 / 未探明'
						const extra = r.locked_by !== undefined ? `｜被 country ${r.locked_by} 锁 ${r.lock_days ?? '?'} 天` : ''
						lines.push(
							`    [${r.id}] ${r.type_name || r.type}｜${sysName(r.system)}${far ? ` ↔ ${sysName(far)}` : ''}｜${tail}${extra}`,
						)
					}
				}

				if (only !== 'wormhole' && only !== 'lgate' && gates.length) {
					// 星门成网：按 active_connections 做并查集，把连在一起的印成一组
					const parent = new Map(gates.map((r) => [r.id, r.id]))
					const find = (x) => (parent.get(x) === x ? x : (parent.set(x, find(parent.get(x))), parent.get(x)))
					const union = (a, b) => {
						if (!parent.has(a) || !parent.has(b)) return
						const ra = find(a)
						const rb = find(b)
						if (ra !== rb) parent.set(ra, rb)
					}
					for (const r of gates) for (const c of [...(r.active_connections ?? []), ...(r.connections ?? [])]) union(r.id, c)
					const groups = new Map()
					for (const r of gates) {
						const root = find(r.id)
						if (!groups.has(root)) groups.set(root, [])
						groups.get(root).push(r)
					}
					lines.push(`  星门网络 ${groups.size} 组：`)
					let n = 0
					for (const members of groups.values()) {
						n += 1
						if (n > cap) break
						const usable = members.filter((m) => m.usable).length
						lines.push(`    网 #${n}（${members.length} 口，可用 ${usable}）：${members.map((m) => sysName(m.system)).join(' / ')}`)
					}
					if (groups.size > n) lines.push(`    （另有 ${groups.size - n} 组没印；加 limit 或 raw: true）`)
				}
				const shownPairs = Math.min(pairCount, cap)
				if (only !== 'gateway' && pairCount > shownPairs) lines.push(`  （另有 ${pairCount - shownPairs} 条/对没印；加 limit 或 raw: true）`)
				lines.push('「可用」= 在我国的 usable_bypasses 里；虫洞要先探索（stellaris_explore_wormhole），星门要有起用的门')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 探虫洞（通用一轮：选虫洞 → 选船 → 单步投 explore_bypass）----
	   虫洞探索是**任意舰船通用**的动作（2026-11-22 玩家指出，实测工程船探完、两端都变可用）。
	   不必先移动：queue:true 的探索令在船很远、手上还有别的指令时也照收，fleet order 自己带它过去。
	   不给 bypass_id 就挑「离船最近的一条未探明虫洞」（用 find_path 问天数）。 */
	add({
		name: 'stellaris_explore_wormhole',
		description:
			'Explore ONE unexplored wormhole — the step without which a wormhole can never be traversed (bridge explore_bypass { fleet_id, bypass_id, queue }). **Wormhole exploration is a generic fleet action, not science-ship-only**: measured 2026-11-22, a construction ship completed it and the wormhole turned usable; military fleets and army transports can take the order too (any fleet works). Wormholes are one-to-one pairs, so either end works; exploring one end makes BOTH usable. Omitting bypass_id picks the nearest unexplored one, measured from that ship with find_path. fleet_id may be omitted: an idle CIVILIAN ship is picked (science ships first, then construction, then colony) — the automatic pick never touches military fleets or army transports, so pass fleet_id for those. A busy ship is refused unless force:true. The engine accepts the order even when the ship is far away and already has another order (queue:true appends, and the fleet-order pathing takes it there), so no separate move is posted — a move would replace the fleet\'s current orders. dry_run:true prints the plan and posts nothing. Check stellaris_bypasses a few ticks later: that wormhole turns usable:true.',
		parameters: {
			type: 'object',
			properties: {
				bypass_id: { type: 'number', description: 'wormhole bypass id from stellaris_bypasses; omit for the nearest unexplored one' },
				fleet_id: { type: 'number', description: 'science ship fleet id; omit to pick an idle one' },
				force: { type: 'boolean', description: 'allow taking a ship that is already busy' },
				queue: { type: 'boolean', description: 'append to the fleet queue (default true); false replaces the current orders' },
				dry_run: { type: 'boolean', description: 'print the plan, post nothing' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const bp = await rpc(pipe, 'get_bypasses', {})
				if (bp?.error) return text(bridgeFail(bp))
				const rows = Array.isArray(bp?.bypasses) ? bp.bypasses : []
				const worms = rows.filter((r) => String(r.type || '').includes('wormhole') && !r.usable)
				if (!worms.length) return text('没有「未探明」的天然虫洞了（stellaris_bypasses 里全是已探明或没有虫洞）')

				// 选船：虫洞探索是「任意舰船通用」的动作（2026-11-22 玩家指出并实测：工程船照样探完），
				// 所以池子里放科学船 / 工程船 / 殖民船，默认挑空闲的，优先科学船。
				const civ = await rpc(pipe, 'get_civilian_fleets', {})
				const pool = [
					...(Array.isArray(civ?.science_ships) ? civ.science_ships : []),
					...(Array.isArray(civ?.construction_ships) ? civ.construction_ships : []),
					...(Array.isArray(civ?.colony_ships) ? civ.colony_ships : []),
				]
				if (!pool.length) return text('没有可派的民用船（get_civilian_fleets 三类都为空）')
				let ship = null
				if (typeof args?.fleet_id === 'number') {
					ship = pool.find((s) => s.fleet_id === args.fleet_id) ?? { fleet_id: args.fleet_id, name: `舰队 ${args.fleet_id}`, status: '?' }
				} else {
					ship = pool.find((s) => s.status === '无指令') ?? null
					if (!ship) {
						if (args?.force !== true) {
							return text(`民用船都在忙：${pool.map((s) => `${s.name}「${s.status}」`).join(' / ')}\n要强行占用就加 force: true`)
						}
						ship = pool[0]
					}
				}
				if (ship.status && ship.status !== '无指令' && ship.status !== '?' && args?.force !== true) {
					return text(`舰队 ${ship.fleet_id}（${ship.name}）正忙（「${ship.status}」）；要强行占用就加 force: true`)
				}

				// 选虫洞：给了就用；没给就逐条问路，挑天数最小的
				let target = null
				let plan = ''
				if (typeof args?.bypass_id === 'number') {
					target = worms.find((w) => w.id === args.bypass_id) ?? rows.find((r) => r.id === args.bypass_id) ?? null
					if (!target) return text(`没有 id=${args.bypass_id} 的虫洞（stellaris_bypasses 里确认一下）`)
					if (target.usable) return text(`虫洞 ${target.id} 已经探明了，不用再探`)
				} else {
					let best = null
					for (const w of worms) {
						const sysId = w.system?.id
						if (typeof sysId !== 'number') continue
						const p = await rpc(pipe, 'find_path', { fleet_id: ship.fleet_id, to_system_id: sysId }).catch(() => null)
						const days = p?.estimated_days ?? p?.days ?? p?.eta_days ?? null
						if (!best || (typeof days === 'number' && (best.days === null || days < best.days))) best = { w, days }
					}
					if (!best) return text('选不出目标（虫洞没有星系信息？）')
					target = best.w
					plan = typeof best.days === 'number' ? `（预估 ${best.days} 天）` : ''
				}

				const sysId = target.system?.id
				const sysName = target.system?.name ?? `星系 ${sysId}`
				const head = `虫洞 [${target.id}] ${target.type_name ?? target.type}｜${sysName}${target.linked_system ? ` ↔ ${target.linked_system.name}` : ''}`
				const shipLine = `船：${ship.name}（${ship.fleet_id}）｜当前「${ship.status}」`
				if (args?.dry_run === true) {
					return text(`${head}\n${shipLine}\n计划：explore_bypass(${ship.fleet_id}, ${target.id}, queue)${plan}\n（dry_run：什么都没投）`)
				}

				// 单步即可：2026-11-22 实测，船不在那个星系时 queue:true 照样被引擎接受
				//（fleet order 自己会带船过去），所以不再先投 move_fleet —— 那会顶掉它手上的指令
				const queue = args?.queue !== false
				const ex = await rpc(pipe, 'explore_bypass', { fleet_id: ship.fleet_id, bypass_id: target.id, queue })
				if (ex?.success === false || ex?.error) {
					return text(`${bridgeFail(ex)}\n可退一步：先 move_fleet(${ship.fleet_id} → 星系 ${sysId}) 到位，再用 queue: false 单发 explore_wormhole`)
				}
				return text(`${head}\n${shipLine}\n已投探索令（queue）：船会先去 ${sysName}${plan}，到位后开始探索\n过几 tick 用 stellaris_bypasses 看这条是否变 usable`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 开局星系设置（只读）：get_galaxy_settings ----
	   读的是 CSettings::galaxy_config_sp（SGalaxyConfigSettingsSP），也就是「新游戏设置」那一套；
	   写的那只（set_galaxy_settings）只影响之后从菜单开的新局，不改已生成的银河，所以先不接。 */
	const GALAXY_FIELD_CN = {
		template: '银河模板',
		shape: '形状',
		clustered: '聚簇',
		lgate_enabled: 'L-星门',
		num_gateways: '星门数',
		num_wormhole_pairs: '虫洞对数',
		num_hyperlanes: '航道密度',
		num_guaranteed_colonies: '保底殖民地',
		colonizable_planet_odds: '可殖民率',
		resource_abundance: '资源丰度',
		primitive_odds: '原始文明',
		num_empires: '普通帝国',
		num_advanced_empires: '高级帝国',
		num_fallen_empires: '堕落帝国',
		num_marauder_empires: '掠夺者',
		num_nomad_empires: '游牧',
		advanced_starts_near_player: '高级帝国靠近玩家',
		random_empires: '随机普通帝国',
		random_advanced_empires: '随机高级帝国',
		random_fallen_empires: '随机堕落帝国',
		random_marauder_empires: '随机掠夺者',
		random_nomad_empires: '随机游牧',
		mid_game_start: '中期年',
		end_game_start: '后期年',
		victory_year: '胜利年',
		difficulty: '难度',
		difficulty_adjusted_ai_modifiers: 'AI 修正随难度',
		aggressiveness: 'AI 侵略性',
		scaling: '难度缩放',
		technology_difficulty_scale: '科技难度',
		fallen_empire_strength_scale: '堕落帝国强度',
		crisis_type: '天灾类型',
		crises: '天灾强度',
		voidworms_scaling: '虚空虫规模',
		cutholoids_scaling: '触手兽规模',
		technology: '科技花费',
		traditions: '传统花费',
		naval_capacity_mult: '海军容量倍率',
		command_limit_galaxy_mult: '指挥上限倍率',
		growth_scale: '人口成长',
		logistic_ceiling_v2: '后勤上限',
		logistics_fleet_upkeep_costs: '舰队后勤维护',
		logistics_planet_deficit_costs: '行星赤字代价',
		ironman: '铁人',
		caravaneers_enabled: '商队',
		cosmic_storm_devastation: '风暴破坏',
		cosmic_storm_early_game_spawn_chance_scale: '风暴·早期生成率',
		cosmic_storm_early_game_spawn_max_cap: '风暴·早期上限',
		cosmic_storm_late_game_spawn_chance_scale: '风暴·后期生成率',
		cosmic_storm_late_game_spawn_max_cap: '风暴·后期上限',
		cosmic_storm_mid_game_spawn_chance_scale: '风暴·中期生成率',
		cosmic_storm_mid_game_spawn_max_cap: '风暴·中期上限',
		cosmic_storm_spawn_cooldown_scale: '风暴·冷却倍率',
	}
	const GALAXY_GROUPS = [
		['银河', ['template', 'shape', 'clustered', 'lgate_enabled', 'num_gateways', 'num_wormhole_pairs', 'num_hyperlanes', 'num_guaranteed_colonies', 'colonizable_planet_odds', 'resource_abundance', 'primitive_odds']],
		['帝国', ['num_empires', 'num_advanced_empires', 'num_fallen_empires', 'num_marauder_empires', 'num_nomad_empires', 'advanced_starts_near_player', 'random_empires', 'random_advanced_empires', 'random_fallen_empires', 'random_marauder_empires', 'random_nomad_empires']],
		['时间线', ['mid_game_start', 'end_game_start', 'victory_year']],
		['难度与 AI', ['difficulty', 'difficulty_adjusted_ai_modifiers', 'aggressiveness', 'scaling', 'technology_difficulty_scale', 'fallen_empire_strength_scale']],
		['天灾与生物', ['crisis_type', 'crises', 'voidworms_scaling', 'cutholoids_scaling']],
		['宇宙风暴', ['cosmic_storm_devastation', 'cosmic_storm_early_game_spawn_chance_scale', 'cosmic_storm_early_game_spawn_max_cap', 'cosmic_storm_mid_game_spawn_chance_scale', 'cosmic_storm_mid_game_spawn_max_cap', 'cosmic_storm_late_game_spawn_chance_scale', 'cosmic_storm_late_game_spawn_max_cap', 'cosmic_storm_spawn_cooldown_scale']],
		['倍率与后勤', ['technology', 'traditions', 'naval_capacity_mult', 'command_limit_galaxy_mult', 'growth_scale', 'logistic_ceiling_v2', 'logistics_fleet_upkeep_costs', 'logistics_planet_deficit_costs']],
		['其他', ['ironman', 'caravaneers_enabled']],
	]
	add({
		name: 'stellaris_galaxy_settings',
		description:
			'The **new-game galaxy setup** as the running game still remembers it (bridge get_galaxy_settings → CSettings::galaxy_config_sp, 53 fields): galaxy template / shape / clustered, empire counts (normal / advanced / fallen / marauder / nomad), mid/end game years and victory year, difficulty, AI aggressiveness, scaling, crisis type and strength, wormhole pairs, gateways, hyperlane density, planet odds, resource abundance and the tech/tradition/navy/command/upkeep multipliers, plus ironman and caravaneers. A field whose value is null inherits the galaxy template\'s own value. **Read-only on purpose**: set_galaxy_settings would only affect the NEXT new game started from the menu (it never rewrites the galaxy you are already in), and starting a new game is not wired.',
		parameters: {
			type: 'object',
			properties: {
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged (all 53 fields + enum choices)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_galaxy_settings', {})
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const st = d?.settings ?? {}
				const choices = d?.choices ?? {}
				const enumText = (key, value) => {
					const list = Array.isArray(choices[key]) ? choices[key] : []
					const hit = list.find((c) => c.value === value)
					return hit ? `${hit.name}（${value}）` : String(value)
				}
				const SCALING_CN = { off: '关闭', mid: '中期', late: '后期' }
				const fmt = (key, v) => {
					if (v === null || v === undefined) return '跟随模板'
					if (typeof v === 'boolean') return v ? '是' : '否'
					if (key === 'scaling' && typeof v === 'string') return `${SCALING_CN[v] ?? v}（${v}）`
					if (Array.isArray(choices[key])) return enumText(key, v)
					return String(v)
				}
				const lines = [`开局星系设置（${Object.keys(st).length} 项，null = 跟随银河模板）`]
				const used = new Set()
				for (const [title, keys] of GALAXY_GROUPS) {
					const rows = keys.filter((k) => k in st).map((k) => `${GALAXY_FIELD_CN[k] ?? k} ${fmt(k, st[k])}`)
					for (const k of keys) used.add(k)
					if (rows.length) lines.push(`  ${title}：${rows.join('｜')}`)
				}
				const rest = Object.keys(st).filter((k) => !used.has(k))
				if (rest.length) lines.push(`  未归类：${rest.map((k) => `${k} ${fmt(k, st[k])}`).join('｜')}`)
				lines.push('写的那只（set_galaxy_settings）只影响之后新开的局，不改已生成的银河，所以没接')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 巨型结构（2026-11-22 接）：读全部 / 读单条 / 看工程船能建什么 / 下建造令 / 升级 ----
	   建造位置有两种，桥里也是两条路：
	     at_planet  → 给 planet_id（天体）
	     open space → 给 system_id + toward_system_id（朝向哪个邻星；x/y 由引擎按菜单同一算法算）
	   get_buildable_megastructures 会把「这个星系里合法的那几个位置」列成 sites，直接照着给。 */
	add({
		name: 'stellaris_megastructures',
		description:
			'List the megastructures you can see (bridge get_all_megastructures — own ones plus any in a system with intel >= 2): id / type / system / owner, and whether one is upgrading or being dismantled. Pass megastructure_id for one in detail (bridge get_megastructure: also the upgrade target, progress in days and, for your own, the upgrade requirements). type filters by type key (e.g. dyson_sphere), owner:"own" keeps only yours.',
		parameters: {
			type: 'object',
			properties: {
				megastructure_id: { type: 'number', description: 'one megastructure id: print it in detail' },
				owner: { type: 'string', enum: ['all', 'own'], description: 'own = only yours (default all)' },
				type: { type: 'string', description: 'only types whose key contains this text' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				if (typeof args?.megastructure_id === 'number') {
					const d = await rpc(pipe, 'get_megastructure', { megastructure_id: args.megastructure_id })
					if (d?.error) return text(bridgeFail(d))
					if (args?.raw === true) return text(JSON.stringify(d, null, 2))
					const lines = [`[${d.id}] ${d.name ? `${d.name}｜` : ''}${d.type_name || d.type}`]
					if (d.system) lines.push(`  所在：${d.system}（${d.system_id}）${d.planet_id ? `｜天体 ${d.planet_id}` : ''}`)
					if (d.owner) lines.push(`  所属：${d.owner}（${d.owner_id}）`)
					if (d.dismantling) lines.push('  正在拆除')
					if (d.upgrading) {
						const u = d.upgrading
						lines.push(`  升级中 → ${u.to_name || u.to}（已 ${u.progress_days ?? '?'} 天）${u.halted ? '｜已暂停' : ''}`)
					}
					return text(lines.join('\n'))
				}
				const d = await rpc(pipe, 'get_all_megastructures', {})
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const all = Array.isArray(d?.megastructures) ? d.megastructures : []
				const me = await rpc(pipe, 'get_player_country', {}).catch(() => null)
				const myId = typeof me?.country_id === 'number' ? me.country_id : null
				let rows = all
				if (args?.owner === 'own' && myId !== null) rows = rows.filter((x) => (x.owner_id ?? -1) === myId)
				if (typeof args?.type === 'string' && args.type) {
					const t = args.type.toLowerCase()
					rows = rows.filter((x) => String(x.type ?? '').toLowerCase().includes(t))
				}
				const upgrading = rows.filter((x) => x.upgrading).length
				const lines = [`巨型结构 ${rows.length} 个（可见 ${all.length} / 全银河 ${d?.table_total ?? '?'}）｜升级中 ${upgrading}`]
				for (const x of rows.slice(0, 60)) {
					const up = x.upgrading ? `｜↑${x.upgrading.to_name || x.upgrading.to}` : ''
					lines.push(`  [${x.id}] ${x.type_name || x.type}｜${x.system ?? '?'}｜${x.owner || '（无主）'}${x.planet_id ? `｜天体 ${x.planet_id}` : ''}${up}`)
				}
				if (rows.length > 60) lines.push(`  （另有 ${rows.length - 60} 个没印；用 type / owner 收窄，或 raw: true）`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_buildable_megastructures',
		description:
			'What one construction ship could build in one system (bridge get_buildable_megastructures): for each type, its placement mode — at_planet (a celestial body) or open space (a point toward a neighbouring system) — the cost, and the legal **sites** in that system. at_planet sites come back as planet_id + name; open-space sites as toward_system_id + the neighbour name (the engine derives x/y the same way the build menu does). When no site is left, the row carries the engine\'s most common refusal reason.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id' },
				system_id: { type: 'number', description: 'the system to look at (the ship should be able to reach it)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			required: ['fleet_id', 'system_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.system_id !== 'number') return text('要给 fleet_id 与 system_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_megastructures', {
					fleet_id: args.fleet_id,
					system_id: args.system_id,
				})
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const list = Array.isArray(d?.megastructures) ? d.megastructures : Array.isArray(d?.types) ? d.types : []
				// 花费是 {资源键: 数量}，资源键能走引擎本地化（alloys → 合金）
				const costKeys = [...new Set(list.flatMap((t) => Object.keys(t.cost ?? {})))]
				const costNames = costKeys.length
					? ((await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: costKeys }).catch(() => null))?.names ?? {})
					: {}
				const costText = (c) =>
					Object.entries(c ?? {})
						.map(([k, v]) => `${costNames[k] && costNames[k] !== k ? costNames[k] : k} ${v}`)
						.join('、')
				// 引擎给的 reason 里混了花费、基础值与百分比修正，只留前置条件与「缺少…」这类结论
				const oneLine = (text) => {
					const parts = String(text ?? '')
						.split(/\n+/)
						.map((x) => x.replace(/^✗\s*/, '').trim())
						.filter(Boolean)
					const keep = parts.filter((x) => !/^(花费|基础值)/.test(x) && !/[+-]\d+%/.test(x))
					return (keep.length ? keep : parts).join('；').replace(/；$/, '')
				}
				const lines = [`舰队 ${d?.fleet_id ?? args.fleet_id} 在星系 ${d?.system_id ?? args.system_id} 能建 ${list.length} 种`]
				for (const t of list) {
					const placement = t.placement ?? '?'
					const cost = t.cost && Object.keys(t.cost).length ? `｜${costText(t.cost)}` : ''
					lines.push(`  ${t.name || t.type}（${t.type}）｜${placement}${cost}`)
					const sites = Array.isArray(t.sites) ? t.sites : []
					if (sites.length) {
						const siteText = sites
							.slice(0, 8)
							.map((x) => (x.planet_id !== undefined ? `${x.name}（planet ${x.planet_id}）` : `朝 ${x.toward ?? '?'}（toward_system ${x.toward_system_id}）`))
							.join(' / ')
						lines.push(`      可选位置：${siteText}${sites.length > 8 ? ` …共 ${sites.length} 处` : ''}`)
					} else if (t.reason) {
						lines.push(`      ✗ 没位置：${oneLine(t.reason)}`)
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_build_megastructure',
		description:
			'Order ONE construction ship to build a megastructure (bridge build_megastructure). **Two placement forms**: at_planet types need planet_id (a celestial body); open-space types need system_id + toward_system_id (the neighbouring system the point leans toward — the engine derives x/y exactly as the build menu does). This tool first asks get_buildable_megastructures and refuses when the type is not on that list, or when the planet/toward pair is not one of its legal sites. Nothing is posted unless confirm:true.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id' },
				type: { type: 'string', description: 'megastructure type key, e.g. dyson_sphere' },
				system_id: { type: 'number', description: 'system (required for open-space placement)' },
				planet_id: { type: 'number', description: 'celestial body (at_planet placement)' },
				toward_system_id: { type: 'number', description: 'neighbouring system the open-space point leans toward' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing its orders' },
				confirm: { type: 'boolean', description: 'actually post (otherwise only the checks are shown)' },
			},
			required: ['fleet_id', 'type'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const fleet_id = args?.fleet_id
			const type = args?.type
			if (typeof fleet_id !== 'number' || typeof type !== 'string' || !type) return text('要给 fleet_id 与 type')
			try {
				const sysId = typeof args?.system_id === 'number' ? args.system_id : null
				const sight = sysId !== null ? await rpc(pipe, 'get_buildable_megastructures', { fleet_id, system_id: sysId }) : null
				const list = Array.isArray(sight?.megastructures) ? sight.megastructures : Array.isArray(sight?.types) ? sight.types : []
				const row = list.find((t) => t.type === type) ?? null
				if (sight && !row) return text(`这个星系里这支工程船不能建 ${type}（可建：${list.map((t) => t.type).join(' / ') || '无'}）`)
				const placement = row?.placement ?? (typeof args?.planet_id === 'number' ? 'at_planet' : 'open space')
				const openSpace = !/planet/i.test(String(placement))
				const lines = [`计划：${row?.name || type}（${type}）｜${placement}｜工程船 ${fleet_id}${sysId !== null ? `｜星系 ${sysId}` : ''}`]
				if (openSpace) {
					if (typeof args?.toward_system_id !== 'number') {
						return text(`${lines.join('\n')}\n✗ 这是「开放空间」型：要给 system_id + toward_system_id（朝向哪个邻星）`)
					}
					const sites = Array.isArray(row?.sites) ? row.sites : []
					if (sites.length && !sites.some((x) => x.toward_system_id === args.toward_system_id)) {
						return text(`${lines.join('\n')}\n✗ toward_system_id ${args.toward_system_id} 不在合法位置里（${sites.map((x) => `${x.toward}(${x.toward_system_id})`).join(' / ')}）`)
					}
				} else if (typeof args?.planet_id !== 'number') {
					return text(`${lines.join('\n')}\n✗ 这是「天体」型：要给 planet_id`)
				}
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真建请加 confirm: true）`)
				const params = { fleet_id, type, queue: args?.queue === true }
				if (typeof args?.planet_id === 'number') params.planet_id = args.planet_id
				if (sysId !== null) params.system_id = sysId
				if (typeof args?.toward_system_id === 'number') params.toward_system_id = args.toward_system_id
				const res = await rpc(pipe, 'build_megastructure', params)
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${bridgeFail(res)}`)
				return text(`${lines.join('\n')}\n已投建造令${res?.planet_id ? `（天体 ${res.planet_id}）` : ''}${res?.toward_system_id ? `（朝 ${res.toward_system_id}）` : ''}\n用 stellaris_megastructures 复核`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_upgrade_megastructure',
		description:
			'Upgrade one of your megastructures to the next stage (bridge upgrade_megastructure { megastructure_id, type }) — type is the upgrade target key (e.g. dyson_sphere_2). The engine validates cost and requirements; this tool first reads get_megastructure and refuses when the structure is not yours or is already upgrading. Nothing is posted unless confirm:true.',
		parameters: {
			type: 'object',
			properties: {
				megastructure_id: { type: 'number', description: 'megastructure id' },
				type: { type: 'string', description: 'upgrade target type key' },
				confirm: { type: 'boolean', description: 'actually post (otherwise only the checks are shown)' },
			},
			required: ['megastructure_id', 'type'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			if (typeof args?.megastructure_id !== 'number' || typeof args?.type !== 'string') return text('要给 megastructure_id 与 type')
			try {
				const now = await rpc(pipe, 'get_megastructure', { megastructure_id: args.megastructure_id })
				if (now?.error) return text(bridgeFail(now))
				const me = await rpc(pipe, 'get_player_country', {}).catch(() => null)
				const myId = typeof me?.country_id === 'number' ? me.country_id : null
				const lines = [`[${now.id}] ${now.name ? `${now.name}｜` : ''}${now.type_name || now.type}｜${now.system ?? '?'}`]
				if (myId !== null && now.owner_id !== myId) return text(`${lines.join('\n')}\n✗ 不是你的（所属 ${now.owner || now.owner_id}）`)
				if (now.upgrading) return text(`${lines.join('\n')}\n✗ 已经在升级（→ ${now.upgrading.to}）`)
				lines.push(`计划：升级到 ${args.type}`)
				if (args?.confirm !== true) return text(`${lines.join('\n')}\n（未投：要真升级请加 confirm: true）`)
				const res = await rpc(pipe, 'upgrade_megastructure', { megastructure_id: args.megastructure_id, type: args.type })
				if (res?.success === false || res?.error) return text(`${lines.join('\n')}\n引擎拒绝：${bridgeFail(res)}`)
				return text(`${lines.join('\n')}\n已投升级令\n用 stellaris_megastructures 复核`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 电弧熔炉候选星系（2026-11-22）----
	   电弧熔炉的收益看**星系内天体数量**（游戏文案：揭露星系内每颗行星、卫星和小行星的矿物，开采站产出 +25%），
	   所以按天体数从多到少排；再套脚本的两条前置（还没电弧熔炉、至少一颗熔融星球）。
	   给了 fleet_id 就让引擎逐个熔融星球验一次 IsValid，直接标出「现在能不能投、投在哪颗」。 */
	add({
		name: 'stellaris_arc_furnace_candidates',
		description:
			'Rank systems for an **orbital arc furnace** (bridge find_arc_furnace_systems). The furnace reveals a size-1 mineral deposit on every planet, moon and asteroid in its system and adds +25% mining-station output, so systems are sorted by **celestial-body count** (descending). Each row also carries **jumps** — hyperlane distance from an origin that defaults to the construction ship\'s own system (or your capital when no fleet_id is given); pass from_system_id to measure from elsewhere, and sort:"jumps" to rank by closeness (ties by body count, unreachable last). Two script preconditions are applied: the system must have no arc furnace yet, and must contain at least one molten planet (pc_molten). Pass fleet_id and the engine validates every molten planet in each candidate (the same IsValid the build menu uses), so buildable rows carry the exact planet to build on and the rest carry the engine\'s reason (for a nomad that is usually "the hyperlane needs a waystation").',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id: validate each candidate (recommended)' },
				min_bodies: { type: 'number', description: 'only systems with at least this many celestial bodies' },
				from_system_id: { type: 'number', description: 'measure jumps from here (default: the fleet system, else your capital)' },
				sort: { type: 'string', enum: ['bodies', 'jumps'], description: 'bodies (default) or jumps: nearest first' },
				limit: { type: 'number', description: 'how many rows (default 30, max 200)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const params = { limit: typeof args?.limit === 'number' ? args.limit : 30 }
			if (typeof args?.fleet_id === 'number') params.fleet_id = args.fleet_id
			if (typeof args?.min_bodies === 'number') params.min_bodies = args.min_bodies
			if (typeof args?.from_system_id === 'number') params.from_system_id = args.from_system_id
			if (args?.sort === 'jumps') params.sort = 'jumps'
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'find_arc_furnace_systems', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const rows = Array.isArray(d?.systems) ? d.systems : []
				const ok = rows.filter((x) => x.buildable === true)
				const no = rows.filter((x) => x.buildable !== true)
				const byJumps = args?.sort === 'jumps'
				const jumpsText = (x) => (x.jumps === null || x.jumps === undefined ? '距离 ?' : `距离 ${x.jumps} 跳`)
				const lines = [`候选星系 ${rows.length} 个（按${byJumps ? '距离近→远' : '天体数'}排序，起点星系 ${d?.from_system_id ?? '?'}）${typeof args?.fleet_id === 'number' ? `｜现在能投 ${ok.length} 个` : ''}`]
				const one = (x, mark) => {
					const head = `${mark}${x.system}  天体 ${x.bodies}（熔融 ${x.molten}）｜${jumpsText(x)}｜${x.owner || '无主'}`
					if (x.buildable === true) return `${head}｜可投：${x.site_name ?? '?'}（planet ${x.site_planet_id}）`
					if (x.buildable === false) {
						const why = String(x.reason ?? '').split('\n').map((t) => t.replace(/^✗\s*/, '').trim()).filter(Boolean)
						const keep = why.filter((t) => !/^(花费|基础值)/.test(t) && !/[+-]\d+%/.test(t))
						return `${head}｜不可投：${(keep[0] ?? why[0] ?? '?')}`
					}
					return head
				}
				if (ok.length) {
					lines.push('现在能投：')
					for (const x of ok) lines.push('  ' + one(x, '★ '))
				}
				if (no.length) {
					if (ok.length) lines.push('天体更多但暂时投不了：')
					// 同一个原因会重复很多次，按原因分组，每组先列几条再报数
					const whyOf = (x) => {
						const parts = String(x.reason ?? '')
							.split('\n')
							.map((t) => t.replace(/^✗\s*/, '').trim())
							.filter((t) => t && !/^(花费|基础值)/.test(t) && !/[+-]\d+%/.test(t))
						return parts[0] ?? '?'
					}
					const byReason = new Map()
					for (const x of no) {
						const k = whyOf(x)
						if (!byReason.has(k)) byReason.set(k, [])
						byReason.get(k).push(x)
					}
					for (const [reason, group] of byReason) {
						for (const x of group.slice(0, 4)) lines.push(`  ${x.system}  天体 ${x.bodies}（熔融 ${x.molten}）｜${jumpsText(x)}｜${x.owner || '无主'}`)
						if (group.length > 4) lines.push(`    …另有 ${group.length - 4} 个同原因`)
						lines.push(`    原因：${reason}`)
					}
				}
				if (!rows.length) lines.push('  （没有符合条件的星系：要有熔融星球、且还没有电弧熔炉）')
				lines.push('提示：给 fleet_id 才会跑引擎校验；不给时只是脚本条件筛出来的候选')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 戴森云候选星系（2026-11-22）----
	   戴森云只吃**恒星自身产出**，且只能建在**单星星系**（脚本：is_multiple_star = no，
	   且不能是黑洞 / 中子星 / 脉冲星）。产出按玩家定的等价折算：1 稀有文物 = 1 战略资源 = 2 科研点 = 8 能量币。 */
	add({
		name: 'stellaris_dyson_swarm_candidates',
		description:
			'Rank systems for a **Dyson swarm** (bridge find_dyson_swarm_systems). A swarm feeds only on the **star\'s own output**, and the script allows it only in a **single-star system** that is not a black hole / neutron star / pulsar (`is_multiple_star = no`, plus `NOT is_star_class = sc_black_hole|sc_neutron_star|sc_pulsar`). Each row gives the star class, the star\'s deposits as energy / research / strategic / artifacts, and a **value** in energy-credit equivalents using: 1 minor artifact = 1 strategic resource = 2 research points = 8 energy credits. sort:"value" (default) ranks by that value; sort:"jumps" ranks by distance from the construction ship (or from_system_id). Pass fleet_id and the engine validates the build order on the star itself.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'construction ship fleet id: validate each candidate' },
				sort: { type: 'string', enum: ['value', 'jumps'], description: 'value (default) or jumps: nearest first' },
				from_system_id: { type: 'number', description: 'measure jumps from here (default: the fleet system, else your capital)' },
				limit: { type: 'number', description: 'how many rows (default 30, max 200)' },
				raw: { type: 'boolean', description: 'return the bridge JSON unchanged' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const want = typeof args?.limit === 'number' ? args.limit : 30
			// 要校验就得把整个候选集拉回来（可投的往往排在值/距离后面），再在本地分组截断
			const params = { limit: typeof args?.fleet_id === 'number' ? 200 : want }
			if (typeof args?.fleet_id === 'number') params.fleet_id = args.fleet_id
			if (typeof args?.from_system_id === 'number') params.from_system_id = args.from_system_id
			if (args?.sort === 'value') params.sort = 'value'
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'find_dyson_swarm_systems', params)
				if (d?.error) return text(bridgeFail(d))
				if (args?.raw === true) return text(JSON.stringify(d, null, 2))
				const rows = Array.isArray(d?.systems) ? d.systems : []
				const ok = rows.filter((x) => x.buildable === true)
				const no = rows.filter((x) => x.buildable !== true)
				const nums = (x) => {
					const parts = []
					if (x.energy) parts.push(`能量 ${x.energy}`)
					if (x.research) parts.push(`科研 ${x.research}`)
					if (x.strategic) parts.push(`战略 ${x.strategic}`)
					if (x.artifacts) parts.push(`文物 ${x.artifacts}`)
					if (x.minerals) parts.push(`矿物 ${x.minerals}`)
					return parts.length ? `｜${parts.join('·')}` : ''
				}
				const line = (x, mark) => {
					const jumps = x.jumps === null || x.jumps === undefined ? '距离 ?' : `距离 ${x.jumps} 跳`
					const val = Number.isFinite(x.value) ? `值 ${x.value}` : ''
					const tail = x.buildable === true ? `｜可投：天体 ${x.site_planet_id}` : ''
					return `${mark}${x.system}  ${x.star_class_name || x.star_class}｜${val}${nums(x)}｜${jumps}｜${x.owner || '无主'}${tail}`
				}
				const lines = [
					`候选星系 ${rows.length} 个（按${args?.sort === 'value' || args?.sort === undefined ? '恒星产出折算值' : '距离'}排序，起点星系 ${d?.from_system_id ?? '?'}）${typeof args?.fleet_id === 'number' ? `｜现在能投 ${ok.length} 个` : ''}`,
				]
				if (ok.length) {
					lines.push('现在能投：')
					for (const x of ok.slice(0, want)) lines.push('  ' + line(x, '★ '))
				}
				if (no.length) {
					if (ok.length) lines.push('其余候选（暂时投不了）：')
					const whyOf = (x) => {
						const parts = String(x.reason ?? '')
							.split('\n')
							.map((t) => t.replace(/^✗\s*/, '').trim())
							.filter((t) => t && !/^(花费|基础值)/.test(t) && !/[+-]\d+%/.test(t))
						return parts[0] ?? '（未校验）'
					}
					const byReason = new Map()
					for (const x of no) {
						const k = typeof args?.fleet_id === 'number' ? whyOf(x) : '（未校验）'
						if (!byReason.has(k)) byReason.set(k, [])
						byReason.get(k).push(x)
					}
					for (const [reason, group] of byReason) {
						for (const x of group.slice(0, Math.max(1, Math.min(4, want)))) lines.push('  ' + line(x, '  '))
						if (group.length > 4) lines.push(`    …另有 ${group.length - 4} 个`)
						if (typeof args?.fleet_id === 'number') lines.push(`    原因：${reason}`)
					}
				}
				if (!rows.length) lines.push('  （没有单星、非黑洞/中子星/脉冲星、且还没有戴森云的星系）')
				lines.push('折算：1 稀有文物 = 1 战略资源 = 2 科研点 = 8 能量币')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 手动切换殖民地设计 ---- */
	add({
		name: 'stellaris_set_colony_designation',
		description:
			'Change one colony designation (colony type) by key, e.g. col_habitat_energy (发电站), col_habitat_gestalt_farming (水培居住站), col_capital_machine. Uses the native CSetColonyTypeCommand; the bridge resolves the key to the engine designation object itself. The engine does NOT validate potential: an illegal designation is accepted, then silently reverted to automatic at the next monthly update. So after setting one, let a month pass and read the designation again — if it changed back, that key was illegal for this colony; pick another from the same family — use stellaris_list_colony_designations with for_planet: true to get the valid family for that colony.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				key: { type: 'string', description: 'designation key, e.g. col_habitat_energy' },
			},
			required: ['planet_id', 'key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (!args?.planet_id || !args?.key) return text('需要 planet_id 和 key')
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'set_colony_designation', {
					planet_id: args.planet_id,
					key: args.key,
				})
				if (answer?.success) {
					return text(`已切换：${args.key}\n${JSON.stringify(answer)}`)
				}
				return text(`失败：${JSON.stringify(answer)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 建造与开发：查可建项、下单、取消（桥方法都是本项目自己写的，见 README） ---- */

	/** 资源键 → 中文名；没列到的键原样打印，宁可难看也不猜 */
	const RESOURCE_ZH = {
		minerals: '矿物', alloys: '合金', energy: '能量', influence: '影响力',
		consumer_goods: '消费品', food: '食物', unity: '凝聚力', research: '研究',
		volatile_motes: '易爆微粒', exotic_gases: '异星瓦斯', rare_crystals: '稀有水晶',
		living_metal: '活金属', zro: '零元素', sr_dark_matter: '暗物质',
	}
	const costText = (cost) => {
		const parts = Object.entries(cost ?? {}).filter(([, v]) => typeof v === 'number' && v > 0)
		if (!parts.length) return '（无花费）'
		return parts.map(([k, v]) => `${RESOURCE_ZH[k] ?? k} ${Number.isInteger(v) ? v : v.toFixed(1)}`).join(' + ')
	}
	const priceText = (row) => {
		const bits = [costText(row?.cost)]
		if (row?.build_days) bits.push(`${row.build_days} 天`)
		return bits.join(' · ')
	}
	/** 舰船设计的一行名字：黑鱼 1552（护卫舰） */
	const shipRow = (row) => `${row?.name ?? '?'} ${row?.design_id ?? '?'}（${row?.ship_size_name ?? row?.ship_size ?? '?'}）`
	const bridgeFail = (result) => `失败：${result?.error ?? JSON.stringify(result ?? {})}`
	/**
	 * 引擎的拒绝理由不是一句话，而是一棵带缩进的条件树：`✓ 社会学 行星防御` 这些"满足的"也照样列出来，
	 * 多个原因并列（`✗ 缺少易爆微粒 100` / `✗ 工程学 固态全息铸模` / `该区划特化无法支持该建筑。`），
	 * 嵌套条件有 3–6 个空格的前导缩进。
	 * 所以不能压成一行、也不能逐行 trim——那会把并列关系和嵌套层级一起抹掉。这里只加统一缩进，原样保留。
	 */
	const reasonBlock = (reason, indent) => {
		const rows = String(reason ?? '').replace(/\r\n/g, '\n').split('\n')
		while (rows.length && !rows[0].trim()) rows.shift()
		while (rows.length && !rows[rows.length - 1].trim()) rows.pop()
		if (!rows.length) return [`${indent}（引擎没给原因）`]
		return rows.map((row) => (row.trim() ? indent + row : ''))
	}

	/** 建造类动作的返回：先一行看得懂的，再附原始 JSON（后面的调用还要 id） */
	const renderAction = (answer) => {
		if (answer?.success !== true) return bridgeFail(answer)
		const bits = []
		if (answer.building_name) bits.push(`${answer.building_name} ${answer.building_key}`)
		if (answer.district_name) bits.push(`${answer.district_name} ${answer.district_key}`)
		if (answer.zone_name) bits.push(`${answer.zone_name} ${answer.zone}`)
		if (answer.build_days) bits.push(`${answer.build_days} 天`)
		if (answer.cost) bits.push(costText(answer.cost))
		const head = bits.length ? bits.join(' · ') : '成功'
		return `${head}${answer.message ? `（${answer.message}）` : ''}\n${JSON.stringify(answer)}`
	}

	const buildAction = (name, method, description, properties, required) =>
		add({
			name,
			description,
			parameters: { type: 'object', properties, required, additionalProperties: false },
			output: textOutput,
			execute: async (args) => {
				try {
					return text(renderAction(await rpc(config.pipe || DEFAULTS.pipe, method, args ?? {})))
				} catch (error) {
					return text(`bridge: ${error.message}`)
				}
			},
		})

	add({
		name: 'stellaris_buildable_buildings',
		description:
			'What a colony can build right now, one block per zone slot: the zone (and the zone_id that stellaris_build_building wants), how many of its building slots are used, and every building the game would accept there with cost and build days. Pass building_key to ask about ONE building instead — the bridge then answers zone by zone with can_build and the game\'s own refusal reason. That reason is the engine\'s requirement tree, one condition per line (✓ met / ✗ unmet) and sometimes 20+ lines; zones that share the same reason are grouped under one block. The per-zone list only shows what you can afford at this moment, so an empty list usually means "not enough resources", not "nothing is possible"; use building_key when unsure.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
				building_key: { type: 'string', description: 'ask about this one building, e.g. building_research_lab_1' },
				zone_id: { type: 'number', description: 'only look at this zone (zone_id from a previous call)' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			const params = typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id }
			if (args.building_key) params.building_key = args.building_key
			if (typeof args.zone_id === 'number') params.zone_id = args.zone_id
			try {
				const result = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_buildings', params)
				if (result?.success !== true) return text(bridgeFail(result))
				const sub = []
				if (result.colony_id !== undefined) sub.push(`殖民地 ${result.colony_id}`)
				if (result.building_key) sub.push(`${result.building_name ?? ''} ${result.building_key}`.trim())
				const lines = [`行星 ${result.planet_id}${sub.length ? `（${sub.join('，')}）` : ''}`]
				// building_key 模式下 zones[] 是"逐槽位判断"，理由可能很长且多个槽位完全相同：
				// 可建的排在前面，不可建的按理由分组（同一理由只印一次），理由原样保留缩进与并列。
				const canBuild = []
				const blocked = new Map()
				for (const z of result.zones ?? []) {
					const head = `  ${z.zone_name ?? ''} ${z.zone}（zone_id ${z.zone_id}）`
					if (z.can_build !== undefined) {
						if (z.can_build) {
							canBuild.push(`${head}  可建  ${priceText(z)}`)
						} else {
							const why = String(z.reason ?? '')
							const group = blocked.get(why) ?? []
							group.push(`${z.zone}（zone_id ${z.zone_id}）`)
							blocked.set(why, group)
						}
						continue
					}
					const used = z.max_buildings ? `${z.buildings ?? 0}/${z.max_buildings}` : `${z.buildings ?? 0}`
					lines.push(`${head}  已用 ${used}  可建 ${z.buildable?.length ?? 0}`)
					for (const b of z.buildable ?? []) lines.push(`      ${b.name} ${b.key}  ${priceText(b)}`)
					if (!(z.buildable ?? []).length) lines.push('      （现在一项也建不了：多半是资源不够，用 building_key 问具体那一个）')
				}
				lines.push(...canBuild)
				for (const [why, zones] of blocked) {
					const label = zones.length > 3 ? `${zones.slice(0, 3).join('、')} 等 ${zones.length} 个槽位` : zones.join('、')
					lines.push(`  ✗ 不可建：${label}`)
					lines.push(...reasonBlock(why, '      '))
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_buildable_districts',
		description:
			'District types the game would queue on this colony right now, with how many of each are already built, cost and build days. Pass district_key to ask about one type and get the refusal reason. Like the building list this only contains what is affordable at this moment — an empty list means "cannot pay", not "maxed out"; the built counts and the construction queue are the truth about capacity.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
				district_key: { type: 'string', description: 'ask about this one district, e.g. district_hab_science' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			const params = typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id }
			if (args.district_key) params.district_key = args.district_key
			try {
				const result = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_districts', params)
				if (result?.success !== true) return text(bridgeFail(result))
				if (result.district) {
					const d = result.district
					if (d.can_build) {
						return text(`行星 ${result.planet_id}\n${d.name} ${d.key}  已建 ${d.built} 座  可建  ${priceText(d)}`)
					}
					return text(
						[
							`行星 ${result.planet_id}`,
							`${d.name} ${d.key}  已建 ${d.built} 座   ✗ 不可建：`,
							...reasonBlock(d.reason, '    '),
						].join('\n'),
					)
				}
				const lines = [`行星 ${result.planet_id}（殖民地 ${result.colony_id}）`]
				const current = (result.current ?? []).map((c) => `${c.name} ${c.built} 座`)
				if (current.length) lines.push(`已建：${current.join(' · ')}`)
				lines.push('可建（只列现在付得起的）：')
				for (const d of result.buildable ?? []) {
					lines.push(`    ${d.name} ${d.key}  已建 ${d.built} 座  ${priceText(d)}`)
				}
				if (!(result.buildable ?? []).length) lines.push('    （一项也没有：多半是合金不够）')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* 区划特化：一个区划槽位能换成哪几种 zone */
	add({
		name: 'stellaris_available_district_zones',
		description:
			'For every district on the colony, each of its zone slots and which zone specialisations it could be changed into (name, key, cost, build days). The list never contains the zone already installed in that slot, and each slot has its own set. include_blocked also returns the zones the game refuses with its reason. Use the district_id, slot and zone_key with stellaris_set_district_zone.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				district_type: { type: 'string', description: 'only this district type, e.g. district_hab_science' },
				include_blocked: { type: 'boolean', description: 'also list zones the game refuses, with the reason' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			const params = typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id }
			if (args.district_type) params.district_type = args.district_type
			if (args.include_blocked) params.include_blocked = true
			try {
				const result = await rpc(config.pipe || DEFAULTS.pipe, 'get_available_district_zones', params)
				if (result?.success !== true) return text(bridgeFail(result))
				const lines = [`行星 ${result.planet_id}（殖民地 ${result.colony_id}）`]
				for (const d of result.districts ?? []) {
					lines.push(`${d.district_name} ${d.district_type}（district_id ${d.district_id}）`)
					for (const s of d.slots ?? []) {
						const current = s.zone ? `当前 ${s.zone_name} ${s.zone}（zone_id ${s.zone_id}）` : '当前空槽'
						const options = s.buildable_zones ?? []
						lines.push(`    槽位 ${s.slot}  ${current}  可换 ${options.length} 种`)
						if (options.length) {
							lines.push(`        ${options.map((z) => `${z.name} ${z.zone}`).join('｜')}`)
							lines.push(`        价目：${priceText(options[0])}`)
						}
						for (const b of s.blocked_zones ?? []) lines.push(`        ✗ ${b.zone}  ${b.reason}`)
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_build_building',
		description:
			'Queue one building on a colony through the normal construction queue (minerals + build days, exactly like clicking it in the planet view). Get building_key from stellaris_buildable_buildings. zone_id is that tool\'s zone_id — the zone to put it in — NOT a building slot number, and NOT the grid cell either: the game always fills the lowest free cell of that zone, so a new building cannot be aimed at a specific slot. omit it and the bridge uses the first zone that accepts the building. While a district or zone is still under construction its slots cannot be changed — let the build finish (one tick) first. It refuses with the game\'s reason when prerequisites or resources are missing.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
				building_key: { type: 'string', description: 'building type key, e.g. building_research_lab_1' },
				zone_id: { type: 'number', description: 'target zone (zone_id from stellaris_buildable_buildings); optional' },
			},
			required: ['building_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			// 桥的 build_building 只认 slot_index（它不读 zone_id，传了也不报错，会静默落到第一个能建的槽位）。
			// get_buildable_buildings 才是两个名字都认。所以这里必须改名，不能原样转发。
			if ((typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number') || !args?.building_key)
				return text('需要（planet_id 或 colony_id）和 building_key')
			const params = args?.colony_id
				? { colony_id: args.colony_id, building_key: args.building_key }
				: { planet_id: args.planet_id, building_key: args.building_key }
			if (typeof args.zone_id === 'number' && args.zone_id >= 0) params.slot_index = args.zone_id
			try {
				return text(renderAction(await rpc(config.pipe || DEFAULTS.pipe, 'build_building', params)))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	buildAction(
		'stellaris_upgrade_building',
		'upgrade_building',
		'Upgrade one already-built building to its next level. building_id is the building object id from stellaris_planet_structures (the save-side colony_report has no ids), upgrade_to_key is the next building type key. Queues like any construction, so it costs resources and takes days.',
		{
			planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
			colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
			building_id: { type: 'number', description: 'building object id, not its type key' },
			upgrade_to_key: { type: 'string', description: 'building key of the next level, e.g. building_research_lab_2' },
		},
		['building_id', 'upgrade_to_key'],
	)

	buildAction(
		'stellaris_build_district',
		'build_district',
		'Queue one more district of a type on a colony (the district count is its level). district_key comes from stellaris_buildable_districts; cost and days are in the reply. current/built does not change until the district finishes — the construction queue item carries remaining_days.',
		{
			planet_id: { type: 'number', description: 'planet id of the colony' },
			district_key: { type: 'string', description: 'district type key, e.g. district_hab_science' },
		},
		['planet_id', 'district_key'],
	)

	buildAction(
		'stellaris_demolish_district',
		'demolish_district',
		'Remove one district of a type from the colony (CDestroyDistrictCommand) — instant, no build time and no refund, and not undoable. Pass the district TYPE key (e.g. district_hab_energy), not a district instance id: districts of one type are identical and only counted, so there is no "which one". While some of the type remains, buildings are untouched; removing the LAST one of a type clears that type\'s zone specialisations and every building on them. The colony reads one tick behind: right after a successful call the district still shows as built with 0 zone slots, then disappears — do not take that as a failure. If the district is still under construction, cancel it with stellaris_cancel_construction instead.',
		{
			planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
			colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
			district_key: { type: 'string', description: 'district type key to remove one of' },
		},
		['district_key'],
	)

	/* ---- 清除障碍：clear_blocker（入建造队列，instant_build 下几秒完成） ---- */
	/* 注意：get_clearable_blockers 只给 can_clear 布尔与一句泛化的「未满足清理前提条件」，
	   缺什么要发 clear_blocker 才问得出来（引擎会回「Cannot be queued: ✗ 社会学 …」）。 */
	add({
		name: 'stellaris_clear_blocker',
		description:
			'Queue the clearance of one planet blocker (clear_blocker): it goes into the colony\'s normal construction queue (typically 180–270 days, instant_build finishes it in seconds), costs energy as listed in clear_cost, and the blocker is replaced by its swap_type feature (or just disappears). deposit_id and deposit_key come from get_clearable_blockers (or get_planet_details\' blockers). get_clearable_blockers only says can_clear true/false — **the reason it is false comes back only from this call**, e.g. "Cannot be queued: ✗ 社会学 海洋生态管理" means the ocean-ecology tech is missing.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				deposit_id: { type: 'number', description: 'blocker deposit id from get_clearable_blockers' },
				deposit_key: { type: 'string', description: 'blocker key, e.g. d_toxic_kelp' },
			},
			required: ['planet_id', 'deposit_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' || typeof args?.deposit_id !== 'number') {
				return text('需要 planet_id 和 deposit_id（从 get_clearable_blockers 拿）')
			}
			const params = { planet_id: args.planet_id, deposit_id: args.deposit_id }
			if (args.deposit_key) params.deposit_key = args.deposit_key
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'clear_blocker', params)
				if (d?.success !== true) return text(bridgeFail(d))
				return text(
					`${d.deposit_name ?? d.deposit_key ?? '障碍'} 清除已入队（队列 ${d.queue_id}）` +
						`${d.deposit_key ? ` ${d.deposit_key}` : ''}\n${JSON.stringify(d)}`,
				)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 招募陆军：桥的 recruit_planet_army ----
	   坑：桥不传 species_id 时会从 colony+0xF68 取物种，在 colony 0（14 号）上读到无效值，
	   引擎就把整条命令判成「prerequisites not met」。所以这只工具自己定物种：
	   先试国家的建国物种，再逐个试帝国内的其它物种，直到入队成功。 */
	add({
		name: 'stellaris_recruit_army',
		description:
			'Recruit one army on a colony (bridge recruit_planet_army { planet_id, army_key, species_id }). army_key comes from get_planet_armies\' recruitable_armies (e.g. machine_assault_1 for a machine empire). The bridge\'s own species fallback is unreliable — on colony 0 it reads an invalid species and the engine then answers the generic "Cannot be queued (prerequisites not met)" — so this tool always passes species_id itself: the empire founder species first, then the other empire species, and the reply says which one worked. With instant_build the army appears at once (the queue empties); if the colony has deploy_in_orbit on, it goes straight into a transport fleet instead of standing on the ground. Defense armies cannot be recruited this way.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark (it has no planet)' },
				army_key: { type: 'string', description: 'army type key from get_planet_armies recruitable_armies, e.g. machine_assault_1' },
				species_id: { type: 'number', description: 'species to recruit from; omit to try the founder species first' },
			},
			required: ['army_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if ((typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number') || !args?.army_key) {
				return text('需要（planet_id 或 colony_id）和 army_key（army_key 从 get_planet_armies 的 recruitable_armies 拿）')
			}
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const who = typeof args?.colony_id === 'number' ? args.colony_id : args.planet_id
				const target = typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id }
				if (typeof args.species_id === 'number') {
					const d = await rpc(pipe, 'recruit_planet_army', { ...target, army_key: args.army_key, species_id: args.species_id })
					return text(d?.success === true
						? `${args.army_key} 招募已入队（${typeof args?.colony_id === 'number' ? '殖民地' : '行星'} ${who}，物种 ${args.species_id}）\n${JSON.stringify(d)}`
						: bridgeFail(d))
				}
				const sp = await rpc(pipe, 'get_species', { mode: 'empire' })
				const founder = sp?.founder_species_id
				const ids = (sp?.species ?? []).map((s) => s.species_id).filter((id) => typeof id === 'number')
				// 顺序：建国物种 → 其他物种 → **不带物种**。
				// 特殊陆军（如 machine_assault_3 巨型战体）根本不带物种 —— 引擎自己那条的 +0x10 就是
				// 0xFFFFFFFF，硬填主族反而被拒（2026-11-22 实测）。
				const order = [...new Set([founder, ...ids].filter((id) => typeof id === 'number')), null]
				let last = null
				for (const id of order) {
					const d = await rpc(
						pipe,
						'recruit_planet_army',
						id === null ? { ...target, army_key: args.army_key } : { ...target, army_key: args.army_key, species_id: id },
					)
					if (d?.success === true) {
						const which = id === null ? '不带物种' : id === founder ? '物种 ' + id + '，建国物种' : '物种 ' + id
						return text(`${args.army_key} 招募已入队（${typeof args?.colony_id === 'number' ? '殖民地' : '行星'} ${who}，${which}）\n${JSON.stringify(d)}`)
					}
					last = d
				}
				return text(`${bridgeFail(last)}\n（试过物种 ${order.map((x) => x ?? '无').join(', ')}，都不行）`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 陆军：读 / 解散 / 部署开关 / 上船 / 登陆（2026-11-22 接入）
	   桥的五个方法同一个来源（OutlinerManager 的 army_access），planet_id 指行星，不是殖民地 id。
	   方舟殖民地挂在舰上、没有 CPlanet::colony，所以游牧档会回「Planet has no active colony」。 */
	add({
		name: 'stellaris_planet_armies',
		description:
			'Read the armies standing on one colony (bridge get_planet_armies): the overview (stationed counts, garrison/assault power, deploy_in_orbit, recruitment queue), every stationed army (army_id / name / type_key / is_defense / health / power / embarked) and the recruitable catalog (key / name / build_time_days) that stellaris_recruit_army wants. planet_id is a PLANET id. Nomad ark colonies answer "Planet has no active colony" — armies stand on planets, not on the ark.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_planet_armies', typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id })
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				const o = d?.overview ?? {}
				const n1 = (v) => (typeof v === 'number' ? Math.round(v * 10) / 10 : (v ?? '?'))
				const lines = [
					`行星 ${d?.planet_id ?? args.planet_id}（${d?.planet_name ?? '?'}）｜驻军 ${o.total_stationed_armies ?? 0}（守备 ${o.defense_armies_count ?? 0} / 突击 ${o.assault_armies_count ?? 0}）｜总战力 ${n1(o.total_power)}｜部署到轨道 ${o.deploy_in_orbit ? '是' : '否'}｜招募队列 ${o.recruitment_queue_count ?? 0}`,
				]
				const stationed = Array.isArray(d?.stationed_armies) ? d.stationed_armies : []
				for (const a of stationed) {
					lines.push(
						`  [${a.army_id}] ${a.name} ${a.type_key}｜${a.is_defense ? '守备' : '突击'}｜${n1(a.health)}/${n1(a.max_health)}｜战力 ${n1(a.power)}${a.embarked ? '｜在运输舰上' : ''}`,
					)
				}
				if (!stationed.length) lines.push('  地面没有部队')
				const rec = Array.isArray(d?.recruitable_armies) ? d.recruitable_armies : []
				if (rec.length) {
					lines.push(`  可招募 ${rec.length}：`)
					for (const a of rec) lines.push(`    ${a.key} ${a.name}（${a.build_time_days ?? '?'} 天）`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_disband_army',
		description:
			'Disband ONE stationed army (bridge disband_planet_army { planet_id, army_id }). army_id comes from stellaris_planet_armies. This destroys the army — there is no way to get it back.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				army_id: { type: 'number', description: 'army_id from stellaris_planet_armies' },
			},
			required: ['planet_id', 'army_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' || typeof args?.army_id !== 'number') return text('需要 planet_id 和 army_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'disband_planet_army', { planet_id: args.planet_id, army_id: args.army_id })
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`陆军 ${args.army_id} 已解散\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_army_deploy',
		description:
			'Read or flip one colony\'s "deploy in orbit" switch (bridge set_planet_army_settings { planet_id, deploy_in_orbit }). While it is on, newly recruited armies drop straight into a transport fleet instead of standing on the ground. Omit deploy_in_orbit to only read the current value (comes from get_planet_armies.overview).',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony' },
				deploy_in_orbit: { type: 'boolean', description: 'true = new armies board transports; omit to read only' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				if (typeof args.deploy_in_orbit !== 'boolean') {
					const d = await rpc(pipe, 'get_planet_armies', { planet_id: args.planet_id })
					if (d?.success === false || d?.error) return text(bridgeFail(d))
					const o = d?.overview ?? {}
					return text(`行星 ${args.planet_id}：部署到轨道 ${o.deploy_in_orbit ? '开' : '关'}（想改就带 deploy_in_orbit）`)
				}
				const d = await rpc(pipe, 'set_planet_army_settings', { planet_id: args.planet_id, deploy_in_orbit: args.deploy_in_orbit })
				if (d?.success !== true) return text(bridgeFail(d))
				const o = d?.overview ?? {}
				return text(`部署到轨道 → ${args.deploy_in_orbit ? '开' : '关'}（行星 ${args.planet_id}，现读 ${o.deploy_in_orbit ? '开' : '关'}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_embark_armies',
		description:
			'Embark every assault army on a colony into a NEW transport fleet (bridge embark_all_armies { planet_id }). Defense armies refuse to embark. The reply names the transport fleet id, which stellaris_land_armies then takes. With deploy_in_orbit on, armies recruited later go to orbit by themselves — this tool is for ones already standing on the ground.',
		parameters: {
			type: 'object',
			properties: { planet_id: { type: 'number', description: 'planet id of the colony' } },
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'embark_all_armies', { planet_id: args.planet_id })
				if (d?.success !== true) return text(bridgeFail(d))
				const fleet = d?.transport_fleet_id ?? d?.fleet_id
				return text(`上船完成${fleet ? `：运输舰队 ${fleet}` : ''}${typeof d?.embarked_count === 'number' ? `（${d.embarked_count} 支）` : ''}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_land_armies',
		description:
			'Land a transport fleet\'s armies on a planet (bridge land_armies { fleet_id, planet_id, queue }). fleet_id is the TRANSPORT fleet (stellaris_planet_armies after embarking, or get_civilian_fleets.transport_fleets). When the landing succeeds the transport fleet disappears and the armies show up in stationed_armies.',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'transport fleet id' },
				planet_id: { type: 'number', description: 'planet id to land on' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
			},
			required: ['fleet_id', 'planet_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.fleet_id !== 'number' || typeof args?.planet_id !== 'number') return text('需要 fleet_id 和 planet_id')
			const params = { fleet_id: args.fleet_id, planet_id: args.planet_id }
			if (args.queue === true) params.queue = true
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'land_armies', params)
				if (d?.success === false || d?.error) return text(bridgeFail(d))
				return text(`登陆指令已投（舰队 ${args.fleet_id} → 行星 ${args.planet_id}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 殖民新行星：colonize（fleet_id 可省，工具自己找空闲殖民船） ---- */
	add({
		name: 'stellaris_colonize',
		description:
			'Post a colonization order (bridge colonize { fleet_id, planet_id, queue }). fleet_id may be omitted: the tool then picks an idle colony ship from get_civilian_fleets.colony_ships and reports which one it used. Refusals come from CColonizePlanetOrder::CanQueue with the engine\'s own text (e.g. 行星有异常现象 / 行星不可殖民). With the instant_colony console switch the colony forms in about 20 days, otherwise roughly a year.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id to colonise' },
				fleet_id: { type: 'number', description: 'colony ship fleet id; omit to pick an idle one' },
				queue: { type: 'boolean', description: 'append to the fleet queue instead of replacing orders' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				let fleetId = args.fleet_id
				let picked = ''
				if (typeof fleetId !== 'number') {
					const civ = await rpc(pipe, 'get_civilian_fleets', {})
					const ships = Array.isArray(civ?.colony_ships) ? civ.colony_ships : []
					if (!ships.length) return text('没有殖民船（get_civilian_fleets.colony_ships 为空）——可直接传 fleet_id')
					const idle = ships.find((s) => s.status === '无指令') ?? ships[0]
					fleetId = idle.fleet_id
					picked = `（自动选船：${idle.name} ${idle.fleet_id}，${idle.status}）`
				}
				const params = { fleet_id: fleetId, planet_id: args.planet_id }
				if (args.queue === true) params.queue = true
				const d = await rpc(pipe, 'colonize', params)
				if (d?.success !== true) return text(`${bridgeFail(d)}${picked}`)
				return text(`殖民指令已投：${d.planet ?? args.planet_id}（船 ${fleetId}）${picked}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 行星飞升：ascend_colony ---- */
	add({
		name: 'stellaris_ascend_colony',
		description:
			'Raise one colony\'s ascension tier by one (bridge ascend_colony { planet_id }). Costs unity; the gate is FREE ASCENSION PERK SLOTS (get_traditions summary.ascension_perk_slots_available — one slot per completed tradition tree), not perks already picked. Each tier is +25% to the designation\'s effects. The reply carries previous_tier / target_tier.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			try {
				const d = await rpc(
					config.pipe || DEFAULTS.pipe,
					'ascend_colony',
					typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id },
				)
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`飞升 ${d.previous_tier ?? '?'} → ${d.target_tier ?? '?'} 级（${typeof args?.colony_id === 'number' ? '殖民地 ' + args.colony_id : '行星 ' + args.planet_id}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 地貌改造：读选项 / 开 / 取消 ----
	   注意两个科技：基础改造链接要 tech_terrestrial_sculpting，改自家殖民地还要 tech_ecological_adaptation；
	   剧毒星球要 tech_climate_restoration + 排毒飞升。改造是项目不是队列项，instant_build 不管它。 */
	add({
		name: 'stellaris_terraforming',
		description:
			'Read terraforming options (bridge get_terraforming_options { planet_id }): the current planet class, the running project (target class, days done / total, percent, remaining) and every link whose can_terraform came back true from CStartTerraformationCommand. The link_index shown here is what start_terraforming wants when one target appears twice. Habitats/machine worlds have 0 options; planets whose tech is missing show can_terraform false.',
		parameters: {
			type: 'object',
			properties: { planet_id: { type: 'number', description: 'planet id' } },
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_terraforming_options', { planet_id: args.planet_id })
				if (d?.error) return text(`bridge: ${d.error}`)
				const lines = [
					`行星 ${d.planet_id}（${d.planet_name}）｜当前 ${d.current_planet_class_name ?? d.current_planet_class ?? '?'}`,
				]
				const p = d.current_process
				if (d.is_terraforming && p) {
					lines.push(
						`  进行中 → ${p.target_planet_class_name ?? p.target_planet_class}｜${p.progress_days}/${p.total_days} 天（剩 ${p.remaining_days}，${p.progress_percent}%）`,
					)
				} else {
					lines.push('  当前没有改造项目')
				}
				const opts = Array.isArray(d.options) ? d.options : []
				lines.push(`  可选 ${d.available_options_count ?? opts.length}：`)
				for (const o of opts) {
					lines.push(
						`    ${o.can_terraform ? '✓' : '✗'} ${o.target_planet_class_name ?? o.target_planet_class}｜${o.target_planet_class}｜${o.duration_days} 天｜link ${o.link_index}`,
					)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_start_terraforming',
		description:
			'Start a terraforming project on one planet (bridge start_terraforming { planet_id, target_class, link_index? }). target_class comes from stellaris_terraforming (e.g. pc_continental); pass link_index as well when the same target appears twice. A world you do not own needs tech_terrestrial_sculpting, a colony of yours also needs tech_ecological_adaptation, toxic worlds need tech_climate_restoration plus a detox perk. Takes 1800–3600 days and is a project, so instant_build does not speed it up.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id' },
				target_class: { type: 'string', description: 'target planet class key, e.g. pc_continental' },
				link_index: { type: 'number', description: 'link_index from stellaris_terraforming' },
			},
			required: ['planet_id', 'target_class'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' || !args?.target_class) return text('需要 planet_id 和 target_class')
			const params = { planet_id: args.planet_id, target_class: args.target_class }
			if (typeof args.link_index === 'number') params.link_index = args.link_index
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'start_terraforming', params)
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`改造已开始：→ ${args.target_class}（行星 ${args.planet_id}，${d.duration_days ?? '?'} 天）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_cancel_terraforming',
		description:
			'Cancel the terraforming project running on one planet (bridge cancel_terraforming { planet_id }). No refund of the days already spent; the planet can be terraformed again afterwards.',
		parameters: {
			type: 'object',
			properties: { planet_id: { type: 'number', description: 'planet id' } },
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'cancel_terraforming', { planet_id: args.planet_id })
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`改造已取消（行星 ${args.planet_id}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	buildAction(
		'stellaris_set_district_zone',
		'set_district_zone',
		'Change what a district slot is specialised into (zone). district_id, slot and zone_key all come from stellaris_available_district_zones; the change is queued as construction (commonly 360 days, instant_build makes it immediate). Buildings already on the slot are filtered by the NEW zone: the ones it accepts stay, the rest are removed without refund. While a zone is being rebuilt do not queue buildings inside it — they can land in the placeholder zone; save and load the game once the zone is done, then build.',
		{
			planet_id: { type: 'number', description: 'planet id of the colony' },
			district_id: { type: 'number', description: 'district_id from stellaris_available_district_zones' },
			slot: { type: 'number', description: 'slot index inside that district (0-based)' },
			zone_key: { type: 'string', description: 'zone key to change to, e.g. zone_foundry' },
		},
		['planet_id', 'district_id', 'slot', 'zone_key'],
	)

	buildAction(
		'stellaris_cancel_construction',
		'cancel_construction',
		'Take one item out of a construction queue (colony or shipyard) — the queue\'s cancel button, and it refunds the full cost immediately. item_id comes from stellaris_planet_structures (construction queue) and must never be built by hand. Verified for districts and buildings; the same command removes starbase modules.',
		{
			item_id: { type: 'number', description: 'construction_queue[].item_id (see stellaris_planet_structures)' },
		},
		['item_id'],
	)

	/* ---- 造船：船坞队列（桥的 get_buildable_ships / build_ship） ---- */
	/* 与「可建模块 → 入队」同一条思路：先问能造什么，再下单。
	   桥的 get_buildable_ships 只列"现在造得起"的——空列表多半是合金不够，不是船坞不能造。 */
	const shipyardArg = {
		starbase_id: { type: 'number', description: 'starbase id from stellaris_starbases; omit it and give system_id to use that system\'s starbase' },
		system_id: { type: 'number', description: 'system id instead of starbase_id (the starbase sitting in that system), e.g. 106' },
	}

	add({
		name: 'stellaris_buildable_ships',
		description:
			'Which ship designs the shipyard at this starbase can build right now, with alloy cost and build days. The list only holds what you can afford at this moment, so an EMPTY list usually means "not enough alloys", not "nothing is possible" — pass design_id to ask about one design and get the engine\'s own refusal reason. Give starbase_id, or system_id to use the starbase in that system. Colony ships come back with colony_ship: true and use the founder species unless species_id says otherwise.',
		parameters: {
			type: 'object',
			properties: {
				...shipyardArg,
				design_id: { type: 'number', description: 'ask about this one design only (its refusal reason comes back in refused[])' },
				species_id: { type: 'number', description: 'species for a colony ship; defaults to the founder species' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const params = {}
			for (const key of ['starbase_id', 'system_id', 'design_id', 'species_id']) {
				if (typeof args?.[key] === 'number') params[key] = args[key]
			}
			if (params.starbase_id === undefined && params.system_id === undefined) {
				return text('需要 starbase_id 或 system_id')
			}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_buildable_ships', params)
				if (d?.success !== true) return text(bridgeFail(d))
				const rows = Array.isArray(d.buildable) ? d.buildable : []
				const refused = Array.isArray(d.refused) ? d.refused : []
				const lines = [`船坞 starbase ${d.starbase_id}（队列 ${d.queue_id}）`]
				if (params.design_id !== undefined) {
					// 点名单一设计：能造就给价格，造不了给原因——两种都要看
					for (const r of rows) lines.push(`  可建 ${shipRow(r)}  ${priceText(r)}`)
					for (const r of refused) {
						lines.push(`  ✗ ${shipRow(r)}`)
						lines.push(...reasonBlock(r.reason, '      '))
					}
					return text(lines.join('\n'))
				}
				if (!rows.length) {
					const why = d.refused_count ? `${d.refused_count} 种被拒` : '没有可造的设计'
					lines.push(`  现在一种都造不起：${why}——多半是合金不够；给 design_id 能拿到引擎给的原因`)
					return text(lines.join('\n'))
				}
				lines.push(`  可建 ${rows.length} 种：`)
				for (const r of rows) lines.push(`      ${shipRow(r)}  ${priceText(r)}${r.colony_ship ? '（殖民船）' : ''}`)
				if (d.refused_count) lines.push(`  另有 ${d.refused_count} 种被拒（给 design_id 看原因）`)
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_build_ship',
		description:
			'Queue ships at a starbase shipyard through the normal build queue — real alloys and real build days, the same queue you get by clicking. design_id comes from stellaris_buildable_ships; count is 1–10 and the ships are queued one at a time. NEW SHIPS FORM THEIR OWN FLEET, they do not join an existing one (only reinforcement does that). Give starbase_id, or system_id to use the starbase in that system. A refused design comes back success:false with the engine reason; when only part of the count could be queued, success is true and `error` says why it stopped.',
		parameters: {
			type: 'object',
			properties: {
				design_id: { type: 'number', description: 'design id from stellaris_buildable_ships, e.g. 1552' },
				count: { type: 'number', description: 'how many ships, 1–10 (default 1)' },
				...shipyardArg,
				species_id: { type: 'number', description: 'species for a colony ship; defaults to the founder species' },
			},
			required: ['design_id'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.design_id !== 'number') return text('需要 design_id（从 stellaris_buildable_ships 拿）')
			if (typeof args?.starbase_id !== 'number' && typeof args?.system_id !== 'number') {
				return text('需要 starbase_id 或 system_id')
			}
			const params = { design_id: args.design_id }
			for (const key of ['count', 'starbase_id', 'system_id', 'species_id']) {
				if (typeof args?.[key] === 'number') params[key] = args[key]
			}
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'build_ship', params)
				if (d?.success !== true) {
					// 桥这条报错原文是 "see stellaris_get_buildable_ships"——本插件没有这个名字，
					// 照原样转出去模型会去调一个不存在的工具，所以在这里改口
					const why = String(d?.error ?? JSON.stringify(d ?? {})).replace(
						/stellaris_get_buildable_ships/g,
						'stellaris_buildable_ships',
					)
					return text(`失败：${why.trim()}`)
				}
				const bits = [`${shipRow(d)} 入队 ${d.queued} 艘`]
				if (d.cost) bits.push(costText(d.cost))
				if (d.build_days) bits.push(`${d.build_days} 天`)
				bits.push(`starbase ${d.starbase_id}（队列 ${d.queue_id}）`)
				if (d.colony_ship) bits.push(`殖民船 · species ${d.species_id}`)
				const tail = d.error ? `\n  只排进 ${d.queued} 艘：${d.error}` : ''
				return text(`${bits.join(' · ')}${tail}\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 某星系在不在风暴里（内存直读；只取 cluster 的星系表与名字）----
	   cluster 段：+0x180 星系数组 / +0x18C 条数；名字在对象 +0x1D8 的 CPersistentName.key（+0x18）。 */
	const stormAtSystem = async (pipe, systemId) => {
		if (typeof systemId !== 'number') return null
		const readMem = async (address, length) => {
			if (!address || !length) return null
			const r = await rpc(pipe, 'read_memory', { address, length })
			if (!r?.success || typeof r.hex !== 'string') return null
			return Buffer.from(r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16)))
		}
		const readPdx = async (addr) => {
			const b = await readMem(addr, 0x40)
			if (!b || b.length < 0x20) return ''
			const size = Number(b.readBigUInt64LE(0x10))
			if (!size || size > 256) return ''
			const cap = Number(b.readBigUInt64LE(0x18))
			if (cap < 16) return b.subarray(0, Math.min(size, 63)).toString('utf8')
			const ptr = Number(b.readBigUInt64LE(0))
			if (!ptr) return ''
			const d = await readMem(ptr, Math.min(size, 255) + 1)
			return d ? d.toString('utf8').replace(/\0.*$/, '') : ''
		}
		const base = Number((await rpc(pipe, 'get_module_base'))?.base)
		if (!base) return null
		const holder = await readMem(base + 0x311BE90, 8)
		const mgr = holder ? Number(holder.readBigUInt64LE(0)) : 0
		if (!mgr) return null
		const head = await readMem(mgr, 0x60)
		if (!head) return null
		const arr = Number(head.readBigUInt64LE(0x18))
		const cap = head.readUInt32LE(0x20)
		const hits = []
		for (let off = 0; off < cap; off += 512) {
			const n = Math.min(cap - off, 512)
			const chunk = await readMem(arr + off * 16, n * 16)
			if (!chunk) break
			for (let k = 0; k * 16 + 8 <= chunk.length; k += 1) {
				const st = Number(chunk.readBigUInt64LE(k * 16 + 8))
				if (!st) continue
				const b = await readMem(st, 0x400)
				if (!b || b.length < 0x1C0) continue
				const objsPtr = Number(b.readBigUInt64LE(0x180))
				const objsCount = b.readUInt32LE(0x18C)
				if (!objsPtr || objsCount <= 0 || objsCount >= 512) continue
				const ob = await readMem(objsPtr, objsCount * 4)
				if (!ob) continue
				let inside = false
				for (let j = 0; j < objsCount; j += 1) {
					if (ob.readUInt32LE(j * 4) === systemId) {
						inside = true
						break
					}
				}
				if (!inside) continue
				const nameKey = await readPdx(st + 0x1D8 + 0x18)
				const typePtr = Number(b.readBigUInt64LE(0x340))
				const typeKey = typePtr ? await readPdx(typePtr + 0x20) : ''
				hits.push({ name_key: nameKey, type: typeKey, radius: Number(b.readBigInt64LE(0x1B8)) / 1e5 })
			}
		}
		if (!hits.length) return { inside: false, storms: [] }
		const keys = [...new Set(hits.map((h) => h.name_key).filter(Boolean))]
		const names = keys.length ? ((await rpc(pipe, 'localize', { keys }).catch(() => null))?.names ?? {}) : {}
		for (const h of hits) h.name = names[h.name_key] && names[h.name_key] !== h.name_key ? names[h.name_key] : h.name_key
		return { inside: true, storms: hits }
	}

	/* ---- 殖民地产出 / 维护 / 净（内存直读；方舟与行星同一条 CColony）----
	   CColony +0xE80 produced / +0xEA0 upkeep / +0xEC0 net：每张表是指针（表对象：数据 +0x00、条数 +0x0C），
	   条目 i64 定点 ÷1e5。资源名按 CStrategicResourceDatabase（base+0x3159BF0）的顺序取，键在资源对象 +0x30。 */
	let resourceNameCache = null
	const readColonyProduction = async (pipe, colonyPtr) => {
		if (!colonyPtr) return null
		const readMem = async (address, length) => {
			if (!address || !length) return null
			/* 管道偶发单次失败（短时间内一连串连接时见过），重试两次 */
			for (let attempt = 0; attempt < 3; attempt += 1) {
				if (attempt) await new Promise((r) => setTimeout(r, 40))
				const r = await rpc(pipe, 'read_memory', { address, length }).catch(() => null)
				if (r?.success && typeof r.hex === 'string') {
					return Buffer.from(r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16)))
				}
			}
			return null
		}
		const readPdxAt = async (addr) => {
			const b = await readMem(addr, 0x40)
			if (!b || b.length < 0x20) return ''
			const size = Number(b.readBigUInt64LE(0x10))
			if (!size || size > 128) return ''
			const cap = Number(b.readBigUInt64LE(0x18))
			if (cap < 16) return b.subarray(0, size).toString('utf8')
			const p = Number(b.readBigUInt64LE(0))
			const d = p ? await readMem(p, size + 1) : null
			return d ? d.toString('utf8') : ''
		}
		try {
			if (!resourceNameCache) {
				const base = Number((await rpc(pipe, 'get_module_base'))?.base)
				if (!base) return null
				const slot = await readMem(base + 0x3159BF0, 8)
				const db = slot ? Number(slot.readBigUInt64LE(0)) : 0
				const head = db ? await readMem(db, 0x20) : null
				const count = head ? head.readUInt32LE(0x14) : 0
				const arr = head ? Number(head.readBigUInt64LE(0x08)) : 0
				if (!arr || !count || count > 256) return null
				const ptrs = await readMem(arr, count * 8)
				const names = []
				for (let i = 0; i < count; i += 1) {
					const rp = ptrs ? Number(ptrs.readBigUInt64LE(i * 8)) : 0
					names.push(rp ? (await readPdxAt(rp + 0x30)).trim() : '')
				}
				resourceNameCache = names
			}
			const table = async (off) => {
				const t = await readMem(colonyPtr + off, 8)
				const tp = t ? Number(t.readBigUInt64LE(0)) : 0
				if (!tp) return {}
				const th = await readMem(tp, 0x10)
				if (!th) return {}
				const data = Number(th.readBigUInt64LE(0))
				const size = th.readUInt32LE(0x0C)
				if (!data || !size || size > 256) return {}
				const v = await readMem(data, size * 8)
				if (!v) return {}
				const out = {}
				for (let i = 0; i < Math.min(size, resourceNameCache.length); i += 1) {
					const raw = Number(v.readBigInt64LE(i * 8))
					if (raw && resourceNameCache[i]) out[resourceNameCache[i]] = raw / 1e5
				}
				return out
			}
			return { produced: await table(0xe80), upkeep: await table(0xea0), net: await table(0xec0) }
		} catch {
			/* 单次读失败会整段返回 null（读风暴表时也见过偶发失败）——调用方重试一次 */
			return null
		}
	}
	/* 一块殖民地的状态数值（内存直读；行星与方舟同一套 CColony 字段，值都是 ×1e5 定点） */
	const readColonyStatus = async (pipe, colonyPtr) => {
		if (!colonyPtr) return null
		let r = null
		for (let attempt = 0; attempt < 3; attempt += 1) {
			if (attempt) await new Promise((res) => setTimeout(res, 40))
			r = await rpc(pipe, 'read_memory', { address: Number(colonyPtr), length: 0x1000 }).catch(() => null)
			if (r?.success && typeof r.hex === 'string') break
		}
		if (!r?.success || typeof r.hex !== 'string') return null
		const b = Buffer.from(r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16)))
		if (b.length < 0xFC8) return null
		const at = (off) => Number(b.readUInt32LE(off)) / 1e5
		return {
			stability: at(0xfb8),
			crime: at(0xfc0),
			amenities: at(0xfa8),
			amenities_usage: at(0xfb0),
			housing: at(0xf88),
			housing_usage: at(0xf90),
			housing_free: at(0xf98),
		}
	}

	/* 一块殖民地的岗位（内存直读；方舟与行星同一条 CColony）
	   CColony +0x38 数组 / +0x44 条数（每条 4 字节 handle），handle → CPopJob 库
	   （base+0x311BDA8 → +0x18 数组 / +0x20 容量，槽 16、指针 +8）。
	   CPopJob：+0xF8 在岗 · +0x100 编制 · +0x108 加成 · +0x110 上限定（都是 i64 ÷1e5）；
	   +0x18 是岗位类型对象 → +0x20 键名、+0xaa0 → 阶层对象 +0x20 键名。 */
	const readColonyJobs = async (pipe, colonyPtr) => {
		if (!colonyPtr) return null
		const readMem = async (address, length) => {
			for (let attempt = 0; attempt < 3; attempt += 1) {
				if (attempt) await new Promise((r) => setTimeout(r, 40))
				const r = await rpc(pipe, 'read_memory', { address, length }).catch(() => null)
				if (r?.success && typeof r.hex === 'string') {
					return Buffer.from(r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16)))
				}
			}
			return null
		}
		const readPdxKey = async (addr) => {
			const b = await readMem(addr, 0x40)
			if (!b || b.length < 0x20) return ''
			const size = Number(b.readBigUInt64LE(0x10))
			if (!size || size > 128) return ''
			const cap = Number(b.readBigUInt64LE(0x18))
			if (cap < 16) return b.subarray(0, Math.min(size, 64)).toString('utf8')
			const p = Number(b.readBigUInt64LE(0))
			const d = p ? await readMem(p, Math.min(size, 127) + 1) : null
			return d ? d.toString('utf8').replace(/\0.*$/, '') : ''
		}
		const head = await readMem(Number(colonyPtr) + 0x30, 0x20)
		if (!head) return null
		const arr = Number(head.readBigUInt64LE(0x08))
		const cnt = head.readUInt32LE(0x14)
		if (!arr || !cnt || cnt > 256) return null
		const handles = await readMem(arr, cnt * 4)
		if (!handles) return null
		const base = Number((await rpc(pipe, 'get_module_base'))?.base)
		if (!base) return null
		const dbSlot = await readMem(base + 0x311bda8, 8)
		const db = dbSlot ? Number(dbSlot.readBigUInt64LE(0)) : 0
		const dbHead = db ? await readMem(db, 0x24) : null
		const jArr = dbHead ? Number(dbHead.readBigUInt64LE(0x18)) : 0
		const jCap = dbHead ? dbHead.readUInt32LE(0x20) : 0
		if (!jArr || !jCap) return null
		const rows = []
		for (let i = 0; i < cnt; i += 1) {
			const handle = handles.readUInt32LE(i * 4)
			const slot = handle & 0xffffff
			if (slot >= jCap) continue
			const e = await readMem(jArr + slot * 16 + 8, 8)
			const job = e ? Number(e.readBigUInt64LE(0)) : 0
			if (!job) continue
			const b = await readMem(job + 0xf0, 0x30)
			if (!b) continue
			const at = (off) => Number(b.readBigInt64LE(off)) / 1e5
			const cur = at(0x08)
			const max = at(0x10)
			const bonus = at(0x18)
			const limit = at(0x20)
			if (cur <= 0 && max <= 0) continue
			const t = await readMem(job + 0x18, 8)
			const tp = t ? Number(t.readBigUInt64LE(0)) : 0
			const key = tp ? (await readPdxKey(tp + 0x20)).trim() : ''
			if (!key) continue
			let stratum = 'worker'
			if (tp) {
				const c = await readMem(tp + 0xaa0, 8)
				const cp = c ? Number(c.readBigUInt64LE(0)) : 0
				if (cp) {
					const sk = (await readPdxKey(cp + 0x20)).trim()
					if (sk) stratum = sk
				}
			}
			rows.push({ key, name: '', stratum, cur, max, bonus, limit })
		}
		const keys = [...new Set(rows.map((r) => 'job_' + r.key))]
		const names = keys.length ? ((await rpc(pipe, 'localize', { keys }).catch(() => null))?.names ?? {}) : {}
		for (const r of rows) {
			const hit = names['job_' + r.key]
			r.name = hit && hit !== 'job_' + r.key ? hit : r.key
		}
		return rows
	}

	/* 一块殖民地的逐物种人口（内存直读；行星与方舟同一套）
	   CColony +0xF68 数组指针 / +0xF70 条数，条目步长 12：+0x00 人数、+0x08 物种 id
	   物种名：base+0x311BD58 取库 → +0x18 数组 / +0x20 容量，槽 16 字节、指针在 +8，
	   对象 +0x10 是 id、+0x60 是 CPersistentName.key（CPdxString），再走 localize。 */
	const readColonySpecies = async (pipe, colonyPtr) => {
		if (!colonyPtr) return null
		const readMem = async (address, length) => {
			for (let attempt = 0; attempt < 3; attempt += 1) {
				if (attempt) await new Promise((r) => setTimeout(r, 40))
				const r = await rpc(pipe, 'read_memory', { address, length }).catch(() => null)
				if (r?.success && typeof r.hex === 'string') {
					return Buffer.from(r.hex.trim().split(/\s+/).map((h) => parseInt(h, 16)))
				}
			}
			return null
		}
		const readPdxKey = async (addr) => {
			const b = await readMem(addr, 0x40)
			if (!b || b.length < 0x20) return ''
			const size = Number(b.readBigUInt64LE(0x10))
			if (!size || size > 256) return ''
			const cap = Number(b.readBigUInt64LE(0x18))
			if (cap < 16) return b.subarray(0, Math.min(size, 64)).toString('utf8')
			const p = Number(b.readBigUInt64LE(0))
			const d = p ? await readMem(p, Math.min(size, 255) + 1) : null
			return d ? d.toString('utf8').replace(/\0.*$/, '') : ''
		}
		const head = await readMem(Number(colonyPtr) + 0xf60, 0x20)
		if (!head) return null
		const arr = Number(head.readBigUInt64LE(0x08))
		const cnt = head.readUInt32LE(0x10)
		if (!arr || !cnt || cnt > 64) return null
		const rows = await readMem(arr, cnt * 12 + 4)
		if (!rows) return null
		const base = Number((await rpc(pipe, 'get_module_base'))?.base)
		if (!base) return null
		const dbSlot = await readMem(base + 0x311bd58, 8)
		const smgr = dbSlot ? Number(dbSlot.readBigUInt64LE(0)) : 0
		const dbHead = smgr ? await readMem(smgr, 0x24) : null
		const spArr = dbHead ? Number(dbHead.readBigUInt64LE(0x18)) : 0
		const spCap = dbHead ? dbHead.readUInt32LE(0x20) : 0
		const out = []
		for (let i = 0; i < cnt; i += 1) {
			const count = rows.readUInt32LE(i * 12)
			const sid = rows.readUInt32LE(i * 12 + 8)
			if (!sid || !count) continue
			let key = ''
			const slot = sid & 0xffffff
			if (spArr && slot < spCap) {
				const e = await readMem(spArr + slot * 16 + 8, 8)
				const sp = e ? Number(e.readBigUInt64LE(0)) : 0
				if (sp) {
					const idb = await readMem(sp + 0x10, 4)
					if (idb && (idb.readUInt32LE(0) & 0xffffff) === slot) key = await readPdxKey(sp + 0x60)
				}
			}
			out.push({ species_id: sid, count, key })
		}
		if (!out.length) return out
		const keys = [...new Set(out.map((r) => r.key).filter(Boolean))]
		const names = keys.length ? ((await rpc(pipe, 'localize', { keys }).catch(() => null))?.names ?? {}) : {}
		for (const r of out) r.name = r.key && names[r.key] && names[r.key] !== r.key ? names[r.key] : r.key || `物种 ${r.species_id}`
		out.sort((a, b) => b.count - a.count)
		/* 同名（不同亚种/立绘）时带上是哪个物种 id，否则三个「大魔女」分不清 */
		const nameCount = new Map()
		for (const r of out) nameCount.set(r.name, (nameCount.get(r.name) ?? 0) + 1)
		for (const r of out) if (nameCount.get(r.name) > 1) r.name = `${r.name}#${r.species_id}`
		return out
	}

	/* probe_colony 偶发单次失败，重试两次 */
	const probeColony = async (pipe, colonyId) => {
		for (let attempt = 0; attempt < 3; attempt += 1) {
			if (attempt) await new Promise((r) => setTimeout(r, 40))
			const r = await rpc(pipe, 'probe_colony', { colony_id: colonyId }).catch(() => null)
			if (r?.success && r?.ptr) return r
		}
		return null
	}

	/* 把一张资源表印成一行（只留非零，按绝对值降序；upkeep 不写正号） */
	const formatResourceTable = (table, names, signed = true) => {
		const rows = Object.entries(table ?? {})
			.filter(([, v]) => Math.abs(v) >= 0.005)
			.sort((a, b) => Math.abs(b[1]) - Math.abs(a[1]))
		if (!rows.length) return '（无）'
		return rows.map(([k, v]) => `${names[k] ?? k} ${v > 0 && signed ? '+' : ''}${v.toFixed(2)}`).join(' / ')
	}

	/* ---- 行星结构：区划 → 槽位 → 建筑（带 id）与建造队列（带 item_id） ---- */
	/* 升级要 building_id、撤队列要 item_id，而存档报告（手动跑 colony_report.py）两个 id 都没有；
	   这两个 id 只在 get_planet_details 里。所以单独开一个工具，只渲染这一层。 */
	add({
		name: 'stellaris_planet_structures',
		description:
			'Live layout of one colony: per-species population (CColony +0xF68), stability / crime (deviance for gestalt) / amenities / housing (CColony +0xFB8 / +0xFC0 / +0xFA8 / +0xF88), monthly produced / upkeep / net, storm membership — plus every district, its zone slots, the buildings inside each slot — with the building_id that stellaris_upgrade_building wants — plus the construction queue with the item_id that stellaris_cancel_construction wants. Also the zone_id to pass to stellaris_build_building, and the colony\'s blockers (with can_clear, clear cost/time, status and deposit_id for stellaris_clear_blocker) and planetary features. Use it whenever you need ids; stellaris_colonies and stellaris_colony_report are cheaper for summaries (and the save-side report has no ids at all).',
		parameters: {
			type: 'object',
			properties: { planet_id: { type: 'number', description: 'planet id of the colony' } },
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			// 方舟没有行星：给 colony_id 就直接走 colony 这条路（与行星共用同一棵 CColony 树）
			if (typeof args?.colony_id === 'number') {
				try {
					const s = await rpc(config.pipe || DEFAULTS.pipe, 'probe_colony_structures', { colony_id: args.colony_id })
					if (s?.success !== true) return text(bridgeFail(s))
					const stab = Number(s.stability_fixed) >= 0 ? (Number(s.stability_fixed) / 1e5).toFixed(1) : '?'
					const lines = [
						`殖民地 ${s.colony_id}（${s.carrier_type === 1 ? '方舟 ship ' + s.carrier_id : '行星 ' + s.carrier_id}）`,
						`  人口 ${s.num_sapient_pops ?? '?'}｜飞升 ${s.ascension_tier ?? 0}｜稳定 ${stab}%｜区划 ${s.district_array ?? '?'}｜岗位 ${s.pop_jobs ?? '?'}｜人口组 ${s.pop_groups ?? '?'}｜队列 ${s.queue_items ?? 0} 项（queue_id ${s.queue_id ?? '?'}）`,
					]
					for (const ds of s.districts ?? []) {
						lines.push(`  区划 ${ds.name} ${ds.type}  level ${ds.level}`)
						for (const z of ds.slots ?? []) {
							const bs = z.buildings ?? []
							lines.push(`     槽 ${z.slot}  ${z.zone_name ?? '（空槽）'} ${z.zone ?? ''}（zone_id ${z.zone_id}）  建筑 ${bs.length}`)
							for (const b of bs) lines.push(`         建筑 id ${b.building_id}  ${b.name ?? '?'} ${b.key ?? ''}`)
						}
					}
					const q = Array.isArray(s.queue) ? s.queue : []
					if (q.length) {
						lines.push(`  建造队列 ${q.length} 项（queue_id ${s.queue_id}）：`)
						for (const it of q) {
							const days = it.total_days ? `${it.remaining_days}/${it.total_days} 天` : ''
							lines.push(`     item_id ${it.item_id}  ${it.item_name ?? '?'} ${it.key ?? ''}  ${it.type ?? ''}  ${days}`.trimEnd())
						}
					}
					const mods = Array.isArray(s.modifiers) ? s.modifiers : []
					if (mods.length) {
						lines.push(`  修正 ${mods.length} 条：`)
						for (const m of mods) {
							const d = Number(m.days)
							const dur = d < 0 ? '永久' : `${d} 天`
							lines.push(`     ${m.name ?? m.key} ${m.key}  ${dur}${m.clear_on_owner_change ? '  换主时清除' : ''}`)
						}
					}
					const st = await stormAtSystem(config.pipe || DEFAULTS.pipe, s.system_id)
					if (st) {
						lines.push(
							`  风暴：${st.inside ? `在 ${st.storms.map((x) => `${x.name}（${x.type}，半径 ${x.radius.toFixed(0)}）`).join('、')} 内` : '不在任何风暴内'}` +
								(s.system_name ? `　（所在星系 ${s.system_name} ${s.system_id}）` : ''),
						)
					}
					const pc = await probeColony(config.pipe || DEFAULTS.pipe, s.colony_id)
					if (pc?.ptr) {
						const st = await readColonyStatus(config.pipe || DEFAULTS.pipe, Number(pc.ptr))
						if (st) {
							lines.push(
								`  状态：稳定 ${st.stability.toFixed(1)}%｜犯罪/反常 ${st.crime.toFixed(1)}%` +
									`｜舒适 ${st.amenities.toFixed(1)}（用 ${st.amenities_usage.toFixed(1)}，余 ${(st.amenities - st.amenities_usage).toFixed(1)}）` +
									`｜住房 ${st.housing.toFixed(1)}（用 ${st.housing_usage.toFixed(1)}，余 ${st.housing_free.toFixed(1)}）`,
							)
						}
					}
					const spRows = pc?.ptr ? await readColonySpecies(config.pipe || DEFAULTS.pipe, Number(pc.ptr)) : null
					if (spRows?.length) {
						const total = spRows.reduce((n, r) => n + r.count, 0)
						const fmtPop = (n) => (n >= 1000 ? `${(n / 1000).toFixed(1)}K` : String(n))
						const shown = spRows
							.slice(0, 8)
							.map((r) => `${r.name} ${fmtPop(r.count)}（${total ? ((r.count / total) * 100).toFixed(1) : '?'}%）`)
						lines.push(`  物种人口 ${spRows.length} 种：${shown.join(' / ')}${spRows.length > 8 ? ` / 另 ${spRows.length - 8} 种` : ''}`)
					}
					let prod = pc?.ptr ? await readColonyProduction(config.pipe || DEFAULTS.pipe, Number(pc.ptr)) : null
					if (!prod && pc?.ptr) prod = await readColonyProduction(config.pipe || DEFAULTS.pipe, Number(pc.ptr))
					if (prod) {
						const pkeys = [...new Set(Object.keys({ ...prod.produced, ...prod.upkeep, ...prod.net }))]
						const pnames = pkeys.length
							? ((await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: pkeys }).catch(() => null))?.names ?? {})
							: {}
						lines.push(`  产出：${formatResourceTable(prod.produced, pnames)}`)
						lines.push(`  维护：${formatResourceTable(prod.upkeep, pnames, false)}`)
						lines.push(`  净额：${formatResourceTable(prod.net, pnames)}`)
					}
					const feats = Array.isArray(s.features) ? s.features : []
					if (feats.length) {
						const byName = new Map()
						for (const f of feats) byName.set(f.name ?? f.key ?? '?', (byName.get(f.name ?? f.key ?? '?') ?? 0) + 1)
						lines.push(`  特征 ${feats.length} 个（持有者类型 ${s.feature_holder_type ?? '?'}${s.feature_holder_type === 2 ? ' = 舰/方舟' : ''}）：`)
						for (const [name, n] of byName) {
							const key = (feats.find((f) => (f.name ?? f.key) === name) ?? {}).key ?? ''
							lines.push(`     ${name} ${key}` + (n > 1 ? ` ×${n}` : ''))
						}
					}
					return text(lines.join('\n'))
				} catch (error) {
					return text('殖民地结构：' + error.message)
				}
			}
			if (typeof args?.planet_id !== 'number') return text('需要 planet_id')
			try {
				const detail = await rpc(config.pipe || DEFAULTS.pipe, 'get_planet_details', { planet_id: args.planet_id })
				if (!detail || detail.error) return text(bridgeFail(detail))
				const kpi = detail.kpi ?? {}
				const lines = [
					`行星 ${detail.planet_id}（${detail.name ?? '?'}）· ${detail.designation ?? '?'}（${detail.designation_key ?? '?'}）`,
					`  人口 ${kpi.pop_display ?? '?'}｜稳定 ${kpi.stability_percent ?? '?'}%｜犯罪/反常 ${kpi.crime_percent ?? '?'}%｜舒适 ${kpi.amenities_display ?? kpi.amenities ?? '?'}｜住房余 ${kpi.housing ?? '?'}｜失业 ${kpi.unemployed ?? '?'}`,
				]
				for (const d of detail.districts ?? []) {
					lines.push(`  区划 ${d.name} ${d.type}  已建 ${d.built}（${d.zone_slots} 个槽位）`)
					for (const z of d.zones ?? []) {
						const used = z.max_buildings ? `${(z.buildings ?? []).length}/${z.max_buildings}` : `${(z.buildings ?? []).length}`
						lines.push(`      ${z.name} ${z.key}（zone_id ${z.slot_index}）  建筑 ${used}`)
						for (const b of z.buildings ?? []) {
							lines.push(`          槽 ${b.slot_index ?? '?'}  建筑 id ${b.id}  ${b.name} ${b.key}（${b.status}）${b.is_capital ? ' 首都' : ''}`)
						}
					}
				}
				const queue = detail.construction_queue ?? []
				const blockers = Array.isArray(detail.blockers) ? detail.blockers : []
				const features = Array.isArray(detail.planetary_features?.features) ? detail.planetary_features.features : []
				if (blockers.length || features.length) {
					lines.push('')
					if (blockers.length) {
						const clearable = blockers.filter((b) => b.can_clear).length
						lines.push(`  障碍 ${blockers.length}（可清 ${clearable}）：`)
						for (const b of blockers) {
							const cost = Object.entries(b.clear_cost ?? {})
								.map(([k, v]) => `${k} ${v}`)
								.join(' + ')
							lines.push(
								`      ${b.can_clear ? '✓' : '✗'} ${b.name} ${b.key}  id ${b.deposit_id}` +
									`${cost ? `  ${cost}` : ''}${b.clear_time ? ` / ${b.clear_time} 天` : ''}` +
									`${b.status_text ? `  ${b.status_text}` : ''}${b.is_queued ? '  （已在清除中）' : ''}`,
							)
						}
					}
					const plain = features.filter((f) => !f.is_blocker)
					if (plain.length) {
						lines.push(
							`  特征 ${plain.length}：` +
								plain.slice(0, 8).map((f) => f.name ?? f.key).join('、') +
								(plain.length > 8 ? ` …（另 ${plain.length - 8} 条）` : ''),
						)
					}
				}
				// 旧路径的 planetary_features 会漏掉「后天增加」的特征（实测行星 0 的星界丝缕 +2 它读成 0）。
				// 这里再用 deposit 表补一段 —— 与方舟那条分支同一套读法，行星按 id 且排除 type 2。
				if (typeof detail.colony_id === 'number') {
					try {
						const s = await rpc(config.pipe || DEFAULTS.pipe, 'probe_colony_structures', { colony_id: detail.colony_id })
						const pmods = Array.isArray(s?.modifiers) ? s.modifiers : []
						if (pmods.length) {
							lines.push(`  修正 ${pmods.length} 条：`)
							for (const m of pmods) {
								const d = Number(m.days)
								lines.push(`     ${m.name ?? m.key} ${m.key}  ${d < 0 ? '永久' : `${d} 天`}`)
							}
						}
						const pst = await stormAtSystem(config.pipe || DEFAULTS.pipe, s?.system_id)
						if (pst) {
							lines.push(
								`  风暴：${pst.inside ? `在 ${pst.storms.map((x) => `${x.name}（${x.type}，半径 ${x.radius.toFixed(0)}）`).join('、')} 内` : '不在任何风暴内'}` +
									(s?.system_name ? `　（所在星系 ${s.system_name} ${s.system_id}）` : ''),
							)
						}
						const ppc = await probeColony(config.pipe || DEFAULTS.pipe, detail.colony_id)
						const spRowsP = ppc?.ptr ? await readColonySpecies(config.pipe || DEFAULTS.pipe, Number(ppc.ptr)) : null
						if (spRowsP?.length) {
							const totalP = spRowsP.reduce((n, r) => n + r.count, 0)
							const fmtPopP = (n) => (n >= 1000 ? `${(n / 1000).toFixed(1)}K` : String(n))
							const shownP = spRowsP
								.slice(0, 8)
								.map((r) => `${r.name} ${fmtPopP(r.count)}（${totalP ? ((r.count / totalP) * 100).toFixed(1) : '?'}%）`)
							lines.push(`  物种人口 ${spRowsP.length} 种：${shownP.join(' / ')}${spRowsP.length > 8 ? ` / 另 ${spRowsP.length - 8} 种` : ''}`)
						}
						let pprod = ppc?.ptr ? await readColonyProduction(config.pipe || DEFAULTS.pipe, Number(ppc.ptr)) : null
						if (!pprod && ppc?.ptr) pprod = await readColonyProduction(config.pipe || DEFAULTS.pipe, Number(ppc.ptr))
						if (pprod) {
							const pkeys = [...new Set(Object.keys({ ...pprod.produced, ...pprod.upkeep, ...pprod.net }))]
							const pnames = pkeys.length
								? ((await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: pkeys }).catch(() => null))?.names ?? {})
								: {}
							lines.push(`  产出：${formatResourceTable(pprod.produced, pnames)}`)
							lines.push(`  维护：${formatResourceTable(pprod.upkeep, pnames, false)}`)
							lines.push(`  净额：${formatResourceTable(pprod.net, pnames)}`)
						}
						const feats = Array.isArray(s?.features) ? s.features : []
						if (feats.length) {
							const byName = new Map()
							for (const f of feats) {
								const n = f.name ?? f.key ?? '?'
								byName.set(n, (byName.get(n) ?? 0) + 1)
							}
							lines.push(`  特征（deposit 表）${feats.length} 个：` + [...byName].map(([n, c]) => (c > 1 ? `${n} ×${c}` : n)).join('、'))
						}
					} catch {
						/* 读不到就跳过，不影响原有渲染 */
					}
				}
				lines.push(queue.length ? '  建造队列：' : '  建造队列：（空）')
				for (const item of queue) {
					const days = item.total_days ? `${item.remaining_days}/${item.total_days} 天` : ''
					lines.push(`      item_id ${item.item_id}  ${item.item_name} ${item.key}  ${item.type ?? ''} ${days}`.trimEnd())
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 行星决议：查与执行（桥的 get_planetary_decisions / enact_decision） ---- */
	/* can_enact 是逐条用 CEnactDecisionCommand::IsValid 探出来的：只问不做，免费 */
	add({
		name: 'stellaris_planetary_decisions',
		description:
			'Planetary decisions a colony can enact right now. `can_enact` is the engine\'s own IsValid verdict for each decision — asking is free and enacts nothing. Only a handful of the ~100 database entries are usually enactable at a time. Pass all: true to list the ones that cannot be enacted as well.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
				all: { type: 'boolean', description: 'also list the decisions that cannot be enacted' },
			},
			required: [],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number')
				return text('需要 planet_id（行星）或 colony_id（方舟）')
			try {
				const d = await rpc(config.pipe || DEFAULTS.pipe, 'get_planetary_decisions', typeof args?.colony_id === 'number' ? { colony_id: args.colony_id } : { planet_id: args.planet_id })
				if (d?.error) return text(bridgeFail(d))
				const list = Array.isArray(d.decisions) ? d.decisions : []
				const can = list.filter((x) => x.can_enact)
				const lines = [`行星 ${d.planet_id}（${d.planet_name ?? '?'}）· ${list.length} 条决议，可执行 ${can.length} 条`]
				for (const x of can) lines.push(`  ✓ ${x.name} ${x.key}${x.days ? `  ${x.days} 天` : ''}`)
				if (!can.length) lines.push('  （现在没有可执行的决议）')
				if (args.all) {
					for (const x of list) if (!x.can_enact) lines.push(`  ✗ ${x.name} ${x.key}`)
				} else if (list.length > can.length) {
					lines.push(`  其余 ${list.length - can.length} 条当前不可执行（条件未满足 / 已生效 / 资源不足）；加 all: true 全列`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_enact_decision',
		description:
			'Enact one planetary decision on a colony. decision_key comes from stellaris_planetary_decisions (the bridge also takes a few short aliases: luxuries, food, population_control, martial_law, mastery_of_nature). **A decision usually cannot be undone** — some run for a fixed number of days and have their own opposite decision (population control does), others are permanent; check can_enact first. A refusal comes back with the engine\'s own reason.',
		parameters: {
			type: 'object',
			properties: {
				planet_id: { type: 'number', description: 'planet id of the colony (planet colonies)' },
				colony_id: { type: 'number', description: 'colony id — use this for a Nomads ark, which has no planet' },
				decision_key: { type: 'string', description: 'decision key from stellaris_planetary_decisions, e.g. decision_enact_population_control_gestalt' },
			},
			required: ['decision_key'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if ((typeof args?.planet_id !== 'number' && typeof args?.colony_id !== 'number') || !args?.decision_key)
				return text('需要（planet_id 或 colony_id）和 decision_key')
			try {
				const d = await rpc(
					config.pipe || DEFAULTS.pipe,
					'enact_decision',
					typeof args?.colony_id === 'number'
						? { colony_id: args.colony_id, decision_key: args.decision_key }
						: { planet_id: args.planet_id, decision_key: args.decision_key },
				)
				if (d?.success !== true) return text(bridgeFail(d))
				return text(`${d.decision_name ?? d.decision_key} 已执行（${d.decision_key}）\n${JSON.stringify(d)}`)
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 本地化：把游戏里的键翻成当前语言 ---- */
	add({
		name: 'stellaris_localize',
		description:
			'Translate Stellaris localisation keys to the current language through the game itself (e.g. job_replicator → 复制者, building_organic_sanctuary → 有机庇护所). Useful for turning the keys that save files and memory reads return into readable names.',
		parameters: {
			type: 'object',
			properties: {
				keys: { type: 'array', items: { type: 'string' }, description: 'localisation keys, e.g. ["job_replicator"]' },
				key: { type: 'string', description: 'a single key' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const keys = []
			if (Array.isArray(args?.keys)) keys.push(...args.keys.filter((k) => typeof k === 'string'))
			if (typeof args?.key === 'string') keys.push(args.key)
			if (!keys.length) return text('没有给键')
			try {
				const answer = await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys })
				const names = answer?.names ?? {}
				return text(keys.map((k) => `${k} → ${names[k] || '（无翻译）'}`).join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 已研究科技：内存直读（桥 get_researched_techs），不存盘、无缓存 ---- */

	/** 取已研究科技（Set）。内存直读：桥的 get_researched_techs，不存盘、不算缓存。 */
	const researchedTechs = async (pipe) => {
		try {
			const r = await rpc(pipe, 'get_researched_techs')
			if (!r || r.success === false) return { error: r?.error || '桥没有返回 get_researched_techs' }
			const rows = r.technologies ?? []
			return {
				techs: new Set(rows.map((t) => t.key)),
				/* 循环科技的 level 就是已研究的次数（实测 39 / 89 / 62 都对），别丢 */
				levels: new Map(rows.map((t) => [t.key, Number(t.level ?? 1)])),
				count: rows.length,
				country_id: r.country_id,
			}
		} catch (error) {
			return { error: error.message }
		}
	}

	add({
		name: 'stellaris_researched_techs',
		description:
			'Researched technologies, read straight from memory (no temp save, nothing written to disk): CTechnologyStatus (player CCountry + 0x1620) holds a vector at +0x18 — data +0x20, size +0x2C — of 0x28-byte entries, each with its level (i32) at +0x08 and its CTechnology* at +0x10; the key is the string at CTechnology+0x20. Instant. Verified key-for-key against the save tech_status block at 2222.01.01 (46/46, same set).',
		parameters: {
			type: 'object',
			properties: {
				country: { type: 'number', description: 'ignored: only the player country can be read (the array hangs off the player CCountry object)' },
				all: { type: 'boolean', description: 'also list every technology key, not just the first 20' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const r = await researchedTechs(pipe)
			if (r.error) return text(`读取失败：${r.error}`)
			const list = [...r.techs].sort()
			const lv = (k) => r.levels?.get(k) ?? 1
			const label = (k) => (lv(k) > 1 ? `${k}  ×${lv(k)}` : k)
			const repeats = list.filter((k) => lv(k) > 1)
			const lines = [`已研究 ${list.length} 项（内存直读，country ${r.country_id ?? 0}）`]
			if (repeats.length) lines.push(`其中循环科技 ${repeats.length} 项（×N = 已研究次数）`)
			if (args?.all === true) lines.push(...list.map(label))
			else lines.push(...list.slice(0, 20).map(label), `…（另有 ${Math.max(0, list.length - 20)} 项，all:true 看全部）`)
			return text(lines.join('\n'))
		},
	})


	/** 同一游戏日期内的结果缓存（避免重复存盘） */

	/** 存档目录：取「最近写过存档」的那个子目录（换存档后目录名会变） */
	const savesDir = () => {
		if (config.saveDir) return config.saveDir
		const base = path.join(homedir(), 'Documents', 'Paradox Interactive', 'Stellaris', 'save games')
		try {
			let best = null
			for (const d of readdirSync(base, { withFileTypes: true })) {
				if (!d.isDirectory()) continue
				const dir = path.join(base, d.name)
				for (const f of readdirSync(dir)) {
					if (!f.endsWith('.sav')) continue
					const t = statSync(path.join(dir, f)).mtimeMs
					if (!best || t > best.t) best = { dir, t }
				}
			}
			if (best) return best.dir
		} catch {}
		return path.join(base, '_1449518320')
	}

	const fileSize = (file) => {
		try {
			return statSync(file).size
		} catch {
			return -1
		}
	}

	/** 等存档写完：连续两次大小一致且非零 */
	const waitForSave = async (file, timeoutMs = 20000) => {
		const started = Date.now()
		let last = -1
		let stable = 0
		while (Date.now() - started < timeoutMs) {
			await new Promise((r) => setTimeout(r, 250))
			const size = fileSize(file)
			if (size > 0 && size === last) {
				if (++stable >= 2) return size
			} else {
				stable = 0
			}
			last = size
		}
		return fileSize(file)
	}

	/* ---- 星海共同体：存一次档 → 解析成员 / 排队 / 投票 / 已通过 → 删临时档 ---- */

	const runCommunityReport = (file) =>
		new Promise((resolve) => {
			const python = config.python || 'C:/Users/user/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe'
			const script =
				config.communityScript ||
				path.join(DEFAULTS.workspace || 'C:/Users/user/Documents/deepseek-harness/default-workspace', 'stellaris-save', 'community_report.py')
			execFile(
				python,
				[script, file, '--json', '--no-loc'],
				{ windowsHide: true, timeout: 120000, encoding: 'utf8', env: { ...process.env, PYTHONIOENCODING: 'utf-8', PYTHONUTF8: '1' } },
				(error, stdout, stderr) => {
					if (error) return resolve({ error: stderr || error.message })
					try {
						resolve({ json: JSON.parse(stdout) })
					} catch (e) {
						resolve({ error: `解析失败: ${e.message}` })
					}
				},
			)
		})

	/* ---- 星海共同体：内存优先（抓到共同体对象就秒回），没抓到才退回临时存档 ---- */

	/** 游戏日期 u32：值 = (年*360 + (月-1)*30 + (日-1)) * 24 + 43800000（四个已知日期对出来的） */
	const decodeGameDate = (v) => {
		if (!v || v < 43800000) return ''
		const ord = Math.floor((v - 43800000) / 24)
		const y = Math.floor(ord / 360)
		const rem = ord % 360
		const m = Math.floor(rem / 30) + 1
		const d = (rem % 30) + 1
		return `${y}.${String(m).padStart(2, '0')}.${String(d).padStart(2, '0')}`
	}

	add({
		name: 'stellaris_community',
		description:
			'Galactic Community state. Prefers memory: once the community object has been captured (install_community_capture + one save), members / council / the resolution on the floor / the queue / what passed / the day counters all come straight out of it in milliseconds. Without a capture it falls back to a temp save + community_report.py (~8 s, pausing for an instant). `voting` is null during a recess; supporters/opponents come from the resolution, abstentions are the members in neither list. To arm the fast path: stellaris_state { method: "install_community_capture" }, then a save.',
		parameters: {
			type: 'object',
			properties: {
				pause: { type: 'boolean', description: 'pause for an instant around the temp save (only on the fallback path; default true)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const shouldPause = args?.pause !== false

			let status
			try {
				status = await rpc(pipe, 'get_status')
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
			const wasPaused = status?.is_paused === true

			/* 国家名：桥的联系表（引擎拼好的显示名），两条路共用；联系表不含自己，单独补一次 */
			const contactName = new Map()
			try {
				const contacts = await rpc(pipe, 'get_contacts', { all: true })
				for (const row of contacts?.empires ?? []) contactName.set(Number(row.country_id), String(row.name ?? ''))
			} catch {}
			try {
				const me = await rpc(pipe, 'get_player_country')
				if (me && me.country_id != null) contactName.set(Number(me.country_id), String(me.name ?? ''))
			} catch {}
			const byId = (id) => Number(id) & 0xffffff
			const who = (id, fallback) => {
				if (id == null || id === 0xffffffff) return fallback || '—'
				const bare = byId(id)
				return contactName.get(bare) || fallback || `country ${bare}`
			}
			const localize = async (keys) => {
				const list = [...new Set(keys)].filter(Boolean)
				if (!list.length) return {}
				try {
					const answer = await rpc(pipe, 'localize', { keys: list })
					return answer?.names ?? {}
				} catch {
					return {}
				}
			}

			/* —— 内存路径 —— */
			let capture = null
			try {
				capture = await rpc(pipe, 'read_community_capture')
			} catch {}
			if (capture?.captured) {
				let all = null
				try {
					all = await rpc(pipe, 'get_resolutions')
				} catch {}
				if (all?.success) {
					const byRes = new Map((all.items ?? []).map((r) => [r.id, r]))
					const names = await localize([...byRes.values()].map((r) => r.type))
					const rname = (r) => names[r?.type] || r?.type || '?'
					const members = capture.members ?? []
					const council = capture.council ?? []

					const lines = []
					lines.push(`共同体　${decodeGameDate(capture.formed_raw) || '?'} 成立　席位 ${capture.council_positions ?? '?'}`)
					lines.push(`领袖　${who(capture.leader)}`)
					lines.push(`成员 ${members.length}：${members.map((id) => who(id)).join(' / ')}`)
					lines.push(`议会：${council.map((id) => who(id)).join(' / ')}`)
					if (typeof capture.vote_length === 'number' && typeof capture.days === 'number') {
						lines.push(`本届会议 ${capture.days}/${capture.vote_length} 天，剩 ${Math.max(0, capture.vote_length - capture.days)} 天`)
					}
					lines.push('')

					const floor = byRes.get(capture.voting)
					if (!floor) {
						lines.push('正在投票：无（休会中）')
					} else {
						const sup = floor.supporters ?? []
						const opp = floor.opponents ?? []
						const said = new Set([...sup, ...opp].map(byId))
						const rest = members.filter((id) => !said.has(byId(id)))
						lines.push(`正在投票　#${floor.id} ${rname(floor)}　提案 ${who(floor.country)}`)
						lines.push(`  支持 ${sup.length}／反对 ${opp.length}／弃权 ${rest.length}`)
						if (sup.length) lines.push(`  支持：${sup.map((id) => who(id)).join(' / ')}`)
						if (opp.length) lines.push(`  反对：${opp.map((id) => who(id)).join(' / ')}`)
						if (rest.length) lines.push(`  弃权：${rest.map((id) => who(id)).join(' / ')}`)
					}
					lines.push('')

					const queue = (capture.proposed ?? []).map((id) => byRes.get(id)).filter(Boolean)
					lines.push(`排队 ${queue.length} 条（先上先议）`)
					queue.forEach((q, i) => {
						lines.push(`  ${String(i + 1).padStart(2)}. #${q.id} ${rname(q)}　支持 ${(q.supporters ?? []).length}／反对 ${(q.opponents ?? []).length}　提案 ${who(q.country)}`)
					})
					lines.push('')

					const passed = (capture.passed ?? []).map((id) => byRes.get(id)).filter(Boolean)
					passed.sort((a, b) => String(b.pass_date ?? '').localeCompare(String(a.pass_date ?? '')))
					lines.push(`已通过 ${passed.length} 条`)
					for (const p of passed) lines.push(`  #${p.id} ${rname(p)}　${p.pass_date || '?'} 通过`)
					return text(lines.join('\n'))
				}
			}

			/* —— 回退：临时存档 + community_report.py —— */
			const tempName = 'zz-dsh-comm'
			const file = path.join(savesDir(), `${tempName}.sav`)

			try {
				if (shouldPause && !wasPaused) await rpc(pipe, 'set_paused', { paused: true })
				const saved = await rpc(pipe, 'save_game', { name: tempName, overwrite: true })
				if (saved && saved.success === false) return text(`存档失败：${saved.error || saved.message || '未知'}`)
				const size = await waitForSave(file)
				if (size <= 0) return text(`存档没有写出来：${file}`)

				const result = await runCommunityReport(file)
				if (result.error) return text(`解析失败：${result.error}`)
				const data = result.json

				/* 议案名走引擎本地化（键就是 type 本身） */
				const typeKeys = new Set()
				const collect = (r) => r?.type && typeKeys.add(r.type)
				collect(data.voting)
				for (const q of data.queue ?? []) collect(q)
				for (const p of data.passed ?? []) collect(p)
				const resNames = await localize([...typeKeys])
				const rname = (r) => resNames[r?.type] || r?.name || r?.type || '?'

				const c = data.community
				const lines = []
				lines.push(`共同体　${c.formed ?? '?'} 成立　议会 ${c.council_established ?? '?'}　席位 ${c.council_positions ?? '?'}`)
				lines.push(`领袖　${who(c.leader_id, c.leader)}`)
				lines.push(`成员 ${c.members.length}：${c.members.map((n, i) => who(c.member_ids[i], n)).join(' / ')}`)
				lines.push(`议会：${c.council.map((n, i) => who(c.council_ids[i], n)).join(' / ')}`)
				if (c.days_left != null) lines.push(`本届会议 ${c.days}/${c.vote_length} 天，剩 ${c.days_left} 天`)
				lines.push('')

				const v = data.voting
				if (!v) {
					lines.push('正在投票：无（休会中）')
				} else {
					lines.push(`正在投票　#${v.id} ${rname(v)}　提案 ${who(v.proposer_id, v.proposer)}`)
					lines.push(`  支持 ${v.supported_ids.length}／反对 ${v.opposed_ids.length}／弃权 ${v.abstain_ids.length}`)
					if (v.supported_ids.length) lines.push(`  支持：${v.supported_ids.map((id) => who(id, v.supported[v.supported_ids.indexOf(id)])).join(' / ')}`)
					if (v.opposed_ids.length) lines.push(`  反对：${v.opposed_ids.map((id) => who(id, v.opposed[v.opposed_ids.indexOf(id)])).join(' / ')}`)
					if (v.abstain_ids.length) lines.push(`  弃权：${v.abstain_ids.map((id) => who(id, v.abstain[v.abstain_ids.indexOf(id)])).join(' / ')}`)
					if (v.voted?.length) lines.push(`  已投 ${v.voted.length}：${v.voted.map((x) => `${who(x.country_id, x.country)}(${x.date})`).join(' / ')}`)
				}
				lines.push('')

				lines.push(`排队 ${data.queue.length} 条（先上先议）`)
				data.queue.forEach((q, i) => {
					lines.push(`  ${String(i + 1).padStart(2)}. #${q.id} ${rname(q)}　支持 ${q.supported_count}／反对 ${q.opposed_count}　提案 ${who(q.proposer_id, q.proposer)}`)
				})
				lines.push('')

				lines.push(`已通过 ${data.passed.length} 条`)
				const passed = [...data.passed].sort((a, b) => String(b.pass_date ?? '').localeCompare(String(a.pass_date ?? '')))
				for (const p of passed) lines.push(`  #${p.id} ${rname(p)}　${p.pass_date ?? '?'} 通过`)

				if (Object.keys(c.category_timers ?? {}).length) {
					lines.push('')
					lines.push('类别冷却（该类别可再提之日）')
					for (const [k, day] of Object.entries(c.category_timers)) lines.push(`  ${k}  ${day}`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`出错：${error.message}`)
			} finally {
				try {
					await fs.unlink(file)
				} catch {}
				if (shouldPause && !wasPaused) {
					try {
						await rpc(pipe, 'set_paused', { paused: false })
					} catch {}
				}
			}
		},
	})

	/* ---- 议案清单：按类别分组，标出现在能不能提（引擎的 allow 判定） ---- */

	add({
		name: 'stellaris_resolutions',
		description:
			'Resolution types grouped by category, with the engine\'s own verdict on whether each can be submitted right now. `proposable` comes from building a `propose_resolution_command` per type and running IsValid, so the allow block (already active / already proposed by someone / category cooldown / needs custodian / needs the GDF…) is the engine\'s answer with its reason verbatim. `potential` is NOT part of IsValid: data/resolution-categories.json records which types only appear when an emperor / custodian exists, and those are filtered by facts read from the live community object. Default lists only what can be submitted now; `all: true` lists everything with reasons.',
		parameters: {
			type: 'object',
			properties: {
				all: { type: 'boolean', description: 'also list the ones the engine refuses, with its reason' },
				country_id: { type: 'number', description: 'test as this country (default: the player)' },
				target_id: {
					type: 'number',
					description: 'target country for the target = yes resolutions (declare crisis, denounce, imperial charter…). Without it those cannot be judged and show no reason',
				},
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const all = args?.all === true
			const params = {}
			if (all) params.all = true
			if (args?.country_id != null) params.country_id = Number(args.country_id)
			if (args?.target_id != null) params.target_id = Number(args.target_id)

			let index = {}
			try {
				/* 数据文件跟着插件走，不要用 DEFAULTS.workspace（DSH 进程的 cwd 不是工作区） */
				const file = path.join(here, 'data', 'resolution-categories.json')
				index = JSON.parse(readFileSync(file, 'utf8'))
			} catch {}
			/* 引擎给的本地化名偶尔太短（例如只写「废除」），这里补一次 */
			let overrides = {}
			try {
				overrides = JSON.parse(readFileSync(path.join(here, 'data', 'resolution-name-overrides.json'), 'utf8'))
			} catch {}

			let cap = null
			try {
				cap = await rpc(pipe, 'read_community_capture')
			} catch {}
			const hasCustodian = Number(cap?.custodian_term ?? 0) > 0
			const hasEmperor = Number(cap?.imperial_authority ?? 0) > 0

			/* 「废除 X」只能废正生效的那条：把已通过的议案类型集合拼出来（potential 里的
			   is_active_resolution 引擎不判，得我们自己筛） */
			let activeTypes = null
			if (Array.isArray(cap?.passed) && cap.passed.length) {
				try {
					const allRes = await rpc(pipe, 'get_resolutions')
					if (allRes?.success) {
						const passed = new Set(cap.passed)
						activeTypes = new Set((allRes.items ?? []).filter((r) => passed.has(r.id)).map((r) => r.type))
					}
				} catch {}
			}

			const res = await rpc(pipe, 'probe_resolution_types', params)
			if (!res?.success) return text(`bridge: ${res?.error ?? 'probe failed'}`)

			const rows = (res.items ?? []).filter((r) => {
				const meta = index[r.key] ?? {}
				if (meta.requires === 'emperor' && !hasEmperor) return false
				if (meta.requires === 'custodian' && !hasCustodian) return false
				/* 「没有帝皇／监管人时才存在」的那批，反过来过滤 */
				if (meta.requires === 'no_emperor' && hasEmperor) return false
				if (meta.requires === 'no_custodian' && hasCustodian) return false
				/* 「废除 X」：X 不在生效集合里就提不了 */
				if (meta.active && activeTypes && !activeTypes.has(meta.active)) return false
				return true
			})

			let names = {}
			try {
				const keys = [...rows.map((r) => r.key), ...Object.values(index).map((m) => m?.category)]
				const answer = await rpc(pipe, 'localize', { keys: [...new Set(keys)].filter(Boolean) })
				names = answer?.names ?? {}
			} catch {}
			/* 议案名：本地化优先，太短的那些用 data/resolution-name-overrides.json 里的补名 */
			const label = (key) => overrides[key] || names[key] || key

			const groups = new Map()
			for (const r of rows) {
				const cat = index[r.key]?.category || ''
				if (!groups.has(cat)) groups.set(cat, [])
				groups.get(cat).push(r)
			}

			const lines = []
			lines.push(`议案 ${rows.length} 条（按类别）　★ = 现在可提交　${hasCustodian ? '有监管人' : '无监管人'}${hasEmperor ? ' · 有帝皇' : ''}${args?.target_id != null ? `　目标 country ${Number(args.target_id) & 0xffffff}` : ''}`)
			if (!Object.keys(index).length) {
				lines.push('（类别表没读到：data/resolution-categories.json，跑一次 scripts/resolution_index.py）')
			}
			const order = [...groups.keys()].sort((a, b) => String(names[a] ?? a).localeCompare(String(names[b] ?? b)))
			for (const cat of order) {
				const list = groups.get(cat).sort((a, b) => Number(b.proposable) - Number(a.proposable) || a.key.localeCompare(b.key))
				lines.push('')
				lines.push(`■ ${names[cat] ?? (cat || '（未归类）')}`)
				for (const r of list) {
					const star = r.proposable ? '★' : '·'
					let why = !r.proposable && r.reason ? `　— ${String(r.reason).replace(/\s+/g, ' ').trim()}` : ''
					/* 带 target 的议案没有目标时引擎不给答案——把原因写出来，别让它看起来像「被拒」 */
					if (!r.proposable && !r.reason && index[r.key]?.target && args?.target_id == null) {
						why = '　— （带目标，需 target_id；不给目标时引擎不判）'
					}
					lines.push(`  ${star} ${label(r.key)}${why}`)
				}
			}
			return text(lines.join('\n'))
		},
	})

	/* ---- 提交议案：先按跟清单同一套筛选拦一次（桥只跑 IsValid，不查 potential） ---- */

	add({
		name: 'stellaris_propose_resolution',
		description:
			'Submit one resolution. The engine wants a pointer to the resolution TYPE, so the bridge looks it up by key in CResolutionTypeDatabase, builds `propose_resolution_command` (type + our country + target), runs IsValid and posts. Because IsValid only covers the allow block, this tool first applies the same potential filters as stellaris_resolutions (emperor / custodian / `is_active_resolution` repeal gating) and refuses with the reason — otherwise it would submit a resolution the UI never offers. `dry_run: true` validates only.',
		parameters: {
			type: 'object',
			properties: {
				type: { type: 'string', description: 'resolution type key, e.g. resolution_industry_repeal_1 (from stellaris_resolutions)' },
				target_id: { type: 'number', description: 'target country for target = yes resolutions' },
				dry_run: { type: 'boolean', description: 'only run IsValid, post nothing' },
			},
			required: ['type'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const key = String(args?.type ?? '').trim()
			if (!key) return text('需要 type（议案类型键，见 stellaris_resolutions）')
			const targetId = args?.target_id != null ? Number(args.target_id) : null
			const dryRun = args?.dry_run === true

			let index = {}
			try {
				index = JSON.parse(readFileSync(path.join(here, 'data', 'resolution-categories.json'), 'utf8'))
			} catch {}
			const meta = index[key]
			if (!meta) {
				return text(`没有这个议案类型：${key}（键要去 stellaris_resolutions 里取）`)
			}

			let cap = null
			try {
				cap = await rpc(pipe, 'read_community_capture')
			} catch {}
			const hasCustodian = Number(cap?.custodian_term ?? 0) > 0
			const hasEmperor = Number(cap?.imperial_authority ?? 0) > 0

			/* potential 那一层：跟清单用同一套判据 */
			if (meta.requires === 'emperor' && !hasEmperor) return text('引擎不会列出这条：需要星海帝皇')
			if (meta.requires === 'custodian' && !hasCustodian) return text('引擎不会列出这条：需要星海监管人')
			if (meta.requires === 'no_emperor' && hasEmperor) return text('引擎不会列出这条：有帝皇时不存在')
			if (meta.requires === 'no_custodian' && hasCustodian) return text('引擎不会列出这条：有监管人时不存在')
			if (meta.target && targetId == null) return text('这条议案要指定目标国（target_id）')
			if (meta.active) {
				let activeTypes = null
				if (Array.isArray(cap?.passed) && cap.passed.length) {
					try {
						const allRes = await rpc(pipe, 'get_resolutions')
						if (allRes?.success) {
							const passed = new Set(cap.passed)
							activeTypes = new Set((allRes.items ?? []).filter((r) => passed.has(r.id)).map((r) => r.type))
						}
					} catch {}
				}
				if (activeTypes && !activeTypes.has(meta.active)) {
					return text(`「${key}」废的是 ${meta.active}，它现在没有生效——引擎不会给出这条`)
				}
			}

			const params = { type_key: key }
			if (targetId != null) params.target_id = targetId
			if (dryRun) params.dry_run = true
			const res = await rpc(pipe, 'propose_resolution', params)
			if (!res?.success) {
				const why = res?.reason || res?.error || '未知'
				return text(`没提上去（${res?.stage ?? '?'}）：${why}`)
			}
			const target = targetId != null ? `　目标 country ${targetId & 0xffffff}` : ''
			if (res.dry_run) return text(`校验通过（未投递）：${key}${target}`)
			return text(`已提交：${key}${target}　（去 stellaris_community 的「排队」里看）`)
		},
	})

	/* ---- 对在台上的议案投票 / 提前结束会议 ---- */

	add({
		name: 'stellaris_resolution_vote',
		description:
			'Vote on the resolution on the senate floor, or end the session early (custodian only). `action`: support (vote 0) / oppose (vote 1) / abstain (vote 2) / end_session (CEndSenateSessionCommand). `resolution_id` defaults to the resolution currently on the floor (read from the captured community object). The engine refuses a vote change within 120 days ("近期已对此议案投票…") and an early end before the cut-off — its reasons come back verbatim. All three vote values are measured: 1 put us into the opponents list, 2 took us out of the supporters without entering the opponents, so 0 is yes (note this is the OPPOSITE of CDiploActionVoteCommand, where 1 = yes).',
		parameters: {
			type: 'object',
			properties: {
				action: { type: 'string', enum: ['support', 'oppose', 'abstain', 'end_session'], description: 'what to do on the floor resolution' },
				resolution_id: { type: 'number', description: 'resolution id (default: the one on the floor)' },
				vote: { type: 'number', description: 'raw vote value override (2 = abstain measured; 1 / 0 not measured)' },
				dry_run: { type: 'boolean', description: 'only run IsValid, post nothing' },
			},
			required: ['action'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const action = String(args?.action ?? '')
			const dryRun = args?.dry_run === true

			let resolutionId = args?.resolution_id != null ? Number(args.resolution_id) : null
			if (resolutionId == null) {
				let cap = null
				try {
					cap = await rpc(pipe, 'read_community_capture')
				} catch {}
				resolutionId = cap?.voting
				if (resolutionId == null || resolutionId < 0) {
					return text('没有议案在台上（休会中）；要给 resolution_id 才行')
				}
			}

			const legacy = action === 'end_session'
			/* 取值实测（2026-11-21）：0 = 赞成、1 = 反对、2 = 弃权——与联邦表决那条命令正好相反 */
			const voteMap = { support: 0, oppose: 1, abstain: 2 }
			let vote = args?.vote != null ? Number(args.vote) : voteMap[action]
			if (!legacy && vote == null) {
				return text('action 只能是 support / oppose / abstain / end_session')
			}

			if (action === 'end_session') {
				const res = await rpc(pipe, 'end_senate_session', dryRun ? { resolution_id: resolutionId, dry_run: true } : { resolution_id: resolutionId })
				if (!res?.success) return text(`没结束（${res?.stage ?? '?'}）：${res?.reason || res?.error || '未知'}`)
				return text(res.dry_run ? `校验通过（未投递）：提前结束 #${resolutionId}` : `已请求提前结束 #${resolutionId}`)
			}

			const params = { resolution_id: resolutionId, vote }
			if (dryRun) params.dry_run = true
			const res = await rpc(pipe, 'vote_resolution', params)
			if (!res?.success) return text(`没投出去（${res?.stage ?? '?'}）：${res?.reason || res?.error || '未知'}`)
			const label = { 0: '赞成', 1: '反对', 2: '弃权' }[vote] ?? `vote=${vote}`
			return text(res.dry_run ? `校验通过（未投递）：#${resolutionId} ${label}` : `已投：#${resolutionId} ${label}`)
		},
	})

	/* ---- 排队中的议案：支持 / 反对 / 弃权 / 否决 / 冻结 / 紧急措施 ---- */

	add({
		name: 'stellaris_queued_resolution',
		description:
			'Act on a resolution that is still in the queue (not on the floor). `action`: support (CSupportProposedResolutionCommand) / oppose (COppose…) / abstain (= withdraw support, CRemoveSupport…) / veto (CVetoResolutionCommand — removes it from the queue and puts the proposer on proposal cooldown; council) / freeze (CFreezeProposedResolutionCommand — 1440 days without reaching the senate; council) / emergency (CProposeEmergencyMeasureCommand — jumps the queue and the recess straight onto the senate floor; council). These carry no 120-day vote cooldown (unlike the floor vote). The engine refuses when the actor lacks the seat/role and its reason comes back verbatim. `resolution_id` is the # shown in stellaris_community’s queue.',
		parameters: {
			type: 'object',
			properties: {
				resolution_id: { type: 'number', description: 'queued resolution id (the # in the queue)' },
				action: {
					type: 'string',
					enum: ['support', 'oppose', 'abstain', 'veto', 'freeze', 'emergency'],
					description: 'support / oppose / abstain (withdraw support) / veto / freeze / emergency',
				},
				dry_run: { type: 'boolean', description: 'only run IsValid, post nothing' },
			},
			required: ['resolution_id', 'action'],
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			const id = Number(args?.resolution_id)
			const action = String(args?.action ?? '')
			const dryRun = args?.dry_run === true
			const label = {
				support: '支持', oppose: '反对', abstain: '弃权（撤支持）',
				veto: '理事会否决', freeze: '冻结', emergency: '紧急措施',
			}[action]
			if (!label) return text('action 只能是 support / oppose / abstain / veto / freeze / emergency')

			const params = { resolution_id: id, action }
			if (dryRun) params.dry_run = true
			const res = await rpc(pipe, 'queued_resolution_action', params)
			if (!res?.success) {
				return text(`没成（${res?.stage ?? '?'}）：${res?.reason || res?.error || '未知'}`)
			}
			return text(res.dry_run ? `校验通过（未投递）：#${id} ${label}` : `已提交：#${id} ${label}　（去 stellaris_community 的队列看变化）`)
		},
	})



	add({
		name: 'stellaris_colonies',
		description:
			'List the empire colonies — one row per colony, planet or Nomads ark (bridge get_colonies). ' +
			'Each row: colony id, kind, name, population, districts, job slots, stability and the **net monthly output** ' +
			'(top three resources by absolute value, from CColony +0xEC0). An ark colony has ' +
			'no CPlanet at all — the same CColony hangs off CShip::colony +0x100 — so this reads the colony ' +
			'object itself and works for both kinds. detail:true also prints every colony\'s district/zone tree ' +
			'(bridge probe_colony_structures). Two arks can share a display name, so key on colony_id, not the name.',
		parameters: {
			type: 'object',
			properties: {
				detail: { type: 'boolean', description: 'also print each colony\'s districts/zones/buildings' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const d = await rpc(pipe, 'get_colonies')
				if (d?.success !== true) return text(bridgeFail(d))
				const cols = Array.isArray(d?.colonies) ? d.colonies : []
				if (!cols.length) return text('没有殖民地')
				/* 方舟与它的 CStarbase 是两棵树：顺手把 colony → 基地 id 对上，概览里就能直接拿 id */
				const arkBase = new Map()
				try {
					const ob = await rpc(pipe, 'get_our_starbases', {})
					if (ob?.success === true) {
						for (const a of ob.arks ?? []) {
							if (typeof a.colony_id === 'number') arkBase.set(a.colony_id, a.starbase_id)
						}
					}
				} catch {}
				/* 每块殖民地读一次净额（产出 − 维护），只取绝对值前 3 项印在概览行尾 */
				const prods = new Map()
				for (const c of cols) {
					const pc = await probeColony(pipe, c.colony_id)
					if (!pc?.ptr) continue
					let p = await readColonyProduction(pipe, Number(pc.ptr))
					if (!p) p = await readColonyProduction(pipe, Number(pc.ptr))
					if (p) prods.set(c.colony_id, p)
				}
				const resKeys = [...new Set([...prods.values()].flatMap((p) => Object.keys(p.net ?? {})))]
				const resNames = resKeys.length ? ((await rpc(pipe, 'localize', { keys: resKeys }).catch(() => null))?.names ?? {}) : {}
				const resLabel = (k) => (resNames[k] && resNames[k] !== k ? resNames[k] : k)
				const netTop3 = (p) => {
					const rows = Object.entries(p?.net ?? {})
						.filter(([, v]) => Math.abs(v) >= 0.05)
						.sort((a, b) => Math.abs(b[1]) - Math.abs(a[1]))
						.slice(0, 3)
					return rows.map(([k, v]) => `${resLabel(k)} ${v > 0 ? '+' : ''}${v.toFixed(0)}`).join(' / ')
				}
				const lines = [
					`殖民地 ${d.total} 个（行星 ${d.planet_carriers ?? 0} / 方舟 ${d.ship_carriers ?? 0}）`,
				]
				for (const c of cols) {
					const stab = Number(c.stability_fixed) >= 0 ? (Number(c.stability_fixed) / 1e5).toFixed(1) : '?'
					const tag = c.kind === 'ark' ? '▲方舟' : '行星'
					const net = prods.has(c.colony_id) ? netTop3(prods.get(c.colony_id)) : ''
					lines.push(
						`  [${c.colony_id}] ${c.name ?? '?'}（${tag}${c.kind === 'ark' ? ` ship ${c.carrier_id}` : ` planet ${c.carrier_id}`}）` +
							`｜人口 ${c.pops ?? '?'}｜飞升 ${c.ascension_tier ?? 0}｜稳定 ${stab}%｜区划 ${c.districts ?? '?'}｜岗位 ${c.pop_jobs ?? '?'}｜人口组 ${c.pop_groups ?? '?'}` +
							(c.planet_size !== undefined ? `｜尺寸 ${c.planet_size}` : '') +
							(c.kind === 'ark' && arkBase.has(c.colony_id) ? `｜基地 ${arkBase.get(c.colony_id)}` : '') +
							(net ? `｜净 ${net}` : ''),
					)
				}
				if (args?.detail) {
					for (const c of cols) {
						const s = await rpc(pipe, 'probe_colony_structures', { colony_id: c.colony_id })
						if (s?.success !== true) { lines.push(`  [${c.colony_id}] 结构读不到：${s?.error ?? '?'}`); continue }
						lines.push('')
						lines.push(`[${c.colony_id}] ${c.name ?? '?'} 的结构`)
						for (const ds of s.districts ?? []) {
							lines.push(`  区划 ${ds.name} ${ds.type}  level ${ds.level}`)
							for (const z of ds.slots ?? []) {
								const bs = z.buildings ?? []
								lines.push(`     槽 ${z.slot}  ${z.zone_name ?? '（空槽）'} ${z.zone ?? ''}（zone_id ${z.zone_id}）  建筑 ${bs.length}`)
								for (const b of bs) lines.push(`          id ${b.building_id}  ${b.name ?? '?'} ${b.key ?? ''}`)
							}
						}
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text('殖民地列表：' + error.message)
			}
		},
	})

	/* ---- 舰队列表：自有 / 借出 / 借入（桥 get_military_fleets 的 owner / controller / relation）----
	   2026-11-21：借出期间 owner 不变、controller 换成借用国（玩家在界面上确认）。桥现在把
	   借出（owner=本国、controller=别国）与借入（controller=本国、owner=别国）都列出来，
	   并给出 ownership_status / lease_period（剩余天数）。 */
	add({
		name: 'stellaris_fleets',
		description:
			'List military fleets with owner and controller: own, lent out (owner is us, controller is someone else) ' +
			'and borrowed (controller is us, owner someone else). The loan is an agreement with a duration, so lent ' +
			'fleets carry ownership_status and lease_period (days left). Bridged from get_military_fleets, which now ' +
			'also walks the fleet database for borrowed fleets (they have no template of ours, so the template-driven ' +
			'list alone never showed them). relation: own / lent / borrowed.',
		parameters: {
			type: 'object',
			properties: {
				relation: {
					type: 'string',
					enum: ['all', 'own', 'lent', 'borrowed', 'ark'],
					description: '只列某一类（默认 all；ark = 方舟舰，按军用口径列出）',
				},
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const data = await rpc(pipe, 'get_military_fleets')
				let fleets = Array.isArray(data?.fleets) ? data.fleets : []
				if (!fleets.length) return text('没有军事舰队')
				const want = String(args?.relation || 'all')
				if (want !== 'all') fleets = fleets.filter((f) => f.relation === want)
				if (!fleets.length) return text(`没有 ${want} 的舰队`)
				const count = (rel) => (Array.isArray(data?.fleets) ? data.fleets.filter((f) => f.relation === rel).length : 0)
				const lines = [`军事舰队 ${data.total_military_fleets} 支（自有 ${count('own')} / 借出 ${count('lent')} / 借入 ${count('borrowed')} / 方舟 ${count('ark')}）`]
				const pad = (s, n) => s + ' '.repeat(Math.max(0, n - [...s].length))
				for (const f of fleets) {
					const tag = f.relation === 'lent'
						? `借出 → ${f.controller_name || f.controller}`
						: f.relation === 'borrowed'
							? `借入 ← ${f.owner_name || f.owner}`
							: f.relation === 'ark'
								? `方舟（殖民地 ${f.colony_id ?? '?'}）`
								: '自有'
					const lease = Number.isFinite(Number(f.lease_period)) && Number(f.ownership_status) === 1
						? `，剩 ${Number(f.lease_period)} 天`
						: ''
					const bomb = f.bombardment_stance ? `　轰炸 ${f.bombardment_stance}` : ''
					lines.push(
						`  ${pad(String(f.name ?? ''), 16)}${pad(tag, 26)}${String(Math.round(Number(f.military_power) || 0)).padStart(9)} 战力` +
							`　${f.total_ships} 艘${f.status ? `　${f.status}` : ''}${lease}${bomb}`,
					)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text('舰队列表：' + error.message)
			}
		},
	})

	add({
		name: 'stellaris_fleet_composition',
		description:
			'读一支军事舰队的真实构成：实际几艘、每条编制要几艘、差多少。需要本地编译的桥（见 stellaris-bridge-patch/README.md）；官方 0.7.0 的计数是错的。',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'fleet id; omit to list every military fleet' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const data = await rpc(pipe, 'get_fleets')
				const fleets = (data && data.fleets ? data.fleets : []).filter((f) =>
					args && args.fleet_id !== undefined ? f.fleet_id === Number(args.fleet_id) : true,
				)
				if (fleets.length === 0) return text('没有找到这支舰队')
				const lines = []
				for (const f of fleets) {
					lines.push(f.name + '（#' + f.fleet_id + '）共 ' + f.total_ships + ' 艘，编制 ' + f.total_quota + ' 艘')
					for (const d of f.designs || []) {
						const gap = d.target_quota - d.actual_count
						const note = gap > 0 ? '缺 ' + gap : gap < 0 ? '超编 ' + -gap : '正好'
						lines.push('  ' + d.design_name + '：实际 ' + d.actual_count + ' / 编制 ' + d.target_quota + '（' + note + '）')
					}
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text('舰队编制：' + error.message)
			}
		},
	})

		/* ---- 舰队编制模板库（2026-11-22）----
	   数据源是桥的 get_fleets：它是按「国家自己的模板向量」（CCountry.fleet_template_manager）
	   枚举的，所以**不依赖某支舰队**也能看到模板——模板名、编制总数、设计条目与各自 quota 都在里面。
	   再配 get_ship_designs 把 design_id 换成舰型键，用来认兽群（crystal_ship_* / voidworms_*）。 */
	add({
		name: 'stellaris_fleet_templates',
		description:
			'The **fleet template library**, read without needing a built fleet: bridge get_fleets enumerates the country\'s OWN template vector (CCountry.fleet_template_manager), so each row is a template — its name, template_id, total quota, ships present, and every design entry with that entry\'s quota. Each entry\'s design name and ship_size come from the **global** CShipDesign table (get_all_designs), which — unlike get_ship_designs — also covers event ships (NAME_Derelict…) and every space-fauna design; a fauna template carries a design whose ship_size starts with crystal_ / voidworm_ / space_ / cutholoid_ (each fauna kind gets an auto-generated design). fauna:true keeps only those; raw:true returns the joined JSON. This is the template-level view — stellaris_fleet_composition is the per-fleet view (actual ships vs quota).',
		parameters: {
			type: 'object',
			properties: {
				fauna: { type: 'boolean', description: 'only templates that contain a space-fauna design' },
				raw: { type: 'boolean', description: 'return the joined JSON (templates + design labels)' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				// 设计名与舰型来自**全局** CShipDesign 库（get_all_designs）：本国的 ship_design_collection
				// 看不到事件船（NAME_Derelict 那种）与兽群的部分设计，全局库才齐
				// 串行：同一个管道上并发两问会互相踩（实测 ship_size 全成 null，就是因为 Promise.all 抢管道）
				const f = await rpc(pipe, 'get_fleets', {})
				const idx = await rpc(pipe, 'get_all_designs', { limit: 5000 }).catch(() => null)
				if (f?.error) return text(bridgeFail(f))
				const rows = Array.isArray(f?.fleets) ? f.fleets : []
				const designs = Array.isArray(idx?.designs) ? idx.designs : []
				const sizeOf = new Map(designs.map((d) => [d.design_id, d.ship_size]))
				const nameOf = new Map(designs.map((d) => [d.design_id, d.name]))
				const isFaunaDesign = (id, label) => {
					const size = String(sizeOf.get(id) ?? '')
					if (/^(crystal|voidworm|space_|cutholoid|amoeba)/i.test(size)) return true
					return /CRYSTALLINE|VOIDWORM|SPACE_AMOEBA|TIYANKI|CUTHOLOID/i.test(String(label ?? ''))
				}
				const templates = rows.map((r) => ({
					name: r.name,
					template_id: r.template_id,
					fleet_id: r.fleet_id,
					total_quota: r.total_quota ?? 0,
					total_ships: r.total_ships ?? 0,
					entries: (r.designs ?? []).map((d) => ({
						design_id: d.design_id,
						design_name: nameOf.get(d.design_id) || d.design_name,
						ship_size: sizeOf.get(d.design_id) ?? null,
						quota: d.target_quota ?? 0,
					})),
				}))
				// 设计名在全局库里是本地化键（MOL4_SHIP_SunkenHorror / NAME_Derelict…），批量翻一次
				const nameKeys = [...new Set(templates.flatMap((t) => t.entries.map((e) => e.design_name)).filter(Boolean))]
				const locNames = nameKeys.length
					? ((await rpc(config.pipe || DEFAULTS.pipe, 'localize', { keys: nameKeys }).catch(() => null))?.names ?? {})
					: {}
				const cnName = (key) => {
					const hit = locNames[key]
					return hit && hit !== key ? hit : null
				}
				for (const t of templates) {
					for (const e of t.entries) e.display = cnName(e.design_name) || e.design_name
					t.fauna = t.entries.some((e) => isFaunaDesign(e.design_id, e.design_name))
				}
				if (args?.raw === true) return text(JSON.stringify(templates, null, 2))
				const fauna = templates.filter((t) => t.fauna)
				const list = args?.fauna === true ? fauna : templates
				const quotaTotal = templates.reduce((n, t) => n + t.total_quota, 0)
				const short = (t) => Math.max(0, t.total_quota - t.total_ships)
				const head = `模板 ${templates.length} 个｜合计编制 ${quotaTotal} 艘｜兽群 ${fauna.length} 支`
				// 同样的编制（同设计 + 同数量 + 同总数）合并成一组，先总后分
				const signature = (t) =>
					t.entries.map((e) => `${e.design_id}:${e.quota}`).sort().join('|') + `#${t.total_quota}`
				const entryLine = (e) => {
					const label = e.display || e.design_name || `design ${e.design_id}`
					const keyNote = e.display && e.design_name ? `（${e.design_name}）` : ''
					const size = e.ship_size ? ` · ${e.ship_size}` : ''
					return `${label}${keyNote}${size} × ${e.quota}`
				}
				const lines = [head]
				const lacking = list.filter((t) => short(t) > 0)
				if (lacking.length) {
					lines.push(`缺员 ${lacking.length} 支：`)
					for (const t of lacking) {
						lines.push(`  ${t.name}  ${t.total_ships}/${t.total_quota}（缺 ${short(t)}）  ${t.entries.map(entryLine).join('、')}`)
					}
				}
				const rest = list.filter((t) => short(t) === 0)
				if (rest.length) lines.push(lacking.length ? '其余模板：' : '')
				for (let i = lines.length - 1; i >= 0; i -= 1) if (lines[i] === '') lines.splice(i, 1)
				const full = new Map()
				for (const t of rest) {
					const k = signature(t)
					if (!full.has(k)) full.set(k, [])
					full.get(k).push(t)
				}
				for (const [k, members] of full) {
					const t0 = members[0]
					const quotaText = `${t0.total_ships}/${t0.total_quota}`
					const faMark = t0.fauna ? ' ★兽群' : ''
					if (members.length === 1) {
						lines.push(`  ${t0.name}  ${quotaText}  ${t0.entries.map(entryLine).join('、')}${faMark}`)
					} else {
						lines.push(`  ${members.length} 支同编制${faMark}：${t0.entries.map(entryLine).join('、')}`)
						lines.push(`    ${members.map((m) => `${m.name} ${quotaText}`).join('｜')}`)
					}
				}
				if (!list.length) lines.push('  （没有匹配的模板）')
				lines.push('提示：`stellaris_fleet_composition` 是按舰队看实际/编制；这只是按模板的目录')
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	/* ---- 运输舰队构成：陆军装在哪支舰队里（桥 get_armies + probe_fleet_ships） ---- */
	add({
		name: 'stellaris_fleet_armies',
		description:
			'Composition of the player\'s transport fleets: which armies are riding in which fleet. Each embarked army knows its transport ship (bridge get_armies -> transport_armies[].ship), and probe_fleet_ships gives a fleet\'s ship ids, so this joins the two. Give fleet_id for one fleet; omit it to list every transport fleet (get_civilian_fleets.transport_fleets). This is the army counterpart of stellaris_fleet_composition (which only covers military fleets).',
		parameters: {
			type: 'object',
			properties: {
				fleet_id: { type: 'number', description: 'transport fleet id; omit to list every transport fleet' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const pipe = config.pipe || DEFAULTS.pipe
			try {
				const armies = await rpc(pipe, 'get_armies')
				const byShip = new Map()
				for (const a of armies?.transport_armies ?? []) {
					if (typeof a.ship === 'number') byShip.set(a.ship, a)
				}
				let fleets
				if (typeof args?.fleet_id === 'number') {
					const civ = await rpc(pipe, 'get_civilian_fleets')
					const known = (civ?.transport_fleets ?? []).find((f) => f.fleet_id === args.fleet_id)
					fleets = [{ fleet_id: args.fleet_id, name: known?.name ?? '' }]
				} else {
					const civ = await rpc(pipe, 'get_civilian_fleets')
					fleets = civ?.transport_fleets ?? []
					if (!fleets.length) return text('没有运输舰队——陆军都在地面（或一支也没造）')
				}
				const lines = []
				for (const f of fleets) {
					const probe = await rpc(pipe, 'probe_fleet_ships', { fleet_id: f.fleet_id })
					const ships = probe?.ships ?? []
					const aboard = ships.map((s) => byShip.get(s.ship_id)).filter(Boolean)
					lines.push(`${f.name || '舰队'}（fleet ${f.fleet_id}）  ${ships.length} 艘，${aboard.length} 支陆军`)
					for (const a of aboard) {
						lines.push(
							`    ${a.name} ${a.type_key}  ${a.health}/${a.max_health}  战力 ${a.power}` +
								`${a.species_name ? `  ${a.species_name}` : ''}（ship ${a.ship}）`,
						)
					}
					if (aboard.length < ships.length) lines.push(`    （${ships.length - aboard.length} 艘没匹配到陆军）`)
				}
				return text(lines.join('\n'))
			} catch (error) {
				return text(`bridge: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_panel',
		description:
			'In-game panel: show or hide it by writing show.txt (the panel reads it every frame, even while closed), or pass log=N to read the last N panel log lines.',
		parameters: {
			type: 'object',
			properties: {
				show: { type: 'boolean', description: 'true to show, false to hide' },
				log: { type: 'number', description: 'instead of toggling: return the last N log lines' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			if (args?.log !== undefined) {
				try {
					const all = (await fs.readFile(logfile, 'utf8')).split(/\r?\n/).filter(Boolean)
					const n = Math.max(1, Math.min(400, Number(args.log || 30)))
					return text(all.slice(-n).join('\n') || 'log is empty')
				} catch (error) {
					return text(`no log at ${logfile}: ${error.message}`)
				}
			}
			const show = args?.show !== false
			try {
				await fs.mkdir(dir, { recursive: true })
				await fs.writeFile(path.join(dir, 'show.txt'), show ? '1' : '0', 'utf8')
				return text(`show.txt = ${show ? 1 : 0}`)
			} catch (error) {
				return text(`could not write show.txt: ${error.message}`)
			}
		},
	})

	add({
		name: 'stellaris_launch',
		description:
			'Start Stellaris through the launcher (the only way the DLL plugins load). Takes about a minute; the game window appears on this machine.',
		parameters: {
			type: 'object',
			properties: {
				cont: { type: 'boolean', description: 'continue from the last save' },
			},
			additionalProperties: false,
		},
		output: textOutput,
		execute: async (args) => {
			const flags = args?.cont ? ['launch', '--continue'] : ['launch']
			const r = await run(stl, flags, 180000)
			return text(r.text || (r.ok ? 'launched' : `launch failed (exit ${r.code})`))
		},
	})

	ctx.effect(() => () => {
		const count = disposers.length
		for (const dispose of disposers.splice(0)) {
			try {
				dispose()
			} catch {}
		}
		mark(`disposed ${count}`)
	}, 'dsh-stellaris.tools')

	/* ---------------------------------------------------------------- 自动化提醒
	 * 面板每五秒写一次 alive.txt（epoch 秒）。游戏在，就在最近的会话里建一条
	 * 两分钟的提醒；游戏走了（心跳过期），就把那条删掉。这样 DSH 不空转。
	 */
	const alivePath = path.join(dir, 'alive.txt')
	const autoStatePath = path.join(here, 'auto-schedule.json')
	const autoLogPath = path.join(here, 'auto-schedule.log')
	/* 超过 1 MB 时轮转成 .log.1（覆盖上一份），保留最近一段历史，而不是一把清空 */
	const autoLogMaxBytes = 1024 * 1024
	const logAuto = (line) => {
		try {
			try {
				if (statSync(autoLogPath).size > autoLogMaxBytes) renameSync(autoLogPath, `${autoLogPath}.1`)
			} catch {}
			appendFileSync(autoLogPath, `${new Date().toISOString()} [${INSTANCE_ID}] ${line}\n`, 'utf8')
		} catch {}
	}

	const readAlive = () => {
		try {
			const seconds = Number(readFileSync(alivePath, 'utf8').trim())
			return Number.isFinite(seconds) ? seconds : 0
		} catch {
			return 0
		}
	}

	const readAutoState = () => {
		try { return JSON.parse(readFileSync(autoStatePath, 'utf8')) } catch { return null }
	}
	const writeAutoState = (value) => {
		try { writeFileSync(autoStatePath, JSON.stringify(value ?? {}, null, 2) + '\n', 'utf8') } catch {}
	}

	/* 投递会话：取最近活跃、且不是子代理会话的那一个；选不到就什么也不做 */
	const pickSession = () => {
		let list = []
		try {
			list = ctx.sessions?.list?.() ?? []
		} catch (error) {
			logAuto(`sessions.list failed: ${error.message}`)
			return null
		}
		if (!Array.isArray(list)) {
			logAuto(`sessions.list returned ${typeof list}`)
			return null
		}
		const cwd = String(process.cwd()).toLowerCase()
		const scored = []
		for (const record of list) {
			const header = record?.header ?? record ?? {}
			const id = header.id ?? record?.id
			if (!id) continue
			if (header.parentId ?? header.parent_id) continue
			const archived = header.archived ?? header.archivedAt ?? header.archived_at ?? record?.archived
			/* 会话属于哪个工作区：字段名不确定，能读到哪个用哪个；读不到就当作“本地” */
			const ws =
				header.workspace ?? header.workspacePath ?? header.cwd ?? header.projectPath ?? record?.workspace ?? record?.cwd
			const local = typeof ws === 'string' && ws ? String(ws).toLowerCase() === cwd : true
			const raw =
				header.updatedAt ??
				header.updated ??
				header.updated_at ??
				header.createdAt ??
				header.created ??
				header.created_at ??
				header.modifiedAt ??
				header.lastActiveAt ??
				record?.updatedAt ??
				record?.updated_at ??
				record?.time ??
				0
			const stamp = typeof raw === 'number' ? raw : Date.parse(raw) || 0
			scored.push({ id, stamp, archived: Boolean(archived), local })
		}
		/* 记一行：以后会话选错了，先看这行，别猜 */
		if (list.length) logAuto(`session sample: ${JSON.stringify(list[0].header ?? list[0]).slice(0, 700)}`)
		logAuto(
			`sessions(${scored.length}): ` +
				(scored
					.map(
						(s) =>
							`${s.id.slice(0, 18)}@${s.stamp}${s.archived ? ' archived' : ''}${s.local ? '' : ' foreign'}`,
					)
					.join(' | ') || '(none)'),
		)
		/* 优先级：未归档 > 本工作区；两级都没有才退回全部 */
		const live = scored.filter((s) => !s.archived)
		const liveLocal = live.filter((s) => s.local)
		const pool = liveLocal.length ? liveLocal : live.length ? live : scored
		pool.sort((a, b) => b.stamp - a.stamp)
		return pool.length ? pool[0].id : null
	}

	let migrationChecked = false
	let autoSchedule = readAutoState()
	if (autoSchedule?.id && autoSchedule.sessionId) {
		logAuto(`resuming with ${autoSchedule.id} in ${autoSchedule.sessionId}`)
	} else {
		autoSchedule = null
	}

	const readAutoSwitch = () => {
		try {
			return String(readFileSync(path.join(dir, 'auto.txt'), 'utf8')).trim() !== '0'
		} catch {
			return true
		}
	}

	const syncAuto = async () => {
		const aliveSeconds = readAlive()
		const fresh = aliveSeconds > 0 && Date.now() - aliveSeconds * 1000 < 30000

		/* 面板上的开关也管这条提醒：关着就不建，已有的也撤掉 */
		const enabled = readAutoSwitch()

		/* 启动后只检查一次：已有 schedule 若落在旧会话上，搬到当前该投的会话 */
		if (!migrationChecked) {
			migrationChecked = true
			if (autoSchedule) {
				const target = pickSession()
				if (target && target !== autoSchedule.sessionId) {
					logAuto(`moving ${autoSchedule.id} from ${autoSchedule.sessionId} to ${target}`)
					try {
						await ctx.schedule.delete({ sessionId: autoSchedule.sessionId, id: autoSchedule.id })
					} catch (error) {
						logAuto(`delete old failed: ${error.message}`)
					}
					autoSchedule = null
					writeAutoState(null)
				}
			}
		}

		if (fresh && enabled) {
			if (autoSchedule) return
			const sessionId = pickSession()
			if (!sessionId) {
				logAuto('game is up but there is no session to deliver into yet')
				return
			}
			const record = await ctx.schedule.create(
				sessionId,
				{ title: AUTO_TITLE, prompt: AUTO_PROMPT, every_seconds: AUTO_EVERY_SECONDS },
				undefined,
			)
			const id = record?.id ?? record?.record?.id ?? record?.schedule?.id
			if (!id) {
				logAuto(`schedule.create returned no id: ${JSON.stringify(record).slice(0, 200)}`)
				return
			}
			autoSchedule = { sessionId, id }
			writeAutoState(autoSchedule)
			logAuto(`created ${id} in session ${sessionId}`)
			return
		}

		if (!autoSchedule) return
		/* 走到这里是「不再需要提醒」，两种原因要分开记：心跳断了，还是开关被关掉 */
		const staleWhy = !fresh ? 'the game heartbeat is stale' : 'the auto switch is off'
		try {
			await ctx.schedule.delete({ sessionId: autoSchedule.sessionId, id: autoSchedule.id })
			logAuto(`deleted ${autoSchedule.id}: ${staleWhy}`)
		} catch (error) {
			logAuto(`delete failed: ${error.message}`)
		}
		autoSchedule = null
		writeAutoState(null)
	}

	/* 宿主会把同一个 bundle 应用两次，两次可能落在**不同的模块实例**上（模块级变量不共享），
	   甚至 `here` 也可能因为 link/junction 而不同——所以锁必须放在固定位置，且要能认出自己。
	   token = pid:时间戳:随机数，每 10 秒续写；tick 时发现锁里不是自己的 token 就自己停下。 */
	const myToken = `${process.pid}:${Date.now()}:${Math.random().toString(36).slice(2)}`
	const lockPath = path.join(homedir(), '.dsh', 'dsh-stellaris-auto.lock')
	const readLock = () => {
		try {
			return readFileSync(lockPath, 'utf8').trim()
		} catch {
			return ''
		}
	}
	const acquireLock = () => {
		const text = readLock()
		if (text) {
			const [pidStr, tsStr] = text.split(':')
			const age = Date.now() - Number(tsStr || 0)
			/* 另一个 DSH 进程还活着：让给它。同进程的旧实例（token 不同、pid 相同）则直接接管。 */
			if (pidStr && Number(pidStr) !== process.pid && age < 60000) return false
		}
		try {
			writeFileSync(lockPath, myToken)
			return true
		} catch {
			return false
		}
	}
	const stillOwner = () => readLock() === myToken

	/* 最后一道闸：进程级标志挂在 globalThis 上，跨模块实例也共享。文件锁挡的是「另一个进程」，
	   这道闸挡的是「同一个进程里第二次 apply」——之前正是这种叠加让两个 watcher 同时 tick。 */
	const WATCHER_FLAG = '__dshStellarisAutoWatcher'
	if (globalThis[WATCHER_FLAG]) {
		mark('auto watcher already running in this process; skipping the second one')
	} else if (!acquireLock()) {
		mark('another DSH process holds dsh-stellaris-auto.lock; not starting a watcher')
	} else {
		globalThis[WATCHER_FLAG] = true
		const autoTimer = setInterval(() => {
			/* 被后来者接管了就安静退出，不再喊 no session */
			if (!stillOwner()) {
				clearInterval(autoTimer)
				mark('auto watcher: lock moved to another instance, stopping')
				return
			}
			try {
				writeFileSync(lockPath, myToken)
			} catch {}
			syncAuto().catch((error) => logAuto(`sync failed: ${error.message}`))
		}, 10000)
		setTimeout(() => {
			if (!stillOwner()) return
			syncAuto().catch((error) => logAuto(`first sync failed: ${error.message}`))
		}, 3000)
		ctx.effect(() => () => {
			clearInterval(autoTimer)
			globalThis[WATCHER_FLAG] = false
			mark('auto watcher stopped')
		}, 'dsh-stellaris.auto')
	}

	mark(`apply ok: ${disposers.length} tools, stl=${stl}`)
}



















