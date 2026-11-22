#!/usr/bin/env bash
# Builds tests/fixtures/TestDisc.tdvd, the small synthetic DVD the tests play.
# It's made entirely from generated test patterns and silence, so it can be
# committed and used in CI. The tests only need the committed .tdvd; run
# this again only to change the disc.
#
# What's on it (NTSC, 720x480):
#   First play  -> jumps to title 1
#   Root menu   -> two buttons side by side: "Play" (title 1) and
#                  "Chapter 2" (title 1, chapter 2)
#   Title 1     -> 20 s test pattern (silent), chapters at 0, 7 and 14 s,
#                  one subtitle track, returns to the root menu at the end
#
# Needs ffmpeg and dvdauthor (which includes spumux):
#   macOS: brew install ffmpeg dvdauthor
#   Debian/Ubuntu: sudo apt install ffmpeg dvdauthor

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${ROOT_DIR}/tests/fixtures/TestDisc.tdvd"
PYTHON="${PYTHON:-python3}"

for tool in ffmpeg dvdauthor spumux; do
    command -v "$tool" >/dev/null || { echo "Error: $tool is required" >&2; exit 1; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# Transparent 720x480 PNG with boxes drawn on it. Each argument is a
# drawbox spec; spumux wants at most 4 colors including transparent. The
# boxes are drawn on black, which is then made transparent (drawn straight
# onto a transparent canvas, they'd come out transparent too)
overlay_png() {
    local out="$1"; shift
    local filters="format=rgb24"
    for box in "$@"; do filters+=",drawbox=${box}"; done
    ff -f lavfi -i "color=c=black:s=720x480" -frames:v 1 \
       -vf "${filters},format=rgba,colorkey=black:0.01:0" "$out"
}

B1="x=100:y=360:w=200:h=60"
B2="x=420:y=360:w=200:h=60"

# --- Root menu: color bars, with the buttons as a subpicture overlay
ff -f lavfi -i "smptebars=s=720x480:r=30000/1001:d=1" \
   -f lavfi -i "anullsrc=r=48000:cl=stereo" -t 1 \
   -target ntsc-dvd -b:v 2000k -aspect 4:3 menu_bg.mpg
overlay_png menu_normal.png "${B1}:c=white:t=4" "${B2}:c=white:t=4"
overlay_png menu_highlight.png "${B1}:c=yellow:t=fill" "${B2}:c=yellow:t=fill"
overlay_png menu_select.png "${B1}:c=red:t=fill" "${B2}:c=red:t=fill"
cat > menu.xml <<'EOF'
<subpictures>
  <stream>
    <spu start="00:00:00.00" force="yes"
         image="menu_normal.png" highlight="menu_highlight.png" select="menu_select.png">
      <button name="play" x0="100" y0="360" x1="300" y1="420" right="chapter2"/>
      <button name="chapter2" x0="420" y0="360" x1="620" y1="420" left="play"/>
    </spu>
  </stream>
</subpictures>
EOF
spumux -m dvd menu.xml < menu_bg.mpg > menu.mpg 2>/dev/null

# --- Title 1: 30 s test pattern and tone, with a subtitle track
ff -f lavfi -i "testsrc=s=720x480:r=30000/1001:d=20" \
   -f lavfi -i "anullsrc=r=48000:cl=stereo" -t 20 \
   -target ntsc-dvd -b:v 1500k -aspect 4:3 -ac 2 title_av.mpg
overlay_png sub1.png "x=200:y=400:w=320:h=40:c=white:t=fill"
overlay_png sub2.png "x=200:y=400:w=320:h=40:c=white:t=fill" "x=210:y=410:w=300:h=20:c=black:t=fill"
cat > subs.xml <<'EOF'
<subpictures>
  <stream>
    <spu start="00:00:02.00" end="00:00:06.00" image="sub1.png"/>
    <spu start="00:00:09.00" end="00:00:12.00" image="sub2.png"/>
  </stream>
</subpictures>
EOF
spumux -m dvd subs.xml < title_av.mpg > title.mpg 2>/dev/null

# --- Author the disc
cat > dvd.xml <<'EOF'
<dvdauthor dest="disc">
  <vmgm>
    <fpc>jump title 1;</fpc>
    <menus>
      <pgc entry="title">
        <pre>jump titleset 1 menu;</pre>
      </pgc>
    </menus>
  </vmgm>
  <titleset>
    <menus>
      <video format="ntsc" aspect="4:3"/>
      <subpicture lang="en"/>
      <pgc entry="root">
        <button name="play">jump title 1;</button>
        <button name="chapter2">jump title 1 chapter 2;</button>
        <vob file="menu.mpg" pause="inf"/>
      </pgc>
    </menus>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <subpicture lang="en"/>
      <pgc>
        <vob file="title.mpg" chapters="0,0:07,0:14"/>
        <post>call menu;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
EOF
VIDEO_FORMAT=NTSC dvdauthor -x dvd.xml >/dev/null 2>dvdauthor.log || { cat dvdauthor.log >&2; exit 1; }

# --- Convert it the same way users do, at low quality since nobody watches
# it, with made-up movie details in place of an online lookup
"$PYTHON" "${ROOT_DIR}/scripts/convert-to-tinydvd.py" --skip-enrichment \
    --preset veryfast --crf 45 "disc" "converted"
cat > converted/metadata.json <<'JSON'
{
  "title": "TinyDVD Test Disc",
  "year": 2001,
  "director": "Ada Testmaker",
  "cast": "Pat Placeholder, Sam Sample",
  "genres": "Test Pattern",
  "overview": "Color bars, a test pattern, two menu buttons and a subtitle track."
}
JSON
rm -f "$OUT"
"$PYTHON" "${ROOT_DIR}/scripts/convert-to-tinydvd.py" --skip-enrichment "converted" "$OUT"
echo "Wrote ${OUT} ($(du -h "$OUT" | cut -f1))"
