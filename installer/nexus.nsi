; NEXUS installer (NSIS 3). Build: makensis -DSRCDIR=<dir with NEXUS.exe> -DOUTFILE=<setup.exe> installer/nexus.nsi
Target amd64-unicode ; native 64-bit installer (NEXUS is x64-only)
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.9.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "..\build\Release"
!endif
!ifndef OUTFILE
  !define OUTFILE "NEXUS-${VERSION}-setup-x64.exe"
!endif

!include "MUI2.nsh"
!include "x64.nsh"

Name "NEXUS ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\NEXUS"
InstallDirRegKey HKLM "Software\NEXUS" "InstallDir"
BrandingText "NEXUS ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "NEXUS"
VIAddVersionKey "FileDescription" "NEXUS installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "..\res\nexus.ico"
!define MUI_UNICON "..\res\nexus.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\NEXUS.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start NEXUS"

!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NEXUS"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "NEXUS requires 64-bit Windows 10 or Windows 11."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Section "NEXUS (required)" SecMain
  SectionIn RO
  ; Close a running copy so the executable can be replaced.
  nsExec::Exec 'taskkill /IM NEXUS.exe'
  SetOutPath "$INSTDIR"
  File "${SRCDIR}\NEXUS.exe"
  File "..\LICENSE"
  File /oname=README.txt "..\docs\USER_GUIDE.md"
  WriteRegStr HKLM "Software\NEXUS" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "NEXUS"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "NEXUS Project"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\NEXUS.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 4096
SectionEnd

Section "Start menu shortcut" SecStartMenu
  SetShellVarContext all
  CreateShortCut "$SMPROGRAMS\NEXUS.lnk" "$INSTDIR\NEXUS.exe" "" "$INSTDIR\NEXUS.exe" 0
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  SetShellVarContext all
  CreateShortCut "$DESKTOP\NEXUS.lnk" "$INSTDIR\NEXUS.exe" "" "$INSTDIR\NEXUS.exe" 0
SectionEnd

Section /o "Use NEXUS instead of Task Manager" SecReplace
  ; Documented Image File Execution Options "Debugger" redirection (the same mechanism Process Explorer uses).
  WriteRegStr HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe" "Debugger" '"$INSTDIR\NEXUS.exe"'
SectionEnd

LangString DESC_Main ${LANG_ENGLISH} "The NEXUS application."
LangString DESC_Replace ${LANG_ENGLISH} "Open NEXUS when Task Manager is requested (Ctrl+Shift+Esc). Can be turned off in NEXUS Settings or by uninstalling."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecReplace} $(DESC_Replace)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetRegView 64
FunctionEnd

Section "un.NEXUS" UnSecMain
  SectionIn RO
  nsExec::Exec 'taskkill /IM NEXUS.exe'
  ; Restore Task Manager only if it currently points at NEXUS.
  ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe" "Debugger"
  StrCmp $0 "" +3
  StrCmp $0 '"$INSTDIR\NEXUS.exe"' 0 +2
  DeleteRegValue HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe" "Debugger"
  ; Remove the per-user "Start with Windows" entry of the installing user.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NEXUS"
  SetShellVarContext all
  Delete "$SMPROGRAMS\NEXUS.lnk"
  Delete "$DESKTOP\NEXUS.lnk"
  Delete "$INSTDIR\NEXUS.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\NEXUS"
SectionEnd

Section /o "un.Remove my NEXUS settings and recordings" UnSecData
  SetShellVarContext current
  RMDir /r "$APPDATA\NEXUS"
  RMDir /r "$LOCALAPPDATA\NEXUS"
SectionEnd
