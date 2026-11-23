// 打印 index.js 的分段地图：每条「/* —— … —— */」注释一格，带行号与这一段注册的工具数。
// 拆分 13000 行单文件之前的准备工作——先看清每一段是什么、有多少工具、哪里是「0 工具」的纯逻辑。
//
// 用法：node scripts/code-map.mjs [--tools]
//   --tools 连该段注册的工具名一起列
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const file = path.resolve(here, '..', 'index.js')
const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
const withTools = process.argv.includes('--tools')

// 分段注释：/* ---- 标题 ---- */（注释可能多行，标题总在第一行）
const marks = []
lines.forEach((l, i) => {
	const m = /^\s*\/\*+\s*[—-]{2,}\s*(.+?)\s*(?:[—-]{2,})?\s*(?:\*\/)?\s*$/.exec(l)
	if (m) marks.push({ line: i + 1, title: m[1] })
})

const toolAt = (from, to) => {
	const set = new Set()
	for (let i = from - 1; i < to - 1; i++) {
		for (const m of lines[i].matchAll(/(?:name:\s*|bridgeTool\(\s*)'(stellaris_[a-z_0-9]+)'/g)) set.add(m[1])
		const solo = /^\s*'(stellaris_[a-z_0-9]+)'\s*,?\s*$/.exec(lines[i])
		if (solo) set.add(solo[1])
	}
	return [...set]
}

let total = 0
for (let k = 0; k < marks.length; k++) {
	const to = k + 1 < marks.length ? marks[k + 1].line : lines.length
	const tools = toolAt(marks[k].line, to)
	total += tools.length
	console.log(`${String(marks[k].line).padStart(6)}  ${String(tools.length).padStart(3)} 工具  ${marks[k].title.slice(0, 70)}`)
	if (withTools && tools.length) console.log('             ' + tools.join(' '))
}

const all = new Set(toolAt(1, lines.length))
console.log(`\n共 ${marks.length} 段；段内合计 ${total} 条注册（全文件去重 ${all.size} 个）；文件 ${lines.length} 行`)
