function Invoke-Native {

    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).Path,
        [switch]$Echo
    )

    $quoted = foreach ($a in $Arguments) {
        if ($a -match '[\s"&|<>^]') {
            '"' + ($a -replace '"', '\"') + '"'
        } else {
            $a
        }
    }
    $cmdLine = '"' + $FilePath + '" ' + ($quoted -join ' ')

    $bat = Join-Path $env:TEMP ('dsh-native-' + [guid]::NewGuid().ToString('N') + '.bat')
    try {
        @(
            '@echo off'
            "cd /d `"$WorkingDirectory`""
            "$cmdLine 2>&1"
            'exit /b %ERRORLEVEL%'
        ) | Set-Content -Path $bat -Encoding ASCII

        if ($Echo) { Write-Host "[native] $cmdLine" }

        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $out = & cmd /c "`"$bat`"" 2>&1
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prev
        }

        return [pscustomobject]@{
            ExitCode    = $code
            Output      = @($out)
            CommandLine = $cmdLine
        }
    } finally {
        Remove-Item $bat -Force -ErrorAction SilentlyContinue
    }
}

function Assert-Native {

    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).Path,
        [int]$TailLines = 40
    )
    $r = Invoke-Native -FilePath $FilePath -Arguments $Arguments `
                       -WorkingDirectory $WorkingDirectory -Echo
    if ($r.ExitCode -ne 0) {
        $tail = ($r.Output | Select-Object -Last $TailLines) -join "`n"
        throw "$FilePath failed (exit $($r.ExitCode))`n$tail"
    }
    return $r
}
