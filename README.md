# tinydvd

**tinydvd** shrinks full DVDs down to a fraction of their original size (~75–80% smaller, e.g., **7.8 GB → 1.8 GB**) using modern H.264/AAC `.mkv` compression **without losing the interactive DVD experience**.

Unlike standard rippers that throw away menus or split a disc into dozens of disconnected files, **tinydvd** keeps everything that makes a DVD a DVD:
- **Full Interactive DVD Menus:** Root menus, animated transitions, scene selection, language/audio setup, and bonus feature navigation work just like a real DVD player.
- **Massive Space Savings:** Replaces bulky 1990s MPEG-2 video streams with modern H.264 video in `.mkv` containers, while collapsing the DVD navigation/subtitle stream into tiny `.mvob` ("Meta-VOB") files (~99% smaller than original `.VOB`s).
- **Single-File `.tdvd` Format:** Packages the entire converted disc into a single `.tdvd` (or `.tinydvd`) file with instant, zero-copy random access—ideal for double-clicking in file managers.
- **Closed Captions, Subtitles & Multi-Track Audio:** Preserves original DVD Closed Captions (SDH), multi-language subtitles, and multiple audio tracks with instant toggling during playback.
- **Modern Player Controls:** Navigate menus with keyboard or mouse, jump around the clip timeline (`-10s`/`+10s`, `-60s`/`+60s`, `0%`–`90%`), and step through chapters with On-Screen Display (OSD) feedback.

---

## 1. How to Build It

### Install Dependencies

Run the automatic dependency installer for macOS or Linux:

```bash
./scripts/install-deps.sh
```

<details>
<summary>Or install dependencies manually (macOS / Debian / Ubuntu / Fedora / Arch)</summary>

#### macOS (Homebrew)
```bash
brew install cmake pkg-config libdvdnav libdvdread libdvdcss dvdbackup mpv sdl2 ffmpeg python3
```

#### Linux (Debian / Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config \
    libdvdnav-dev libdvdread-dev dvdbackup \
    libmpv-dev libsdl2-dev libgl1-mesa-dev \
    ffmpeg python3 python3-pip
```

#### Linux (Fedora / RHEL)
```bash
sudo dnf install -y \
    gcc gcc-c++ cmake pkgconf-pkg-config \
    libdvdnav-devel libdvdread-devel dvdbackup \
    mpv-libs-devel SDL2-devel mesa-libGL-devel \
    ffmpeg python3
```

#### Linux (Arch Linux)
```bash
sudo pacman -S --needed cmake base-devel pkgconf libdvdnav libdvdread dvdbackup mpv sdl2 ffmpeg python
```
</details>

### Compile `tinydvd`

```bash
cmake -B build -S .
cmake --build build
```

This builds the player binary at `./build/tinydvd`.

---

## 2. How to Use It in Practice

### Step 1: Rip a DVD (If Starting from a Physical Disc)

If you are starting from a commercial encrypted DVD in an optical drive, use `dvdbackup` to rip a decrypted copy to disk first (if you already have a decrypted DVD folder or `.iso`, skip to Step 2):

```bash
# macOS (find your DVD drive with `diskutil list`, e.g., /dev/rdisk4):
dvdbackup -M -i /dev/rdisk4 -o ./rips

# Linux (default optical drive /dev/sr0):
dvdbackup -M -i /dev/sr0 -o ./rips
```

### Step 2: Convert with `convert-to-tinydvd`

Run [`scripts/convert-to-tinydvd`](file:///Users/boutell/boutell/tinydvd/scripts/convert-to-tinydvd.py) on your ripped DVD folder or `.iso` image to produce a single `.tdvd` file (or an unpacked folder):

```bash
# Produce a single-file .tdvd archive:
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ./tinydvds/MY_MOVIE.tdvd

# Or from an unencrypted ISO image:
./scripts/convert-to-tinydvd /path/to/movie.iso ./tinydvds/MY_MOVIE.tdvd

# Or package an already-converted tinydvd directory into a single .tdvd file without re-encoding:
./scripts/convert-to-tinydvd ./tinydvds/MY_MOVIE ./tinydvds/MY_MOVIE.tdvd

# Custom H.264 CRF quality and preset:
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ./tinydvds/MY_MOVIE.tdvd --crf 18 --preset slow
```

### Step 3: Play with `tinydvd`

Launch the player pointing to your `.tdvd` file (or converted directory), or map `.tdvd` / `.tinydvd` to `tinydvd` in your file manager:

```bash
./build/tinydvd ./tinydvds/MY_MOVIE.tdvd

# Or start directly in fullscreen:
./build/tinydvd --fullscreen ./tinydvds/MY_MOVIE.tdvd

# Unpacked tinydvd directories are also supported:
./build/tinydvd ./tinydvds/MY_MOVIE
```

### Thumbnails & Cover Art (`tinydvd-set-thumbnail`)

While new `.tdvd` files automatically include a menu frame thumbnail during conversion, you can embed or update thumbnails in existing `.tdvd` archives at any time without re-encoding or copying video data:

```bash
# Automatically extract and embed a menu/movie frame in an existing .tdvd:
./scripts/tinydvd-set-thumbnail ./tinydvds/MY_MOVIE.tdvd

# Embed a custom movie poster or cover image:
./scripts/tinydvd-set-thumbnail ./tinydvds/MY_MOVIE.tdvd ./posters/cover.jpg

# Batch-add thumbnails to an entire folder of .tdvd files:
./scripts/tinydvd-set-thumbnail ./tinydvds/*.tdvd

# Check which .tdvd archives already have embedded thumbnails:
./scripts/tinydvd-set-thumbnail --check ./tinydvds/*.tdvd

# Overwrite existing thumbnails with --force:
./scripts/tinydvd-set-thumbnail --force ./tinydvds/MY_MOVIE.tdvd
```

### GNOME / Linux Desktop Integration (`.tdvd` File Association)

To install `tinydvd` so `.tdvd` and `.tinydvd` files show thumbnails and disc icons in GNOME Files (Nautilus) and play when double-clicked:

```bash
./install
```

It builds `tinydvd`, then asks for your password (via `sudo`) and installs under `/usr/local`:
1. The `tinydvd` player, `convert-to-tinydvd`, `tinydvd-set-thumbnail` and `tinydvd-thumbnailer` in `/usr/local/bin`.
2. The `tinydvd` application launcher.
3. The `video/x-tinydvd` file type for `*.tdvd` and `*.tinydvd`, with `tinydvd` as its default application.
4. The application and file icons.
5. The Nautilus thumbnailer, which shows each disc's embedded thumbnail (see `tinydvd-set-thumbnail` above).

Everything goes under `/usr/local` because GNOME runs thumbnailers in a sandbox that can't see your home folder, so a thumbnailer installed in `~/.local/bin` never runs.

If GNOME Files was already open, restart it with `nautilus -q` to see thumbnails.

To remove it all again:
```bash
./install --uninstall
```

### Player Controls

| Key / Input | Action |
| --- | --- |
| **Arrow Keys** (`↑` `↓` `←` `→`) *(in Menu)* | Navigate interactive DVD menu buttons |
| **Enter** / **Space** *(in Menu)* | Activate ("click") highlighted menu button |
| **Mouse Move / Left Click** | Hover and click DVD menu buttons directly (the cursor hides after 2 seconds without mouse activity) |
| **Left / Right Arrow** (`←` `→`) *(in Clip)* | Seek **-10s / +10s** along the clip timeline |
| **Shift + Left / Right** (`⇧←` `⇧→`) | Seek **-60s / +60s** (-1 min / +1 min) along the clip timeline |
| **Down / Up Arrow** (`↓` `↑`) *(in Clip)* | Seek **-60s / +60s** (-1 min / +1 min) along the clip timeline |
| **`,` / `.`** *(Comma / Period)* | Seek **-10s / +10s** along the clip timeline |
| **`0` – `9`** | Jump directly to **0% – 90%** of the current clip's timeline |
| **`[` / `]`** or **PageUp / PageDown** | Jump to **Previous / Next Chapter** |
| **`C`** or **`S`** | Toggle / cycle **Closed Captions & Subtitles** (`Off` → `Track 1 [CC/SDH]` → `Track 2` → … → `Off`) |
| **`A`** | Cycle **Audio** tracks (`Track 1` → `Track 2` → …) |
| **Space** *(in Clip)* | Toggle playback pause |
| **M** | Return to DVD Root Menu |
| **Escape** | Go up a menu level (or exit if at root menu) |
| **F** | Toggle Fullscreen |
| **Q** / **Ctrl-C** | Quit |

---

## 3. Technical Details (Under the Hood)

### System Architecture

Each DVD title set and menu domain is decomposed into two paired assets:

1. **Compact Meta-VOB (`VIDEO_TS.mvob`, `VTS_xx_0.mvob`, `VTS_xx_1..N.mvob`):**
   - Preserves original 2048-byte Navigation Packs (`NV_PCK`) with PCI button highlight tables and DSI metadata.
   - Collapses **all** MPEG-2 video sectors in each VOBU into a **single** 2048-byte sector containing the 40-byte [`mvob_video_ref_t`](file:///Users/boutell/boutell/tinydvd/src/mvob_parser.h#L16-L27) pointer descriptor.
   - Preserves Subpicture (`SPU`) 2-bit RLE overlay sectors (`0x20..0x3F`) for interactive menu buttons, Closed Captions (SDH), and subtitles.
   - Reduces `.mvob` container size by **~98.8%** compared to original `.VOB` files (e.g., ~96 MB total across a 7.8 GB dual-layer DVD).

2. **Companion Matroska Video + Audio (`VIDEO_TS_VIDEO.mkv`, `VTS_xx_0_VIDEO.mkv`, `VTS_xx_VIDEO.mkv`):**
   - Encodes video with `libx264` (`-crf 18 -preset slow -tune film`) and all audio tracks with stereo AAC (`-c:a aac -b:a 192k`).
   - Preserves still-frame menu timelines (`tpad`) so every menu `VOB_ID` maps 1:1 to its `NV_PCK` presentation timestamps.
   - Uses byte-level `concat:` protocol on multi-file title VOBs (`VTS_xx_1..N.VOB`) to preserve continuous MPEG-PS timestamps across 1 GB VOB splits.
   - Maintains storage aspect ratio (720x480, SAR 32:27 or 8:9) to match SPU pixel coordinates 1:1.

3. **Custom DVD Virtual Machine & Smooth Playback Engine ([`src/dvd_vm_bridge.c`](file:///Users/boutell/boutell/tinydvd/src/dvd_vm_bridge.c)):**
   - Replaces `libdvdnav`'s rigid optical sector reader with a dedicated DVD VM instruction evaluator (`Groups 0..6`, `GPRM[0..15]`, `SPRM[0..23]`, `RSM` resume slots) and compact `.mvob` indexer built on `libdvdread` (`ifoOpen`, `navRead_PCI`, `navRead_DSI`).
   - Tracks expected presentation timestamps across VOBUs: when consecutive VOBUs have contiguous timestamps, `libmpv` ([`src/mpv_controller.c`](file:///Users/boutell/boutell/tinydvd/src/mpv_controller.c)) continues playing smoothly without issuing redundant seeks.
   - Composites 2-bit RLE DVD subpictures and menu button highlights over the `libmpv` OpenGL video frame via [`src/spu_compositor.c`](file:///Users/boutell/boutell/tinydvd/src/spu_compositor.c), including timed DCSQ `STP_DSP` expiration for movie Closed Captions and subtitles.

4. **Single-File `.tdvd` / `.tinydvd` Container ([`src/tdvd_archive.c`](file:///Users/boutell/boutell/tinydvd/src/tdvd_archive.c)):**
   - Uses a standard uncompressed ZIP (`ZIP_STORED`, with automatic `ZIP64` support for files > 4 GB) so the Central Directory at the end of the file provides $O(1)$ lookup of every constituent `.IFO`, `.mvob`, and `.mkv` file's contiguous `[offset, size)` byte range.
   - Because `.mkv` video/audio is already compressed, `ZIP_STORED` adds zero CPU overhead and allows both `.mvob` navigation sectors (`fseeko`) and `.mkv` video streams (`mpv_stream_cb_add_ro` custom `tdvd://` protocol in [`src/mpv_controller.c`](file:///Users/boutell/boutell/tinydvd/src/mpv_controller.c) using `pread`) to be streamed and seeked **directly in-place from the `.tdvd` file without unpacking video to `/tmp`**.
   - Any `.tdvd` file can also be inspected with standard tools (`unzip -l MY_MOVIE.tdvd`).

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

### Transcoded Output Directory Layout

```text
tinydvds/MY_MOVIE/
├── VIDEO_TS.IFO                  # Original Video Manager navigation metadata
├── VIDEO_TS.BUP                  # VMG backup
├── VIDEO_TS.mvob                 # Compact VMGM menu Meta-VOB (NV_PCK + 1 video ref/VOBU + SPU)
├── VIDEO_TS_VIDEO.mkv            # Companion VMGM menu H.264 + AAC video/audio
├── VTS_01_0.IFO                  # Title set 1 navigation metadata
├── VTS_01_0.BUP                  # Title set 1 backup
├── VTS_01_0.mvob                 # Compact VTSM menu Meta-VOB (NV_PCK + 1 video ref/VOBU + SPU)
├── VTS_01_0_VIDEO.mkv            # Companion VTSM menu H.264 + AAC video/audio
├── VTS_01_1.mvob                 # Compact title Meta-VOB (~99% smaller than original VOB)
├── VTS_01_1.VOB -> VTS_01_1.mvob # Compatibility symlink
├── VTS_01_VIDEO.mkv              # Companion H.264 video + multi-track AAC audio
└── vts_01_keyframes.txt          # Generated x264 keyframe table
```

### How `dvdbackup` Uses `libdvdcss`

- **macOS (Homebrew):** `libdvdread` links directly to `libdvdcss.2.dylib`. `dvdbackup` loads `libdvdcss` automatically with no extra environment variables.
- **Linux (Debian / Ubuntu):** `libdvd-pkg` compiles and installs `libdvdcss.so.2` into `/usr/lib/`, which `libdvdread` loads via `dlopen("libdvdcss.so.2")`.
- **Debugging CSS Decryption:** Run `DVDCSS_VERBOSE=2 dvdbackup -M -i /dev/rdisk4 -o ./rips` to inspect CSS drive authentication and title key descrambling logs.

### Running Tests

```bash
# C unit & integration tests (MVOB parser, SPU compositor, and DVD VM bridge)
./build/test_mvob
./build/test_spu
./build/test_dvd_vm

# Python transcoder & packager integration test
python3 tests/test_transcode.py
```
