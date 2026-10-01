# Build Instructions

## MCWiiU Launcher (Windows x64)

Build the launcher with its in-process Cemu persistent core using these presets.
The runtime reaches Ready without launching Minecraft or creating a game window.

Prerequisites:

- Native Git for Windows and network access for dependency downloads.
- Visual Studio 2022 17.14+ / MSVC 19.44+ with Desktop development with C++,
  C++ CMake tools and Windows 11 SDK 10.0.22621+.
- CMake 3.31 or newer.
- Node.js 22.12 or newer and npm on PATH (`node` and `npm.cmd`).
- Microsoft Edge WebView2 Runtime to run the executable. Saucer downloads its
  pinned WebView2 SDK during the build; no separate SDK install is needed.

Prepare the fork, then run commands from its repository root:

```powershell
git clone --recursive https://github.com/IGNSeed/MCWiiU-Launcher.git
cd MCWiiU-Launcher
# For an existing checkout:
git submodule update --init --recursive

cmake --preset launcher-debug
cmake --build --preset launcher-debug

cmake --preset launcher-development
cmake --build --preset launcher-development

cmake --preset launcher-release
cmake --build --preset launcher-release
```

Use native Git/CMake rather than MSYS tools. CMake selects Visual Studio 2022 x64;
these commands do not require a Developer Command Prompt.

In preset notation, `sourceDir` is the repository root. Generated files belong
under `${sourceDir}/../_work`, outside the repository:

| Preset | Configuration | Executable relative to the repository |
| --- | --- | --- |
| `launcher-debug` | Debug | `../_work/build/runtime-debug/bin/Debug/MCWiiU Launcher.exe` |
| `launcher-development` | RelWithDebInfo | `../_work/build/runtime-relwithdebinfo/bin/RelWithDebInfo/MCWiiU Launcher.exe` |
| `launcher-release` | Release | `../_work/build/runtime-release/bin/Release/MCWiiU Launcher.exe` |

Launcher presets enable reusable Cemu libraries, disable the legacy executable
and wxWidgets, and use the project-owned `x64-windows-static-md` triplet (static
libraries with dynamic CRT, /MD or /MDd). Legacy Cemu retains its separate /MT
build. Use fresh `runtime-*` trees; Architecture v1 shell caches are preserved.
The first integrated configure builds Cemu dependencies and takes substantially
longer than the shell build. See [Runtime Foundation](docs/architecture/runtime-foundation.md)
for initialization, dedicated LocalAppData paths and process lifetime limits.

Debug and Development expose the DevTools button and binding; Release exposes
neither. Saucer v8.0.5's WebView2 `set_dev_tools(true)` both enables tools and
opens their window.

Saucer is pinned by commit and fetched to the external build tree. CMake stages
frontend source there, runs `npm ci` and TypeScript/Vite, then embeds the assets
with Saucer's official helper. Do not run npm install/build in
`launcher/frontend`: dependencies, `dist`, caches and embedding sources must
stay external. No frontend dev server is needed.

Repository-internal build trees are rejected by `ExternalOutput.cmake`, even
when ignored. Tracked `bin/` and `dist/` contain upstream resources, not new
output. Machine-specific CMakeUserPresets.json and AGENTS.md stay local-only.
Final distributions belong in sibling `_artifacts`.

In VS Code CMake Tools, select a launcher configure/build preset. If the workspace
root is the parent directory, set its local `cmake.sourceDirectory` to the
`MCWiiU-Launcher` child. Keep user-specific settings local.

## Existing Cemu verification (Windows)

The wxWidgets `CemuBin` remains independent. Cemu stays C++20; Saucer and the
application target use C++23. With the same CMake/MSVC prerequisites and
initialized submodules:

```powershell
cmake --preset cemu-development
cmake --build --preset cemu-development
```

Output is
`../_work/build/cemu-relwithdebinfo/bin/RelWithDebInfo/Cemu_relwithdebinfo.exe`,
with adjacent `resources` and `gameProfiles` copied from tracked upstream data.
The first configure installs many dependencies and may take substantial time.

`ExternalVcpkg.cmake` clones the exact submodule revision to
`<binaryDir>/vcpkg-source`. The pinned vcpkg CMake toolchain automatically
bootstraps there if its executable is absent, before manifest installation.
No manual bootstrap in `dependencies/vcpkg` is required. Later configures reuse
the executable. Downloads/binary caches use sibling `_work/cache/vcpkg`.
Existing Cemu CI bootstraps its external checkout early because NuGet credential
setup needs it before CMake; CMake then reuses that executable.

To verify a new build tree while preserving an existing build:

```powershell
cmake --preset cemu-development -B ../_work/build/cemu-validation
$env:VCPKG_FORCE_DOWNLOADED_BINARIES = '1'
$env:VCPKG_DOWNLOADS = [IO.Path]::GetFullPath('../_work/cache/vcpkg/downloads')
$env:VCPKG_DEFAULT_BINARY_CACHE = [IO.Path]::GetFullPath('../_work/cache/vcpkg/binaries')
cmake --build ../_work/build/cemu-validation --config RelWithDebInfo --target CemuBin
```

Do not change submodule revisions to troubleshoot failures. Inspect
`<binaryDir>/vcpkg-bootstrap.log`, `vcpkg-manifest-install.log`, and
`vcpkg-source/buildtrees/<port>/`. After an intentional vcpkg revision update,
use a new external tree; a different pinned checkout is rejected.
If the source submodule is shallow (for example in CI), the helper fetches full
history at the pinned revision into the external checkout only. Versioned ports
need this history to find the manifest's baseline. The source submodule and its
revision are preserved.

Build presets inherit their configure preset's vcpkg environment. A direct
`cmake --build <directory>` command needs the same environment, as shown above,
if automatic regeneration runs; this also avoids picking MSYS CMake from PATH.
Launcher CRT validation rejects LNK4098/LNK4286/LNK2038. The separate legacy
build retains /MT; fresh builds of both policies must remain compatible.
Incremental builds can expose upstream shared-PCH PDB warnings (LNK4020);
use a clean build to validate debug information after changing that environment.
Build success does not establish gameplay compatibility.

## Continuous integration

`.github/workflows/build_launcher.yml`, called by `build_check.yml`, independently
configures/builds Development and Release on Windows x64 with these presets.
It installs CMake 3.31.6 and Node 22, downloads Saucer/frontend dependencies in
each clean job, and checks source-tree cleanliness, including ignored files.
Failures fail the job. Successful jobs upload seven-day test executables named
`mcwiiu-launcher-windows-x64-development` and
`mcwiiu-launcher-windows-x64-release`; these are not official releases.

Existing Cemu CI and release packaging are retained. Interactive GUI smoke
checks, including DevTools opening, are separate from CI build validation.

## Legacy / upstream Cemu notes

These notes retain upstream Cemu maintenance guidance. MCWiiU Launcher supports
Windows x64 only. Other platform notes do not extend launcher support and have
not been locally validated on those platforms. Examples also use external trees.

### Linux

Upstream recommends Clang 15+ with C++20 support. Dependency package names vary
with distribution versions:

```sh
# Arch and derivatives
sudo pacman -S --needed base-devel bluez-libs clang cmake freeglut git glm gtk3 libgcrypt libpulse libsecret linux-headers llvm nasm ninja systemd unzip zip wayland-protocols
# Debian / Ubuntu
sudo apt install -y cmake curl clang-15 freeglut3-dev git libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libtool nasm ninja-build
# Fedora
sudo dnf install bluez-libs-devel clang cmake cubeb-devel freeglut-devel git glm-devel gtk3-devel kernel-headers libgcrypt-devel libsecret-devel libtool libusb1-devel llvm nasm ninja-build perl-core systemd-devel wayland-protocols-devel zlib-devel zlib-static
```

From this fork's root, with submodules initialized:

```sh
cmake -S . -B ../_work/build/cemu-linux -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang-15 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-15 -G Ninja
cmake --build ../_work/build/cemu-linux --target CemuBin
```

Use actual installed compiler paths. GCC can be selected with
`-DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++`.
For debug, use a separate external tree and `-DCMAKE_BUILD_TYPE=Debug`.
Output is `<binaryDir>/bin/<configuration>/Cemu_<configuration>`, with the
output-name configuration suffix lowercase.

### macOS

Upstream requires an Xcode/LLVM toolchain supporting C++20, CMake and Ninja:

```sh
brew install automake boost cmake git libtool nasm ninja pkgconf
```

Upstream's private-API MoltenVK setup is required for full Vulkan functionality.
Stage its download outside the repository:

```sh
mkdir -p ../_work/cache/molten-vk
cd ../_work/cache/molten-vk
curl -L -O https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.1/MoltenVK-macos-privateapi.tar
tar xf MoltenVK-macos-privateapi.tar
```

Install the dylib using the archive layout and host system library path.
Homebrew's non-private MoltenVK has upstream rendering limitations.
Return to this fork's root before configuring:

```sh
cmake -S . -B ../_work/build/cemu-macos -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build ../_work/build/cemu-macos --target CemuBin
```

Output is `../_work/build/cemu-macos/bin/Release/Cemu_release`.
Add `-DMACOS_BUNDLE=ON` for `Cemu_release.app` in that directory.
Use `-DCMAKE_MAKE_PROGRAM` if Ninja discovery fails.

### FreeBSD

Upstream support is experimental and requires a C++20-capable toolchain.
Several Linux-specific features are unavailable. Upstream reference packages:

```sh
sudo pkg install boost-libs cmake-core curl glslang gtk3 libzip ninja png pkgconf pugixml rapidjson sdl2 wayland wayland-protocols wx32-gtk3 xorg zstd
cmake -S . -B ../_work/build/cemu-freebsd -DCMAKE_BUILD_TYPE=Release -DENABLE_BLUEZ=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_FERAL_GAMEMODE=OFF -DENABLE_HIDAPI=OFF -DENABLE_VCPKG=OFF -G Ninja
cmake --build ../_work/build/cemu-freebsd --target CemuBin
```

This is not a verified dependency recipe for the fork. Use an external install
prefix: `cmake --install ../_work/build/cemu-freebsd --prefix ../_work/staging/cemu-freebsd`.

### Cemu configure flags

These apply to the existing runtime, not new launcher product features.
See root `CMakeLists.txt` for authoritative defaults.

| Flag | Purpose |
| --- | --- |
| `CEMU_BUILD_RUNTIME` | Existing runtime/wxWidgets executable (default ON) |
| `MCWIIU_BUILD_LAUNCHER` | Windows x64 shell (default OFF; launcher presets enable it) |
| `ALLOW_PORTABLE` | Upstream portable runtime data mode |
| `CEMU_CXX_FLAGS` | Additional Cemu compiler flags |
| `ENABLE_VCPKG` | Runtime dependency package manager |
| `ENABLE_CUBEB`, `ENABLE_DIRECTAUDIO`, `ENABLE_XAUDIO` | Audio backends |
| `ENABLE_HIDAPI`, `ENABLE_SDL`, `ENABLE_DIRECTINPUT`, `ENABLE_XINPUT`, `ENABLE_LIBUSB` | Input/device backends |
| `ENABLE_OPENGL`, `ENABLE_VULKAN` | Graphics backends |
| `ENABLE_WXWIDGETS` | Existing wxWidgets UI, currently required by Cemu |
| `ENABLE_DISCORD_RPC` | Upstream Discord integration |
| `ENABLE_BLUEZ`, `ENABLE_FERAL_GAMEMODE`, `ENABLE_WAYLAND` | Linux integrations |
| `MACOS_BUNDLE` | macOS application bundle |

Preserve pins when updating this feature branch. Fetch/review upstream changes
separately, then initialize submodules for the selected revision. Never resolve
build errors by deleting user changes or generating files in the repository.

See [Architecture v1](docs/architecture/architecture-v1.md) for runtime boundaries,
privacy rules and future integration requirements.
