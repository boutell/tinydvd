# Working on TinyDVD

Notes for anyone, human or AI agent, changing this project. Read this before you start; the README covers how to *use* TinyDVD.

## The point of the project

**TinyDVD exists to keep whole DVDs, menus and all, in as little disk space as possible without losing video quality.** Everything else serves that. Don't trade it away:

- **Store everything once.** No second copy of the video for any player, no transcoding at play time, no caches of converted video. The desktop player and the web player both read the same files, in place, inside the `.tdvd` package. If a feature seems to need a copy, find another way, or raise it with the maintainer first.
- **Don't lower quality to save space, and don't spend space without a reason.** The encode settings (x264 CRF 18, `slow`, `film`; AAC 192k) are the quality bar. If you change anything that affects what's stored (encoder settings, container, layout, extra files), measure the package size before and after on a real feature-length disc, and say what it cost or saved. Small overheads that buy real capability can be fine; that's the maintainer's call, so report the number.
- **Keep the whole DVD.** Menus, button highlights, chapters, every audio track, subtitles and closed captions all survive conversion, and play where the disc means them to. Dropping a track or a menu to make something easier isn't acceptable.

## How it fits together

- **Converter**: `scripts/convert-to-tinydvd.py`. It reads a DVD (folder, ISO, or a rip made by the importer) and writes a package:
  - The IFO/BUP files, copied as they are.
  - `.mvob` "Meta-VOB" files: the DVD's navigation packs and subpicture (menu button, subtitle) sectors, with each VOBU's video replaced by one small reference sector. About 1% of the original VOBs.
  - Per video (`VIDEO_TS`, `VTS_NN_0` for a title set's menus, `VTS_NN` for its titles): `NAME_VIDEO.mp4` (H.264 only) and `NAME_AUDIO_1.mp4`, `NAME_AUDIO_2.mp4`, ... (AAC, one per DVD audio stream). See `scripts/tinydvd_mp4.py`.
- **Package**: a `.tdvd` is an uncompressed ZIP (`ZIP_STORED`), so every member is a plain byte range of the file. Everything reads members in place: `src/tdvd_archive.c` (C), `scripts/tinydvd_archive.py` (Python). An unpacked folder with the same files works too.
- **DVD VM**: `src/dvd_vm_bridge.c` and friends, a clean-room DVD virtual machine (no libdvdnav/libdvdread). It runs the disc's navigation commands from the IFOs and `.mvob`s and tells a player what to load, where to seek, and which audio track to use, through `src/player_adapter.c`.
- **Desktop player**: `scripts/tinydvd_app.py` with `scripts/tinydvd_desktop_ui.py` (SDL window, library UI), driving `libtinydvd_core` (`src/core_bridge.c`). Video plays through libmpv (`src/mpv_controller.c`), reading package members via a `tdvd://offset:size:path` stream protocol; a video's audio files become mpv's audio tracks 1..N.
- **Web player**: `scripts/tinydvd_web_server.py` (one DVD VM per browser session, over a WebSocket) and `web/index.html`. Every video streams as HLS. The playlists come from each MP4's own index (`sidx`), and their segments are byte ranges served straight out of the package. Safari plays HLS natively; other browsers use the bundled hls.js (`web/vendor/`). Alternate audio renditions mean a browser downloads only the track being heard.
- **Library and import**: `scripts/tinydvd_library.py` (folder scanning, the import job), `scripts/tinydvd_metadata.py` (movie details), the thumbnailer scripts, and `install` (build, Python venv, desktop integration, macOS app).

## Things that must stay true

- **Frame-accurate timing, and sound in sync.** The VM addresses video by the DVD's own timestamps, and still-frame menus depend on landing on exactly the right frame. Video and audio are separate files, kept in sync by their timestamps alone. Fragmented MP4 can't hide the encoder's start-up delays the way a plain MP4 does (a frame or two of video reordering, 21 ms of AAC priming), so `split_video` measures them and adds edit lists; mpv and hls.js honour them. The test disc's title has a flash and beeps at the same moment for checking this (`tests/test_av_sync.py`). If you touch encoding or muxing, keep those tests passing and try a real disc.
- **Audio track N is DVD audio stream N.** The VM picks tracks by DVD stream number. The converter maps audio by MPEG stream ID in the order the IFO declares, and fills a declared-but-missing stream with silence so numbering never shifts. This holds on both encode paths: normal titles, and menus/short videos encoded segment by segment.
- **Packages stay uncompressed**, so both players can read members as byte ranges.
- **Never overwrite or lose a user's file.** Archives are written to hidden temporary files and renamed into place; a TinyDVD is never replaced unless the user confirms it, in a question that names the file (the import dialogs ask before starting; the converter takes `--replace` or a yes in a terminal), and otherwise the new one is saved beside it; uninstall removes only what `./install` put there.
- **The web server only serves what it should.** Everything needs the login, cross-site requests are refused, and media names are checked against a strict pattern before they touch the filesystem.

## Building and testing

```bash
cmake -B build -S . && cmake --build build -j
ctest --test-dir build --output-on-failure
```

- Tests must pass on macOS and Ubuntu (GitHub Actions runs both; see `.github/workflows/`). CI builds in Release mode; the C tests keep `assert()` on anyway.
- **Tests must never need proprietary content**, such as a commercial DVD rip; we can't put that in a repository or CI. They use `tests/fixtures/TestDisc.tdvd`, a synthetic disc made from test patterns and silence by `tests/fixtures/make-test-disc.sh` (needs `dvdauthor`). If a feature needs something the test disc doesn't have, extend the script and commit the regenerated disc, keeping it small (it's about half a megabyte).
- Test what actually happens, not just that nothing crashed: positions, frames, tracks, bytes.
- For the web player, also try a slow link: some problems only show up when the network isn't a LAN. A small throttling TCP proxy in front of `tinydvd --serve` is enough.

## Conventions

- **Git**: commit only when asked, and never push without the maintainer's permission. No AI attribution in commits or pull requests (no `Co-Authored-By` lines or "generated with" footers).
- **Before the first public release**, the package format and APIs can change freely: nobody else has packages yet, so don't add backwards compatibility for earlier development formats. After release, any format change needs a migration path.
- Python 3.8+ for scripts (Homebrew's 3.12 on macOS). `install` must run under macOS's bash 3.2.
- Comments explain *why*, in plain language. Don't restate the code.
- Keep the README's user-facing claims (sizes, features, commands) true when you change behaviour.

## Things learned the hard way

- **Chrome can't stream a fragmented MP4 as a plain `<video src>`.** It walks the whole file one fragment at a time, one request each, before playing. That's why the web player uses HLS (hls.js) instead.
- **A plain MP4's index is proportional to its length** (10+ MB for a feature film) and must be downloaded before the first frame: fine on a LAN, too slow anywhere else.
- **The converter's temporary MKV stores no decode timestamps**, so ffmpeg makes some up when splitting it into MP4s. With the edit list, the result still matches the original frame for frame. ffmpeg's `negative_cts_offsets` and the `setts` filter don't help here.
- **The disc's timeline starts at the first video frame**, but ffmpeg starts a file's timeline at its earliest stream, often audio a few ms earlier. Both encode paths start the video at exactly 0 and move the audio back by the same amount; otherwise the sound ends up a few ms late. The test disc's short title only exercises the segment-by-segment path, so `tests/test_av_sync.py` also runs a generated MPEG through the full-length title encode.
- **A fragmented MP4 drops AAC priming** (the 1024 samples an MKV or plain MP4 marks to skip) and plays it as 21 ms of silence, putting the sound late, unless the audio file gets its own edit list.
- **To measure sync in a browser**, compare against a plain MP4 of the same content in the same harness (Web Audio onset vs `requestVideoFrameCallback`, median of several runs): absolute numbers include the output latency.
- **Seeking shows the frame whose display interval contains the time**, in both mpv and Chrome. To check where a browser seek landed, use `requestVideoFrameCallback` while paused; after `play()` it reports the *next* frame.
- **mpv's `audio-add` only works after a file has loaded.** Set `options/audio-files` before `loadfile` instead.
- **Homebrew's libarchive is keg-only**; mpv's pkg-config file needs it, so `CMakeLists.txt` adds its pkgconfig folder. On Linux, GL 1.5+/2.0 functions need `GL_GLEXT_PROTOTYPES` and `GL/glext.h`.
- **GitHub's Ubuntu runners set `XDG_CONFIG_HOME`**; tests that fake a home folder must clear it.
