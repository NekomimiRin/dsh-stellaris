// 插件自检：文档与 index.js 必须互相一致，且工具结构符合 ToolRuntime 要求。
//
// 背景：同一份信息有两个载体——README（给人 / 给下一个会话看）与 index.js 的 description
// （给运行时模型看）。改了一边就以为改了，于是出现「README 标了 ⚠、描述里没有」
// 「README 删了工具、描述里还在引用」这类漂移。这里把这些检查变成机器可查。
//
// 原 tests/plugin.test.mjs 的回归断言（工具结构、disposer 计数、战力换算）已并入本文件，
// 那个文件是 11-15 的旧副本（断言 10 个旧工具名），已删除。
//
// 用法：node scripts/plugin.test.mjs        （非 0 退出即失败）

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')
const code = fs.readFileSync(path.join(root, 'index.js'), 'utf8')
const readme = fs.readFileSync(path.join(root, 'README.md'), 'utf8')

// ---- 真实注册的工具 ----
const registered = []
let disposed = 0
const cleanups = []

const { apply, withDisplayPower } = await import(pathToFileURL(path.join(root, 'index.js')).href)
apply(
	{
		tools: {
			register: (d) => {
				registered.push(d)
				return () => {
					disposed += 1
				}
			},
		},
		sessions: { list: () => [] },
		schedule: { create: async () => ({}), delete: async () => {}, list: async () => [] },
		effect: (fn) => cleanups.push(fn()),
	},
	{}
)
const tools = [...new Set(registered.map((d) => d.name))].sort()

// 重名（2026-11-23 加）：注册两个同名工具时，宿主会让后一个顶掉前一个，谁都不报错。
// 这里比「注册次数」与「去重后的个数」，不相等就是重名。
const dupNames = registered.length === tools.length ? [] : registered.map((d) => d.name).filter((n, i, a) => a.indexOf(n) !== i)


// ---- README 工具表 ----
const tableNames = new Set()
for (const line of readme.split('\n')) {
	const m = line.match(/^\|\s*`(stellaris_[a-z_0-9]+)`\s*\|/)
	if (m) tableNames.add(m[1])
}

// ---- README 里标 ⚠ ----
const readmeWarn = new Set()
for (const line of readme.split('\n')) {
	if (!line.trim().startsWith('|') || !line.includes('⚠')) continue
	for (const m of line.matchAll(/`(stellaris_[a-z_0-9]+)`/g)) readmeWarn.add(m[1])
}

// ---- index.js 的 UNVERIFIED 集合 ----
const warnBlock = code.match(/UNVERIFIED\s*=\s*new Set\(\[([\s\S]*?)\]\)/)
const codeWarn = new Set()
if (warnBlock) for (const m of warnBlock[1].matchAll(/'(stellaris_[a-z_0-9]+)'/g)) codeWarn.add(m[1])

const failures = []
const ok = (m) => console.log('  ok   ' + m)
const bad = (m) => (failures.push(m), console.log('  FAIL ' + m))

console.log(`工具 ${tools.length} 个 | README 表 ${tableNames.size} | README ⚠ ${readmeWarn.size} | 描述 ⚠ ${codeWarn.size}\n`)

console.log('[1] 工具名对齐 README 工具表')
const missingInTable = tools.filter((t) => !tableNames.has(t))
const extraInTable = [...tableNames].filter((t) => !tools.includes(t))
missingInTable.length ? bad('README 表缺: ' + missingInTable.join(', ')) : ok('表格不缺项')
extraInTable.length ? bad('README 表多出: ' + extraInTable.join(', ')) : ok('表格无多余项')

console.log('\n[1b] 工具名不重复')
dupNames.length ? bad('重名（后者会顶掉前者）: ' + [...new Set(dupNames)].join(', ')) : ok(`注册 ${registered.length} 次，${tools.length} 个不同名字`)

console.log('\n[2] ⚠ 集合一致')
const onlyReadme = [...readmeWarn].filter((t) => !codeWarn.has(t))
const onlyCode = [...codeWarn].filter((t) => !readmeWarn.has(t))
onlyReadme.length ? bad('README 标了但描述没标: ' + onlyReadme.join(', ')) : ok('没有只在 README 里的 ⚠')
onlyCode.length ? bad('描述标了但 README 没标: ' + onlyCode.join(', ')) : ok('没有只在描述里的 ⚠')

console.log('\n[3] README 里不得出现不存在的工具名')
// 2026-11-23 收尾时把这条从「三个硬编码名字」改成通用规则：凡是 stellaris_ 开头的名字，
// 要么是注册过的工具，要么在下面这份「不是工具」的白名单里（仓库路径 / 上游目录名）。
// 以前靠手工点名，删掉 colony_jobs / check_researched_techs 之后正文里留了两个月没人发现。
const NON_TOOL_NAMES = new Set([
	'stellaris_bridge', // 上游仓库 stellaris-mcp 的源码目录名
	'stellaris_sdk',
	'stellaris_4_5_2_windows_sdk',
	'stellaris_4',
	'stellaris_native_runtime', // 上游 services/stellaris_native_runtime/ 路径
])
const mentionedNames = new Set([...readme.matchAll(/stellaris_[a-z_0-9]+/g)].map((m) => m[0]))
const staleNames = [...mentionedNames].filter((n) => !tools.includes(n) && !NON_TOOL_NAMES.has(n))
staleNames.length
	? bad('正文里出现不存在的工具名: ' + staleNames.join(', '))
	: ok(`正文提到 ${mentionedNames.size} 个名字，全部是已注册工具或白名单`)

console.log('\n[4] AUTO_PROMPT 只引用存在的工具')
const at = code.indexOf('AUTO_PROMPT')
const promptBlock = at < 0 ? '' : code.slice(at, code.indexOf('\n]', at) + 1)
const refs = [...new Set([...promptBlock.matchAll(/stellaris_[a-z_0-9]+/g)].map((m) => m[0]))]
const unknownRefs = refs.filter((r) => !tools.includes(r))
unknownRefs.length ? bad('AUTO_PROMPT 引用了不存在的工具: ' + unknownRefs.join(', ')) : ok(`引用 ${refs.length} 个工具名，全部存在`)

console.log('\n[5] 每个工具的结构符合 ToolRuntime')
const shapeErrors = []
for (const d of registered) {
	if (typeof d.description !== 'string' || d.description.length <= 10) shapeErrors.push(`${d.name}: description`)
	if (!d.parameters || d.parameters.type !== 'object') shapeErrors.push(`${d.name}: parameters`)
	if (typeof d.execute !== 'function') shapeErrors.push(`${d.name}: execute`)
	// ToolRuntime 要求 output: { schema, render }，缺了这条整条 entry 都不会激活
	if (!d.output || typeof d.output.schema !== 'object') shapeErrors.push(`${d.name}: output.schema`)
	if (typeof d.output?.render !== 'function') shapeErrors.push(`${d.name}: output.render`)
}
shapeErrors.length ? bad('结构不符: ' + shapeErrors.join(', ')) : ok(`${registered.length} 个工具全部合规`)

console.log('\n[6] 卸载时撤掉每一个注册')
for (const c of cleanups) if (typeof c === 'function') c()
disposed === registered.length ? ok(`disposed ${disposed} / ${registered.length}`) : bad(`disposed ${disposed} / ${registered.length}`)

console.log('\n[7] 战力换算（桥值 ≈ 面板值 / 3.05）')
const converted = withDisplayPower({ fleets: [{ military_power: 93.47072 }], total_military_power: 35.69664 })
const kept = converted.fleets[0].military_power === 93.47072
const near = Math.abs(converted.fleets[0].estimated_display_power - 285.09) < 0.1
if (kept && near) {
	ok(`原值保留 ${converted.fleets[0].military_power}，估计 ${converted.fleets[0].estimated_display_power.toFixed(2)}`)
} else {
	bad(`换算不对: ${JSON.stringify(converted.fleets[0])}`)
}

console.log('\n[8] 正文里的数字与实测一致')
// 表格有 [1][2] 管着，散文里的「67 个工具」「174 个方法」没人管——2026-11-18 就是这么漂的：
// 表已经 67 行，开头还写 59，工具现状还写 58，桥方法还写 164。数字一律现算，不手抄。
let bridgeMethods = new Set()
let bridgeReadme = ''
try {
	const patch = path.join(root, '..', 'stellaris-bridge-patch')
	bridgeReadme = fs.readFileSync(path.join(patch, 'README.md'), 'utf8')
	const bridgeSrc = fs.readFileSync(path.join(patch, 'source', 'src', 'ipc_server.cpp'), 'utf8')
	bridgeMethods = new Set([...bridgeSrc.matchAll(/method\s*==\s*"([a-z0-9_]+)"/g)].map((m) => m[1]))
} catch {
	bad('读不到桥源码或桥 README')
}

// 插件显式引用的桥方法：rpc/pipeCall 的第二个参数 + bridgeTool/buildAction 的第二个参数。
// 2026-11-22 修正旧口径：旧版只扫「含 rpc( 的行」+ 单行 bridgeTool，于是
// ① pipeCall 的调用整批漏掉（select_research / get_research_state / build_district …）
// ② 跨行的 buildAction(...) 漏掉（它把工具名与方法名各占一行）
// ③ 同一行里的描述字面量被误当调用（queue_starbase_upgrade）
// 现在按「取第一个实参的括号配对、再取顶层逗号后的字面量」算，跨行也对。
const secondArgLiteral = (text, openParenIdx) => {
	let i = openParenIdx + 1
	let depth = 1
	let quote = null
	while (i < text.length && depth > 0) {
		const ch = text[i]
		if (quote) {
			if (ch === '\\') i += 1
			else if (ch === quote) quote = null
		} else if (ch === "'" || ch === '"' || ch === '`') quote = ch
		else if (ch === '(' || ch === '[' || ch === '{') depth += 1
		else if (ch === ')' || ch === ']' || ch === '}') depth -= 1
		i += 1
	}
	const body = text.slice(openParenIdx + 1, i - 1)
	let d = 0
	let q = null
	let cut = -1
	for (let k = 0; k < body.length; k += 1) {
		const ch = body[k]
		if (q) {
			if (ch === '\\') k += 1
			else if (ch === q) q = null
			continue
		}
		if (ch === "'" || ch === '"' || ch === '`') q = ch
		else if (ch === '(' || ch === '[' || ch === '{') d += 1
		else if (ch === ')' || ch === ']' || ch === '}') d -= 1
		else if (ch === ',' && d === 0) {
			cut = k
			break
		}
	}
	if (cut < 0) return null
	const lit = /^\s*'([a-z0-9_]+)'/.exec(body.slice(cut + 1))
	return lit ? lit[1] : null
}

const usedMethods = new Set()
const unknownMethods = new Set()
for (const m of code.matchAll(/\b(?:rpc|pipeCall)\s*\(/g)) {
	const name = secondArgLiteral(code, m.index + m[0].length - 1)
	if (!name) continue // stellaris_state 的 rpc(pipe, method, …) 是动态转发，算不进来
	if (bridgeMethods.has(name)) usedMethods.add(name)
	else unknownMethods.add(name)
}
for (const m of code.matchAll(/\b(?:bridgeTool|buildAction)\s*\(\s*'stellaris_[a-z0-9_]+'\s*,\s*'([a-z0-9_]+)'/g)) {
	if (bridgeMethods.has(m[1])) usedMethods.add(m[1])
	else unknownMethods.add(m[1])
}
if (unknownMethods.size) {
	bad(`有调用指向不存在的桥方法：${[...unknownMethods].sort().join(', ')}`)
}

const claims = [
	['开头「N 个工具」', readme, /小插件：\*\*(\d+) 个工具\*\*/g, tools.length],
	['工具清单标题', readme, /工具清单（(\d+) 个/g, tools.length],
	['目录顶部「N 个 ####」', readme, /正文另有 (\d+) 个 `####` 小节/g, (readme.match(/^#### /gm) || []).length],
	['工具现状的桥方法数', readme, /桥方法\s+(\d+) 个/g, bridgeMethods.size],
	['待接线的桥方法数', readme, /桥 \*\*(\d+) 个方法\*\*/g, bridgeMethods.size],
	['插件用到的桥方法数', readme, /插件用了(?:约 )?\s*(\d+) 个/g, usedMethods.size],
	['桥 README 的 IPC 方法数', bridgeReadme, /DLL 有 \*\*(\d+) 个 IPC 方法\*\*/g, bridgeMethods.size],
]
for (const [label, src, re, want] of claims) {
	const found = [...src.matchAll(re)].map((m) => Number(m[1]))
	if (!found.length) {
		bad(`${label}: 正文里找不到这一句（改了句子就同步改这个断言）`)
	} else if (found.some((n) => n !== want)) {
		bad(`${label}: 正文写着 ${[...new Set(found)].join('/')}，实际 ${want}`)
	} else {
		ok(`${label}: ${want}`)
	}
}

console.log('')
if (failures.length) {
	console.log(`失败 ${failures.length} 条`)
	process.exit(1)
}
console.log('全部通过')
// apply() 会注册随心跳开合的定时器，不清掉进程不会自己退出
process.exit(0)
