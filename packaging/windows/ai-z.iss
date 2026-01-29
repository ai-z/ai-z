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
Name: "{autoprograms}\\{#AppDisplayName}"; Filename: "{app}\\{#AppExeName}"
Name: "{autodesktop}\\{#AppDisplayName}"; Filename: "{app}\\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\\{#AppExeName}"; Description: "Run {#AppDisplayName}"; Flags: nowait postinstall skipifsilent
