#!/usr/bin/env python
"""Legacy GCA0/GCA1 fixtures written by gcac_v09e.exe.

The fixtures live in ../fixtures/gcacv09e_*.gca and cover the two plain
variants, the non-compressed mode and encrypted normal/solid archives.

    python legacy.py [path-to-gcac.exe]

Set GCAX_NO_GCAC=1 to test only the C tools.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UNPACK = os.path.join(HERE, "build", "gcac_unpack.exe")
FIXTURES = os.path.join(ROOT, "fixtures")
GCAC = os.environ.get("GCAC", os.path.join(ROOT, "gcac.exe"))
HAVE_GCAC = os.path.exists(GCAC) and not os.environ.get("GCAX_NO_GCAC")
IS_WIN = os.name == "nt"

CASES = [
    ("gcacv09e_plain.gca", None),
    ("gcacv09e_solid.gca", None),
    ("gcacv09e_none.gca", None),
    ("gcacv09e_encrypted.gca", "fixture"),
    ("gcacv09e_encrypted_solid.gca", "fixture"),
]

TIMES = {
    "legacy/a.txt": time.mktime((2001, 2, 3, 4, 5, 6, 0, 0, -1)),
    "legacy/sub/b.bin": time.mktime((2002, 3, 4, 5, 6, 7, 0, 0, -1)),
    "legacy/hidden.txt": time.mktime((2003, 4, 5, 6, 7, 8, 0, 0, -1)),
}


def sh(args, timeout=30):
    p = subprocess.run(args, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("latin1"), p.stderr.decode("latin1")


def tree(root):
    """rel -> (is_dir, size, bytes, attributes, mtime)."""
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


def main():
    if not os.path.exists(UNPACK):
        print("build the C tools first (build.bat or make)")
        return 2
    tmp = tempfile.mkdtemp(prefix="gcalegacy")
    ok = bad = 0

    def check(label, cond):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("PASS %s" % label)
        else:
            bad += 1
            print("FAIL %s" % label)

    reference = None
    for name, password in CASES:
        src = os.path.join(FIXTURES, name)
        out = os.path.join(tmp, name[:-4])
        args = [UNPACK, "e", src, out]
        if password:
            args += ["-p", password]
        rc, _, err = sh(args)
        check("extract %s" % name, rc == 0)
        if rc != 0:
            continue
        t = tree(out)
        check("%s has the expected tree" % name,
              set(t) == {"legacy", "legacy/sub", "legacy/a.txt",
                         "legacy/hidden.txt", "legacy/sub/b.bin"})
        if reference is None:
            reference = t
            check("plain files have content", t["legacy/a.txt"][2] and
                  t["legacy/sub/b.bin"][1] == 1000)
        else:
            check("%s matches the plain files" % name,
                  all(t[k][0:3] == reference[k][0:3] for k in reference))
        if IS_WIN:
            check("%s restores attributes" % name,
                  t["legacy/a.txt"][3] & 0x1 and
                  t["legacy/hidden.txt"][3] & 0x2 and
                  t["legacy/sub/b.bin"][3] & 0x20)
            check("%s restores timestamps" % name,
                  all(abs(t[k][4] - v) < 2 for k, v in TIMES.items()))
        if HAVE_GCAC:
            outg = os.path.join(tmp, name[:-4] + "_gcac")
            args = [GCAC, "e", src, outg]
            if password:
                args += ["-p", password]
            rc, _, _ = sh(args)
            check("gcac.exe agrees on %s" % name,
                  rc == 0 and tree(outg) == t)

    # key-file encryption (the key is SHA-1 hashed like -p)
    src = os.path.join(FIXTURES, "gcacv09e_encrypted_key.gca")
    keyfile = os.path.join(FIXTURES, "gcacv09e_key.bin")
    out = os.path.join(tmp, "keyfile")
    rc, _, _ = sh([UNPACK, "e", src, out, "-k", keyfile])
    check("extract gcacv09e_encrypted_key.gca", rc == 0 and
          reference is not None and tree(out) == reference)
    if HAVE_GCAC:
        outg = os.path.join(tmp, "keyfile_gcac")
        rc, _, _ = sh([GCAC, "e", src, outg, "-k", keyfile])
        check("gcac.exe agrees on gcacv09e_encrypted_key.gca",
              rc == 0 and tree(outg) == reference)

    # encrypted archives must not be silently parsed without a password
    rc, _, _ = sh([UNPACK, "l", os.path.join(FIXTURES,
                                             "gcacv09e_encrypted.gca")])
    check("encrypted legacy archive needs a password", rc != 0)

    print("\n%d checks OK, %d failed" % (ok, bad))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
