#define BuildRoot "..\..\build\windows-x64\ClassicPlayer_artefacts\Release"
#ifdef FINAL_RELEASE
  #define DisplayName "Classic Player 2.0.4"
  #define InstallDir "{autopf}\Classic Keys\Classic Player"
  #define InstallerName "Classic-Player-2.0.4-Windows-x64-Setup"
  #define ReleaseAppId "{{7A44AE69-49B7-4FD9-A468-5164FA579A98}"
#else
  #define DisplayName "Classic Player 2.0.4 Teste"
  #define InstallDir "{autopf}\Classic Keys\Classic Player 2.0.4 Teste"
  #define InstallerName "Classic-Player-2.0.4-Standalone-VST3-teste-Windows-x64-Setup"
  #define ReleaseAppId "{{93DADFF3-E1B0-4B96-8B34-10D6CD8F0201}"
#endif

[Setup]
AppId={#ReleaseAppId}
AppName={#DisplayName}
AppVersion=2.0.4
AppPublisher=Classic Keys
VersionInfoVersion=2.0.4.0
VersionInfoProductVersion=2.0.4.0
DefaultDirName={#InstallDir}
DefaultGroupName=Classic Keys
OutputDir=..\..\outputs\vst3-test
OutputBaseFilename={#InstallerName}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
ShowLanguageDialog=yes
UninstallDisplayIcon={app}\Classic Player.exe

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "{#BuildRoot}\Standalone\Classic Player.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\Standalone\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildRoot}\Standalone\Resources\WebUI\*"; DestDir: "{app}\Resources\WebUI"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildRoot}\VST3\Classic Player.vst3\*"; DestDir: "{commoncf64}\VST3\Classic Player.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#DisplayName}"; Filename: "{app}\Classic Player.exe"

[Run]
Filename: "{app}\Classic Player.exe"; Description: "{cm:LaunchClassicPlayer}"; Flags: nowait postinstall skipifsilent

[CustomMessages]
brazilianportuguese.LaunchClassicPlayer=Abrir Classic Player
english.LaunchClassicPlayer=Launch Classic Player
spanish.LaunchClassicPlayer=Abrir Classic Player

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  LanguageDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    LanguageDir := ExpandConstant('{commonappdata}\Classic Keys\Classic Player');
    if ForceDirectories(LanguageDir) then
      SaveStringToFile(LanguageDir + '\installation-language.txt', ActiveLanguage(), False);
  end;
end;
