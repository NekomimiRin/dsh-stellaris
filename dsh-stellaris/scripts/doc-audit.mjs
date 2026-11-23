// 文档审计：工具 / 桥方法在 README 里的覆盖、表格完整性、旧名字。
//
// plugin.test.mjs 管「每个工具在工具表里有一行」与「正文数字与实测一致」；
// 这个脚本管它管不到的三件事：
//   ① 每个工具在正文里有没有被讲到（只出现在汇总表里 = 空壳）
//   ② Markdown 表格有没有坏（单元格数与表头不一致——增量编辑最常见的手滑）
//   ③ 正文里还有没有已删除的工具名
//
// 用法：
//   node scripts/doc-audit.mjs            覆盖清单 + 表格体检
//   node scripts/doc-audit.mjs --stale    只列不再存在的工具名（有发现退出码 1）
//   node scripts/doc-audit.mjs --bridge   桥 README 没提过的桥方法
//
// 工具名与桥方法的对应：bridgeTool('stellaris_x', 'method', …) 这一层要算进来——
// 文档里常写桥方法名（`get_traditions`）而不写工具名，那不是缺正文。
// 但只靠桥方法名算「提过」会偏宽（`get_status` 这种到处都有），所以两种计数分开列。

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')
const patch = path.join(root, '..', 'stellaris-bridge-patch')

const readme = fs.readFileSync(path.join(root, 'README.md'), 'utf8')
const bridgeReadme = fs.readFileSync(path.join(patch, 'README.md'), 'utf8')
const ipc = fs.readFileSync(path.join(patch, 'source', 'src', 'ipc_server.cpp'), 'utf8')
const code = fs.readFileSync(path.join(root, 'index.js'), 'utf8')

// ---- 代码里的工具：真跑一次 apply()，与 plugin.test 取同一份来源 ----
const registered = []
const { apply } = await import(pathToFileURL(path.join(root, 'index.js')).href)
apply(
	{
		tools: { register: (d) => (registered.push(d), () => {}) },
		sessions: { list: () => [] },
		schedule: { create: async () => ({}), delete: async () => {}, list: async () => [] },
		effect: (fn) => fn(),
	},
	{}
)
const tools = [...new Set(registered.map((d) => d.name))].sort()

// 工具 → 桥方法（bridgeTool 的第一个/第二个字面量）
const toolToMethod = new Map()
for (const m of code.matchAll(/bridgeTool\(\s*'(stellaris_[a-z0-9_]+)'\s*,\s*'([a-z0-9_]+)'/g)) {
	toolToMethod.set(m[1], m[2])
}

// ---- 代码里的桥方法 ----
const methods = [...new Set([...ipc.matchAll(/method\s*==\s*"([a-z0-9_]+)"/g)].map((m) => m[1]))].sort()

const lines = readme.split(/\r?\n/)
// 发布版的文档拆成了 tools.md（逐工具）与 build.md；有它就算正文，别把「只在表里」全报成空壳
const toolsDocPath = path.join(root, 'docs', 'tools.md')
const toolsDocLines = fs.existsSync(toolsDocPath) ? fs.readFileSync(toolsDocPath, 'utf8').split(/\r?\n/) : []
// 「只在汇总表里」的汇总表 = 工具清单那一节；章节里的表（面板、参数口径…）算正文，
// 否则给某个工具做完一节正文、把它写进那一节的表里，反而会被判成空壳。
const listStart = lines.findIndex((l) => /^### 工具清单/.test(l))
const listEnd = listStart < 0 ? -1 : lines.findIndex((l, k) => k > listStart && /^#{2,3} /.test(l))
const inToolList = (n) => listStart >= 0 && n > listStart && (listEnd < 0 || n < listEnd)
const isTableRow = (line, n) => /^\|\s*`stellaris_[a-z_0-9]+`\s*\|/.test(line) && inToolList(n)
const word = (s) => new RegExp(`(?<![A-Za-z0-9_])${s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?![A-Za-z0-9_])`)

// ---- ① 覆盖 ----
const bare = [] // 正文里连桥方法名都没有
const viaMethod = [] // 只有桥方法名，没有工具名
const toolOnly = []
for (const t of tools) {
	let nameHits = 0
	for (let n = 1; n <= lines.length; n++) if (lines[n - 1].includes(t) && !isTableRow(lines[n - 1], n)) nameHits++
	for (const l of toolsDocLines) if (l.includes(t)) nameHits++
	const method = toolToMethod.get(t)
	let methodHits = 0
	if (method) {
		const re = word(method)
		for (const l of lines) if (re.test(l)) methodHits++
	}
	if (nameHits) toolOnly.push(`${t} ×${nameHits}`)
	else if (methodHits) viaMethod.push(`${t} → \`${method}\` ×${methodHits}`)
	else bare.push(t)
}

// ---- ② 表格完整性 ----
const tableProblems = []
let i = 0
while (i < lines.length) {
	if (!/^\s*\|/.test(lines[i])) {
		i++
		continue
	}
	const start = i
	const block = []
	while (i < lines.length && /^\s*\|/.test(lines[i])) block.push([i + 1, lines[i]]), i++
	if (block.length < 2) continue
	const cells = (l) => l.replace(/^\s*\|/, '').replace(/\|\s*$/, '').split(/(?<!\\)\|/).length
	const want = cells(block[0][1])
	for (const [n, l] of block) {
		if (cells(l) !== want) tableProblems.push(`第 ${n} 行：${cells(l)} 格（表头 ${want} 格）｜${l.slice(0, 60)}`)
	}
	void start
}

// ---- ④ 反引号不成对（经 shell 写文档时 `\n` 被吃掉的那种伤）----
const unbalanced = []
{
	let fence = false
	lines.forEach((l, k) => {
		if (/^\s*```/.test(l)) {
			fence = !fence
			return
		}
		if (fence) return
		const ticks = (l.match(/(?<!\\)`/g) || []).length
		if (ticks % 2) unbalanced.push(`第 ${k + 1} 行（${ticks} 个）：${l.slice(0, 70)}`)
	})
}

// ---- ⑤ 标题落在代码块里（= 漏了一个收尾的 ```；会吞掉后面几百行）----
const trapped = []
{
	let fence = false
	let openAt = 0
	lines.forEach((l, k) => {
		if (/^\s*```/.test(l)) {
			if (!fence) openAt = k + 1
			fence = !fence
			return
		}
		if (fence && /^#{1,3} /.test(l)) trapped.push(`第 ${k + 1} 行「${l}」（块开于 ${openAt}）`)
	})
}

// ---- ③ 旧名字 ----
const mentioned = new Set([...readme.matchAll(/stellaris_[a-z_0-9]+/g)].map((m) => m[0]))// 不是工具的合法名字：文件名 / 仓库名 / 版本号
const LEGIT = new Set(['stellaris_bridge', 'stellaris_sdk', 'stellaris_4_5_2_windows_sdk', 'stellaris_4', 'stellaris_native_runtime'])
const stale = [...mentioned].filter((n) => !tools.includes(n) && !LEGIT.has(n)).sort()

const args = process.argv.slice(2)

if (args.includes('--stale')) {
	console.log(`正文提到 ${mentioned.size} 个名字；旧名字 ${stale.length} 个`)
	for (const s of stale) {
		const at = lines.map((l, k) => (l.includes(s) ? k + 1 : 0)).filter(Boolean)
		console.log(`  ${s}  @ ${at.join(', ')}`)
	}
	process.exit(stale.length ? 1 : 0)
}

if (args.includes('--bridge')) {
	const bh = new Map(methods.map((m) => [m, 0]))
	const re = new Map(methods.map((m) => [m, word(m)]))
	for (const l of bridgeReadme.split(/\r?\n/)) for (const m of methods) if (re.get(m).test(l)) bh.set(m, bh.get(m) + 1)
	// 探针 / 内部方法不必写正文
	const internal = /^(probe_|scan_|dump_|hook_|read_\w+_hook|find_|self_test|clone_|cmd_|walk_|call_|list_views|los)/
	const missing = methods.filter((m) => !bh.get(m) && !internal.test(m))
	console.log(`桥方法 ${methods.length} 个；排除探针/内部后，桥 README 完全没提 ${missing.length} 个：`)
	for (const m of missing) console.log('  ' + m)
	process.exit(0)
}

console.log(`工具 ${tools.length} 个｜桥方法 ${methods.length} 个｜工具名下挂了桥方法 ${toolToMethod.size} 个`)
console.log(`\n[表格] 单元格数不一致的行：${tableProblems.length}`)
for (const p of tableProblems) console.log('  ' + p)

console.log(`\n[旧名字] ${stale.length} 个${stale.length ? '：' + stale.join(', ') : ''}`)

console.log(`\n[反引号] 不成对的行：${unbalanced.length}`)
for (const u of unbalanced) console.log('  ' + u)

console.log(`\n[围栏] 标题落在代码块里：${trapped.length}`)
for (const t of trapped) console.log('  ' + t)

console.log(`\n[空壳] 正文里连桥方法名都没出现（只有汇总表）：${bare.length}`)
for (const t of bare) console.log('  ' + t)

console.log(`\n[只靠桥方法名提到] ${viaMethod.length} 个（不算空壳，但写文档时留意口径）`)
for (const t of viaMethod) console.log('  ' + t)

// apply() 注册了随心跳开合的定时器，不清掉进程不会自己退出
process.exit(0)
