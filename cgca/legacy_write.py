#!/usr/bin/env python
"""Tests for `gcac_pack -L` (legacy GCA0/GCA1 output).

Creates a small source tree, writes it in every legacy mode and checks that
gcac.exe, gcac_v09e.exe (if present), gcac_unpack and GcaSDK all read it.

    python legacy_write.py
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PACK = os.path.join(HERE, "build", "gcac_pack.exe")
UNPACK = os.path.join(HERE, "build", "gcac_unpack.exe")
GCAC = os.environ.get("GCAC", os.path.join(ROOT, "gcac.exe"))
GCAC_E = os.path.join(ROOT, "gcac_v09e.exe")
SDK = os.path.join(ROOT, "build", "gcatest.exe")
IS_WIN = os.name == "nt"

MODES = [
    ("a", ["-L"]),
    ("s", ["-L", "-s"]),
    ("n", ["-L", "-n"]),
    ("p", ["-L", "-p", "fixture"]),
    ("k", ["-L", "-k", "key.bin"]),
    ("sp", ["-L", "-s", "-p", "fixture"]),
]

TIMES = {
    "legacy/a.txt": time.mktime((2004, 5, 6, 7, 8, 9, 0, 0, -1)),
    "legacy/sub/b.bin": time.mktime((2005, 6, 7, 8, 9, 10, 0, 0, -1)),
    "legacy/hidden.txt": time.mktime((2006, 7, 8, 9, 10, 11, 0, 0, -1)),
}


def sh(args, cwd=None, timeout=30):
    p = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("latin1"), p.stderr.decode("latin1")


def tree(root):
    out = {}
    for base, dirs, files in os.walk(root):
        for name in dirs + files:
            p = os.path.join(base, name)
            rel = os.path.relpath(p, root).replace("\\", "/")
            st = os.stat(p)
            isdir = os.path.isdir(p)
            out[rel] = (isdir, 0 if isdir else st.st_size,
                        b"" if isdir else open(p, "rb").read(),
                        getattr(st, "st_file_attributes", 0),
                        0 if isdir else st.st_mtime)
    return out


def set_attrs(path, attrs):
    if IS_WIN:
        import ctypes
        ctypes.windll.kernel32.SetFileAttributesW(path, attrs)


def main():
    if not (os.path.exists(PACK) and os.path.exists(UNPACK)):
        print("build the C tools first (build.bat or make)")
        return 2
    tmp = tempfile.mkdtemp(prefix="gcalegacyw")
    src = os.path.join(tmp, "src")
    tree_root = os.path.join(src, "legacy")
    os.makedirs(os.path.join(tree_root, "sub"))
    os.makedirs(os.path.join(tree_root, "empty"))
    open(os.path.join(tree_root, "a.txt"), "wb").write(
        b"legacy writer test\n" * 200)
    open(os.path.join(tree_root, "sub", "b.bin"), "wb").write(
        bytes((i * 7 + 3) % 251 for i in range(2000)))
    open(os.path.join(tree_root, "hidden.txt"), "wb").write(b"hidden")
    for rel, t in TIMES.items():
        os.utime(os.path.join(src, rel), (t, t))
    if IS_WIN:
        set_attrs(os.path.join(tree_root, "hidden.txt"), 0x2)
    keyfile = os.path.join(src, "key.bin")
    open(keyfile, "wb").write(bytes(range(64)))
    spell_key = hashlib.sha1(open(keyfile, "rb").read()).hexdigest()

    ok = bad = 0

    def check(label, cond):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("PASS %s" % label)
        else:
            bad += 1
            print("FAIL %s" % label)

    expected = tree(src)
    expected.pop("key.bin", None)
    # the legacy writer skips empty directories
    expected.pop("legacy/empty", None)

    for tag, opts in MODES:
        arc = os.path.join(tmp, "w_%s.gca" % tag)
        rc, _, err = sh([PACK] + opts + [arc, "legacy"], cwd=src)
        check("write legacy %s" % tag, rc == 0)
        if rc != 0:
            continue
        raw = open(arc, "rb").read()
        check("%s has the legacy magic" % tag,
              raw[:4] == (b"GCA1" if "s" in tag else b"GCA0"))
        check("%s header version" % tag,
              int.from_bytes(raw[4:8], "little") ==
              (6 if tag in ("p", "k", "sp") else 2))

        out = os.path.join(tmp, "x_%s" % tag)
        args = [UNPACK, "e", arc, out]
        if tag in ("p", "sp"):
            args += ["-p", "fixture"]
        elif tag == "k":
            args += ["-k", keyfile]
        rc, _, _ = sh(args)
        check("gcac_unpack reads legacy %s" % tag,
              rc == 0 and tree(out) == expected)

        if os.path.exists(GCAC):
            outg = os.path.join(tmp, "g_%s" % tag)
            args = [GCAC, "e", arc, outg]
            if tag in ("p", "sp"):
                args += ["-p", "fixture"]
            elif tag == "k":
                args += ["-k", keyfile]
            rc, _, _ = sh(args)
            check("gcac.exe reads legacy %s" % tag,
                  rc == 0 and tree(outg) == expected)

        if os.path.exists(GCAC_E) and tag != "k":
            oute = os.path.join(tmp, "e_%s" % tag)
            args = [GCAC_E, "e", arc, oute]
            if tag in ("p", "sp"):
                args += ["-p", "fixture"]
            rc, _, _ = sh(args)
            check("gcac_v09e.exe reads legacy %s" % tag,
                  rc == 0 and tree(oute) == expected)

        if os.path.exists(SDK):
            outs = os.path.join(tmp, "s_%s" % tag)
            spell = ("fixture" if tag in ("p", "sp") else
                     (spell_key if tag == "k" else ""))
            args = [SDK, arc, outs] + ([spell] if spell else [])
            rc, _, _ = sh(args)
            check("GcaSDK reads legacy %s" % tag,
                  rc == 0 and tree(outs) == expected)

    # unsupported legacy combinations
    rc, _, _ = sh([PACK, "-L", "-s", "-n",
                   os.path.join(tmp, "bad1.gca"), "legacy"], cwd=src)
    check("legacy solid+raw is rejected", rc != 0)
    only = os.path.join(src, "onlydirs", "a", "b")
    os.makedirs(only)
    rc, _, _ = sh([PACK, "-L", os.path.join(tmp, "bad2.gca"), "onlydirs"],
                  cwd=src)
    check("legacy empty-directory-only tree is rejected", rc != 0)

    print("\n%d checks OK, %d failed" % (ok, bad))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
