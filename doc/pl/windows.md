# Przygotowanie maszyny Windows do testów

Przygotuj Windows x64 ze zwykłym kontem użytkownika, dostępem do internetu
na czas instalacji i katalogiem roboczym z prawem zapisu, np. `C:\src`.

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

Przygotuj paczkę na Linuksie ze sprawdzanej migawki źródeł. Potrzebuje ona
checkoutu HAL wskazanego przez `JHBASE_HAL_ROOT` (domyślnie `../JaszczurHAL`)
oraz trzech zależności: Unity, utf8proc i zestawu danych do testów normalizacji
Unicode wraz z licencją. Najpierw przygotuj te zależności, a potem je spakuj:

```sh
./scripts/ensure_tools.sh unity utf8proc unicode-test
./scripts/package_windows.sh
```

Wynikiem jest `.build/windows-transfer/jhbase-windows-source.tar.gz`.
Archiwum zawiera źródła jhbase, fixture liczb i licencję HAL, Unity w katalogu
`tools` oraz manifest SHA-256. Ignorowany checkout utf8proc z metadanymi Git,
dane Unicode i ich licencja są dołączane jawnie. Pozostałe pliki ignorowane,
cache buildów i lokalne klucze dostępu są pomijane. Skrypt pakujący sprawdza
zależności, ale ich nie pobiera.

Prześlij archiwum i rozpakuj je przez windowsowe `tar` do nowego katalogu
z prawem zapisu. Paczka ma już wymagany układ sąsiednich katalogów. Nowy
katalog zapobiega wykorzystaniu obiektów z poprzedniej migawki źródeł.

Z katalogu `jhbase` uruchom:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\runalltests.ps1 -HalRoot C:\src\JaszczurHAL -ToolsRoot C:\src\tools
```

Runner buduje i uruchamia CTest z Unicode wyłączonym i włączonym na x64
oraz Win32. Każda konfiguracja zawiera `jb::host`. `-Architecture x64`
wybiera samo x64, zachowując oba ustawienia Unicode. Każdy katalog buildu
zawiera raport JUnit `ctest.xml`:

| Architektura | Unicode | Katalog buildu i raportu |
| --- | --- | --- |
| x64 | OFF | `.build/msvc-x64` |
| x64 | ON | `.build/msvc-x64-unicode` |
| Win32 | OFF | `.build/msvc-Win32` |
| Win32 | ON | `.build/msvc-Win32-unicode` |

Błąd konfiguracji, buildu lub testu przerywa przebieg;
żadna niesprawna konfiguracja nie jest pomijana.

[Opis testów Linuksa](testing.md) obejmuje sanitizery, Valgrind, fuzzing
i kompilację freestanding. Te kontrole działają osobno od natywnego MSVC.
