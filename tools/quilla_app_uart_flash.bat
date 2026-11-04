@echo off
REM ---------------------------------------------------------------------------
REM Quilla - flash the QAPP image through the QBOOT serial bootloader (QIAP protocol).
REM
REM   usage: tools\quilla_app_uart_flash.bat <COMx> [baud] [switches: /P /F /M /K=key.hex /V=N | --probe --force --mdk --key= --version=]
REM
REM Flashes the raw binary the GNU (EIDE) build produces by default:
REM   user\py32f040xb\EIDE\QAPP\QAPP.bin
REM /M (--mdk) prefers the Keil MDK-ARM bin instead (for Attach-debug symbol match).
REM Falls back to the other bin only if the preferred one is absent.
REM Build it FIRST yourself: make -C user\py32f040xb\EIDE all (or in Keil/EIDE).
REM This wrapper does NOT compile; it only flashes an already-built bin.
REM
REM Step 2 hands the bin to the reference host updater over USART2 PA2/PA3.
REM This wrapper is deliberately ASCII-only: the path contains spaces and the
REM console is GBK, so non-ASCII here breaks parsing.
REM
REM Operator notes:
REM   * Keep any serial terminal on that COM port CLOSED - two openers fight.
REM   * If the board is already running the old APP, either press RESET so
REM     QBOOT prints its banner, or type "swqboot" in the APP shell first.
REM   * QBOOT itself must already be flashed (once) with the debugger.
REM   * Shell invocation: from a plain cmd.exe prompt the short switches work
REM     as written (e.g. "quilla_app_uart_flash.bat 7 /K=key.hex /V=1"). Under
REM     PowerShell or Git-Bash, wrap with "cmd /c" or use PowerShell's stop-
REM     parsing "--%" token so the /K and /V switches are not eaten by the
REM     shell: "quilla_app_uart_flash.bat 7 --% --key=key.hex --version=1".
REM
REM Override the interpreter with PY=<python.exe>; it needs pyserial.
REM ---------------------------------------------------------------------------
setlocal

if "%~1"=="" goto :usage

REM Operator can pre-set PY to another interpreter (needs pyserial).
if defined PY goto :py_ready
REM Auto-detect: prefer the Windows `py` launcher, then `python` on PATH.
REM Each probe is a standalone statement -- no parenthesised block, see the
REM block-parser note further down for why.
for /f "delims=" %%P in ('py -3 -c "import sys;print(sys.executable)" 2^>nul') do set "PY=%%P"
if defined PY goto :py_ready
for /f "delims=" %%P in ('python -c "import sys;print(sys.executable)" 2^>nul') do set "PY=%%P"
if defined PY goto :py_ready
echo [X] Python 3 not found. Install Python 3 (with pyserial), or set PY=^<python.exe^>
exit /b 1
:py_ready

REM Normalize ROOT to a clean absolute path (no trailing slash, no "..").
REM This matters: cmd's `if exist` is unreliable on paths that contain ".."
REM together with long, space-bearing segments, which would otherwise make
REM the bin lookup miss a file that is actually present.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "ROOT=%ROOT%\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "EIDE=%ROOT%\user\py32f040xb\EIDE"
set "GNU_BIN=%EIDE%\QAPP\QAPP.bin"
set "MDK=%ROOT%\user\py32f040xb\MDK-ARM"
set "KEIL_BIN=%MDK%\Output\QAPP.bin"

set "PORT=%~1"
REM Normalize a bare number (e.g. "7") to the Windows port name "COM7".
REM Pure-numeric PORT token? (avoid findstr regex quirks) -> "COMn".
set "PORT_NUM="
for /f "delims=0123456789" %%N in ("%PORT%") do set "PORT_NUM=%%N"
if not defined PORT_NUM set "PORT=COM%PORT%"

REM ---------------------------------------------------------------------------
REM After the port, every other token is scanned in a loop:
REM   a bare number or COMn (no switch prefix) is the optional BAUD
REM   switches:
REM     /P  --probe     : probe board state only, no flash
REM     /F  --force     : hint to rebuild GNU bin before flash (bat does NOT compile)
REM     /K=p --key=p    : Ed25519 private key hex - send SIGN frame (secure boot)
REM     /V=N --version=N: image version for the SIGN frame (default 1)
REM NOTE: /K and /V are cmd.exe reserved switches; under PowerShell/Git-Bash
REM       use the long forms --key= and --version=, and stop parsing with
REM       PowerShell's --% token or an explicit cmd /c wrapper (see header).
REM ---------------------------------------------------------------------------
set "BAUD=115200"
set "BAUD_SET=0"
set "PROBE=0"
set "FORCE=0"
set "KEYARG="
set "VERARG="
set "MDK_PREF=0"

REM Drop the PORT token, then scan the rest (BAUD is positional-optional).
shift

:parse_args
if "%~1"=="" goto :args_done
set "A=%~1"
REM Bare numeric token (no switch prefix) => optional BAUD (legacy position).
REM Detect "all digits" with a delims scan rather than findstr regex.
set "TOK_NUM="
for /f "delims=0123456789" %%N in ("%A%") do set "TOK_NUM=%%N"
if not defined TOK_NUM (
  if "%BAUD_SET%"=="0" ( set "BAUD=%A%" & set "BAUD_SET=1" & shift & goto :parse_args )
)
if /i "%A%"=="/P" ( set "PROBE=1" & shift & goto :args_done )
if /i "%A%"=="-P" ( set "PROBE=1" & shift & goto :args_done )
if /i "%A%"=="--probe" ( set "PROBE=1" & shift & goto :args_done )
if /i "%A%"=="/F" ( set "FORCE=1" & shift & goto :parse_args )
if /i "%A%"=="-F" ( set "FORCE=1" & shift & goto :parse_args )
if /i "%A%"=="--force" ( set "FORCE=1" & shift & goto :parse_args )
REM Prefer the Keil MDK-ARM bin over the GNU (EIDE) bin. Use this when you need
REM the running image to match the Keil QAPP.axf symbols for an Attach debug
REM session (the two toolchains lay code out differently).
if /i "%A%"=="/M" ( set "MDK_PREF=1" & shift & goto :parse_args )
if /i "%A%"=="-M" ( set "MDK_PREF=1" & shift & goto :parse_args )
if /i "%A%"=="--mdk" ( set "MDK_PREF=1" & shift & goto :parse_args )
REM Key: "--key=value" / "/K=value" (single token) OR "--key value" / "/K value"
REM (PowerShell native-command split of --key=value). Both forms accepted.
echo %A% | findstr /i /c:"/K=" /c:"--key=" >nul
if not errorlevel 1 (
  for /f "tokens=1,* delims==" %%K in ("%A%") do set "KEYARG=%%L"
  shift & goto :parse_args
)
if /i "%A%"=="--key" ( set "KEYARG=%~2" & shift & shift & goto :parse_args )
if /i "%A%"=="/K" ( set "KEYARG=%~2" & shift & shift & goto :parse_args )
REM Version: same two forms.
echo %A% | findstr /i /c:"/V=" /c:"--version=" >nul
if not errorlevel 1 (
  for /f "tokens=1,* delims==" %%V in ("%A%") do set "VERARG=%%V"
  shift & goto :parse_args
)
if /i "%A%"=="--version" ( set "VERARG=%~2" & shift & shift & goto :parse_args )
if /i "%A%"=="/V" ( set "VERARG=%~2" & shift & shift & goto :parse_args )
echo [warn] ignored unknown arg: %A%
shift
goto :parse_args
:args_done

if "%PROBE%"=="1" (
  echo [probe] QBOOT upgrade-state probe [QIAP] on %PORT% @ %BAUD%
  "%PY%" "%ROOT%\tools\quilla_host_updater.py" %PORT% -b %BAUD% --probe
  set "RC=%ERRORLEVEL%"
  if not "%RC%"=="0" echo [X] probe exited with %RC%
  exit /b %RC%
)

REM /F (or -F) is a hint only; this wrapper does NOT compile (see notes above).

if not exist "%PY%" (
  echo [X] python not found: "%PY%"
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM IMPORTANT: this wrapper does NOT compile the firmware. Building is the
REM developer's job -- run `make -C "%EIDE%" all` (or build in Keil/EIDE)
REM yourself BEFORE flashing. Here we only select an existing bin and hand it
REM to the host updater.
REM ---------------------------------------------------------------------------

REM Choose the binary. By default the GNU (EIDE) build is authoritative; /M
REM (--mdk) inverts the preference to the Keil bin so the running image matches
REM the Keil QAPP.axf for an Attach debug session (the toolchains differ in code
REM layout). We always fall back to the other bin if the preferred one is
REM missing. Existence is tested with Python (a hard dependency) because cmd
REM `if exist` misreports present files on long space-bearing paths.
if "%MDK_PREF%"=="1" goto :bin_mdk
set "BIN=%GNU_BIN%"
set "BIN_ALT=%KEIL_BIN%"
goto :bin_chosen
:bin_mdk
set "BIN=%KEIL_BIN%"
set "BIN_ALT=%GNU_BIN%"
:bin_chosen
"%PY%" -c "import os,sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 1)" "%BIN%" >nul 2>nul
if errorlevel 1 (
  set "BIN=%BIN_ALT%"
)
"%PY%" -c "import os,sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 1)" "%BIN%" >nul 2>nul
if errorlevel 1 (
  echo [X] bin not found: "%GNU_BIN%" nor "%KEIL_BIN%"
  echo     install make or build manually: make -C "%EIDE%" all
  exit /b 1
)
for %%F in ("%BIN%") do echo [1/2] use bin: "%BIN%" = %%~zF bytes

echo [2/2] QAPP download via QBOOT [QIAP] on %PORT% @ %BAUD%
REM Anchor a relative key path to ROOT so it resolves regardless of the current
REM directory the bat is launched from (ROOT is the project root, derived from
REM %~dp0, so "tools\_keytest\private.hex" always lands at ROOT\tools\_keytest...).
REM KEYABS/EXTRA are computed with standalone `if` statements (NOT inside a
REM parenthesized () block). This is deliberate: cmd's block parser treats a
REM ')' -- even a balanced literal one inside an echo -- as a candidate block
REM close, which can orphan a following `set` and abort with the
REM "set was unexpected at this time" error. Keeping every statement on its own
REM non-block line makes the "(version N)" parentheses harmless literal text.
set "KEYABS="
if not "%KEYARG%"=="" set "KEYABS=%KEYARG%"
if not "%KEYARG%"=="" if not "%KEYARG:~1,1%"==":" set "KEYABS=%ROOT%\%KEYARG%"
set "EXTRA="
if not "%KEYARG%"=="" set "EXTRA=--key "%KEYABS%""
if not "%VERARG%"=="" set "EXTRA=%EXTRA% --version %VERARG%"
if not "%KEYARG%"=="" echo [*] secure boot: SIGN frame via key %KEYABS% (version %VERARG%)
"%PY%" "%ROOT%\tools\quilla_host_updater.py" %PORT% "%BIN%" -b %BAUD% %EXTRA%
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo [X] updater exited with %RC%
  exit /b %RC%
)
echo [OK] done

endlocal
exit /b 0

:usage
echo usage: %~nx0 ^<COMx^> [baud] [switches]
echo   COMx  : serial port, e.g. COM7 (bare "7" is accepted too)
echo   baud  : default 115200
echo   switches (any order, after COMx/baud):
echo     /P   --probe      : probe board state only, no flash
echo     /F   --force      : hint to rebuild GNU bin before flash (bat does NOT compile)
echo     /M   --mdk        : prefer the Keil MDK-ARM bin (match QAPP.axf for Attach)
echo     /K=p --key=p      : Ed25519 private key hex - send SIGN frame, secure boot
echo     /V=N --version=N  : image version for SIGN frame (default 1)
echo   NOTE: /K and /V are cmd.exe reserved switches; under PowerShell/Git-Bash
echo         use --key= and --version= instead (direct cmd.exe call keeps /K /V).
exit /b 2
