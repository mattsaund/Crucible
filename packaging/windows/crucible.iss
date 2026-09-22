; Crucible's Windows installer: the double-click kind.
;
; install.ps1 compiles from source, which on Windows means Visual Studio Build
; Tools, a winget prompt for administrator, and the better part of an hour.
; That is a reasonable thing to ask of somebody working on Crucible and an
; unreasonable one to ask of somebody trying it, so this packages a build that
; already happened: unpack, make the shortcuts, register an uninstaller.
;
; Built by .github/workflows/release.yml with
;   iscc /DStageDir=<staged prefix> /DVersion=<x.y.z> packaging\windows\crucible.iss
; where the staged prefix is what `cmake --install --component crucible` wrote:
; bin\crucible.exe and the three llama.cpp DLLs beside it. Windows has no RPATH
; and looks next to the executable, which is why that layout needs no change
; here.

#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef Version
  #define Version "0.5.1"
#endif

[Setup]
; A fixed id, so an upgrade replaces the install rather than
; landing beside it and leaving two entries in Apps & features.
AppId={{44B700E9-B63E-51AB-8FA1-94D1BADF757D}
AppName=Crucible
AppVersion={#Version}
AppPublisher=Crucible
AppPublisherURL=https://github.com/mattsaund/Crucible
DefaultDirName={localappdata}\Programs\Crucible
DefaultGroupName=Crucible
DisableProgramGroupPage=yes
; Per-user, so there is no administrator prompt. Crucible needs nothing outside
; the user's own directories, and asking for administrator to install a chat
; window is how an installer teaches people to click through that dialog.
PrivilegesRequired=lowest
OutputDir=.
OutputBaseFilename=Crucible-Setup
SetupIconFile=..\crucible.ico
UninstallDisplayIcon={app}\bin\crucible.exe
UninstallDisplayName=Crucible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; x64 rather than x64compatible: the newer spelling wants Inno 6.3, and
; the runners do not all have it.
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Tasks]
Name: "desktopicon"; Description: "Create a &Desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
; Both of these can be dragged to the taskbar, which is the point of making
; them: a program people pin is a program they come back to.
Name: "{group}\Crucible";      Filename: "{app}\bin\crucible.exe"; WorkingDir: "{app}\bin"
Name: "{userdesktop}\Crucible"; Filename: "{app}\bin\crucible.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\crucible.exe"; Description: "Start Crucible"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The install directory itself, once its files are gone. Crucible's config,
; models and history live under the user's profile and are deliberately left
; alone: uninstalling the program is not a request to delete the models, and
; `crucible --uninstall` is the command that offers to take those too.
Type: dirifempty; Name: "{app}\bin"
Type: dirifempty; Name: "{app}"
