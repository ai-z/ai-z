#define AppName "ai-z"
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

[Setup]
AppId={{9A4E9F4C-CC9A-4B58-B7C6-87CE1A0A8D7C}
AppName={#AppName}
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
Name: "{autoprograms}\\{#AppName}"; Filename: "{app}\\{#AppExeName}"
Name: "{autodesktop}\\{#AppName}"; Filename: "{app}\\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\\{#AppExeName}"; Description: "Run {#AppName}"; Flags: nowait postinstall skipifsilent
