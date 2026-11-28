# FFXOptimizations

Performance mods and diagnostics for the 32-bit Steam release of Final Fantasy X HD Remaster.

## Texture loading beta

FFXTexturePrefetch uses the mod paths configured in External File Loader. It queues predicted battle textures on a background worker and retains completed files in a **1 GiB RAM cache**. Previous slow reads and field-map lookups help prioritize the next battle map. The cache grows on demand and checks host memory and 32-bit address-space headroom before allocating.

Slow reads outside battle also feed the worker. It prioritizes previously slow map, menu, character, and effects textures at startup and queues newly observed slow textures for later use in the same session. These assets can occupy up to 256 MiB within the 1 GiB cache. They only evict other non-battle entries.

A cache hit returns resident bytes and advances the native file pointer. FFX uses its normal reader for cache misses, unsupported handles, and partial reads. Texture creation and upload still run through the game's renderer. Other selected textures use background Windows file-cache read-ahead.

### Build and install

1. Install Visual Studio 2022 C++ Build Tools with the x86 compiler and Windows SDK.
2. Run `build.cmd`.
3. With FFX closed, copy `build\FFXTexturePrefetch.dll` to the game's `modules` folder beside `ff10-file-loader.dll`. The game needs a module loader and a working build of External File Loader for its executable version. [External File Loader source](https://github.com/WanxTitanx/ffx-external-file-loader)
4. Start FFX and check `ffx-texture-prefetch.log` in the game directory. The log records hook status, slow reads, resident cache size and hits, and separate counts, bytes, and worst latency for reads through the native Windows path.

To disable the texture logs, create `modules\config\FFXTexturePrefetch.ini` in the game directory with the following settings. The worker still learns which textures to queue.

```ini
[Diagnostics]
Logging=0
```

To remove it, close the game and move `FFXTexturePrefetch.dll` out of `modules`.

### Validate without launching FFX

`build\prefetch-test.exe` checks the Windows API hooks, byte equality, file offsets, and partial-read fallback. It accepts the `ffx_data\gamedata\ps3data` directory of a pack with battle-map overrides and optionally the `FFXTexturePrefetch.hot` file from the game's `modules\config` directory. For an asset-free smoke test, run the following in PowerShell from the repository root.

```powershell
$fixture = Join-Path $env:TEMP ("ffx-prefetch-test-" + [guid]::NewGuid())
$battle = New-Item -ItemType Directory -Path (Join-Path $fixture 'btlmap\kino\kino07_a')
$menu = New-Item -ItemType Directory -Path (Join-Path $fixture 'menu')
[IO.File]::WriteAllBytes((Join-Path $battle.FullName 'fixture.dds.phyre'), [byte[]](0..255))
[IO.File]::WriteAllBytes((Join-Path $menu.FullName 'fixture.dds.phyre'), [byte[]](255..0))
.\build\prefetch-test.exe $fixture
```

Offline validation verifies file behavior, including a normal texture that first falls through to the native reader and later hits the cache. Gameplay performance depends on when predicted textures finish loading.

## Shader reuse

FFX creates the same shaders and input layouts again for every model it loads. A session has thousands of these creates but only a few hundred distinct objects. Each create compiles in the graphics driver. On AMD's D3D11 driver the compile holds a device lock, and the render thread waits behind it, so frames hitch when characters or enemies load. `FFXShaderReuse.dll` returns the existing object for a create with identical bytecode and, for input layouts, identical elements. Only new shaders are compiled. Shaders and layouts cannot be changed after creation, so reuse is safe. The module keeps one reference to each object for the session.

The hooks patch the device vtable and work with the system D3D11 runtime, ReShade, and DXVK. Under DXVK, which compiles shaders in the background, the module still skips the duplicate creates.

Copy `build/FFXShaderReuse.dll` into `modules`. `ffx-shader-reuse.log` in the game directory records hook status and the reuse count. To disable logging or the module, use `modules/config/FFXShaderReuse.ini`.

```ini
[General]
Enabled=1
[Diagnostics]
Logging=0
```

`build/shader-reuse-test.exe` creates a Direct3D 11 device, compiles test shaders, and checks that a duplicate create returns the same object while different bytecode or elements create a new one.

## Optional startup and battle fixes

Copy `build/FFXRuntimeFixes.dll` into the game's `modules` folder to enable these fixes through the FFX module loader.

- The game window uses a black background while Direct3D starts. The hook targets the Phyre game window and chains existing window-class hooks.
- The optional battle sound cue reader skips inaccessible records and forwards valid sound IDs to the game's audio engine. This fix works independently of the FPS mod and checks the expected instructions before patching.

### FMV Volume

Install the [FFX-60fps fork](https://github.com/jshethbright/FFX-60fps) with its Config menu enabled. Open the in-game **Config** menu, scroll to **FMV Volume**, and use left or right to adjust the slider. The menu entry uses English text and appears with the Western text languages.

**50** uses the game's original movie playback level, **0** mutes movies, and **100** doubles their audio gain (about +6 dB). The control adjusts the complete movie soundtrack and preserves the game's movie fades and mutes. At higher settings, loud peaks can reach the movie player's existing clipping limit. The setting is saved under `[Audio]` as `FMVVolume` in `modules/config/FFXRuntimeFixes.ini`.

Run `build/runtime-fixes-test.exe "path/to/FFX.exe"` to validate the sound patches, FMV gain and persistence, and the black window class. The test maps the executable as test data and leaves the game unopened.

## Optional texture and frame profiler

FFXGpuTextureTrace is a 32-bit ReShade add-on. It records frame and Present timing, CPU time for texture and shader creation, and sampled GPU texture transfers when the D3D11 context hook attaches. It also logs local video memory usage and budget when the DXGI adapter exposes them. CPU creation with initial data includes driver work. GPU timestamp samples cover explicit updates and copies.

1. Use a 32-bit ReShade installation with full add-on support.
2. Run `build-gpu-trace.cmd "path\to\reshade\include"` with the ReShade SDK headers. The build was verified with API 18 at the linked revision. [ReShade SDK revision](https://github.com/crosire/reshade/tree/4a50d1eddace85734871d91792ff214f13f66c01/include)
3. Run `build\gpu-trace-test.exe` for an offline D3D11 WARP smoke test.
4. With the game closed, copy `build\FFXGpuTextureTrace.addon32` to the game directory. Move it out before a later launch to remove it.

The add-on writes `ffx-gpu-texture-trace.csv`. With Python 3, compare the latest capture against the prefetch module's slow-read log on the same performance-counter timeline.

```powershell
python analyze-trace.py "path\to\FFX"
```
