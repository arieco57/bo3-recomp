# Black Ops III (Xbox 360) — Linux native static recompilation

Goal: run *Call of Duty: Black Ops III* (Xbox 360 version, title ID `4156091D`) natively on
Linux, using **static recompilation** (PowerPC → C++ → x86-64), the approach used by
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp). This is not an emulator.

> You must own the game. Nothing in `extracted/`, `private/`, `content/` or `ppc/` may be
> redistributed (see `.gitignore`).

## Players: install and play (releases)

Download the release for your system (Linux `.tar.gz` or Android `.apk`) - it contains no game
files. On the first run the game installs itself from **your own copy** (`runtime/src/setup.cpp`):

- the disc image (`.iso`: Redump XGD2 / XGD3 or XISO) -> `extracted/game/`
- the Title Update package (console `Content/0000000000000000/4156091D/000B0000/`, STFS) and the
  free **Compatibility Pack 1 + 2** (required) plus any other DLC (`.../00000002/`) -> `content/`
- `XexPatcher` applies the TU to both executables -> `private/<mp|zm>/default_patched.xex`

Everything is verified by SHA-1 (the recompiled code matches exactly that disc + Title Update 1).
Inputs are taken from the data folder, its `instalar/` folder and, on Android, `/sdcard/BO3Recomp`
or Downloads ("all files access" is asked for). User instructions: `docs/LEEME-*.txt`.
`BO3_SETUP_YES=1` skips the dialogs (tests).

## Developers: building, releases, Discord

1. `tools/fetch_deps.sh` - third-party sources (pinned) and the static xma2-only FFmpeg.
2. `tools/prepare.sh` - needs your copy of the game: extracts it and recompiles the executables
   into `ppc/` (generated from the game's code: never committed).
3. `cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build`
4. Releases: `tools/make_release.sh v0.x.y` -> `dist/v0.x.y/` (Linux tarball with static FFmpeg and
   libstdc++ - needs glibc 2.38+ and an AVX2 CPU - and the Android APK). The online backend
   (Supabase URL + publishable key) is compiled in from `release.conf` (not in git); source builds
   have none unless `bo3recomp.ini` sets it.
5. Publishing a GitHub release runs `.github/workflows/discord-release.yml`: it posts the notes to
   Discord #announcements and the Linux / Android download links to their channels (repository
   secrets `DISCORD_WEBHOOK_ANNOUNCEMENTS`, `DISCORD_WEBHOOK_LINUX`, `DISCORD_WEBHOOK_ANDROID`).

## Current status

| Stage | Status |
|---|---|
| Unpack disc image (`.zar` = ZArchive) | ✅ 532 files, 4.2 GB, SHA-256 integrity verified |
| Apply Title Update 1 (`.xexp`) to both executables | ✅ |
| Decrypt/decompress XEX, dump image | ✅ |
| Register save/restore helpers, `setjmp`/`longjmp` | ✅ found and verified by disassembly |
| Jump tables (switches) | ✅ 156 per executable (custom detector, see below) |
| PPC → C++ recompilation | ✅ **0 warnings / 0 unrecognized instructions** (MP and ZM) |
| Generated C++ compiles (Clang 21, -O2) | ✅ 0 errors / 0 warnings → `libBO3RecompLib_MP.a`, `libBO3RecompLib_ZM.a` (~57 MB each) |
| **Runtime** (`runtime/`): memory, loader, threads, sync, files, XAM, offline net | ✅ **both executables reach the engine main loop and run stable at 60 fps** (headless) |
| GPU command processor | ✅ PM4 ring/IBs, waits, fences, scratch writeback, synchronous CP interrupts, constants (SET_CONSTANT/2, LOAD_ALU_CONSTANT), swaps |
| **Renderer (Vulkan)** | 🟡 **title → menus → lobby → loading → in match / Freerun with the 3D world rendered** (lighting, shadows, reflections, post-processing, HUD), ~60 fps in menus, Freerun and MP matches on a Radeon 680M — see "Renderer" below |
| Audio | ✅ XAudio frames (5.1) downmixed to stereo on SDL audio, paced by the device; **XMA hardware decoder emulated** (MMIO register traps + FFmpeg `xma2`) - music, UI and in-game sound |
| Input | ✅ SDL3 gamepad, **keyboard + mouse look** (PC-style layout) → XInput pad 0 |
| Window | ✅ SDL3 (Wayland/X11), resizable, HiDPI, **F1 video options (resolution, fullscreen on/off, saved in `bo3recomp.ini`)**, F11 / Alt+Enter / `--fullscreen` / `--windowed`, letterboxed |
| Video (Bink) | ❌ movies don't advance (async decode path stalls); the boot logo movie is skipped (`BO3_PLAY_INTRO=1` to try it) |

## Layout

```
BKU-Call of Duty - Black Ops III.zar   original disc image (ZArchive)
content/                               Xbox 360 storage dump (TU, DLC, saves) — see below
extracted/game/                        unpacked disc: default.xex, default_zm.xex, *.xpak, snd/, video/ ...
private/{mp,zm}/                       per-executable work dir
    default.xex / default.xexp         base XEX + TU1 delta patch
    default_patched.xex                patched XEX (input to XenonRecomp/XenonAnalyse)
    image.bin, info.txt                decrypted memory image (base 0x82000000) + section map
    imports.txt                        279 kernel/XAM imports the runtime must implement
    recomp_addresses.toml              auto-detected helper addresses
config/BO3_{MP,ZM}.toml                XenonRecomp configs (generated)
config/BO3_*_switch_tables.toml        jump tables (generated)
ppc/{mp,zm}/                           generated C++ (~170 MB each, 144 files)
tools/
    prepare.sh                         runs the whole pipeline end to end
    zar_extract.py                     ZArchive extractor (+ --verify)
    gen_config.py                      writes config/BO3_*.toml
    find_callers.py                    find `bl` callers of an import (used to locate longjmp)
    bo3prep/bo3prep.cpp                patch XEX + dump image + find save/restore helpers
    bo3prep/bo3switch.cpp              scheduling-tolerant jump-table detector
    bo3prep/ppcdis.cpp                 tiny disassembler for image.bin
    patches/xenonrecomp-bo3.patch      instructions added to XenonRecomp (applied by prepare.sh)
    XenonRecomp/                       hedge-dev/XenonRecomp @ ddd128b (+ patch)
```

### What is in the files

* **Two executables**: `default.xex` (Multiplayer + Freerun) and `default_zm.xex` (Zombies).
  The Xbox 360 version has no campaign. They are separate programs → two separate recompilations
  and, eventually, two Linux binaries.
* `content/0000000000000000/4156091D/000B0000/tu00000001_00000000` — **BO3 Title Update 1**
  (`default.xexp`, `default_zm.xexp` + patched fastfiles `*.ff`). Applied on top of the disc XEX.
* `content/0000000000000000/4156091D/00000002/*` — DLC: **The Giant**, **Awakening**,
  **Compatibility Pack 1/2**. Needed later by the runtime's content/DLC mounting.
* `content/E0300000126D662A/4156091D/00000001/*` — offline saves (stats, loadouts, settings).
* Not BO3 (ignored): `4156081C` = World at War TU7 + save, `415608C3` = Black Ops II TU.
* `extracted/game/$SystemUpdate/` — Xbox dashboard system update, not needed.

## Running the pipeline

```bash
tools/prepare.sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j10      # ~27 GB RAM: keep -j moderate, each TU uses 1–2 GB
```

Requirements (Fedora): `clang cmake ninja-build git python3 python3-zstandard`.

## How the addresses were found

* **Save/restore helpers** (`__savegprlr_14` etc.): byte patterns from the XenonRecomp README,
  matched on two consecutive instructions to avoid hitting inlined prologues; exactly one hit each.
* **longjmp**: the only `RtlUnwind` caller that restores f14–f31 from the `jmp_buf` in r7
  (MP `0x82A11500`, ZM `0x82A03720`).
* **setjmp**: the function storing f14–f31, r13–r31 and v64+ into r3 at the same `jmp_buf`
  offsets (MP `0x82A60780`, ZM `0x82A52740`).
* **Exception handler records**: the 8 bytes before every `.pdata` function with the exception
  flag (C++ frame handler, `__C_specific_handler`, 1 custom) are data; listed in
  `invalid_instructions` so the linear sweep doesn't decode them as code.

## Changes needed versus Sonic Unleashed

BO3 was built with a newer XDK compiler, so stock XenonRecomp/XenonAnalyse were not enough:

1. **Jump tables** — XenonAnalyse found 0: it matches fixed opcode sequences, while BO3's compiler
   interleaves the table setup and inserts `nop`s. `bo3switch` instead evaluates registers
   symbolically from the bounds check (`cmplwi` + `bgt`/`bge`) to `mtctr`/`bctr`, handles
   register copies (`mr`), a re-computed index, and mask-bounded indices with offset (Bink code),
   and accepts a table only if every target lies inside the owning function. 401 other `bctr`
   sites are real indirect calls (vtables, function-pointer tables) and correctly stay indirect.
2. **Leaf functions with switches** (no `.pdata`): XenonRecomp's analyser stops at the `bctr`;
   `bo3switch` emits explicit bounds for the 24 affected functions.
3. **56 missing instructions** added to XenonRecomp (`tools/patches/xenonrecomp-bo3.patch`):
   update-form loads/stores (`lfsu stfsu lhzu sthu lbzux lwzux ldux stdux …`), `addc addme subfze
   eqv lhbrx dcbst frsqrte bdzf bdnzt`, VMX (`vsrah vslh vsrh vsl vminsw vminuh vmaxuh vsubuwm
   vsububm vsubshs vsubuws vaddsws vavguh vavgsw vspltish vnor vsel128 vcmpgtsw vcmpgtuw vcmpgtsb
   vpkswss vpkshss vpkuwus128 vpkuhus128 vrfip vcfpuxws128 lvehx lvebx stvebx`).
   Also: `bdnzf` used a hard-coded `eq` CR bit (correct for Unleashed only) — now uses the
   real bit; `vpkd3d128` float16_4 with pack mode 3 accepted (same layout as mode 2 for shift
   0–2, per Xenia); record forms `rldicl.` / `rlwimi.` now update cr0.

   ⚠️ These instruction implementations were written from the PowerPC/VMX128 specs and checked
   against the existing code's conventions, but are **not yet tested at runtime**. Once a
   runtime exists, verify them with XenonTests-style unit tests.

## Runtime (`runtime/src`)

Built per executable (`build/bo3_mp`, `build/bo3_zm`), run from the repository root:

```bash
build/bo3_mp            # [--trace] logs every import call; [--game DIR] [--update DIR]
```

| File | What it does |
|---|---|
| `main.cpp` | entry, function table (`PPC_LOOKUP_FUNC`), crash handler (prints guest address/lr) |
| `memory.*` | 4 GiB guest reservation; virtual heap 0x40000000+, physical heap 0xA0000000+ |
| `loader.*` | loads `private/<m>/default_patched.xex`, resolves kernel *variables* and IAT entries, guest copy of the XEX header + `LDR_DATA_TABLE_ENTRY` |
| `kernel_vars.cpp` | `XboxHardwareInfo`, `KeTimeStampBundle` (updated every 1 ms), `XexExecutableModuleHandle`, … |
| `thread.*` | guest threads on host threads; PCR (r13) / KTHREAD / TLS layout from Xenia; host-driven guest threads for callbacks |
| `kobject.*` | events, semaphores, mutants, timers, handles, named objects, waits (one dispatcher lock) |
| `kernel_sync.cpp` | `ExCreateThread`, `Nt*Event/Semaphore/Mutant/Timer`, `Ke*`, critical sections, spinlocks, TLS, time |
| `kernel_memory.cpp` | `NtAllocateVirtualMemory`, `MmAllocatePhysicalMemoryEx`, … |
| `kernel_file.cpp` + `vfs.*` | `NtCreateFile/ReadFile/QueryDirectoryFile/…`, case-insensitive paths, `game:`/`d:` → TU then disc, `cache:`/`hdd:` → `~/.local/share/bo3recomp`, symbolic links |
| `kernel_misc.cpp` | `sprintf`/`_vsnprintf`/`DbgPrint` (guest varargs), `Rtl*` strings, `Xex*`, `ExGetXConfigSetting` |
| `xam.cpp` | video mode, launch data, one signed-in local profile, notifications, system UI stubs |
| `net.cpp` | offline: static XNADDR, real UDP/TCP sockets **forced to 127.0.0.1** |
| `gpu.cpp` | null Xenos command processor: PM4 ring/IBs, `WAIT_REG_MEM`, `MEM_WRITE`, fences, scratch-register writeback, per-CPU `INTERRUPT`, swap, 60 Hz vsync callback, DPCs, `Vd*`. `BO3_GPU_TRACE=1` logs every packet |
| `audio.cpp` | XAudio render driver (null output), XMA context allocation, voice (no headset) |
| `input.*` | `XamInputGetState/GetCapabilities/SetState` |
| `import_stubs.cpp` | generated (`tools/gen_import_stubs.py`): weak fallback for every import — logs `UNIMPLEMENTED name(...)` once |
| `mp/hooks.cpp`, `zm/hooks.cpp` | hooks on recompiled functions; currently the engine's error function, so `GAME ERROR file:line: message` is logged before it `longjmp`s |

Fixes found while booting (all in `tools/patches/xenonrecomp-bo3.patch`): PowerPC integer divides never trap
but x86 does on /0 and INT_MIN/-1 → generated divides are now guarded (result 0, like Xenia).

Engine-specific behaviour found so far:
* `xenon_net.cpp:964` asserts "Unable to find an XNADDR." if `XNetGetTitleXnAddr` reports none → we report a static address.
* `xenon_net.cpp:1009` "Couldn't allocate IP port" without sockets → loopback sockets.
* `stream.cpp:1563` "No more free path items" without directory enumeration → `NtQueryDirectoryFile`.
* Arguments past r10 are in 8-byte slots at `r1+0x50` (verified at an `NtCreateFile` call site).
* D3D throttles frames with "write SCRATCH_REG1 = 1; WAIT_REG_MEM [scratch writeback + 4] == 1" — needs the
  Xenos scratch-register memory writeback (`SCRATCH_ADDR`/`SCRATCH_UMSK`).
* GPU interrupts: the handler clears its bit in a CPU mask using `PCR.current_cpu`, so CP interrupts are
  delivered once per CPU in the `PM4_INTERRUPT` mask with that field set.
* `xenon_voice.cpp:90` "Error ... on XHVCreateEngine()" until the audio render driver and
  `XamUserReadProfileSettings` work.
* Jump tables: some bounds checks are 84 instructions before the `bctr` (zlib inflate), others live in a
  predecessor block — `bo3switch` handles both (168 tables per executable).

## Renderer (`runtime/src/gpu`)

| File | What it does |
|---|---|
| `vk.*` | SDL3 window, Vulkan 1.3 device, swapchain (mailbox), **guest physical memory is GPU memory**: 512 MiB allocated by Vulkan, exported as a dma-buf and mapped by the CPU at all three physical views (0xA0000000 / 0xC0000000 / 0xE0000000+0x1000), zero-copy for the shaders. Fallback without dma-buf export: host-pointer import of an unaliased view |
| `shader_translator.*` | **Xenos microcode → GLSL 4.60**: full control flow (exec/cond-exec/predicates/loops/calls/jumps via `pc` switch), vector+scalar ALU co-issue with Xenos "0 × x = 0" multiply, vertex fetch of every Xenos format with endian swap, texture fetch, exports, alpha test |
| `shader_compiler.cpp` | GLSL → SPIR-V with glslang (static). Compiled shaders are kept in `shadercache/` (reset automatically when the translator is rebuilt); new ones compile on 4 background threads and draws using them are skipped until ready (`BO3_SYNC_SHADERS=1` compiles inline, `BO3_NO_SHADER_CACHE=1` ignores the disk cache) |
| `renderer.*` | EDRAM render targets as Vulkan images, dynamic rendering, pipeline cache (dynamic cull/depth/stencil), rect-list/quad-list via geometry shaders, viewport/VTE → host NDC in the shader, resolves (copy + colour/depth clears), present of the front-buffer resolve |
| `texture_cache.*` | guest textures: tiling (Xenia's `Tiled2D`), endian swap, fetch-constant swizzle → image view, DXT1/3/5/DXN as BC1/2/3/5, 8/16/32-bit formats, render-to-texture through resolve targets, samplers |
| `runtime/xenia/` | Xenia's `xenos.h`, `ucode.h`, `registers.h`, `register_table.inc`, `texture_address.h` (BSD) + a tiny shim |

Frame analysis: `BO3_FRAME_LOG=<frame>` logs every draw (targets, formats, MSAA, depth state, VTE, textures with
their fetch constants) and resolve of that frame; with `BO3_FRAME_DUMP=dir` every resolve is written as PPM (HDR
clamped) with channel means, `BO3_FRAME_DUMP_DRAWS=n,m` also dumps the bound target after those draws.
`BO3_TEX_DUMP=dir` writes every texture upload (untiled blocks), `BO3_MEMEXPORT_LOG=1` logs memexport streams.

Debug switches: `BO3_SCREENSHOT=file.ppm BO3_SCREENSHOT_FRAME=n[,n...]` (dump presented frames; several →
`file_<n>.ppm`), `BO3_GPU_TIMING=1` (serialized GPU cost per operation type, every 60 frames),
`BO3_GPU_TRACE=1`, `BO3_DUMP_SHADERS=dir` (+ `build/shader_test dir/*.bin` to check the translator offline),
`BO3_NO_WINDOW=1` (headless), `BO3_GAMELOG=1`, `BO3_BINKLOG=1`, `BO3_TRACE_WAITS=1`, `BO3_TRACE_HANDLE=0x...`,
`BO3_TRACE_IMPORTS=<guest thread id|all>` (every kernel/XAM call with args, caller and result),
`BO3_AUTOPRESS="5:START,8:A"`, `BO3_VK_VALIDATION=1`. Crashes print all GPRs and a guest backtrace; engine
errors (`GAME ERROR`) print a guest backtrace too.
On-screen controller (Android): OPCIONES > "Mando virtual" (ini `[input] virtual_pad=1`) shows an Xbox-style
touch controller (`android/src/com/bo3recomp/VirtualPad.java`: sticks, ABXY, d-pad, LB/RB/LT/RT, View/Menu, L3/R3),
semi-transparent over the game; its state reaches `host_input.cpp` through JNI (`VirtualPad.nativeUpdate`) and is
merged with a physical pad. Touches that miss the controls go to the game (the OPCIONES button). Saving the
options keeps every ini section other than `[video]` / `[input]` as it was (it used to drop `live=1`).
Live remote control (driving a running instance from scripts): `BO3_PRESS_FILE=<path>` - writing "A,DOWN,A" to
that file presses those buttons 0.6 s apart (the file is then deleted; Android always uses `press.txt`);
`BO3_SHOT_FILE=<path>` - writing an output path (`.ppm`) to it saves the next frame there.

Quick test into Freerun (light, good for testing): `BO3_AUTOPRESS="7:A,10:DOWN,11:A,15:A,19:A" build/bo3_mp`.
Quick test that walks into a match (MP): `BO3_AUTOPRESS="7:A,10:A,15:A,20:A,45:A" build/bo3_mp`
(title → Multiplayer → Start Game → class select → spawn).

Controls (keyboard + mouse; the first click captures the mouse, F10 releases it):

| Action | Key | | Action | Key |
|---|---|---|---|---|
| Move | WASD | | Look | mouse (`BO3_MOUSE_SENS`, default 1.0) |
| Fire / Aim | left / right click | | Jump | Space |
| Crouch / slide | C or Ctrl | | Sprint | Shift |
| Melee | V or mouse side button | | Reload / use | R or F |
| Lethal / tactical | G / Q | | Specialist | X |
| Switch weapon | 1, 2 or wheel | | Scorestreaks (D-pad) | 3-6, arrows |
| Pause menu | Esc | | Scoreboard | Tab |
| Menu accept / back | Enter / Backspace | | Fullscreen | F11 or Alt+Enter |
| Options (video, FPS counter, mouse) | F1 | | Release mouse | F10 |
| FPS counter off / FPS / detailed | F2 | | | |

**Mouse look** goes through the client's gamepad look function (MP `sub_822E3E38`, ZM `sub_822DFD78`): it stores
the frame's view turn rates at `[client] + 12704` (pitch) / `+12708` (yaw, negative = right), with the client pointer
at MP 0x84475F74 / ZM 0x84439358. The hook adds `mouse degrees / frame time` divided by the game's own turn factors
(measured: yaw ×111.6, pitch ×49.3), so the mouse uses the same path as the stick - client command + prediction:
instant and smooth - without the stick's response curve (dead zone 0.22, rate ∝ x^2.52) and its ~0.35 s turn ramp.
Horizontal = `mouse_ratio` × vertical (default 2). Earlier attempts and why they failed: writing the server-side view
angles (`sub_821B2E88`, kept as `BO3_MOUSE_LEGACY=1`) only showed on screen at server-snapshot rate (15 rendered
frames without any change while the angles moved); mouse-as-stick (`BO3_MOUSE_STICK_CTL=1`) has the turn ramp.
Tools: `BO3_MOUSE_EVENTS=<s>` (SDL motion events at 1 kHz), `BO3_MOUSE_TRACE=<s>`, `BO3_STICK_SWEEP=<s>`,
`BO3_LOOK_LOG=1`, `BO3_SCREENSHOT_AT=<s>,<n>` (consecutive frames), `BO3_WATCH_RW=1` (read+write watchpoints).

Black textures after a few minutes: the persistent descriptor-set cache is cleared when it reaches 24576 sets, but the
"last set" shortcut kept pointing into the destroyed pools. Fixed (`BO3_SETCACHE_MAX=<n>` forces early clears).

FOV: the `cg_fov` dvar (hash 0x31DCBADD, 65) is found and can be written (`BO3_FOV_EXPERIMENT=1`, `fov` in the ini),
but it has no visible effect yet - not exposed in the menu.

FPS counter (F2 or F1 → Contador de FPS, `fps_overlay` in the ini, `BO3_FPS=0/1/2`): FPS, frame time (average /
worst), GPU busy time, command-thread load, draws / resolves per frame, internal and screen resolution.

Video options (F1): **internal resolution** (0.5x = 640x360, 1x = the console's 1280x720, 1.5x = 1920x1080, 2x = 2560x1440,
3x = 3840x2160; fractional scales round every scaled coordinate),
window / screen-mode size (1280x720 ... native) and fullscreen on/off, applied immediately and saved to
`bo3recomp.ini` in the working directory (`render_scale`, `width`/`height` with 0 = native, `fullscreen`).
`BO3_RENDER_SCALE=1..3`, `BO3_FULLSCREEN=0/1`, `--fullscreen` and `--windowed` override the file.
The internal resolution multiplies the EDRAM sample grid: render targets, viewport/scissor, resolve images and
the pixel position the shaders get (`pg`, in guest pixels) are scaled; resolves read back through memory are
averaged down to the guest size; texture sizes in shaders come from the fetch constant. Changing it drops all
render targets at the next frame boundary. Radeon 680M, Shadows of Evil start: 1x ~55 fps, 2x ~38, 3x ~25.

Any SDL gamepad works too (stick Y is negated in int and clamped: -(-32768) overflowed int16, so full deflection
up read as full down). Audio: `BO3_VOLUME=0..2`, `BO3_NO_AUDIO=1`; diagnostics `BO3_XMA_LOG=1`,
`BO3_AUDIO_DUMP=file` (raw 48 kHz stereo float), `BO3_XMA_DUMP=dir` (decoder output per context).

XMA (`runtime/src/xma.cpp`): the game reads the context array address from register 0x600 and writes Kick/Lock/Clear
bit masks with plain stores; the register page is mapped read-only and a SIGSEGV handler decodes the x86 store,
applies it and skips the instruction. A worker decodes kicked contexts with the system FFmpeg 7.1 `xma2` decoder
(headers in `thirdparty/ffmpeg-src`, no -devel package needed) into the guest's 16-bit big-endian output ring.

Performance, third pass (uncapped, 1x: Shadows of Evil alley ~4400 draws 58 -> ~95 fps (-> ~143 after the items below), command-processor
thread 3.8 -> 2.4 us/draw; MP match 98 fps with the map missing -> 121 fps drawn):
* **Measuring**: `BO3_AB_TOGGLE=n` flips `g_ab` every n stats windows (120 frames) so old and new path run in
  the same scene; the stats line is tagged `[A]`/`[B]`. Code under test checks `g_ab` (currently: submit
  thread, constant upload masks, host memcpy). Per-draw CP cost from the stats lines: busy = frame time - idle.
  `BO3_PROFILE_STACKS=1` adds call stacks to the profiler (`BO3_CALLERS=<function>` and `BO3_DEPTH=n` in
  `tools/profile_report.py`), `BO3_PROFILE_THREAD=<guest thread name>` samples that game thread instead of the
  CP (e.g. `2_Backend`, `Main`, `3_Server`).
* **Texture change detection by write tracking** (`write_watch.cpp`): textures were hashed every frame (a sparse
  sample = 512 cache misses per big texture, plus a dense hash every 16th frame; 13% of the CP thread). Now the
  64 KiB granules of guest physical memory a texture covers are write-protected in all three views; a guest
  store faults once, the SIGSEGV handler records a write sequence number and unprotects. A texture whose
  granules have no write newer than its last check is not read at all. Several cache entries can watch the
  same memory, so granules keep sequence numbers (no "dirty" flag to consume). GPU writes are reported
  (resolve to memory; memexport: the stream constants read by the `eA` write - base, element count - give the
  range) at record time and again when the submission completes. `pread`/`recv` into guest memory keep their
  range unprotected while the syscall runs (`WatchHostWrite`). Safety net: every 64 frames each texture gets the
  old sparse sample. `BO3_NO_WRITE_WATCH=1` disables, `BO3_WATCH_VERIFY=1` hashes anyway and reports misses
  (only the menu Bink planes, a few times). Side effect: a 512x256x4 3D texture that changes every frame
  (0x163F0000 in SoE) was only re-uploaded when the sparse sample noticed; now every frame, on the GPU (the
  untile shader handles 3D tiling too).
* Shader microcode loads (D3D reloads the shader for almost every draw) reuse their hash while the write
  tracking reports no store into the code.
* **Descriptor set cache bug** (MP world disappearing after a while): `SetEntry` had 4 bytes of uninitialised
  padding and was compared with memcmp, so identical sets often missed, a new set was allocated and the entry
  replaced - the number of entries never reached the clear limit while the pools kept growing until
  `vkCreateDescriptorPool` failed. Every following draw then tried to create a pool (35% of the CP thread in
  libdrm's VA allocator) and was skipped. Fixed (no padding; clears count allocated sets; a failed pool
  creation clears the cache).
* **Submit thread** (`gpu-submit`): vkQueueSubmit costs ~70 us of kernel time and there are ~9 per frame; the
  CP hands command buffers to a thread that submits them in order. Everything else using the queue (present,
  waits, swapchain recreation) calls `QueueSync()` first. `BO3_SYNC_SUBMIT=1` disables. (Submitting less often
  instead did not help.)
* Constant blocks: shaders record which float-constant groups (4 constants) and fetch-constant uvec4s they read
  (all with relative addressing); a block is re-uploaded only when one of those changed and only those are
  written to the ring.
* `WriteReg` stored to an `std::atomic` with seq_cst (an `xchg`, i.e. a full barrier, per register write).
* Pipeline lookup without building a `std::string` key (transparent hash), view cached in the texture entry.
* The game's CRT memcpy / memset (ZM 0x82A08D60 / 0x82A09360, MP 0x82A16B40 / 0x82A17140) are host
  memmove / memset (the recompiled loops byte-swapped every word): ~3% fps.
* Now the CP thread is no longer the only limit: the game's `2_Backend`, `Main` and `3_Server` threads run at
  ~90-95% - but ~35% of `2_Backend` is its job system polling for work (`sub_82145xxx`..`sub_82146xxx` +
  `NtYieldExecution`), so the CP is still the limit in heavy scenes.
* **Split command processor** (`BO3_CP_SPLIT=0` restores one thread): `gpu-pm4` walks the ring and the indirect
  buffers and queues register writes, constants (big-endian, applied as blocks), draws, shader hashes (microcode
  only the first time) and every ordered packet (waits, fence writes, events, interrupts, swaps) into a lock-free
  queue; `gpu-cp` applies them in order and does all the Vulkan work (same code paths: `ExecuteSyncOp`). The
  parser reads guest memory ahead of the recorder, so at a WAIT_REG_MEM it waits for the recorder unless the
  condition already holds - against memory, the registers and fence values written earlier in the stream by the
  CP itself (EVENT_WRITE_SHD, MEM_WRITE, scratch writeback); only ~2 per frame need that (a CPU handshake on
  0x1967FFE2). MP match 121 -> 138 fps, SoE ~5%. Diagnostics: `BO3_SPLIT_LOG=1` (unresolved waits),
  `BO3_PM4_STATS=1` (type-3 packets per frame).
* Recompiled code and runtime are built for **x86-64-v3** (`-DBO3_MARCH=...` to change; MOVBE does every guest
  byte-swapped load/store in one instruction; `-ffp-contract=off` keeps PowerPC float results).
* Constant packets (SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT) are compared/copied 16 registers at a time
  big-endian to big-endian; plain register blocks go straight into the MMIO mirror.
* **Index buffer address as `firstInstance`** (the vertex shader reads `gl_InstanceIndex`): it changed with
  almost every draw and re-sent the push constants each time (3978 -> 393 push-constant updates per frame; RADV
  emits pending state in vkCmdDraw, 23% -> 14% of the recording thread). SoE 105 -> 119 fps.
  `BO3_STATE_STATS=1` logs the per-draw state commands per frame.
* Descriptor sets: a direct-mapped front (4096 entries, 64-bit key) before the ~20k-entry map (-5.5% per draw);
  render target lookups memoized; hot 64..128-byte comparisons inlined (`FastEq`, AVX2) instead of libc memcmp;
  textures rewritten at every recent check are uploaded without hashing (re-hashed every 32nd frame).
* **Kernel dispatcher**: every SetEvent woke *all* guest threads blocked on anything (one condition variable,
  notify_all) and NtPulseEvent yielded hoping the waiters ran before the reset (12.7% + 6.3% of the game's Main
  thread, from a job-system pulse at `sub_826DC3A0`). Now each blocked thread has its own `KWaiter` registered on
  the objects it waits for; events / semaphores / mutants wake only their waiters, a pulse hands the event to the
  current waiters directly. Other changes still wake everyone (`DispatchNotify`). SoE 120 -> ~143 fps.
* **Stutters while moving** (frames of 100 ms .. 1.7 s, the recorder idle): the game's Main thread starved on a
  critical section (0x843E6580) that the job workers, `2_Backend` and `3_Server` re-take in a loop - at every
  release Main woke up and found it taken again. Critical sections now keep a FIFO of their waiters; a free lock
  may still be taken by anyone, unless the oldest waiter has waited longer than `BO3_CS_STARVE_MS` (5 ms): then
  the lock is handed to it at release. 24-30 frames over 20 ms per 30 s walk -> 0-1. Also a pulsed event with
  nobody waiting stays signalled (synchronization event) instead of being lost: a worker between checking its
  queue and waiting missed the pulse and slept until its timeout.
  Tools: `BO3_AUTOMOVE=<start>,<seconds>` (walk forward while turning), `BO3_SPIKE_LOG=<ms>` (every slow frame
  with what happened in it), `BO3_STALL_DUMP=<ms>` (when the recorder waits that long for the game: every guest
  thread's host RIP, guest backtrace, the critical section / object it waits for and the lock owner),
  `BO3_LONG_WAIT=<ms>` (single long kernel waits with backtrace), `BO3_GLOBAL_WAKE=1` (old event wakeups).
* A/B measurements per change: `BO3_AB_FEATURE=submit|constmask|memcpy|constblock|setfront|csfair` with `BO3_AB_TOGGLE`.
  `BO3_FPS_LIMIT` accepts up to 1000 for benchmarks (the menu offers up to 480: 30, 60, 120, 144, 165, 240, 360, 480).

Performance, second pass (SoE 1x: 60 fps cap, GPU 8.4 -> 4.9 ms/frame; 2x: ~58 fps, GPU 17 -> ~13 ms):
* The overlay / log show **GPU busy** (start/end timestamps of every command buffer - not serializing) and the
  command-processor thread split (`renderer: CP thread per frame`). The CP thread, not the GPU, was the limit.
* MEM_WRITE / COND_WRITE / REG_TO_MEM / scratch writeback land when the CP reaches them (Xenos semantics); only
  EVENT_WRITE_SHD waits for the GPU. CP interrupts no longer stall for the GPU (1.7 ms/frame). `BO3_DEFER_ALL=1`.
* Deferred-write fences ride on the work submission (one vkQueueSubmit), `WAIT_REG_MEM` submits at most once
  per ms (`BO3_WAIT_SUBMIT_US`).
* Bound pipeline / descriptor sets / push constants / all dynamic state are only re-recorded when they change
  (`BO3_NO_DYNCACHE=1`); compute dispatches invalidate the graphics push constants.
* **Fragment shading rate (VRS)**: MSAA surfaces live in sample space (one host pixel per sample), so pixel
  shaders ran once per sample; now they run once per pixel (1x2 for 2x MSAA, 2x2 for 4x) like real MSAA, depth /
  stencil / coverage per sample (`BO3_NO_VRS=1`).
* Straight-line shaders (no loops/calls/jumps, ~80%) are emitted without the `pc` state machine
  (`BO3_SHADER_STATE_MACHINE=1`).
* Resolution scale applies to surfaces of 256+ samples only: colour-grading LUTs, bloom chains and small effect
  buffers stay 1x (scaled, the LUT came out wrong: magenta fringes in bright areas at 2x). Each resolve keeps its
  own scale.
* `BO3_GPU_TIMING=2`: GPU cost per rendering pass / resolve (few timestamps, little distortion).
* Pipelines are compiled on 3 background threads like shaders (~50 ms each on a driver-cache miss: 13 s of
  hitches the first time a map loaded after a translator change); draws needing one are skipped until it is ready.
  With an empty driver cache (`MESA_SHADER_CACHE_DISABLE=true`) the worst in-game frame is ~25 ms.

Performance work (Shadows of Evil start area, 4300 draws/frame: 39 -> ~55 fps; MP match 49 -> 60):
* The command processor thread records all Vulkan work; it was the bottleneck. A sampling profiler
  (`BO3_PROFILE=<start_s>,<duration_s>` → `bo3_profile.txt`, resolve with `tools/profile_report.py
  bo3_profile.txt build/bo3_zm`, `BO3_LINES=<function>` for a per-line breakdown) showed where it went.
* Descriptor sets cached by content across frames (persistent pools) + a per-fetch-constant memo per frame.
* Constant blocks: only the 16-register chunks the CP changed are re-read (`g_gpuConstDirty`), and a stage's
  block is re-uploaded only when something it contains changed.
* Texture change detection: dense hash only for ≤16 KB; bigger textures get a sparse sample every frame and the
  dense hash every 16th frame (staggered).
* Texture uploads of formats stored as is (DXT, 8888, 8, ...) are untiled + endian-swapped by a compute shader
  reading guest memory directly (`BO3_CPU_UNTILE=1` for the old CPU path; `BO3_UPLOAD_LOG=1` logs uploads).
* Mid-frame submits (so fence writes can land) continue the frame in a new command buffer of the same pool
  instead of switching frame slots (which waited for the GPU).
* `WAIT_REG_MEM` on a value the CP itself wrote (still pending until the GPU finishes) is satisfied from the
  pending write: the Vulkan queue is in order, so waiting there only serialized CPU and GPU (`BO3_DBG_WAIT_GPU=1`
  disables it). Before a CP interrupt all pending writes are made visible (its handler reads them).
* Resolve images bigger than the fetch (pitch-aligned destinations, reused bigger resolves) are sampled through
  an exact-size copy - this was the black band on the right of the image (`BO3_NO_FIT=1` disables).
* Shader stutter: up to 1.2 s hitches when a map started (translation + glslang on the CP thread) - now disk
  cache + background compilation; with a warm cache no frame over ~18 ms in game.

Known renderer problems (next work):
* Some alpha-tested/blended decals have blocky "staircase" edges (Shadows of Evil alley walls); cause unknown.
* A few textures are still garbage (Bink-driven screens: movies don't play), stacked 2D-array textures and
  linear mip chains not supported, 3D textures have no mips.
* memexport draws (menus) end the rendering pass + full barrier each.
* **Bink**: movies never advance (menu backgrounds are black; boot logo skipped).
* ~30 fps in game, ~2500 draws/frame: per-draw constant upload (2 × 5 KB) and descriptor sets are the next
  CPU costs; texture format 23 is not supported yet.
* Not yet: mip levels, 3D/cube textures, MSAA, EDRAM aliasing between formats, gamma, memexport, depth
  resolves, CPU readback of resolves.

Fixes found while bringing the renderer up:
* `mftb` was translated to the host TSC; the guest time base is 50 MHz → now `PPC_QUERY_TIMEBASE()` (runtime).
* XAudio client callbacks take a pointer to a dword holding the client argument (Xenia's "wrapped" arg).
* `SET_CONSTANT2`/`SET_SHADER_CONSTANTS` use 16-bit absolute register indices (masking to 11 bits overwrote
  SCRATCH_UMSK/ADDR and deadlocked the GPU), `LOAD_ALU_CONSTANT` loads constants from memory.
* Content enumeration (DLC) is required for boot to continue; implemented from the `content/` folder.
* Switches index with `.u32` (a sign-extended-as-unsigned index jumped through a host table out of range);
  import results are sign-extended to 64 bits like on hardware.
* `XamContentCreateEx` CREATE_ALWAYS empties the package (the game then uses FILE_CREATE); the original save
  data is copied once to `content/backup/` first.
* Physical memory must alias across its three views (D3D keeps command-buffer fixup lists at 0xC... addresses
  of objects allocated at 0xA...).
* PM4 packets can wrap around the end of the ring buffer (an IB packet there jumped into garbage and
  overwrote the scratch registers → GPU hang).
* Pixel shaders indexed the register array dynamically (`r[param_gen]`), which forces drivers to keep all 64
  registers in scratch memory: one UI quad cost ~1 ms. Constant indices + sized arrays: 128 → 0.7 ms/frame.
* `XMsgInProcessCall` must return an HRESULT (< 0 on failure); xlivebase NAT type (0x58006) → open.
* In-game world (was white/black): EDRAM aliasing (RTs keyed by host format, so 2_10_10_10 and
  2_10_10_10_AS_10_10_10_10 share an image), **MSAA surfaces stored in sample space** (D3D clears depth with a
  4x surface at half pitch; the scene is 2x), rect-list geometry shader strip order (clears left a triangle),
  depth resolves (shadow maps / scene depth for `24_8` fetches), **resolves also written to guest memory by a
  compute shader** (format, exp bias, endian, tiling: the colour-grading 3D LUT is a 2D resolve re-read as 3D),
  3D and cube textures, cube coordinates in [1,2] (`*2-3`), polygon offset (shadow acne), tfetch filter
  overrides, base level of packed-mip textures inside the mip tail, and **memexport** (vertex shaders that write
  memory - D3D uses it to copy vertex data) executed with rasterizer discard.
* **Fastfile loader crash** (thread "Database", ~1 in 2 runs): GPU writes to guest memory (resolve-to-memory,
  memexport) landed after the guest had been told the GPU was done (our CP writes fences when it *processes*
  them, Vulkan runs later) and after the loader reused the memory. Fix: before any CPU-visible fence the CP
  finishes such GPU work (`RendererFlushGuestWrites`), and resolves are only written to memory when something
  reads them there; the colour-grading LUT (2D resolve read as 3D) is rebuilt on the GPU with a precomputed
  2D-tiled -> 3D-tiled texel map instead.
* **GPU page faults** after ~1-2 min of play: a resized resolve target was destroyed while the command buffer
  being recorded still referenced it (and cached views kept the old handle). Images are now retired to a
  graveyard and destroyed 3 frames later.
* Resolves with `copy_dest_swap` store R/B swapped: direct sampling now composes that into the view swizzle.
* Performance (MP match 25 -> 60 fps): per-draw values in push constants, constant blocks rebuilt only when
  constant registers change, descriptor sets reused while bindings are equal, sampled change detection for big
  textures, no per-fence GPU syncs in game.
* **Skinned models (characters, first-person arms) collapsed to a flat shape**: vertex "mini fetches" reuse the
  previous full fetch's constant, index *and stride*; the stride was not inherited, so every bone matrix fetch read
  the first matrix. Mini fetches also reuse the *index value* (the shader may overwrite the index register in
  between) - that gave spikes on arms/bodies.
* **Fences are written when the GPU really finished** (stretched polygons on zombies / viewmodel): the CP used to
  write fence values (`MEM_WRITE`, `EVENT_WRITE_SHD`, scratch writeback, `COND_WRITE`) while *processing* the
  commands, so the game reused its per-frame ring (bone matrices, dynamic vertices) before Vulkan had read it.
  Now those writes are queued (`RendererDeferGuestWrite`) and a completion thread applies them after the VkFence of
  the submission that contains the preceding work signals. The CP submits pending work when it idles, before
  `WAIT_REG_MEM` and before interrupts. This replaces the blocking `RendererFlushGuestWrites`.
* Mip levels (base + mip chain + packed mip tail, Xenia's layout), mip filter and anisotropy from the fetch constant.
* Guest memory accesses in shaders are masked to the 512 MiB physical range (robustBufferAccess/robustImageAccess
  also enabled).
* `EVENT_WRITE_ZPD` (occlusion queries) must write a sample count or D3D's `GetData` spins forever (match
  start hang). Also implemented: `vupkd3d128` types 2–5, `vpkd3d128` types 2/3, `blrl`.

## Debugging tips

* `GAME ERROR file:line: message` lines come from the engine's own assert/error function (hooked).
* `indirect call to 0x..., which is not a known function start` → a missing function boundary or jump table.
* Hung? `gdb -p $(pgrep bo3_mp) -batch -ex "thread apply all bt 8"` — guest functions show up as `__imp__sub_XXXXXXXX`.
* `BO3_GPU_TRACE=1` to see what the GPU is waiting for.

## Next steps

The generated code is only the CPU half. A host runtime is required, as in UnleashedRecomp
(`UnleashedRecomp/kernel`, `gpu`, `apu`, `hid`). The imports to implement (`private/mp/imports.txt`):

| Area | Imports | Notes |
|---|---|---|
| UI / profile / content (Xam*, XMsg, XNotify) | 69 | sign-in, saves (`XamContentCreateEx`), DLC mounting, keyboard UI |
| Threads / sync (Ke*, Nt* events, critical sections, TLS) | 58 | map to pthreads/futex |
| Networking (NetDll_*, XNet*, XHttp*) | 54 | can stub to offline first |
| Graphics (Vd*) | 21 | ring buffer / swap → Xenos GPU emulation layer (Vulkan) |
| Crypto (XeCrypt*, XeKeys*) | 15 | SHA etc. straightforward |
| Audio (XAudio*, XMA*) | 13 | XMA decoding (UnleashedRecomp uses FFmpeg-based) |
| Files (Nt*File*) | 12 | map `game:`/`d:` → `extracted/game`, `update:` → TU folder |
| Memory (Mm*, NtAllocateVirtualMemory) | 9 | 4 GB guest address space reservation |
| Other (Rtl*, Xex*, Dbg*, CRT) | 28 | |

Suggested order:
1. Minimal host `main()`: reserve guest memory, map the image (`image.bin` at `0x82000000`),
   set up `PPCContext`, call `_xstart`; stub every import with logging. Iterate until the game
   reaches its first `VdSwap`.
2. Filesystem + threads + memory → engine boot, fastfile (`.ff`/`.xpak`) loading.
3. GPU: reuse UnleashedRecomp's approach (plume/Vulkan + **XenosRecomp** for shaders). BO3
   shaders live inside the fastfiles, so a fastfile reader is needed to pull them out.
4. Audio (XMA), input (XInput → SDL), then networking (start offline-only).

Plan for real-world effort: Unleashed Recompiled took a team over a year for a game with a much
smaller engine. BO3 has a big multithreaded engine, its own job system, and online
features, so expect a long project. The steps in this repo are done so that work can start
on the runtime.

## Cloud saves (Supabase)

`cloud.cpp`: the profile's save packages (`content/<profile>/4156091D/...`: MP / ZM / Freerun stats, loadouts,
settings) live in a Supabase table, so PC and phone share the same progress. Setup: run
`tools/supabase_schema.sql` in the project's SQL editor, turn "Confirm email" off (Authentication), and fill
`bo3recomp.ini`:

    [online]
    supabase_url=https://<project>.supabase.co
    supabase_key=<anon public key>
    email=<account used on every device>
    password=<its password>

The account is created on first use. At startup (before the game reads its saves) newer files are downloaded;
while playing, changed files are uploaded every 20 s and when the app closes / goes to the background. Conflicts
are decided with hashes against the last sync (`content/<profile>/.cloud_sync`), not clocks: whatever changed
on only one side wins; if both changed (or on a device's first sync) the cloud wins and the local copy goes to
`content/backup/cloud-<time>/`. HTTPS: libcurl loaded at run time on the desktop, Java's HttpURLConnection
through JNI on Android (`android/src/com/bo3recomp/Http.java`). The online mode's files (below) live in
`content/<profile>/4156091D/online/`, so they sync the same way.

## Online mode (Demonware emulation)

`live=1` in the `[online]` section of `bo3recomp.ini` (or `BO3_LIVE=1`; `BO3_LIVE=0` turns it off): the profile
is signed in to "Xbox Live" and the game talks to our own Demonware (`dw_server.cpp`, inside the process - no
server, no network). MP and ZM both reach the online main menu (MULTIPLAYER / ZOMBIES / STORE / PLAY LOCAL) and
the online Multiplayer flow (first-time Specialists intro).

* **Auth** (`xhttp.cpp`): XHttp is emulated; the auth3 POST to `ops3-xbox360-auth3.prod.demonware.net` is answered
  locally (`auth_task` = request + 1, code 700, plain little-endian bdAuthTicket, `extra_data`). XAuth title buffer
  kept (`XampXAuthStartup` / `GetTitleBuffer`).
* **LSP / lobby**: `XNetLogonGetServiceInfo` + the enumerator give one server (10.68.87.1); the TCP connect to it
  becomes a socketpair served by `DwLobbyAccept`. Protocol (360): client header `C8000000 FFFF0300`, frames
  `[u32 size][u8 0 = plain][body]`; messages `[u8 type][payload]` (1 task reply, 4 login done); service calls
  `[service][typed ubyte task][typed args]`; typed byte buffer as on PC (1 bool, 3 ubyte, 6 u16, 7 i32, 8 u32,
  10 u64, 13 float, 16 string, 19 blob).
* **Services**: bdStorage (publisher files, user files get / set / get_files / upload), bdTitleUtilities (server
  time), bdDML (location); the rest answer "ok, no results".
* **Publisher files** (`online/publisher/<name>` overrides): playlists / motd / featured cards are built in -
  zlib stored blocks of `BO3RECOMP\n<text>`. The game checks an RSA signature on them; the hook
  (`live_hooks.h`, `LIVE_DW_HOOKS`) accepts our unsigned header instead. No FFOTD: its failure callback also sets
  the FFOTD-fetched byte. Without playlists / FFOTD the game sits on "Connecting to Online Service" forever
  (`IsDemonwareFetchingDone` mask 0x00B123DE; the hook logs the flags).
* **User files**: `stats_mp`, `loadouts_mp`, `stats_zm`, `loadouts_zm`, `stats_fr`, `metplayers`... are created by
  the game on first connect (fresh online stats, separate from the offline ones) and stored in
  `content/<profile>/4156091D/online/` -> Supabase through the cloud sync.
* **Lobby keep-alive**: the client sends empty frames (size 0) now and then; they are answered with an empty
  frame. (Reading them as normal frames threw `std::out_of_range` and killed the game a while after connecting -
  the "random" online crashes.) A request that fails to parse is answered "ok, empty" instead of crashing.
* **bdEventLog filter** (`eventFilter` hook): without a category list from a real server every telemetry event
  was dropped, and the game then returns a locally completed task whose last reference it releases while still
  using it - freed memory, crash (e.g. spending an unlock token on the first Specialist). Nothing is dropped now;
  the events (service 67 task 5) are answered ok.
* Addresses: MP `8253EEE8` (fetch done), `8256B550` (signature), `8256BAF8` (FFOTD failed), FFOTD byte
  `0x844761E1`, `827A2608` (event filter), `8253F900` (host bandwidth check); ZM `8253E030`, `8256A100`,
  `8256A6A8`, `0x844395A5`, `82794680`, `8253EA48`.

### Public matches (online matchmaking)

MULTIPLAYER > PUBLIC MATCH > FIND MATCH works between instances that see each other on the virtual network
(same LAN - PC and phone on one Wi-Fi - or `peers=` in the ini): the first one hosts and waits ("Waiting for 1
more player", 2 minimum), the next one finds it, joins, map vote, match.

* **Playlists**: built into `dw_server.cpp` (`kPlaylists`, playlists.info text format - the comment there
  documents it), or `online/playlists.txt` to try other ones without rebuilding. Core: TDM, Domination, Kill
  Confirmed, Hardpoint, Demolition, Safeguard, Uplink; Party Games: FFA, Gun Game. Parser details that matter:
  categories / supercategories need `sessionmode mp|zm` (others are not listed), the Public Match menu shows
  categories with `filter playermatch`, categories must be defined before the playlists (inside a playlist
  `category <name>` assigns it), playlist entries are comma separated (`core,tdm,1`: map or maplist,
  gametype, weight).
* **Hosting**: the game only hosts after its upload bandwidth test (Demonware bandwidth test server, missing);
  the `hostBandwidth` hook lets the "has not necessary bandwidth" check pass, otherwise it searches forever.
* **bdMatchMaking** (service 21): create (1) / update (2, 12) store the host's MatchMakingInfo and the virtual
  network announces it to every peer (`PKT_SESSION`); find (5) returns the peers' sessions. Result format (what
  the game deserializes, `sub_823361D8` / base `sub_827A4640`): host address blob, session id blob (8), game
  type, max players, player count, then the game's fields (security id + key, netcode / playlist values, skill,
  geo, host xuid) exactly as the host serialized them.
* **QoS**: `XNetQosListen` data (the host's lobby state) is announced to the peers (`PKT_QOS`); `XNetQosLookup`
  answers a probe from it (complete, contacted, 20 ms). The search QoS-probes every result before joining.
* `BO3_LUA_LOG=1` logs the UI / lobby scripts' `Engine.PrintInfo` messages (`lua: ...`): every lobby step,
  search result count, QoS, join and "CANNOT HOST A SERVER: <reason>".
* **Over the internet** (`rendezvous.cpp`, table from `tools/supabase_online.sql`): in online mode with a signed-in
  account, the game asks a public STUN server (Google / Cloudflare) for the public IP:port of its vnet socket,
  publishes it with its LAN address in `online_peers` every 10 s and reads the games seen in the last 90 s; the
  vnet sends its hellos to all of them every second (UDP hole punching), after which they are normal peers. Same
  public IP: the LAN address is used. `BO3_NET_NOLAN=1` turns off LAN discovery and the LAN address (to test
  the internet path on one network). Needs at least one side whose router keeps the port per destination
  (endpoint-independent mapping, most home routers); two strict (symmetric) NATs - some mobile carriers - would
  need a relay, not done.
* **Zombies** (ZOMBIES > PLAY > JOIN PUBLIC GAME): the map list (`zmmapslist.lua`) shows one category per map,
  named after it (`zm_zod`, `zm_factory`, `zm_castle`, filter `playermatch`); the category's name / icon /
  description are what the list shows. Same matchmaking as MP (host after no result, others join).
* Playlist images: `icon` (`playlist_tdm`, `playlist_domination`, ... / `img_t7_menu_zm_preview_zod`). Image
  names longer than 29 characters are cut by the parser (`img_t7_menu_zm_preview_factory` -> use
  `img_mapvote_zm_factory`).
* **GobbleGums / Liquid Divinium** (ZM, online mode; `zm/hooks.cpp`): no Demonware involved - Mega counts are the
  online stats `ItemStats[i].stats.bgbConsumablesGained - bgbConsumablesUsed`, Liquid Divinium is
  `PlayerStatsList.BGB_TOKENS_GAINED - BGB_TOKENS_USED`. When the online fetch is done (`LiveStatsReady`) the
  game's DDL functions set 999 of every gum and 999 Liquid Divinium (the factory counter has 3 digits), refilled
  each launch. Classic gums unlock by rank: the lock check `sub_821C9AC8` reports every `zmui_bgb_*` item
  unlocked. Dr. Monty's factory works (spends the divinium, gives gums).
* Not yet: a relay for symmetric NATs.

## System link (LAN)

Works on PC (tested: two instances on one machine, Team Deathmatch on Combine with two players) and PC <->
Android (PC hosting, the phone on another router behind NAT with `peers=<PC IP>`: joined and played a match). `BO3_NET=off` goes back to the offline mode (no cable).

* **Virtual network** (`net.cpp`): every UDP / VDP socket of the game is virtual (an AF_UNIX datagram pair the
  game reads / polls, so blocking, non-blocking and `select` behave as before); datagrams travel encapsulated
  (`B3VT` header: virtual source / destination address and port) over one real UDP socket per instance, the
  first free port of 47600..47615. Each instance has a virtual IP 10.x.y.z (`BO3_VIP`), which is its XNADDR
  and its "secure" address; instances announce themselves every second (broadcast on the LAN + localhost on
  every port of the range) and learn each other's real endpoint. Several copies on one machine work because
  only the real socket needs a port. TCP stays on loopback.
* **Peers beyond the LAN**: `[network] peers=host[:port],...` in `bo3recomp.ini` (or `BO3_PEERS`) - the hello
  is sent there too, and every peer learned from its packets gets the hellos as well (keeps NAT mappings open):
  only one side needs to reach the other (a phone behind a second router lists the PC; the PC answers to the
  router's mapped address).
* **XNet**: link up, XNADDR from the virtual IP, XnAddr <-> InAddr, registered session keys (XNetInAddrToXnAddr
  returns the session's XNKID - with 0 the host ignored the join request), connect status, machine id.
* **XSession**: `XamSessionCreateHandle` / `RefObjByHandle` and the XGI messages (app 0xFB): `XSessionCreate`
  as host fills XSESSION_INFO (system link XNKID, our XNADDR, key); the rest succeeds. The game does its own
  discovery: search = 30-byte broadcast on port 1000, answer = 364 bytes; join and game traffic on the VDP
  socket (port 1002), ~60 packets/s client -> host and ~20/s back in a match.
* **Profile per machine**: `[network] gamertag=` / `xuid=` (generated once into `bo3recomp.ini`): system link
  drops a player whose XUID is already in the session. The saves stay in `content/E0300000126D662A`.
  Tests: `BO3_XUID` / `BO3_GAMERTAG` (with `BO3_XUID` the saves come from `content/<XUID>`), `BO3_NET_TRACE=1`
  logs every virtual packet; the log shows the traffic every 5 s.
* Fixed on the way: `sendto` / `recvfrom` took the address one argument too far (never ran offline).

## Android (in progress)

Runs on a Redmi Note 10 Pro (Adreno 618, kernel 4.14): menus at 60 fps, Shadows of Evil in game ~20 fps at
0.5x (640x360). Needs the whole `content/0000000000000000/4156091D` (DLC packs included: without them a "required
content" notice loops back to the title screen) and `private/<mp|zm>/default_patched.xex`.

* Toolchain in `~/Android/Sdk`: cmdline-tools, platform-tools (adb), platform 36, build-tools 36.1, NDK r29.
* Native build: `cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Android/Sdk/ndk/29.0.14206865/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29`
  then `ninja -C build-android bo3_zm bo3_mp` (`libbo3_zm.so` / `libbo3_mp.so`, SDL linked in, entry `SDL_main`).
  FFmpeg first: `tools/build_ffmpeg_android.sh` (7.1.1, xma2 decoder only -> `thirdparty/ffmpeg-android`).
* APK: `tools/package_android.sh` -> `build-android/bo3recomp.apk` (aapt2 / javac / d8 / zipalign / apksigner, no
  Gradle; debug key in `~/.android/debug.keystore`). Project files in `android/` (manifest, SDL3's Java glue,
  `ZombiesActivity` / `MultiplayerActivity`, each in its own process, landscape).
* Install and data: `adb install build-android/bo3recomp.apk`, start it once (creates the folder), then
  `adb push extracted content /sdcard/Android/data/com.bo3recomp/files/`. Log: `bo3.log` in that folder and logcat
  (tag `BO3`).
* Portability: `arch.h` (CPU pause, signal-context PC, memfd syscall); the XMA register-page trap decodes ARM64
  STR/STUR (unsigned offset, unscaled, pre/post-indexed, register offset); Vulkan functions are loaded with volk
  (`thirdparty/volk`, MIT; the NDK's libvulkan only exports 1.1); without dma-buf export (Android) guest memory is
  a memfd mapped at the three views and imported with VK_EXT_external_memory_host (`BO3_NO_DMABUF=1` forces it on
  the desktop - RADV refuses shared memory there, amdgpu userptr is anonymous-only).
* UI: on-screen **OPCIONES** button (touch) instead of F1; physical gamepad only (`platform.h`;
  `BO3_TOUCH_UI=1 BO3_GAMEPAD_ONLY=1` on the desktop).
* Internal scale in 1/6 steps (0.5 = 640x360 for slow devices).
* **Turnip**: `tools/build_turnip_android.sh` builds Mesa 26.2.6's Vulkan driver (KGSL, android-stub; needs
  `~/Android/mesa` and the meson/mako venv `~/Android/mesa-venv`) -> `thirdparty/turnip-android/libvulkan_freedreno.so`.
  It is loaded with **libadrenotools** (`thirdparty/libadrenotools`, BSD-2: the system loader in a private
  namespace, redirected to the driver in the APK's native library folder); the Android surface is created with
  `vkCreateAndroidSurfaceKHR` from that instance (SDL's own loader is a different one). A file `use_stock_driver`
  in the data folder keeps the phone's driver. Turnip exports dma-bufs on KGSL, so guest memory uses the same
  path as the desktop - but KGSL's dma-buf mmap is uncached and atomics on it fault (SIGBUS). So guest memory
  is a memfd (cached) imported into the GPU: our patch `tools/patches/turnip-kgsl-host-import.patch` (applied by
  the build script) adds `VK_EXT_external_memory_host` to Turnip's KGSL backend (GPUOBJ_IMPORT of a user
  address, or the legacy `IOCTL_KGSL_MAP_USER_MEM` that 4.14 kernels need).
* **Weak memory ordering**: PPC `sync` / `lwsync` / `eieio` were no-ops (fine on x86's TSO); XenonRecomp now emits
  `PPC_FENCE_*` (real barriers on ARM64, nothing on x86) and the CP reads WPTR with acquire semantics - without
  them the GPU command processor read stale commands and hung at random.
* **Pipeline cache on disk** (`shadercache/pipelines.bin`, all drivers): Mesa's disk cache is off on Android, so
  every pipeline was recompiled each run (~250 ms each on the Adreno 618) and their draws skipped meanwhile
  (black floor / sky that "fixed itself"). Saved every 5 s from the compile threads and when the app goes to the
  background. (Turnip sets the pipeline-level cache-hit feedback bit even on misses - check the stage flags.)
* GPU (Adreno 618, measured with `BO3_GPU_TIMING=2`): without fragment shading rate, 2x MSAA surfaces (the main
  scene) keep one sample per pixel (`BO3_MSAA_FULL=1` keeps both; 4x stays full: D3D clears depth through 4x
  surfaces) - 116 -> 90 ms/frame; indexed draws use real index buffers (indices byte-swapped into a per-frame
  host ring, `vkCmdDrawIndexed`) so the post-transform cache reuses vertices - 90 -> 53 ms/frame. Default on
  Android only (desktops are CP-thread bound: 185 -> 165 fps there; `BO3_HOST_INDEX=1` / `BO3_GUEST_INDEX=1`).
  `BO3_DBG_TINY_SCISSOR=1` (1x1 scissor) splits vertex from pixel cost.
* CPU (simpleperf, APK is debuggable): guest yield loops (`Sleep(0)` in the job workers) back off to 100 us sleeps
  after 16 quick yields, the parser's wait sleeps after its spin, and the SDL event loop polls + sleeps (its timed
  wait spun a core on Android).
* Remote testing: `bo3env.txt` in the data folder (KEY=VALUE lines, read at start - Android has no shell
  environment) and `press.txt` ("A,DOWN,A": pressed 0.6 s apart, then deleted).
