#!/usr/bin/env python3
"""Package only the development runtime, SD folders and license notices."""
from pathlib import Path
import shutil
import zipfile
root = Path(__file__).resolve().parents[1]
out = root / 'dist/VBoy-Color-GX'
if out.exists():
    shutil.rmtree(out)
app = out / 'apps/vboycolorgx'
app.mkdir(parents=True)
for name in ['meta.xml', 'icon.png']:
    shutil.copyfile(root / 'hbc' / name, app / name)
shutil.copyfile(root / 'executables/vboycolorgx-wii.dol', app / 'boot.dol')
for folder in ['roms', 'saves', 'cheats', 'covers', 'artwork', 'screenshots', 'vboycolor']:
    (out / 'vboycolorgx' / folder).mkdir(parents=True)
licenses = app / 'licenses'
licenses.mkdir()
for source, target in [
    ('LICENSE', 'GPL-3.txt'), ('NOTICE', 'NOTICE.txt'),
    ('vendor/vboycolor/THIRD-PARTY-NOTICES.txt', 'THIRD-PARTY-NOTICES.txt'),
    ('vendor/vboycolor/third_party/beetle-vb-libretro/COPYING', 'beetle-vb-COPYING.txt'),
]:
    shutil.copyfile(root / source, licenses / target)
for source in (root / 'vendor/vboycolor/licenses').iterdir():
    if source.is_file():
        shutil.copyfile(source, licenses / source.name)
archive = root / 'dist/VBoy-Color-GX-dev.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as package:
    for path in sorted(out.rglob('*')):
        package.write(path, path.relative_to(out))
print(archive)
