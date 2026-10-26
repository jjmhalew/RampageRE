"""Makes res/vita/, the PS Vita build's LiveArea images (build_vita.sh puts them into the .vpk's sce_sys/), from
res/switch_icon.jpg, the port's own 256 x 256 art (tools/mkicon.py says what is in it): icon0.png 128 x 128 (the
bubble), livearea/contents/bg.png 840 x 500 (the art centred on its own darkest colour) and startup.png 280 x 158 (the
start gate: the art, scaled to its height, on the same colour). The system takes only 8-bit palette PNGs there, so each
is quantized to 256 colours. template.xml beside them is written by hand.
Usage: python tools/mkvita.py   (needs Pillow)"""
import os
from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = Image.open(os.path.join(root, 'res', 'switch_icon.jpg')).convert('RGB')
if src.size != (256, 256):
    src = src.resize((256, 256), Image.LANCZOS)
back = min(src.getcolors(1 << 24), key=lambda c: sum(c[1]))[1]   # the art's darkest colour: the margins'


def on_back(w, h, side):
    im = Image.new('RGB', (w, h), back)
    art = src.resize((side, side), Image.LANCZOS)
    im.paste(art, ((w - side) // 2, (h - side) // 2))
    return im


def save(im, *path):
    out = os.path.join(root, 'res', 'vita', *path)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    im.quantize(256, method=Image.MEDIANCUT, dither=Image.NONE).save(out, optimize=True)
    print(out)


save(src.resize((128, 128), Image.LANCZOS), 'icon0.png')
save(on_back(840, 500, 256), 'livearea', 'contents', 'bg.png')
save(on_back(280, 158, 158), 'livearea', 'contents', 'startup.png')
