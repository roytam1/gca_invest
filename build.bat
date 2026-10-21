@echo off
rem ------------------------------------------------------------------
rem build.bat -- build the GcaSDK reconstruction with Visual C++ 6.
rem
rem   build\gcasdk.lib   the SDK
rem   build\gcatest.exe  the test driver (if tests\gcatest.cpp exists)
rem
rem The VC6 linker needs mspdb60.dll, which is NOT in VC98\Bin on this
rem machine (it lives in Common\MSDev98\Bin, which VCVARS32 puts on the
rem PATH).  cl.exe's internal link step fails silently for that reason,
rem so compile and link in two explicit steps.
rem ------------------------------------------------------------------
call "C:\Program Files (x86)\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul 2>&1
rem "%~dp0." keeps the trailing backslash from escaping the quote
pushd "%~dp0."

if not exist build mkdir build
del /q build\* 2>nul

set SRCS=GcaSDK\gca_api.cpp GcaSDK\gca_arc.cpp GcaSDK\gca_bwt.cpp GcaSDK\gca_cod.cpp GcaSDK\gca_crypt.cpp GcaSDK\gca_numb.cpp GcaSDK\gca_numv.cpp GcaSDK\gca_sha1.cpp GcaSDK\gca_stage.cpp

"%MSVCDir%\BIN\CL.EXE" /nologo /c /MD /O2 /GX /D_CRT_SECURE_NO_WARNINGS /I. /I"GcaSDK" /Fo"build\\" %SRCS%
if errorlevel 1 goto :fail
echo CL_EXIT=%ERRORLEVEL%

"%MSVCDir%\BIN\LIB.EXE" /nologo /OUT:build\gcasdk.lib build\gca_api.obj build\gca_arc.obj build\gca_bwt.obj build\gca_cod.obj build\gca_crypt.obj build\gca_numb.obj build\gca_numv.obj build\gca_sha1.obj build\gca_stage.obj
if errorlevel 1 goto :fail
echo LIB_EXIT=%ERRORLEVEL%

if not exist tests\gcatest.cpp goto :libdone
"%MSVCDir%\BIN\CL.EXE" /nologo /c /MD /O2 /GX /D_CRT_SECURE_NO_WARNINGS /I. /I"GcaSDK" /Fo"build\\" tests\gcatest.cpp
if errorlevel 1 goto :fail
"%MSVCDir%\BIN\LINK.EXE" /nologo /OUT:build\gcatest.exe build\gcatest.obj build\gcasdk.lib
if errorlevel 1 goto :fail
echo LINK_EXIT=%ERRORLEVEL%
if exist build\gcatest.exe echo BUILT_OK
goto :eof

:libdone
if exist build\gcasdk.lib echo BUILT_OK
goto :eof

:fail
echo BUILD_FAILED
