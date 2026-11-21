[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Debug', 'Release', 'MinSizeRel')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean,
    [switch]$Reconfigure,

    [string]$BootstrapName = ''
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    try { & $Exe @Arguments } finally { $ErrorActionPreference = $previous }
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $repo 'out\build'

Write-Host "[build] repo      : $repo"
Write-Host "[build] build dir : $buildDir"
Write-Host "[build] config    : $Config"

Invoke-Expression (Get-Content -Raw (Join-Path $PSScriptRoot 'env.ps1'))

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "[build] removing $buildDir"
    Remove-Item $buildDir -Recurse -Force
    $Reconfigure = $true
}

$cacheFile = Join-Path $buildDir 'CMakeCache.txt'

$cachedName = ''
if (Test-Path $cacheFile) {
    $hit = Select-String -Path $cacheFile -Pattern '^DSH_BOOTSTRAP_NAME:STRING=' | Select-Object -First 1
    if ($hit) { $cachedName = ($hit.Line -split '=', 2)[1].Trim() }
}
if ($BootstrapName -and $cachedName -ne $BootstrapName) {
    Write-Host "[build] the cache says '$cachedName' and '$BootstrapName' was asked for -- reconfiguring"
    $Reconfigure = $true
}

if ($Reconfigure -or -not (Test-Path $cacheFile)) {
    Write-Host "[build] configuring ..."

    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $cmakeArgs = @(
        '-S', $repo,
        '-B', $buildDir,
        '-G', 'Visual Studio 18 2026',
        '-A', 'x64',
        "-DCMAKE_CONFIGURATION_TYPES=$Config"
    )
    if ($BootstrapName) {
        Write-Host "[build] bootstrap  : $BootstrapName"
        $cmakeArgs += "-DDSH_BOOTSTRAP_NAME=$BootstrapName"
    }
    Invoke-Native -Exe 'cmake' -Arguments $cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
} else {
    Write-Host "[build] reusing existing configure (pass -Reconfigure to redo)"
}

Write-Host "[build] building ..."
Invoke-Native -Exe 'cmake' -Arguments @('--build', $buildDir, '--config', $Config, '--parallel')
if ($LASTEXITCODE -ne 0) {

    Write-Warning "[build] the parallel build failed without reporting an error -- retrying one node at a time"
    Invoke-Native -Exe 'cmake' -Arguments @('--build', $buildDir, '--config', $Config, '--parallel', '1')
}
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$bin = Join-Path $repo 'out\bin'

if (-not (Test-Path $bin)) {
    Write-Warning "artifact directory missing: $bin"
    exit 1
}

$expected = 'dollsnest-coop'
if (Test-Path $cacheFile) {
    $hit = Select-String -Path $cacheFile -Pattern '^DSH_BOOTSTRAP_NAME:' | Select-Object -First 1
    if ($hit) { $expected = ($hit.Line -split '=', 2)[1].Trim() }
}

if ($BootstrapName -and $expected -ne $BootstrapName) {
    Write-Warning ("'{0}' was asked for and the configure still says '{1}' -- refusing to hand over a build whose name does not match what was requested" -f $BootstrapName, $expected)
    exit 1
}

$dll = Join-Path $bin ("$expected.dll")
if (-not (Test-Path $dll)) {
    Write-Warning "the configure says the DLL is '$expected.dll' and $bin has no such file"
    exit 1
}

$dllDirs = @()
$bsCMake = Join-Path $repo 'Code\dsh-bootstrap\CMakeLists.txt'
if (Test-Path $bsCMake) {
    $linked = @([regex]::Matches((Get-Content $bsCMake -Raw), 'dsh::([A-Za-z0-9_-]+)') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    $dllDirs += (Join-Path $repo 'Code\dsh-bootstrap')
    foreach ($m in $linked) {
        $dir = if ($m -eq 'dsh') { 'dsh' } else { "dsh-$m" }
        $dllDirs += (Join-Path $repo "Code\$dir")
    }
}
if ($dllDirs.Count -le 1) {
    Write-Warning "[build] dsh-bootstrap's link list could not be read, so the staleness check is SKIPPED rather than guessed"
} else {
    $dllTime = (Get-Item $dll).LastWriteTime
    $newest = Get-ChildItem -Path $dllDirs -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -eq '.cpp' -or $_.Extension -eq '.h' } |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($newest -and $newest.LastWriteTime -gt $dllTime) {
        $where = $newest.FullName.Replace("$repo\", '')
        Write-Warning ("{0}.dll is OLDER than {1} ({2}) -- the build did not relink it" -f $expected, $newest.Name, $where)
        exit 1
    }
}

Write-Host ''
Write-Host "[build] inject THIS file:" -ForegroundColor Green
Write-Host "        $dll" -ForegroundColor Green
Write-Host "[build] OK"
exit 0
