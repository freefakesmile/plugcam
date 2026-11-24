# The video decoder: FFmpeg on Direct3D 12

Plugcam turns the phone's H.264 stream into pictures with FFmpeg's decoder running on the graphics
chip through Direct3D 12 video decoding (`plugcam_dec.dll`). The decoder built into Windows
(Media Foundation) is still there as the fallback. This page is for working on that part: why it
is built this way, how to build and update the DLL, and how to measure it.

## Why not the decoder built into Windows

With hardware decoding (DXVA), Windows' H.264 decoder (`msmpeg2vdec.dll`) keeps two of its own
threads busy for as long as the graphics chip works on each picture. On an RTX 3080 Ti that is
about 9 ms of every 33 ms frame, so most of what streaming cost was that waiting. Splitting the
live process by thread (Plugcam at 1080p30, preview hidden):

| Thread | CPU |
|---|---|
| `msmpeg2vdec.dll` (two threads) | 282 ms/s |
| NVIDIA driver (`nvwgf2umx.dll`) | 84 ms/s |
| Plugcam's own pipeline (color conversion, frames to the camera) | 82 ms/s |

None of the decoder's knobs helped: one worker thread, low-latency mode off, and a Direct3D device
without the driver's own thread (`D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`;
that only moved the waiting into Plugcam's thread) all cost the same. Decoding on the CPU was
barely cheaper (317–354 against ~390 ms/s here) and would be much worse on a laptop.

FFmpeg decoding the same recording on the same graphics chip took 25–35 ms/s of CPU, with
`-hwaccel d3d12va` as well as `-hwaccel cuda`. `d3d12va` works with any graphics chip that has
Direct3D 12 video decoding (Windows 10 and later), so that is what Plugcam uses.

## How it fits together

```
scrcpy H.264 ─▶ decode::H264Decoder ─┬─▶ ffdecode::FfDecoder ─▶ plugcam_dec.dll ─▶ FFmpeg h264 + d3d12va
   packets                           │        (Rust, loads the DLL)   (C wrapper)         │ crop/scale: D3D12
                                     │                                                    │ video processor
                                     │                                                    ▼
                                     │                                   NV12 in system memory
                                     └─▶ decode::MfDecoder (Media Foundation, the fallback)
                                                                                          ▼
                                                                    frame::FrameConverter ─▶ camera
```

- **`decoder/plugcam_dec.c`** is the only code in the DLL that is not FFmpeg. It exports a few
  plain C functions (`pc_dec_open`, `pc_dec_send`, `pc_dec_receive`, `pc_dec_set_crop`,
  `pc_dec_close`, `pc_dec_last_error`, `pc_dec_version`, `pc_dec_api_version`) so the Rust side
  never touches FFmpeg's structures. A picture comes back as NV12 planes in system memory, valid
  until the next call.
- **Crop** is done on the graphics chip as with the Windows decoder: `pc_dec_set_crop` sets up a
  Direct3D 12 video processor that cuts the rectangle out and scales it to the size it will have
  in the camera frame, before the picture is read back. On the CPU that scaling costs about 7 ms
  a frame (~210 ms/s at 30 fps). If the video processor cannot be made, the picture comes back
  whole and the frame converter crops it on the CPU.
- **Fallback.** `decode::H264Decoder::new` uses the DLL when it is found and opens; otherwise, and
  for 10 minutes after the FFmpeg decoder failed in a session, it uses Media Foundation as before
  (on the GPU, else on the CPU). The log says which one runs: `decoding on the GPU with FFmpeg 9.1
  (Direct3D 12)` or `decoding on the GPU`.
- **Parameter sets.** scrcpy sends SPS/PPS in a packet of their own; FFmpeg wants them in front of
  the next picture, so `FfDecoder` holds them back and prepends them (scrcpy's own client does
  the same).
- **The DLL is loaded at runtime** from `resources\` (or next to the exe, or `PLUGCAM_DEC_DLL`),
  like `plugcam_cam.dll`. A build without it works, just with the Windows decoder.
- `PC_DEC_API_VERSION` in the wrapper and `API_VERSION` in `src-tauri/src/ffdecode.rs` must match;
  bump both whenever the C functions or `PcFrame` change, so an old DLL is refused rather than
  misread.

## Results

All at 1080p, 30 fps, on an Intel Core i7-13700K (16 threads) with an RTX 3080 Ti.

`replay` of a 45-second Galaxy S10 recording at its own pace (`PLUGCAM_PACE=1`), decoding and
color conversion, whole process:

| | Windows decoder | FFmpeg, Direct3D 12 |
|---|---|---|
| Whole picture | ~376 ms/s | **~78 ms/s** |
| Cropped (15% left and right, 10% top and bottom) | ~430 ms/s | **~83 ms/s** |
| Cropped (30% left, 25% bottom) | ~358 ms/s | **~80 ms/s** |

The pictures are the same: the last frame of the recording is identical byte for byte between the
two decoders, whole and cropped.

The app streaming from the phone (Galaxy S10, rotated 180°, preview hidden), Plugcam's whole
process: **458 → 118 ms/s** of CPU (2.9% → 0.7% of all threads). Decoding now shows on the
graphics chip's *Video Decode* engine (~10%) instead of its 3D engine.

## Building the DLL

The DLL is cross-built on Linux with mingw-w64 (FFmpeg's build wants a POSIX shell and make; the
FFmpeg builds people use on Windows are made this way too). CI does it in an `ubuntu:26.04`
container (the `decoder` job in [build.yml](../.github/workflows/build.yml) and
[release.yml](../.github/workflows/release.yml)) and hands `plugcam_dec.dll` to the Windows job as
an artifact.

By hand, on Ubuntu 26.04 (or in WSL):

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 pkg-config xz-utils curl
sh decoder/build.sh        # -> decoder/out/plugcam_dec.dll and its license files
```

Then on Windows `.\scripts\stage-resources.ps1` copies `decoder\out\plugcam_dec.dll` into
`src-tauri\resources` and the license files into `resources\licenses`; `pnpm tauri build` and
`.\scripts\portable.ps1` pick them up from there. For `cargo run`/`replay`, copying the DLL into
`src-tauri\resources` is enough.

What `build.sh` does:

1. Downloads FFmpeg's release tarball and checks its SHA-256 (pinned in the script).
2. Configures FFmpeg with `--disable-everything`, then only the H.264 decoder and the `h264_d3d12va`
   hwaccel; no formats, filters, scaling, network, programs or assembly (the CPU only parses
   headers here). It fails if `configure` quietly left `d3d12va` out, which happens with
   mingw-w64 headers that are too old.
3. Builds the wrapper against Microsoft's [DirectX-Headers](https://github.com/microsoft/DirectX-Headers)
   (also pinned by checksum; mingw-w64's headers have no Direct3D 12 video processor) and links
   FFmpeg in statically. Only the `pc_dec_*` functions are exported; the DLL needs nothing but
   `kernel32`, `msvcrt` and `bcrypt` (`d3d12.dll` and `dxgi.dll` are loaded when a decoder opens).
4. Writes `FFmpeg-LICENSE.txt`, `FFmpeg-SOURCE.txt` and `DirectX-Headers-LICENSE.txt` next to it.

The result is about 1 MB. FFmpeg is built once and cached in `decoder/build/prefix`;
`FFMPEG_REBUILD=1 sh decoder/build.sh` builds it again, otherwise only the wrapper is rebuilt.

### Updating FFmpeg

1. Set `FFMPEG_VERSION` in `decoder/build.sh` to the new release from
   [ffmpeg.org/download](https://ffmpeg.org/download.html#releases).
2. Check the tarball's signature and put its SHA-256 in `FFMPEG_SHA256`:
   ```sh
   curl -fsSLO https://ffmpeg.org/releases/ffmpeg-X.Y.tar.xz{,.asc}
   curl -fsSL https://ffmpeg.org/ffmpeg-devel.asc | gpg --import
   gpg --verify ffmpeg-X.Y.tar.xz.asc ffmpeg-X.Y.tar.xz   # key FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8
   sha256sum ffmpeg-X.Y.tar.xz
   ```
3. Build, then run the checks below. A new major version of FFmpeg may change
   `AVD3D12VAFrame` or the d3d12va frame handling the crop relies on (see `scale()` in the
   wrapper): read `libavutil/hwcontext_d3d12va.h` and the download path in
   `libavutil/hwcontext_d3d12va.c` for changes.
4. Update the version in `THIRD_PARTY_NOTICES.md` and `scripts/third-party-notices.py`.

## Checking and measuring

`replay` decodes a recording made with `plugcam-cli --dump FILE` without a phone:

```powershell
cd src-tauri
cargo build --release --example replay
$r = "..\target\release\examples\replay.exe"   # or wherever CARGO_TARGET_DIR is
& $r s10-1080p.scrcpy last.bmp                         # as fast as it decodes; saves the last frame
$env:PLUGCAM_PACE = 1; & $r s10-1080p.scrcpy            # at 30 fps: "CPU … ms/s" is like streaming
$env:PLUGCAM_CROP = "15,15,10,10"; & $r s10-1080p.scrcpy  # with a crop (left, right, top, bottom %)
$env:PLUGCAM_DECODER = "windows"; & $r s10-1080p.scrcpy # the Windows decoder, to compare
```

Comparing `last.bmp` from both decoders is the quickest check that the picture is right.
`cargo test --release` includes `tests/fixtures.rs`, which decodes recorded streams with whichever
decoder is available.

In the running app, [`scripts/measure.ps1`](../scripts/measure.ps1) averages CPU and memory of the
whole process tree. To see which threads use the CPU, Process Explorer (Threads tab) or Windows
Performance Recorder show the start module of each thread: the FFmpeg path has no
`msmpeg2vdec.dll` threads at all.

Environment variables:

| Variable | Effect |
|---|---|
| `PLUGCAM_DECODER=windows` | Always use the decoder built into Windows |
| `PLUGCAM_SOFTWARE_DECODE=1` | Windows decoder on the CPU (implies the above) |
| `PLUGCAM_DEC_DLL=path` | Load `plugcam_dec.dll` from there |
| `PLUGCAM_PACE=1` | `replay` only: feed frames at their own pace |
| `PLUGCAM_CROP=l,r,t,b` | `replay` only: crop, in percent |

## Not done yet

- Tested on an NVIDIA RTX 3080 Ti only. Intel and AMD graphics should work the same way
  (Direct3D 12 video decoding is part of their drivers too) but have not been tried.
- On laptops with two graphics chips FFmpeg uses the default adapter; Windows' graphics settings
  for Plugcam.exe decide which one that is.
- Plugcam's own pipeline (NV12 → BGR, rotation, copying frames to the camera) is now most of the
  remaining CPU time (~50–80 ms/s). The Direct3D 12 video processor could also convert to RGB and
  rotate before the picture is read back.
- Wi-Fi streams have not been checked with the new decoder yet.
