"""Contact sheet of a MOVIE\\GRMnn.MOV, to see which scene a movie shows.
usage: mov_frames.py <GRMnn.MOV> <out.png> [seconds between frames, default 5] [ffmpeg.exe]
The video packs (MPEG-PS runs, see mov_audio.py) go to ffmpeg; one frame every few seconds, 6 per row,
in time order. ffmpeg reports damaged slices at some pack joins, the frames are still clear enough."""
import os, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mov_audio

path, out = sys.argv[1], sys.argv[2]
every = float(sys.argv[3]) if len(sys.argv) > 3 else 5
ff = sys.argv[4] if len(sys.argv) > 4 else shutil.which('ffmpeg')
d = open(path, 'rb').read()
v = bytearray()
p = 0x800
while p < len(d):
    if d[p:p + 4] == b'\x00\x00\x01\xba':
        e = mov_audio.pack_end(d, p)
        v += d[p:e]
        p = (e + 0x7FF) & ~0x7FF
    else:
        p += 0x800
mpg = out + '.mpg'
open(mpg, 'wb').write(v)
try:
    subprocess.run([ff, '-loglevel', 'quiet', '-y', '-i', mpg, '-vf', 'fps=1/%g,scale=240:-1,tile=6x8' % every,
                    '-frames:v', '1', out], check=True)
finally:
    os.remove(mpg)
