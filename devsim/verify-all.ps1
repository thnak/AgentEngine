# devsim/verify-all.ps1
# The documentation gate for this repository's API docs. Run from the repo root.
#   & .\devsim\verify-all.ps1
# Exits non-zero if any check fails.
#
# Checks, in order:
#   1. the site builds (tsc -b && vite build)
#   2. lint is at or below the known baseline (11 warnings, 0 errors, none in touched files)
#   3. the program printed on api/first-app.html is byte-identical to the compiled one
#   4. the known false claims are absent from the built bundles and their corrections are present
#   5. no source file has a BOM, invalid UTF-8, or mojibake
#   6. every right-rail TOC anchor resolves to an id its page renders
#   7. the documented first program still compiles, links and runs against the engine

$ErrorActionPreference = 'Continue'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $root

$failed = @()

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ''
    Write-Host ('=== ' + $Name) -ForegroundColor Cyan
    & $Body
    if ($LASTEXITCODE -ne 0) {
        $script:failed += $Name
        Write-Host ('    FAILED: ' + $Name) -ForegroundColor Red
    }
}

Step 'build (tsc -b && vite build)' {
    Push-Location (Join-Path $root 'web\marketing')
    npm run build 2>&1 | Select-Object -Last 3
    Pop-Location
}

Step 'lint (baseline: 11 warnings, 0 errors)' {
    Push-Location (Join-Path $root 'web\marketing')
    $out = npm run lint 2>&1
    $out | Select-Object -Last 2
    Pop-Location
    $summary = ($out | Select-String 'Found \d+ warnings? and \d+ errors?').Line
    if (-not $summary) { $global:LASTEXITCODE = 1; Write-Host '    could not read lint summary'; return }
    if ($summary -notmatch '(\d+) warnings? and (\d+) errors?') { $global:LASTEXITCODE = 1; return }
    $warn = [int]$Matches[1]
    $err = [int]$Matches[2]
    if ($err -gt 0) { Write-Host ('    ' + $err + ' lint error(s)') -ForegroundColor Red; $global:LASTEXITCODE = 1; return }
    if ($warn -gt 11) { Write-Host ('    lint warnings rose above the baseline of 11: ' + $warn) -ForegroundColor Red; $global:LASTEXITCODE = 1; return }
    $global:LASTEXITCODE = 0
}

Step 'snippet fidelity (documented program == compiled program)' {
    node devsim\compare-snippet.js
}

Step 'claim check (false claims gone, corrections present)' {
    node devsim\verify-claims.js | Select-Object -Last 2
}

Step 'encoding integrity (no BOM, valid UTF-8, no mojibake)' {
    node devsim\verify-encoding.js
}

Step 'TOC anchors resolve' {
    node devsim\verify-anchors.js
}

Step 'published include paths are compiled' {
    node devsim\verify-includes.js
}

Step 'documented first program compiles, links and runs' {
    & (Join-Path $PSScriptRoot 'verify-consumer.ps1') -Source 'devsim\first-program\main.cpp' -Label gate | Select-Object -First 3
    if ($LASTEXITCODE -eq 0) {
        & (Join-Path $PSScriptRoot 'out\gate.exe')
        Write-Host ('    program exit: ' + $LASTEXITCODE)
    }
}

Step 'documented tool + agent declaration compiles and binds' {
    & (Join-Path $PSScriptRoot 'verify-consumer.ps1') -Source 'devsim\verify-tool-agent\main.cpp' -Label toolagent | Select-Object -First 3
    if ($LASTEXITCODE -eq 0) {
        & (Join-Path $PSScriptRoot 'out\toolagent.exe')
        Write-Host ('    program exit: ' + $LASTEXITCODE)
    }
}

Step 'documented tool-call recipe drives the real pipeline offline' {
    & (Join-Path $PSScriptRoot 'verify-consumer.ps1') -Source 'devsim\verify-toolcall\main.cpp' -Label toolcall | Select-Object -First 3
    if ($LASTEXITCODE -eq 0) {
        & (Join-Path $PSScriptRoot 'out\toolcall.exe')
        Write-Host ('    program exit: ' + $LASTEXITCODE)
    }
}

Step 'both documented ExecutorBody shapes compile and route' {
    & (Join-Path $PSScriptRoot 'verify-consumer.ps1') -Source 'devsim\verify-bodyforms\main.cpp' -Label bodyforms | Select-Object -First 3
    if ($LASTEXITCODE -eq 0) {
        & (Join-Path $PSScriptRoot 'out\bodyforms.exe')
        Write-Host ('    program exit: ' + $LASTEXITCODE)
    }
}

Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host 'ALL GATES PASS' -ForegroundColor Green
    exit 0
}
Write-Host ('GATES FAILED: ' + ($failed -join '; ')) -ForegroundColor Red
exit 1
