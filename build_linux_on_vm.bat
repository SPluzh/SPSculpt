@echo off
setlocal

:: ============================================================================
:: Build SPSculpt on Linux Virtual Machine via SSH
:: Minimal sync mode: Only src/, CMakeLists.txt and shell scripts are transferred.
:: ============================================================================

set "VM_USER=user"
set "VM_IP=192.168.219.149"
set "VM_DIR=/home/user/sculptsp-native"

echo ==========================================================
echo [Linux VM Builder] Target: %VM_USER%@%VM_IP%:%VM_DIR%
echo ==========================================================

:: 1. Quick connection check and show disk space
echo [1/3] Checking connection and free disk space on VM...
ssh -n %VM_USER%@%VM_IP% "df -h / | awk 'NR==2 {print \"      Available on /: \" $4 \" (used \" $5 \")\"}'"
if errorlevel 1 (
    echo [ERROR] Cannot connect to %VM_USER%@%VM_IP%. Is the VM running?
    exit /b 1
)

:: 2. Minimal Sync: Only src, CMakeLists.txt and shell scripts (skips heavy folders)
echo [2/3] Syncing ONLY source files (skipping build, resources, third_party)...
scp -r src CMakeLists.txt build_linux.sh run_linux.sh %VM_USER%@%VM_IP%:%VM_DIR%/
if errorlevel 1 (
    echo [ERROR] scp failed.
    exit /b 1
)
echo       Sync complete.

:: 3. Run build script on VM
echo [3/3] Compiling on Linux VM...
ssh -n %VM_USER%@%VM_IP% "cd %VM_DIR% && sed -i 's/\r$//' *.sh 2>/dev/null && chmod +x build_linux.sh run_linux.sh && ./build_linux.sh"
if errorlevel 1 (
    echo.
    echo ==========================================================
    echo [FAILED] Compilation failed on Linux VM.
    echo ==========================================================
    exit /b 1
)

echo.
echo ==========================================================
echo [SUCCESS] Linux build complete!
echo Binary is ready on VM at: %VM_DIR%/build/SPSculpt
echo ==========================================================

if /I "%~1"=="run" (
    echo [RUN] Launching SPSculpt on VM...
    ssh -n %VM_USER%@%VM_IP% "cd %VM_DIR% && ./run_linux.sh"
)

exit /b 0
