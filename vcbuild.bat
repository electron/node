@echo off

cd %~dp0

if /i "%1"=="help" goto help
if /i "%1"=="--help" goto help
if /i "%1"=="-help" goto help
if /i "%1"=="/help" goto help
if /i "%1"=="?" goto help
if /i "%1"=="-?" goto help
if /i "%1"=="--?" goto help
if /i "%1"=="/?" goto help

@rem Process arguments.
set config=Release
set msiplatform=x86
set target=Build
set target_arch=ia32
set debug_arg=
set snapshot_arg=
set noprojgen=
set nobuild=
set nosign=
set nosnapshot=
set test_args=
set msi=
set licensertf=
set jslint=
set buildnodeweak=
set noetw=
set noetw_arg=
set noetw_msi_arg=
set noperfctr=
set noperfctr_arg=
set noperfctr_msi_arg=
set i18n_arg=
set download_arg=

:next-arg
if "%1"=="" goto args-done
if /i "%1"=="debug"         set config=Debug&goto arg-ok
if /i "%1"=="release"       set config=Release&goto arg-ok
if /i "%1"=="clean"         set target=Clean&goto arg-ok
if /i "%1"=="ia32"          set target_arch=ia32&goto arg-ok
if /i "%1"=="x86"           set target_arch=ia32&goto arg-ok
if /i "%1"=="x64"           set target_arch=x64&goto arg-ok
if /i "%1"=="noprojgen"     set noprojgen=1&goto arg-ok
if /i "%1"=="nobuild"       set nobuild=1&goto arg-ok
if /i "%1"=="nosign"        set nosign=1&goto arg-ok
if /i "%1"=="nosnapshot"    set nosnapshot=1&goto arg-ok
if /i "%1"=="noetw"         set noetw=1&goto arg-ok
if /i "%1"=="noperfctr"     set noperfctr=1&goto arg-ok
if /i "%1"=="licensertf"    set licensertf=1&goto arg-ok
if /i "%1"=="test"          set test_args=%test_args% sequential parallel message -J&set jslint=1&goto arg-ok
if /i "%1"=="test-ci"       set test_args=%test_args% -p tap --logfile test.tap message sequential parallel&goto arg-ok
if /i "%1"=="test-simple"   set test_args=%test_args% sequential parallel -J&goto arg-ok
if /i "%1"=="test-message"  set test_args=%test_args% message&goto arg-ok
if /i "%1"=="test-gc"       set test_args=%test_args% gc&set buildnodeweak=1&goto arg-ok
if /i "%1"=="test-internet" set test_args=%test_args% internet&goto arg-ok
if /i "%1"=="test-pummel"   set test_args=%test_args% pummel&goto arg-ok
if /i "%1"=="test-all"      set test_args=%test_args% sequential parallel message gc internet pummel&set buildnodeweak=1&set jslint=1&goto arg-ok
if /i "%1"=="jslint"        set jslint=1&goto arg-ok
if /i "%1"=="msi"           set msi=1&set licensertf=1&goto arg-ok
if /i "%1"=="small-icu"     set i18n_arg=%1&goto arg-ok
if /i "%1"=="full-icu"      set i18n_arg=%1&goto arg-ok
if /i "%1"=="intl-none"     set i18n_arg=%1&goto arg-ok
if /i "%1"=="download-all"  set download_arg="--download=all"&goto arg-ok

echo Warning: ignoring invalid command line option `%1`.

:arg-ok
:arg-ok
shift
goto next-arg

:args-done
if "%config%"=="Debug" set debug_arg=--debug
if "%target_arch%"=="x64" set msiplatform=x64
if defined nosnapshot set snapshot_arg=--without-snapshot
if defined noetw set noetw_arg=--without-etw& set noetw_msi_arg=/p:NoETW=1
if defined noperfctr set noperfctr_arg=--without-perfctr& set noperfctr_msi_arg=/p:NoPerfCtr=1

if "%i18n_arg%"=="full-icu" set i18n_arg=--with-intl=full-icu
if "%i18n_arg%"=="small-icu" set i18n_arg=--with-intl=small-icu
if "%i18n_arg%"=="intl-none" set i18n_arg=--with-intl=none

:project-gen
@rem Skip project generation if requested.
if defined noprojgen goto msbuild

if defined NIGHTLY set TAG=nightly-%NIGHTLY%

@rem Generate the VS project.
SETLOCAL
  if defined VS100COMNTOOLS call "%VS100COMNTOOLS%\VCVarsQueryRegistry.bat"
  python configure %download_arg% %i18n_arg% %debug_arg% %snapshot_arg% %noetw_arg% %noperfctr_arg% --dest-cpu=%target_arch% --tag=%TAG%
  if errorlevel 1 goto create-msvs-files-failed
  if not exist node.sln goto create-msvs-files-failed
  echo Project files generated.
ENDLOCAL

:msbuild
@rem Skip project generation if requested.
if defined nobuild goto sign

@rem Look for Visual Studio 2013
if not defined VS120COMNTOOLS goto msbuild-not-found
if not exist "%VS120COMNTOOLS%\..\..\vc\vcvarsall.bat" goto msbuild-not-found
if "%VCVARS_VER%" NEQ "120" (
  call "%VS120COMNTOOLS%\..\..\vc\vcvarsall.bat"
  SET VCVARS_VER=120
)
if not defined VCINSTALLDIR goto msbuild-not-found
set GYP_MSVS_VERSION=2013
goto msbuild-found

:msbuild-not-found
echo Build skipped. To build, this file needs to run from VS cmd prompt.
goto run

:msbuild-found
@rem Build the sln with msbuild.
msbuild node.sln /m /t:%target% /p:Configuration=%config% /clp:NoSummary;NoItemAndPropertyList;Verbosity=minimal /nologo
if errorlevel 1 goto exit

:sign
@rem Skip signing if the `nosign` option was specified.
if defined nosign goto licensertf

signtool sign /a /d "io.js" /t http://timestamp.globalsign.com/scripts/timestamp.dll Release\iojs.exe
if errorlevel 1 echo Failed to sign exe&goto exit

:licensertf
@rem Skip license.rtf generation if not requested.
if not defined licensertf goto msi

%config%\iojs tools\license2rtf.js < LICENSE > %config%\license.rtf
if errorlevel 1 echo Failed to generate license.rtf&goto exit

:msi
@rem Skip msi generation if not requested
if not defined msi goto run
call :getnodeversion

if not defined NIGHTLY goto msibuild
set NODE_VERSION=%NODE_VERSION%.%NIGHTLY%

:msibuild
echo Building iojs-%NODE_VERSION%
msbuild "%~dp0tools\msvs\msi\nodemsi.sln" /m /t:Clean,Build /p:Configuration=%config% /p:Platform=%msiplatform% /p:NodeVersion=%NODE_VERSION% %noetw_msi_arg% %noperfctr_msi_arg% /clp:NoSummary;NoItemAndPropertyList;Verbosity=minimal /nologo
if errorlevel 1 goto exit

if defined nosign goto run
signtool sign /a /d "io.js" /t http://timestamp.globalsign.com/scripts/timestamp.dll Release\iojs-v%NODE_VERSION%-%msiplatform%.msi
if errorlevel 1 echo Failed to sign msi&goto exit

:run
@rem Run tests if requested.

:build-node-weak
@rem Build node-weak if required
if "%buildnodeweak%"=="" goto run-tests
"%config%\iojs" deps\npm\node_modules\node-gyp\bin\node-gyp rebuild --directory="%~dp0test\gc\node_modules\weak" --nodedir="%~dp0."
if errorlevel 1 goto build-node-weak-failed
goto run-tests

:build-node-weak-failed
echo Failed to build node-weak.
goto exit

:run-tests
if "%test_args%"=="" goto jslint
if "%config%"=="Debug" set test_args=--mode=debug %test_args%
if "%config%"=="Release" set test_args=--mode=release %test_args%
echo running 'cctest'
"%config%\cctest"
echo running 'python tools\test.py %test_args%'
python tools\test.py %test_args%
goto jslint

:jslint
if not defined jslint goto exit
echo running jslint
%config%\iojs tools\eslint\bin\eslint.js src lib test --rulesdir tools\eslint-rules --reset --quiet
goto exit

:create-msvs-files-failed
echo Failed to create vc project files.
goto exit

:help
echo vcbuild.bat [debug/release] [msi] [test-all/test-uv/test-internet/test-pummel/test-simple/test-message] [clean] [noprojgen] [small-icu/full-icu/intl-none] [nobuild] [nosign] [x86/x64] [download-all]
echo Examples:
echo   vcbuild.bat                : builds release build
echo   vcbuild.bat debug          : builds debug build
echo   vcbuild.bat release msi    : builds release build and MSI installer package
echo   vcbuild.bat test           : builds debug build and runs tests
goto exit

:exit
goto :EOF

rem ***************
rem   Subroutines
rem ***************

:getnodeversion
set NODE_VERSION=
for /F "usebackq tokens=*" %%i in (`python "%~dp0tools\getnodeversion.py"`) do set NODE_VERSION=%%i
if not defined NODE_VERSION echo Cannot determine current version of io.js & exit /b 1
goto :EOF
