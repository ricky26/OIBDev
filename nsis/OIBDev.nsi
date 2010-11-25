!include "x64.nsh"
!include "MUI2.nsh"
!include "EnvVarUpdate.nsh"

Name "OpeniBoot Tools"
OutFile "OIBDev.exe"
InstallDir "$PROGRAMFILES\OpeniBoot Tools"
RequestExecutionLevel admin

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "../LICENSE.txt"
;!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
;!insertmacro MUI_UNPAGE_WELCOME
;!insertmacro MUI_UNPAGE_CONFIRM
;!insertmacro MUI_UNPAGE_INSTFILES
;!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Function SetupPaths
	Var /GLOBAL "DPINST"
	Var /GLOBAL "DPATH"
	Var /GLOBAL "DUPATH"
	
	${If} ${RunningX64}
		StrCpy "$DPINST" "$INSTDIR\dpinst64.exe"
	${Else}
		StrCpy "$DPINST" "$INSTDIR\dpinst.exe"
	${Endif}
	
	GetVersion::WindowsName
	Pop $0
	StrCpy "$DPATH" "$INSTDIR\Windows $0"
	StrCpy "$DUPATH" "$DPATH\oibdev.inf"
FunctionEnd

Function un.SetupPaths
	${If} ${RunningX64}
		StrCpy "$DPINST" "$INSTDIR\dpinst64.exe"
	${Else}
		StrCpy "$DPINST" "$INSTDIR\dpinst.exe"
	${Endif}
	
	GetVersion::WindowsName
	Pop $0
	StrCpy "$DPATH" "$INSTDIR\Windows $0"
	StrCpy "$DUPATH" "$DPATH\oibdev.inf"
FunctionEnd

Section "Tools" SecTools
	SetOutPath "$INSTDIR"

	File /r  /x "*.zip" "..\release\*.*"
	Call SetupPaths
	ExecWait '"$INSTDIR\vcredist10_x86.exe" /q'
	ExecWait '"$DPINST" /c /q /sa /sw /PATH "$DPATH"'
	ExecWait '"$DPINST" /c /q /sa /sw /PATH "$INSTDIR\ADB"'

	${EnvVarUpdate} $0 "PATH" "A" "HKLM" "$INSTDIR" 
	WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
	Call un.SetupPaths
	ExecWait '"$DPINST" /c /q /sa /sw /u "$INSTDIR\ADB\android_winusb.inf"'
	ExecWait '"$DPINST" /c /q /sa /sw /u "$DUPATH"'

	${un.EnvVarUpdate} $0 "PATH" "R" "HKLM" "$INSTDIR"
	Delete "$INSTDIR\Uninstall.exe"
	RMDir /r "$INSTDIR"
SectionEnd
