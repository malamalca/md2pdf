# Prikaze izvorno Windows pogovorno okno in izpise izbrano pot.
# Podatki se prenesejo prek spremenljivk okolja, da se izognemo
# tezavam s kodno stranjo pri parametrih ukazne vrstice.
#
#   PUBLISH_MODE    save | openfile | folder   (privzeto save)
#   PUBLISH_DIR     zacetna mapa
#   PUBLISH_NAME    predlagano ime datoteke    (save, openfile)
#   PUBLISH_FILTER  filter datotek             (save, openfile)
#
# Ob preklicu ne izpise nicesar.

Add-Type -AssemblyName System.Windows.Forms
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Nevidno lastnisko okno poskrbi, da se pogovorno okno prikaze v ospredju.
$owner = New-Object System.Windows.Forms.Form
$owner.TopMost = $true
$owner.ShowInTaskbar = $false
$owner.Opacity = 0
$owner.Show()

$mode = if ($env:PUBLISH_MODE) { $env:PUBLISH_MODE } else { 'save' }
$result = $null

switch ($mode) {

    'folder' {
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Izberite mapo s predlogami'
        $dlg.ShowNewFolderButton = $false
        if ($env:PUBLISH_DIR) { $dlg.SelectedPath = $env:PUBLISH_DIR }
        if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            $result = $dlg.SelectedPath
        }
    }

    'openfile' {
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = 'Izberite datoteko'
        $dlg.CheckFileExists = $true
        if ($env:PUBLISH_FILTER) { $dlg.Filter = $env:PUBLISH_FILTER }
        if ($env:PUBLISH_DIR)    { $dlg.InitialDirectory = $env:PUBLISH_DIR }
        if ($env:PUBLISH_NAME)   { $dlg.FileName = $env:PUBLISH_NAME }
        if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            $result = $dlg.FileName
        }
    }

    default {
        $dlg = New-Object System.Windows.Forms.SaveFileDialog
        $dlg.Title = 'Objavi v PDF'
        $dlg.Filter = if ($env:PUBLISH_FILTER) { $env:PUBLISH_FILTER } else { 'PDF (*.pdf)|*.pdf|Vse datoteke (*.*)|*.*' }
        $dlg.DefaultExt = 'pdf'
        $dlg.AddExtension = $true
        $dlg.OverwritePrompt = $true
        if ($env:PUBLISH_DIR)  { $dlg.InitialDirectory = $env:PUBLISH_DIR }
        if ($env:PUBLISH_NAME) { $dlg.FileName = $env:PUBLISH_NAME }
        if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            $result = $dlg.FileName
        }
    }
}

if ($result) { [Console]::Out.Write($result) }

$owner.Close()
$owner.Dispose()
