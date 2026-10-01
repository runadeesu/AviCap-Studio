; AviCap Studio installer (Inno Setup 6).
;
; Built by tools/package_windows.sh (Linux cross build) or build_release.bat
; (Windows). Defines passed on the ISCC command line:
;   /DAppVersion=0.1.0  /DStageDir=<folder with the application files>
;   /O<output folder>
;
; Design notes
; - Per-user install by default (no admin prompt); "install for all users"
;   is offered in the wizard (PrivilegesRequiredOverridesAllowed=dialog).
; - Registers the .avicap project file type.
; - Uninstalling never touches user data: projects, media, settings, cache
;   and autosave (%LOCALAPPDATA%\AviCapStudio) are left in place.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "..\release\stage"
#endif

#define AppName "AviCap Studio"
#define AppExe "AviCapStudio.exe"
#define ProgId "AviCapStudio.Project"

[Setup]
AppId={{6A1D3C1E-4E7B-4B1F-9C3E-5A2B7D0C8E41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=AviCap Studio contributors
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=AviCapStudio-Setup
SetupIconFile=..\apps\studio\resources\avicap.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
LicenseFile={#StageDir}\licenses\LICENSE-SUMMARY.txt
ShowLanguageDialog=auto

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
japanese.AssocTask=.avicap プロジェクトファイルを AviCap Studio に関連付ける
english.AssocTask=Associate .avicap project files with AviCap Studio
japanese.ProjectFile=AviCap Studio プロジェクト
english.ProjectFile=AviCap Studio Project
japanese.LaunchApp=AviCap Studio を起動する
english.LaunchApp=Launch AviCap Studio

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "{cm:AssocTask}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "portable.txt"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.avicap"; ValueType: string; ValueName: ""; ValueData: "{#ProgId}"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.avicap\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "{cm:ProjectFile}"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".avicap"; ValueData: ""; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

; No [UninstallDelete] entries on purpose: user projects, media and the
; per-user data folder are never removed by the uninstaller.
