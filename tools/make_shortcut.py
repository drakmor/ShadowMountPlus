#!/usr/bin/env python3
"""Make a ShadowMountPlus shortcut folder: a home screen icon that starts an installed app with arguments.

Any app:
    python3 tools/make_shortcut.py --title-id SHRT00001 --name "My game" --icon cover.png \\
        --target PPSA12345 --arg --some-option --arg value

A PS2 game in PS5SX2 (title ID, cover, target and arguments filled in from the serial):
    python3 tools/make_shortcut.py --ps2-serial SLUS-20946 --name "Grand Theft Auto: San Andreas" \\
        --image "/data/PCSX2/games/Grand Theft Auto - San Andreas (USA) (v1.03).iso"

The folder holds sce_sys/param.json, sce_sys/icon0.png and smp-launch.json. Copy it into a scan path
(for example /data/homebrew/<title ID>): ShadowMountPlus adds its launcher as eboot.bin and installs it.
Needs Pillow for the icon.
"""
import argparse
import io
import json
import re
import sys
import urllib.request
from pathlib import Path

from PIL import Image

PS2_COVER_URL = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/{serial}.jpg"
PS5SX2_TITLE_ID = "PPSA99203"
ICON_SIZE = 512


def param_json(title_id: str, name: str) -> dict:
    return {
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "attribute": 0,
        "attribute2": 0,
        "attribute3": 0,
        "conceptId": title_id[4:],
        "contentBadgeType": 1,
        "contentId": f"UP9000-{title_id}_00-SMPSHORTCUT00000",
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


def load_image(source: str) -> Image.Image:
    if re.match(r"https?://", source):
        with urllib.request.urlopen(source, timeout=30) as r:
            return Image.open(io.BytesIO(r.read())).convert("RGB")
    return Image.open(source).convert("RGB")


def make_icon(source: str, out: Path):
    picture = load_image(source)
    picture.thumbnail((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    icon = Image.new("RGB", (ICON_SIZE, ICON_SIZE), (0, 0, 0))
    icon.paste(picture, ((ICON_SIZE - picture.width) // 2, (ICON_SIZE - picture.height) // 2))
    icon.save(out, "PNG")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="the name on the home screen")
    ap.add_argument("--title-id", help="the shortcut's own title ID, SHRT plus five digits")
    ap.add_argument("--icon", help="icon picture: a file or an http(s) URL")
    ap.add_argument("--target", help="title ID of the installed app to start")
    ap.add_argument("--arg", action="append", default=[], help="an argument for the target (repeat)")
    ap.add_argument("--adapter", choices=["ps5sx2"], help="extra launch handling for the target")
    ap.add_argument("--ps2-serial", help="a PS2 game in PS5SX2: its serial, like SLUS-20946")
    ap.add_argument("--image", help="with --ps2-serial: the disc image's path on the console")
    ap.add_argument("--out", type=Path, default=Path("shortcuts"), help="folder to create the shortcut in")
    a = ap.parse_args()

    if a.ps2_serial:
        m = re.fullmatch(r"[A-Z]{4}-(\d{5})", a.ps2_serial)
        if not m:
            sys.exit("--ps2-serial must look like SLUS-20946")
        # The same rule as the PS5SX2 adapter, so a wrong path fails here and not on the console.
        if not a.image or not re.fullmatch(r"/data/PCSX2(?:/games)?/[^/]+\.(?:iso|chd|cso|zso)", a.image,
                                           re.IGNORECASE):
            sys.exit("--image must be an .iso, .chd, .cso or .zso file directly in /data/PCSX2/games or /data/PCSX2")
        a.title_id = a.title_id or "SHRT" + m.group(1)
        a.icon = a.icon or PS2_COVER_URL.format(serial=a.ps2_serial)
        a.target, a.adapter, a.arg = PS5SX2_TITLE_ID, "ps5sx2", ["--boot", a.image]

    if not a.title_id or not re.fullmatch(r"SHRT\d{5}", a.title_id):
        sys.exit("--title-id must be SHRT plus five digits")
    if not a.target or not re.fullmatch(r"[A-Z]{4}\d{5}", a.target):
        sys.exit("--target must be a title ID like PPSA12345")
    if not a.icon:
        sys.exit("--icon is required")

    folder = a.out / a.title_id
    (folder / "sce_sys").mkdir(parents=True, exist_ok=True)
    (folder / "sce_sys" / "param.json").write_text(json.dumps(param_json(a.title_id, a.name), indent=2))
    make_icon(a.icon, folder / "sce_sys" / "icon0.png")
    launch = {"title_id": a.target, "args": a.arg}
    if a.adapter:
        launch["adapter"] = a.adapter
    (folder / "smp-launch.json").write_text(json.dumps(launch, indent=2) + "\n")
    print(f"{folder}: copy it to /data/homebrew/{a.title_id}")


if __name__ == "__main__":
    main()
