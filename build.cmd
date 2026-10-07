@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools"
set "MSVC=%VS%\VC\Tools\MSVC\14.16.27023"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.17763.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "LIBPATH=%MSVC%\lib\x64;%MSVC%\lib\onecore\x64;%SDK%\References"

echo.
echo ========================================
echo LightSpeed Image Viewer
echo JPEG + PNG + WebP + SVG + GIF + ICO
echo ========================================
echo.

where cl >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe not found.
    exit /b 1
)

where rc >nul 2>nul
if errorlevel 1 (
    echo ERROR: rc.exe not found.
    exit /b 1
)

where powershell >nul 2>nul
if errorlevel 1 (
    echo ERROR: PowerShell not found.
    exit /b 1
)

echo.
echo Creating icon.ico from icon.png...

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$p='%ROOT%\icon.png'; $o='%ROOT%\icon.ico'; $b=[IO.File]::ReadAllBytes($p); if($b.Length -lt 24 -or $b[0] -ne 137 -or $b[1] -ne 80 -or $b[2] -ne 78 -or $b[3] -ne 71){throw 'Invalid PNG'}; $w=($b[16] -shl 24) -bor ($b[17] -shl 16) -bor ($b[18] -shl 8) -bor $b[19]; $h=($b[20] -shl 24) -bor ($b[21] -shl 16) -bor ($b[22] -shl 8) -bor $b[23]; Write-Host ('PNG: ' + $w + 'x' + $h); if($w -gt 256 -or $h -gt 256){throw 'icon.png must be 256x256 or smaller'}; $ms=New-Object IO.MemoryStream; $bw=New-Object IO.BinaryWriter($ms); $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]1); $bw.Write([Byte]$(if($w -eq 256){0}else{$w})); $bw.Write([Byte]$(if($h -eq 256){0}else{$h})); $bw.Write([Byte]0); $bw.Write([Byte]0); $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$b.Length); $bw.Write([UInt32]22); $bw.Write($b); $bw.Flush(); [IO.File]::WriteAllBytes($o,$ms.ToArray()); $bw.Dispose(); $ms.Dispose()"

if errorlevel 1 (
    echo ERROR: Could not create icon.ico from icon.png.
    exit /b 1
)

echo Icon: OK

if not exist "%ROOT%\third_party" (
    mkdir "%ROOT%\third_party"
)

if not exist "%ROOT%\bin" (
    mkdir "%ROOT%\bin"
)

if not exist "%ROOT%\third_party\nanosvg.h" (
    echo Downloading NanoSVG...

    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/memononen/nanosvg/master/src/nanosvg.h' -OutFile '%ROOT%\third_party\nanosvg.h'"

    if errorlevel 1 (
        echo ERROR: nanosvg.h download failed.
        exit /b 1
    )
)


if not exist "%ROOT%\third_party\nanosvgrast.h" (
    echo Downloading NanoSVG rasterizer...

    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/memononen/nanosvg/master/src/nanosvgrast.h' -OutFile '%ROOT%\third_party\nanosvgrast.h'"

    if errorlevel 1 (
        echo ERROR: nanosvgrast.h download failed.
        exit /b 1
    )
)


echo.
echo NanoSVG: OK
echo.


rem ============================================================
rem Create version resource
rem ============================================================

echo Creating version resource...

> "%ROOT%\version.rc" echo #include ^<windows.h^>
>> "%ROOT%\version.rc" echo 1 ICON "icon.ico"
>> "%ROOT%\version.rc" echo.
>> "%ROOT%\version.rc" echo VS_VERSION_INFO VERSIONINFO
>> "%ROOT%\version.rc" echo FILEVERSION 0,22,0,0
>> "%ROOT%\version.rc" echo PRODUCTVERSION 0,22,0,0
>> "%ROOT%\version.rc" echo FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
>> "%ROOT%\version.rc" echo FILEFLAGS 0
>> "%ROOT%\version.rc" echo FILEOS VOS__WINDOWS32
>> "%ROOT%\version.rc" echo FILETYPE VFT_APP
>> "%ROOT%\version.rc" echo FILESUBTYPE 0
>> "%ROOT%\version.rc" echo BEGIN
>> "%ROOT%\version.rc" echo     BLOCK "StringFileInfo"
>> "%ROOT%\version.rc" echo     BEGIN
>> "%ROOT%\version.rc" echo         BLOCK "040904B0"
>> "%ROOT%\version.rc" echo         BEGIN
>> "%ROOT%\version.rc" echo             VALUE "CompanyName", "free"
>> "%ROOT%\version.rc" echo             VALUE "FileDescription", "LightSpeed Image Viewer"
>> "%ROOT%\version.rc" echo             VALUE "FileVersion", "0.22"
>> "%ROOT%\version.rc" echo             VALUE "InternalName", "LightSpeed"
>> "%ROOT%\version.rc" echo             VALUE "LegalCopyright", "free"
>> "%ROOT%\version.rc" echo             VALUE "OriginalFilename", "LightSpeed.exe"
>> "%ROOT%\version.rc" echo             VALUE "ProductName", "LightSpeed Image Viewer"
>> "%ROOT%\version.rc" echo             VALUE "ProductVersion", "0.22"
>> "%ROOT%\version.rc" echo         END
>> "%ROOT%\version.rc" echo     END
>> "%ROOT%\version.rc" echo     BLOCK "VarFileInfo"
>> "%ROOT%\version.rc" echo     BEGIN
>> "%ROOT%\version.rc" echo         VALUE "Translation", 0x0409, 1200
>> "%ROOT%\version.rc" echo     END
>> "%ROOT%\version.rc" echo END


echo.
echo ========================================
echo Building version resource...
echo ========================================
echo.

rc /nologo ^
   /fo "%ROOT%\version.res" ^
   "%ROOT%\version.rc"

if errorlevel 1 (
    echo.
    echo ========================================
    echo RESOURCE BUILD FAILED
    echo ========================================
    exit /b 1
)


echo.
echo ========================================
echo Building x64 Release...
echo ========================================
echo.


cl /nologo ^
   /O1 ^
   /GL ^
   /Gy ^
   /Gw ^
   /GR- ^
   /MT ^
   /EHsc ^
   /DNDEBUG ^
   /DUNICODE ^
   /D_UNICODE ^
   /I"%ROOT%\third_party" ^
   "%ROOT%\main.cpp" ^
   /link ^
   "%ROOT%\version.res" ^
   /LTCG ^
   /SUBSYSTEM:WINDOWS ^
   /ENTRY:wWinMainCRTStartup ^
   /OPT:REF ^
   /OPT:ICF ^
   /INCREMENTAL:NO ^
   kernel32.lib ^
   user32.lib ^
   gdi32.lib ^
   ole32.lib ^
   advapi32.lib ^
   shell32.lib ^
   d2d1.lib ^
   windowscodecs.lib ^
   /OUT:"%ROOT%\bin\lsv.exe"


if errorlevel 1 (
    echo.
    echo ========================================
    echo BUILD FAILED
    echo ========================================
    exit /b 1
)


echo.
echo ========================================
echo BUILD SUCCESSFUL
echo ========================================


endlocal
