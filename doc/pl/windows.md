# Przygotowanie maszyny Windows do testów

Przygotuj VM z Windows x64, zwykłym kontem użytkownika, dostępem do internetu
na czas instalacji i katalogiem roboczym z prawem zapisu, np. `C:\src`.
Istniejąca VM ma 4 vCPU i 4 GB RAM; na czas instalacji i budowania zamknij
pozostałe programy.

## Narzędzia do budowania

1. Pobierz [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe).
   Zaznacz **Programowanie aplikacji klasycznych w języku C++**. Uwzględnij
   narzędzia MSVC v143 x64/x86 i Windows SDK. Samo IDE jest opcjonalne.
   [Lista komponentów Microsoft](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)
   opisuje ten zestaw. Odpowiednik w terminalu administratora:

   ```powershell
   .\vs_BuildTools.exe --wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
   ```

2. Zainstaluj [CMake](https://cmake.org/download/) w wersji co najmniej 3.22
   i zaznacz dodanie narzędzi do PATH. Pakiet zawiera CTest. Używamy generatora
   Visual Studio, więc Ninja jest opcjonalna; zestaw `vscode_workspace` buduje
   tymczasową kopię przez Ninja, gdy jest dostępna, a inaczej przez NMake
   z Developer PowerShell (zobacz [VS Code](vscode.md)).
3. Zainstaluj [Git for Windows](https://gitforwindows.org/) z dostępem z terminala.
4. Wykonaj restarty wymagane przez instalatory i otwórz nowy terminal.

Natywne testy biblioteki nie wymagają Pythona, WSL, SDK mikrokontrolerów ani
toolchainów firmware. Unity pochodzi z rewizji przypiętej
w `third_party/unity_version.conf`; paczka transferowa zawiera jego źródła
w katalogu `tools`.

## Dostęp zdalny

OpenSSH Server pozwala wykonywać polecenia, przesyłać źródła i odbierać logi
bez sesji pulpitu. Instrukcja:
[instalacja OpenSSH według Microsoft](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse).
W PowerShellu uruchomionym jako administrator wykonaj:

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
Get-Service sshd
```

Do buildów użyj zwykłego konta. Dodaj przekazany klucz publiczny do jego
`authorized_keys`. Lokalizacje plików i zasady uprawnień opisuje
[zarządzanie kluczami według Microsoft](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement).
Konta administratorów używają innej lokalizacji kluczy. Nie przesyłaj haseł
ani kluczy prywatnych w czacie.

Port TCP 22 musi być osiągalny z hosta VM. Domyślna sieć libvirt używa adresu
hosta `192.168.122.1`; sprawdź go przed opcjonalnym ograniczeniem reguły zapory
utworzonej przy instalacji SSH:

```powershell
Set-NetFirewallRule -Name OpenSSH-Server-In-TCP -Enabled True -RemoteAddress 192.168.122.1
```

## Kontrola przed przekazaniem dostępu

W nowym, zwykłym terminalu PowerShell wykonaj:

```powershell
git --version
cmake --version
ctest --version
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Get-Service sshd
ipconfig
```

Przekaż adres VM, nazwę konta, odcisk klucza hosta i ścieżkę katalogu roboczego
z prawem zapisu. Potwierdź logowanie przez SSH z hosta. Kompilator nie musi być
w PATH zwykłego terminala: runner sam ładuje moduł developer shell Visual Studio.

## Uruchomienie testów

Prześlij kompletną migawkę sprawdzanych źródeł wraz z fixture liczb HAL
i przypiętymi źródłami Unity wskazanymi przez kontrole zależności.
Na Linuksie `./scripts/package_windows.sh` tworzy
`.build/windows-transfer/jhbase-windows-source.tar.gz` z bieżącymi własnymi
źródłami, fixture i licencją HAL, przypiętymi źródłami Unity w katalogu
`tools` i manifestem SHA-256. Pliki ignorowane, cache buildów i lokalne klucze
dostępu są pomijane. Rozpakuj archiwum przez windowsowe `tar` do katalogu
z prawem zapisu; paczka ma już wymagany układ sąsiednich katalogów.

Z katalogu `jhbase` uruchom:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\runalltests.ps1 -HalRoot C:\src\JaszczurHAL -ToolsRoot C:\src\tools
```

Runner buduje i uruchamia te same zestawy CTest dla x64 i Win32. Jedną
konfigurację można wybrać przez `-Architecture x64`. Raporty, w tym JUnit XML,
powstają w `.build/msvc-x64` i `.build/msvc-Win32`. Błąd konfiguracji, buildu
lub testu przerywa przebieg; żadna niesprawna konfiguracja nie jest pomijana.

[Opis testów Linuksa](testing.md) obejmuje sanitizery, Valgrind, fuzzing
i kompilację freestanding. Te kontrole działają osobno od natywnego MSVC.
