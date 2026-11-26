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
