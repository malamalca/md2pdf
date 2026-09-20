# Runs every tests\*.md through md2pdf.exe and checks the result.
# Usage:  powershell -ExecutionPolicy Bypass -File cpp\tests\run-tests.ps1
#
# Must run with the repo root as the working directory (or it will cd there
# itself) because md2pdf looks for templates\ next to the exe, then in the cwd.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # ...\md2pdf
$exe  = Join-Path $root 'cpp\build\md2pdf.exe'
$md   = Join-Path $root 'cpp\tests'
$out  = Join-Path $md   'out'

if (-not (Test-Path $exe)) { throw "md2pdf.exe not found at $exe - run cpp\build.bat first" }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root

# name -> expected page count (0 = don't care), and whether it must succeed
$expect = @{
    '01-basic'             = @{ pages = 1;  ok = $true  }
    '02-gfm'               = @{ pages = 2;  ok = $true  }
    '03-unicode-pagebreak' = @{ pages = 3;  ok = $true  }
    '04-edge-cases'        = @{ pages = 0;  ok = $true  }
    '05-images'            = @{ pages = 0;  ok = $true  }
    '06-stress'            = @{ pages = 0;  ok = $true  }
    '07-hang'              = @{ pages = 0;  ok = $false }  # runaway script: must fail, not hang
    '08-anchors'           = @{ pages = 2;  ok = $true; links = 7 }  # in-document links
}

$chromeBefore = (Get-Process chrome -ErrorAction SilentlyContinue | Measure-Object).Count
$fail = 0

foreach ($name in ($expect.Keys | Sort-Object)) {
    $src = Join-Path $md "$name.md"
    if (-not (Test-Path $src)) { Write-Host "SKIP $name (no .md)" -ForegroundColor DarkGray; continue }

    $pdf  = Join-Path $out "$name.pdf"
    Remove-Item $pdf -ErrorAction SilentlyContinue

    # 07-hang deliberately wedges the renderer; cap its wait so the suite stays quick.
    # [string[]] matters: PowerShell unrolls a 1-element array to a scalar, and
    # splatting a scalar string passes it one character at a time.
    [string[]]$extra = if ($name -eq '07-hang') { @('--timeout=6') } else { @() }

    $sw  = [Diagnostics.Stopwatch]::StartNew()
    $log = & $exe $src "--output=$pdf" @extra 2>&1 | Out-String
    $sw.Stop()
    $code = $LASTEXITCODE

    $want   = $expect[$name]
    $status = 'PASS'; $note = ''

    if ($want.ok -and $code -ne 0) {
        $status = 'FAIL'; $note = "exit=$code"
    } elseif (-not $want.ok -and $code -eq 0) {
        $status = 'FAIL'; $note = 'expected failure but succeeded'
    } elseif ($want.ok) {
        if (-not (Test-Path $pdf)) {
            $status = 'FAIL'; $note = 'no PDF written'
        } else {
            $raw   = [IO.File]::ReadAllBytes($pdf)
            # 28591 = ISO-8859-1; ::Latin1 only exists on PowerShell 7+
            $text  = [Text.Encoding]::GetEncoding(28591).GetString($raw)
            $pages = ([regex]::Matches($text, '/Type\s*/Page[^s]')).Count
            $links = ([regex]::Matches($text, '/Subtype\s*/Link')).Count
            if ($text.Substring(0, 5) -ne '%PDF-') { $status = 'FAIL'; $note = 'bad PDF header' }
            elseif ($want.pages -gt 0 -and $pages -ne $want.pages) {
                $status = 'FAIL'; $note = "pages=$pages expected $($want.pages)"
            } elseif ($want.links -and $links -ne $want.links) {
                # anchors gone = every in-document link silently dropped by Chrome
                $status = 'FAIL'; $note = "links=$links expected $($want.links)"
            } else {
                $note = "$pages page(s), $($raw.Length) bytes"
                if ($want.links) { $note += ", $links internal link(s)" }
            }
        }
    } else {
        $note = 'failed as expected'
    }

    # anything over ~60s means we are back to waiting on a reply that never comes
    if ($sw.Elapsed.TotalSeconds -gt 60) { $status = 'FAIL'; $note += ' (TOO SLOW - possible hang)' }

    if ($status -eq 'FAIL') { $fail++ }
    $colour = if ($status -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host ("{0} {1,-22} {2,5:N2}s  {3}" -f $status, $name, $sw.Elapsed.TotalSeconds, $note) -ForegroundColor $colour
    if ($status -eq 'FAIL') { Write-Host ($log.Trim() -replace '(?m)^', '       ') -ForegroundColor DarkRed }
}

# --- template location flags -------------------------------------------------
# --header/--body/--footer take a file path; --header/--footer still accept
# literal markup, and a path that does not exist must be an error, not a fallback.
$custom = Join-Path $md 'custom'
if (Test-Path $custom) {
    $dump = Join-Path $out 'tpl-check.html'
    Remove-Item $dump -ErrorAction SilentlyContinue
    & $exe (Join-Path $md '01-basic.md') "--html-out=$dump" `
           "--body=$(Join-Path $custom 'my-body.html')" 2>&1 | Out-Null
    $bodyOk = (Test-Path $dump) -and ((Get-Content $dump -Raw) -match 'CUSTOM BODY TEMPLATE')
    if ($bodyOk) { Write-Host "PASS --body= uses the given template" -ForegroundColor Green }
    else { Write-Host "FAIL --body= did not apply the given template" -ForegroundColor Red; $fail++ }

    & $exe (Join-Path $md '01-basic.md') "--html-out=$dump" "--body=.\no-such-template.html" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "PASS missing template is an error" -ForegroundColor Green }
    else { Write-Host "FAIL missing template did not fail" -ForegroundColor Red; $fail++ }

    Remove-Item $dump -ErrorAction SilentlyContinue
}

Pop-Location

# No Chrome may survive a run, and nothing may be left in %TEMP%. Tearing down a
# wedged renderer via the job object is not instant, so poll rather than sleep once.
$sw = [Diagnostics.Stopwatch]::StartNew()
do {
    Start-Sleep -Milliseconds 250
    $chromeAfter = (Get-Process chrome -ErrorAction SilentlyContinue | Measure-Object).Count
} while ($chromeAfter -gt $chromeBefore -and $sw.Elapsed.TotalSeconds -lt 30)
$sw.Stop()

if ($chromeAfter -gt $chromeBefore) {
    Write-Host "FAIL orphaned chrome processes after 30s: $chromeBefore -> $chromeAfter" -ForegroundColor Red
    $fail++
} else {
    Write-Host ("PASS no orphaned chrome processes (settled in {0:N1}s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

$leftover = @(Get-ChildItem $env:TEMP -Filter 'md2pdf-cdp-*' -Force -ErrorAction SilentlyContinue)
if ($leftover.Count -gt 0) {
    Write-Host "WARN $($leftover.Count) md2pdf-cdp-* file(s) still in TEMP (swept after 1h)" -ForegroundColor Yellow
}

Write-Host ""
if ($fail -eq 0) { Write-Host "All checks passed." -ForegroundColor Green; exit 0 }
Write-Host "$fail check(s) failed." -ForegroundColor Red
exit 1
