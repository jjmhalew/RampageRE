"""Makes res/rampagere.ico, the icon of RampageRE.exe and of the standalone launcher (res/rampagere.rc), from
res/switch_icon.jpg, the port's own 256 x 256 art (the Switch build's icon since commit f96b0cb: the words RAMPAGE RE
in plain lettering over a skyline of flat rectangles at dusk, made for this project; no art of the game is in it).
Sizes 256, 128, 64, 48, 32, 24 and 16, each scaled down from the JPEG (256 is kept as PNG inside the .ico).
Usage: python tools/mkicon.py [preview.png]   (needs Pillow; after WoodyRE's tools/mkicon.py)"""
import os, sys
from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = Image.open(os.path.join(root, 'res', 'switch_icon.jpg')).convert('RGBA')
if src.size != (256, 256):
    src = src.resize((256, 256), Image.LANCZOS)
sizes = [256, 128, 64, 48, 32, 24, 16]
imgs = [src if s == 256 else src.resize((s, s), Image.LANCZOS) for s in sizes]
out = os.path.join(root, 'res', 'rampagere.ico')
imgs[0].save(out, format='ICO', sizes=[(s, s) for s in sizes], append_images=imgs[1:])
if len(sys.argv) > 1:                                       # preview: the sizes side by side
    sheet = Image.new('RGBA', (sum(sizes) + 10 * len(sizes), 256), (255, 255, 255, 255))
    x = 0
    for im in imgs:
        sheet.paste(im, (x, 0), im)
        x += im.width + 10
    sheet.save(sys.argv[1])
print(out)
