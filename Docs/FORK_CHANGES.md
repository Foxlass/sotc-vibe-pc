# Changes in this fork

This fork is [Foxlass/sotc-vibe-pc](https://github.com/Foxlass/sotc-vibe-pc), based on [LightVelox/sotc-vibe-pc](https://github.com/LightVelox/sotc-vibe-pc) at `2607f95`.

The runtime submodule now points at [Foxlass/PS2Recomp](https://github.com/Foxlass/PS2Recomp), branch `sotc-port`. That branch is based on `TaylorNAlbarnaz/PS2Recomp` `sotc-port` at `ac9efa0`.

Every commit message has the full rationale. This file is the index: what changed, where, which settings it adds and how it was checked.

To see the diffs:

```
git diff 2607f95..main                                   # this repo
git -C External/PS2Recomp diff ac9efa0..sotc-port        # the runtime
```

Existing clones need `git submodule sync && git submodule update` to pick up the new submodule URL.

## Results

These are measurements on the developer's machine, with the 60 fps mode and the camera moving.

| | Before | After |
| --- | --- | --- |
| Game updates per second | 43–48 | 49–52 |
| Gaps over 50 ms between shown frames, per run | 15–30 | 1–4 |
| VU1 thread busy | 71% | 50–63% |
| Total VU1 time on the gameplay trace | 100% | 63% |

With `SOTC_FRAME_LOCK=30`, every frame is shown at an even pace.

A steady 60 is not reached yet. The EE (game) thread is now the only limit; see [What's next](#whats-next).

## New and changed settings

| Setting | Default | Where | Effect |
| --- | --- | --- | --- |
| `SOTC_FRAME_LOCK` | `60` | port | `60`/`0`/`off`: the game's own pacing. `30`: hold each flip for two fields, for an even 33.3 ms. |
| `SOTC_FULLSCREEN_REFRESH` | `auto` | port | Monitor rate while fullscreen. `auto`: 59 Hz, then 60 Hz (50 Hz for PAL). `<N>`: that rate. `0`/`off`: keep the desktop rate. |
| `PS2X_GS_PRESENT_VSYNC` | on | runtime | Latch an image at every emulated VBlank and show it on that schedule. `0`: the old behaviour. |
| `PS2X_GS_PRESENT_LATENCY_MS` | adaptive | runtime | Fixed display delay after the emulated VBlank, instead of the adaptive 90th percentile (capped at 66 ms). |
| `PS2X_GS_SHARED_GPU_WAIT` | on | runtime | GPU-side fence waits for shared presentation. `0`: the old CPU polling. |
| `PS2X_VU1_NATIVE` | on | runtime | `0` disables all native VU1 kernels. |
| `PS2X_VU1_EXACT` | off | port | `1`: bit-exact program-00 kernel and no program-05 kernel, instead of the relaxed ones. |
| `SOTC_HEAVY_JOBS` | min(cores/2, RAM/3 GB) | CMake | Parallel compiles of generated game/VU1 code. |
| `SOTC_BUILD_PRIORITY` | below normal | build.bat | `normal`: build at normal CPU priority. |
| `PS2X_BUILD_TEST` | `ON` (was forced `OFF`) | CMake | Configures `vu1_recomp`, `gs_replay` and `ps2x_tests`; they are built only when named. |
| `generate.py --no-vu1` | | tool | Skip VU1 extraction and compilation. |

`SOTC_FRAME_LOCK` and `SOTC_FULLSCREEN_REFRESH` are added to `sotc.ini`'s migrated settings. Files that lack them get them appended. Values already set in the environment take precedence for that session.

## Runtime (PS2Recomp submodule)

### `52a223c` Build the IOP library like the runtime and drop buffer-security checks
- `ps2xIOP/CMakeLists.txt`: `ps2_iop` gets `/Ob2 /Oi /GL /Gy /Gw /GS-` in Release and RelWithDebInfo. The EE calls into the IOP on every scheduler checkpoint, so this lets those calls be inlined.
- `ps2xRuntime/cmake/ReleaseMode.cmake`: `/GS-` for the runtime. The generated game code already used it.

### `f747b52` Cut per-call and per-checkpoint overhead on the EE thread
- `ps2_runtime.cpp`:
  - The branch-trace hook on every guest call and return is now an inlined size check; the real work moved to an out-of-line `pushBranchEventSlow`. It costs nothing when tracing is off. It uses portable `PS2_FORCEINLINE` / `PS2X_RUNTIME_NOINLINE`.
  - `PS2X_EE_SINGLE_LOOKUP` is read once at startup, which removes a static-init guard from every guest branch.
- `EeScheduler.cpp`, `ee_scheduler.h`, `ps2_save_state.cpp`:
  - GS interrupt sources are polled every 256 EE cycles instead of on every checkpoint. Reading the GS thread's CSR pulled its cache line across cores.
  - EE timers are still checked on every checkpoint.
  - The poll cycle is reset on reset and when a save state loads.

### `1c17caf` Switch the monitor to the field rate while fullscreen
- New `ps2_host_display_mode.cpp` (API in `ps2_display_options.h`):
  - picks a mode with the same resolution and depth at the preferred rate, else the fallback rate;
  - switches with `ChangeDisplaySettingsExW(CDS_FULLSCREEN)`, which is temporary and never written to the registry;
  - restores the desktop mode when leaving fullscreen, closing the window or exiting (also registered with `atexit`).
- The switch is off unless the port sets a rate. The Win32 code is `#if _WIN32` only.
- `PS2Runtime::run()` calls it around the borderless toggle and at shutdown.

### `e19017e` Shared presentation: fix multi-second freezes, show frames on the VBlank schedule

**Freeze fix (`gs_gl.cpp`, `gs_gl.h`)**
- The cause: `glClientWaitSync` makes NVIDIA's threaded driver drain its queue, even with a zero timeout. Polling every slot's fences on each present stalled the GS worker for milliseconds per frame.
- The window's release fence is now waited for with `glWaitSync` on the GPU.
- Slots the window has already taken or skipped are reused without any fence query.
- With 6 unseen frames, or no free slot, the worker waits (up to 100 ms) for the oldest one. Without this bound, every slot could be overwritten before its fence signalled, and the window never got a frame.

**VBlank latching**
- New `gs_present_clock.h/.cpp`: a lock-free ring that maps field number to the host time of its VBlank.
- At `VBlankStart` the EE latches a presentation for that field:
  - with the async VIF/GIF pipeline, as a new `PresentMarker` item, which the GIF thread runs after all GS data queued before it (`ps2_async_vif.cpp/.h`);
  - otherwise synchronously on the EE thread.
- The field number travels through `GSPresentationRequest::vsyncTick` and `PublishShared` into `SharedFrame::vsyncTick` (`gs_frontend.cpp`, `gs_gpu_backend.cpp`, `gs_shared_present.h`).
- `UploadFrame` no longer latches on the window thread in this mode.
- The window's `PresentPacer` shows a frame at its VBlank time plus a latency. The latency is the 90th percentile of the last 128 measurements of how late frames became ready, capped at 66 ms.

### `918139f` VU1: statically extracted programs and hooks for native kernels
- `ps2_vu1_core.cpp` and `ps2_vu1_impl.inl`:
  - `registerVu1CompiledPrefixProgram` selects a compiled program when micro memory *starts with* its bytes. One FNV-1a pass checks every registered length, and the longest match wins. Whole-image hashes are tried first, as before.
  - A one-time warning is printed for images that match nothing.
  - `findCompiledVu1ProgramForImage` exposes the same lookup to tools.
- `registerVu1NativeEntry` and `VU1InterpreterAccess::Native<Key>` let a port attach a hand-written kernel at a program's entry PC. The kernel returns `Continue` (resume compiled code at its new PC) or `Fallback` (state untouched). `PS2X_VU1_NATIVE=0` turns it off.
- `PS2X_VU1_VERIFY` with native kernels:
  - cycles, ready tables and XGKICK timing are not compared, because the kernels only estimate them;
  - everything else is still compared bit for bit;
  - the cycle-estimate error is reported, and `printVu1VerifySummary()` prints the totals.
- `ps2xTest/tools/vu1_recomp.cpp`:
  - accepts `vu1prog_*.bin` (any multiple of 8 bytes) and compiles only `[0, length)`. A branch or E-bit in the last pair falls back to the interpreter;
  - emits `registerVu1CompiledPrefixProgram` for these programs;
  - compiles duplicate inputs once.

## Port (this repository)

### `1304639` Build tooling: reliable MSVC setup, throttled heavy compiles, tools buildable
- `Tools/win/vsenv.cmd`:
  - finds VS 2022 with vswhere (any edition, including Build Tools; `VSROOT` still overrides);
  - picks the newest Windows SDK that has the UCRT headers;
  - clears any leftover compiler environment first;
  - checks that `crtdbg.h` is on `INCLUDE`, and prints the vcvars log if setup fails.
- `build.bat`: re-runs itself at below-normal priority (`SOTC_BUILD_PRIORITY=normal` opts out) and returns the build's exit code.
- `CMakeLists.txt`:
  - Ninja job pools: `sotc_heavy_compile` for generated game/VU1 code, and `sotc_heavy_link=1` for the `/LTCG` link;
  - `PS2X_BUILD_TEST` is an option that defaults to `ON`.
- `Port/CMakeLists.txt`:
  - missing generated code is a warning, not a fatal error, so a fresh clone can build `ps2_recomp`/`vu1_recomp` first;
  - the heavy targets use the job pools;
  - `register_functions.cpp` compiles with `/Od /Ob0` and no PCH, because it took MSVC over an hour at `/O2` for no benefit.

### `072729c` Point PS2Recomp at the Foxlass fork
- `.gitmodules` now uses `https://github.com/Foxlass/PS2Recomp.git` (`sotc-port`), and the submodule points at `918139f`.

### `1a7632d` Generate VU1 programs from the ELF instead of run-time captures
- New `Tools/vu1_extract.py`:
  - finds MANAGER's VU1 microcode table in `sotc_linked.elf` (13 entries on SCES-53326 v1.00);
  - decodes each DMA/VIF packet's MPG payloads;
  - writes `vu1prog_<index>_<vaddr>.bin` and a `manifest.txt`.
- The bytes come from the user's own disc; nothing from the game is committed.
- `Tools/generate.py` runs the extraction, then `vu1_recomp`, and syncs the result into `Port/generated/vu1`.
  - It needs `build.bat vu1_recomp` first; `--no-vu1` skips the step.
  - `sync_tree` creates the destination folder and ignores subdirectories.

### `4d9f21d` Frame pacing: `SOTC_FRAME_LOCK=30/60` and fullscreen at the field rate
- New `Port/src/sotc/frame_pacing.cpp/.h` documents how the game paces itself. The main loop sets the flip request (`0x1DC810`) and blocks on an unbuffered message queue. The GP thread (`sub_001BE700`) accepts the flip at a VBlank once `iosCheckDrawFinish` reports the GS idle.
- `SOTC_FRAME_LOCK=30` wraps `iosCheckDrawFinish` (`0x1BD8E8`): for GP-thread calls (`ra == 0x1BE798`), a flip is held until two fields have passed. `60` installs no hook.
- A mailbox-style mode (accepting frames between VBlanks) was tried and measured no better, because the game's time step is a whole number of fields.
- `main.cpp` maps `SOTC_FULLSCREEN_REFRESH` onto the runtime's fullscreen refresh API.
- `config.cpp` adds both settings to the migrated defaults.

### `eb45add` Native VU1 kernels for the hottest strip routines (programs 00 and 05)
- `Port/src/vu1_native/sotc_vu1_native.cpp`, program 00 (world geometry):
  - the strip kernel at entries `0x05B0`/`0x0628`;
  - the triangle clipping path (`0x1410`–`0x14D8`: winding cull, five-plane Sutherland-Hodgman, transform and XGKICK).
  - Two variants: exact (`PS2X_VU1_EXACT=1`), using the interpreter's arithmetic helpers and full flag bookkeeping; and relaxed (default), plain SSE with round toward zero and FTZ/DAZ.
  - The relaxed variant gives up two things, and no SotC program depends on either: memory operands with an all-ones exponent, and under/overflow and sticky flags.
- `Port/src/vu1_native/sotc_vu1_skin.cpp`, program 05 (skinned characters):
  - routine `0x10E0` (entries `0x0598`/`0x0920`/`0x09B8`): bone blending, screen transform, lighting, fog, clip test and per-triangle classification, transcribed pair by pair from the software-pipelined loop;
  - relaxed only;
  - hands back at `0x14E8`, or at `0x14C8` when the strip needs clipping.
- Settings are read once at registration. A per-run `getenv` had made the first version slower than the compiled code.
- `Port/CMakeLists.txt` builds both files into `sotc_vu1_code`. `main.cpp` registers them after the generated programs.
- `Port/tools/vu1_bench.cpp`:
  - `--compare` classifies differences against the interpreter (identical / flags only / value-level / control flow), with per-field maxima and histograms;
  - `--tsc` adds `__rdtsc` timing;
  - `BENCH_NO_NATIVE` skips the kernels and `BENCH_CYCLE_LOG` logs cycles per run;
  - `--verify` prints the verify summary.
- New `Tools/vu_disasm.py`: a VU micro-program disassembler.
- **Verification (`sotc_vu1_bench --verify` on the recorded gameplay trace):**
  - all 9,261 runs are bit-identical to the interpreter, including the 808 that use the skin kernel;
  - the cycle estimate is within 1.6%;
  - the skin entry points are 4.3× faster, and total VU1 time is 37% lower.

## How the final state was checked
- `build.bat sotc` (RelWithDebInfo, MSVC) builds the final tree.
- The commits were split by topic from one working tree, and only the final tree was built. Each intermediate commit touches separate code, but none was built on its own.
- The results above come from in-game runs and `sotc_vu1_bench` during development, not from a CI run.

## Not included
- A Vulkan-Headers/Vulkan-Hpp `FetchContent` block (with `cmake_minimum_required 3.22.1`) exists locally but is not committed. Nothing uses it yet, and Vulkan work waits until the OpenGL path holds 60.
- A faster DMA chain copy was tried and reverted, because it was slower.

## What's next
1. **EE thread speed, the obstacle to a steady 60.**
   - The recompiler's output: it stores the PC on almost every instruction, keeps registers in memory, and sends every call through the dispatcher.
   - IOP/SPU2 emulation: about 8% of the game thread.
   - Native rewrites of the hottest game functions, once profiled.
2. Remaining VU1 kernels: program 05 `0x0D10`/`0x0ED0` and program 00's other entries.
3. Longer term: VU1 work as GPU shaders, and GS writes as direct draw calls.
