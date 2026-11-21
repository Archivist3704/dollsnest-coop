param(
    [Parameter(Mandatory = $true)][string]$Build,
    [string]$GameDir = '',
    [switch]$SkipPdb
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Fail($message) {
    Write-Host "[deploy] FAILED: $message"
    exit 1
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    Fail "-GameDir is required, e.g. -GameDir 'D:\SteamLibrary\steamapps\common\Dolls Nest\DollsNest\Binaries\Win64'"
}

if ($Build -match '^dollsnest-coop-(v[0-9]+)$') {
    $Build = $Matches[1]
}
if ($Build -notmatch '^v[0-9]+$') {
    Fail "'$Build' is not a build number (expected vNNN or dollsnest-coop-vNNN)"
}

$sourceName = 'dollsnest-coop-' + $Build
$sourceDll = Join-Path $repo ('out\bin\' + $sourceName + '.dll')
$sourcePdb = Join-Path $repo ('out\bin\' + $sourceName + '.pdb')
$proxy = Join-Path $repo 'out\bin\version.dll'

if (-not (Test-Path $sourceDll)) { Fail "no such build: $sourceDll" }
if (-not (Test-Path $proxy)) { Fail "no proxy at $proxy (build it with the main build script)" }
if (-not (Test-Path $GameDir)) { Fail "no such directory: $GameDir" }

$exe = Join-Path $GameDir 'DollsNest-Win64-Shipping.exe'
if (-not (Test-Path $exe)) { Fail "$GameDir does not look like the game's Win64 directory (no DollsNest-Win64-Shipping.exe)" }

$running = Get-Process -Name 'DollsNest-Win64-Shipping' -ErrorAction SilentlyContinue
if ($running -ne $null) {
    Fail "DollsNest-Win64-Shipping.exe is running (pid $($running.Id)); close the game first -- a replaced DLL is not a reloaded one"
}

Write-Host "[deploy] build    : $Build"
Write-Host "[deploy] game dir : $GameDir"

Copy-Item $proxy (Join-Path $GameDir 'version.dll') -Force
Write-Host "[deploy] version.dll            <- out\bin\version.dll"

Copy-Item $sourceDll (Join-Path $GameDir 'dollsnest-coop.dll') -Force
Write-Host "[deploy] dollsnest-coop.dll     <- $sourceName.dll"

if (-not $SkipPdb) {
    if (Test-Path $sourcePdb) {
        Copy-Item $sourcePdb (Join-Path $GameDir 'dollsnest-coop.pdb') -Force
        Write-Host "[deploy] dollsnest-coop.pdb     <- $sourceName.pdb"
    } else {
        Write-Host "[deploy] WARNING: $sourceName.pdb is missing; crashes in this build cannot be symbolised"
    }
}

$commands = Join-Path $GameDir 'dsh-commands.txt'
Set-Content -Path $commands -Value '' -NoNewline
Write-Host "[deploy] dsh-commands.txt       cleared (an old line would be replayed into this run)"

$sourceHash = (Get-FileHash $sourceDll -Algorithm SHA256).Hash
$targetHash = (Get-FileHash (Join-Path $GameDir 'dollsnest-coop.dll') -Algorithm SHA256).Hash
$proxyHash = (Get-FileHash $proxy -Algorithm SHA256).Hash
$targetProxyHash = (Get-FileHash (Join-Path $GameDir 'version.dll') -Algorithm SHA256).Hash

Write-Host ''
Write-Host "[deploy] dollsnest-coop.dll sha256 = $targetHash"
Write-Host "[deploy] version.dll        sha256 = $targetProxyHash"

if ($sourceHash -ne $targetHash) {
    Fail "the copy does not match the source (source $sourceHash)"
}
if ($proxyHash -ne $targetProxyHash) {
    Fail "version.dll does not match out\bin\version.dll"
}

Write-Host ''
Write-Host "[deploy] OK -- same bytes at both ends. Now:"
Write-Host "[deploy]   1. start the game from Steam (no injector)"
Write-Host "[deploy]   2. check that dsh-proxy-load.txt has a line from THIS launch"
Write-Host "[deploy]   3. in dollsnest-coop.log, the first screen must say build=... and file sha256=..."
