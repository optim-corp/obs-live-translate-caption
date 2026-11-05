; Inno Setup script for OBS Live Translate Caption.
; Compiled in CI with:  ISCC /DMyAppVersion=<version> installer\obs-live-translate-caption.iss
; Installs the plugin DLL into the detected OBS Studio install directory.
; Modified by OPTiM Corporation on 2026-11-05:
; Updated repository and support URLs for this fork.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppName "OBS Live Translate Caption"
#define MyAppPublisher "plan12be"
#define MyAppURL "https://github.com/optim-corp/obs-live-translate-caption"

[Setup]
; Keep AppId stable across versions so upgrades/uninstall work.
AppId={{6D2B9E41-7C3A-4F58-B1E6-2A9C0D4F7E13}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
DefaultDirName={code:GetOBSDir}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
OutputBaseFilename=obs-live-translate-caption-{#MyAppVersion}-windows-x64-installer
; Use Restart Manager to detect/close OBS if it has the DLL open.
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\build_x64\RelWithDebInfo\obs-live-translate-caption.dll"; \
  DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; Locale files (data/locale/*.ini); OBS looks them up under data/obs-plugins/<module>.
Source: "..\data\*"; DestDir: "{app}\data\obs-plugins\obs-live-translate-caption"; \
  Flags: recursesubdirs ignoreversion

[InstallDelete]
; The original speech-only plugin (obs-live-translate) uses different source ids,
; so it would not collide, but its filter would linger in the Filters list next
; to ours and confuse users. Remove it on install.
Type: files; Name: "{app}\obs-plugins\64bit\obs-live-translate.dll"
Type: files; Name: "{app}\obs-plugins\64bit\obs-live-translate.pdb"

[Code]
{ Resolve the OBS Studio install directory from the registry written by OBS's
  own installer; fall back to the default Program Files location. }
function GetOBSDir(Param: String): String;
var
  Path: String;
begin
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') then
    Result := Path
  else
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;
