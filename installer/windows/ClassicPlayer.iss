#define MyAppName "Classic Player"
#define MyAppVersion "1.6.1"
#define MyAppPublisher "Classic Keys"
#define BuildRoot "..\..\build\windows-x64\ClassicPlayer_artefacts\Release"

[Setup]
AppId={{7A44AE69-49B7-4FD9-A468-5164FA579A98}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion=1.6.1.0
VersionInfoProductVersion=1.6.1.0
VersionInfoDescription=Classic Player SF2 Workstation Setup
VersionInfoProductName=Classic Player
VersionInfoCompany=Classic Keys
VersionInfoCopyright=Copyright (C) 2026 Willam Silva & Classic Keys
MinVersion=10.0.10240
DefaultDirName={autopf}\Classic Keys\Classic Player
DefaultGroupName=Classic Keys
OutputDir=..\..\outputs\installers
OutputBaseFilename=Classic-Player-1.6.1-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\Classic Player.exe

[Files]
Source: "{#BuildRoot}\Standalone\Classic Player.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\Standalone\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildRoot}\Standalone\Resources\WebUI\*"; DestDir: "{app}\Resources\WebUI"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildRoot}\WebView2Bootstrapper.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist
Source: "{#BuildRoot}\VST3\Classic Player.vst3\*"; DestDir: "{commoncf64}\VST3\Classic Player.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\Classic Player"; Filename: "{app}\Classic Player.exe"
Name: "{autodesktop}\Classic Player"; Filename: "{app}\Classic Player.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Criar atalho na área de trabalho"; GroupDescription: "Atalhos:"

[Run]
Filename: "{tmp}\WebView2Bootstrapper.exe"; Parameters: "/silent /install"; StatusMsg: "Instalando o componente de interface do Windows..."; Flags: waituntilterminated skipifdoesntexist
Filename: "{app}\Classic Player.exe"; Description: "Abrir Classic Player"; Flags: nowait postinstall skipifsilent
