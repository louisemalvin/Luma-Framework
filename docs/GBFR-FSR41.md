# GBFR FSR 4.1 prototype

This branch adds an optional FSR 4.1 backend for Granblue Fantasy: Relink. The
game remains D3D11. FSR 4.1 runs through a same-adapter D3D12 device with
shared D3D11/D3D12 textures and fences. FSR3 is still the default automatic
backend and remains the fallback when the provider or bridge is unavailable.

## Requirements

- Windows with the Visual Studio toolchain used by `Luma.sln`.
- An AMD GPU and a driver/Proton combination that supports the signed FSR 4.1
  provider and Shader Model 6.6. AMD's current FSR 4.1.1 documentation lists
  discrete Radeon RX 7000 series or Radeon RX 9000 series and newer hardware;
  the target test GPU is an RX 9060 XT.
- The AMD-supplied signed provider pair. Provider binaries are intentionally
  not included in this repository.

The integration follows AMD's public [FSR API](https://gpuopen.com/manuals/fsr_sdk/getting-started/ffx-api/)
and [FSR upscaler documentation](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/),
including the FSR 4.1.1 context-version extension.

## Provider setup

Place the signed loader and its matching signed FSR upscaler effect DLL in
either the game directory or its `Luma/` subdirectory. The loader checks these
names:

- `amd_fidelityfx_loader_dx12.dll`
- `amd_fidelityfx_upscaler_dx12.dll`

These are a provider pair, not interchangeable alternatives. The loader DLL
must be able to load the matching upscaler effect DLL from the same provider
installation. Do not rename or mix versions of the two files.

For a non-standard location, set `LUMA_FSR41_PROVIDER` to the absolute path of
the loader DLL before starting the game, with the matching effect DLL beside
it. No provider DLL, driver component, or generated runtime cache should be
committed here.

In a Development or Test build, select `FSR 4.1` in the Super Resolution menu.
`Auto` intentionally continues to prefer FSR3 so existing GBFR behavior does
not change. If FSR4.1 initialization fails, an explicit FSR4.1 selection uses
FSR3 when available. A dispatch or resource-bridge failure switches the active
FSR4.1 instance to its FSR3 fallback for the rest of that device lifetime.

## GBFR resource mapping

- Color is passed at render resolution. HDR/linear paths use the existing GBFR
  pre-SR encode pass; SDR paths are marked as sRGB/non-linear for FSR4.1.
- Depth is passed as the GBFR non-inverted device depth resource.
- Motion vectors are unjittered. GBFR supplies the existing negative render
  width/height scales expected by its FSR3 path.
- Jitter is forwarded in pixel units from the existing GBFR table jitter.
  GBFR intentionally keeps its patched eight-phase Halton table so FSR4.1 and
  FSR3 see the same camera sequence; scale-specific FSR4 phase counts are not
  enabled by this prototype.
- The AdaptLuminance texture is used only when it is a 1x1 `R32_FLOAT`
  exposure resource. FSR4.1 auto exposure remains enabled otherwise.
- The D3D12 output is copied back to the existing GBFR D3D11 SR output before
  the normal post-SR encode and post-processing chain.

## Diagnostics and limitations

The bridge fails closed if the native device lacks `ID3D11Device5` or
`ID3D11DeviceContext4`, the matching D3D12 device cannot provide Shader Model
6.6, a shared texture cannot be created/imported, or a fence/dispatch fails.
Failure messages are emitted with the `FSR4.1` prefix. Development traces label
the active path as `FSR4.1` or `FSR3 fallback`.

The bridge currently uses synchronous D3D11 copies around the D3D12 dispatch.
This is deliberate for correctness and makes bridge cost a measured result,
not an assumption. Unsupported resource formats, device loss, alt-tab
resource replacement, and resolution changes recreate the cached bridge
resources or fall back safely. Frame generation and dynamic-resolution control
are not part of this prototype.

## Build and benchmark

Build `Granblue Fantasy Relink` from `Luma.sln` for the required Windows
configuration. The project links the system `d3d12.lib` and `dxgi.lib`; the
FSR4.1 provider remains a runtime dependency.

For a Steam/Proton run, use MangoHud in the launch options:

```text
MANGOHUD=1 MANGOHUD_CONFIG=fps,frametime,frame_timing,gpu_stats,vram,ram %command%
```

Capture the same save, camera route, output resolution, render scale, display
mode, driver, Proton version, and MangoHud configuration for each case:

| Case | Output/render scale | GPU frame time | FPS / 1% low | Frametime p95 | VRAM | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Native / game TAA | pending | pending | pending | pending | pending | baseline |
| FSR3 | pending | pending | pending | pending | pending | known-good path |
| FSR4.1 | pending | pending | pending | pending | pending | include bridge overhead |

The benchmark is not considered complete until the table is filled from the
actual target game, the ReShade log confirms which backend ran, and the native
Windows run has equivalent GPU-time, frame-pacing, and memory captures.
