# Motion Capture Camera ART — Recorder

Enregistreur C++ de flux DTrack (ART) avec visualisation optionnelle.

## Prérequis

- **CMake ≥ 3.25** (4.x recommandé)
- **Ninja** + **MinGW-w64 (g++)** — configuration par défaut
  ```powershell
  winget install Kitware.CMake
  winget install Ninja-build.Ninja
  winget install MSYS2.MSYS2   # puis : pacman -S mingw-w64-ucrt-x86_64-gcc
  ```
- *Optionnel* : **Visual Studio 2022** (ou Build Tools) avec la charge "Développement Desktop en C++" pour les presets `vs2022-*`
- **git** (pour le submodule)

## Installation

```powershell
# Cloner avec le submodule DTrackSDK
git clone --recursive https://github.com/tintin75011/Motion_Capture_Camera_ART_Recorder.git
cd Motion_Capture_Camera_ART_Recorder

# Si le clone a été fait sans --recursive :
git submodule update --init --recursive
```

## Build

Lister les configurations disponibles :
```powershell
cmake --list-presets
```

Configurer + compiler (Ninja / MinGW, Release) :
```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Variante Debug :
```powershell
cmake --preset ninja-debug
cmake --build --preset ninja-debug
```

Avec Visual Studio 2022 :
```powershell
cmake --preset vs2022-release
cmake --build --preset vs2022-release
```

## Utilisation

```powershell
.\build\record.exe [hostname] [port]
# ex: .\build\record.exe 192.168.1.10 50105
```

## Tests

```powershell
ctest --preset ninja-release
```

## Structure du projet

```
Motion_Capture_Camera_ART_Recorder/
├── CMakeLists.txt        # point d'entrée CMake
├── CMakePresets.json     # configurations de build prédéfinies
├── src/
│   └── record.cpp        # exécutable enregistreur
├── DTrackSDK/            # submodule — SDK ART (ne pas modifier)
├── build/                # généré (Ninja) — ignoré par git
└── build-vs/             # généré (VS) — ignoré par git
```

## Notes


- Le SDK est épinglé à un commit précis via `.gitmodules` : mettre à jour avec
  ```powershell
  git submodule update --remote DTrackSDK
  ```

