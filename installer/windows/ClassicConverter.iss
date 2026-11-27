; Setup wizard for the stand-alone Classic Converter (.npno / .nsmp to .ckp / .cks / .sf2).
#define MyAppName "Classic Converter"
#define MyAppVersion "3.1.0"

[Setup]
AppId={{C3B7E1D4-5A92-4F0B-9E6A-8D21F4C7A350}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Classic Keys
VersionInfoVersion=3.1.0.0
VersionInfoProductVersion=3.1.0.0
VersionInfoDescription=Classic Converter Setup
VersionInfoProductName=Classic Converter
VersionInfoCompany=Classic Keys
VersionInfoCopyright=Copyright (C) 2026 Willam Silva & Classic Keys
MinVersion=10.0.10240
DefaultDirName={autopf}\Classic Keys\Classic Converter
DefaultGroupName=Classic Keys
DisableProgramGroupPage=yes
OutputDir=..\..\outputs\converter
OutputBaseFilename=Classic-Converter-3.1.0-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
ShowLanguageDialog=yes
UninstallDisplayIcon={app}\Classic Converter.exe

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\..\build\windows-x64\ClassicConverter_artefacts\Release\Classic Converter.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Classic Converter"; Filename: "{app}\Classic Converter.exe"
Name: "{autodesktop}\Classic Converter"; Filename: "{app}\Classic Converter.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Classic Converter.exe"; Description: "{cm:LaunchProgram,Classic Converter}"; Flags: nowait postinstall skipifsilent
