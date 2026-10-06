#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate Harbor's Android launcher resources from dist/harbor/harbor-icon.svg.

Needs rsvg-convert and ImageMagick (magick) on PATH for the raster files.
"""
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(__file__).resolve().parents[2]
svg = root / 'dist/harbor/harbor-icon.svg'
res = root / 'src/android/app/src/main/res'
paths = [(n.attrib['d'], n.attrib['fill'])
         for n in ET.parse(svg).getroot().findall('{http://www.w3.org/2000/svg}path')]
NOTE = '<!-- Generated from dist/harbor/harbor-icon.svg by scripts/harbor/generate-android-icons.py. -->'

# Adaptive icon layers: 512 unit artwork scaled into the 66dp safe zone of the 108dp canvas.
for mono in (False, True):
    lines = ['<?xml version="1.0" encoding="utf-8"?>', NOTE,
             '<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="108dp" '
             'android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">',
             '  <group android:translateX="21" android:translateY="21" '
             'android:scaleX="0.12890625" android:scaleY="0.12890625">']
    for data, fill in paths:
        if mono and fill in ('#16304b', '#193251', '#fff4cf'):
            continue
        lines.append(f'    <path android:pathData="{data}" android:fillColor="{"#FFFFFF" if mono else fill}" />')
    name = 'ic_launcher_monochrome.xml' if mono else 'ic_launcher_foreground.xml'
    (res / 'drawable' / name).write_text('\n'.join(lines + ['  </group>', '</vector>', '']))

(res / 'drawable/ic_launcher_background.xml').write_text(f'''<?xml version="1.0" encoding="utf-8"?>
{NOTE}
<shape xmlns:android="http://schemas.android.com/apk/res/android" android:shape="rectangle">
  <gradient android:angle="315" android:startColor="#213B61" android:endColor="#09172C" />
</shape>
''')

# Pre-API-26 launcher PNGs: the full rounded-square artwork.
for density, px in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    subprocess.run(['rsvg-convert', '-w', str(px), '-h', str(px), '-o',
                    str(res / f'mipmap-{density}/ic_launcher.png'), str(svg)], check=True)

# Android TV banner (320x180): the icon centred on the same navy gradient.
tmp = res / 'drawable-xhdpi/.banner-icon.png'
subprocess.run(['rsvg-convert', '-w', '150', '-h', '150', '-o', str(tmp), str(svg)], check=True)
subprocess.run(['magick', '-size', '320x180', 'gradient:#213b61-#09172c', str(tmp),
                '-gravity', 'center', '-composite', str(res / 'drawable-xhdpi/tv_banner.png')], check=True)
tmp.unlink()
