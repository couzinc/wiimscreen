#!/usr/bin/env python3
"""Animated demo of the real app for the project page: site/demo.webp (animated WebP) and site/demo-poster.jpg.

The simulator (pio run -e sim) runs a scripted tour and saves a frame every 100 ms; this script draws a finger
ripple where each tap lands and writes an animated WebP (libwebp merges the frames that do not change).

    pio run -e sim && python3 tools/demo/make_demo.py [--out site] [--width 960] [--quality 75]

Needs Pillow with WebP support. Coordinates are those of the 1280x720 screen (English UI, simulator data).
"""
import argparse, glob, os, shutil, subprocess, sys, tempfile

from PIL import Image, ImageDraw

FRAME_MS = 100
WARMUP_MS = 1500  # presets and covers load before the first frame

# Tour: ("wait", ms) lets time pass (one frame per FRAME_MS), ("tap", x, y) is a simulator tap.
# Keyboard (QWERTY): "miles" = m(861,541) i(975,330) l(1089,435) e(405,330) s(291,435); Search key (1089,647)
TOUR = [
    ("wait", 2200),                                 # now playing, progress moving
    ("tap", 784, 585), ("wait", 3000),              # an album tile: pressed, waiting ring, the album plays
    ("tap", 50, 261), ("wait", 1600),               # Library
    ("tap", 557, 56), ("wait", 1600),               # filter: Qobuz
    ("tap", 50, 458), ("wait", 700),                # Search
    ("tap", 861, 541), ("wait", 200), ("tap", 975, 330), ("wait", 200), ("tap", 1089, 435), ("wait", 200),
    ("tap", 405, 330), ("wait", 200), ("tap", 291, 435), ("wait", 500),
    ("tap", 1089, 647), ("wait", 5200),             # Search: waiting, then the results
    ("tap", 600, 300), ("wait", 2600),              # a result plays
    ("tap", 1216, 348), ("wait", 3500),             # full-screen button: ambient view
    ("tap", 300, 250), ("wait", 1500),              # a touch: back to the controls
]


def build_script(path):
    """Simulator script: a shot every FRAME_MS, taps in between. Returns the taps as (frame, x, y)."""
    lines, taps, frame = [f"wait {WARMUP_MS}"], [], 0
    for step in TOUR:
        if step[0] == "wait":
            for _ in range(max(1, step[1] // FRAME_MS)):
                lines += [f"wait {FRAME_MS}", f"shot f{frame:04d}"]
                frame += 1
        else:
            lines.append(f"tap {step[1]} {step[2]}")
            taps.append((frame, step[1], step[2]))
    lines.append("quit")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return taps


def ripple(img, x, y, age):
    """Finger mark: a soft disc that grows and fades over 3 frames."""
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    r = 26 + age * 9
    alpha = max(0, 150 - age * 38)
    d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, alpha // 3), outline=(255, 255, 255, alpha), width=3)
    return Image.alpha_composite(img.convert("RGBA"), over).convert("RGB")


def record(a, work):
    script = os.path.join(work, "demo.txt")
    taps = build_script(script)
    frames_dir = os.path.join(work, "frames")
    subprocess.run([a.sim, "--headless", "--quiet", "--reset", "--state", os.path.join(work, "state"),
                    "--out", frames_dir, "--script", script], check=True, stdout=subprocess.DEVNULL)
    ppms = sorted(glob.glob(os.path.join(frames_dir, "f*.ppm")))
    if not ppms:
        sys.exit("no frames")
    h = a.width * 720 // 1280
    frames = []
    for i, p in enumerate(ppms):
        img = Image.open(p).convert("RGB")
        for f, x, y in taps:
            if -1 <= i - f < 3:  # from the frame before the tap: the finger lands on the screen it touches
                img = ripple(img, x, y, i - f + 1)
        frames.append(img.resize((a.width, h), Image.LANCZOS))
    os.makedirs(a.out, exist_ok=True)
    webp = os.path.join(a.out, "demo.webp")
    frames[0].save(webp, save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0, quality=a.quality,
                   method=4)
    frames[0].save(os.path.join(a.out, "demo-poster.jpg"), quality=85)
    print(f"{len(ppms)} frames ({len(ppms) * FRAME_MS / 1000:.1f} s), {os.path.getsize(webp) // 1024} KB -> {webp}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=".pio/build/sim/program")
    ap.add_argument("--out", default="site")
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--quality", type=int, default=75)
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="wiimscreen-demo-")  # about 600 MB of frames: always removed
    try:
        record(a, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
