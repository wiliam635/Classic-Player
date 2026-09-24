#define BuildRoot "..\..\build\windows-x64\ClassicPlayer_artefacts\Release"

[Setup]
AppId={{93DADFF3-E1B0-4B96-8B34-10D6CD8F0201}
AppName=Classic Player 2.0.1 Teste
AppVersion=2.0.1
AppPublisher=Classic Keys
VersionInfoVersion=2.0.1.0
VersionInfoProductVersion=2.0.1.0
DefaultDirName={autopf}\Classic Keys\Classic Player 2.0.1 Teste
DefaultGroupName=Classic Keys
OutputDir=..\..\outputs\vst3-test
OutputBaseFilename=Classic-Player-2.0.1-Standalone-VST3-teste-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\Classic Player.exe

[Files]
Source: "{#BuildRoot}\Standalone\Classic Player.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\Standalone\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\Standalone\Resources\WebUI\*"; DestDir: "{app}\Resources\WebUI"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildRoot}\VST3\Classic Player.vst3\*"; DestDir: "{commoncf64}\VST3\Classic Player.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Classic Player 2.0.1 Teste"; Filename: "{app}\Classic Player.exe"

[Run]
Filename: "{app}\Classic Player.exe"; Description: "Abrir Classic Player 2.0.1 Teste"; Flags: nowait postinstall skipifsilent
