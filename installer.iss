; ============================================================================
;  Satellite -- Inno Setup Installer Script
;  Requires: Inno Setup 6+ (https://jrsoftware.org/isinfo.php)
;
;  Build:  pwsh scripts/fetch-redist.ps1 ; iscc installer.iss
;  Output: dist\SatelliteSetup.exe
;
;  ViGEmBus 1.22.0 (final upstream release, repo archived 2023-11) is
;  bundled as a prerequisite. The /VIGEM= switch overrides the auto-detect:
;    /VIGEM=auto      (default)  install only if missing or older
;    /VIGEM=bundled              force install even over a newer version
;    /VIGEM=skip                 leave the driver alone entirely
; ============================================================================

#define MyAppName "Satellite"
; Read the authoritative version from the /VERSION file at preprocess time.
; Single source of truth shared with CMake + src/core/version.h.
#define VersionFile FileOpen(SourcePath + "VERSION")
#define MyAppVersion Trim(FileRead(VersionFile))
#expr FileClose(VersionFile)
#undef VersionFile
#define MyAppPublisher "TinkerNorth"
#define MyAppURL "https://github.com/TinkerNorth/satellite"
#define MyAppExeName "satellite.exe"

; --- Bundled ViGEmBus (see redist/README.md for vendoring notes) ---
#define ViGEmBusVersion "1.22.0"
#define ViGEmBusInstaller "ViGEmBus_1.22.0_x64_x86_arm64.exe"

[Setup]
AppId={{B8F3A2E1-7D4C-4E5F-9A1B-3C6D8E0F2A4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
AppComments=Low-latency Xbox controller forwarding over the network. Tray app + web UI at http://localhost:9877.
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
; Embed version info into SatelliteSetup.exe itself (Properties > Details).
; Improves AV reputation and makes the bootstrapper self-identifying.
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
; If satellite.exe is running during install/uninstall, ask Restart Manager
; to close it gracefully instead of leaving locked files behind.
CloseApplications=yes
RestartApplications=no
; PrivilegesRequired=admin combined with the HKCU autostart entry triggers
; an Inno Setup warning by default. Acknowledge it: under UAC elevation,
; Inno Setup correctly resolves HKCU/{userappdata} to the calling user's
; hive (not the admin user's), so the autostart Run entry lands in the
; right place. We need admin for ViGEmBus + Program Files anyway.
UsedUserAreasWarning=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main";  Description: "Satellite (required)"; Types: full custom; Flags: fixed
Name: "vigem"; Description: "ViGEmBus driver -- required for virtual gamepad output"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start Satellite with Windows"; GroupDescription: "Other:"

[Files]
Source: "satellite.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: main
; ViGEmBus prerequisite -- only extracted when we'll actually run it (see ShouldRunViGEm).
Source: "redist\{#ViGEmBusInstaller}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Components: vigem; Check: ShouldRunViGEm

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Start {#MyAppName} (system tray app -- web UI at http://localhost:9877)"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Start {#MyAppName} (system tray app -- web UI at http://localhost:9877)"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Add Windows Firewall rules for LAN access
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite HTTP"" dir=in action=allow protocol=TCP localport=9877 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (HTTP)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite UDP"" dir=in action=allow protocol=UDP localport=9876 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (UDP)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite Pairing"" dir=in action=allow protocol=TCP localport=9878 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (Pairing)..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Satellite Discovery"" dir=in action=allow protocol=UDP localport=9879 program=""{app}\{#MyAppExeName}"" profile=private"; Flags: runhidden; StatusMsg: "Configuring firewall (Discovery)..."
; Interactive install: normal "Launch Satellite?" tickbox on the finish page.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: shellexec nowait postinstall skipifsilent
; OTA-driven silent install (the running app launched us with /OTA):
; relaunch the new binary unconditionally, no UI. WantsOTARelaunch is
; defined in the [Code] section below.
Filename: "{app}\{#MyAppExeName}"; Flags: shellexec nowait; Check: WantsOTARelaunch

[UninstallRun]
Filename: "taskkill"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden; RunOnceId: "KillSatellite"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite HTTP"""; Flags: runhidden; RunOnceId: "FwHTTP"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite UDP"""; Flags: runhidden; RunOnceId: "FwUDP"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite Pairing"""; Flags: runhidden; RunOnceId: "FwPair"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Satellite Discovery"""; Flags: runhidden; RunOnceId: "FwDisc"
; NOTE: we deliberately do NOT uninstall ViGEmBus here. Other apps
; (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.) share the driver and
; would silently break. Users who want to remove it should do so via
; Settings > Apps.

[Code]
// ============================================================================
//  OTA relaunch flag
//
//  When the running satellite.exe applies an in-app update, it spawns this
//  installer with `/VERYSILENT /OTA /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS`.
//  The /OTA switch tells us "the user is mid-session; relaunch the binary
//  for them even though this is a silent install". WantsOTARelaunch is
//  referenced by the second [Run] entry above.
// ============================================================================

function WantsOTARelaunch: Boolean;
var
    i: Integer;
    Up: String;
begin
    Result := False;
    for i := 1 to ParamCount do begin
        Up := Uppercase(ParamStr(i));
        if (Up = '/OTA') or (Up = '-OTA') then begin
            Result := True;
            Exit;
        end;
    end;
end;

// ============================================================================
//  ViGEmBus prerequisite handling
//
//  Auto-detect logic at wizard start:
//    detected == bundled    -> skip
//    detected <  bundled    -> upgrade
//    detected >  bundled    -> keep user's (do not downgrade)
//    detected == ''         -> install bundled
//
//  /VIGEM=skip and /VIGEM=bundled override the auto-decision. The user can
//  also uncheck the "ViGEmBus driver" component on the Components page,
//  which is equivalent to /VIGEM=skip for that run.
// ============================================================================

const
    NefariusRegKey = 'SOFTWARE\Nefarius Software Solutions\ViGEm Bus Driver';

var
    VigemMode: String;          // 'auto' | 'bundled' | 'skip'
    DetectedVigemVersion: String;
    RebootNeeded: Boolean;
    StatusLabel: TNewStaticText;

function ParseVigemSwitch: String;
var
    i: Integer;
    P, Up: String;
begin
    Result := 'auto';
    for i := 1 to ParamCount do begin
        P := ParamStr(i);
        Up := Uppercase(P);
        if Copy(Up, 1, 7) = '/VIGEM=' then
            Result := Lowercase(Trim(Copy(P, 8, MaxInt)));
    end;
    // Validate -- anything unrecognized falls back to 'auto'
    if (Result <> 'auto') and (Result <> 'bundled') and (Result <> 'skip') then
        Result := 'auto';
end;

function GetInstalledVigemVersion: String;
var
    V, SysPath: String;
begin
    Result := '';
    // The driver file is the source of truth -- registry remnants from a
    // half-uninstalled ViGEmBus must not mask a missing driver.
    SysPath := ExpandConstant('{sys}\drivers\ViGEmBus.sys');
    if not FileExists(SysPath) then
        Exit;
    if GetVersionNumbersString(SysPath, V) then
        Result := V
    else if RegQueryStringValue(HKEY_LOCAL_MACHINE, NefariusRegKey, 'Version', V) then
        Result := V;
end;

function CompareVigemVersion(Installed, Bundled: String): Integer;
var
    IP, BP: Int64;
begin
    if not StrToVersion(Installed, IP) then IP := 0;
    if not StrToVersion(Bundled, BP) then BP := 0;
    Result := ComparePackedVersion(IP, BP);
end;

// Returns True iff we should run the bundled ViGEmBus installer.
// Used both by the [Files] Check= (gates extraction) and the post-install
// step (gates execution). Both must agree.
function ShouldRunViGEm: Boolean;
var
    Cmp: Integer;
begin
    Result := False;
    if VigemMode = 'skip' then Exit;
    if not WizardIsComponentSelected('vigem') then Exit;
    if VigemMode = 'bundled' then begin
        Result := True;
        Exit;
    end;
    // 'auto' -- install only if missing or older than bundled
    if DetectedVigemVersion = '' then begin
        Result := True;
        Exit;
    end;
    Cmp := CompareVigemVersion(DetectedVigemVersion, '{#ViGEmBusVersion}');
    Result := (Cmp < 0);
end;

function StatusText: String;
var
    Cmp: Integer;
begin
    if DetectedVigemVersion = '' then
        Result := 'ViGEmBus not detected -- will install ' + '{#ViGEmBusVersion}'
    else begin
        Cmp := CompareVigemVersion(DetectedVigemVersion, '{#ViGEmBusVersion}');
        if Cmp < 0 then
            Result := 'ViGEmBus ' + DetectedVigemVersion + ' detected -- will upgrade to ' + '{#ViGEmBusVersion}'
        else if Cmp = 0 then
            Result := 'ViGEmBus ' + DetectedVigemVersion + ' already installed -- will skip'
        else
            Result := 'ViGEmBus ' + DetectedVigemVersion + ' detected (newer than bundled ' + '{#ViGEmBusVersion}' + ') -- keeping yours';
    end;

    if VigemMode = 'skip' then
        Result := Result + #13#10 + '(/VIGEM=skip -- driver will not be touched)'
    else if VigemMode = 'bundled' then
        Result := Result + #13#10 + '(/VIGEM=bundled -- bundled installer will run regardless)'
    else
        Result := Result + #13#10 + 'Uncheck the component above to skip.';
end;

procedure InitializeWizard;
begin
    VigemMode := ParseVigemSwitch;
    DetectedVigemVersion := GetInstalledVigemVersion;
    RebootNeeded := False;

    StatusLabel := TNewStaticText.Create(WizardForm);
    StatusLabel.Parent := WizardForm.SelectComponentsPage;
    StatusLabel.AutoSize := False;
    StatusLabel.WordWrap := True;
    StatusLabel.Left := WizardForm.ComponentsList.Left;
    StatusLabel.Width := WizardForm.ComponentsList.Width;
    StatusLabel.Top := WizardForm.ComponentsList.Top + WizardForm.ComponentsList.Height + ScaleY(8);
    StatusLabel.Height := ScaleY(40);
    StatusLabel.Caption := StatusText;
end;

procedure RunBundledViGEm;
var
    InstallerPath: String;
    ResultCode: Integer;
    Msg: String;
begin
    InstallerPath := ExpandConstant('{tmp}\{#ViGEmBusInstaller}');
    WizardForm.StatusLabel.Caption := 'Installing ViGEmBus driver (this can take a minute)...';
    WizardForm.FilenameLabel.Caption := '{#ViGEmBusInstaller}';

    if not Exec(InstallerPath, '/quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
        MsgBox('Could not launch the bundled ViGEmBus installer. Satellite ' +
               'will still install, but you must install ViGEmBus manually ' +
               'before connecting a sender.' + #13#10 + #13#10 +
               'Get it from: https://github.com/nefarius/ViGEmBus/releases',
               mbInformation, MB_OK);
        Exit;
    end;

    // Documented WixSharp Burn / MSI exit codes:
    //   0          - success
    //   1602       - user cancelled
    //   1603       - fatal error
    //   1638, 3011 - same/newer already installed (treat as success)
    //   1641       - reboot was initiated
    //   3010       - reboot required (deferred)
    case ResultCode of
        0, 1638, 3011:
            ; // success -- nothing to do
        1641, 3010:
            RebootNeeded := True;
        1602:
            MsgBox('ViGEmBus installation was cancelled. Install it manually ' +
                   'before using virtual gamepads, or re-run the Satellite ' +
                   'installer.', mbInformation, MB_OK);
        1603:
            MsgBox('ViGEmBus installation failed (fatal error). Check the ' +
                   'installer log under %TEMP% and install manually before ' +
                   'using virtual gamepads.', mbError, MB_OK);
        else begin
            Msg := 'ViGEmBus installer returned exit code ' + IntToStr(ResultCode) + '.' + #13#10 +
                   'Satellite will still install, but virtual gamepad output ' +
                   'may not work until ViGEmBus is installed manually.';
            MsgBox(Msg, mbError, MB_OK);
        end;
    end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
    if (CurStep = ssPostInstall) and ShouldRunViGEm then
        RunBundledViGEm;
end;

function NeedRestart: Boolean;
begin
    Result := RebootNeeded;
end;

// ============================================================================
//  Uninstall: optional ViGEmBus removal
//
//  ViGEmBus is intentionally an opt-in choice on uninstall: many apps
//  (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.) share the driver, and a
//  default-yes here would silently break them. Behavior:
//
//    /REMOVEVIGEM=auto  (default)  prompt the user, default No;
//                                  silent uninstalls do not remove the driver
//    /REMOVEVIGEM=yes              uninstall ViGEmBus
//    /REMOVEVIGEM=no               leave the driver in place
//
//  We locate the ViGEmBus uninstaller by scanning the standard
//  HKLM\...\Uninstall key for a DisplayName matching ViGEmBus, then exec
//  whatever QuietUninstallString it advertises. If the registry entry is
//  gone (rare -- usually means a half-uninstalled state), we surface a
//  friendly message and stop.
// ============================================================================

const
    UninstallKeyRoot = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';

function ParseRemoveVigemSwitch: String;
var
    i: Integer;
    P, Up: String;
begin
    Result := 'auto';
    for i := 1 to ParamCount do begin
        P := ParamStr(i);
        Up := Uppercase(P);
        if Copy(Up, 1, 13) = '/REMOVEVIGEM=' then
            Result := Lowercase(Trim(Copy(P, 14, MaxInt)));
    end;
    if (Result <> 'yes') and (Result <> 'no') then
        Result := 'auto';
end;

function FindVigemUninstallString: String;
var
    Names: TArrayOfString;
    i: Integer;
    DisplayName, UninstallStr, KeyPath: String;
begin
    Result := '';
    if not RegGetSubkeyNames(HKEY_LOCAL_MACHINE, UninstallKeyRoot, Names) then
        Exit;
    for i := 0 to GetArrayLength(Names) - 1 do begin
        KeyPath := UninstallKeyRoot + '\' + Names[i];
        if not RegQueryStringValue(HKEY_LOCAL_MACHINE, KeyPath, 'DisplayName', DisplayName) then
            Continue;
        if (Pos('ViGEm Bus Driver', DisplayName) = 0) and (Pos('ViGEmBus', DisplayName) = 0) then
            Continue;
        // Found it. Prefer QuietUninstallString; fall back to UninstallString
        // with our own silent flags appended.
        if RegQueryStringValue(HKEY_LOCAL_MACHINE, KeyPath, 'QuietUninstallString', UninstallStr) then begin
            Result := UninstallStr;
            Exit;
        end;
        if RegQueryStringValue(HKEY_LOCAL_MACHINE, KeyPath, 'UninstallString', UninstallStr) then begin
            Result := UninstallStr + ' /quiet /norestart';
            Exit;
        end;
    end;
end;

procedure SplitCmdLine(const CmdLine: String; var Exe, Args: String);
var
    i, Len: Integer;
begin
    Exe := '';
    Args := '';
    Len := Length(CmdLine);
    if Len = 0 then Exit;
    i := 1;
    while (i <= Len) and (CmdLine[i] = ' ') do Inc(i);
    if i > Len then Exit;
    if CmdLine[i] = '"' then begin
        Inc(i); // skip opening quote
        while (i <= Len) and (CmdLine[i] <> '"') do begin
            Exe := Exe + CmdLine[i];
            Inc(i);
        end;
        if (i <= Len) and (CmdLine[i] = '"') then Inc(i); // skip closing quote
    end else begin
        while (i <= Len) and (CmdLine[i] <> ' ') do begin
            Exe := Exe + CmdLine[i];
            Inc(i);
        end;
    end;
    while (i <= Len) and (CmdLine[i] = ' ') do Inc(i);
    if i <= Len then
        Args := Copy(CmdLine, i, MaxInt);
end;

procedure RunVigemUninstall;
var
    CmdLine, Exe, Args: String;
    ResultCode: Integer;
begin
    CmdLine := FindVigemUninstallString;
    if CmdLine = '' then begin
        if not UninstallSilent then
            MsgBox('Could not locate the ViGEmBus uninstaller in the registry.' + #13#10 +
                   'Remove it manually via Settings > Apps if you no longer need it.',
                   mbInformation, MB_OK);
        Exit;
    end;
    SplitCmdLine(CmdLine, Exe, Args);
    if Exe = '' then Exit;
    Exec(Exe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    // We deliberately do not surface ViGEmBus uninstaller exit codes: if the
    // user said "yes, remove it" and we tried, that is enough. Any persistent
    // failure surfaces in Settings > Apps where they can retry.
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
    Mode: String;
    ShouldRemove: Boolean;
begin
    if CurUninstallStep <> usPostUninstall then Exit;
    // Nothing to do if ViGEmBus is not installed in the first place.
    if not FileExists(ExpandConstant('{sys}\drivers\ViGEmBus.sys')) then Exit;

    Mode := ParseRemoveVigemSwitch;
    ShouldRemove := False;
    if Mode = 'yes' then
        ShouldRemove := True
    else if Mode = 'no' then
        ShouldRemove := False
    else if UninstallSilent then
        // Silent uninstall + no explicit /REMOVEVIGEM= => don't touch driver.
        ShouldRemove := False
    else
        ShouldRemove := MsgBox(
            'Also uninstall the ViGEmBus driver?' + #13#10 + #13#10 +
            'Other apps (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.) commonly ' +
            'share this driver. Removing it will break them until they ' +
            'reinstall it themselves.' + #13#10 + #13#10 +
            'Click No if you''re unsure -- you can always remove ViGEmBus later ' +
            'from Settings > Apps.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;

    if ShouldRemove then
        RunVigemUninstall;
end;
