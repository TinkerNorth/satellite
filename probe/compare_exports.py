"""Throwaway probe: what main's published profile copies differ in from the catalog files at the tag.

usage: python -I probe/compare_exports.py <hidmaestro-profiles dir> <catalog files dir>
Semantic JSON comparison (key order and number spelling ignored, array order kept),
beyond each copy's id and notes and the one key or field Satellite edits.
"""
import json
import os
import sys

SONY = ["dualsense", "dualsense-composite", "dualsense-edge", "dualsense-edge-composite",
        "dualshock-4-v2", "dualshock-4-v2-composite"]


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def walk(a, b, path, out):
    if isinstance(a, dict) and isinstance(b, dict):
        for key in a:
            if key not in b:
                out.append(f"  only in catalog: {path}/{key} = {json.dumps(a[key])[:300]}")
            else:
                walk(a[key], b[key], f"{path}/{key}", out)
        for key in b:
            if key not in a:
                out.append(f"  only in export:  {path}/{key} = {json.dumps(b[key])[:300]}")
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"  length differs:  {path} catalog {len(a)} export {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, f"{path}[{i}]", out)
        return
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))
                                       and not isinstance(a, bool) and not isinstance(b, bool)):
        out.append(f"  type differs:    {path} catalog {json.dumps(a)[:200]} export {json.dumps(b)[:200]}")
        return
    if a != b:
        out.append(f"  value differs:   {path} catalog {json.dumps(a)[:200]} export {json.dumps(b)[:200]}")


def main():
    exports, catalog = sys.argv[1], sys.argv[2]
    total = 0
    for cid in SONY + ["switch-pro"]:
        original = load(os.path.join(catalog, cid + ".json"))
        copy = load(os.path.join(exports, "satellite-" + cid + ".json"))
        for key in ("id", "notes"):
            original.pop(key, None)
            copy.pop(key, None)
        if cid == "switch-pro":
            simple = [r for r in copy["inputReports"] if r.get("name") == "simple"]
            print(f"{cid}: copy's simple report send = {simple[0].get('send')!r} (catalog "
                  f"{[r.get('send') for r in original['inputReports'] if r.get('name') == 'simple'][0]!r})")
            for r in copy["inputReports"]:
                if r.get("name") == "simple":
                    r["send"] = "change"
        else:
            print(f"{cid}: catalog has stream = {'stream' in original}, export has stream = {'stream' in copy}")
            original.pop("stream", None)
        out = []
        walk(original, copy, "", out)
        total += len(out)
        print(f"{cid}: {len(out)} difference(s) beyond id, notes and the documented edit; "
              f"catalog keys {len(original)}, export keys {len(copy)}")
        for line in out:
            print(line)
    print(f"TOTAL differences: {total}")


main()
