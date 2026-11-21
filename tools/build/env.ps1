$ErrorActionPreference = 'Stop'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }

$vsRoot = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                     -property installationPath -format value 2>$null | Select-Object -First 1
if (-not $vsRoot) {
    $vsRoot = & $vswhere -products * -property installationPath -format value 2>$null | Select-Object -First 1
}
if (-not $vsRoot) { throw "vswhere found no VS instance" }

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "VS-bundled cmake.exe not found: $cmake" }

$probeBat = Join-Path $env:TEMP 'dsh_vcvars_env.bat'
@(
    '@echo off'
    "call `"$vcvars`" >nul 2>&1"
    'set'
) | Set-Content -Path $probeBat -Encoding ASCII

$envDump = & cmd /c "`"$probeBat`""
if (-not $envDump) { throw "vcvars64.bat produced no environment (temp bat: $probeBat)" }

$pathFromVcvars = $null
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $Matches[1]
        $value = $Matches[2]
        if ($name -ieq 'Path') { $pathFromVcvars = $value; continue }
        if ($name -notmatch '^(COMSPEC|PATHEXT|PROMPT)$') {

            Set-Item -LiteralPath "Env:$name" -Value $value -ErrorAction SilentlyContinue
        }
    }
}
if ($pathFromVcvars) { $env:PATH = $pathFromVcvars }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $head = if ($pathFromVcvars) { $pathFromVcvars.Substring(0, [Math]::Min(200, $pathFromVcvars.Length)) } else { '<none>' }
    throw "cl.exe still not on PATH after vcvars64. vcvars Path began with: $head"
}

$cmakeDir = Split-Path $cmake -Parent
if ($env:PATH -notlike "*$cmakeDir*") { $env:PATH = "$cmakeDir;$env:PATH" }

$script:VsRoot   = $vsRoot
$script:CmakeExe = $cmake

$clVer  = (& cmd /c "cl 2>&1" | Select-String -Pattern 'Version' | Select-Object -First 1)
$cmVer  = (& cmd /c "`"$cmake`" --version" | Select-Object -First 1)
Write-Host "[env] VS BuildTools : $vsRoot"
Write-Host "[env] $clVer"
Write-Host "[env] $cmVer"
