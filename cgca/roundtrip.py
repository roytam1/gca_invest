#!/usr/bin/env python
"""Round trip test for the C tools against the original gcac.exe.

Creates a corpus, packs it with gcac_pack, extracts it with gcac.exe
(or with gcac_unpack) and compares the result byte for byte.

    python roundtrip.py [path-to-gcac.exe]

Without an argument the script looks for gcac.exe next to the repository
root.  Set GCAX_NO_GCAC=1 to test the C tools alone.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PACK = os.path.join(HERE, "build", "gcac_pack.exe")
UNPACK = os.path.join(HERE, "build", "gcac_unpack.exe")
GCAC = os.environ.get("GCAC", os.path.join(ROOT, "gcac.exe"))

CORPUS = {
    "rnd.bin":       bytes((i * 7 + 3) % 251 for i in range(5000)),
    "text.txt":      (b"the quick brown fox jumps over the lazy dog\n") * 400,
    "empty.bin":     b"",
    "big.bin":       bytes((i // 97) % 256 for i in range(2_500_000)),
    "sub/inner.bin": b"inner" * 3000,
}


def sh(args, cwd=None):
    p = subprocess.run(args, cwd=cwd, capture_output=True)
    return p.returncode, p.stdout.decode("latin1"), p.stderr.decode("latin1")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def tree(root):
    out = {}
    for base, dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, root)] = open(p, "rb").read()
    return out


def main():
    tmp = tempfile.mkdtemp(prefix="gcaround")
    src = os.path.join(tmp, "src")
    os.makedirs(src)
    for name, data in CORPUS.items():
        p = os.path.join(src, name.replace("/", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    names = list(CORPUS)
    paths = [os.path.join(src, n.replace("/", os.sep)) for n in names]

    have_gcac = os.path.exists(GCAC) and not os.environ.get("GCAX_NO_GCAC")
    ok = bad = 0

    def check(label, cond):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("PASS %s" % label)
        else:
            bad += 1
            print("FAIL %s" % label)

    for mode, args in (("plain", []), ("solid", ["-s"]), ("nocomp", ["-n"]),
                       ("dirc", ["-C"])):
        arc = os.path.join(tmp, "p_%s.gca" % mode)
        rc, so, se = sh([PACK] + args + [arc] + paths)
        if rc != 0:
            check("pack %s" % mode, False)
            continue
        # a file argument is stored under its base name
        expect = dict((os.path.basename(n), d) for n, d in CORPUS.items())
        # gcac.exe must be able to read what we wrote
        if have_gcac:
            out = os.path.join(tmp, "x_%s" % mode)
            rc, so, se = sh([GCAC, "e", arc, out])
            check("gcac.exe extracts our %s archive" % mode,
                  rc == 0 and tree(out) == expect)
        # and we must be able to read what gcac.exe wrote
        if have_gcac and mode in ("plain", "solid", "nocomp"):
            arc2 = os.path.join(tmp, "g_%s.gca" % mode)
            cmd = {"plain": "a", "solid": "s", "nocomp": "n"}[mode]
            # the list has to live in the corpus directory: gcac.exe
            # resolves its entries relative to the list file, and it
            # expects Windows style path separators
            lst = os.path.join(src, "list.txt")
            open(lst, "w").write("\n".join(n.replace("/", os.sep)
                                           for n in CORPUS))
            rc, so, se = sh([GCAC, cmd, arc2, "-o", lst], cwd=src)
            os.remove(lst)
            out = os.path.join(tmp, "y_%s" % mode)
            if rc == 0:
                rc, so, se = sh([UNPACK, arc2, "-o", out])
            check("we extract gcac.exe's %s archive" % mode,
                  rc == 0 and tree(out) == expect)

    # the legacy containers, if the fixtures are present
    for legacy in ("gcasdk_v09d.gca", "gcatxt.gca"):
        path = os.path.join(ROOT, "fixtures", legacy)
        if not os.path.exists(path):
            continue
        out = os.path.join(tmp, "leg_" + legacy)
        rc, so, se = sh([UNPACK, path, "-o", out])
        check("we extract the legacy %s" % legacy,
              rc == 0 and len(tree(out)) > 0)

    print("\n%d checks OK, %d failed" % (ok, bad))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
