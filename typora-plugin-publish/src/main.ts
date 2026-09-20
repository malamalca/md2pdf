import { Plugin, PluginSettings, Notice, fs, path } from '@typora-community-plugin/core'
import { reqnode } from 'typora'
import { DEFAULT_SETTINGS, PublishSettingTab, type PublishSettings } from './settings'
// Skripta pogovornih oken se ob gradnji vgradi kot besedilo.
import pickDialogScript from './pick-dialog.ps1'

/** Najdaljsi dopusten cas pretvorbe (ms). */
const TIMEOUT = 5 * 60 * 1000

/** Najdaljsi cas cakanja na odgovor v pogovornem oknu (ms). */
const DIALOG_TIMEOUT = 10 * 60 * 1000

export type PickOptions = {
  mode: 'save' | 'openfile' | 'folder'
  dir?: string
  name?: string
  filter?: string
}

export default class PublishPdfPlugin extends Plugin<PublishSettings> {

  private statusBarEl: HTMLElement | null = null
  private running = false

  onload() {
    const settings = new PluginSettings<PublishSettings>(this.app, this.manifest, { version: 1 })
    settings.setDefault(DEFAULT_SETTINGS)
    this.registerSettings(settings)
    this.registerSettingTab(new PublishSettingTab(this))

    this.registerCommand({
      id: 'publish',
      title: 'Objavi v PDF',
      scope: 'global',
      hotkey: 'Ctrl+Alt+P',
      callback: () => this.publish(),
    })

    this.statusBarEl = this.addStatusBarItem({
      position: 'right',
      type: 'item',
      hint: 'Objavi odprto datoteko v PDF (Ctrl+Alt+P)',
    })
    this.statusBarEl.textContent = 'Objavi v PDF'
    this.statusBarEl.style.cursor = 'pointer'
    this.statusBarEl.style.display = 'none'
    this.registerDomEvent(this.statusBarEl, 'click', () => this.publish())

    this.register(this.app.workspace.on('file:open', () => this.refresh()))
    this.refresh()
  }

  onunload() {
    this.statusBarEl = null
  }

  /** Predloga glave (logotip in naslov ARHIM). */
  private get headerTemplate(): string {
    return path.join(this.settings.get('templatesDir'), 'header.html')
  }

  /** Predloga strani (postavitev in CSS). */
  private get bodyTemplate(): string {
    return path.join(this.settings.get('templatesDir'), 'pdf.html')
  }

  /** Gumb je viden samo, kadar je odprta markdown datoteka. */
  private async refresh() {
    if (this.statusBarEl) {
      this.statusBarEl.style.display = this.isMarkdown() ? '' : 'none'
    }
  }

  private isMarkdown(): boolean {
    const file = this.app.workspace.activeFile
    return !!file && /\.(md|markdown)$/i.test(file)
  }

  /** Vrne prvo manjkajoco datoteko orodja ali null, ce so vse na mestu. */
  private async findMissing(): Promise<string | null> {
    const files = [this.settings.get('md2pdfPath'), this.headerTemplate, this.bodyTemplate]
    for (const f of files) {
      if (!f || !await fs.exists(f)) return f || '(pot ni nastavljena)'
    }
    return null
  }

  private async publish() {
    if (this.running) {
      Notice.warning('Objava ze poteka.')
      return
    }

    const file = this.app.workspace.activeFile
    if (!this.isMarkdown()) {
      Notice.warning('Odprta ni nobena markdown datoteka.')
      return
    }

    // Brez predlog bi orodje tiho izdelalo nestiliziran PDF brez glave,
    // zato je bolje pretvorbo ustaviti kot oddati napacen izpis.
    const missing = await this.findMissing()
    if (missing) {
      Notice.error(`Manjka: ${missing}. Preverite nastavitve vticnika.`, 0)
      return
    }

    this.running = true
    try {
      // Vticnik nima privzetega cilja - vedno vprasa uporabnika.
      // Predlaga PDF ob izvorni datoteki.
      const suggested = path.join(
        path.dirname(file),
        path.basename(file, path.extname(file)) + '.pdf')

      const target = await this.pick({
        mode: 'save',
        dir: path.dirname(suggested),
        name: path.basename(suggested),
      })
      if (!target) {
        Notice.info('Objava preklicana.')
        return
      }

      const notice = new Notice(`Objavljam ${path.basename(target)} ...`, 0)
      notice.show()
      try {
        await this.convert(file, target)
        notice.close()
        Notice.success(`Shranjeno: ${path.basename(target)}`)
      }
      catch (e) {
        notice.close()
        throw e
      }
    }
    catch (e: any) {
      Notice.error(`Objava ni uspela: ${e?.message ?? e}`, 0)
      console.error('[publish-pdf]', e)
    }
    finally {
      this.running = false
    }
  }

  /** Pretvori markdown v PDF na podano pot. */
  private convert(source: string, target: string): Promise<string> {
    return this.spawn(
      this.settings.get('md2pdfPath'),
      [
        source,
        `--output=${target}`,
        `--header=${this.headerTemplate}`,
        `--body=${this.bodyTemplate}`,
        '--no-footer',
      ],
      { timeout: TIMEOUT })
  }

  /**
   * Prikaze izvorno Windows pogovorno okno za izbiro poti.
   * Vrne izbrano pot ali null, ce je uporabnik preklical.
   */
  async pick(options: PickOptions): Promise<string | null> {
    const scriptPath = path.join(
      reqnode('os').tmpdir(), 'typora-publish-pick-dialog.ps1')
    await fs.writeText(scriptPath, pickDialogScript)

    const out = await this.spawn(
      'powershell.exe',
      ['-STA', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', scriptPath],
      {
        timeout: DIALOG_TIMEOUT,
        env: {
          ...process.env,
          PUBLISH_MODE: options.mode,
          PUBLISH_DIR: options.dir ?? '',
          PUBLISH_NAME: options.name ?? '',
          PUBLISH_FILTER: options.filter ?? '',
        },
      })

    const chosen = out.trim()
    return chosen === '' ? null : chosen
  }

  /** Ovojnica okoli execFile, ki vrne stdout kot UTF-8 besedilo. */
  private spawn(cmd: string, args: string[], options: Record<string, any>): Promise<string> {
    const { execFile } = reqnode('child_process')

    return new Promise((resolve, reject) => {
      execFile(
        cmd, args,
        { windowsHide: true, encoding: 'buffer', ...options },
        (err: any, stdout: Buffer, stderr: Buffer) => {
          const out = this.clean(stdout)
          if (err) {
            reject(new Error(this.clean(stderr).split('\n').pop() || out.split('\n').pop() || err.message))
          }
          else {
            resolve(out)
          }
        })
    })
  }

  /** Odstrani ANSI barvne kode in obrobne presledke. */
  private clean(buf?: Buffer): string {
    return (buf?.toString('utf8') ?? '')
      .replace(/\x1b\[[0-9;]*m/g, '')
      .trim()
  }
}
