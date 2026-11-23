// 生成桥端 README 的目录：把「## 」标题按文档顺序编号列出，插在「## 目录」与下一条「## 」之间。
// 用法：
//   node scripts/toc-bridge.mjs            重写目录
//   node scripts/toc-bridge.mjs --check    只检查（非 0 = 过期）
// 桥端文档改动频率低，所以不做深度分组；分组的人工「阅读路径」写在目录之上。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const file = path.resolve(here, '..', '..', 'stellaris-bridge-patch', 'README.md')
const raw = fs.readFileSync(file, 'utf8')
const eol = raw.includes('\r\n') ? '\r\n' : '\n'
const lines = raw.split(/\r?\n/)

const start = lines.findIndex((l) => l.trim() === '## 目录（按文档顺序）')
if (start < 0) {
	console.error('找不到「## 目录（按文档顺序）」')
	process.exit(2)
}
const end = lines.findIndex((l, i) => i > start && /^## /.test(l))
if (end < 0) {
	console.error('目录之后找不到下一条 ## 标题')
	process.exit(2)
}

// 目录之后的所有 ## 标题
const heads = []
for (let i = end; i < lines.length; i++) if (/^## /.test(lines[i])) heads.push(lines[i].slice(3).trim())

const built = ['## 目录（按文档顺序）', '']
heads.forEach((h, k) => built.push(`${k + 1}. ${h}`))
built.push('', '> 改过标题就重跑 `node scripts/toc-bridge.mjs`；`--check` 只检查不改。', '')

if (process.argv.includes('--check')) {
	const cur = lines.slice(start, end).join('\n').trim()
	if (cur === built.join('\n').trim()) {
		console.log(`桥端目录是最新的（${heads.length} 节）`)
		process.exit(0)
	}
	console.error(`桥端目录已过期：现有 ${cur.split('\n').length} 行，应为 ${built.length} 行`)
	process.exit(1)
}

const out = [...lines.slice(0, start), ...built, ...lines.slice(end)]
fs.writeFileSync(file, out.join(eol), 'utf8')
console.log(`桥端目录已重写：${heads.length} 节`)
