#ifndef ProductId
  #define ProductId "MCWiiU-Launcher"
#endif
#ifndef ProductName
  #define ProductName "MCWiiU Launcher"
#endif
#ifndef StableAppId
  #define StableAppId "{362BA210-0BBE-45ED-A0F5-6000DB57594B}"
#endif
#ifndef FoundationVersion
  #define FoundationVersion "1.0.2"
#endif
#ifndef LauncherVersion
  #define LauncherVersion "0.4.0"
#endif

[Setup]
AppId={#"{" + StableAppId}
AppName={#ProductName}
AppVersion={#FoundationVersion}
AppPublisher=IGNSeed
VersionInfoProductName={#ProductName}
VersionInfoProductVersion={#FoundationVersion}
DefaultDirName={localappdata}\Programs\{#ProductId}
DefaultGroupName={#ProductName}
PrivilegesRequired=lowest
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=yes
UsePreviousTasks=yes
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
ChangesAssociations=no
UninstallDisplayIcon={app}\MCWiiU-Launcher.exe
UninstallDisplayName={#ProductName}
OutputBaseFilename={#ProductId}-{#LauncherVersion}-Setup-x64
OutputDir={#OutputPath}
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\app-icon.ico

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#BinaryPath}\MCWiiU-Launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinaryPath}\MCWiiU-Updater.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinaryPath}\MCWiiU-SetupGuard.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BundlePath}\foundation.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BundlePath}\foundation.sig"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BundlePath}\manifest.json"; Flags: dontcopy
Source: "{#BundlePath}\manifest.sig"; Flags: dontcopy
Source: "{#BundlePath}\{#PackageName}"; Flags: dontcopy
Source: "{#BinaryPath}\MCWiiU-SetupGuard.dll"; Flags: dontcopy
Source: "{#BinaryPath}\MCWiiU-Launcher.exe"; Flags: dontcopy
Source: "{#BinaryPath}\MCWiiU-Updater.exe"; Flags: dontcopy
Source: "{#BundlePath}\foundation.json"; Flags: dontcopy
Source: "{#BundlePath}\foundation.sig"; Flags: dontcopy

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\MCWiiU-Launcher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\{#ProductName}"; Filename: "{app}\MCWiiU-Launcher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Code]
function HasWebView2: Boolean;
var Version, Key: String;
begin
  Key := 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  Result := (RegQueryStringValue(HKCU32, Key, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0')) or
    (RegQueryStringValue(HKLM32, Key, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0')) or
    (RegQueryStringValue(HKLM64, Key, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0'));
end;

function ValidateDestination(Path: String): Integer;
  external 'ValidateDestination@files:MCWiiU-SetupGuard.dll stdcall setuponly';
function FinishInstallation(Path, Source: String): Integer;
  external 'FinishInstallation@files:MCWiiU-SetupGuard.dll stdcall setuponly';
function ValidateBundle(Source: String): Integer;
  external 'ValidateBundle@files:MCWiiU-SetupGuard.dll stdcall setuponly';
function PrepareRemoval(Path: String): Integer;
  external 'PrepareRemoval@{tmp}\MCWiiU-RemovalGuard.dll stdcall uninstallonly delayload';
function RemoveVersions(Path: String): Integer;
  external 'RemoveVersions@{tmp}\MCWiiU-RemovalGuard.dll stdcall uninstallonly delayload';

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Result := ValidateDestination(WizardDirValue) = 0;
    if not Result then SuppressibleMsgBox('This location is unsafe, belongs to another application, or has an unfinished update. Choose an empty private location or repair the existing installation.', mbError, MB_OK, IDOK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not HasWebView2 then
    Result := 'Install the Microsoft Edge WebView2 Evergreen Runtime before installing the launcher.'
  else if ValidateDestination(ExpandConstant('{app}')) <> 0 then
    Result := 'Installation stopped. Close the launcher normally and verify the existing installation and destination.'
  else begin
    ExtractTemporaryFile('manifest.json');
    ExtractTemporaryFile('manifest.sig');
    ExtractTemporaryFile('{#PackageName}');
    ExtractTemporaryFile('foundation.json');
    ExtractTemporaryFile('foundation.sig');
    ExtractTemporaryFile('MCWiiU-Launcher.exe');
    ExtractTemporaryFile('MCWiiU-Updater.exe');
    if ValidateBundle(ExpandConstant('{tmp}')) <> 0 then
      Result := 'The signed program bundle could not be verified. Nothing will be installed.';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var ExitCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    ExtractTemporaryFile('manifest.json');
    ExtractTemporaryFile('manifest.sig');
    ExtractTemporaryFile('{#PackageName}');
    if FinishInstallation(ExpandConstant('{app}'), ExpandConstant('{tmp}')) <> 0 then
      RaiseException('Program verification failed. Keep existing user data and repair with a verified Setup.');
    if ExpandConstant('{param:NOLAUNCH|0}') <> '1' then begin
      if not Exec(ExpandConstant('{app}\MCWiiU-Launcher.exe'), '', ExpandConstant('{app}'), SW_SHOWNORMAL, ewWaitUntilTerminated, ExitCode) then
        RaiseException('Cannot start the verified bootstrapper.');
      if ExitCode <> 0 then RaiseException('Launcher readiness failed. Use a verified Setup for manual recovery.');
    end;
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := CopyFile(ExpandConstant('{app}\MCWiiU-SetupGuard.dll'), ExpandConstant('{tmp}\MCWiiU-RemovalGuard.dll'), True);
  if not Result then begin
    SuppressibleMsgBox('Cannot prepare the installation verifier. Keep user data and repair with a verified Setup.', mbError, MB_OK, IDOK);
    Exit;
  end;
  Result := PrepareRemoval(ExpandConstant('{app}')) = 0;
  if not Result then SuppressibleMsgBox('Close the launcher normally and recover any unfinished update before uninstalling. User data will be retained.', mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    if RemoveVersions(ExpandConstant('{app}')) <> 0 then
      RaiseException('Uninstall stopped to preserve unknown or unverified files. Keep user data and inspect the installation.');
    UnloadDLL(ExpandConstant('{tmp}\MCWiiU-RemovalGuard.dll'));
  end;
end;
