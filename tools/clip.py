"""Records a video clip of the port with its own sound, after WoodyRE's: an --autoplay run (simulated time, the bots play)
with RAMPAGE_CAPTURE (main.c), which appends every other 1/60 s step's picture (30 frames a second) and every step's
mixed sound (48 kHz) to raw files, muxed into an H.264/AAC MP4. Not the game's: footage (a Reddit post, a README).
Usage: python tools/clip.py NAME START SECONDS [--size WxH] [--keep] -- ARGS...
  START = simulated seconds since the run's start (as the autoplay log's t=), SECONDS = the clip's length.
  ARGS = RampageRE-dev.exe's own (the city, block, --monster, --players, --play, --night ...); --autoplay, --shot, --mute
  off, the profile and the window size are added. Writes build/clips/NAME.mp4 (the raw files in build/clips/NAME/, removed
  unless --keep). Needs RampageRE-dev.exe (build.bat dev), build/disc/RAMPAGE.VOL and imageio-ffmpeg (pip install imageio-ffmpeg).
Examples:
  python tools/clip.py chicago 30 20 -- Chicago 1 --monster George --ap-seed 1
  python tools/clip.py fourp 25 20 -- Chicago 1 --players 4 --bots 4 --ap-seed 2
  python tools/clip.py blimp 40 25 -- NewYork 10 --monster Ralph --cheat ... --ap-seed 1"""
import os, re, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FPS, RATE = 30, 48000

def record(d, start, secs, size, args):
    for f in ("video.rgb", "audio.raw"):
        if os.path.exists(os.path.join(d, f)): os.remove(os.path.join(d, f))
    prof = os.path.join(d, "clip.prof")
    if os.path.exists(prof): os.remove(prof)
    env = dict(os.environ, RAMPAGE_CAPTURE=f"{start} {secs} {d}")
    cmd = [os.path.join(root, "RampageRE-dev.exe"), os.path.join(root, "build", "disc", "RAMPAGE.VOL"), *args, "--autoplay",
           "--ap-minutes", str(start / 60 + secs / 60 + 1), "--ap-log", os.path.join(d, "ap.log"), "--profile", prof,
           "--window", size, "--shot", os.path.join(d, "end.png")]
    out = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True, errors="replace").stdout
    open(os.path.join(d, "console.txt"), "w", encoding="utf-8").write(out)
    m = re.search(r"capture: (\d+)x(\d+)", out)
    if not m or "capture: " not in out.split(m.group(0), 1)[1]:
        sys.exit(f"the run ended before the clip's end (see {d}\\console.txt)")
    return int(m.group(1)), int(m.group(2))

def mp4(d, out, w, h):
    import imageio_ffmpeg
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-y", "-loglevel", "error",
                    "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}", "-r", str(FPS), "-i", os.path.join(d, "video.rgb"),
                    "-f", "s16le", "-ar", str(RATE), "-ac", "2", "-i", os.path.join(d, "audio.raw"),
                    "-vf", "vflip", "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p",
                    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out], check=True)
    print(out, os.path.getsize(out) // 1024, "KB")

if __name__ == "__main__":
    if "--" not in sys.argv or len(sys.argv[:sys.argv.index("--")]) < 4: sys.exit(__doc__)
    own, args = sys.argv[1:sys.argv.index("--")], sys.argv[sys.argv.index("--") + 1:]
    name, start, secs = own[0], float(own[1]), float(own[2])
    size = own[own.index("--size") + 1] if "--size" in own else "1920x1080"
    base = os.path.join(root, "build", "clips"); d = os.path.join(base, name); os.makedirs(d, exist_ok=True)
    w, h = record(d, start, secs, size, args)
    mp4(d, os.path.join(base, name + ".mp4"), w, h)
    if "--keep" not in own:
        for f in ("video.rgb", "audio.raw"): os.remove(os.path.join(d, f))
