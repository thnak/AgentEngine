#Requires -Version 5.1
<#
.SYNOPSIS
  Runs the `live-network` ctest label -- the end-to-end tests that talk to REAL inference servers.

.DESCRIPTION
  tests/test_openrouter_live_e2e.cpp and tests/test_llamacpp_live_e2e.cpp are excluded from a
  default `ctest` run by self-skipping when their configuration is absent (see each file's own top
  comment for why: a live model is nondeterministic, and I5 keeps nondeterminism out of the
  deterministic suite). This script supplies that configuration and selects exactly those tests.

  The API key is read from a local, git-ignored file and passed through the environment -- it is
  never written to a build file, a CMake cache, or the ctest command line (018 §4).

  The key file is line-oriented, matching the layout this repo's own scratch file uses:
    1  the API key
    2  the OpenAI-compatible base URL      (e.g. https://openrouter.ai/api/v1)
    3  the Anthropic-compatible base URL   (e.g. https://openrouter.ai/api)
    4  a local llama.cpp endpoint URL      (e.g. http://localhost:8080/v1/chat/completions)
  Lines 2-4 are optional; only the host is taken from lines 2/3 (both backends share it) and only
  the port from line 4. A missing line simply means that backend's tests skip.

.PARAMETER BuildDir
  A build tree configured with -DAGENTENGINE_WITH_HTTPS=ON. Both tests need it: the OpenAI-
  compatible translation plane lives behind that guard.

.PARAMETER KeyFile
  Path to the line-oriented file described above. Defaults to apt-openrouter.txt in the repo root.

.PARAMETER Model
  OpenRouter model id. Defaults to the test's own default when omitted.

.PARAMETER Config
  Build configuration, for multi-config generators (Visual Studio). Ignored by single-config ones.

.PARAMETER SkipBuild
  Run ctest against whatever is already built instead of building first.

.EXAMPLE
  ./tools/run-live-provider-tests.ps1

.EXAMPLE
  ./tools/run-live-provider-tests.ps1 -BuildDir build-https -Model 'deepseek/deepseek-v4-pro'
#>
[CmdletBinding()]
param(
  [string] $BuildDir = 'build-https',
  [string] $KeyFile,
  [string] $Model,
  [string] $Config = 'Debug',
  [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $KeyFile) { $KeyFile = Join-Path $repoRoot 'apt-openrouter.txt' }

if (-not (Test-Path $BuildDir)) {
  $BuildDir = Join-Path $repoRoot $BuildDir
}
if (-not (Test-Path $BuildDir)) {
  throw "Build directory '$BuildDir' not found. Configure one first: cmake -S . -B build-https -DAGENTENGINE_WITH_HTTPS=ON"
}

# ---- OpenRouter configuration -------------------------------------------------------------------
if (Test-Path $KeyFile) {
  $lines = @(Get-Content -Path $KeyFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })

  if ($lines.Count -ge 1) {
    $env:AGENTENGINE_OPENROUTER_API_KEY = $lines[0]
    Write-Host "OpenRouter key   : loaded from $KeyFile (length $($lines[0].Length))"
  }
  # Both OpenRouter surfaces share a host; the tests append the path prefix themselves.
  if ($lines.Count -ge 2) {
    try {
      $env:AGENTENGINE_OPENROUTER_HOST = ([Uri]$lines[1]).Host
      Write-Host "OpenRouter host  : $($env:AGENTENGINE_OPENROUTER_HOST)"
    } catch {
      Write-Warning "Line 2 of $KeyFile is not a URL; falling back to the test's default host."
    }
  }
  # Line 4: a local llama.cpp endpoint. Only its host/port are used -- the test owns the path.
  if ($lines.Count -ge 4) {
    try {
      $u = [Uri]$lines[3]
      $resolved = if ($u.Host -eq 'localhost') { '127.0.0.1' } else { $u.Host }
      $env:AGENTENGINE_LLAMACPP_HOST = $resolved
      $env:AGENTENGINE_LLAMACPP_PORT = "$($u.Port)"
      Write-Host "llama.cpp        : $resolved`:$($u.Port)"
    } catch {
      Write-Warning "Line 4 of $KeyFile is not a URL; the llama.cpp test will skip."
    }
  }
} else {
  Write-Warning "Key file '$KeyFile' not found -- every live test will SKIP (exit 0), not fail."
}

if ($Model) {
  $env:AGENTENGINE_OPENROUTER_MODEL = $Model
  Write-Host "OpenRouter model : $Model"
}

# ---- Build --------------------------------------------------------------------------------------
# -j4 max, per CLAUDE.md's machine-safety rule: this dev box can hang if a build saturates its cores.
if (-not $SkipBuild) {
  Write-Host "`nBuilding the live tests (-j4)..."
  & cmake --build $BuildDir --config $Config --target test_openrouter_live_e2e test_llamacpp_live_e2e -j 4
  if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

# ---- Run ----------------------------------------------------------------------------------------
# -C is required by multi-config generators (Visual Studio) and harmless to single-config ones.
Write-Host "`nRunning the live-network label...`n"
& ctest --test-dir $BuildDir -C $Config -L live-network --output-on-failure -V
$code = $LASTEXITCODE

# Do not leave a live credential sitting in this shell's environment after the run.
Remove-Item Env:AGENTENGINE_OPENROUTER_API_KEY -ErrorAction SilentlyContinue

exit $code
