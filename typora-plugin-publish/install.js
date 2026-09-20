/**
 * Namesti zgrajeni vticnik med globalne vticnike ogrodja
 * typora-community-plugin:
 *
 *   %USERPROFILE%\.typora\community-plugins\plugins\<id>\
 *
 * V mapo vticnika sodita samo main.js in manifest.json iz ./dist,
 * NE celoten projekt - upravitelj vticnikov isce manifest.json v korenu.
 *
 *   node install.js              # globalna namestitev
 *   node install.js "<pot>"      # namestitev v poljubno mapo vticnikov
 */
import * as fs from 'node:fs/promises'
import * as os from 'node:os'
import * as path from 'node:path'

const manifest = JSON.parse(await fs.readFile('./src/manifest.json', 'utf8'))

const pluginsDir = process.argv[2]
  ?? path.join(os.homedir(), '.typora', 'community-plugins', 'plugins')

const dest = path.join(pluginsDir, manifest.id)

try {
  await fs.access('./dist/manifest.json')
}
catch {
  console.error('V ./dist ni manifest.json - najprej pozenite: npm run build')
  process.exit(1)
}

await fs.rm(dest, { recursive: true, force: true })
await fs.mkdir(dest, { recursive: true })

for (const f of await fs.readdir('./dist')) {
  if (f.endsWith('.map')) continue
  await fs.copyFile(path.join('./dist', f), path.join(dest, f))
}

console.log(`Vticnik ${manifest.id} v${manifest.version} namescen v:`)
console.log(`  ${dest}`)
console.log((await fs.readdir(dest)).map(f => '  - ' + f).join('\n'))
console.log('\nTyporo zaprite in znova odprite, nato vticnik vklopite v nastavitvah.')
