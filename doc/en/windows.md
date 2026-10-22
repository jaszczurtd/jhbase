# Prepare a Windows test machine

Use Windows x64 with a normal user account, internet access for installation
and a writable working directory such as `C:\src`.

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

Prepare the bundle on Linux from the reviewed source snapshot. It needs the
HAL checkout selected by `JHBASE_HAL_ROOT` (default `../JaszczurHAL`) and three
dependencies: Unity, utf8proc, and the Unicode normalization corpus with its
license. Prepare those dependencies explicitly, then package them:

```sh
./scripts/ensure_tools.sh unity utf8proc unicode-test
./scripts/package_windows.sh
```

The result is `.build/windows-transfer/jhbase-windows-source.tar.gz`.
It contains jhbase sources, the HAL numeric fixture and license, Unity under
`tools`, and a SHA-256 manifest. The ignored utf8proc checkout, including its
Git metadata, and the Unicode corpus and license are included explicitly.
Other ignored files, build caches and local access keys are excluded.
The packaging script verifies dependencies without downloading them.

Transfer the archive and extract it with Windows `tar` into a new writable
directory. The bundle already has the required sibling layout. Using a new
directory keeps objects from a previous snapshot out of the build.

From the `jhbase` directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\runalltests.ps1 -HalRoot C:\src\JaszczurHAL -ToolsRoot C:\src\tools
```

The runner builds and runs CTest with Unicode disabled and enabled on x64 and
Win32. Every configuration includes `jb::host`. `-Architecture x64` selects
only x64, retaining both Unicode settings. Each build directory contains its
JUnit report, `ctest.xml`:

| Architecture | Unicode | Build and report directory |
| --- | --- | --- |
| x64 | OFF | `.build/msvc-x64` |
| x64 | ON | `.build/msvc-x64-unicode` |
| Win32 | OFF | `.build/msvc-Win32` |
| Win32 | ON | `.build/msvc-Win32-unicode` |

Configure, build or test errors stop the runner; no failing configuration is skipped.

The [Linux test guide](testing.md) describes sanitizers, Valgrind, fuzzing and
freestanding compilation. Those checks run separately from native MSVC.
