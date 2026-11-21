param(
    [Parameter(Mandatory = $true)][string]$Build,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Fail($message) {
    Write-Host "[package] FAILED: $message"
    exit 1
}

if ($Build -match '^dollsnest-coop-(v[0-9]+)$') {
    $Build = $Matches[1]
}
if ($Build -notmatch '^v[0-9]+$') {
    Fail "'$Build' is not a build number (expected vNNN or dollsnest-coop-vNNN)"
}

$sourceName = 'dollsnest-coop-' + $Build
$sourceDll = Join-Path $repo ('out\bin\' + $sourceName + '.dll')
$proxy = Join-Path $repo 'out\bin\version.dll'
$readme = Join-Path $PSScriptRoot 'README-players.txt'

if (-not (Test-Path $sourceDll)) { Fail "no such build: $sourceDll" }
if (-not (Test-Path $proxy)) { Fail "no proxy at $proxy" }
if (-not (Test-Path $readme)) { Fail "no player README at $readme" }

if ($OutDir -eq '') {
    $OutDir = Join-Path $repo 'out\package'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stage = Join-Path $env:TEMP ('dshpkg-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$win64 = Join-Path $stage 'DollsNest\Binaries\Win64'
New-Item -ItemType Directory -Force -Path $win64 | Out-Null

Copy-Item $proxy (Join-Path $win64 'version.dll') -Force
Copy-Item $sourceDll (Join-Path $win64 'dollsnest-coop.dll') -Force
Copy-Item $readme (Join-Path $win64 'README.txt') -Force

$zip = Join-Path $OutDir ('DollsNest-coop-' + $Build + '.zip')
if (Test-Path $zip) { Remove-Item $zip -Force }

Compress-Archive -Path (Join-Path $stage 'DollsNest') -DestinationPath $zip -CompressionLevel Optimal

Write-Host "[package] build : $Build"
Write-Host "[package] zip   : $zip"
Write-Host ''
Write-Host "[package] members (extract into ...\steamapps\common\Dolls Nest\):"

$members = Get-ChildItem $win64 -File | Sort-Object Name
foreach ($file in $members) {
    $hash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
    Write-Host ("  DollsNest\Binaries\Win64\{0,-20} {1,10} bytes  {2}" -f $file.Name, $file.Length, $hash)
}

$zipItem = Get-Item $zip
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ''
Write-Host ("[package] zip {0} bytes  sha256 {1}" -f $zipItem.Length, $zipHash)

Remove-Item $stage -Recurse -Force

Write-Host ''
Write-Host "[package] OK -- hand this ONE zip to both machines; two different zips are two different versions"
