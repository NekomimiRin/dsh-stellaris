/**
 * 把 dsh-stellaris 接进一个 DSH profile。
 *
 * 四件事（照 dsh-dialog-alpha 的本地安装路径）：
 *   ① 备份 profile 的 package.json / pnpm-lock.yaml / cordis.patch.yml；
 *   ② 写 dependencies["dsh-stellaris"] = "link:<插件目录>"；
 *   ③ 把包名追加进 dsh.profile.bundles（让它成为一个 bundle 层，
 *      而不是往 profile 的 cordis.patch.yml 里插一行——两条路都挂会产出
 *      两条同 id 的 entry）；
 *   ④ 在 profile 的 node_modules 里建目录链接（junction，不需要管理员）。
 *
 * 用法：
 *   node scripts/install.mjs [--profile desktop] [--home <DSH_HOME>]
 *   node scripts/install.mjs --status [--profile desktop]
 *   node scripts/install.mjs --remove
 *
 * 装完必须**完全退出 DeepSeek Harness 再重新打开**：插件表是宿主启动时的
 * 快照，刷新页面补不上。
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const pluginDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const manifest = JSON.parse(fs.readFileSync(path.join(pluginDir, 'package.json'), 'utf8'))
const packageName = manifest.name

/**
 * 解析参数。
 * @param {string[]} argv 参数表。
 * @returns {{ profile: string, home: string, status: boolean, remove: boolean }} 解析结果。
 */
function parseArgs(argv) {
  const args = { profile: 'desktop', home: '', status: false, remove: false }
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index]
    if (token === '--profile') args.profile = argv[(index += 1)] ?? 'desktop'
    else if (token === '--home') args.home = argv[(index += 1)] ?? ''
    else if (token === '--status') args.status = true
    else if (token === '--remove') args.remove = true
  }
  return args
}

const args = parseArgs(process.argv.slice(2))
const home =
  args.home !== ''
    ? path.resolve(args.home)
    : process.env.DSH_HOME && process.env.DSH_HOME !== ''
      ? path.resolve(process.env.DSH_HOME)
      : path.join(os.homedir(), '.dsh')
const profileDir = path.join(home, 'profiles', args.profile)
const manifestPath = path.join(profileDir, 'package.json')
const linkPath = path.join(profileDir, 'node_modules', packageName)

if (!fs.existsSync(profileDir)) {
  console.error(`profile 目录不存在：${profileDir}`)
  process.exit(2)
}

const profile = JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
const declared = profile.dependencies?.[packageName] ?? null
const bundles = Array.isArray(profile.dsh?.profile?.bundles) ? profile.dsh.profile.bundles : []
const linkKind = fs.existsSync(linkPath)
  ? fs.lstatSync(linkPath).isSymbolicLink()
    ? 'link'
    : 'directory'
  : 'missing'

console.log(`package:  ${packageName} @ ${pluginDir}`)
console.log(`profile:  ${profileDir}`)
console.log(`declared: ${declared ?? '(none)'}`)
console.log(`bundled:  ${bundles.includes(packageName)}`)
console.log(`link:     ${linkKind}${linkKind === 'link' ? ` -> ${fs.realpathSync(linkPath)}` : ''}`)

if (args.status) process.exit(0)

if (args.remove) {
  const backupDir = path.join(
    profileDir,
    `.${packageName}-backup-${new Date().toISOString().replace(/[:.]/gu, '-')}`,
  )
  fs.mkdirSync(backupDir, { recursive: true })
  for (const file of ['package.json', 'pnpm-lock.yaml', 'cordis.patch.yml']) {
    const source = path.join(profileDir, file)
    if (fs.existsSync(source)) fs.copyFileSync(source, path.join(backupDir, file))
  }
  delete profile.dependencies?.[packageName]
  if (Array.isArray(profile.dsh?.profile?.bundles)) {
    profile.dsh.profile.bundles = profile.dsh.profile.bundles.filter((name) => name !== packageName)
  }
  fs.writeFileSync(manifestPath, `${JSON.stringify(profile, null, 2)}\n`, 'utf8')
  if (linkKind === 'link') fs.unlinkSync(linkPath)
  console.log(`removed; backup: ${backupDir}`)
  console.log('下一步：完全退出 DeepSeek Harness 再重新打开。')
  process.exit(0)
}

if (linkKind === 'directory') {
  console.error(
    `拒绝覆盖：${linkPath} 是真实目录（大概是 pnpm 装的正式副本）。\n` +
      '请先让 DSH 自己卸载它，或确认后手工删除。',
  )
  process.exit(3)
}

const backupDir = path.join(
  profileDir,
  `.${packageName}-backup-${new Date().toISOString().replace(/[:.]/gu, '-')}`,
)
fs.mkdirSync(backupDir, { recursive: true })
for (const file of ['package.json', 'pnpm-lock.yaml', 'cordis.patch.yml']) {
  const source = path.join(profileDir, file)
  if (fs.existsSync(source)) fs.copyFileSync(source, path.join(backupDir, file))
}
console.log(`backup:   ${backupDir}`)

profile.dependencies = profile.dependencies ?? {}
profile.dependencies[packageName] = `link:${pluginDir.replace(/\\/gu, '/')}`
profile.dsh = profile.dsh ?? {}
profile.dsh.profile = profile.dsh.profile ?? {}
const nextBundles = bundles.includes(packageName) ? bundles : [...bundles, packageName]
profile.dsh.profile.bundles = nextBundles
fs.writeFileSync(manifestPath, `${JSON.stringify(profile, null, 2)}\n`, 'utf8')
console.log(`updated:  ${manifestPath}`)

if (linkKind !== 'link') {
  fs.mkdirSync(path.dirname(linkPath), { recursive: true })
  fs.symlinkSync(pluginDir, linkPath, 'junction')
  console.log(`junction: ${linkPath}`)
}

console.log('')
console.log('下一步：完全退出 DeepSeek Harness 再重新打开（刷新页面不够）。')
