# tinydvd

**tinydvd** shrinks full DVDs down to a fraction of their original size (~75–80% smaller, e.g., **7.8 GB → 1.8 GB**) using modern H.264/AAC compression **without losing the interactive DVD experience**.

Unlike standard rippers that throw away menus or split a disc into dozens of disconnected files, **tinydvd** keeps everything that makes a DVD a DVD:
- **Full Interactive DVD Menus:** Root menus, animated transitions, scene selection, language/audio setup, and bonus feature navigation work just like a real DVD player.
- **Massive Space Savings:** Replaces bulky 1990s MPEG-2 video streams with modern H.264 video and AAC audio, while collapsing the DVD navigation/subtitle stream into tiny `.mvob` ("Meta-VOB") files (~99% smaller than original `.VOB`s).
- **Single-File `.tdvd` Format:** Packages the entire converted disc into a single `.tdvd` (or `.tinydvd`) file that the player reads in place, with no unpacking, so you can double-click it in a file manager.
- **Closed Captions, Subtitles & Multi-Track Audio:** Preserves original DVD Closed Captions (SDH), multi-language subtitles, and multiple audio tracks with instant toggling during playback.
- **Modern Player Controls:** Navigate menus with keyboard or mouse, jump around the clip timeline (`-10s`/`+10s`, `-60s`/`+60s`, `0%`–`90%`), and step through chapters with On-Screen Display (OSD) feedback.

---

## 1. How to Install It

### What you need first

- **macOS:** [Homebrew](https://brew.sh). Its installer also installs Apple's Xcode Command Line Tools (the C compiler) if you don't have them; you can also install those yourself with `xcode-select --install`. Everything else (CMake, mpv, SDL2, Cairo, Pango, FFmpeg, Python 3.12) is installed through Homebrew for you.
- **Linux:** Debian/Ubuntu, Fedora/RHEL or Arch Linux, and an account that can use `sudo`. The packages are installed for you.

### Install

```bash
./install
```

This installs any missing dependencies (with `scripts/install-deps.sh`), builds TinyDVD, then installs it under `/usr/local`. Writing there needs administrator rights, so it asks for your password, after saying exactly what it will write. You get:

- **On macOS:**
  - **TinyDVD.app** in `/Applications`, which shows up in Launchpad, Spotlight and the Dock with its own icon. Opening it shows your movie library (`~/Movies`).
  - `.tdvd` and `.tinydvd` files open in TinyDVD when you double-click them in Finder, and show the TinyDVD disc icon. (Finder doesn't show each disc's own thumbnail; that would need a Quick Look extension, which TinyDVD doesn't have yet.)
  - The `tinydvd`, `convert-to-tinydvd`, `tinydvd-set-thumbnail` and `tinydvd-thumbnailer` commands in `/usr/local/bin`.
- **On Linux:**
  - The same commands in `/usr/local/bin`, and a TinyDVD entry in your applications menu.
  - The `video/x-tinydvd` file type for `*.tdvd` and `*.tinydvd`, with TinyDVD as its default app, so double-clicking one plays it.
  - The application and file icons, and a thumbnailer, so GNOME Files (Nautilus) shows each disc's embedded thumbnail (see `tinydvd-set-thumbnail` below). If GNOME Files was already open, restart it with `nautilus -q` to see them.
  - Everything goes under `/usr/local` because GNOME runs thumbnailers in a sandbox that can't see your home folder, so a thumbnailer installed in `~/.local` never runs.

#### Encrypted DVDs (optional)

Most commercial DVDs are CSS-encrypted. To rip them straight from a drive you need `dvdbackup` and `libdvdcss`, the standard open-source decryption library that VLC also uses. Whether you may use it depends on where you live, so it's your choice. If they aren't already installed, `./install` asks once (y/N, default no) and installs them from your platform's usual packages. You don't have to compile anything yourself:

- **macOS:** Homebrew's `dvdbackup`, which brings `libdvdcss` with it.
- **Debian / Ubuntu:** `dvdbackup` and `libdvd-pkg`, which downloads and builds `libdvdcss` for you. It's in Debian's `contrib` section or Ubuntu's `multiverse` repository; if that isn't enabled, `./install` tells you how to turn it on.
- **Fedora:** `dvdbackup` and `libdvdcss` from [RPM Fusion](https://rpmfusion.org/Configuration) (its "tainted" repository, which `./install` enables). If RPM Fusion itself isn't set up yet, `./install` points you to its one-time setup.
- **Arch:** `dvdbackup` and `libdvdcss` from the official repositories.

`./install --with-libdvdcss` installs them without asking (and adds them later if you said no the first time); `--without-libdvdcss` skips the question. Without them, TinyDVD still plays `.tdvd` files and imports ripped `VIDEO_TS` folders, unencrypted `.iso` images and unencrypted discs.

Importing a disc from a drive needs up to about one and a half times the disc's size free (at most roughly 13 GB) in your library folder while it works. The ripped copy is deleted as soon as it's converted, and when the import is done only the `.tdvd` remains, usually 20–35% of the disc's size. TinyDVD checks the free space before it starts, and it deletes its working files whether the import finishes, fails or is cancelled.

#### Other options

To install somewhere you can write without `sudo`, use `./install --prefix ~/.local` (on macOS the app then goes in `~/Applications`; choose another folder with `--app-dir`). To remove everything again:

```bash
./install --uninstall
```

(Add `--prefix` if you installed with one.) This removes TinyDVD's commands, its runtime folder, and the app or desktop entries. It checks each command and the app are the ones `./install` put there before deleting them. It leaves your movies, your settings (including the web password) and every package TinyDVD uses, such as FFmpeg, mpv, Python, `dvdbackup` and `libdvdcss`, since other programs may need them.

### Building Without Installing

For working on TinyDVD itself, install the dependencies and build in place:

```bash
./scripts/install-deps.sh
cmake -B build -S .
cmake --build build
```

<details>
<summary>Or install dependencies manually (macOS / Debian / Ubuntu / Fedora / Arch)</summary>

#### macOS (Homebrew)
```bash
brew install cmake pkg-config mpv sdl2 cairo pango zlib ffmpeg python@3.12
```

#### Linux (Debian / Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config \
    dvdbackup \
    libmpv-dev libsdl2-dev libgl1-mesa-dev \
    libcairo2-dev libpango1.0-dev zlib1g-dev \
    ffmpeg python3 python3-venv
```

#### Linux (Fedora / RHEL)
```bash
sudo dnf install -y \
    gcc gcc-c++ cmake pkgconf-pkg-config \
    dvdbackup \
    mpv-libs-devel SDL2-devel mesa-libGL-devel \
    cairo-devel pango-devel zlib-devel \
    ffmpeg python3
```

#### Linux (Arch Linux)
```bash
sudo pacman -S --needed cmake base-devel pkgconf dvdbackup mpv sdl2 cairo pango zlib ffmpeg python
```

#### Optional: encrypted-DVD ripping (`dvdbackup` + `libdvdcss`)
`./scripts/install-deps.sh` (and `./install`) offers to install these for you; see [Encrypted DVDs](#encrypted-dvds-optional) above. To do it by hand, where permitted by local law:

- **macOS:** `brew install dvdbackup` (Homebrew's `libdvdread`, which it depends on, brings `libdvdcss`).
- **Debian / Ubuntu** *(needs `contrib` on Debian or `multiverse` on Ubuntu enabled)*: `libdvd-pkg` downloads and builds `libdvdcss` for you:
  ```bash
  sudo apt-get install -y dvdbackup libdvd-pkg && sudo dpkg-reconfigure libdvd-pkg
  ```
- **Fedora / RHEL** *(needs [RPM Fusion Free](https://rpmfusion.org/Configuration) enabled)*:
  ```bash
  sudo dnf install -y rpmfusion-free-release-tainted && sudo dnf install -y dvdbackup libdvdcss
  ```
- **Arch Linux**:
  ```bash
  sudo pacman -S --needed dvdbackup libdvdcss
  ```

None of this is needed to build TinyDVD, play `.tdvd` files, or import ripped `VIDEO_TS` folders, unencrypted `.iso` images (mounted automatically with `hdiutil` on macOS; mounted first or extracted with `7z` on Linux) or unencrypted discs.
</details>

This builds the player at `./build/tinydvd`, which runs straight from the source tree.

---

## 2. How to Use It in Practice

### Common End-to-End Linux Desktop Use Cases

Two common ways to go from a physical disc to watching it:

#### Recipe 1: "How do I just import a DVD and put it with my other videos?"

1. **Rip the physical disc into `./rips`:**
   Linux optical disc drives are well-known as `/dev/sr0` (often symlinked as `/dev/cdrom` or `/dev/dvd`). The `./rips` folder in this repository is already `.gitignore`d, making it the perfect temporary staging ground:
   ```bash
   mkdir -p ./rips
   dvdbackup -M -i /dev/sr0 -o ./rips
   ```
   *Note: `dvdbackup -M` creates a folder inside `./rips` matching the disc's volume label (e.g. `./rips/DUNE`).*

2. **Convert directly to a `.tdvd` file in `~/Videos`:**
   Convert the ripped folder into a single, self-contained `.tdvd` archive saved directly to your standard `~/Videos` directory:
   ```bash
   ./scripts/convert-to-tinydvd ./rips/DUNE ~/Videos/Dune.tdvd
   ```
   - **Movie details:** By default the converter looks up the director, cast, genres, year and a plot summary. It sends only the title (and year, if the file name has one) to Wikidata and Wikipedia, plus TMDb or OMDb if you give it your own API key. Pass `--skip-enrichment` to turn this off. See [Movie metadata](#movie-metadata-tinydvd_metadatapy).
   - **Automatic Thumbnail:** An optimal menu thumbnail is embedded automatically so GNOME Files / Nautilus displays rich cover art.
   - **Clean up the raw rip:** Once conversion finishes, delete the temporary raw rip to reclaim space:
     ```bash
     rm -rf ./rips/DUNE
     ```

3. **Browse and play in Videos:**
   Open your `~/Videos` folder in your file manager (e.g. GNOME Files / Nautilus):
   ```bash
   xdg-open ~/Videos
   ```
   The `.tdvd` file appears right alongside your other media files with its generated video thumbnail. Simply double-click it to play in `tinydvd` (if you ran `./install`), or run:
   ```bash
   tinydvd ~/Videos/Dune.tdvd
   ```

---

#### Recipe 2: "How do I just serve my tinydvds with the web player?"

To stream your collection to a web browser, tablet, phone or smart TV:

1. **Start the server:**
   ```bash
   # Serve your videos folder to this computer only (http://localhost:8080):
   tinydvd --serve

   # Let other devices on your network connect too:
   tinydvd --serve --bind 0.0.0.0

   # Serve a different folder:
   tinydvd --serve /path/to/movies
   ```
   Like the desktop player, it uses your videos folder by default (`~/Movies` on macOS, `~/Videos` or your desktop's configured Videos folder on Linux), and prints which folder it's serving.
   The web player asks for a password. On first run `tinydvd` makes a random one, saves it in your config folder (`~/.config/tinydvd/web-password` on Linux, `~/Library/Application Support/tinydvd/web-password` on macOS) and prints it when the server starts. To choose your own, put it in that file, or set `TINYDVD_WEB_PASSWORD`, or pass `--web-password-file`.

   The server speaks plain HTTP. That's fine at home; don't expose it to the internet without a TLS reverse proxy in front.

   **Keep it running all the time (Linux with systemd: Ubuntu, Debian, ...).** `./install` adds a `tinydvd-web-service` command:
   ```bash
   tinydvd-web-service enable                      # your videos folder, every network, port 8080
   tinydvd-web-service enable --port 9000 --library /srv/movies --bind 127.0.0.1
   tinydvd-web-service status
   tinydvd-web-service disable
   ```
   It installs a system service, `tinydvd-web`, that starts at boot. The service runs as you, never as root, with your files and your web password. Installing it needs administrator rights, so `enable` and `disable` ask for your password through `sudo` (from a root shell, use `--user <name>`). Unlike `tinydvd --serve`, the service listens on every network by default (`--bind 0.0.0.0`), because a server only this computer can reach isn't much use. Settings you leave out keep their current values. They're saved in `~/.config/tinydvd/web-service.conf`; if you edit that file by hand, run `sudo systemctl restart tinydvd-web` afterwards. Logs: `journalctl -u tinydvd-web`. `enable` prints the password, but the service doesn't write it to the log. `./install --uninstall` stops and removes the service too.

2. **Graceful Tolerance of Non-TinyDVD Files:**
   You do not need to separate your `.tdvd` files into their own isolated folder. TinyDVD expands `~` automatically and scans `~/Videos`, **tolerating and silently ignoring** any non-TinyDVD files (such as `.mp4`, `.mkv`, `.avi`, `.srt`, or downloads) and other subdirectories. Only `.tdvd` archives (and unpacked TinyDVD folders) are added to the library catalog.

3. **Watch in your browser:**
   Open `http://localhost:8080` (or `http://<your-ip>:8080` from another device, if you used `--bind 0.0.0.0`). You can:
   - Import DVDs from the browser, if you started the server with `--allow-web-import`. This is off by default because it lets anyone who has the password run the ripping and conversion tools on any path the server can read. With it on, **"+ Import DVD"** rips, converts and adds a disc (from an optical drive, mounted disc, folder or `.iso`) to your library.
   - Search your catalog live by **actor, director, genre, title, or year**.
   - Click any card to launch the interactive DVD player in full cinema mode.
   - Enjoy multi-session isolation: multiple family members or tabs can watch different movies or menus at the same time without interfering with each other.

---

### Step-by-Step Manual Workflow

#### Step 1: Rip a DVD (If Starting from a Physical Disc)

If you are starting from a commercial encrypted DVD in an optical drive, use `dvdbackup` to rip a decrypted copy to disk first (if you already have a decrypted DVD folder or `.iso`, skip to Step 2). This needs `dvdbackup` and `libdvdcss`; if you didn't install them with TinyDVD, run `./install --with-libdvdcss` (see [Encrypted DVDs](#encrypted-dvds-optional)):

```bash
# macOS (find your DVD drive with `diskutil list`, e.g., /dev/rdisk4):
dvdbackup -M -i /dev/rdisk4 -o ./rips

# Linux (default optical drive /dev/sr0):
dvdbackup -M -i /dev/sr0 -o ./rips
```

#### Step 2: Convert with `convert-to-tinydvd`

Run [`scripts/convert-to-tinydvd`](scripts/convert-to-tinydvd.py) on your ripped DVD folder or `.iso` image to produce a single `.tdvd` file (or an unpacked folder):

```bash
# Produce a single-file .tdvd archive (metadata enrichment is enabled by default):
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ~/Videos/MY_MOVIE.tdvd

# Skip online metadata enrichment if offline or desired:
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ~/Videos/MY_MOVIE.tdvd --skip-enrichment

# Also look the movie up on TMDb or OMDb, with your own API keys:
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ~/Videos/MY_MOVIE.tdvd --tmdb-key "your_tmdb_key" --omdb-key "your_omdb_key"

# Or from an unencrypted ISO image:
./scripts/convert-to-tinydvd /path/to/movie.iso ~/Videos/MY_MOVIE.tdvd

# Or package an already-converted tinydvd directory into a single .tdvd file without re-encoding:
./scripts/convert-to-tinydvd ./tinydvds/MY_MOVIE ~/Videos/MY_MOVIE.tdvd

# Custom H.264 CRF quality and preset:
./scripts/convert-to-tinydvd ./rips/MY_MOVIE ~/Videos/MY_MOVIE.tdvd --crf 18 --preset slow
```

#### Step 3: Play with `tinydvd`

Launch the player with no arguments to open the native **Library Browser & DVD Importer UI** (defaulting to `~/Movies` on macOS, or your desktop's Videos folder on Linux, usually `~/Videos`), point it at a library folder, or pass a `.tdvd` file / converted directory directly to start playback (or map `.tdvd` / `.tinydvd` to `tinydvd` in your file manager):

```bash
# Launch the native Library Browser & DVD Importer UI:
./build/tinydvd
./build/tinydvd ~/Movies

# Or launch playback directly on a movie:
./build/tinydvd ~/Videos/MY_MOVIE.tdvd

# Or start directly in fullscreen:
./build/tinydvd --fullscreen ~/Videos/MY_MOVIE.tdvd

# Unpacked tinydvd directories are also supported:
./build/tinydvd ./tinydvds/MY_MOVIE
```

#### Step 4: Web Player & Library Catalog (`--serve`)

You can stream any `.tdvd` movie or an entire folder of movies to modern web browsers over HTTP/WebSocket with **full interactive DVD menu navigation, seeking, chapter stepping, and subtitle controls**:

```bash
# Play a single movie in direct web cinema mode:
./build/tinydvd --serve ./tinydvds/MY_MOVIE.tdvd

# Or serve an entire collection with catalog browsing and search:
./build/tinydvd --serve ./tinydvds/

# Custom port, reachable from other devices on your network:
./build/tinydvd --serve ./tinydvds/ --port 8080 --bind 0.0.0.0

# Also allow importing DVDs from the browser:
./build/tinydvd --serve ./tinydvds/ --allow-web-import
```

* **Login**: one shared password (see [Recipe 2](#recipe-2-how-do-i-just-serve-my-tinydvds-with-the-web-player)). Logins last 30 days; changing the password signs everyone out.
* **Built-in DVD Importer** (with `--allow-web-import`): Click **"+ Import DVD"** in the library header to rip an optical disc (`dvdbackup`), convert it to `.tdvd`, and refresh the catalog without leaving the browser.
* **Multi-Session Isolation**: Every browser tab and device gets its own independent DVD VM and playback timeline—different family members can watch or browse different movies at the same time without interfering with each other.
* **Streaming straight from your files**: the web player reads the same video and audio files as the desktop player, in place inside each `.tdvd`, with no copies, conversion or cache. Videos stream as HLS in segments of a few seconds, so starting, seeking and skipping chapters download only what's needed, which keeps things quick over slower connections too. Only the audio track you're listening to is downloaded. Safari plays HLS itself; other browsers use the bundled [hls.js](https://github.com/video-dev/hls.js). While a video loads, the player shows a spinner and the disc waits for it.
* **Live Catalog Search**: Search your collection instantly by **actor, director, genre, title, year**, or plot summary.

### Movie Metadata (`tinydvd_metadata.py`)

To add cast, directors, release years and plot summaries to `.tdvd` files you already have:

```bash
# Look up one movie:
./scripts/tinydvd_metadata.py ./tinydvds/MY_MOVIE.tdvd

# Look up every movie in a folder:
./scripts/tinydvd_metadata.py ./tinydvds/

# Also use TMDb and/or OMDb, with your own free API keys:
export TMDB_API_KEY="your_tmdb_key"
export OMDB_API_KEY="your_omdb_key"
./scripts/tinydvd_metadata.py ./tinydvds/Dune.tdvd

# Or manually supply/override metadata fields:
./scripts/tinydvd_metadata.py ./tinydvds/Dune.tdvd --director "Denis Villeneuve" --year 2021 --cast "Timothée Chalamet, Zendaya"
```

Where the details come from, in this order:
- **[TMDb](https://www.themoviedb.org/settings/api)**, if you have set a key. This product uses the TMDB API but is not endorsed or certified by TMDB.
- **[OMDb](https://www.omdbapi.com/apikey.aspx)**, if you have set a key.
- **[Wikidata](https://www.wikidata.org/)**, with no key needed. Its data is CC0. The plot summary comes from the movie's English Wikipedia article (CC BY-SA 4.0); its source and link are saved in the metadata next to it.

Only the title, and the year if known, is sent. When several films share a title, the one whose running time is closest to the disc's main feature wins.

Changing a `.tdvd` file (metadata or thumbnail) writes a new copy next to it and swaps it in, so an interrupted update never damages the original. It needs enough free space for a second copy while it runs.

### Thumbnails & Cover Art (`tinydvd-set-thumbnail`)

New `.tdvd` files get a thumbnail during conversion. It's a frame from the disc's main menu: the menu video of the title set that holds the main feature. If there's no such menu, it's a frame 30 seconds into the main feature, past any fade-in. The disc-level menu video is skipped, because it usually holds the copyright warnings.

You can embed or update thumbnails in existing `.tdvd` archives at any time, without re-encoding:

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

# Use a frame 10 minutes into the main feature instead:
./scripts/tinydvd-set-thumbnail --force --time 00:10:00 ./tinydvds/MY_MOVIE.tdvd
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

`tinydvd --help` prints the same list.

---

## 3. Technical Details (Under the Hood)

### System Architecture

Each DVD title set and menu domain is decomposed into two paired assets:

1. **Compact Meta-VOB (`VIDEO_TS.mvob`, `VTS_xx_0.mvob`, `VTS_xx_1..N.mvob`):**
   - Preserves original 2048-byte Navigation Packs (`NV_PCK`) with PCI button highlight tables and DSI metadata.
   - Collapses **all** MPEG-2 video sectors in each VOBU into a **single** 2048-byte sector containing the 40-byte [`mvob_video_ref_t`](src/mvob_parser.h#L16-L27) pointer descriptor.
   - Preserves Subpicture (`SPU`) 2-bit RLE overlay sectors (`0x20..0x3F`) for interactive menu buttons, Closed Captions (SDH), and subtitles.
   - Reduces `.mvob` container size by **~98.8%** compared to original `.VOB` files (e.g., ~96 MB total across a 7.8 GB dual-layer DVD).

2. **Companion Video and Audio (`VIDEO_TS_VIDEO.mp4`, `VTS_xx_0_VIDEO.mp4`, `VTS_xx_VIDEO.mp4`, plus `…_AUDIO_1.mp4`, `…_AUDIO_2.mp4`, … per audio track):**
   - Encodes video with `libx264` (`-crf 18 -preset slow -tune film`) and every audio track with stereo AAC (`-c:a aac -b:a 192k`), in the disc's own audio stream order, so the DVD's audio choices pick the right track.
   - Stores each as a fragmented MP4 with an index of its fragments at the start ([`scripts/tinydvd_mp4.py`](scripts/tinydvd_mp4.py)). The desktop player (mpv) plays the video file with its audio files as tracks; the web player streams the very same files as HLS. Nothing is stored twice.
   - An edit list hides the encoder's frame reordering delay, so both players see every frame at its exact original time.
   - Preserves still-frame menu timelines (`tpad`) so every menu `VOB_ID` maps 1:1 to its `NV_PCK` presentation timestamps.
   - Uses byte-level `concat:` protocol on multi-file title VOBs (`VTS_xx_1..N.VOB`) to preserve continuous MPEG-PS timestamps across 1 GB VOB splits.
   - Keeps each video's frame size (720x480 NTSC or 720x576 PAL), frame rate (29.97 or 25, read from the disc's IFO files) and aspect ratio (4:3 or 16:9), so menu button and subtitle coordinates match the video 1:1.
   - Keeps film at its exact timing, including soft-telecined NTSC film's 3:2 cadence, and deinterlaces only the frames the disc marks as interlaced (one frame per field, 59.94 or 50 per second), so mixed film and video titles play like they do on a DVD player.

3. **Custom DVD Virtual Machine & Smooth Playback Engine ([`src/dvd_vm_bridge.c`](src/dvd_vm_bridge.c)):**
   - Replaces `libdvdnav` and `libdvdread` with a dedicated DVD VM instruction evaluator (`Groups 0..6`, `GPRM[0..15]`, `SPRM[0..23]`, `RSM` resume slots), a compact `.mvob` indexer, and a clean-room, bounds-checked `.IFO`/`.BUP` & PCI/HLI parser ([`src/dvd_ifo.c`](src/dvd_ifo.c)).
   - Tracks expected presentation timestamps across VOBUs: when consecutive VOBUs have contiguous timestamps, `libmpv` ([`src/mpv_controller.c`](src/mpv_controller.c)) continues playing smoothly without issuing redundant seeks.
   - Composites 2-bit RLE DVD subpictures and menu button highlights over the `libmpv` OpenGL video frame via [`src/spu_compositor.c`](src/spu_compositor.c), including timed DCSQ `STP_DSP` expiration for movie Closed Captions and subtitles.

4. **Single-File `.tdvd` / `.tinydvd` Container ([`src/tdvd_archive.c`](src/tdvd_archive.c)):**
   - Uses a standard uncompressed ZIP (`ZIP_STORED`, with `ZIP64` for files over 4 GB). The ZIP index at the end of the file gives the byte range of every `.IFO`, `.mvob` and `.mp4` inside it.
   - Because the video and audio are already compressed, `ZIP_STORED` adds zero CPU overhead and allows `.IFO`/`.BUP` tables to be loaded directly into memory while `.mvob` navigation sectors (`fseeko`), the desktop player's video and audio (`mpv_stream_cb_add_ro` custom `tdvd://` protocol in [`src/mpv_controller.c`](src/mpv_controller.c) using `pread`) and the web player's HLS segments (byte ranges) are all read **directly in place from the `.tdvd` file without unpacking**.
   - Any `.tdvd` file can also be inspected with standard tools (`unzip -l MY_MOVIE.tdvd`).

### Binary Descriptor (`mvob_video_ref_t`)

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 0x4D564F42 ("MVOB")
    uint16_t version;         // 0x0001
    uint16_t vts_id;          // Video Title Set index (e.g., 1 for VTS_01)
    uint32_t vobu_index;      // VOBU index within cell
    uint64_t pts_nanos;       // Target presentation timestamp in the companion video
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
├── VIDEO_TS_VIDEO.mp4            # Companion VMGM menu H.264 video
├── VIDEO_TS_AUDIO_1.mp4          # ...and its AAC audio
├── VTS_01_0.IFO                  # Title set 1 navigation metadata
├── VTS_01_0.BUP                  # Title set 1 backup
├── VTS_01_0.mvob                 # Compact VTSM menu Meta-VOB (NV_PCK + 1 video ref/VOBU + SPU)
├── VTS_01_0_VIDEO.mp4            # Companion VTSM menu H.264 video
├── VTS_01_0_AUDIO_1.mp4          # ...and its AAC audio
├── VTS_01_1.mvob                 # Compact title Meta-VOB (~99% smaller than original VOB)
├── VTS_01_VIDEO.mp4              # Companion title H.264 video
├── VTS_01_AUDIO_1.mp4            # AAC audio, one file per DVD audio stream
├── VTS_01_AUDIO_2.mp4
└── vts_01_keyframes.txt          # Generated x264 keyframe table
```

### How `dvdbackup` Uses `libdvdcss`

- **macOS (Homebrew):** Optional `dvdbackup` links via Homebrew's `libdvdread` to `libdvdcss.2.dylib` automatically with no extra environment variables.
- **Linux (Debian / Ubuntu):** `dvdbackup` uses the system `libdvdread`, which works without `libdvdcss` on unencrypted discs and loads optional `libdvdcss.so.2` via `dlopen("libdvdcss.so.2")` when `libdvd-pkg` / `libdvdcss` is installed.
- **Debugging CSS Decryption:** Run `DVDCSS_VERBOSE=2 dvdbackup -M -i /dev/rdisk4 -o ./rips` to inspect CSS drive authentication and title key descrambling logs.

### Running Tests

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

This runs the C tests (MVOB parser, subtitle compositor, DVD VM, desktop playback through mpv) and the Python tests (web server and HLS streaming, archive helpers, converter). The playback tests use `tests/fixtures/TestDisc.tdvd`, a half-megabyte synthetic disc (test patterns, a two-button menu, chapters, two audio tracks and a subtitle track). `tests/fixtures/make-test-disc.sh` rebuilds it; that needs `dvdauthor`, which the tests themselves don't.

---

## License & Third-Party Dependencies

TinyDVD's own source code is licensed under the **MIT License** (see [`LICENSE.md`](LICENSE.md)).

When built and linked into binaries (`libtinydvd_core` / `tinydvd`), TinyDVD relies on system-provided third-party libraries (and does **not** link against `libdvdread`, `libdvdnav`, or `libdvdcss`):

- **`libmpv`**: **LGPL-2.1-or-later** core (commonly built as **GPL-2.0-or-later** / **GPL-3.0** by OS distributions and Homebrew when GPL features/FFmpeg are enabled)
- **`cairo`**: **LGPL-2.1** or **MPL-1.1**
- **`pango` / `pangocairo`**: **LGPL-2.0-or-later** / **LGPL-2.1-or-later**
- **`SDL2`** & **`zlib`**: **zlib License**
- **Python 3**: **PSF-2.0 License**

The web player bundles **hls.js** (**Apache-2.0**) in `web/vendor/`, with its license; see `web/vendor/README`.

When compiled against a typical distribution or Homebrew GPL-enabled build of `libmpv`, **any distributed compiled binary packages are combined works subject to the GPL** (requiring corresponding source availability and GPL license notices if you redistribute binaries). If built against an LGPL-only `libmpv` (e.g., `-Dgpl=false`), no GPL-licensed library is linked into `libtinydvd_core`.

External tools invoked only as separate CLI subprocesses (`ffmpeg`, optional `dvdbackup`, and optional `zenity`/`kdialog`) as well as user-installed optional `libdvdcss` (GPL-2.0-or-later) and `libdvdread` used by `dvdbackup` are governed by their respective licenses and are not bundled with TinyDVD.

