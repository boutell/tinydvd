# TASK SPEC: tinydvd Player & Transcoder

## 1. System Objective
Build a cross-platform desktop application and accompanying CLI authoring tool that preserves full interactive DVD-Video functionality (menus, button overlays, multi-angle branching, audio/subtitle selection) while replacing high-bitrate MPEG-2 video payloads with hardware-accelerated H.264/AVC streams stored in Matroska (`.mkv`) files.

Long-Term Target Platforms: Linux (Debian/X11/Wayland), macOS (Metal), Windows (Direct3D 11).

Initial Target Platform: Linux (Debian 13, Wayland). That is what you must build for today.

---

## 2. Architecture & File Format Specs

### 2.1 File Separation
Each DVD title set and menu domain is decomposed into two paired assets:
1. `VIDEO_TS.mvob` / `VTS_xx_0.mvob` / `VTS_xx_1..N.mvob` ("Compact Meta-VOB"):
   - Preserves 2048-byte Navigation Packs (`NV_PCK` with PCI button tables and DSI sector metadata).
   - Preserves Subpicture (`SPU`) 2-bit RLE bitmap packs.
   - Collapses all video packs (`0x000001E0`) of each VOBU into a **single** 2048-byte sector containing the `mvob_video_ref_t` pointer descriptor (reducing `.mvob` size by ~98.8% instead of padding every original video sector with zeroes).
2. `VIDEO_TS_VIDEO.mkv` / `VTS_xx_0_VIDEO.mkv` / `VTS_xx_VIDEO.mkv`:
   - Video stream encoded with `libx264` (`-crf 18 -preset slow -tune film`) + multi-track stereo AAC audio (`-c:a aac -b:a 192k`).
   - Preserves still-frame menu timelines (`tpad`) and continuous multi-VOB title timelines (`concat:` protocol).
   - Storage aspect ratio preserved at 720x480 (SAR 32:27 or 8:9) to match SPU pixel coordinates 1:1.

### 2.2 Binary Descriptor (`mvob_video_ref_t`)
Packed struct written inside the PES payload of hollowed video sectors:

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 0x4D564F42 ("MVOB")
    uint16_t version;         // 0x0001
    uint16_t vts_id;          // Video Title Set index (e.g., 1 for VTS_01)
    uint32_t vobu_index;      // VOBU index within cell
    uint64_t pts_nanos;       // Target presentation timestamp in MKV
    uint32_t duration_nanos;  // VOBU duration in nanoseconds
    uint8_t  is_idr;          // 1 if VOBU start boundary (IDR frame)
    uint8_t  reserved[15];    // Zero padding
} mvob_video_ref_t;
#pragma pack(pop)
```

---

## 3. Repository Layout & File Manifest

```text
dvd-hybrid/
├── CMakeLists.txt
├── scripts/
│   └── dvd-hybrid-transcode.py    # Offline extraction, qpfile gen, and transcode
└── src/
    ├── main.c                     # SDL2/GLFW window, event pump, main loop
    ├── mvob_parser.h
    ├── mvob_parser.c              # Sector reader and descriptor parser
    ├── dvd_vm_bridge.h
    ├── dvd_vm_bridge.c            # libdvdnav wrapper, register state, VM events
    ├── mpv_controller.h
    ├── mpv_controller.c           # libmpv context, render context, async seeking
    ├── spu_compositor.h
    └── spu_compositor.c           # 2-bit SPU decoder, CLUT palette mapping, OSD blit
```

---

## 4. Implementation Steps

### Step 1: Transcoder & Packager CLI (`scripts/dvd-hybrid-transcode.py`)
1. Read source DVD directory or ISO using `libdvdread` / `dvdread` Python bindings.
2. Traverse all `NV_PCK` headers in `VTS_xx_*.VOB`:
   - Record `VOBU_S_PTM` (Start Presentation Time) for each VOBU.
   - Convert timestamps into frame indices using disc frame rate (29.97 / 23.976 / 25 fps).
3. Emit `vobu_keyframes.txt` formatted for `x264`:
   ```text
   0 I -1
   15 I -1
   30 I -1
   ```
4. Run `ffmpeg` to encode companion video:
   ```bash
   ffmpeg -i original_title.vob -an -sn \
     -c:v libx264 -crf 18 -preset slow -tune film \
     -x264-params "qpfile=vobu_keyframes.txt:open-gop=0:scenecut=0" \
     -sar 32:27 \
     VTS_01_VIDEO.mkv
   ```
5. Emit Compact `VTS_xx_*.mvob` and `VIDEO_TS.mvob`:
   - Iterate through original VOBUs.
   - Copy `NV_PCK` and `SPU` subpicture sectors verbatim.
   - Collapse all video sectors in each VOBU into a single 2048-byte `mvob_video_ref_t` pointer sector (and omit redundant raw audio sectors that are muxed into the companion `.mkv`).

### Step 2: Custom DVD Virtual Machine & Compact `.mvob` Navigator (`src/dvd_vm_bridge.c`)
1. Open `.IFO` tables using `libdvdread` (`DVDOpen`, `ifoOpen`).
2. Implement the 8-byte DVD VM instruction evaluator (`Groups 0..6`, `GPRM[0..15]`, `SPRM[0..23]`, `RSM` resume state).
3. Index compact `.mvob` files per domain (`VMGM`, `VTSM`, `VTSTT`), mapping original DVD sector LBNs and `(VOB_ID, C_ID)` cells to compact `.mvob` file offsets and `mvob_video_ref_t` timestamps.
4. During VOBU playback:
   - Parse `NV_PCK` PCI/DSI via `navRead_PCI` and update active menu button highlights.
   - Feed `SPU` sectors to `spu_compositor`.
   - Check if `pts_nanos` is consecutive with `expected_next_pts_nanos` (within 250ms). When consecutive, allow `mpv` to play continuously without seeking; when discontinuous, call `mpv_controller_seek_exact(pts_nanos)`.

### Step 3: Media Decoder & Renderer (`src/mpv_controller.c`)
1. Instantiate `mpv_handle` and configure options:
   - `hr-seek=yes`
   - `keep-open=yes`
   - `audio-pitch-correction=no`
2. Create `mpv_render_context` using platform-appropriate graphics API (OpenGL for Linux, Metal for macOS, D3D11 for Windows).
3. Implement `mpv_controller_seek_exact(uint64_t pts_nanos)`:
   - Execute asynchronous seek command:
     ```c
     const char *args[] = {"seek", pts_str, "absolute+exact", NULL};
     mpv_command_async(mpv_handle, 0, args);
     ```
   - Since IDR boundaries align with VOBU jumps, seek latency is instantaneous.

### Step 4: SPU Highlight Compositor & User Input (`src/spu_compositor.c`, `src/main.c`)
1. Extract 2-bit SPU run-length-encoded (RLE) packets from `.mvob`.
2. Parse 16-color YCbCr palette (CLUT) from `.IFO` Program Chain tables.
3. On highlight events, map button rect to active palette index and alpha transparency.
4. Render the resulting RGBA overlay to screen via `mpv_render_context_render` or OSD overlay buffer.
5. In `main.c`, capture SDL2 events:
   - Map mouse coordinates from window space to 720x480 coordinate space:
     - $X_{dvd} = X_{win} \times \frac{720}{W_{win}}$
     - $Y_{dvd} = Y_{win} \times \frac{480}{H_{win}}$
   - Pass mouse coordinates to `dvdnav_mouse_select()`.
   - Map keyboard arrow keys and Enter to `dvdnav_upper_button_select()`, `dvdnav_lower_button_select()`, and `dvdnav_button_activate()`.

---

## 5. Build Requirements & Dependencies
- C11 / C++17 compiler (GCC, Clang, or MSVC)
- CMake >= 3.20
- `libdvdnav` & `libdvdread`
- `libmpv` (v2.0+)
- `SDL2` (windowing & input handling)
- `ffmpeg` CLI with `libx264` enabled (runtime authoring pipeline)
