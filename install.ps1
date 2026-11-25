<#
.SYNOPSIS
    Install Crucible on Windows.

.DESCRIPTION
    The Windows half of install.sh. One command:

        irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex

    And one to undo it. `iex` cannot pass a switch to the script it runs, so
    an option means turning the download into a script block first -- which is
    still one line:

        & ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1))) -Uninstall

    It installs the program: find or install the build tools, fetch the
    source, configure, build, install into a user-writable prefix, put that
    prefix on PATH, and create the directories Crucible keeps its files in.
    Then, by asking the program itself, its own Python, the compute runtimes
    this machine can use, and the fine-tuning environment. No models.

    Run as a file from inside a checkout, it builds that checkout instead of
    fetching one, the same as install.sh does. Windows runs no script files
    until it is told to, so start it as:

        powershell -ExecutionPolicy Bypass -File .\install.ps1

.PARAMETER Prefix
    Where to install. Defaults to %LOCALAPPDATA%\Programs\Crucible.

.PARAMETER Branch
    The branch or release tag to build, as git clone --branch takes it.
    Defaults to main. Crucible's own update names a tag here.

.PARAMETER Source
    A checkout to build instead of fetching one.

.PARAMETER NoGui
    Accepted and ignored. Crucible is one program and it is the window; the
    terminal face this chose between is gone.

.PARAMETER Gui
    Accepted and ignored, for the same reason.

.PARAMETER Uninstall
    Remove Crucible and everything it installed: both programs, the libraries,
    the config, the folder-trust list, the models, the runtimes and the history.
#>
[CmdletBinding()]
param(
    [string] $Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Crucible'),
    [string] $Branch = 'main',
    [string] $Source = '',
    [int]    $Jobs   = 0,
    [switch] $Gui,
    [switch] $NoGui,
    [switch] $NoDeps,
    # The compute backends. Installed by default: the releases carry a
    # prebuilt module for every platform, so this is seconds and tens of
    # megabytes, and an install that cannot load a model until you find a
    # settings page is not an install.
    [switch] $NoRuntimes,
    # The Python environment fine-tuning runs in. Installed by default and
    # several gigabytes, so the size is printed before it starts. Skipping it
    # costs nothing permanent: crucible --install-trainer does the same job
    # afterwards.
    [switch] $NoTrainer,
    [string] $TrainerFlavor = '',
    [switch] $Yes,
    [switch] $Check,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The banner is braille, and Windows PowerShell 5.1 writes it as question marks
# unless the console is told otherwise. PowerShell 7 is already UTF-8; this
# costs nothing there. Wrapped because a host without a real console -- the
# ISE, an embedded runspace -- throws on the assignment, and a mangled banner
# is not worth failing an install over.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$RepoUrl = 'https://github.com/mattsaund/Crucible.git'
$RawUrl  = 'https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1'

# The checkout this installer keeps for itself -- and the one the uninstall
# removes, which is why a checkout named with -Source is never put here.
$SrcDir  = Join-Path $env:LOCALAPPDATA 'crucible\src'

# Where Crucible keeps its files. Created by the installer so the first run
# opens onto directories that are already there, and so the uninstall has one
# place to look. Nothing is put in them.
$ConfigDir = Join-Path $env:APPDATA 'crucible'
$DataDir   = Join-Path $env:LOCALAPPDATA 'crucible'
$ModelsDir = Join-Path $DataDir 'models'

# The models folder Crucible will use: the one its config names, or the default
# when it names none -- read the way the program reads it, where empty is the
# default and a leading ~/ is the home folder. The summary used to name the
# default whatever the config said.
function Get-ModelsDir {
    try {
        $file = Join-Path $ConfigDir 'config.json'
        if (Test-Path $file) {
            $named = "$((Get-Content -Raw -Encoding UTF8 $file | ConvertFrom-Json).models_dir)"
            if ($named -eq '~') { return $HOME }
            if ($named -like '~/*') { return Join-Path $HOME ($named.Substring(2) -replace '/', '\') }
            if ($named) { return $named }
        }
    } catch { }
    return $ModelsDir
}

# ---------------------------------------------------------------------------
# State
#
# Everything the functions below change, in one table. This script runs three
# ways -- piped into iex, as a scriptblock, as a file -- and $script: does not
# name the same scope in all three, so a function writing $script:BarShown and
# the top of the file reading $BarShown are not guaranteed to be the same
# variable. A table is one object, whichever scope reaches for it.
# ---------------------------------------------------------------------------
$State = @{
    StepIndex  = -1
    StepPct    = 0
    BarShown   = $false
    BarLabel   = 'starting'
    ExitCode   = 0
    InstallLog = @()
}

# ---------------------------------------------------------------------------
# How this script ends
#
# Never with a bare `exit`. Piped into iex, or run through the scriptblock the
# uninstall line builds, there is no script file, and with no file `exit` does
# not end the script -- it ends PowerShell. Every failure closed the window and
# took its own error message with it, which from outside is a crash.
#
# So whatever has to stop calls Stop-Script, which unwinds to the bottom of
# this file. Only a run from a real file, where an exit code means something to
# whoever started it, turns that into `exit`.
# ---------------------------------------------------------------------------
$FromFile   = -not [string]::IsNullOrEmpty($PSCommandPath)
$StopSignal = 'crucible-installer-stop'

function Stop-Script ([int] $Code) {
    $State.ExitCode = $Code
    throw $StopSignal
}

# ---------------------------------------------------------------------------
# Progress
#
# A waterfall of everything the install does, with one bar under it for the
# whole install, from the first check to the last download:
#
#     ==> Installing compute runtimes
#             cuda: downloading runtime-cuda-windows-x64-b1-k1.tar.gz (412.0 MB) from github.com
#             cuda ready
#         install  [##########..................]  38%  cuda: downloading 41.0 MB of 412.0 MB
#
# Every line a part prints, winget's, git's, cmake's, the program's own setup,
# goes by above the bar as it happens. The bar stays on the bottom row and only
# moves forward. A line that only moves a number on from the last moves the
# bar instead of being written again.
#
# The figure is not a guess about elapsed time. Each part carries a weight and
# the parts are nothing like equal in length: building is minutes and checking
# for git is milliseconds, so a bar that moved a seventh per part would sit
# most of the way along for almost the whole install.
# ---------------------------------------------------------------------------
# system, build tools, source, build and install, Python, runtimes, fine-tuner
$StepWeights = @(1, 18, 4, 41, 6, 10, 20)
$IsConsole   = $Host.Name -eq 'ConsoleHost'

function Get-OverallPercent {
    $done = 0
    for ($i = 0; $i -lt $State.StepIndex -and $i -lt $StepWeights.Count; $i++) {
        $done += $StepWeights[$i]
    }
    if ($State.StepIndex -ge 0 -and $State.StepIndex -lt $StepWeights.Count) {
        $done += [int]($StepWeights[$State.StepIndex] * $State.StepPct / 100)
    }
    if ($done -gt 100) { $done = 100 }
    return $done
}

# How wide the row is. Asked of the console rather than assumed, because the
# whole display depends on the line staying on one row: a line that wraps is a
# line the next redraw cannot take back, and the bar is then stranded there for
# the rest of the install. One column of slack, since a line that exactly fills
# the width only stays put on a host that defers the wrap.
function Get-RowWidth {
    $width = 78
    try {
        $reported = $Host.UI.RawUI.WindowSize.Width
        if ($reported -gt 20) { $width = $reported - 1 }
    } catch { }
    return $width
}

function Show-Bar {
    if (-not $IsConsole) { return }
    $percent = Get-OverallPercent
    $row     = Get-RowWidth
    # The bar shrinks with the window so there is always room for a label.
    $width   = if ($row -lt 50) { 10 } elseif ($row -lt 70) { 18 } else { 28 }
    $filled  = [int]($percent * $width / 100)
    $bar     = ('#' * $filled) + ('.' * ($width - $filled))
    # Padded to the row width and rewritten in place, so a short label cannot
    # leave the tail of a longer one behind it.
    $line = "    install  [$bar] {0,3}%  {1}" -f $percent, $State.BarLabel
    if ($line.Length -gt $row) { $line = $line.Substring(0, $row - 1) + '…' }
    Write-Host ("`r" + $line.PadRight($row)) -NoNewline -ForegroundColor DarkYellow
    $State.BarShown = $true
}

# Take the bar off the line so something else can be written there. Everything
# that prints during an install goes through this first.
function Hide-Bar {
    if ($State.BarShown) {
        Write-Host ("`r" + (' ' * (Get-RowWidth)) + "`r") -NoNewline
        $State.BarShown = $false
    }
}

function Step-Begin ($Label) {
    $State.StepIndex = $State.StepIndex + 1
    $State.StepPct   = 0
    $State.BarLabel  = $Label
    Write-Line "==> $Label" 'Cyan' ''
    Show-Bar
}

function Step-At ($Percent, $Label) {
    if ($Percent -lt $State.StepPct) { return }   # never backwards
    $State.StepPct = [Math]::Min(100, $Percent)
    if ($Label) { $State.BarLabel = $Label }
    Show-Bar
}

function Step-Done {
    $State.StepPct = 100
    Show-Bar
}

function Stop-Progress {
    $State.StepIndex = $StepWeights.Count
    Show-Bar
    if ($State.BarShown) { Write-Host '' ; $State.BarShown = $false }
}

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
function Write-Banner {
    # packaging/flame.txt, verbatim. A single-quoted here-string, so nothing
    # in it is expanded or escaped.
    Write-Host @'

      ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
      ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
      ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   Crucible
      ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   An all in one local AI workshop.
      ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
      ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
      ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
      ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
      ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
      ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀

'@ -ForegroundColor DarkYellow
}

# One line of the waterfall, written above the bar, which goes back under it.
# Only if there was one: after Stop-Progress there is not, and redrawing it
# would leave a finished 100% bar sitting under the closing summary.
function Write-Line ([string] $Text, [string] $Color = 'DarkGray', [string] $Indent = '        ') {
    $was = $State.BarShown
    Hide-Bar
    Write-Host "$Indent$Text" -ForegroundColor $Color
    if ($was) { Show-Bar }
}

# All of it is said, bar or no bar.
function Write-Note ($Message) { Write-Line $Message 'Gray' '    ' }
function Write-Ok   ($Message) { Write-Line "OK $Message" 'Green' '    ' }
function Write-Warn ($Message) { Write-Line "!  $Message" 'Yellow' '    ' }
function Stop-Install ($Message) {
    Hide-Bar
    Write-Host ""
    Write-Host "error: $Message" -ForegroundColor Red
    Stop-Script 1
}

# What a program said before it failed: the lines that look like errors when
# there are any, and the end of what it wrote when there are not. A failure
# that prints nothing but "failed" sends whoever reads it to go and find a log.
function Show-Tail ([object[]] $Lines, [string] $Pattern = 'error|FAILED|fatal') {
    Hide-Bar
    $all   = @($Lines | ForEach-Object { "$_" })
    $shown = @($all | Where-Object { $_ -match $Pattern } | Select-Object -First 20)
    if ($shown.Count -eq 0) { $shown = @($all | Select-Object -Last 20) }
    foreach ($line in $shown) { Write-Host "    $line" -ForegroundColor Red }
}

function Test-Command ($Name) {
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# Running other programs
#
# Everything this script starts goes through here, with what it writes to
# stderr folded into what it writes to stdout.
#
# That is for Windows PowerShell 5.1, the one Windows ships. Redirect a native
# program's stderr there and every line of it becomes an error record -- and
# with ErrorActionPreference at Stop, the first one ends the script. cmake
# writes to stderr during an ordinary configure (llama.cpp prints its build
# type there), so every install stopped partway through configuring. In here a
# line on stderr is only a line. Whether the program failed is its exit code,
# which every caller reads from $LASTEXITCODE.
# ---------------------------------------------------------------------------
function Invoke-Native ([string] $Program, [string[]] $Arguments = @()) {
    $ErrorActionPreference = 'Continue'
    & $Program @Arguments 2>&1 | ForEach-Object { "$_" }
}

# A size as a program wrote it, "41.0 MB", in bytes. Read the same way
# whatever this machine takes a decimal point to be.
function ConvertFrom-Size ([string] $Value, [string] $Unit) {
    $number = 0.0
    if (-not [double]::TryParse($Value, [Globalization.NumberStyles]::Float,
                                [Globalization.CultureInfo]::InvariantCulture, [ref] $number)) {
        return 0
    }
    $scale = 1
    if ($Unit -like 'K*') { $scale = 1KB }
    elseif ($Unit -like 'M*') { $scale = 1MB }
    elseif ($Unit -like 'G*') { $scale = 1GB }
    elseif ($Unit -like 'T*') { $scale = 1TB }
    return $number * $scale
}

# A program run as Invoke-Native runs one, with everything it says in the
# waterfall above the bar. A line that only moves a number on from the last
# ("downloading 41.0 MB of 412.0 MB", git's "Receiving objects: 45%") moves
# the bar between $From and $To of the part instead of being written again.
# Where the program first names the parts it goes through ("Installing compute
# runtimes: cpu cuda"), each takes its share. Hands back every line, for
# Show-Tail when it fails; whether it did is $LASTEXITCODE.
function Invoke-Streamed ([string] $Program, [string[]] $Arguments = @(), [int] $From = 0, [int] $To = 100) {
    $ErrorActionPreference = 'Continue'
    $said  = ''
    $parts = @()
    $part  = 0
    & $Program @Arguments 2>&1 | ForEach-Object {
        $line = "$_"
        $line
        $text = (($line -split "`r")[-1] -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
        $text = $text -replace '^remote: ', ''
        # winget's spinner and the glyphs of its bar say nothing a line can.
        if ($text -notmatch '[A-Za-z]{2}') { return }

        if ($text -match '^Installing compute runtimes: (.+)$') { $parts = @($Matches[1] -split ' ') }
        if ($parts.Count -gt 0 -and $text -match '^([A-Za-z0-9_.+-]+):? ') {
            $index = [Array]::IndexOf($parts, $Matches[1])
            if ($index -ge 0) { $part = $index }
        }

        # The label keeps sizes but not a percentage: the bar has the one
        # percentage on the screen.
        $pct   = -1
        $label = $text
        if ($text -match '(\d{1,3})%$') {
            $pct   = [int]$Matches[1]
            $label = $text.Substring(0, $text.Length - $Matches[0].Length).TrimEnd()
        } elseif ($text -match '^Receiving objects:\s+(\d{1,3})%') {
            $pct   = [int]$Matches[1]
            $label = 'receiving objects'
        } elseif ($text -match '([0-9.]+) ?([KMGT]i?B) (of|/) (about )?([0-9.]+) ?([KMGT]i?B)') {
            $all = ConvertFrom-Size $Matches[5] $Matches[6]
            if ($all -gt 0) { $pct = [int](100 * (ConvertFrom-Size $Matches[1] $Matches[2]) / $all) }
        }
        if ($pct -ge 0) {
            $pct = [Math]::Min(100, $pct)
            if ($parts.Count -gt 0) { $pct = [int](($part * 100 + $pct) / $parts.Count) }
            Step-At ($From + [int](($To - $From) * $pct / 100)) $label
        }

        # Its figures out, a size with its unit as one: "0 B of 72.7 MB" and
        # "4.9 MB of 72.7 MB" are the same line moving on.
        $shape = ($text -replace '[0-9][0-9.]* ?[KMGT]?i?B', '#') -replace '[0-9]', ''
        if ($shape -eq $said) { return }
        $said = $shape
        # winget's "1.00 MB / 2.00 MB" is only ever how far along it is.
        if ($text -match '^[0-9.]+ ?[KMGT]i?B / ') { return }
        $color = if ($text -match 'was not installed|could not be|failed:') { 'Yellow' } else { 'DarkGray' }
        Write-Line $text $color
    }
}

# ---------------------------------------------------------------------------
# Dependencies
#
# winget rather than chocolatey or scoop: it ships with Windows 10 1809 and
# later, so it is the one package manager that is already there. Where it is
# not, this says what to install by hand rather than installing a package
# manager to install a compiler.
# ---------------------------------------------------------------------------
function Install-Package ($Id, $What, [string] $Override = '', [int] $From = 0, [int] $To = 0) {
    if (-not (Test-Command 'winget')) {
        Stop-Install "$What could not be found, and winget is not here to install it. Install $What by hand, then run this again."
    }
    $State.BarLabel = "installing $What"
    Write-Note "installing $What with winget ($Id)"
    $arguments = @('install', '--id', $Id, '--exact', '--silent',
                   '--accept-source-agreements', '--accept-package-agreements')
    if ($Override) { $arguments += @('--override', $Override) }
    $State.InstallLog = @(Invoke-Streamed 'winget' $arguments $From ([Math]::Max($From, $To)))
    # winget puts new tools on PATH for future sessions, not this one.
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('PATH', 'User')
}

# Whether an install worked is whether the thing is there afterwards. The exit
# code does not say: winget has codes of its own for "installed, restart to
# finish", and the Visual Studio installer reports 3010 for the same thing.
function Assert-Installed ([bool] $There, [string] $What) {
    if ($There) { return }
    Show-Tail $State.InstallLog
    Stop-Install "could not install $What"
}

# vswhere is installed by every Visual Studio since 2017 and lives at a fixed
# path, which is the whole reason it exists.
function Invoke-VsWhere ([string[]] $Arguments) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $found = @(Invoke-Native $vswhere (@('-latest', '-products', '*') + $Arguments) |
               Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($found.Count -eq 0) { return $null }
    return $found[0].Trim()
}

# A Visual Studio with the C++ compiler in it -- the only kind that can build
# this. Returns where it is installed.
function Find-VisualStudio {
    return Invoke-VsWhere @('-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                            '-property', 'installationPath')
}

function Install-BuildTools {
    $existing = Invoke-VsWhere @('-property', 'installationPath')
    if ($null -eq $existing) {
        # The workload is the whole point. The Build Tools installed with none
        # named are MSBuild and nothing to compile with -- which is what winget
        # installs by default, so the check after it found no compiler and
        # stopped. --wait keeps the bootstrapper from returning before the
        # install it started underneath has finished.
        Install-Package 'Microsoft.VisualStudio.2022.BuildTools' 'the Visual Studio build tools' `
            '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' 66 100
        return
    }

    # Visual Studio is here, only without its C++ compiler: a C#-only install,
    # or build tools another program put down. winget will not add a workload
    # to something it already counts as installed; the Visual Studio installer
    # will. The same compiler is a different workload in the Build Tools than
    # in the full product.
    $product  = Invoke-VsWhere @('-property', 'productId')
    $workload = if ("$product" -like '*BuildTools') { 'Microsoft.VisualStudio.Workload.VCTools' } else { 'Microsoft.VisualStudio.Workload.NativeDesktop' }
    $setup    = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $State.BarLabel = 'adding the C++ compiler to Visual Studio'
    Write-Note "adding the C++ compiler to the Visual Studio at $existing"
    Show-Bar
    $process = Start-Process -FilePath $setup -Wait -PassThru -ArgumentList @(
        'modify', '--installPath', "`"$existing`"", '--add', $workload,
        '--includeRecommended', '--passive', '--norestart')
    $State.InstallLog = @("the Visual Studio installer exited with code $($process.ExitCode)")
}

function Initialize-BuildEnvironment {
    # The compiler needs its environment: INCLUDE, LIB and a cl.exe on PATH.
    # VsDevCmd sets them, and the only way to get them into this process is to
    # run it and read back what it left.
    $vs = Find-VisualStudio
    if ($null -eq $vs) { return $false }

    $devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devcmd)) { return $false }

    # Through a batch file of its own rather than a `cmd /c` line. The line
    # needs quotes inside quotes -- the path has spaces in it -- and how those
    # reach cmd depends on which PowerShell is passing them. A file has no
    # quoting to get wrong.
    $batch = Join-Path ([IO.Path]::GetTempPath()) "crucible-vsdevcmd-$PID.cmd"
    Set-Content -Path $batch -Encoding Ascii -Value @(
        '@echo off'
        "call `"$devcmd`" -arch=amd64 -no_logo >nul"
        'set'
    )
    try {
        foreach ($line in @(Invoke-Native 'cmd' @('/c', $batch))) {
            if ($line -match '^([^=]+)=(.*)$') {
                Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
            }
        }
    } finally {
        Remove-Item $batch -ErrorAction SilentlyContinue
    }
    return (Test-Command 'cl')
}

# ---------------------------------------------------------------------------
# Uninstall
#
# One question, and it takes everything. See src/app/uninstall.cpp, which is
# where the real work happens whenever the binary is still there to do it: it
# knows the prefix it was installed into and the paths it put down.
# ---------------------------------------------------------------------------
function Remove-Crucible {
    $exe     = Join-Path $Prefix 'bin\crucible.exe'
    $handled = $false

    if (Test-Path $exe) {
        & $exe --uninstall @($(if ($Yes) { '--yes' }))
        # A run that succeeded and left its own binary on disk is one where the
        # answer was no. Without this the sweep below would go on to delete the
        # whole prefix, which is precisely what was just declined.
        if ($LASTEXITCODE -eq 0 -and (Test-Path $exe)) {
            Write-Note 'nothing was removed'
            Stop-Script 0
        }
        $handled = ($LASTEXITCODE -eq 0)
    }

    # Whatever the binary could not speak for. Where its own uninstaller ran,
    # it has already taken the config, the data and the models, and this is
    # only the install prefix and the source checkout it does not know about.
    # The shortcuts too: a .lnk pointing at a program that is gone is litter,
    # and it is litter this script made.
    foreach ($where in @([Environment]::GetFolderPath('Programs'),
                         [Environment]::GetFolderPath('Desktop'))) {
        if ($where) { Remove-Item (Join-Path $where 'Crucible.lnk') -Force -ErrorAction SilentlyContinue }
    }

    $sweep = @($Prefix, $SrcDir)
    if (-not $handled) { $sweep += @($ConfigDir, $DataDir) }
    $sweep = @($sweep | Where-Object { $_ -and (Test-Path $_) })

    if ($sweep.Count -gt 0) {
        if (-not $handled) {
            Write-Host ''
            Write-Host '  This will remove:' -ForegroundColor Gray
            foreach ($path in $sweep) { Write-Note $path }
            Write-Host ''
            if (-not $Yes) {
                $reply = Read-Host '  Remove Crucible and everything above? [Y/n]'
                if ($reply -and $reply -notmatch '^(y|yes)$') {
                    Write-Note 'nothing was removed'
                    Stop-Script 0
                }
            }
        }
        foreach ($path in $sweep) {
            Remove-Item -Recurse -Force $path -ErrorAction SilentlyContinue
        }
    }

    # The install put bin\ on the user PATH, so the uninstall takes it off
    # again. A PATH entry pointing at a directory that no longer exists is not
    # harmful, but it is litter, and it is litter this script created.
    $binDir   = Join-Path $Prefix 'bin'
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    if ($userPath -and $userPath -like "*$binDir*") {
        $kept = @($userPath -split ';' | Where-Object { $_ -and $_ -ne $binDir })
        [Environment]::SetEnvironmentVariable('PATH', ($kept -join ';'), 'User')
    }

    # Say what survived rather than claiming success over the top of it.
    $left = @(@($Prefix, $SrcDir, $ConfigDir, $DataDir) | Where-Object { Test-Path $_ })
    if ($left.Count -gt 0) {
        foreach ($path in $left) { Write-Warn "still present: $path" }
        Stop-Script 1
    }
    Write-Ok 'done'
    Stop-Script 0
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# What makes this an application rather than a command.
#
# A .lnk in the Start Menu and one on the Desktop, both pointing at the binary
# in the prefix. Windows has not allowed a program to pin itself to the taskbar
# since Windows 10, so that stays the user's right-click -- but a Start Menu
# entry is what they right-click on.
function New-Shortcuts ([string] $Source) {
    $exe = Join-Path $Prefix 'bin\crucible.exe'
    if (-not (Test-Path $exe)) { return }

    # The icon is copied into the prefix rather than referenced in the source
    # checkout: the checkout is a build artifact and may be deleted, and a
    # shortcut whose icon has gone shows a blank page.
    $icon = Join-Path $Prefix 'crucible.ico'
    $shipped = Join-Path $Source 'packaging\icons\crucible.ico'
    if (Test-Path $shipped) { Copy-Item $shipped $icon -Force }

    # A shortcut is a nicety on top of an install that has already succeeded, so
    # nothing here may stop it. A locked-down profile with no Desktop folder, a
    # policy that blocks the scripting host, a roaming profile mid-sync: all of
    # them end with Crucible installed and one fewer icon, which is a warning
    # rather than a failed install somebody has to work out how to repeat.
    try {
        $shell = New-Object -ComObject WScript.Shell
    } catch {
        Write-Warn 'could not make the shortcuts (no Windows Script Host)'
        return
    }
    foreach ($where in @([Environment]::GetFolderPath('Programs'),
                         [Environment]::GetFolderPath('Desktop'))) {
        if (-not $where) { continue }
        try {
            if (-not (Test-Path $where)) {
                New-Item -ItemType Directory -Force -Path $where | Out-Null
            }
            $link = $shell.CreateShortcut((Join-Path $where 'Crucible.lnk'))
            $link.TargetPath       = $exe
            $link.WorkingDirectory = Join-Path $Prefix 'bin'
            $link.Description      = 'Crucible -- a local AI lab'
            if (Test-Path $icon) { $link.IconLocation = $icon }
            $link.Save()
        } catch {
            Write-Warn "could not make a shortcut in $where"
        }
    }
}

function Invoke-Install {
    Write-Banner

    if ($Uninstall) { Remove-Crucible }

    # What gets built: the checkout above, fetched fresh -- unless there is
    # already one to build. -Source names one. Run as a file from inside a
    # checkout, it is that checkout without being asked, which is how a change
    # gets tried on Windows before it is pushed.
    $buildSrc = $SrcDir
    $fetch    = $true
    if ($Source) {
        if (-not (Test-Path (Join-Path $Source 'CMakeLists.txt'))) {
            Stop-Install "$Source is not a Crucible checkout"
        }
        $buildSrc = (Resolve-Path $Source).Path
        $fetch    = $false
    } elseif ($PSScriptRoot) {
        $lists = Join-Path $PSScriptRoot 'CMakeLists.txt'
        if ((Test-Path $lists) -and (Select-String -Path $lists -Pattern 'project\(crucible' -Quiet)) {
            $buildSrc = $PSScriptRoot
            $fetch    = $false
        }
    }

    if ($Check) {
        Write-Note "prefix     : $Prefix"
        Write-Note "source     : $(if ($fetch) { "$SrcDir (fetched from $Branch)" } else { $buildSrc })"
        Write-Note "shortcuts  : Start Menu and Desktop"
        Write-Note "cmake      : $(if (Test-Command 'cmake') { 'found' } else { 'would install' })"
        Write-Note "git        : $(if (Test-Command 'git')   { 'found' } else { 'would install' })"
        Write-Note "compiler   : $(if ($null -ne (Find-VisualStudio)) { 'found' } else { 'would install' })"
        Write-Note "python     : Crucible's own, tens of MB"
        Write-Note "runtimes   : $(if ($NoRuntimes) { 'skipped (-NoRuntimes)' } else { 'the compute backends this machine can use' })"
        Write-Note "fine-tuner : $(if ($NoTrainer) { 'skipped (-NoTrainer)' } else { 'a private Python environment, a few GB' })"
        Write-Note "config     : $ConfigDir"
        Write-Note "models     : $(Get-ModelsDir)"
        Write-Ok 'nothing was changed'
        Stop-Script 0
    }

    # A part left out gives its share of the bar to the build.
    if ($NoRuntimes) { $StepWeights[3] += $StepWeights[5]; $StepWeights[5] = 0 }
    if ($NoTrainer)  { $StepWeights[3] += $StepWeights[6]; $StepWeights[6] = 0 }

    Step-Begin 'Checking the machine'
    $jobCount = if ($Jobs -gt 0) { $Jobs } else { [Environment]::ProcessorCount }
    Write-Note "Windows $([Environment]::OSVersion.Version), $jobCount processors"
    Write-Note "installing into $Prefix"
    Step-Done

    Step-Begin 'Installing the build tools'
    if ($NoDeps) {
        Write-Note 'skipping the build tools (-NoDeps)'
    } else {
        if (-not (Test-Command 'git')) {
            Install-Package 'Git.Git' 'git' '' 0 33
            Assert-Installed (Test-Command 'git') 'git'
        } else {
            Write-Note 'git is already here'
        }
        Step-At 33
        if (-not (Test-Command 'cmake')) {
            Install-Package 'Kitware.CMake' 'cmake' '' 33 66
            Assert-Installed (Test-Command 'cmake') 'cmake'
        } else {
            Write-Note 'cmake is already here'
        }
        Step-At 66
        if ($null -eq (Find-VisualStudio)) {
            Install-BuildTools
            Assert-Installed ($null -ne (Find-VisualStudio)) 'the Visual Studio C++ build tools'
        }
    }
    if (-not (Initialize-BuildEnvironment)) {
        Stop-Install @'
no C++ compiler. Install the "Desktop development with C++" workload from the
Visual Studio Build Tools, then run this again:
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
'@
    }
    Step-Done

    Step-Begin 'Fetching the source'
    if (-not $fetch) {
        Write-Note "building the checkout at $buildSrc"
    } else {
        if ((Test-Path $SrcDir) -and -not (Test-Path (Join-Path $SrcDir '.git'))) {
            # Left behind by a clone that did not finish. git will not clone
            # into a directory that is not empty, so one failed first attempt
            # would otherwise fail every attempt after it.
            Remove-Item -Recurse -Force $SrcDir
        }
        if (Test-Path (Join-Path $SrcDir '.git')) {
            Write-Note "updating $SrcDir from $RepoUrl ($Branch)"
            $fetchLog = @(Invoke-Streamed 'git' @('-C', $SrcDir, 'fetch', '--progress', '--depth', '1', 'origin', $Branch))
            if ($LASTEXITCODE -eq 0) {
                $fetchLog += @(Invoke-Native 'git' @('-C', $SrcDir, 'checkout', '-q', 'FETCH_HEAD'))
            }
        } else {
            Write-Note "cloning $RepoUrl ($Branch) into $SrcDir"
            New-Item -ItemType Directory -Force -Path (Split-Path $SrcDir) | Out-Null
            $fetchLog = @(Invoke-Streamed 'git' @('clone', '--progress', '--depth', '1', '--branch', $Branch, $RepoUrl, $SrcDir))
        }
        if ($LASTEXITCODE -ne 0) {
            Show-Tail $fetchLog
            Stop-Install "could not fetch $RepoUrl ($Branch)"
        }
    }
    Step-Done

    Step-Begin 'Building Crucible'
    $State.BarLabel = 'configuring'
    $buildDir = Join-Path $buildSrc 'build'

    # Ninja when it is there and MSBuild when it is not. Ninja is several times
    # faster on a build this size, and the Visual Studio installer ships it.
    $generator = if (Test-Command 'ninja') { @('-G', 'Ninja') } else { @() }
    $configure = @('-S', $buildSrc, '-B', $buildDir) + $generator + @(
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_INSTALL_PREFIX=$Prefix",
        '-DCRUCIBLE_BACKEND_DL=ON',
        # The unit tests are built only to be run, and nothing here runs
        # them: building them was minutes of every install for nothing. CI
        # builds and runs them on every push.
        '-DCRUCIBLE_BUILD_TESTS=OFF')

    $configureLog = @(Invoke-Streamed 'cmake' $configure 0 12)
    if ($LASTEXITCODE -ne 0 -and (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        # Configure failed over a build directory that was already there. By
        # far the likeliest reason is a cache from an earlier run with another
        # generator or other paths, which cmake refuses to reuse. The cache is
        # derived data, so clearing it and trying once more is safe -- the same
        # thing install.sh and the runtime builder do.
        Write-Warn 'the existing build directory is stale; clearing it and trying again'
        Remove-Item -Recurse -Force $buildDir
        $configureLog = @(Invoke-Streamed 'cmake' $configure 0 12)
    }
    if ($LASTEXITCODE -ne 0) {
        Show-Tail $configureLog 'CMake Error|error:'
        Stop-Install 'cmake configure failed'
    }
    Step-At 12 'compiling'

    # cmake reports its own progress per compiled unit, and it is a real, ordered
    # figure worth turning into a bar rather than a spinner. Two spellings,
    # because the generator decides: the Makefile generators write
    # "[ 42%] Building ..." and Ninja writes "[123/456] Building ...". Ninja is
    # the one that will normally be in play here, since the Visual Studio
    # installer ships it and the configure above prefers it.
    $buildLog = @()
    Invoke-Native 'cmake' @('--build', $buildDir, '--config', 'Release', '-j', "$jobCount") |
        Tee-Object -Variable buildLog | ForEach-Object {
            $done = -1
            if ($_ -match '^\s*\[\s*(\d+)%\]') {
                $done = [int]$Matches[1]
            } elseif ($_ -match '^\s*\[(\d+)/(\d+)\]' -and [int]$Matches[2] -gt 0) {
                $done = [int](100 * [int]$Matches[1] / [int]$Matches[2])
            }
            if ($done -ge 0) { Step-At (12 + [int]($done * 0.82)) 'compiling' }
            $shown = ("$_" -replace '^\s*\[\s*\d+(%|/\d+)\]\s*', '').Trim()
            if ($shown) { Write-Line $shown }
        }
    if ($LASTEXITCODE -ne 0) {
        Show-Tail $buildLog
        Stop-Install 'the build failed'
    }
    Step-At 94 'installing'

    # The component, for the same reason the shell installer passes it: llama.cpp
    # and ggml carry their own install rules written for people installing them
    # as a library, and a plain install would scatter their headers and import
    # libraries through the prefix.
    $installLog = @(Invoke-Streamed 'cmake' @('--install', $buildDir, '--config', 'Release',
                                              '--component', 'crucible') 94 100)
    if ($LASTEXITCODE -ne 0) {
        Show-Tail $installLog
        Stop-Install 'the install failed'
    }

    New-Item -ItemType Directory -Force -Path $ConfigDir, $DataDir, $ModelsDir | Out-Null
    New-Shortcuts $buildSrc

    $binDir   = Join-Path $Prefix 'bin'
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    $addedToPath = $false
    if ($userPath -notlike "*$binDir*") {
        $newPath = if ($userPath) { "$userPath;$binDir" } else { $binDir }
        [Environment]::SetEnvironmentVariable('PATH', $newPath, 'User')
        $addedToPath = $true
    }
    Step-Done

    # Crucible's own Python, which routing and the chat run on and the trainer is
    # built from. Always, -NoTrainer or not: without it no prompt is routed.
    # Never fatal: the window fetches it on first start if this could not.
    # It, the runtimes and the fine-tuner all go under the same bar.
    Step-Begin "Fetching Crucible's Python"
    $null = @(Invoke-Streamed (Join-Path $binDir 'crucible.exe') @('--install-python'))
    $pythonReady = ($LASTEXITCODE -eq 0)
    if (-not $pythonReady) {
        Write-Warn "Crucible's Python did not install. The window will try again when it starts."
        Write-Note "or try again now with:  $(Join-Path $binDir 'crucible.exe') --install-python"
    }
    Step-Done

    # The compute backends. Quick -- tens of megabytes over the wire -- so they
    # go before the fine-tuner, and a machine that gives up partway through
    # the big download still ends up able to run a model.
    $runtimesReady = $false
    Step-Begin 'Installing compute runtimes'
    if ($NoRuntimes) {
        Write-Note 'skipping the compute runtimes (-NoRuntimes)'
    } else {
        $null = @(Invoke-Streamed (Join-Path $binDir 'crucible.exe') @('--install-runtimes'))
        # 2 is "already in place", which is what an update finds when
        # llama.cpp has not moved. Read as a failure, it warned on every such
        # update.
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 2) {
            $runtimesReady = $true
        } else {
            Write-Warn 'not every runtime installed. Crucible will use whichever did.'
            Write-Note "see what is there with:  $(Join-Path $binDir 'crucible.exe') --runtime-status"
        }
    }
    Step-Done

    # The fine-tuner's Python environment, done by the program rather than
    # here: `crucible --install-trainer` drives the same code the settings
    # screen does, so there is one implementation of "fetch Crucible's Python,
    # make a venv, resolve the right torch" instead of one in shell, one here
    # and one in C++ that could disagree.
    #
    # Never fatal. A proxy that blocks PyPI, a full disk -- none of those is a
    # reason to fail an install of a program that runs models perfectly well
    # without a trainer.
    $trainerReady = $false
    Step-Begin 'Setting up the fine-tuner'
    if ($NoTrainer) {
        Write-Note 'skipping the fine-tuner (-NoTrainer)'
    } else {
        $trainerArgs = @('--install-trainer')
        if ($TrainerFlavor) { $trainerArgs += @('--trainer-flavor', $TrainerFlavor) }
        $null = @(Invoke-Streamed (Join-Path $binDir 'crucible.exe') $trainerArgs)
        # 2 is "already in place", as it is for the runtimes.
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 2) {
            $trainerReady = $true
        } else {
            Write-Warn 'the fine-tuner is not set up. Crucible works without it, but training does not.'
            Write-Note "try again later with:  $(Join-Path $binDir 'crucible.exe') --install-trainer"
        }
    }
    Stop-Progress

    Write-Host ''
    # Asked of the binary, because re-running this script is also how an update
    # lands and "which one did I just get" is the first thing to say.
    $installed = (& (Join-Path $binDir 'crucible.exe') --version 2>$null | Select-Object -First 1)
    if ($installed) {
        Write-Host "  $installed is installed." -ForegroundColor Green
    } else {
        Write-Host '  Crucible is installed.' -ForegroundColor Green
    }
    Write-Host ''
    Write-Host "    crucible      $(Join-Path $binDir 'crucible.exe')"
    Write-Host "    shortcuts     Start Menu, Desktop"
    Write-Host "    config        $ConfigDir"
    Write-Host "    models        $(Get-ModelsDir)"
    if ($pythonReady) {
        Write-Host "    python        ready"
    } else {
        Write-Host "    python        not yet -- the window fetches it when it starts"
    }
    if (-not $NoRuntimes) {
        if ($runtimesReady) {
            Write-Host "    runtimes      installed"
        } else {
            Write-Host "    runtimes      incomplete -- crucible --runtime-status"
        }
    }
    if (-not $NoTrainer) {
        if ($trainerReady) {
            Write-Host "    fine-tuner    ready"
        } else {
            Write-Host "    fine-tuner    not set up -- crucible --install-trainer"
        }
    }
    if ($addedToPath) {
        Write-Host ''
        Write-Warn 'open a new terminal for the PATH change to take effect'
    }
    Write-Host ''
    Write-Host "  To remove it:  & ([scriptblock]::Create((irm $RawUrl))) -Uninstall"
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
try {
    Invoke-Install
} catch {
    if ("$($_.Exception.Message)" -ne $StopSignal) {
        # Anything that did not stop on purpose: a cmdlet failing under
        # ErrorActionPreference Stop, or a mistake in this script. Said once
        # and plainly, with the line it came from, so it can be reported --
        # and the window stays open to read it.
        Hide-Bar
        Write-Host ''
        Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "       (install.ps1, line $($_.InvocationInfo.ScriptLineNumber))" -ForegroundColor DarkGray
        $State.ExitCode = 1
    }
}
if ($FromFile) { exit $State.ExitCode }
