; PhoneCam installer (Phase 5). Built with Inno Setup (ISCC.exe) -- see docs/build.md for the
; exact command and prerequisites for both source-side and installer testing.
;
; What this installs and why, matching the pieces each already has a working implementation of
; (this script wires them together, it doesn't reimplement any of them):
;   - phonecam-host.exe / phonecam-svc.exe -- built via CMake, Release config, static CRT (no
;     VC++ redistributable dependency -- see windows/CMakeLists.txt's CMAKE_MSVC_RUNTIME_LIBRARY).
;   - phonecam-vcam.dll -- built via its own MSBuild project (windows/vcam), installed to
;     C:\ProgramData\PhoneCam, the SAME path PhoneCamVCam.vcxproj's PostBuildEvent already
;     refreshes on every dev build -- keep these in sync; two different "the registered copy"
;     paths is exactly the bug that PostBuildEvent was added to fix (see docs/architecture.md's
;     Phase 5 section).
;   - adb.exe + its two DLLs (third_party/platform-tools, vendored, not a submodule -- see that
;     directory) -- bundled next to phonecam-host.exe. No code change was needed for this to work:
;     CreateProcessW's search order (used by AdbTransport.cpp / ControlTransport.cpp's
;     RunAdbForward) already checks the calling process's own directory before PATH, so a bundled
;     adb.exe is found automatically once phonecam-host.exe runs from {app}.
;   - phonecam-svc.exe --install / --uninstall -- already implemented (windows/svc/main.cpp,
;     commit af36ed3); this script just calls it, both directions.
;   - regsvr32 register/unregister of the vcam DLL, both directions.
;
; NOT verified by this script or by running it once: "clean install on a fresh Windows account"
; (Phase 5's actual roadmap exit criterion) -- only verifiable on hardware this session doesn't
; have. What WAS verified here: the installer builds, installs, the service installs/starts, the
; DLL registers, and the virtual camera is visible via DirectShow after an uninstall-then-install
; cycle on this machine (not just re-blessing already-installed state). See docs/build.md.

#define MyAppName "PhoneCam"
#define MyAppVersion "0.5.0"
#define MyAppPublisher "PhoneCam (open source)"
#define MyAppURL "https://github.com/abdoeid/phonecam"

[Setup]
AppId={{04890596-CE0C-4212-9FD1-B5D6CACF020D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\PhoneCam
DefaultGroupName=PhoneCam
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=Output
OutputBaseFilename=PhoneCamSetup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
; Needs admin: writing to ProgramData for the vcam DLL, registering it via regsvr32 (HKLM), and
; installing/uninstalling the LocalSystem service (windows/svc) all require elevation.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "..\build\host\Release\phonecam-host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\svc\Release\phonecam-svc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\third_party\platform-tools\adb.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\third_party\platform-tools\AdbWinApi.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\third_party\platform-tools\AdbWinUsbApi.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\third_party\platform-tools\NOTICE.txt"; DestDir: "{app}"; DestName: "adb-NOTICE.txt"; Flags: ignoreversion
; Deliberately NOT {app} -- must match PhoneCamVCam.vcxproj's PostBuildEvent path exactly (see the
; header comment above). Two locations both claiming to be "the registered copy" is the two-copies
; deployment bug this project already hit once.
Source: "..\vcam\x64\Release\phonecam-vcam.dll"; DestDir: "{commonappdata}\PhoneCam"; Flags: ignoreversion

[Icons]
Name: "{group}\PhoneCam"; Filename: "{app}\phonecam-host.exe"
Name: "{group}\Uninstall PhoneCam"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\phonecam-svc.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated; StatusMsg: "Installing PhoneCam background service..."
Filename: "regsvr32.exe"; Parameters: "/s ""{commonappdata}\PhoneCam\phonecam-vcam.dll"""; Flags: runhidden waituntilterminated; StatusMsg: "Registering the PhoneCam virtual camera..."

[UninstallRun]
; Inno runs [UninstallRun] entries before removing [Files]/[UninstallDelete] entries, so both
; executables invoked here still exist on disk at this point.
Filename: "regsvr32.exe"; Parameters: "/s /u ""{commonappdata}\PhoneCam\phonecam-vcam.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterVCam"
Filename: "{app}\phonecam-svc.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UninstallService"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\PhoneCam"
