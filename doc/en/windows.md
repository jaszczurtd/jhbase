# Prepare a Windows test machine

Use a Windows x64 VM with a normal user account, internet access for installation,
and a writable working directory such as `C:\src`. The existing VM has 4 vCPUs
and 4 GB RAM; close other programs during installation and builds.

## Build tools

1. Download [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe).
   Select **Desktop development with C++**. Include MSVC v143 x64/x86 build tools
   and a Windows SDK. The IDE is optional. The
   [Microsoft component list](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)
   describes the workload. From an elevated terminal, the equivalent is:

   ```powershell
   .\vs_BuildTools.exe --wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
   ```

2. Install [CMake](https://cmake.org/download/), version 3.22 or later, and enable
   its command-line PATH option. CTest is included. The Visual Studio generator
   is used, so Ninja is optional; the `vscode_workspace` suite builds a scratch
   copy with Ninja when present, otherwise with NMake from the Developer
   PowerShell (see [VS Code](vscode.md)).
3. Install [Git for Windows](https://gitforwindows.org/) with command-line access.
4. Finish pending installer restarts and open a new terminal.

Python, WSL, embedded SDKs and firmware toolchains are not required for the
native library tests. Unity comes from the revision pinned in
`third_party/unity_version.conf`; the transfer bundle carries its sources in
a `tools` directory.

## Remote access

OpenSSH Server allows commands, source transfer and log collection without a
desktop session. Follow the
[Microsoft OpenSSH installation guide](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse).
Run in an elevated PowerShell terminal:

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
Get-Service sshd
```

Use a normal account for builds and configure its `authorized_keys` with the
provided public key. Account and file locations are described in
[Microsoft key management](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement).
Administrators use a different authorized-keys location. Do not send passwords
or private keys in chat.

Keep TCP port 22 reachable from the VM host. The default libvirt network uses
host address `192.168.122.1`; confirm it before applying this optional restriction
to the firewall rule created by the SSH installation:

```powershell
Set-NetFirewallRule -Name OpenSSH-Server-In-TCP -Enabled True -RemoteAddress 192.168.122.1
```

## Check before handing over access

Run from a new ordinary PowerShell terminal:

```powershell
git --version
cmake --version
ctest --version
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Get-Service sshd
ipconfig
```

Provide the VM address, account name, host-key fingerprint, and the path of the
writable working directory. Confirm that SSH authentication works from the host.
The compiler does not have to be in an ordinary terminal's PATH: the runner
loads the Visual Studio developer-shell module itself.

## Run the tests

Transfer the complete reviewed source snapshot, including the HAL numeric
fixture and the pinned Unity sources identified by the dependency checks.
On Linux, `./scripts/package_windows.sh` creates
`.build/windows-transfer/jhbase-windows-source.tar.gz` with the current owned
sources, the HAL fixture and license, the pinned Unity sources under `tools` and
a SHA-256 manifest. Ignored files, build caches and local access keys are
excluded. Extract it with Windows `tar` into a writable directory; the bundle
already has the required sibling layout.

From the `jhbase` directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\runalltests.ps1 -HalRoot C:\src\JaszczurHAL -ToolsRoot C:\src\tools
```

The runner builds and runs the same CTest suites for x64 and Win32. Select only
one configuration with `-Architecture x64`. Reports are written below
`.build/msvc-x64` and `.build/msvc-Win32`, including JUnit XML. It propagates
configure, build and test errors; no failing configuration is skipped.

The [Linux test guide](testing.md) describes sanitizers, Valgrind, fuzzing and
freestanding compilation. Those checks run separately from native MSVC.
