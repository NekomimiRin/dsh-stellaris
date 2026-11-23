// 从 README 的标题重新生成目录，避免目录与正文互相拆台。
//
// 用法：
//   node scripts/toc.mjs            只列 # 与 ## 与 ###（默认，目录短）
//   node scripts/toc.mjs --deep     连 #### 也列（目录长，但不会漏小节）
//   node scripts/toc.mjs --check    只检查目录是否过期，不改文件（非 0 退出 = 过期）
//
// --check 会先看现有目录里有没有四级项，据此判断它当初是 shallow 还是 --deep 生成的，
// 再用同一种模式比对。否则跑过一次 --deep 之后 --check 就永远绿不了了。
//
// 生成的目录插在「## 目录」与之后第一条 `# ` / `## 标题` 之间。

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')
const file = path.join(root, 'README.md')

const deep = process.argv.includes('--deep')
const check = process.argv.includes('--check')

const raw = fs.readFileSync(file, 'utf8')
// 保留原文件的行尾风格，并把行尾统一：外部编辑器（CRLF）与本脚本（LF）混用会让
// --check 反复报「目录已过期」，其实内容一个字没变。统一之后就不会再抖了。
const eol = raw.includes('\r\n') ? '\r\n' : '\n'
const lines = raw.split(/\r?\n/)

const start = lines.findIndex((l) => l.trim() === '## 目录')
if (start < 0) {
	console.error('找不到「## 目录」')
	process.exit(2)
}
const end = lines.findIndex((l, i) => i > start && (l.startsWith('# ') || l.startsWith('## ')))
if (end < 0) {
	console.error('找不到目录之后的第一条一级标题')
	process.exit(2)
}

// ---- 统计正文标题（一次，供提示文字与 --check 消息用）----
const counts = { h2: 0, h3: 0, h4: 0 }
for (let i = end; i < lines.length; i++) {
	const line = lines[i]
	if (line.startsWith('## ')) {
		if (line.slice(3).trim() !== '目录') counts.h2++
	} else if (line.startsWith('### ')) {
		counts.h3++
	} else if (line.startsWith('#### ')) {
		counts.h4++
	}
}

// ---- 按模式生成目录（shallow / deep 各一份，check 时挑对应的）----
const build = (withDeep) => {
	const built = ['## 目录', '']
	for (let i = end; i < lines.length; i++) {
		const line = lines[i]
		if (line.startsWith('# 第') || (line.startsWith('# ') && !line.startsWith('## '))) {
			if (built[built.length - 1] !== '') built.push('')
			built.push('**' + line.replace(/^#\s*/, '').trim() + '**')
			built.push('')
			continue
		}
		if (line.startsWith('## ')) {
			const name = line.slice(3).trim()
			if (name === '目录') continue
			built.push(`- **${name}**`)
			continue
		}
		if (line.startsWith('### ')) {
			built.push(`  - ${line.slice(4).trim()}`)
			continue
		}
		if (line.startsWith('#### ') && withDeep) {
			built.push(`    - ${line.slice(5).trim()}`)
		}
	}
	// shallow 模式在目录开头写明为什么 #### 不列
	if (!withDeep) {
		built.splice(2, 0, `> 目录列到三级标题；正文另有 ${counts.h4} 个 \`####\` 小节，见各节正文。`, '')
	}
	built.push('', '---', '')
	return built
}

if (check) {
	const current = lines.slice(start, end).join('\n').trim()
	// 现有目录里有四级项（4 空格 + "- "）说明它是 --deep 生成的
	const currentIsDeep = /^ {4}- /m.test(current)
	const wanted = build(currentIsDeep).join('\n').trim()
	const mode = currentIsDeep ? '含 ####' : '不含 ####'
	if (current === wanted) {
		console.log(`目录是最新的（${mode}；## ${counts.h2} / ### ${counts.h3} / #### ${counts.h4}）`)
		process.exit(0)
	}
	console.error(
		`目录已过期（当前 ${mode}）：## ${counts.h2} / ### ${counts.h3} / #### ${counts.h4}，` +
			`跑 node scripts/toc.mjs${currentIsDeep ? ' --deep' : ''}`,
	)
	process.exit(1)
}

const out = [...lines.slice(0, start), ...build(deep), ...lines.slice(end)]
fs.writeFileSync(file, out.join(eol), 'utf8')
console.log(`目录已重写：## ${counts.h2} / ### ${counts.h3} / #### ${counts.h4}${deep ? '（含 ####）' : '（不含 ####，已在目录顶部注明）'}`)
