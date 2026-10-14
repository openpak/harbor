#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate Harbor's Android icon resources from dist/harbor/harbor-icon.svg.

Needs rsvg-convert and ImageMagick (magick) on PATH. The artwork has gradients and clip paths, so
every layer is a raster render rather than an Android vector.
"""
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
svg = root / 'dist/harbor/harbor-icon.svg'
res = root / 'src/android/app/src/main/res'
NAVY = ('#213b61', '#09172c')
NOTE = '<!-- Generated from dist/harbor/harbor-icon.svg by scripts/harbor/generate-android-icons.py. -->'


def render(px, out):
    subprocess.run(['rsvg-convert', '-w', str(px), '-h', str(px), '-o', str(out), str(svg)], check=True)


def on_navy(size, art_px, out, rounded=0):
    """The artwork centred on the navy gradient (optionally a rounded square on transparency)."""
    tmp = out.with_name('.art.png')
    render(art_px, tmp)
    args = ['magick', '-size', size, f'gradient:{NAVY[0]}-{NAVY[1]}', str(tmp), '-gravity', 'center', '-composite']
    if rounded:
        w = int(size.split('x')[0])
        args += ['(', '-size', size, 'xc:none', '-fill', 'white', '-draw',
                 f'roundrectangle 0,0 {w - 1},{w - 1} {rounded},{rounded}', ')',
                 '-compose', 'DstIn', '-composite']
    subprocess.run(args + [str(out)], check=True)
    tmp.unlink()


# Adaptive icon: the artwork in the 66dp safe zone of the 108dp canvas (xxxhdpi: 432 px).
render(288, res / 'drawable/.fg.png')
subprocess.run(['magick', '-size', '432x432', 'xc:none', str(res / 'drawable/.fg.png'), '-gravity', 'center',
                '-composite', str(res / 'drawable-xxxhdpi/ic_launcher_foreground.png')], check=True)
(res / 'drawable/.fg.png').unlink()
(res / 'drawable/ic_launcher_foreground.xml').unlink(missing_ok=True)
(res / 'drawable/ic_launcher_monochrome.xml').unlink(missing_ok=True)
for name in ('ic_launcher.xml', 'ic_launcher_round.xml'):
    (res / 'mipmap-anydpi-v26' / name).write_text(f'''<?xml version="1.0" encoding="utf-8"?>
{NOTE}
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
''')
(res / 'drawable/ic_launcher_background.xml').write_text(f'''<?xml version="1.0" encoding="utf-8"?>
{NOTE}
<shape xmlns:android="http://schemas.android.com/apk/res/android" android:shape="rectangle">
  <gradient android:angle="315" android:startColor="{NAVY[0].upper()}" android:endColor="{NAVY[1].upper()}" />
</shape>
''')

# Pre-API-26 launcher PNGs: a rounded navy square carrying the artwork.
for density, px in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    on_navy(f'{px}x{px}', int(px * 0.9), res / f'mipmap-{density}/ic_launcher.png', rounded=px // 5)

# Android TV banner (320x180).
on_navy('320x180', 150, res / 'drawable-xhdpi/tv_banner.png')

# In-app logos: About screen (transparent), splash icon (padded for the splash mask), file-provider
# root (same file as About) and the 256 px default game icon.
render(512, res / 'drawable/ic_yuzu.png')
tmp = res / 'drawable/.splash.png'
render(340, tmp)
subprocess.run(['magick', '-size', '512x512', 'xc:none', str(tmp), '-gravity', 'center', '-composite',
                str(res / 'drawable/ic_yuzu_splash.png')], check=True)
tmp.unlink()
on_navy('256x256', 230, res / 'drawable/.def.png')
subprocess.run(['magick', str(res / 'drawable/.def.png'), '-quality', '90', str(res / 'drawable/default_icon.jpg')], check=True)
(res / 'drawable/.def.png').unlink()
