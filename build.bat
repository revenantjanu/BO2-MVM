@echo off
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "W=%LOCALAPPDATA%\bo2mvm_build"
set "LOG=%TEMP%\bo2mvm_build.log"
set "LOG2=%TEMP%\bo2mvm_build2.log"
set "VSCMD_SKIP_SENDTELEMETRY=1"
echo === BO2 MVM build %date% %time% === > "%LOG%"
echo Building BO2 MVM in %W% (outside OneDrive). This takes a minute or two...

if not exist "%HERE%bo2mvm.zip" goto no_zip
if exist "%W%" rmdir /s /q "%W%" >> "%LOG%" 2>&1
mkdir "%W%" >> "%LOG%" 2>&1
tar -xf "%HERE%bo2mvm.zip" -C "%W%" >> "%LOG%" 2>&1
if not exist "%W%\bo2mvm\dll\CMakeLists.txt" goto no_src

set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto no_vs
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" goto no_vs
echo VS found: %VSPATH% >> "%LOG%"
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 > "%TEMP%\bo2mvm_vcvars.log" 2>&1
echo === build steps === > "%LOG2%"

echo --- DLL --- >> "%LOG2%"
cd /d "%W%\bo2mvm\dll"
where cmake >> "%LOG2%" 2>&1
where ninja >> "%LOG2%" 2>&1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >> "%LOG2%" 2>&1
if errorlevel 1 goto fail
cmake --build build >> "%LOG2%" 2>&1
if errorlevel 1 goto fail

echo --- INJECTOR --- >> "%LOG2%"
cd /d "%W%\bo2mvm\injector"
dotnet build -c Release >> "%LOG2%" 2>&1
if errorlevel 1 goto fail

if not exist "%HERE%bo2mvm_out" mkdir "%HERE%bo2mvm_out"
copy /y "%W%\bo2mvm\dll\build\bo2mvm.dll" "%HERE%bo2mvm_out\" >> "%LOG2%" 2>&1
copy /y "%W%\bo2mvm\injector\bin\x86\Release\net48\BO2MVM.exe" "%HERE%bo2mvm_out\" >> "%LOG2%" 2>&1
if not exist "%HERE%bo2mvm_out\bo2mvm.dll" goto fail
if not exist "%HERE%bo2mvm_out\BO2MVM.exe" goto fail
echo === SUCCESS === >> "%LOG2%"
copy /b "%LOG%"+"%LOG2%" "%HERE%build_log.txt" /y >nul 2>&1
echo.
echo SUCCESS. Your files are in: %HERE%bo2mvm_out
goto end

:no_zip
echo bo2mvm.zip not found next to build.bat >> "%LOG2%"
goto fail
:no_src
echo could not unpack bo2mvm.zip >> "%LOG2%"
goto fail
:no_vs
echo Visual Studio with the "Desktop development with C++" workload was not found. >> "%LOG2%"
goto fail
:fail
echo === FAILED === >> "%LOG%"
copy /b "%LOG%"+"%LOG2%" "%HERE%build_log.txt" /y >nul 2>&1
echo.
echo BUILD FAILED. Last lines of the log:
powershell -NoProfile -Command "Get-Content -Tail 30 '%LOG2%'"
echo.
echo Tell Claude. The full log is in build_log.txt next to this file.
:end
echo.
pause
