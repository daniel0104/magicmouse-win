; Inno Setup script for MagicMouseFlt. Built by .github/workflows/build.yml:
;   ISCC /DAppVersion=1.0.0 /DStageDir=<dir with x64\ and arm64\ driver packages> MagicMouseFlt.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\stage"
#endif

[Setup]
AppId={{6E3B7A41-2C1D-4F7B-9B2E-4D6A1C9F0A51}
AppName=Magic Mouse Driver
AppVersion={#AppVersion}
AppPublisher=MagicMouseFlt (open source)
DefaultDirName={autopf}\MagicMouseFlt
DefaultGroupName=Magic Mouse
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.16299
OutputDir=output
OutputBaseFilename=MagicMouseDriver-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Magic Mouse Driver
SetupLogging=yes

[Messages]
WelcomeLabel2=This installs a driver that gives your Apple Magic Mouse scrolling (vertical and horizontal), momentum and a middle click on Windows.%n%nPair the mouse in Bluetooth settings before you continue.%n%nThe driver isn't signed by Microsoft, so Setup turns on Windows test-signing mode. Windows then shows "Test Mode" in a corner of the desktop. You'll need to restart at the end.

[Files]
Source: "{#StageDir}\x64\*";   DestDir: "{app}\driver"; Flags: ignoreversion; Check: not IsArm64
Source: "{#StageDir}\arm64\*"; DestDir: "{app}\driver"; Flags: ignoreversion skipifsourcedoesntexist; Check: IsArm64
Source: "..\tools\Install.ps1";               DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\Uninstall.ps1";             DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\MagicMouseSettings.ps1";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\Set-MagicMouseOptions.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";                       DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Magic Mouse Settings"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\MagicMouseSettings.ps1"""; \
  IconFilename: "{sys}\main.cpl"
Name: "{group}\Repair Magic Mouse Driver (after re-pairing)"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Install.ps1"" -PackageDir ""{app}\driver"" -StateDir ""{app}"" -ShowResult"
Name: "{group}\Uninstall Magic Mouse Driver"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\MagicMouseSettings.ps1"""; \
  Description: "Open Magic Mouse settings"; Flags: postinstall nowait skipifsilent unchecked; Check: not NeedRestart

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Uninstall.ps1"" -StateDir ""{app}"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveMagicMouseDriver"

[UninstallDelete]
Type: files; Name: "{app}\install.log"
Type: files; Name: "{app}\testsigning-enabled.flag"

[Code]
var
  RebootNeeded: Boolean;
  UninstallReboot: Boolean;

function PowerShell(): String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  ExtractTemporaryFile('Install.ps1');
  if Exec(PowerShell(), '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\Install.ps1') + '" -CheckOnly',
          '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 2) then
    Result := 'Secure Boot is turned on, so Windows will not load this driver.' + #13#10 + #13#10 +
              'Turn off Secure Boot in your PC''s firmware (UEFI/BIOS) settings: restart, open the firmware ' +
              'setup (usually F2, F10, F12 or Del during start-up, or Settings > System > Recovery > Advanced ' +
              'start-up > UEFI Firmware Settings), find Secure Boot, set it to Disabled, save and restart. ' +
              'Then run this setup again.' + #13#10 + #13#10 +
              'If your drive uses BitLocker, have your recovery key ready first.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Params: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

  WizardForm.StatusLabel.Caption := 'Installing the Magic Mouse driver...';
  Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\Install.ps1') + '"' +
            ' -PackageDir "' + ExpandConstant('{app}\driver') + '" -StateDir "' + ExpandConstant('{app}') + '"';
  if not Exec(PowerShell(), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := 1;

  case Code of
    0: ;
    3010: RebootNeeded := True;
    2: MsgBox('Test-signing mode could not be turned on (Secure Boot or a company policy is blocking it). ' +
              'The driver is copied but will not load. Turn off Secure Boot and choose ' +
              '"Repair Magic Mouse Driver" from the Start menu.', mbError, MB_OK);
    4: MsgBox('The driver is installed, but no paired Magic Mouse was found.' + #13#10 + #13#10 +
              'Pair the mouse in Settings > Bluetooth & devices, then choose "Repair Magic Mouse Driver" ' +
              'from the Start menu.', mbInformation, MB_OK);
  else
    MsgBox('Driver setup reported an error (code ' + IntToStr(Code) + ').' + #13#10 +
           'Details are in ' + ExpandConstant('{app}\install.log'), mbError, MB_OK);
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := RebootNeeded;
end;

function InitializeUninstall(): Boolean;
begin
  UninstallReboot := FileExists(ExpandConstant('{app}\testsigning-enabled.flag'));
  Result := True;
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := UninstallReboot;
end;
