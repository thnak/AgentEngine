# devsim/verify-consumer.ps1
#
# Compiles and links an arbitrary consumer .cpp against the already-built engine in build-dev,
# using the exact flag/library set CMake gives the repo's own examples (extracted from
# build-dev/build.ninja). This is the empirical test of "can a developer build this?".
#
# Attempt A uses the repo's own gate: /W4 /WX (warnings are errors).
# If A fails, Attempt B drops /WX, which distinguishes "does not compile" from
# "compiles but does not meet the repo's warning gate" -- two different doc failures.
#
# Usage: & .\devsim\verify-consumer.ps1 -Source devsim\p1\main.cpp -Label p1

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Label = "consumer"
)

$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $repo "build-dev"
$out = Join-Path $PSScriptRoot "out"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not (Test-Path -LiteralPath $Source)) { Write-Host "MISSING SOURCE: $Source"; exit 2 }
$src = (Resolve-Path -LiteralPath $Source).Path

$obj = Join-Path $out "$Label.obj"
$exe = Join-Path $out "$Label.exe"

$defs = '-DNOMINMAX -DWIN32_LEAN_AND_MEAN -I"' + (Join-Path $repo 'include') + '" /DWIN32 /D_WINDOWS /EHsc /DNDEBUG'
$std = '-std:c++latest -MD /Od'

# Link set copied from build.ninja's example_01 link statement (engine libs + winsock + system).
$libs = 'agentengine_pal_cpu.lib agentengine_chat_recording.lib agentengine_message_json.lib ' +
        'agentengine_rt_session.lib agentengine_rt_file_log.lib agentengine_tool_pipeline.lib ' +
        'agentengine_reactor_asio.lib ws2_32.lib mswsock.lib agentengine_rt_workflow.lib ' +
        'agentengine_docker_surface.lib kernel32.lib user32.lib gdi32.lib winspool.lib ' +
        'shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib'

function Invoke-Attempt {
    param([string]$Tag, [string]$WarnFlags)

    $bat = Join-Path $out "build-$Label-$Tag.bat"
    $log = Join-Path $out "build-$Label-$Tag.log"

    # Double-quoted here-string so PowerShell substitutes the paths; no `+` array pitfalls.
    $content = @"
@echo off
call "$vcvars" >nul 2>&1
cl.exe /nologo /TP $defs $std $WarnFlags /Fo"$obj" /c "$src"
if errorlevel 1 exit /b 10
cl.exe /nologo "$obj" /Fe:"$exe" /link /machine:x64 /INCREMENTAL:NO /subsystem:console /LIBPATH:"$build" $libs
exit /b %errorlevel%
"@
    Set-Content -LiteralPath $bat -Value $content -Encoding ASCII

    $captured = & cmd.exe /c "`"$bat`"" 2>&1
    $code = $LASTEXITCODE
    $captured | Out-File -LiteralPath $log -Encoding utf8
    return $code
}

Write-Host "=== $Label : $Source"
$a = Invoke-Attempt -Tag 'strict' -WarnFlags '/W4 /WX'
if ($a -eq 0) {
    Write-Host "VERDICT: PASS -- compiles and links under the repo's own /W4 /WX gate"
    Write-Host "binary: $exe"
    exit 0
}

$b = Invoke-Attempt -Tag 'nowarn' -WarnFlags '/W4'
if ($b -eq 0) {
    Write-Host "VERDICT: PASS(with warnings) -- compiles and links only once /WX is dropped"
    Write-Host "--- strict-mode diagnostics ---"
    Get-Content -LiteralPath (Join-Path $out "build-$Label-strict.log") -ErrorAction SilentlyContinue | Select-Object -First 40
    exit 0
}

Write-Host "VERDICT: FAIL -- does not compile and link"
Write-Host "--- diagnostics (first 60 lines) ---"
Get-Content -LiteralPath (Join-Path $out "build-$Label-nowarn.log") -ErrorAction SilentlyContinue | Select-Object -First 60
exit 1
