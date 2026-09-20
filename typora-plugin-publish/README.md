# Typora Plugin: Publish PDF

Vtičnik za [typora-community-plugin](https://github.com/typora-community-plugin/typora-community-plugin).
Odprti markdown izvozi v PDF z orodjem `md2pdf.exe`.

## Kaj naredi

- **Ukaz** `Objavi v PDF` v ukazni plošči (<kbd>F1</kbd>), s privzeto bližnjico
  <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd>.
- **Gumb v statusni vrstici**, viden, kadar je odprta markdown datoteka.
- Vtičnik nima privzetega cilja — **vedno** odpre izvorno okno **»Shrani kot«**,
  prednastavljeno na PDF ob izvorni datoteki. Ob preklicu se pretvorba ne izvede.
- Med izvajanjem prikaže obvestilo, ob koncu uspeh ali napako.

## Zaledje

```
md2pdf.exe <md> --output=<izbrana pot> --header=<tpl>/header.html --body=<tpl>/pdf.html --no-footer
```

Predlogi se podata **z absolutno potjo**. Privzete poti v orodju so relativne
(`templates/header.html`), zato bi bil izpis odvisen od delovne mape — brez
predlog nastane nestiliziran dokument brez glave. Vtičnik zato obstoj obeh
datotek pred pretvorbo preveri in ob manjkajoči pretvorbo ustavi, namesto da bi
tiho oddal napačen izpis.

Noga je izklopljena z `--no-footer`, ker privzeta predloga na vsako stran
natisne davčne in bančne podatke.

## Nastavitve

V *Settings → Publish PDF*:

| Nastavitev | Privzeto |
|---|---|
| Pot do `md2pdf.exe` | `D:\bin\md2pdf.exe` |
| Mapa s predlogami | `D:\Webdev\htdocs\md2pdf\templates` |

Obe je mogoče vpisati ročno ali izbrati z gumbom **Prebrskaj …**, ki odpre
izvorno Windows okno (izbirnik datoteke oziroma mape). Ob vsaki vrstici je
značka s stanjem — `v redu`, `ne obstaja`, `manjka header.html` — tako da je
napačna pot vidna takoj in ne šele ob prvi pretvorbi. Gumb **Ponastavi** vrne
privzete vrednosti.

Nastavitve se shranjujejo prek `PluginSettings` v konfiguracijo ogrodja.

## Razvoj

```sh
npm install
npm run build          # produkcijska različica v ./dist
npm run build:dev      # razvojna različica s source map
npx tsc --noEmit       # preverjanje tipov
```

## Namestitev

Najprej mora biti nameščeno ogrodje **typora-community-plugin** (installer ga
razpakira v `%USERPROFILE%\.typora\community-plugins` in popravi Typorine
datoteke). Nato:

```sh
npm run build
node install.js
```

Skripta prekopira **samo `main.js` in `manifest.json`** iz `dist/` v globalno
mapo vtičnikov:

```
%USERPROFILE%\.typora\community-plugins\plugins\arhim.publish-pdf\
```

Typoro zaprite in znova odprite, nato vtičnik vklopite v nastavitvah.

> **Pogosta napaka:** v mapo vtičnika ne kopirajte celotnega projekta.
> Upravitelj vtičnikov išče `manifest.json` v **korenu** mape vtičnika; če je
> skrit v podmapi `dist`, vtičnika ne bo na seznamu nameščenih.

## Pogovorna okna

Okna so izvorna Windows okna (`SaveFileDialog`, `OpenFileDialog`,
`FolderBrowserDialog`), prikazana prek PowerShella (`src/pick-dialog.ps1`, ob
gradnji vgrajen v `main.js`; način izbere spremenljivka `PUBLISH_MODE`).
Electronovega `dialog` ni mogoče uporabiti, ker Typora ne vključuje
`@electron/remote`. Poti se prenašajo prek spremenljivk okolja in berejo kot
UTF-8, da šumniki v imenih map ostanejo nepoškodovani.

## Struktura

| Datoteka | Vloga |
|---|---|
| `src/main.ts` | implementacija vtičnika |
| `src/settings.ts` | zavihek z nastavitvami |
| `src/pick-dialog.ps1` | pogovorna okna za izbiro poti |
| `src/manifest.json` | metapodatki (id, različica, združljivost) |
| `build.js` | gradnja z esbuild |
| `install.js` | namestitev v globalno mapo vtičnikov |
