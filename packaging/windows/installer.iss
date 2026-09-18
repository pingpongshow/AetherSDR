; AetherSDR Inno Setup Installer Script
; Version is passed via /DAPP_VERSION=x.y.z from the CI workflow

#ifndef APP_VERSION
  #define APP_VERSION "0.0.0"
#endif

[Setup]
AppName=AetherSDR
AppVersion={#APP_VERSION}
AppPublisher=AetherSDR Project
AppPublisherURL=https://github.com/ten9876/AetherSDR
AppSupportURL=https://github.com/ten9876/AetherSDR/issues
DefaultDirName={autopf}\AetherSDR
DefaultGroupName=AetherSDR
UninstallDisplayIcon={app}\AetherSDR.exe
OutputBaseFilename=AetherSDR-v{#APP_VERSION}-Windows-x64-setup
OutputDir=..\..
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\AetherSDR"; Filename: "{app}\AetherSDR.exe"
Name: "{group}\Uninstall AetherSDR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\AetherSDR"; Filename: "{app}\AetherSDR.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AetherSDR.exe"; Description: "Launch AetherSDR"; Flags: nowait postinstall skipifsilent
