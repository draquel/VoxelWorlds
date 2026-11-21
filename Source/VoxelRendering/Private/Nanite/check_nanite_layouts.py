"""Compare the Nanite layouts VoxelNaniteFormat.h copies with the installed engine's source (runtime Nanite).

The page headers and the fixup chunk are private to the engine (a Developer module's files, an Internal header), so the
encoder copies them; a release that changes one breaks the encoder silently (garbage or a GPU hang). Run this at every
engine upgrade, before building: it lists each copied struct's fields (type and name, in order) on both sides and fails
on any difference.

usage: python check_nanite_layouts.py [--engine "D:/Program Files/Epic Games/UE_5.8"]
"""
import os
import re
import sys

ENGINE = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--engine=")), "D:/Program Files/Epic Games/UE_5.8")
HERE = os.path.dirname(os.path.abspath(__file__))
OURS = os.path.join(HERE, "VoxelNaniteFormat.h")
ENCODE = os.path.join(ENGINE, "Engine/Source/Developer/NaniteBuilder/Private/Encode")
FIXUP = os.path.join(ENGINE, "Engine/Source/Runtime/Engine/Internal/Nanite/NaniteFixupChunk.h")

# (engine file, engine struct, our struct)
PAIRS = [
    (os.path.join(ENCODE, "NaniteEncode.cpp"), "FPageGPUHeader", "FPageGPUHeader"),
    (os.path.join(ENCODE, "NaniteEncode.cpp"), "FPageDiskHeader", "FPageDiskHeader"),
    (os.path.join(ENCODE, "NaniteEncode.cpp"), "FClusterDiskHeader", "FClusterDiskHeader"),
    (os.path.join(ENCODE, "NaniteEncodeShared.h"), "FPageSections", "FPageSections"),
    (os.path.join(ENCODE, "NaniteEncodeShared.h"), "FPackedUVHeader", "FPackedUVHeader"),
    (FIXUP, "FHeader", "FFixupHeader"),
    (FIXUP, "FGroupFixup", "FFixupGroup"),
    (FIXUP, "FPartFixup", "FFixupPart"),
    (FIXUP, "FParentFixup", "FFixupParent"),
]
# Types written differently on purpose: the engine's on the left, ours on the right
EQUIVALENT_TYPES = {("FUintVector2", "uint32[2]"), ("FPageRangeKey", "uint32")}
FIELD = re.compile(r"^\s*([A-Za-z_][\w:<>]*)\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=[^;]*)?;")


def struct_body(text, name):
    match = re.search(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{", text)
    if not match:
        return None
    depth, start = 1, match.end()
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
    return None


def fields(body):
    out, depth = [], 0
    for line in body.splitlines():
        code = line.split("//")[0]
        if depth == 0:
            match = FIELD.match(code)
            if match and "(" not in code:
                kind = match.group(1) + (("[" + match.group(3).strip("[] ") + "]") if match.group(3) else "")
                out.append((kind, match.group(2)))
        depth += code.count("{") - code.count("}")
    return out


def same(theirs, ours):
    if len(theirs) != len(ours):
        return False
    for (tk, tn), (ok, on) in zip(theirs, ours):
        if tn != on or (tk != ok and (tk, ok) not in EQUIVALENT_TYPES and (tk.split("[")[0], ok) not in EQUIVALENT_TYPES):
            return False
    return True


ours_text = open(OURS, encoding="utf-8").read()
failures = 0
for path, engine_name, our_name in PAIRS:
    if not os.path.isfile(path):
        print("MISSING  %s (%s)" % (engine_name, path))
        failures += 1
        continue
    theirs = struct_body(open(path, encoding="utf-8", errors="replace").read(), engine_name)
    ours = struct_body(ours_text, our_name)
    if theirs is None or ours is None:
        print("NOTFOUND %s / %s" % (engine_name, our_name))
        failures += 1
        continue
    tf, of = fields(theirs), fields(ours)
    ok = same(tf, of)
    failures += 0 if ok else 1
    print("%s %-18s %d fields" % ("ok      " if ok else "DIFFERS ", our_name, len(tf)))
    if not ok:
        print("   engine: %s" % tf)
        print("   ours:   %s" % of)
print("%d of %d layouts differ" % (failures, len(PAIRS)))
sys.exit(1 if failures else 0)
