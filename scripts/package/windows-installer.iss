; Sp3ctra Windows installer — Standalone (Program Files) + VST3 (Common Files\VST3),
; each selectable. Compiled in CI by ISCC (preinstalled on the GitHub runner):
;   ISCC /DAppVersion=x.y.z /DArtefactsDir=<...\Release> /DRepoDir=<repo> ^
;        /DOutDir=<dir> /DOutName=<basename-without-.exe> windows-installer.iss
; Section entries must stay on one line (no line continuation in .iss).

[Setup]
AppId={{8DEDC6A1-DB8A-4878-B4F1-754429AF27A8}
AppName=Sp3ctra
AppVersion={#AppVersion}
AppPublisher=Ondulab
AppPublisherURL=https://www.ondulab.com
DefaultDirName={autopf}\Sp3ctra
DisableProgramGroupPage=yes
LicenseFile={#RepoDir}\LICENSE
OutputDir={#OutDir}
OutputBaseFilename={#OutName}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "standalone"; Description: "Standalone application"; Types: full
Name: "vst3"; Description: "VST3 plug-in"; Types: full

[Files]
Source: "{#ArtefactsDir}\Standalone\Sp3ctra.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#ArtefactsDir}\Standalone\Resources\*"; DestDir: "{app}\Resources"; Components: standalone; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ArtefactsDir}\VST3\Sp3ctra.vst3\*"; DestDir: "{commoncf64}\VST3\Sp3ctra.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Sp3ctra"; Filename: "{app}\Sp3ctra.exe"; Components: standalone

[Run]
Filename: "{app}\Sp3ctra.exe"; Description: "Launch Sp3ctra"; Flags: nowait postinstall skipifsilent; Components: standalone
