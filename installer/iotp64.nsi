; NSIS installer script for iotp

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

; For environment variable code
!include "WinMessages.nsh"
!define env_hklm 'HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"'

Name "Luomi iotp"
!define VERSION 2.0.23
OutFile "iotp-${VERSION}-install-windows-x64.exe"

!include "x64.nsh"
InstallDir "$PROGRAMFILES64\iotp"

;--------------------------------
; Installer pages
!insertmacro MUI_PAGE_WELCOME

!insertmacro MUI_PAGE_COMPONENTS
Page Custom ShowDbPath LeaveDbPath
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH


;--------------------------------
; Uninstaller pages
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

;--------------------------------
; Languages
!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer sections
Var TextDataPath
Section "Files" SecInstall
	SectionIn RO

	ExecWait 'sc stop mosquitto'
	Sleep 1000

	SetOutPath "$INSTDIR"
	File "..\logo\iotp.ico"
	File "..\build\src\Debug\mosquitto.exe"
	File "..\build\src\Debug\cjson.dll"
	File "..\build\src\Debug\libcrypto-3-x64.dll"
	File "..\build\src\Debug\libssl-3-x64.dll"
	File "..\build\src\Debug\websockets.dll"
	File "..\build\src\Debug\sqlite3.dll"
	
	;File "..\build64\apps\mosquitto_ctrl\Release\mosquitto_ctrl.exe"
	;File "..\build64\apps\mosquitto_passwd\Release\mosquitto_passwd.exe"
	;File "..\build64\client\Release\mosquitto_pub.exe"
	;File "..\build64\client\Release\mosquitto_sub.exe"
	;File "..\build64\client\Release\mosquitto_rr.exe"
	;File "..\build64\lib\Release\mosquitto.dll"
	;File "..\build64\lib\cpp\Release\mosquittopp.dll"
	File "..\build\plugins\iedb\Debug\iedb.dll"
	;File "..\aclfile.example"
	;File "..\ChangeLog.txt"
	;File "..\mosquitto.conf"
	;File "..\NOTICE.md"
	;File "..\pwfile.example"
	;File "..\README.md"
	;File "..\README-windows.txt"
	;File "..\README-letsencrypt.md"
	;File "..\SECURITY.md"
	;File "..\edl-v10"
	;File "..\epl-v20"

	;File "..\build64\vcpkg_installed\x64-windows-release\bin\cjson.dll"
	;File "..\build64\vcpkg_installed\x64-windows-release\bin\libcrypto-3-x64.dll"
	;File "..\build64\vcpkg_installed\x64-windows-release\bin\libssl-3-x64.dll"
	;File "..\build64\vcpkg_installed\x64-windows-release\bin\pthreadVC3.dll"
	;File "..\build64\vcpkg_installed\x64-windows-release\bin\uv.dll"
	;File "..\build64\vcpkg_installed\x64-windows-release\bin\websockets.dll"

	;SetOutPath "$INSTDIR\devel"
	;File "..\build64\lib\Release\mosquitto.lib"
	;File "..\build64\lib\cpp\Release\mosquittopp.lib"
	;File "..\include\mosquitto.h"
	;File "..\include\mosquitto_broker.h"
	;File "..\include\mosquitto_plugin.h"
	;File "..\include\mqtt_protocol.h"
	;File "..\lib\cpp\mosquittopp.h"
	
	SetOutPath "$INSTDIR"
	File /r   "..\www"

	WriteUninstaller "$INSTDIR\Uninstall.exe"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "DisplayName" "Luomi iotp(64 bit)"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "DisplayIcon" "$INSTDIR\iotp.ico"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "HelpLink" "https://www.lmgateway.com/"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "URLInfoAbout" "https://www.lmgateway.com/"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "NoModify" "1"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64" "NoRepair" "1"

	WriteRegExpandStr ${env_hklm} MOSQUITTO_DIR $INSTDIR
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
	
	FileOpen $0 "$INSTDIR\mosquitto.conf" w
	FileWrite $0 "listener 1883$\r$\n"
	FileWrite $0 "protocol mqtt$\r$\n"
	FileWrite $0 "listener 8080$\r$\n"
	FileWrite $0 "protocol websockets$\r$\n"
	
	FileWrite $0  "http_dir $INSTDIR\www$\r$\n"
	FileWrite $0  "persistence_location $TextDataPath$\r$\n"
	
	FileWrite $0  "log_dest file mosquitto.log$\r$\n"
	FileWrite $0  "log_type error$\r$\n"
	FileWrite $0  "log_type warning$\r$\n"
	FileWrite $0  "log_type notice$\r$\n"
	FileWrite $0  "log_type information$\r$\n"
	FileWrite $0  "connection_messages true$\r$\n"
	FileWrite $0  "log_timestamp true$\r$\n"
	FileWrite $0  "log_timestamp_format %Y-%m-%dT%H:%M:%S$\r$\n"
	FileWrite $0  "allow_anonymous true$\r$\n"
	
	FileWrite $0  "plugin $INSTDIR\iedb.dll$\r$\n"
	FileWrite $0  "plugin_opt_iedb_dir $TextDataPath$\r$\n"
	FileWrite $0  "plugin_opt_iedb_retention_hours 1440$\r$\n"
	FileWrite $0  "plugin_opt_iedb_retention_mbs 10240$\r$\n"
	FileClose $0
SectionEnd

Section "Visual Studio Runtime"
  SetOutPath "$INSTDIR"
  File "VC_redist.x64.exe"
  ExecWait '"$INSTDIR\VC_redist.x64.exe" /quiet /norestart'
  Delete "$INSTDIR\VC_redist.x64.exe"
SectionEnd

Section "Service" SecService
	ExecWait '"$INSTDIR\mosquitto.exe" install'
	ExecWait 'sc start mosquitto'
SectionEnd

Section "Uninstall"
	ExecWait 'sc stop mosquitto'
	Sleep 1000
	ExecWait '"$INSTDIR\mosquitto.exe" uninstall'
	Sleep 1000

	;Delete "$INSTDIR\mosquitto.dll"
	Delete "$INSTDIR\mosquitto.exe"
	;Delete "$INSTDIR\mosquitto_ctrl.exe"
	;Delete "$INSTDIR\mosquitto_passwd.exe"
	;Delete "$INSTDIR\mosquitto_pub.exe"
	;Delete "$INSTDIR\mosquitto_rr.exe"
	;Delete "$INSTDIR\mosquitto_sub.exe"
	;Delete "$INSTDIR\mosquittopp.dll"
	;Delete "$INSTDIR\mosquitto_dynamic_security.dll"
	Delete "$INSTDIR\iedb.dll"
	;Delete "$INSTDIR\aclfile.example"
	;Delete "$INSTDIR\ChangeLog.txt"
	Delete "$INSTDIR\mosquitto.conf"
	;Delete "$INSTDIR\pwfile.example"
	;Delete "$INSTDIR\NOTICE.md"
	;Delete "$INSTDIR\README.md"
	;Delete "$INSTDIR\README-windows.txt"
	;Delete "$INSTDIR\README-letsencrypt.md"
	;Delete "$INSTDIR\SECURITY.md"
	;Delete "$INSTDIR\edl-v10"
	;Delete "$INSTDIR\epl-v20"
	;Delete "$INSTDIR\mosquitto.ico"

	Delete "$INSTDIR\cjson.dll"
	Delete "$INSTDIR\libcrypto-3-x64.dll"
	Delete "$INSTDIR\libssl-3-x64.dll"
	Delete "$INSTDIR\pthreadVC3.dll"
	Delete "$INSTDIR\uv.dll"
	Delete "$INSTDIR\websockets.dll"
	Delete "$INSTDIR\sqlite3.dll"

	;Delete "$INSTDIR\devel\mosquitto.h"
	;Delete "$INSTDIR\devel\mosquitto_broker.h"
	;Delete "$INSTDIR\devel\mosquitto_plugin.h"
	;Delete "$INSTDIR\devel\mosquittopp.h"
	;Delete "$INSTDIR\devel\mqtt_protocol.h"
	;RMDir "$INSTDIR\devel\mosquitto"
	;RMDir "$INSTDIR\devel"

	Delete "$INSTDIR\Uninstall.exe"
	RMDir "$INSTDIR"
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\iotp64"

	DeleteRegValue ${env_hklm} MOSQUITTO_DIR
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

LangString DESC_SecInstall ${LANG_ENGLISH} "The main installation."
LangString DESC_SecService ${LANG_ENGLISH} "Install iotp as a Windows service?"

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecInstall} $(DESC_SecInstall)
	!insertmacro MUI_DESCRIPTION_TEXT ${SecService} $(DESC_SecService)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

LangString DESC_DbPathPage ${LANG_ENGLISH} "Configure the root path for the database"
LangString DESC_Database_Path ${LANG_ENGLISH} "Browse..."
LangString WARNING_DbPathIsEmpty ${LANG_ENGLISH} "The path for database is empty"

var HWNDDbPathBrowse
Var HWNDDbPath

Function DbPathOnBack
FunctionEnd

Function DbPathBrowse
	nsDialogs::SelectFolderDialog   "Select Database Path" "D:\iotp"
	Pop $TextDataPath
	${NSD_SetText} $HWNDDbPath $TextDataPath
	Pop $0
FunctionEnd

Function ShowDbPath

    nsDialogs::Create /NOUNLOAD 1018
    Pop $0

    ${If} $0 == error
        Abort
    ${EndIf}

    GetFunctionAddress $0 DbPathOnBack
    nsDialogs::OnBack $0

    ${NSD_CreateLabel} 0 0 100% 10u $(DESC_DbPathPage)
    Pop $0

    ${NSD_CreateText} 0 45 240u 12u $TextDataPath
    Pop $HWNDDbPath
    ${NSD_CreateButton} 370 45 50u 12u $(DESC_Database_Path)
    Pop $HWNDDbPathBrowse
    
    ${NSD_OnClick} $HWNDDbPathBrowse DbPathBrowse
    Pop $0

    nsDialogs::Show
FunctionEnd

Function LeaveDbPath
    ${NSD_GetText} $HWNDDbPath $TextDataPath

    StrLen $1 $TextDataPath
    ${If} $1 == 0
        MessageBox MB_ICONEXCLAMATION|MB_OK $(WARNING_DbPathIsEmpty)
        Abort
    ${EndIf}

FunctionEnd