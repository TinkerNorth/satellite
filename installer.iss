; ============================================================================
;  Satellite Inno Setup Installer Script
;  Requires: Inno Setup 6.2+ (https://jrsoftware.org/isinfo.php)
;
;  Build:  pwsh scripts/fetch-redist.ps1 ; iscc installer.iss
;  Output: dist\SatelliteSetup.exe
;
;  Notes:
;    * Authenticode signing wired in via SignTool=signtool (define a
;      signtool program with `iscc /Ssigntool=...` or your CI); see
;      scripts/sign.ps1 for the recipe and the registration line below.
;    * Restart Manager: if satellite.exe is running, it gets closed
;      cleanly (WM_QUERYENDSESSION) instead of taskkill /F so config
;      writes survive an in-place upgrade.
;    * AppMutex prevents two installers from racing each other.
;    * SetupLogging=yes preserves logs under %TEMP% so failed installs
;      can be diagnosed remotely.
;    * Autostart is OPT-IN (Flags: unchecked): users actively choose to
;      add the app to startup.
;    * OTA upgrades preserve the user's last autostart / desktop-icon
;      choices via GetPreviousData, instead of clobbering them.
;    * Firewall rules apply to private and domain profiles (public is
;      excluded).
;    * Per-machine install only (PrivilegesRequired=admin): gates
;      ViGEmBus + firewall behind one UAC consent at install time, then
;      the app itself runs asInvoker (see satellite.manifest).
;
;  ViGEmBus 1.22.0 (final upstream release, repo archived 2023-11) is
;  bundled as a prerequisite. It creates the virtual gamepads and (from
;  v1.17 on, so v1.22.0 included) carries controller motion (gyro /
;  accelerometer) to games via the DualShock 4 extended report. An older
;  ViGEmBus is therefore upgraded, not kept.
;
;  Switches:
;    /VIGEM=auto      (default)  install only if missing or older
;    /VIGEM=bundled              force install even over a newer version
;    /VIGEM=skip                 leave the driver alone entirely
;    /REMOVEVIGEM=yes|no|auto    uninstall-time companion (see bottom)
;    /OTA                        signals an in-app self-update; we relaunch
;                                the binary via Restart Manager (no UI)
; ============================================================================

#define MyAppName "Satellite"
; Read the authoritative version from /VERSION at preprocess time. Single
; source of truth shared with CMake + src/core/version.h.
#ifndef MyAppVersion
#define VersionFile FileOpen(SourcePath + "VERSION")
#define MyAppVersion Trim(FileRead(VersionFile))
#expr FileClose(VersionFile)
#undef VersionFile
#endif
#define MyAppPublisher "TinkerNorth"
#define MyAppURL "https://github.com/TinkerNorth/satellite"
#define MyAppExeName "satellite.exe"
#define MyAppCopyright "Copyright (c) 2026 TinkerNorth"

; --- Bundled ViGEmBus (see redist/README.md for vendoring notes) ---
#define ViGEmBusVersion "1.22.0"
#define ViGEmBusInstaller "ViGEmBus_1.22.0_x64_x86_arm64.exe"

[Setup]
; Stable AppId: never change this without a migration plan; it's how
; Windows recognises in-place upgrades.
AppId={{B8F3A2E1-7D4C-4E5F-9A1B-3C6D8E0F2A4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
AppCopyright={#MyAppCopyright}
AppContact={#MyAppURL}/issues
AppComments=Low-latency Xbox controller forwarding over the network. Tray app + web UI at http://localhost:9877.
AppReadmeFile={#MyAppURL}#readme

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=SatelliteSetup
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

; Admin is required to write to Program Files, add firewall rules, and
; install ViGEmBus, but the installed binary runs asInvoker (see
; satellite.manifest). One UAC prompt at install, none at runtime.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Embed version info into SatelliteSetup.exe (Properties > Details).
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright={#MyAppCopyright}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

; Restart Manager: if satellite.exe is running, ask the OS to close it
; gracefully (sends WM_QUERYENDSESSION; tray.cpp returns TRUE and saves
; config on WM_ENDSESSION). RestartApplications=yes brings the app back up
; after install finishes, replacing the hand-rolled /OTA relaunch.
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=yes
; AppMutex prevents two installer instances from racing. Must match nothing
; the app itself holds: the app's runtime singleton uses
; "Local\TinkerNorth.Satellite.Singleton.v1" (see app_lifecycle.cpp). This
; one is the installer's own coordinator.
AppMutex=Global\TinkerNorth.Satellite.Installer.v1

; Log every install/uninstall to %TEMP%\Setup Log YYYY-MM-DD #NNN.txt.
SetupLogging=yes

; PrivilegesRequired=admin combined with the HKCU autostart entry would
; normally trigger a "you're writing to HKCU as an elevated user"
; warning. We suppress it because:
;   (a) we ALSO self-heal at app launch via lifecycle::reconcileAutoStart,
;       which runs asInvoker and so always lands in the right hive; and
;   (b) the common case (user elevates themselves via UAC consent) puts
;       HKCU in the right place anyway. The OTS-elevation edge case is
;       covered by (a).
UsedUserAreasWarning=no

; Code signing (Authenticode). Activated only when iscc is invoked with
; /Ssigntool=... defining the named tool. Example:
;   iscc /Ssigntool=$qsigntool sign /a /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $f$q installer.iss
; scripts/sign.ps1 wraps that. If $signtool is not defined at iscc-time, the
; SignTool= line below is treated as documentation and the installer is built
; unsigned (which SmartScreen will warn about).
;SignTool=signtool
;SignedUninstaller=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main";  Description: "Satellite (required)"; Types: full custom; Flags: fixed
Name: "vigem"; Description: "ViGEmBus driver: virtual gamepads, rumble and controller motion (gyro/accelerometer)"; Types: full

[Tasks]
; Autostart is opt-in, matching the Settings > Apps > Startup model.
; ShouldShowAutostartTask hides the task on the OTA path so the user's prior
; choice (stored via SetPreviousData) is preserved.
Name: "autostart"; Description: "Start {#MyAppName} automatically when I sign in"; GroupDescription: "Startup:"; Flags: unchecked; Check: ShouldShowAutostartTask
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; Check: ShouldShowDesktopIconTask

[Files]
Source: "satellite.exe"; DestDir: "{app}"; Flags: ignoreversion sign; Components: main
Source: "web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: main
; LICENSE + README give Programs & Features something to link to and let
; the user open them from the Start Menu without internet.
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion; Components: main
; ViGEmBus prerequisite: only extracted when we'll run it (see ShouldRunViGEm).
Source: "redist\{#ViGEmBusInstaller}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Components: vigem; Check: ShouldRunViGEm

[Dirs]
; Pre-create the LocalAppData tree the app uses for dumps + logs, with
; the user's standard ACLs (no special permissions needed since the
; app runs asInvoker). Inno Setup's {localappdata} resolves to the
; user being installed-for, which is correct here.
Name: "{localappdata}\TinkerNorth\Satellite\dumps"; Flags: uninsneveruninstall
Name: "{localappdata}\TinkerNorth\Satellite\logs";  Flags: uninsneveruninstall

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Start {#MyAppName} (system tray app, web UI at http://localhost:9877)"
Name: "{group}\{#MyAppName} Web UI"; Filename: "http://localhost:9877"; Comment: "Open the Satellite web UI in your default browser"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Start {#MyAppName} (system tray app, web UI at http://localhost:9877)"; Tasks: desktopicon

[Registry]
; Autostart entry. Quoted path mitigates the classic C:\Program.exe
; planting attack. Value name "Satellite" matches the constant in
; src/platform/windows/config.cpp::kRunValueName: if you change one,
; change the other (and add a migration). uninsdeletevalue cleans it
; up on uninstall; the app's setAutoStart(false) cleans it up on
; user opt-out.
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Firewall rules apply to private + domain profiles. Public is excluded so
; the LAN-discovery beacon doesn't broadcast on untrusted Wi-Fi. The
; add-then-delete pattern keeps the rule list idempotent across upgrades.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite HTTP"""; Flags: runhidden; StatusMsg: "Resetting firewall (HTTP)..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Satellite HTTP"" dir=in action=allow protocol=TCP localport=9877 program=""{app}\{#MyAppExeName}"" profile=private,domain"; Flags: runhidden; StatusMsg: "Configuring firewall (HTTP)..."

Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite UDP"""; Flags: runhidden; StatusMsg: "Resetting firewall (UDP)..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Satellite UDP"" dir=in action=allow protocol=UDP localport=9876 program=""{app}\{#MyAppExeName}"" profile=private,domain"; Flags: runhidden; StatusMsg: "Configuring firewall (UDP)..."

; Protocol-0's plaintext pairing listener (TCP 9878) is gone — pairing rides
; the HTTPS client API (9443) — so no rule is ADDED for it anymore. The delete
; stays: upgrades over pre-protocol-1 installs must close the stale hole.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Pairing"""; Flags: runhidden; StatusMsg: "Removing stale pairing firewall rule..."

Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Discovery"""; Flags: runhidden; StatusMsg: "Resetting firewall (Discovery)..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Satellite Discovery"" dir=in action=allow protocol=UDP localport=9879 program=""{app}\{#MyAppExeName}"" profile=private,domain"; Flags: runhidden; StatusMsg: "Configuring firewall (Discovery)..."

Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite mDNS"""; Flags: runhidden; StatusMsg: "Resetting firewall (mDNS)..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Satellite mDNS"" dir=in action=allow protocol=UDP localport=5353 program=""{app}\{#MyAppExeName}"" profile=private,domain"; Flags: runhidden; StatusMsg: "Configuring firewall (mDNS discovery)..."

Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Client TLS"""; Flags: runhidden; StatusMsg: "Resetting firewall (Client TLS)..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Satellite Client TLS"" dir=in action=allow protocol=TCP localport=9443 program=""{app}\{#MyAppExeName}"" profile=private,domain"; Flags: runhidden; StatusMsg: "Configuring firewall (Client TLS)..."

; Interactive install: normal "Launch Satellite?" tickbox on the finish page.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: shellexec nowait postinstall skipifsilent
; Restart Manager handles the OTA relaunch automatically (we closed the app
; via RM at the start of the install, and RestartApplications=yes brings it
; back). The legacy /OTA explicit-relaunch path is kept only so older
; binaries that pass the switch aren't broken.

[UninstallRun]
; Best-effort graceful shutdown first (WM_CLOSE via taskkill), then
; force-kill as fallback after 3 seconds. Replaces the prior
; unconditional /F which skipped saveConfig.
Filename: "{sys}\taskkill.exe"; Parameters: "/IM {#MyAppExeName}"; Flags: runhidden; RunOnceId: "CloseSatellite"
Filename: "{sys}\timeout.exe"; Parameters: "/T 3 /NOBREAK"; Flags: runhidden; RunOnceId: "WaitSatellite"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden; RunOnceId: "ForceSatellite"

Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite HTTP"""; Flags: runhidden; RunOnceId: "FwHTTP"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite UDP"""; Flags: runhidden; RunOnceId: "FwUDP"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Pairing"""; Flags: runhidden; RunOnceId: "FwPair"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Discovery"""; Flags: runhidden; RunOnceId: "FwDisc"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite mDNS"""; Flags: runhidden; RunOnceId: "FwMDNS"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Satellite Client TLS"""; Flags: runhidden; RunOnceId: "FwClientTLS"
; Do NOT uninstall ViGEmBus here. Other apps (DS4Windows, BetterJoy,
; MoonDeck-Buddy, etc.) share the driver and would break. Users who want to
; remove it can opt in via the wizard's confirmation dialog (see
; CurUninstallStepChanged).

[UninstallDelete]
; Don't leave behind empty %APPDATA% / %LOCALAPPDATA% trees. Logs and
; dumps under LocalAppData stick around (uninsneveruninstall on [Dirs]) so
; a user reinstalling after a crash still has the dump file.
Type: files;          Name: "{userappdata}\satellite\config.json"
Type: dirifempty;     Name: "{userappdata}\satellite"

[Code]
// OTA relaunch flag (legacy, kept for backwards-compatibility).
// A pre-Restart-Manager satellite.exe spawns this installer with
// `/VERYSILENT /OTA`. RM handles the relaunch on modern builds, but the
// switch is still recognised so rolling out a new installer onto an old
// binary doesn't strand the user without a relaunch.

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

// Previous-install awareness.
// On upgrade (OTA or manual re-run), the "Start with Windows" and "Desktop
// icon" toggles preserve the prior choice instead of resetting to the
// unchecked default. Inno Setup gives two hooks:
//   1. RegisterPreviousData writes a string into the installer's metadata
//      under the AppId, persisted across runs.
//   2. GetPreviousData reads it back at the start of the next install.
// They remember which optional tasks the user enabled, then those tasks are
// hidden from the Tasks page on upgrade so the Flags: unchecked default
// doesn't silently reset them. The user can still toggle them from the app's
// UI / Programs & Features.

var
    PreviousAutostart: Boolean;
    PreviousDesktopIcon: Boolean;
    IsFirstInstall: Boolean;

function ShouldShowAutostartTask: Boolean;
begin
    // Show the autostart task ONLY on a first install. On upgrade we
    // honour the user's previous decision via [Registry] persistence
    // (the HKCU\Run value either still exists or doesn't).
    Result := IsFirstInstall;
end;

function ShouldShowDesktopIconTask: Boolean;
begin
    Result := IsFirstInstall;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
    SetPreviousData(PreviousDataKey, 'Autostart',
        IntToStr(Ord(WizardIsTaskSelected('autostart') or PreviousAutostart)));
    SetPreviousData(PreviousDataKey, 'DesktopIcon',
        IntToStr(Ord(WizardIsTaskSelected('desktopicon') or PreviousDesktopIcon)));
end;

// ViGEmBus prerequisite handling.
// Auto-detect logic at wizard start:
//    detected == bundled    -> skip
//    detected <  bundled    -> upgrade
//    detected >  bundled    -> keep user's (do not downgrade)
//    detected == ''         -> install bundled
// /VIGEM=skip and /VIGEM=bundled override the auto-decision. Unchecking the
// "ViGEmBus driver" component on the Components page is equivalent to
// /VIGEM=skip for that run.

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
    if (Result <> 'auto') and (Result <> 'bundled') and (Result <> 'skip') then
        Result := 'auto';
end;

function GetInstalledVigemVersion: String;
var
    V, SysPath: String;
begin
    Result := '';
    // The driver file is the source of truth: registry remnants from a
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
    // 'auto': install only if missing or older than bundled
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
    Detail: String;
begin
    Result := 'ViGEmBus is the kernel driver Satellite uses to create virtual'
            + ' gamepads on this PC. It also delivers rumble and controller'
            + ' motion (gyro / accelerometer) to your games.' + #13#10;

    if DetectedVigemVersion = '' then
        Detail := 'Not detected here. The bundled v' + '{#ViGEmBusVersion}' + ' will be installed.'
    else begin
        Cmp := CompareVigemVersion(DetectedVigemVersion, '{#ViGEmBusVersion}');
        if Cmp < 0 then
            Detail := 'v' + DetectedVigemVersion + ' detected. Will upgrade to v' + '{#ViGEmBusVersion}' + ' (older builds cannot pass controller motion).'
        else if Cmp = 0 then
            Detail := 'v' + DetectedVigemVersion + ' already installed. Will skip.'
        else
            Detail := 'v' + DetectedVigemVersion + ' detected, newer than the bundled v' + '{#ViGEmBusVersion}' + '. Keeping yours.';
    end;
    Result := Result + Detail;

    if VigemMode = 'skip' then
        Result := Result + #13#10 + 'Override active: /VIGEM=skip. The driver will not be touched.'
    else if VigemMode = 'bundled' then
        Result := Result + #13#10 + 'Override active: /VIGEM=bundled. The bundled installer runs regardless of version.'
    else
        Result := Result + #13#10 + 'Default: install or upgrade as above. To skip, uncheck the component above or pass /VIGEM=skip.';
end;

function InitializeSetup: Boolean;
var
    Existing: String;
begin
    // Detect prior install via the same AppId so we can hide the task page
    // knobs on upgrade (see ShouldShow*Task).
    Existing := '';
    RegQueryStringValue(HKEY_LOCAL_MACHINE,
        'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupSetting("AppId")}_is1',
        'DisplayVersion', Existing);
    IsFirstInstall := (Existing = '');

    // Read the persisted task choices so RegisterPreviousData can re-write them.
    PreviousAutostart := (GetPreviousData('Autostart', '0') = '1');
    PreviousDesktopIcon := (GetPreviousData('DesktopIcon', '0') = '1');
    Result := True;
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
    StatusLabel.Height := ScaleY(72);
    StatusLabel.Caption := StatusText;
end;

const
    VigemBusyCode = 1618;
    VigemMaxAttempts = 4;
    VigemRetryDelayMs = 8000;

procedure RunBundledViGEm;
var
    InstallerPath: String;
    ResultCode: Integer;
    Msg: String;
    Attempt: Integer;
begin
    InstallerPath := ExpandConstant('{tmp}\{#ViGEmBusInstaller}');
    WizardForm.StatusLabel.Caption := 'Installing ViGEmBus driver (this can take a minute)...';
    WizardForm.FilenameLabel.Caption := '{#ViGEmBusInstaller}';

    Attempt := 0;
    repeat
        Inc(Attempt);
        if not Exec(InstallerPath, '/quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
            MsgBox('Could not launch the bundled ViGEmBus installer. Satellite ' +
                   'will still install, but you must install ViGEmBus manually ' +
                   'before connecting a sender.' + #13#10 + #13#10 +
                   'Get it from: https://github.com/nefarius/ViGEmBus/releases',
                   mbInformation, MB_OK);
            Exit;
        end;
        if ResultCode <> VigemBusyCode then
            Break;
        if Attempt < VigemMaxAttempts then begin
            WizardForm.StatusLabel.Caption := 'Another Windows install is busy; waiting to install ViGEmBus...';
            Sleep(VigemRetryDelayMs);
        end;
    until Attempt >= VigemMaxAttempts;

    case ResultCode of
        0, 1638, 3011:
            ; // success
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
        VigemBusyCode:
            MsgBox('Another installation (often Windows Update) was still in ' +
                   'progress, so ViGEmBus could not be installed right now. ' +
                   'Let any pending updates finish, then re-run the Satellite ' +
                   'installer to add the driver.', mbInformation, MB_OK);
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

// Uninstall: optional ViGEmBus removal.
// Opt-in on uninstall: many apps (DS4Windows, BetterJoy, MoonDeck-Buddy,
// etc.) share the driver, and a default-yes would break them.
//    /REMOVEVIGEM=auto  (default)  prompt the user, default No;
//                                  silent uninstalls do not remove the driver
//    /REMOVEVIGEM=yes              uninstall ViGEmBus
//    /REMOVEVIGEM=no               leave the driver in place

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
        Inc(i);
        while (i <= Len) and (CmdLine[i] <> '"') do begin
            Exe := Exe + CmdLine[i];
            Inc(i);
        end;
        if (i <= Len) and (CmdLine[i] = '"') then Inc(i);
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
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
    Mode: String;
    ShouldRemove: Boolean;
begin
    if CurUninstallStep <> usPostUninstall then Exit;
    if not FileExists(ExpandConstant('{sys}\drivers\ViGEmBus.sys')) then Exit;

    Mode := ParseRemoveVigemSwitch;
    ShouldRemove := False;
    if Mode = 'yes' then
        ShouldRemove := True
    else if Mode = 'no' then
        ShouldRemove := False
    else if UninstallSilent then
        ShouldRemove := False
    else
        ShouldRemove := MsgBox(
            'Also uninstall the ViGEmBus driver?' + #13#10 + #13#10 +
            'Other apps (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.) commonly ' +
            'share this driver. Removing it will break them until they ' +
            'reinstall it themselves.' + #13#10 + #13#10 +
            'Click No if you''re unsure. You can always remove ViGEmBus later ' +
            'from Settings > Apps.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;

    if ShouldRemove then
        RunVigemUninstall;
end;
