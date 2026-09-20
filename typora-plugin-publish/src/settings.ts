import { SettingTab, fs, path } from '@typora-community-plugin/core'
import type PublishPdfPlugin from './main'

export type PublishSettings = {
  /** Pot do izvrsljive datoteke md2pdf. */
  md2pdfPath: string
  /** Mapa s predlogami (header.html, pdf.html). */
  templatesDir: string
}

export const DEFAULT_SETTINGS: PublishSettings = {
  md2pdfPath: ['D:', 'bin', 'md2pdf.exe'].join(SEP()),
  templatesDir: ['D:', 'Webdev', 'htdocs', 'md2pdf', 'templates'].join(SEP()),
}

/** Znak za locevanje map v poti. */
function SEP(): string {
  return String.fromCharCode(92)
}

export class PublishSettingTab extends SettingTab {

  constructor(private plugin: PublishPdfPlugin) {
    super()
  }

  get name() {
    return 'Publish PDF'
  }

  onshow() {
    this.render()
  }

  private async render() {
    this.containerEl.innerHTML = ''

    this.addSettingTitle('Orodje za pretvorbo')

    await this.addPathSetting({
      key: 'md2pdfPath',
      name: 'Pot do md2pdf.exe',
      description: 'Izvrsljiva datoteka, ki markdown pretvori v PDF.',
      mode: 'openfile',
      filter: 'Programi (*.exe)|*.exe|Vse datoteke (*.*)|*.*',
    })

    await this.addPathSetting({
      key: 'templatesDir',
      name: 'Mapa s predlogami',
      description: 'Mapa z header.html (glava ARHIM) in pdf.html (postavitev in CSS). '
        + 'Ce poti ni, orodje izdela nestiliziran dokument brez glave, zato jo vticnik pred pretvorbo preveri.',
      mode: 'folder',
      expect: ['header.html', 'pdf.html'],
    })

    this.addSetting(setting => {
      setting.addName('Ponastavi na privzete poti')
      setting.addButton(btn => {
        btn.textContent = 'Ponastavi'
        btn.onclick = () => {
          this.plugin.settings.set('md2pdfPath', DEFAULT_SETTINGS.md2pdfPath)
          this.plugin.settings.set('templatesDir', DEFAULT_SETTINGS.templatesDir)
          this.render()
        }
      })
    })
  }

  /** Vrstica z besedilnim poljem, gumbom za brskanje in prikazom stanja. */
  private async addPathSetting(opts: {
    key: keyof PublishSettings
    name: string
    description: string
    mode: 'openfile' | 'folder'
    filter?: string
    expect?: string[]
  }) {
    const value = this.plugin.settings.get(opts.key) as string
    const status = await this.check(value, opts.expect)

    this.addSetting(setting => {
      setting.addName(opts.name)
      setting.addBadge(status)
      setting.addDescription(opts.description)

      setting.addText(input => {
        input.value = value
        input.style.minWidth = '22em'
        input.onchange = () => {
          this.plugin.settings.set(opts.key, input.value.trim())
          this.render()
        }
      })

      setting.addButton(btn => {
        btn.textContent = 'Prebrskaj ...'
        btn.onclick = async () => {
          const picked = await this.plugin.pick({
            mode: opts.mode,
            dir: opts.mode === 'folder' ? value : path.dirname(value),
            name: opts.mode === 'folder' ? '' : path.basename(value),
            filter: opts.filter,
          })
          if (picked) {
            this.plugin.settings.set(opts.key, picked)
            this.render()
          }
        }
      })
    })
  }

  /** Preveri, ali pot obstaja in ali vsebuje pricakovane datoteke. */
  private async check(value: string, expect?: string[]): Promise<string> {
    if (!value) return 'ni nastavljeno'
    if (!await fs.exists(value)) return 'ne obstaja'

    for (const f of expect ?? []) {
      if (!await fs.exists(path.join(value, f))) return `manjka ${f}`
    }
    return 'v redu'
  }
}
