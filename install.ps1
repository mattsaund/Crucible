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

    It installs the program and nothing else: find or install the build tools,
    fetch the source, configure, build, install into a user-writable prefix, put
    that prefix on PATH, and create the directories Crucible keeps its files in.

    No compute runtime and no models. Crucible builds a runtime on demand from
    its settings screen, on the machine that will run it, so an installer that
    guessed at a GPU SDK was doing work the program does better.

    Run as a file from inside a checkout -- .\install.ps1 -- it builds that
    checkout instead of fetching one, the same as install.sh does.

.PARAMETER Prefix
    Where to install. Defaults to %LOCALAPPDATA%\Programs\Crucible.

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
# One bar, for the whole install:
#
#     install  [##########..................]  38%  cloning the source
#
# It is the only thing on screen while the install runs. There used to be a
# step heading and a run of notes for every part -- what was detected, what was
# installed, what each phase achieved -- which turned a one-line display into a
# page of scrollback. Everything they said is in the label beside the bar, in
# the moment it is true.
#
# The figure is not a guess about elapsed time. Each part carries a weight and
# the parts are nothing like equal in length: building is minutes and checking
# for git is milliseconds, so a bar that moved a quarter per part would sit at
# 75% for almost the whole install.
# ---------------------------------------------------------------------------
$StepWeights = @(2, 26, 8, 64)   # system, build tools, source, build+install
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
      ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   a local LLM engine that delegates.
      ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
      ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
      ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
      ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
      ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
      ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀

'@ -ForegroundColor DarkYellow
}

# Notes speak only where there is no bar to speak for them -- the dry run, and
# the uninstall. A warning is exempt: it says something is not as asked, and
# that has to reach the screen whatever else is on it.
function Write-Note ($Message) {
    if (-not $State.BarShown) { Write-Host "    $Message" -ForegroundColor DarkGray }
}
function Write-Ok   ($Message) {
    if (-not $State.BarShown) { Write-Host "    OK $Message" -ForegroundColor Green }
}
function Write-Warn ($Message) {
    # Only put the bar back if there was one. After Stop-Progress there is
    # not, and redrawing it would leave a finished 100% bar sitting under the
    # closing summary.
    $was = $State.BarShown
    Hide-Bar
    Write-Host "    !  $Message" -ForegroundColor Yellow
    if ($was) { Show-Bar }
}
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

# ---------------------------------------------------------------------------
# Dependencies
#
# winget rather than chocolatey or scoop: it ships with Windows 10 1809 and
# later, so it is the one package manager that is already there. Where it is
# not, this says what to install by hand rather than installing a package
# manager to install a compiler.
# ---------------------------------------------------------------------------
function Install-Package ($Id, $What, [string] $Override = '') {
    if (-not (Test-Command 'winget')) {
        Stop-Install "$What could not be found, and winget is not here to install it. Install $What by hand, then run this again."
    }
    $State.BarLabel = "installing $What"
    Show-Bar
    $arguments = @('install', '--id', $Id, '--exact', '--silent',
                   '--accept-source-agreements', '--accept-package-agreements')
    if ($Override) { $arguments += @('--override', $Override) }
    $State.InstallLog = @(Invoke-Native 'winget' $arguments)
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
            '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
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
    $shipped = Join-Path $Source 'packaging\crucible.ico'
    if (Test-Path $shipped) { Copy-Item $shipped $icon -Force }

    $shell = New-Object -ComObject WScript.Shell
    foreach ($where in @([Environment]::GetFolderPath('Programs'),
                         [Environment]::GetFolderPath('Desktop'))) {
        if (-not $where) { continue }
        $link = $shell.CreateShortcut((Join-Path $where 'Crucible.lnk'))
        $link.TargetPath       = $exe
        $link.WorkingDirectory = Join-Path $Prefix 'bin'
        $link.Description      = 'Crucible -- a local AI lab'
        if (Test-Path $icon) { $link.IconLocation = $icon }
        $link.Save()
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
        Write-Note "config     : $ConfigDir"
        Write-Note "models     : $ModelsDir"
        Write-Note 'No compute runtime is installed. Crucible builds one on demand'
        Write-Note 'from its settings screen, on the machine that will run it.'
        Write-Ok 'nothing was changed'
        Stop-Script 0
    }

    Step-Begin 'checking the machine'
    $jobCount = if ($Jobs -gt 0) { $Jobs } else { [Environment]::ProcessorCount }
    Step-Done

    Step-Begin 'installing the build tools'
    if (-not $NoDeps) {
        if (-not (Test-Command 'git')) {
            Install-Package 'Git.Git' 'git'
            Assert-Installed (Test-Command 'git') 'git'
        }
        Step-At 33
        if (-not (Test-Command 'cmake')) {
            Install-Package 'Kitware.CMake' 'cmake'
            Assert-Installed (Test-Command 'cmake') 'cmake'
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

    Step-Begin 'fetching the source'
    if ($fetch) {
        if ((Test-Path $SrcDir) -and -not (Test-Path (Join-Path $SrcDir '.git'))) {
            # Left behind by a clone that did not finish. git will not clone
            # into a directory that is not empty, so one failed first attempt
            # would otherwise fail every attempt after it.
            Remove-Item -Recurse -Force $SrcDir
        }
        if (Test-Path (Join-Path $SrcDir '.git')) {
            $fetchLog = @(Invoke-Native 'git' @('-C', $SrcDir, 'fetch', '--depth', '1', 'origin', $Branch))
            if ($LASTEXITCODE -eq 0) {
                $fetchLog += @(Invoke-Native 'git' @('-C', $SrcDir, 'checkout', '-q', 'FETCH_HEAD'))
            }
        } else {
            New-Item -ItemType Directory -Force -Path (Split-Path $SrcDir) | Out-Null
            $fetchLog = @(Invoke-Native 'git' @('clone', '--depth', '1', '--branch', $Branch, $RepoUrl, $SrcDir))
        }
        if ($LASTEXITCODE -ne 0) {
            Show-Tail $fetchLog
            Stop-Install "could not fetch $RepoUrl ($Branch)"
        }
    }
    Step-Done

    Step-Begin 'configuring'
    $buildDir = Join-Path $buildSrc 'build'

    # Ninja when it is there and MSBuild when it is not. Ninja is several times
    # faster on a build this size, and the Visual Studio installer ships it.
    $generator = if (Test-Command 'ninja') { @('-G', 'Ninja') } else { @() }
    $configure = @('-S', $buildSrc, '-B', $buildDir) + $generator + @(
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_INSTALL_PREFIX=$Prefix",
        '-DCRUCIBLE_BACKEND_DL=ON')

    $configureLog = @(Invoke-Native 'cmake' $configure)
    if ($LASTEXITCODE -ne 0 -and (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        # Configure failed over a build directory that was already there. By
        # far the likeliest reason is a cache from an earlier run with another
        # generator or other paths, which cmake refuses to reuse. The cache is
        # derived data, so clearing it and trying once more is safe -- the same
        # thing install.sh and the runtime builder do.
        Write-Warn 'the existing build directory is stale; clearing it and trying again'
        Remove-Item -Recurse -Force $buildDir
        $configureLog = @(Invoke-Native 'cmake' $configure)
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
    $installLog = @(Invoke-Native 'cmake' @('--install', $buildDir, '--config', 'Release',
                                            '--component', 'crucible'))
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
    Write-Host "    models        $ModelsDir"
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
