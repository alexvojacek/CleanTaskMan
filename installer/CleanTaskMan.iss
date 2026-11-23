#ifndef AppVersion
#error AppVersion is required
#endif
#ifndef AppExecutable
#error AppExecutable is required
#endif
#ifndef AppReadme
#error AppReadme is required
#endif
#ifndef AppLicense
#error AppLicense is required
#endif

[Setup]
AppId={{DC6C028D-1125-4270-8371-23CD046CD9FF}
AppName=Clean Task Manager
AppVersion={#AppVersion}
AppPublisher=CleanTaskMan
DefaultDirName={localappdata}\Programs\CleanTaskMan
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
UninstallDisplayIcon={app}\CleanTaskMan.exe
CloseApplications=yes
RestartApplications=no
OutputBaseFilename=CleanTaskMan-{#AppVersion}-win-x64-setup
WizardStyle=modern
Compression=lzma2

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#AppExecutable}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppReadme}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppLicense}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Clean Task Manager"; Filename: "{app}\CleanTaskMan.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Clean Task Manager"; Filename: "{app}\CleanTaskMan.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Code]
function InitializeUninstall(): Boolean;
var
  Debugger: String;
  InstalledCommand: String;
begin
  Result := True;
  if RegQueryStringValue(HKLM64,
    'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe',
    'Debugger', Debugger) then begin
    InstalledCommand := '"' + ExpandConstant('{app}\CleanTaskMan.exe') + '" --task-manager-replacement';
    if CompareText(Debugger, InstalledCommand) = 0 then begin
      SuppressibleMsgBox('Turn off Replace Task Manager in this copy''s Settings before uninstalling.',
        mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;
