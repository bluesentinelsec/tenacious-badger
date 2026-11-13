# Tenacious Badger: Project Vision (Draft)

## Names

| What | Name |
| --- | --- |
| Project | **Tenacious Badger** |
| Engine shared library | `badger` (`badger.dll`, `libbadger.so`, `libbadger.dylib`) |
| Showcase game executable | `burrow` (`burrow.exe`), the graybox FPS |
| Public symbol prefix | `tb_` for functions and types, `TB_` for macros and constants |

## Purpose

Build a high-performance 3D game engine (`badger`) and a showcase game (`burrow`) in **C17**. Use only **native platform APIs** and **DirectX** on Windows, and roll our own code for everything else. The project is a deliberate practice ground for Windows-native game programming and modern GPU features: hardware ray tracing, path tracing, DLSS, FSR, XeSS and low-latency modes.

Windows comes first, but the deeper practice is building software that scales cleanly to other platforms. In effect, this is our own SDL: a platform layer and engine where we control every design decision and every implementation. Linux (including the Steam Deck) and macOS start as **fatal stubs** behind the same interfaces as Windows (see [Platform stubs](#platform-stubs)). Adding them later means implementing those interfaces, not rewriting the core.

The engine is general-purpose: it must support first-person and third-person 3D, isometric RTS and 2D games. A **graybox first-person shooter** is the first game target. It keeps the engine honest: every feature must serve something playable.

## Guiding Principles

1. **C and native APIs only.** No third-party packages. Anything the platform does not provide (containers, JSON, compression, image/audio decoding, UI) we write ourselves. The project's C idioms (see [Engineering Standards](#engineering-standards)) are binding. They cover warnings as errors, zero-initialization, explicit ownership, arenas, `goto` cleanup, checked arithmetic and Doxygen on public APIs.
2. **Platform code lives behind interfaces. This is non-negotiable.** Every platform- or API-specific symbol sits behind an engine interface. Every interface has an implementation for each target platform, even if that implementation is a **fatal stub**. The build system selects implementations; shared code contains no `#ifdef _WIN32` checks.
3. **Performance by design.** Data-oriented layouts, multi-core execution and an explicit, modern GPU model. Measure before and after optimizing; profiling is built in, not added later.
4. **Thread safety as a requirement.** Most engine subsystems must be safe to use from worker threads. Each public API documents its threading contract.
5. **Borrow proven idioms.** Take patterns from Unreal: modules, an RHI, a render dependency graph, console variables, stat commands, cooking. Take the console and cvars from Quake. Adapt them to C instead of emulating C++ object systems.
6. **Tested foundations.** Test-driven development, deterministic simulation and automated regression checks are evidence of correctness, not optional extras.

## Target Hardware

We design around the hardware we actually own. Capabilities are detected at runtime and selected by tier; they are never assumed.

| Device | GPU class | Path to support | Rendering goal |
| --- | --- | --- | --- |
| Windows desktop | NVIDIA RTX 4090 | D3D12 (primary) | Path tracing + DLSS SR/RR/Frame Gen + Reflex at 4K |
| Linux laptop | AMD, roughly RTX 3060 class | Proton first, then native Vulkan | Hybrid RT + FSR or XeSS at 1080p–1440p |
| Steam Deck | AMD RDNA2 APU | Proton (vkd3d-proton) first, then native Vulkan | Raster + FSR, RT off or minimal, at 800p and 40–60 Hz |
| MacBook Pro | Apple M1 Max | Native Metal backend (later) | Raster + MetalFX upscaling; no hardware RT on M1 |

Running the Windows D3D12 build under Proton on the Steam Deck and the Linux laptop gives us early validation on AMD hardware, long before native Vulkan exists.

**D3D12 baseline.** Shader Model 6.6 with bindless resources (Resource Binding Tier 3) and enhanced barriers where available. **Optional tiers**, detected at runtime:

- DXR 1.1 ray tracing
- mesh shaders
- shader execution reordering and opacity micromaps (the 4090)
- vendor upscalers and frame generation

## Platform Model

### Three outcomes, never confused

| Situation | Behavior |
| --- | --- |
| **Implemented**: the platform or backend supports the operation | It works. |
| **Fatal stub**: the platform or backend is not implemented yet | It logs the interface, function and platform, explains the next step ("Vulkan backend not implemented; run with `-rhi=d3d12`"), then terminates. |
| **Unavailable capability**: an optional feature is missing (no DXR, no `nvngx_dlss.dll`, an unsupported GPU) | A capability query reports it, the engine falls back, and the console and UI say why. This is never fatal. |

Fatal stubs mark missing *work*. Capability queries describe missing *hardware or redistributables*.

### Platform interfaces

`platform/` defines narrow interfaces; `platform/win32/` implements them. `platform/linux/` and `platform/macos/` start as fatal stubs that **compile in CI from day one**, which keeps the gating real.

- **Windowing and events:** windows, DPI, fullscreen/borderless modes, the message pump.
- **Input:** Raw Input for keyboard and mouse, XInput for gamepads (inbox, and what Steam Input exposes under Proton), and raw HID later for other controllers.
- **Time:** high-resolution clocks (`QueryPerformanceCounter`), sleep and frame pacing.
- **Threads and synchronization:** threads, mutexes (SRW locks), condition variables, semaphores, atomics, thread-local storage, affinity, and fibers if we need them.
- **Memory:** reserve/commit/decommit virtual memory, page sizes and large pages.
- **Files and async I/O:** native file handles, overlapped I/O / IOCP, directory watching for hot reload.
- **Dynamic libraries:** `LoadLibraryW`/`GetProcAddress`, mapping to `dlopen`/`dlsym`. Every runtime-loaded vendor DLL goes through this interface.
- **Audio device:** WASAPI output feeding the engine's own mixer, mapping to PipeWire/ALSA and Core Audio later.
- **System information:** CPU topology, memory, OS version, crash dumps and stack traces.

Strings are UTF-8 inside the engine. The Win32 adapter converts to UTF-16 at the boundary and calls only `W` APIs.

### Platform stubs

Every interface needs an implementation on every platform from the first milestone. Windows implements them; Linux and macOS start as fatal stubs that name the native API each one will eventually use. The planned APIs are provisional; we confirm each when its platform is onboarded.

| Interface | Windows (implemented) | Linux and Steam Deck (fatal stub) | macOS (fatal stub) |
| --- | --- | --- | --- |
| Windowing and events | Win32 | Wayland, with X11 to be decided | Cocoa (AppKit) |
| Input | Raw Input, XInput | evdev | Game Controller framework, IOKit HID |
| Time | `QueryPerformanceCounter` | `clock_gettime` | `mach_absolute_time` |
| Threads and synchronization | Win32 threads, SRW locks, condition variables | pthreads | pthreads |
| Memory | `VirtualAlloc`/`VirtualFree` | `mmap`/`munmap` | `mmap`/`munmap` |
| Files and async I/O | Overlapped I/O, IOCP, `ReadDirectoryChangesW` | io_uring or epoll, inotify | kqueue, FSEvents |
| Dynamic libraries | `LoadLibraryW`/`GetProcAddress` | `dlopen`/`dlsym` | `dlopen`/`dlsym` |
| Audio device | WASAPI | PipeWire or ALSA | Core Audio |
| System information and crashes | DbgHelp minidumps, stack walking | `/proc`, signal handlers | `sysctl`, signal handlers |
| RHI backends | D3D12 (implemented); D3D11, OpenGL, software (stubs) | Vulkan, OpenGL, software (stubs) | Metal, software (stubs) |
| Upscaler and latency plugins | DLSS, FSR, XeSS, Reflex, Anti-Lag 2 | Vendor plugins that support Vulkan on Linux (stubs) | MetalFX (stub) |

A missing vendor plugin on a platform where it will never exist (DLSS on macOS, for example) is an *unavailable capability*, not a stub.

## Rendering

### Layers

```
game / ECS systems
   │  render requests (meshes, lights, cameras, sprites)
renderer          — scene representation, render graph, passes, lighting model
   │  RHI calls only
RHI interface     — devices, queues, command lists, resources, pipelines, bindless heaps,
   │                acceleration structures, barriers, timestamps
backends          — d3d12 (implemented) | d3d11 | opengl | vulkan | metal | software (fatal stubs)
```

- **RHI.** The RHI is a function table chosen at startup (`-rhi=d3d12`). It is shaped after explicit APIs (D3D12, Vulkan, Metal) so modern backends map onto it cleanly. Older backends (D3D11, OpenGL) and the software renderer emulate what they lack, or report the capability as unavailable.
- **D3D12 from C.** We use the COM C bindings (`COBJMACROS`/`lpVtbl`) directly, with no C++ shims. A few D3D12 methods that return structs have a different ABI in C; we isolate these in the backend and test them.
- **Render graph.** Each frame declares its passes, their resources and their read/write access. The graph orders work, inserts barriers, aliases transient memory and records command lists in parallel.

### Lighting modes

The renderer offers three modes that share one scene representation:

1. **Raster.** Bindless deferred PBR with temporal anti-aliasing. This mode is required everywhere and is the fallback for every tier.
2. **Hybrid.** The raster G-buffer plus ray-traced shadows, reflections, ambient occlusion and diffuse GI, each toggled independently.
3. **Path tracing.** A reference mode with progressive accumulation (the ground truth for comparisons), then a real-time mode that uses DLSS Ray Reconstruction or our own denoiser.

### Shaders

Each backend has its own hand-written shaders, embedded in the executable as C headers so no shader files ship with the game.

- **D3D12:** HLSL, compiled at build time by the Windows SDK's `dxc` to DXIL. The compiler writes byte-array headers (`-Fh`).
- **OpenGL:** GLSL source strings, compiled by the driver at startup.
- **Metal and Vulkan:** decided when those backends start. The options are MSL source or build-time metallib bytes, and SPIR-V bytes.

Generated headers come from the build, never from hand edits. A shared header of constant-buffer layouts keeps the C and shader views in sync.

### Vendor features

Upscalers, frame generation and latency reducers are **plugins** behind engine interfaces. A plugin loads its vendor DLL at runtime through the platform library interface; when the DLL or hardware is missing, the plugin reports itself unavailable.

- **Upscaler interface.** The renderer supplies jittered color, depth, motion vectors, exposure and reactive masks. Plugins: **DLSS** (Super Resolution, Ray Reconstruction, Frame Generation), **FSR**, **XeSS**, and **MetalFX** later on macOS. A native TAA/upscaler is always available as the baseline.
- **Latency interface:** **NVIDIA Reflex** and **AMD Anti-Lag 2**. Frame generation requires one of them.
- Each vendor SDK's headers, redistributable DLLs and license terms get explicit approval before we adopt them (see Open Questions).

## Engine Architecture

### Modules

The engine is a set of focused modules with explicit startup and shutdown order and declared dependencies, similar to Unreal's modules. Each module exposes a small C API and owns its resources. The modules build into the `badger` shared library; `burrow` and the tools are separate consumers of its public API.

| Module | Responsibility |
| --- | --- |
| `core` | Allocators (arenas, pools, frame allocators), containers, strings and UTF-8, hashing, math (SIMD via compiler intrinsics), logging, asserts and fatal errors |
| `platform` | The interfaces above and their per-OS implementations or stubs |
| `jobs` | Task scheduler and worker threads |
| `ecs` | Entities, components, archetypes, queries and the system scheduler |
| `rhi`, `render` | GPU abstraction and the renderer |
| `audio` | Mixer, voices, 3D spatialization, streaming |
| `input` | Device abstraction and action/axis mapping, with rebinding |
| `vfs` | Virtual file system that mounts directories and package files |
| `assets` | Cooked runtime formats, async loading, reference counting and hot reload |
| `console` | Quake-style console, cvars, commands, config files and `stat` overlays |
| `ui` | Platform-agnostic immediate-mode GUI rendered through the RHI (later the editor's foundation) |
| `serialize` | JSON, binary readers/writers, versioned formats |
| `codec` | Compression, encoding (base64, UTF), checksums and hashes, package obfuscation |
| `media` | Image, audio, font and model decoders, mostly used by tools |

`core` and the other platform-independent modules never include OS or graphics-API headers.

### Gameplay: pure ECS

- Entities are generational IDs. Components are plain C structs stored in **archetype chunks**: contiguous, cache-friendly and easy to iterate in parallel.
- **Systems** are C functions that declare the components they read and write. The scheduler uses those declarations to run independent systems in parallel and order conflicting ones. This is the idea behind Unreal's Mass framework.
- The same ECS serves FPS, action, RTS and 2D games. Genre differences are systems and components (an orthographic camera, sprite rendering, unit selection), not engine forks.
- Engine services (rendering, audio, physics queries) are modules that systems call. They are not entities.

### Threading and the frame pipeline

**Fortnite runs on Unreal Engine 5**, so "Fortnite's threading model" means Unreal's. That model has four parts:

- a **game thread** that runs simulation;
- a **render thread** that builds GPU work for the previous frame's state;
- an **RHI thread** that translates and submits it;
- a pool of **task-graph worker threads** that executes fine-grained, dependency-ordered tasks from all of them, alongside dedicated async loading and audio work.

We emulate that model in C:

1. **Task system first.** A work-stealing task scheduler with explicit dependencies, a bit over one worker per core, and priorities (foreground frame work versus background loading and streaming). Everything parallel runs through it.
2. **Pipelined frames.** Simulation of frame N+1 overlaps render-graph building for frame N and GPU execution of frame N-1. Stages hand off immutable snapshots (render proxies) rather than sharing mutable state.
3. **Parallel inside each stage.** ECS systems, render-graph command recording, culling, animation and asset decoding all split into tasks.
4. **The main thread owns the OS.** It runs the window and message pump, as Windows requires, and forwards input to the simulation.

**Thread-safety contract.** Every public API documents one of:

- *thread-safe*: internally synchronized, or immutable;
- *stage-owned*: called only from a documented stage or thread, and asserted in debug builds;
- *externally synchronized*: the caller serializes access.

Shared mutable state uses atomics or synchronization primitives, never `volatile`.

**Simulation timing (proposed).** A fixed-tick simulation, with rendering interpolated between ticks. A fixed tick gives reproducible behavior, input-recorded replays, regression tests that replay gameplay, and a path to RTS-style lockstep. Determinism is guaranteed for the same build on the same platform; cross-platform floating-point determinism is not a goal at first.

### Tooling and debugging

- **Console first.** A Quake-style drop-down console with cvars, commands, autocompletion, history and `exec`-able config files. Cvars can also be set on the command line.
- **Built-in profiling.** CPU task timelines, GPU timestamp queries per render-graph pass, memory counters per module, and `stat fps`/`stat gpu`-style overlays. A trace capture format can be viewed in-engine later.
- **Debug layers.** The D3D12 debug layer and GPU-based validation in debug builds. PIX markers (PIX loaded at runtime, optional) and named GPU resources.
- **Editor later.** The platform-agnostic `ui` library comes first. A level editor (viewport, outliner, details panel, asset browser) follows, built on the `ui` library and the same console and cvars.

### Assets

- **Source and cooked formats.** Tools import source formats (PNG, glTF, WAV and others) into engine-native cooked formats. The runtime loads only cooked data; this is Unreal's cooking model. All importers and decoders are our own.
- **Packaging.** Cooked assets ship in package files mounted through the VFS. Packages are compressed with our codecs and optionally **obfuscated**. Obfuscation deters casual extraction; it is not security.
- **Loading.** Async streaming through the task system and native async I/O, with hot reload of assets and configuration in development builds.

## Build, Tests and CI

- **Toolchain.** MSVC in C17 mode (`/std:c17 /W4 /WX`, plus `/fsanitize=address` for sanitizer builds) with CMake. CMake also builds the stub platforms on Linux (GCC/Clang) and macOS (Clang), so portability can't silently decay.
- **Build types.** Debug (symbols, no optimization, debug layers), sanitizer (standalone test binaries) and Release (optimized, stripped, no sanitizers).
- **Tests.** A small in-house test harness (no frameworks). It runs:
  - unit tests (strive for 80% source coverage or better);
  - integration tests for module startup and shutdown;
  - D3D12 tests on the WARP software adapter where it supports the feature under test, with golden-image comparisons and tolerances;
  - fixed-tick replay tests for gameplay;
  - tests that every fatal stub fails with a clear message.
  - leave escape hatches in order to test the game loop a specified number of frames.
- **CI.** Windows builds and runs the full suite. Linux and macOS compile everything and run the platform-independent tests and stub checks.
- Every interface, platform requirement and capability tier is documented under `docs/`.


## Releases and Distribution

Release exports are a product feature. Support idiomatic, polished distributions
as users expect from Unreal, Unity, and Godot, with initial export parity with
Unity on **Windows, macOS, and Linux**. Browser and mobile exports follow later;
they must fit the same release model rather than becoming separate projects.

### Desktop exports first

- **Windows:** a runnable game executable with the engine DLL and required game
  data in an ordinary Windows distribution layout, delivered as a ZIP archive
  initially. Include an application icon and native executable version resources
  with product name, file description, company/publisher, copyright, file version,
  and product version. An installer may follow when needed.
- **macOS:** a proper `.app` bundle with `Contents/MacOS`, `Contents/Resources`,
  bundled engine libraries under `Contents/Frameworks`, an application icon, and
  a complete `Info.plist`. Set the display name, executable, bundle identifier,
  semantic version, build version, copyright, minimum OS, and supported
  architectures. Distribute the bundle in an archive or native disk image while
  preserving bundle structure and executable permissions.
- **Linux:** a runnable executable with its engine shared library and game data,
  using relative runtime library paths. Include a desktop entry, application
  icons, and AppStream metadata suitable for desktop integration. Deliver a
  portable archive initially, preserving executable permissions and documenting
  supported architectures and the minimum runtime baseline.

Exports must run after extraction on a supported clean machine without the
source tree, developer SDKs, or a package manager. Bundle project-owned runtime
files and cooked assets in their platform-appropriate locations. Keep developer
tools, tests, build caches, and debug symbols out of the end-user distribution;
publish symbols separately when useful. Validate each exported artifact in CI,
including startup, asset lookup, and engine library discovery.

### Professional metadata and release identity

- Use the author's GitHub alias, **`bluesentinelsec`**, for person, author,
  organization, company, publisher, and maintainer names in release metadata.
  This is a personal project; do not include Amazon or other work-organization
  branding, attribution, templates, or boilerplate.
- Use the actual project and application names for product metadata:
  **Tenacious Badger**, engine **badger**, and showcase game **Burrow**. Use stable
  application identifiers rooted in the personal identity, such as
  `io.github.bluesentinelsec.burrow`, where the platform accepts that format.
- Populate applicable native metadata completely. Do not ship template values,
  placeholder publishers, missing icons, or generic executable descriptions.
- Derive versions from the root `VERSION` file and matching semantic-version
  release tags. Generate platform-specific version fields from that source;
  do not maintain competing version definitions.
- Publish named, versioned, architecture-specific artifacts through GitHub
  Releases, with release notes, checksums, and the Apache-2.0 license and project
  notices. Document the supported OS/runtime baseline and launch instructions.
- Build packaging with our own code and native platform tools, preserving the
  no-third-party-package rule and the explicit approval requirement for external
  dependencies or redistributables.

### Later export targets

After the desktop exports, add **web browser**, **Android**, and **iOS** exports
with the same metadata and release quality. Browser exports need a deployable web
bundle and proper web application metadata; Android needs platform-native app
packages (APK/AAB) and manifests; iOS needs an Xcode project/archive, a native app
bundle, platform metadata, and its normal distribution/signing workflow. Define
and test each target's runtime,
asset, and platform-interface contracts when that export is introduced.

## Engineering Standards

These C and workflow standards are binding for all project code.

### Compiler Behavior
- Enable the strongest practical warning set for each supported compiler, and treat warnings as errors in project-owned code and CI.
- Keep compiler-specific warning flags in compiler-specific build settings.
- Do not suppress a warning silently. If a warning is intentional, explain why and use the narrowest possible suppression; ask the user before suppressing a warning.
- Enable sanitizers when supported by the compiler. Sanitizer builds are intended for testing, so they should usually be standalone development binaries; release builds should not have sanitizer behavior.
- Debug builds must have debug symbols enabled and optimization disabled; release builds should be stripped with optimization enabled (-O3 is preferred when available).

### Variables
- Every variable must be initialized at declaration using zero-initialization (`{0}` or `{}`).
- For user-defined types, prefer a named constant or macro (e.g. `LIST_INIT`) that itself uses zero-initialization; fall back to an explicit `*_init()` function when the zero state is not valid.
- Never shadow variables; it causes confusion and possible scope errors.
- Apply and lean on const correctness.
- Use namespace prefixes for all public symbols (i.e. the `foo_` convention), since C doesn't support real namespaces like C++.

### Memory Allocation
- Initialize allocated objects before use. Use `calloc()` or zeroing only when an all-zero byte representation is valid for the object; otherwise, initialize through the type’s initializer function.
- Use arena allocation when the objects share a clear lifetime. Document or make ownership and reset rules explicit; arena allocation does not remove the need to initialize objects or respect their lifetimes.
- Allocation failures are typically beyond the control of the application; use fail-fast allocation wrappers that check centrally and terminate gracefully to minimize allocation failure checks.
- Use `NULL` only to represent an explicitly optional or absent value. Establish non-null invariants for required pointers at API boundaries, then rely on those invariants internally.

### Error Handling
- Favor `goto` for centralized error handling and cleanup.

### Bounds and Sizes
- Pass buffers together with their lengths; do not rely on a pointer alone to communicate how much memory is valid.
- Use `size_t` for object sizes and array indexes. Check indexes against the relevant length before access, and check size arithmetic for overflow before allocating or computing offsets.

### Testing
- Favor test-driven development: write a focused test for the expected behavior before or alongside its implementation, then keep the test as a regression check.
- Tests should document intent: state specifically what is being checked and why. This can be helpful when deciding whether to deprecate or update a failing test.
- Treat automated tests as essential evidence of correctness. Unit-test business rules and edge cases; use integration tests to check that modules work together.
- Keep tests repeatable and independent of execution order.

### Design Principles
- Give each module one clear responsibility. Keep unrelated behavior out of the same module, and make module boundaries visible in the public API.
- Design stable APIs so new behavior can often be added through new implementations or adapters, rather than by repeatedly modifying established code.
- Apply SOLID deliberately. Treat the Single Responsibility Principle and Open/Closed Principle as mandatory design constraints: give each module one cohesive responsibility, and structure variable behavior so new implementations can be added behind stable interfaces without rewriting established core logic.
- In C, use cohesive modules, opaque types, function tables, and callbacks where they help enforce these constraints.

### Architecture
- Keep core business rules independent of presentation, operating-system, and storage details. Make dependencies point from outer layers toward the core, using interfaces or function tables where the core needs an external capability.
- Isolate platform-specific behavior behind narrow, clearly named platform adapters. Keep each platform’s implementation and build configuration in an explicit, easy-to-find location; select the correct adapter through the build system rather than scattering platform checks throughout shared code.
- Make platform support actionable for both developers and users: document each platform’s required files, dependencies, build steps, and runtime requirements. If a platform or required feature is unsupported, report that clearly and explain the next step.
- Organize the program around three distinct concerns:
  - **Data model:** domain types, state, and invariants.
  - **Business logic:** rules and operations that act on the data model.
  - **Presentation/view:** CLI, GUI, or other front ends that collect input and display results; keep domain rules out of this layer.
- Expose reusable business logic through a library API. Build it as a static library, shared library, or both according to the project’s deployment needs; keep CLI and GUI front ends as separate consumers of that API.

### Formatting
- Format C code with the project’s committed `clang-format` configuration. Run the formatter before review and enforce consistent formatting in CI.

### Public API Documentation
- Document every public function, type, macro, and global symbol; use Doxygen-compatible comments.
- For public functions, document parameters, return values, ownership and lifetime, valid inputs, and error behavior where applicable.
- Document programmer intent for particularly important functions, functions that may be a frequent source of change, and code that is likely to violate the principle of least surprise.

### Package Dependencies
- In my C projects, avoid importing any third-party libraries; rolling and maintaining our own libraries is easier than ever before because of agentic coding. Never import a library without explicit approval from the user.
- Ask the user for approval before fetching and installing any external dependencies.

### Ownership and Lifetimes
- Make ownership explicit for every resource: document who creates it, who releases it, and how long borrowed pointers remain valid.
- Do not retain or return pointers to objects whose lifetime is ending. Do not access an object after it has been freed or its owning arena has been reset.
- Give each resource one clear owner where practical; use borrowing for temporary access.

### Integer and Expression Safety
- Do not rely on signed integer overflow or invalid shifts. Check arithmetic before it can overflow or underflow, divide by zero, or produce an invalid shift count.
- Use `size_t` for sizes and indexes, and fixed-width integer types when a representation must have a specified width. Check conversions when values cross those domains.
- Use `assert()` for internal invariants, not for validating external input or handling runtime failures; assertions may be disabled in release builds.

### Error Contracts
- Give each fallible function a clear, consistent way to report failure. Document which outputs are valid on success and failure, and handle or propagate every failure result.
- Keep cleanup centralized, using the established `goto` cleanup pattern where it fits.

### Concurrency
- If code is accessed by multiple threads, protect shared mutable state with C atomics or synchronization primitives. Do not use `volatile` as a substitute for synchronization.

### Git Workflow
- Submit code changes through a feature branch and pull request; do not commit directly to `main`, even in solo projects.
- Most projects leverage gh for interacting with GitHub, such as creating PRs and querying CI logs.
- Follow the repository’s established branching model. In larger projects that use Git Flow, use its defined branches and release process.
- Prefer squash-merging pull requests into `main`, so each merged PR becomes one focused commit. Keep the PR’s commits and review activity visible until it is merged.

### Commit and History Hygiene
- Commit messages should state the problem that motivated the change; the intent of the change needs to be clear and explicit. Focus on the behavioral changes that result. Do not regurgitate implementation details; that is what the source code is for.
- Do not force-push or rewrite commits that have been pushed to a shared branch. Add a follow-up commit instead; ask before any operation that would rewrite published history.
- Before committing, inspect the working tree and staged diff. Stage only files relevant to the change; do not blindly stage all files.
- Before committing, make sure neither code, comments, nor commit messages contain overly tactical information; comments and documentation should generally be stable until invalidated by later changes or conditions.

### Protecting Existing Work
- Preserve pre-existing working-tree changes. Do not discard, overwrite, reset, clean, or stash them without permission.
- Before merging or rebasing, inspect the branch state and resolve conflicts without dropping unrelated work.
- Give each pull request a clear summary, rationale, and validation notes. Keep it small enough to review, and report any known gaps.

### Version Hygiene
- Follow semantic versioning (non-negotiable).
- During initial development, start versions at 0.1.0. Increment only the minor version to reflect new features, and the patch version for bug fixes or trivial changes. The app will stay on 0.x.x until the author indicates the app is ready for 1.0.0.
- App versions should be defined in ONE location to avoid version drift.

### Clarity and Explicitness
- Optimize for code that a human can understand quickly, not for the fewest lines. Keep functions focused on one task and short enough to follow without jumping across unrelated logic.
- Use descriptive names that express purpose. Prefer positive, direct conditions over hard-to-parse negations or abbreviations.
- Write one meaningful operation per statement. Avoid hidden side effects, chained assignments, comma expressions, nested ternaries, and other compact constructs that make control flow harder to see.
- Always use braces around `if`, `else`, and loop bodies, even when the body contains one statement. In `switch` statements, make fallthrough explicit and intentional.
- Prefer straightforward control flow. Use guard clauses to reduce nesting where they preserve clear cleanup and error-handling paths.
- Flatten nested branches.
- Name meaningful constants instead of embedding unexplained values in expressions.
- Use comments to explain intent, constraints, or non-obvious decisions; do not use comments to narrate code that is already clear (with possible exceptions for public symbols/functions).
- Prefer functions over function-like macros for behavior. Use macros when the C language or build configuration calls for them, and keep their effects obvious.

## Open Questions

Raise the following open questions when they can no longer be deferred:

- **Vendor SDKs.** Which headers and redistributable DLLs we adopt for DLSS (NGX or Streamline), FSR (FidelityFX API), XeSS, Reflex (NVAPI) and Anti-Lag 2. We could hand-declare minimal entry points where the license allows, or vendor the official headers. Each needs explicit approval.
- **D3D12 Agility SDK.** Do we ship Microsoft's redistributable `D3D12Core.dll` to get newer shader models and features (work graphs, SM 6.8+), or stay on the OS runtime?
- **Shader iteration.** Should development builds hot-reload shaders by loading DXC at runtime through the library interface? Do we need a newer DXC than the Windows SDK ships?
- **Physics scope.** Collision queries only, or a full rigid-body solver?
- **Gameplay code iteration.** Do we hot-reload game code as a runtime-loaded DLL, or is there a scripting layer?
- **Fibers.** Are task-system fibers worth adopting after we profile the first scheduler?
