[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$scripts = Get-ChildItem -Path $repo -Recurse -Filter '*.ps1' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\out\\build\\' }

if (-not $Quiet) { Write-Host "checking $($scripts.Count) PowerShell script(s) under $repo" }

$bad = @()
foreach ($s in $scripts) {
    $errors = $null
    $tokens = $null

    $null = [System.Management.Automation.Language.Parser]::ParseFile(
        $s.FullName, [ref]$tokens, [ref]$errors)

    if ($errors -and $errors.Count -gt 0) {
        $bad += $s
        Write-Host ("FAIL  {0}" -f $s.FullName.Replace("$repo\", ''))
        foreach ($e in $errors) {
            Write-Host ("        line {0}: {1}" -f $e.Extent.StartLineNumber, $e.Message)
        }
    } elseif (-not $Quiet) {
        Write-Host ("ok    {0}" -f $s.FullName.Replace("$repo\", ''))
    }
}

Write-Host ""
if ($bad.Count -gt 0) {
    Write-Host "RESULT: FAIL ($($bad.Count) of $($scripts.Count) have syntax errors)"
    exit 1
}
Write-Host "RESULT: PASS (all $($scripts.Count) parse cleanly)"
exit 0
