import * as fs from 'node:fs/promises'
import * as esbuild from 'esbuild'
import typoraPlugin from 'esbuild-plugin-typora'

const IS_PROD = process.argv.slice(2).includes('--prod')

await fs.rm('./dist', { recursive: true, force: true })

await esbuild.build({
  entryPoints: ['src/main.ts'],
  outdir: 'dist',
  format: 'esm',
  bundle: true,
  minify: IS_PROD,
  loader: { '.ps1': 'text' },
  sourcemap: !IS_PROD,
  plugins: [
    typoraPlugin({ mode: IS_PROD ? 'production' : 'development' }),
  ],
})

await fs.copyFile('./src/manifest.json', './dist/manifest.json')

console.log(`Zgrajeno v ./dist (${IS_PROD ? 'production' : 'development'})`)
