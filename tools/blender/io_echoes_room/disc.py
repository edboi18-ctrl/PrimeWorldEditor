"""Read-only access to the user's Echoes disc image for the add-on: worlds, rooms, resources.

Built on echoes_disc.py (vendored from the port), which reads .iso, .gcm and .ciso images.
"""
from __future__ import annotations

import os
import re

from . import echoes_disc as ed


class DiscError(Exception):
    pass


class Disc:
    def __init__(self, path: str):
        if not path or not os.path.isfile(path):
            raise DiscError(f"No disc image at {path!r}.")
        if path.lower().endswith((".rvz", ".wia", ".gcz", ".wbfs")):
            raise DiscError("This reader takes .iso, .gcm or .ciso; convert the image to ISO in Dolphin "
                            "(right-click the game > Convert File...).")
        self.path = path
        self.iso = ed.Iso(path)
        self.paks = ed.load_paks(self.iso)
        self.owner, self.ids = {}, set()
        for pak in self.paks:
            for rid, (rtype, *_rest) in pak.res.items():
                self.owner.setdefault((rtype, rid), pak)
                self.ids.add(rid)
        self._worlds = None

    def close(self):
        self.iso.f.close()

    def get(self, rtype: str, rid: int) -> bytes:
        pak = self.owner.get((rtype, rid))
        if pak is None:
            raise DiscError(f"{rtype} {rid:08X} is in no PAK on this disc.")
        return pak.get(rid)

    def pak_of(self, rtype: str, rid: int) -> str:
        pak = self.owner.get((rtype, rid))
        return pak.path.rsplit("/", 1)[-1] if pak else ""

    def string(self, strg_id: int):
        try:
            for text in ed.strg_strings(self.get("STRG", strg_id)):
                text = re.sub(r"&[^;&]*;", "", text).strip()
                if text:
                    return text.split("\n")[0].strip()
        except Exception:
            return None
        return None

    def worlds(self) -> list:
        """[{name, mlvl, pak, areas: [{index, name, iname, mrea}]}] for every single-player world."""
        if self._worlds is None:
            out = []
            for pak in self.paks:
                for resource_name, (rtype, mlvl_id) in pak.names.items():
                    if rtype != "MLVL":
                        continue
                    try:
                        parsed = ed.parse_mlvl(pak.get(mlvl_id))
                    except Exception:
                        continue
                    areas = [{"index": a["index"], "name": self.string(a["strg"]) or a["iname"] or f"area {a['index']}",
                              "iname": a["iname"], "mrea": a["mrea"]} for a in parsed["areas"]]
                    out.append({"name": self.string(parsed["strg"]) or resource_name, "mlvl": mlvl_id,
                                "pak": pak.path.rsplit("/", 1)[-1], "areas": areas})
            self._worlds = out
        return self._worlds
