# gbfr-luma Agent Guide

## Project

Experimental GBFR-focused Luma Framework work for improving Granblue Fantasy:
Relink rendering under Windows and Proton/Linux. The current focus is SDR color
correctness while retaining the Luma upscaling path.

## Stack

- Runtime: C++ DirectX 11 ReShade addon
- Framework: Luma Framework and ReShade addon API
- Package manager: None
- Database: None
- Deployment: `dxgi.dll`, addon binary, and `Luma/` shaders in the game folder

## Commands

- Build: Open `Luma.sln` in Visual Studio with the required Windows SDK
- Dev: Run the GBFR project through Visual Studio with ReShade installed
- Test: Build and run the GBFR mod in the target game installation
- Lint: None documented by upstream
- Typecheck: None

## Architecture

- `Source/Core/` owns generic rendering hooks, shader compilation, and display
  composition.
- `Source/Games/Granblue Fantasy Relink/` owns GBFR hooks, upscaling, and
  post-processing behavior.
- `Shaders/` contains development HLSL and dumped game shaders.
- `Luma/` contains runtime shader files and compiled shader caches.
- `.github/` contains upstream build and release workflows.

## Known Traps

- Runtime shader `.cso` files are selected through `Shader#...` hashes in the
  ReShade config; source shader changes must trigger a cache recompile.
- Keep experimental source work in this repository, not in Sidekick or the
  Angel/Wolf `/home/retro` environment.
- The shipped target is DirectX 11 through Wine/Proton on the host AMD GPU.
