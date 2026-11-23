# Crucible in the terminal: one line, no compiler.
#
#   irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.ps1 | iex
#
# Fetches the newest release's terminal download, puts it in
# %LOCALAPPDATA%\Programs\CrucibleCLI and adds that to your PATH. Separate from
# the app, and they share their settings, models and chats.
#
# To take it off again (models and chats stay):
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.ps1))) -Uninstall
param(
    [string]$Version = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repo = 'mattsaund/Crucible'
$asset = 'cruciblecli-windows-x86_64.zip'
if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set.' }
$root = Join-Path $env:LOCALAPPDATA 'Programs\CrucibleCLI'
$bin = Join-Path $root 'bin'

function Set-UserPath([string[]]$parts) {
    [Environment]::SetEnvironmentVariable('PATH', ($parts -join ';'), 'User')
}

$userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
$parts = @()
if ($userPath) { $parts = $userPath.Split(';') | Where-Object { $_ } }

if ($Uninstall) {
    if (Test-Path $root) {
        try {
            Remove-Item -Recurse -Force $root
        } catch {
            throw 'Close every cruciblecli first, then run this again.'
        }
    }
    Set-UserPath ($parts | Where-Object { $_ -ne $bin })
    Write-Host 'cruciblecli is gone. Your models, settings and chats are still there.'
    return
}

if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
    throw "There is no download for $env:PROCESSOR_ARCHITECTURE yet."
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("cruciblecli-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $zip = Join-Path $work $asset
    if ($env:CRUCIBLE_CLI_ARCHIVE) {
        # A local file in place of the download, for testing.
        Copy-Item $env:CRUCIBLE_CLI_ARCHIVE $zip
        $sumFile = "$($env:CRUCIBLE_CLI_ARCHIVE).sha256"
        $expected = if (Test-Path $sumFile) { (Get-Content $sumFile -Raw).Split()[0] } else { '' }
    } else {
        $url = if ($Version) { "https://github.com/$repo/releases/download/$Version/$asset" }
               else { "https://github.com/$repo/releases/latest/download/$asset" }
        Write-Host 'Downloading Crucible CLI'
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
        $expected = ''
        try {
            $sum = Invoke-WebRequest -UseBasicParsing -Uri "$url.sha256"
            $expected = ([Text.Encoding]::UTF8.GetString($sum.Content)).Split()[0]
        } catch { }
    }
    if ($expected -and ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $expected.ToUpper())) {
        throw 'The download is damaged. Try again.'
    }

    Expand-Archive -Path $zip -DestinationPath $work -Force
    $unpacked = Join-Path $work 'cruciblecli'
    foreach ($needed in 'bin\cruciblecli.exe', 'bin\crucible-engine.exe') {
        if (-not (Test-Path (Join-Path $unpacked $needed))) { throw "The download has no $needed in it." }
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $root) | Out-Null
    if (Test-Path $root) {
        # A program that is running can be neither deleted nor overwritten, and
        # /update runs this from inside one. It can be moved aside, though, so
        # the old files step out of the way and go once nothing is using them.
        $old = { Get-ChildItem $root -Recurse -File -Filter '*.old' | Remove-Item -Force -ErrorAction SilentlyContinue }
        & $old
        Get-ChildItem $root -Recurse -File | Where-Object { $_.Extension -ne '.old' } | ForEach-Object {
            Move-Item -LiteralPath $_.FullName -Destination "$($_.FullName).$(Get-Random).old" -Force
        }
        Copy-Item -Path (Join-Path $unpacked '*') -Destination $root -Recurse -Force
        & $old
    } else {
        Move-Item $unpacked $root
    }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($parts -notcontains $bin) {
    Set-UserPath (@($parts) + $bin)
}
if (($env:PATH -split ';') -notcontains $bin) { $env:PATH = "$env:PATH;$bin" }

$installed = & (Join-Path $bin 'cruciblecli.exe') --version
Write-Host ''
Write-Host "Installed $installed."
Write-Host 'Type cruciblecli in any folder. A terminal that was already open needs reopening.'
