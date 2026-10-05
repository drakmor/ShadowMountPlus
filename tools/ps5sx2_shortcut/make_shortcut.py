"""Make a PS5SX2 home screen shortcut for one PS2 disc image.

    python3 make_shortcut.py --serial SLUS-20946 --name "Grand Theft Auto: San Andreas" \
        --image "/data/PCSX2/games/Grand Theft Auto - San Andreas (USA) (v1.03).iso"

Needs Pillow, and the launcher built first (make NATIVE=...).

The shortcut is an app folder, PCSX<serial digits> (SLUS-20946 -> PCSX20946), holding the launcher
(dist/eboot.bin, see Makefile), the console's libc.prx, a param.json with the game's name, an icon
made from the xlenore/ps2-covers cover and ps5sx2-boot.txt naming the image. Copy the folder to
/data/homebrew/<id> (or another scan path): ShadowMountPlus finds it and puts it on the home screen.
"""
import argparse
import io
import json
import re
import shutil
import sys
import urllib.request
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
COVER_URL = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/{serial}.jpg"
ICON_SIZE = 512


def title_id_for(serial: str) -> str:
    m = re.fullmatch(r"[A-Z]{4}-(\d{5})", serial)
    if not m:
        sys.exit(f"serial {serial!r} is not like SLUS-20946")
    return "PCSX" + m.group(1)


def param_json(title_id: str, name: str, serial: str) -> dict:
    return {
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "attribute": 0,
        "attribute2": 0,
        "attribute3": 0,
        "conceptId": title_id[4:],
        "contentBadgeType": 1,
        "contentId": f"UP9000-{title_id}_00-PS5SX2{serial.replace('-', '')}",
        "contentVersion": "01.000.000",
        "downloadDataSize": 0,
        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": name}},
        "masterVersion": "01.00",
        "requiredSystemSoftwareVersion": "0x0000000000000000",
        "sdkVersion": "0x0000000000000000",
        "titleId": title_id,
        "versionFileUri": "",
    }


def make_icon(serial: str, out: Path):
    with urllib.request.urlopen(COVER_URL.format(serial=serial), timeout=30) as r:
        cover = Image.open(io.BytesIO(r.read())).convert("RGB")
    cover.thumbnail((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    icon = Image.new("RGB", (ICON_SIZE, ICON_SIZE), (0, 0, 0))
    icon.paste(cover, ((ICON_SIZE - cover.width) // 2, (ICON_SIZE - cover.height) // 2))
    icon.save(out, "PNG")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--image", required=True, help="the disc image's path on the console")
    a = ap.parse_args()

    title_id = title_id_for(a.serial)
    app = HERE / "dist" / "shortcuts" / title_id
    shutil.rmtree(app, ignore_errors=True)
    (app / "sce_sys").mkdir(parents=True)
    (app / "sce_module").mkdir()
    shutil.copy(HERE / "dist" / "eboot.bin", app / "eboot.bin")
    shutil.copy(HERE / "dist" / "libc.prx", app / "sce_module" / "libc.prx")
    (app / "sce_sys" / "param.json").write_text(json.dumps(param_json(title_id, a.name, a.serial), indent=2))
    make_icon(a.serial, app / "sce_sys" / "icon0.png")
    (app / "ps5sx2-boot.txt").write_text(a.image + "\n", newline="\n")
    print(f"{app} ({a.name}): copy it to /data/homebrew/{title_id}")


if __name__ == "__main__":
    main()
