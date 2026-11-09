# tinydvd

**tinydvd** is an interactive DVD-Video hybrid player and authoring toolchain. It preserves full interactive DVD-Video functionality (interactive menus, button overlays, multi-angle branching, and audio/subtitle selection) while replacing high-bitrate legacy MPEG-2 video payloads with modern, hardware-accelerated H.264/AVC streams in Matroska (`.mkv`) containers.

---

## 1. System Architecture

Each DVD title set and menu domain is decomposed into two paired assets:

1. **Compact Meta-VOB (`VIDEO_TS.mvob`, `VTS_xx_0.mvob`, `VTS_xx_1..N.mvob`):**
   - Preserves original 2048-byte Navigation Packs (`NV_PCK`) with PCI button highlight tables and DSI metadata.
   - Collapses **all** MPEG-2 video sectors in each VOBU into a **single** 2048-byte sector containing the 40-byte `mvob_video_ref_t` pointer descriptor.
   - Preserves Subpicture (`SPU`) 2-bit RLE overlay sectors for interactive menu buttons and subtitles.
   - Reduces `.mvob` container size by **~98.8%** compared to original `.VOB` files (e.g., ~94 MB total across a 7.85 GB dual-layer DVD).

2. **Companion Matroska Video + Audio (`VIDEO_TS_VIDEO.mkv`, `VTS_xx_0_VIDEO.mkv`, `VTS_xx_VIDEO.mkv`):**
   - Encodes video with `libx264` (`-crf 18 -preset slow -tune film`) and all audio tracks with stereo AAC (`-c:a aac -b:a 192k`).
   - Preserves still-frame menu timelines (`tpad`) so every menu `VOB_ID` maps 1:1 to its `NV_PCK` presentation timestamps.
   - Uses byte-level `concat:` protocol on multi-file title VOBs (`VTS_xx_1..N.VOB`) to preserve continuous MPEG-PS timestamps across 1 GB VOB splits.
   - Maintains storage aspect ratio (720x480, SAR 32:27 or 8:9) to match SPU pixel coordinates 1:1.

3. **Custom DVD Virtual Machine & Smooth Playback Engine (`src/dvd_vm_bridge.c`):**
   - Replaces `libdvdnav`'s rigid optical sector reader with a dedicated DVD VM instruction evaluator (`Groups 0..6`, `GPRM[0..15]`, `SPRM[0..23]`, `RSM` resume slots) and compact `.mvob` indexer built on `libdvdread` (`ifoOpen`, `navRead_PCI`, `navRead_DSI`).
   - Tracks expected presentation timestamps across VOBUs: when consecutive VOBUs have contiguous timestamps, `libmpv` continues playing smoothly without issuing redundant seeks.

### Binary Descriptor (`mvob_video_ref_t`)

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

## 2. Dependencies & Installation

### Automatic Installation Script

An installer script is provided in `scripts/install-deps.sh` that detects your platform and installs all prerequisites:

```bash
./scripts/install-deps.sh
```

### Manual Requirements

#### macOS (via Homebrew)
```bash
brew install \
    cmake \
    pkg-config \
    libdvdnav \
    libdvdread \
    libdvdcss \
    dvdbackup \
    mpv \
    sdl2 \
    ffmpeg \
    python3
```

#### Linux (Debian 13 / Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libdvdnav-dev \
    libdvdread-dev \
    libdvd-pkg \
    dvdbackup \
    libmpv-dev \
    libsdl2-dev \
    libgl1-mesa-dev \
    ffmpeg \
    python3 \
    python3-pip
```

#### Linux (Fedora / RHEL)
```bash
sudo dnf install -y \
    gcc gcc-c++ cmake pkgconf-pkg-config \
    libdvdnav-devel libdvdread-devel \
    dvdbackup \
    mpv-libs-devel SDL2-devel mesa-libGL-devel \
    ffmpeg python3
```

#### Linux (Arch Linux)
```bash
sudo pacman -S --needed cmake base-devel pkgconf libdvdnav libdvdread dvdbackup mpv sdl2 ffmpeg python
```

---

## 3. Building

Use CMake (version 3.20+) to build the player application and unit test suites:

```bash
cmake -B build -S .
cmake --build build
```

This generates:
- `build/tinydvd`: The main player executable (also symlinked as `build/tinydvd-player`).
- `build/test_mvob`: MVOB parser unit tests.
- `build/test_spu`: SPU compositor unit tests.

---

## 4. End-to-End Workflow

### Step 1 (If Commercial DVD): Strip CSS Copy Protection

Commercial DVDs use CSS (Content Scramble System) encryption. Because `dvd-hybrid-transcode.py` packages the original audio and subtitle packs directly into `.mvob` containers, those assets must be descrambled first.

Use `dvdbackup` (which uses `libdvdread` and `libdvdcss`) to rip a clean, decrypted mirror of the disc structure.

#### How `dvdbackup` Finds `libdvdcss`:
- **macOS (Homebrew)**: `libdvdread` is compiled with an explicit direct dynamic link to `/opt/homebrew/opt/libdvdcss/lib/libdvdcss.2.dylib`. When `dvdbackup` executes, macOS dyld loads `libdvdcss` into the process automatically with no additional environment variables needed.
- **Linux (Debian / Ubuntu)**: Because precompiled `libdvdcss2` is not distributed in Debian's main archive for licensing reasons, `libdvd-pkg` is used. The installer script automates `sudo dpkg-reconfigure -f noninteractive libdvd-pkg` to fetch the source, compile, and install `libdvdcss.so.2` into `/usr/lib/`. `libdvdread` discovers it via standard `dlopen("libdvdcss.so.2")`.

#### Verifying `libdvdcss` Decryption:
You can verify that `libdvdcss` is actively authenticating and descrambling keys by setting `DVDCSS_VERBOSE=2`:
```bash
# Verbose verification mode (prints CSS authentication and key descrambling logs):
DVDCSS_VERBOSE=2 dvdbackup -M -i /dev/rdisk4 -o ./rips
```
When working, `libdvdcss` will print lines confirming CSS authentication:
```text
libdvdcss debug: opening target '/dev/rdisk4'
libdvdcss debug: using CSS key cache dir /Users/username/.dvdcss/
libdvdcss debug: authenticating drive...
libdvdcss debug: decrypted title key for title 1...
```
Decrypted keys are automatically cached in `~/.dvdcss/` so future reads are instantaneous.

#### macOS:
```bash
# 1. Identify your optical drive disk identifier:
diskutil list
# (e.g. /dev/rdisk4)

# 2. Rip and descramble the full disc structure:
dvdbackup -M -i /dev/rdisk4 -o ./rips
```

#### Linux:
```bash
# Rip and descramble from default DVD drive (/dev/sr0 or /dev/dvd):
dvdbackup -M -i /dev/sr0 -o ./rips
```

> **Note**: If you already have an ISO image that was created using tools like MakeMKV, HandBrake, or K3b, CSS was already removed during ripping and you can proceed directly to Step 2.

---

### Step 2: Transcode with `dvd-hybrid-transcode.py`

The transcoder decomposes the DVD source, extracts VOBU boundaries, generates the x264 `qpfile`, encodes the companion MKV, and generates the `.mvob` container.

#### What `/path/to/source_dvd` points to:
The transcoder accepts any of the following input formats:
1. **A DVD root directory** containing a `VIDEO_TS` subfolder (e.g. `./rips/MY_MOVIE` or `/Volumes/DVD`).
2. **A `VIDEO_TS` directory directly** containing `.IFO`, `.BUP`, and `.VOB` files (e.g. `./rips/MY_MOVIE/VIDEO_TS`).
3. **A DVD `.iso` / `.img` image file** (e.g. `/path/to/movie.iso`):
   - Automatically mounted in read-only mode during transcoding and safely detached upon completion.
4. **Any individual `.VOB` / `.IFO` file** from a ripped title (the transcoder will automatically locate its sibling title files in that directory).

#### Running the Transcoder:

```bash
# Transcode a ripped DVD directory:
./scripts/dvd-hybrid-transcode.py ./rips/MY_MOVIE ./transcoded_output

# Or transcode an unencrypted ISO image:
./scripts/dvd-hybrid-transcode.py /path/to/movie.iso ./transcoded_output

# Or specify custom encoding quality, preset, and frame rate:
./scripts/dvd-hybrid-transcode.py ./rips/MY_MOVIE ./transcoded_output \
    --crf 18 \
    --preset slow \
    --tune film \
    --fps 29.97 \
    --sar 32:27
```

#### Output Directory Layout:
```text
transcoded_output/
├── VIDEO_TS.IFO           # Original Video Manager navigation metadata
├── VIDEO_TS.BUP           # VMG backup
├── VIDEO_TS.mvob          # Compact VMGM menu Meta-VOB (NV_PCK + 1 video ref/VOBU + SPU)
├── VIDEO_TS_VIDEO.mkv     # Companion VMGM menu H.264 + AAC video/audio
├── VTS_01_0.IFO           # Title set 1 navigation metadata
├── VTS_01_0.BUP           # Title set 1 backup
├── VTS_01_0.mvob          # Compact VTSM menu Meta-VOB (NV_PCK + 1 video ref/VOBU + SPU)
├── VTS_01_0_VIDEO.mkv     # Companion VTSM menu H.264 + AAC video/audio
├── VTS_01_1.mvob          # Compact title Meta-VOB (~99% smaller than original VOB)
├── VTS_01_1.VOB -> VTS_01_1.mvob  # Compatibility symlink
├── VTS_01_VIDEO.mkv       # Companion H.264 video + multi-track AAC audio
└── vts_01_keyframes.txt   # Generated x264 keyframe table
```

---

### Step 3: Play with `tinydvd`

Launch the player from the command line pointing to the transcoded output directory:

```bash
./build/tinydvd ./transcoded_output

# Or launch directly into fullscreen:
./build/tinydvd --fullscreen ./transcoded_output
```

#### Player Controls

| Key / Input | Action |
| --- | --- |
| **Arrow Keys** (`↑` `↓` `←` `→`) *(in Menu)* | Navigate interactive DVD menu buttons |
| **Enter** / **Space** *(in Menu)* | Activate ("click") highlighted menu button |
| **Mouse Move** | Select DVD menu button under cursor |
| **Mouse Left Click** | Activate DVD menu button directly |
| **Left / Right Arrow** (`←` `→`) *(in Clip)* | Seek **-10s / +10s** along the clip timeline |
| **Shift + Left / Right** (`⇧←` `⇧→`) *(or `↓` `↑`)* | Seek **-60s / +60s** (-1 min / +1 min) along the clip timeline |
| **`,` / `.`** *(Comma / Period)* | Seek **-10s / +10s** along the clip timeline |
| **`0` – `9`** | Jump directly to **0% – 90%** of the current clip's timeline |
| **`[` / `]`** or **PageUp / PageDown** | Jump to **Previous / Next Chapter** |
| **`C`** or **`S`** | Toggle / cycle **Closed Captions & Subtitles** (`Off` → `Track 1 [CC/SDH]` → `Track 2` → … → `Off`) |
| **`A`** | Cycle **Audio** tracks (`Track 1` → `Track 2` → …) |
| **Space** *(in Clip)* | Toggle playback pause |
| **M** / **Escape** | Return to DVD Root Menu |
| **F** | Toggle Fullscreen |
| **Q** / **Ctrl-C** | Quit |

#### Timeline Navigation & Closed Captions

- **Moving Around the Timeline:**
  - During clip/movie playback (when no interactive menu buttons are active), **Left/Right Arrows** (or `,` / `.`) seek **-10s / +10s**, **Shift+Left / Shift+Right** (or **Down/Up Arrows**) seek **-60s / +60s**, number keys **`0`–`9`** jump to **0%–90%** of the clip, and **`[` / `]`** (or **PageUp / PageDown**) step between chapters.
  - Each timeline jump synchronizes the DVD Virtual Machine's active Cell and VOBU index with `libmpv` and displays an on-screen OSD timestamp and chapter indicator (`Timeline: HH:MM:SS / HH:MM:SS (Chapter X/Y)`).
- **Closed Captions & Subtitles:**
  - All DVD SPU Closed Caption (SDH) and subtitle streams (`0x20..0x3F`) are preserved in the compact `.mvob` files and rendered with timed DCSQ `STP_DSP` expiration via the OpenGL SPU compositor.
  - Press **`C`** or **`S`** at any time during playback to toggle or cycle through available Closed Caption / Subtitle tracks (or select them from the DVD's interactive Languages / Subtitles menu).

---

## 5. Running Tests

Unit tests and integration tests can be run directly:

```bash
# C unit tests (MVOB sector parser, SPU compositor, and custom DVD VM)
./build/test_mvob
./build/test_spu
./build/test_dvd_vm

# Python transcoder & packager integration test
python3 tests/test_transcode.py
```
