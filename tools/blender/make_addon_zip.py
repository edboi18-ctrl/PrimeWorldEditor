"""Zip the Echoes Room Tools add-on for Blender's Preferences > Add-ons > Install from Disk.

    python tools/blender/make_addon_zip.py [output.zip]      (Windows: py -3 tools\\blender\\make_addon_zip.py)

Writes io_echoes_room.zip (beside this script unless a path is given), the add-on's folder at its top.
"""
import pathlib
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ADDON = HERE / "io_echoes_room"


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "io_echoes_room.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(ADDON.rglob("*.py")):
            if "__pycache__" not in path.parts:
                z.write(path, path.relative_to(HERE).as_posix())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
