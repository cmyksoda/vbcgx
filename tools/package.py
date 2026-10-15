#!/usr/bin/env python3
"""Package only the development runtime, SD folders and license notices."""
from pathlib import Path
import shutil
import zipfile
root = Path(__file__).resolve().parents[1]
out = root / 'dist/vbcgx'
if out.exists():
    shutil.rmtree(out)
app = out / 'apps/vbcgx'
app.mkdir(parents=True)
for name in ['meta.xml', 'icon.png']:
    shutil.copyfile(root / 'hbc' / name, app / name)
shutil.copyfile(root / 'executables/vbcgx-wii.dol', app / 'boot.dol')
for folder in ['roms', 'saves', 'covers', 'artwork', 'screenshots', 'vboycolor']:
    (out / 'vbcgx' / folder).mkdir(parents=True)
licenses = app / 'licenses'
licenses.mkdir()
for source, target in [
    ('LICENSE', 'GPL-3.txt'), ('NOTICE', 'NOTICE.txt'),
    ('vendor/vboycolor/third_party/beetle-vb-libretro/COPYING', 'beetle-vb-GPL-2.txt'),
]:
    shutil.copyfile(root / source, licenses / target)
# (the licenses of what the Wii build links; vendor/vboycolor/licenses is for VBoy Color's PC and VR builds)
for source in (root / 'licenses').iterdir():
    shutil.copyfile(source, licenses / source.name)
archive = root / 'dist/vbcgx-dev.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as package:
    for path in sorted(out.rglob('*')):
        package.write(path, path.relative_to(out))
print(archive)
