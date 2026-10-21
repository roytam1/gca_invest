@echo off
rem ------------------------------------------------------------------
rem build.bat -- build the C tools with Visual C++ 6.
rem
rem   build\gcac_pack.exe    the packer
rem   build\gcac_unpack.exe  the unpacker
rem
rem cl.exe's internal link step fails silently on this machine because
rem mspdb60.dll is not in VC98\Bin (it lives in Common\MSDev98\Bin,
rem which VCVARS32 puts on the PATH), so compile and link separately.
rem ------------------------------------------------------------------
call "C:\Program Files (x86)\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul 2>&1
rem "%~dp0." keeps the trailing backslash from escaping the quote
pushd "%~dp0."

if not exist build mkdir build
del /q build\* 2>nul

set SRCS=gca_util.c gca_crypt.c gca_codec.c gca_arc.c

"%MSVCDir%\BIN\CL.EXE" /nologo /c /MD /O2 /D_CRT_SECURE_NO_WARNINGS /I. /Fo"build\\" %SRCS%
if errorlevel 1 goto :fail
echo CL_EXIT=%ERRORLEVEL%

"%MSVCDir%\BIN\CL.EXE" /nologo /c /MD /O2 /D_CRT_SECURE_NO_WARNINGS /I. /Fo"build\\" gca_pack.c
if errorlevel 1 goto :fail
"%MSVCDir%\BIN\CL.EXE" /nologo /c /MD /O2 /D_CRT_SECURE_NO_WARNINGS /I. /Fo"build\\" gca_unpack.c
if errorlevel 1 goto :fail

"%MSVCDir%\BIN\LINK.EXE" /nologo /OUT:build\gcac_pack.exe build\gca_pack.obj build\gca_util.obj build\gca_crypt.obj build\gca_codec.obj build\gca_arc.obj
if errorlevel 1 goto :fail
"%MSVCDir%\BIN\LINK.EXE" /nologo /OUT:build\gcac_unpack.exe build\gca_unpack.obj build\gca_util.obj build\gca_crypt.obj build\gca_codec.obj build\gca_arc.obj
if errorlevel 1 goto :fail
echo LINK_EXIT=%ERRORLEVEL%
if exist build\gcac_pack.exe if exist build\gcac_unpack.exe echo BUILT_OK
goto :eof

:fail
echo BUILD_FAILED
