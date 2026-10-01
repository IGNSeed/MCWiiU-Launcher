# Runtime Foundation (Phase 2)

## Purpose and scope

MCWiiU Launcher now links and initializes the real Cemu persistent core inside
the launcher process. `Ready` means that initialization returned successfully;
it does not mean a title, renderer, audio device or controller is running.
Minecraft import, validation and launch, native game windows, rendering, audio,
controller discovery, mods, textures, accounts, network providers, updater and
installer remain future work. There is no world or save management UI.

## Ownership and dependencies

```text
React UI -> Saucer binding -> LauncherBridge -> MinecraftRuntime
    -> CemuRuntimeBackend -> reusable Cemu core libraries
```

Saucer creates the application first. Its owner/main lifecycle thread constructs
`MinecraftRuntime` and synchronously calls `Initialize()` before showing the UI.
The facade owns the backend directly; the backend owns its Cafe implementation.
The facade and bridge outlive the WebView, then RAII shuts the core down before
the Saucer application is destroyed. Runtime failure leaves the facade in `Error`
and allows the existing status card to display it when WebView2 remains usable.
Failure to create the Saucer application/window/WebView is a startup error.

There are no UI or bridge initialize, shutdown or restart operations. Every
facade allows only one initialization attempt, and a backend guard prevents a
second core attempt anywhere in the process. Shutdown is idempotent. A second application process may initialize
normally using the same on-disk data; restarting the core in one process is not
supported. `IsIntegrated()` means the real backend is linked, including when its
initialization fails. Bridge protocol and response schema remain version 1.

## LauncherPaths and ActiveSettings

`ResolveLauncherPaths()` uses Windows `FOLDERID_LocalAppData` and the actual
executable path. It never changes the working directory or process priority.
`LauncherPaths` is shared by runtime setup and WebView storage resolution.

| Field | Relative to `%LOCALAPPDATA%\MCWiiU Launcher` | Use |
| --- | --- | --- |
| root | `.` | Dedicated launcher root |
| config | `config` | Future launcher configuration |
| runtime | `runtime` | Cemu user data |
| mlc | `runtime/mlc01` | Dedicated MLC |
| cache | `cache` | Cemu cache |
| logs | `logs` | Reserved launcher diagnostics |
| webView | `WebView2` | Saucer storage |
| mods | `mods` | Reserved mods storage |
| textures | `textures` | Reserved texture storage |

ActiveSettings receives `runtime` as userDataPath, `config` as configPath,
`cache` as cachePath and the executable's directory as dataPath. Its unchanged
default MLC resolution therefore selects `runtime/mlc01`. The existing Cemu log
is `runtime/log.txt`; no logging subsystem redesign is introduced.

The launcher uses Cemu configuration defaults. It does not call config Load/Save,
NetworkConfig loading, ActiveSettings online certificate initialization or account
refresh/import. `%APPDATA%\Cemu` and portable Cemu data are never selected,
imported, migrated or deleted. Missing otp/seeprom produces the existing offline
diagnostic and does not prevent Ready. Tests need no Nintendo or game data.

## Shared MLC initialization

`Cemu::Runtime::CreateDefaultMLCFiles()` extracts the old wx application's
skeleton into a C++20 utility in CemuComponents. The old CemuApp method delegates
to it and retains its existing caller-side error dialogs. The launcher converts
failure to `RuntimeError::MLCSetupFailed` / runtime `Error`.

The utility creates sys/usr, base/DLC/update title directories, the three Mii
Maker database directories and the stock language content directory. It retains
the original language order, country list and `NN` -> `NULL` representation.
Existing language/country files are preserved, and subsequent process launches
reuse the skeleton. Directory and stream failures are reported. The existing
random-name write probe replaces the fixed `writetestdummy` probe so a user file
with that name is not overwritten. Concurrent processes initializing the same
new MLC are not a supported installation operation in this phase.

Cemu's persistent PDM module may additionally create its own empty system
play-statistics files under this MLC. These are internal core data, not a save
management feature or a supplied game save.

## Initialization order

1. Create launcher-owned directories and configure ActiveSettings paths.
2. Start the existing Cemu log, recording path setup and initialization milestones.
3. Create/reuse the MLC skeleton and verify writability.
4. Construct the backend's `CafeSystem::SystemImplementation` adapter.
5. Initialize AES128, PPC timer and Cemu exception handling.
6. Attach the adapter and call `CafeSystem::Initialize()`.
7. Mark the backend initialized and transition the facade to Ready.

CafeSystem initializes its existing FSC/memory/PPC/RPL/SysAllocator/IOSU/SI
persistent components, including idle service threads. Internal IOSU ACT service
registration is part of that core, not launcher account import or online login.
NIM's package-list background work begins on a title request, which this phase
does not make. No title is prepared or launched.

The launcher does **not** call CemuCommonInit, driver reconfiguration, global
Vulkan/Latte overlay initialization, audio InitializeStatic, InputManager::instance
or load, GraphicPack LoadAll, TitleList/SaveList initialization/refresh, config
loading or any title lifecycle entry point. Linked audio/input/game code stays
idle. OpenGL/Vulkan source remains enabled because the current core configuration
requires a default graphics API for shared headers and LatteShader.cpp references
Vulkan shader state without a compile guard. There is no Renderer instance or
global Vulkan initialization. SDL, HIDAPI, libusb, cubeb and Discord are disabled
in launcher presets where their compile dependencies permit it.

Saucer is pinned to v8.0.5 commit `68345fa45a7465fe5a62b699b51847b916c8c16a`.
Its `src/win32.app.cpp` owns STA `CoInitializeEx` and `CoUninitialize`. The backend
does not copy legacy Cemu's MTA initialization onto that thread.

Its PackageProject 1.13 dependency is pinned separately to
`738d2ed9dba67e01ac2ea2a6d0f8d35c198fed32`. A tracked FetchContent patch preserves
an already registered alias to the same target and rejects conflicting aliases.
This handles vcpkg's add_library wrapper capturing CMP0107 NEW without changing
vcpkg source, relaxing CMake policy or upgrading Saucer. The patch only touches
the external checkout and verifies its expected source pattern.

## wxWidgets boundary and WindowSystem

CemuGui retains the WindowSystem interface. With wx enabled it links CemuWxGui;
launcher presets disable wx and link `EmbeddedWindowSystem.cpp` through the
backend. No wxWindowSystem/MainWindow/CemuWxGui is compiled into the launcher.
The adapter owns zero-initialized WindowInfo, reports core errors to Cemu logging
and returns inactive window/key/input state. Game/window creation, game
notifications and input capture produce diagnostics without creating a canvas,
TV window or keyboard gameplay support. Cafe canvas/process-exit callbacks
likewise report unexpected use. Phase 3 must replace these inactive behaviors
with explicit native game-window ownership before launching any title.

The GPU initialization flag is owned by CafeSystem rather than the legacy main
translation unit. The existing console helper used by LaunchSettings is shared
through CemuUtil; launcher startup never parses legacy command-line settings or
attaches a console. These small relocations resolve real core link dependencies.

## Shutdown and process lifetime

When initialized, the backend calls the existing CafeSystem::Shutdown, which
stops its supported IOSU services/modules in their existing order. Initialization
tracks completed service/module launches so C++ exceptions unwind only work that
actually started. No title is
running. It then detaches the implementation, releases its adapter and flushes
the existing log. It does not shut down subsystems it never initialized.

Memory/FSC/PPC/RPL globals, SysAllocator storage, exception registrations and the
PPC timer have no fully symmetric public teardown here. The PPC calibration
thread and deprecated ACT/NIM ioctl threads are existing process-lifetime work;
the process reclaims them at exit. There is no fabricated memory teardown or
same-process restart promise. The Cemu logging writer retains its existing
static destructor shutdown. Native fatal failures inside existing core code
(for example memory reservation's exit path) remain a limitation; the facade
can report recoverable setup errors and C++ exceptions, not replace all core
fatal handling in this phase.

## Build and CRT policy

| Configuration | Core | Legacy executable | wx | MSVC CRT | vcpkg |
| --- | --- | --- | --- | --- | --- |
| launcher presets | ON | OFF | OFF | /MD, /MDd | x64-windows-static-md |
| cemu-development | ON | ON | ON | /MT | x64-windows-static |

`CEMU_BUILD_RUNTIME` builds reusable libraries. New
`CEMU_BUILD_LEGACY_EXECUTABLE` defaults ON, preserving standalone builds/CI.
The launcher requires a separate build tree with that option OFF and wx OFF.
`cmake/MSVCRuntime.cmake` selects one CMAKE_MSVC_RUNTIME_LIBRARY policy and the
target helper applies it to all previously hard-coded Cemu/ih264d/cubeb targets.
The parent also overrides ZArchive's explicit static CRT target properties
without modifying its pinned submodule. Other source-built dependencies inherit
the same policy. Saucer uses the dynamic CRT. Windows OpenGL, DirectInput and
DirectSound GUID dependencies are declared by the core targets that use them,
independently of wx link dependencies.

The tracked overlay triplet in `cmake/vcpkg-triplets` selects x64, dynamic CRT
and static libraries through VCPKG_OVERLAY_TRIPLETS. vcpkg source/pins are not
modified. External `runtime-*` build trees avoid stale shell/CRT caches. Launcher
CI checks recursive dependencies, builds integrated Development/Release and
fails on LNK4098/LNK4286/LNK2038; it does not suppress mismatches with NODEFAULTLIB.
Existing Windows/Linux/macOS/AppImage jobs remain enabled.

## Validation and next phase

Build all three launcher presets and cemu-development. Verify real bridge Ready
and integrated=true, missing online data, no game/wx window, normal launcher UI,
Development DevTools and Release absence, dedicated filesystem output, preserved
MLC text, clean shutdown and a second fresh process. Inspect native project/CRT
directives and imports and retain build logs outside the repository. Build and
core smoke evidence do not validate Minecraft, renderer, audio, controller,
multiplayer or hardware behavior. Phase 3 introduces native game-window and
renderer ownership after these boundaries are established.

Local validation includes the actual Windows WebView2 UI in Debug, Development
and Release, with Ready and integrated=true, normal window closure, and a second
fresh Development process. A blocked runtime directory exercises Error while
keeping the bridge and UI usable. Development opens real DevTools; Release
rejects the absent binding. Protocol rejection, transport recovery, reload and
external navigation restrictions are exercised against the running native UI.

An external native harness checks the real MLC utility's stock bytes, existing
file preservation and obstructed paths; it also checks actual Wii U memory
reservation, ActiveSettings paths, idle title/renderer state, repeated-call
rejection and idempotent shutdown. Native thread inspection confirms existing
core services in the launcher PID and absence of excluded subsystem threads.
Project/link inputs and static library directives are inspected for dynamic CRT
and absence of wx. Tests, logs, screenshots and fixtures remain outside source.
