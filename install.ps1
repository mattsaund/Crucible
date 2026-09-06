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

.PARAMETER Prefix
    Where to install. Defaults to %LOCALAPPDATA%\Programs\Crucible.

.PARAMETER NoGui
    Build only the terminal program, skipping crucible-gui. The desktop app is
    built by default: it needs nothing extra on Windows, since OpenGL is part
    of the system, so unlike Linux it costs only build time.

.PARAMETER Gui
    Build the desktop application. On by default; the switch is kept so an
    explicit -Gui still means what it says.

.PARAMETER Uninstall
    Remove Crucible and everything it installed: both programs, the libraries,
    the config, the folder-trust list, the models, the runtimes and the history.
#>
[CmdletBinding()]
param(
    [string] $Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Crucible'),
    [string] $Branch = 'main',
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
$SrcDir  = Join-Path $env:LOCALAPPDATA 'crucible\src'

# Where Crucible keeps its files. Created by the installer so the first run
# opens onto directories that are already there, and so the uninstall has one
# place to look. Nothing is put in them.
$ConfigDir = Join-Path $env:APPDATA 'crucible'
$DataDir   = Join-Path $env:LOCALAPPDATA 'crucible'
$ModelsDir = Join-Path $DataDir 'models'

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
$StepIndex   = -1
$StepPct     = 0
$BarShown    = $false
$BarLabel    = 'starting'
$IsConsole   = $Host.Name -eq 'ConsoleHost'

function Get-OverallPercent {
    $done = 0
    for ($i = 0; $i -lt $StepIndex -and $i -lt $StepWeights.Count; $i++) {
        $done += $StepWeights[$i]
    }
    if ($StepIndex -ge 0 -and $StepIndex -lt $StepWeights.Count) {
        $done += [int]($StepWeights[$StepIndex] * $StepPct / 100)
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
    $line = "    install  [$bar] {0,3}%  {1}" -f $percent, $BarLabel
    if ($line.Length -gt $row) { $line = $line.Substring(0, $row - 1) + '…' }
    Write-Host ("`r" + $line.PadRight($row)) -NoNewline -ForegroundColor DarkYellow
    $script:BarShown = $true
}

# Take the bar off the line so something else can be written there. Everything
# that prints during an install goes through this first.
function Hide-Bar {
    if ($script:BarShown) {
        Write-Host ("`r" + (' ' * (Get-RowWidth)) + "`r") -NoNewline
        $script:BarShown = $false
    }
}

function Step-Begin ($Label) {
    $script:StepIndex = $script:StepIndex + 1
    $script:StepPct   = 0
    $script:BarLabel  = $Label
    Show-Bar
}

function Step-At ($Percent, $Label) {
    if ($Percent -lt $script:StepPct) { return }   # never backwards
    $script:StepPct = [Math]::Min(100, $Percent)
    if ($Label) { $script:BarLabel = $Label }
    Show-Bar
}

function Step-Done {
    $script:StepPct = 100
    Show-Bar
}

function Stop-Progress {
    $script:StepIndex = $script:StepWeights.Count
    Show-Bar
    if ($script:BarShown) { Write-Host '' ; $script:BarShown = $false }
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
    if (-not $script:BarShown) { Write-Host "    $Message" -ForegroundColor DarkGray }
}
function Write-Ok   ($Message) {
    if (-not $script:BarShown) { Write-Host "    OK $Message" -ForegroundColor Green }
}
function Write-Warn ($Message) {
    # Only put the bar back if there was one. After Stop-Progress there is
    # not, and redrawing it would leave a finished 100% bar sitting under the
    # closing summary.
    $was = $script:BarShown
    Hide-Bar
    Write-Host "    !  $Message" -ForegroundColor Yellow
    if ($was) { Show-Bar }
}
function Stop-Install ($Message) {
    Hide-Bar
    Write-Host ""
    Write-Host "error: $Message" -ForegroundColor Red
    exit 1
}

function Test-Command ($Name) {
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# Dependencies
#
# winget rather than chocolatey or scoop: it ships with Windows 10 1809 and
# later, so it is the one package manager that is already there. Where it is
# not, this says what to install by hand rather than installing a package
# manager to install a compiler.
# ---------------------------------------------------------------------------
function Install-Package ($Id, $What) {
    if (-not (Test-Command 'winget')) {
        Stop-Install "$What is missing and winget is not available. Install $What by hand, then run this again."
    }
    $script:BarLabel = "installing $What"
    Show-Bar
    winget install --id $Id --exact --silent --accept-source-agreements --accept-package-agreements | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Stop-Install "could not install $What"
    }
    # winget puts new tools on PATH for future sessions, not this one.
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('PATH', 'User')
}

function Find-VisualStudio {
    # vswhere is installed by every Visual Studio since 2017 and lives at a
    # fixed path, which is the whole reason it exists.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    $found = & $vswhere -latest -products * `
                        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -property installationPath 2>$null
    if ([string]::IsNullOrWhiteSpace($found)) { return $null }
    return $found.Trim()
}

function Initialize-BuildEnvironment {
    # The compiler needs its environment: INCLUDE, LIB and a cl.exe on PATH.
    # VsDevCmd sets them, and the only way to get them into this process is to
    # run it and read back what changed.
    $vs = Find-VisualStudio
    if ($null -eq $vs) { return $false }

    $devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devcmd)) { return $false }

    cmd /s /c "`"$devcmd`" -arch=amd64 -no_logo && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
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
            exit 0
        }
        $handled = ($LASTEXITCODE -eq 0)
    }

    # Whatever the binary could not speak for. Where its own uninstaller ran,
    # it has already taken the config, the data and the models, and this is
    # only the install prefix and the source checkout it does not know about.
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
                    exit 0
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
    $left = @($Prefix, $SrcDir, $ConfigDir, $DataDir) | Where-Object { Test-Path $_ }
    if ($left) {
        foreach ($path in $left) { Write-Warn "still present: $path" }
        exit 1
    }
    Write-Ok 'done'
    exit 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Banner

if ($Uninstall) { Remove-Crucible }

# -Gui is accepted and redundant; -NoGui is the one that changes anything.
$BuildGui = -not $NoGui

if ($Check) {
    Write-Note "prefix     : $Prefix"
    Write-Note "desktop app: $(if ($BuildGui) { 'yes' } else { 'no (-NoGui)' })"
    Write-Note "cmake      : $(if (Test-Command 'cmake') { 'found' } else { 'would install' })"
    Write-Note "git        : $(if (Test-Command 'git')   { 'found' } else { 'would install' })"
    Write-Note "compiler   : $(if ($null -ne (Find-VisualStudio)) { 'found' } else { 'would install' })"
    Write-Note "config     : $ConfigDir"
    Write-Note "models     : $ModelsDir"
    Write-Note 'No compute runtime is installed. Crucible builds one on demand'
    Write-Note 'from its settings screen, on the machine that will run it.'
    Write-Ok 'nothing was changed'
    exit 0
}

Step-Begin 'checking the machine'
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
Step-Done

Step-Begin 'installing the build tools'
if (-not $NoDeps) {
    if (-not (Test-Command 'git'))   { Install-Package 'Git.Git' 'git' }
    Step-At 33
    if (-not (Test-Command 'cmake')) { Install-Package 'Kitware.CMake' 'cmake' }
    Step-At 66
    if ($null -eq (Find-VisualStudio)) {
        Install-Package 'Microsoft.VisualStudio.2022.BuildTools' 'the Visual Studio build tools'
    }
}
if (-not (Initialize-BuildEnvironment)) {
    Stop-Install @'
no C++ compiler. Install the "Desktop development with C++" workload from the
Visual Studio Build Tools, then run this again:
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact
'@
}
Step-Done

Step-Begin 'fetching the source'
if (Test-Path (Join-Path $SrcDir '.git')) {
    git -C $SrcDir fetch --depth 1 origin $Branch  | Out-Null
    git -C $SrcDir checkout -q FETCH_HEAD          | Out-Null
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $SrcDir) | Out-Null
    git clone --depth 1 --branch $Branch $RepoUrl $SrcDir | Out-Null
}
if ($LASTEXITCODE -ne 0) { Stop-Install "could not fetch $RepoUrl" }
Step-Done

Step-Begin 'configuring'
$buildDir = Join-Path $SrcDir 'build'

# Ninja when it is there and MSBuild when it is not. Ninja is several times
# faster on a build this size, and the Visual Studio installer ships it.
$generator = if (Test-Command 'ninja') { @('-G', 'Ninja') } else { @() }

& cmake -S $SrcDir -B $buildDir @generator `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_INSTALL_PREFIX="$Prefix" `
    -DCRUCIBLE_BACKEND_DL=ON `
    -DCRUCIBLE_BUILD_GUI="$(if ($BuildGui) { 'ON' } else { 'OFF' })" 2>&1 |
    Tee-Object -Variable configureLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    Hide-Bar
    $configureLog | Select-String -Pattern 'CMake Error|error:' | Select-Object -First 20 |
        ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Stop-Install 'cmake configure failed'
}
Step-At 12 'compiling'

# cmake reports its own progress per compiled unit, and it is a real, ordered
# figure worth turning into a bar rather than a spinner. Two spellings, because
# the generator decides: the Makefile generators write "[ 42%] Building ..."
# and Ninja writes "[123/456] Building ...". Ninja is the one that will
# normally be in play here, since the Visual Studio installer ships it and the
# configure above prefers it.
& cmake --build $buildDir --config Release -j $Jobs 2>&1 |
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
    Hide-Bar
    $buildLog | Select-String -Pattern 'error|FAILED' | Select-Object -First 20 |
        ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Stop-Install 'the build failed'
}
Step-At 94 'installing'

# The component, for the same reason the shell installer passes it: llama.cpp
# and ggml carry their own install rules written for people installing them as
# a library, and a plain install would scatter their headers and import
# libraries through the prefix.
& cmake --install $buildDir --config Release --component crucible | Out-Null
if ($LASTEXITCODE -ne 0) { Stop-Install 'the install failed' }

New-Item -ItemType Directory -Force -Path $ConfigDir, $DataDir, $ModelsDir | Out-Null

$binDir   = Join-Path $Prefix 'bin'
$userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
$addedToPath = $false
if ($userPath -notlike "*$binDir*") {
    [Environment]::SetEnvironmentVariable('PATH', "$userPath;$binDir", 'User')
    $addedToPath = $true
}
Stop-Progress

Write-Host ''
Write-Host '  Crucible is installed.' -ForegroundColor Green
Write-Host ''
Write-Host "    crucible      $(Join-Path $binDir 'crucible.exe')"
if ($BuildGui) {
    Write-Host "    crucible-gui  $(Join-Path $binDir 'crucible-gui.exe')"
}
Write-Host "    config        $ConfigDir"
Write-Host "    models        $ModelsDir"
if ($addedToPath) {
    Write-Host ''
    Write-Warn 'open a new terminal for the PATH change to take effect'
}
Write-Host ''
Write-Host "  To remove it:  & ([scriptblock]::Create((irm $RawUrl))) -Uninstall"
Write-Host ''
