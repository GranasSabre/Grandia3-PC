# HD texture pack builder: upscales dumped textures (texdump\<key>.png) with a super-resolution
# model and writes replacements (textures\<key>.png) at an integer multiple of the original size.
#
#   python -I upscale_textures.py <texdump> <outdir> --model=<file.pth> [--scale=2] [--only=key,key]
#                                  [--skip-existing] [--pad=16] [--list=keys.txt]
#
# The model runs at its own factor (4x); the result is then reduced to --scale with Lanczos, which
# keeps the restored detail but drops most invented high-frequency noise. Edges are padded with
# wrap-around texels so tiling textures stay seamless. Alpha goes through the model as gray.
import os, sys, time
import numpy as np
import torch
from PIL import Image
try:
    import torchvision  # noqa: F401
except ImportError:  # spandrel only needs it for models not used here
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pystub'))
import spandrel

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
src, dst = args[0], args[1]
scale = int(opt.get('scale', '2'))
pad = int(opt.get('pad', '16'))
only = set(k.lower() for k in opt.get('only', '').split(',') if k)
if opt.get('list'):
    only |= set(l.split()[0].lower() for l in open(opt['list']) if l.strip())
os.makedirs(dst, exist_ok=True)

dev = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
desc = spandrel.ModelLoader().load_from_file(opt['model'])
model = desc.model.eval().to(dev)
half = desc.supports_half and dev.type == 'cuda'
bf16 = not half and 'bf16' in opt and desc.supports_bfloat16 and dev.type == 'cuda'
if half:
    model = model.half()
if bf16:
    model = model.bfloat16()
mscale = desc.scale
print(f'model {desc.architecture.name} x{mscale} half={half} bf16={bf16} -> output x{scale}', flush=True)


def run(rgb):  # float32 HxWx3 in 0..1 -> float32 (H*m)x(W*m)x3
    try:
        return run_whole(rgb)
    except torch.cuda.OutOfMemoryError:
        torch.cuda.empty_cache()
    # tiles of 256 texels with 16 texels of (wrapped) context on each side
    h, w = rgb.shape[:2]
    T, C = 256, 16
    ext = np.pad(rgb, ((C, C), (C, C), (0, 0)), mode='wrap')
    out = np.zeros((h * mscale, w * mscale, 3), np.float32)
    for y in range(0, h, T):
        for x in range(0, w, T):
            th, tw = min(T, h - y), min(T, w - x)
            tile = ext[y:y + th + 2 * C, x:x + tw + 2 * C]
            r = run_whole(tile, 0)
            out[y * mscale:(y + th) * mscale, x * mscale:(x + tw) * mscale] = r[C * mscale:(C + th) * mscale, C * mscale:(C + tw) * mscale]
    return out


def run_whole(rgb, pad=pad):
    h, w = rgb.shape[:2]
    p = pad if min(h, w) >= 4 else 0
    x = np.pad(rgb, ((p, p), (p, p), (0, 0)), mode='wrap') if p else rgb
    t = torch.from_numpy(np.ascontiguousarray(x.transpose(2, 0, 1)))[None].to(dev)
    t = t.half() if half else (t.bfloat16() if bf16 else t)
    # size requirements of the architecture (multiple of N)
    req = desc.size_requirements
    mult = max(getattr(req, 'multiple_of', 1) or 1, 1)
    minimum = getattr(req, 'minimum', 0) or 0
    th, tw = t.shape[2], t.shape[3]
    nh = max(minimum, (th + mult - 1) // mult * mult)
    nw = max(minimum, (tw + mult - 1) // mult * mult)
    if nh != th or nw != tw:
        t = torch.nn.functional.pad(t, (0, nw - tw, 0, nh - th), mode='replicate')
    with torch.inference_mode():
        y = model(t)
    y = y[:, :, :th * mscale, :tw * mscale].float().clamp(0, 1)[0].cpu().numpy().transpose(1, 2, 0)
    if p:
        y = y[p * mscale:(p + h) * mscale, p * mscale:(p + w) * mscale]
    return y


def to_img(a):
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8))


files = sorted(f for f in os.listdir(src) if f.lower().endswith('.png'))
t0 = time.time()
done = 0
for f in files:
    key = f[:16].lower()
    if only and key not in only:
        continue
    out = os.path.join(dst, key + '.png')
    if 'skip-existing' in opt and os.path.exists(out):
        continue
    im = Image.open(os.path.join(src, f)).convert('RGBA')
    w, h = im.size
    if w * h < int(opt.get('min-texels', '256')):
        continue
    a = np.asarray(im).astype(np.float32) / 255.0
    rgb = run(a[:, :, :3])
    alpha = a[:, :, 3]
    if alpha.min() < 1.0:
        up_a = run(np.repeat(alpha[:, :, None], 3, axis=2)).mean(axis=2)
    else:
        up_a = np.ones(rgb.shape[:2], np.float32)
    # color and alpha resized separately: colors under transparent texels stay as they were
    # (bilinear filtering in the game blends them in at cutout edges)
    c, al = to_img(rgb), to_img(up_a).convert('L')
    if mscale != scale:
        c = c.resize((w * scale, h * scale), Image.LANCZOS)
        al = al.resize((w * scale, h * scale), Image.LANCZOS)
    Image.merge('RGBA', (*c.split(), al)).save(out)
    done += 1
    if done % 20 == 0:
        print(f'{done} textures, {time.time() - t0:.0f} s', flush=True)
print(f'{done} textures in {time.time() - t0:.0f} s')
