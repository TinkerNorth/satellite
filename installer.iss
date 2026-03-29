; ============================================================================
;  Satellite — Inno Setup Installer Script
;  Requires: Inno Setup 6+ (https://jrsoftware.org/isinfo.php)
;
;  Build:  iscc installer.iss
;  Output: dist\SatelliteSetup.exe
; ============================================================================

#define MyAppName "Satellite"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "TinkerNorth"
#define MyAppURL "https://github.com/TinkerNorth/satellite"
#define MyAppExeName "satellite.exe"

[Setup]
AppId={{B8F3A2E1-7D4C-4E5F-9A1B-3C6D8E0F2A4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=SatelliteSetup
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start Satellite with Windows"; GroupDescription: "Other:"

[Files]
Source: "satellite.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Add Windows Firewall rules for LAN access
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite HTTP"" dir=in action=allow protocol=TCP localport=9877 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (HTTP)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite UDP"" dir=in action=allow protocol=UDP localport=9876 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (UDP)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite Pairing"" dir=in action=allow protocol=TCP localport=9878 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (Pairing)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite Discovery"" dir=in action=allow protocol=UDP localport=9879 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (Discovery)..."
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: shellexec nowait postinstall skipifsilent

[UninstallRun]
Filename: "taskkill"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden; RunOnceId: "KillSatellite"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite HTTP"""; Flags: runhidden; RunOnceId: "FwHTTP"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite UDP"""; Flags: runhidden; RunOnceId: "FwUDP"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite Pairing"""; Flags: runhidden; RunOnceId: "FwPair"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite Discovery"""; Flags: runhidden; RunOnceId: "FwDisc"

