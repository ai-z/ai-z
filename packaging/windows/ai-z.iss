#define AppName "ai-z"
#define AppDisplayName "AI-Z"
#define AppPublisher "ai-z"
#define AppURL "https://www.ai-z.org/"
#define AppExeName "ai-z.exe"

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#ifndef SourceDir
  #define SourceDir "."
#endif

#ifndef OutputDir
  #define OutputDir "."
#endif

#ifndef OutputBaseFilename
  #define OutputBaseFilename "ai-z-setup"
#endif

#ifndef IconFile
  #define IconFile ""
#endif

[Setup]
AppId={{9A4E9F4C-CC9A-4B58-B7C6-87CE1A0A8D7C}
AppName={#AppDisplayName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
#if IconFile != ""
SetupIconFile={#IconFile}
#endif
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Use Windows Terminal (wt.exe) to launch ai-z for better terminal support
; Falls back to direct execution if wt.exe is not found
; IconFilename uses the embedded icon from the exe
Name: "{autoprograms}\\{#AppDisplayName}"; Filename: "{localappdata}\\Microsoft\\WindowsApps\\wt.exe"; Parameters: "-d ""{app}"" -- ""{app}\\{#AppExeName}"""; IconFilename: "{app}\\{#AppExeName}"; Check: WindowsTerminalExists
Name: "{autoprograms}\\{#AppDisplayName}"; Filename: "{app}\\{#AppExeName}"; IconFilename: "{app}\\{#AppExeName}"; Check: not WindowsTerminalExists
Name: "{autodesktop}\\{#AppDisplayName}"; Filename: "{localappdata}\\Microsoft\\WindowsApps\\wt.exe"; Parameters: "-d ""{app}"" -- ""{app}\\{#AppExeName}"""; IconFilename: "{app}\\{#AppExeName}"; Tasks: desktopicon; Check: WindowsTerminalExists
Name: "{autodesktop}\\{#AppDisplayName}"; Filename: "{app}\\{#AppExeName}"; IconFilename: "{app}\\{#AppExeName}"; Tasks: desktopicon; Check: not WindowsTerminalExists

[Run]
; Post-install run: prefer Windows Terminal if available
Filename: "{localappdata}\\Microsoft\\WindowsApps\\wt.exe"; Parameters: "-d ""{app}"" -- ""{app}\\{#AppExeName}"""; Description: "Run {#AppDisplayName}"; Flags: nowait postinstall skipifsilent; Check: WindowsTerminalExists
Filename: "{app}\\{#AppExeName}"; Description: "Run {#AppDisplayName}"; Flags: nowait postinstall skipifsilent; Check: not WindowsTerminalExists

[Code]
function WindowsTerminalExists: Boolean;
var
  WTPath: String;
begin
  WTPath := ExpandConstant('{localappdata}\Microsoft\WindowsApps\wt.exe');
  Result := FileExists(WTPath);
end;
