// 不经 DSH 重启，直接调一个插件工具，看它的真实返回。
//
// 插件用 apply() 注册工具，这里给一个假的 ctx 收下来，再挑一个执行。
// 好处：新接的工具不必重启 DSH 就能对着正在跑的游戏验证（管道调用是插件自己的代码）。
//
// 用法：node scripts/call.mjs <工具名> ['{"planet_id":14}']
//       node scripts/call.mjs <工具名> @args.json     （参数先写进文件，省掉命令行转义）
//       node scripts/call.mjs --list

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')

const registered = new Map()
const { apply } = await import(pathToFileURL(path.join(root, 'index.js')).href)
apply(
	{
		tools: {
			register: (d) => {
				registered.set(d.name, d)
				return () => {}
			},
		},
		sessions: { list: () => [] },
		schedule: { create: async () => ({}), delete: async () => {}, list: async () => [] },
		effect: (fn) => fn(),
	},
	{}
)

const [tool, json] = process.argv.slice(2)

if (!tool || tool === '--list') {
	for (const name of [...registered.keys()].sort()) console.log(name)
	process.exit(0)
}

const definition = registered.get(tool)
if (!definition) {
	console.error(`没有这个工具：${tool}（--list 看全部）`)
	process.exit(1)
}

let args = {}
if (json) {
	try {
		// @file：命令行里塞 JSON 时引号会被外壳吃掉，写进文件再读最稳
		args = JSON.parse(json.startsWith('@') ? fs.readFileSync(json.slice(1), 'utf8') : json)
	} catch (error) {
		console.error(`参数不是 JSON：${error.message}`)
		process.exit(1)
	}
}

const started = Date.now()
const value = await definition.execute(args, {})
const rendered = definition.output.render(args, value)
console.log(rendered.map((part) => part.text).join('\n'))
console.log(`\n[${tool} ${Date.now() - started}ms]`)
// apply() 里有随心跳开合的定时器（自动检查），不退出就会变成僵尸进程写 auto-schedule.log
process.exit(0)
