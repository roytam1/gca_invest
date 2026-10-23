#!/usr/bin/env python
"""Folder/CLI parity test for the C tools against gcac.exe.

Creates a tree with nested directories, empty directories, hidden and
read-only files, a large stored file and several extensions, then checks:

  * gcac_pack and gcac.exe produce byte identical archives (a/s/n, -p, -k)
  * gcac_unpack restores the tree, including empty directories, attributes
    and timestamps
  * gcac.exe can extract the archives written by gcac_pack
  * -o / -d, list-only mode and .gca name handling
  * a corrupted archive does not crash or hang

    python parity.py [path-to-gcac.exe]

Set GCAX_NO_GCAC=1 to run only the checks that do not need gcac.exe.
"""
import ctypes
import hashlib
import os
import random
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PACK = os.path.join(HERE, "build", "gcac_pack.exe")
UNPACK = os.path.join(HERE, "build", "gcac_unpack.exe")
GCAC = os.environ.get("GCAC", os.path.join(ROOT, "gcac.exe"))
HAVE_GCAC = os.path.exists(GCAC) and not os.environ.get("GCAX_NO_GCAC")
SDK = os.path.join(ROOT, "build", "gcatest.exe")
HAVE_SDK = os.path.exists(SDK)
IS_WIN = os.name == "nt"


def sh(args, cwd=None, timeout=30):
    p = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("latin1"), p.stderr.decode("latin1")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def meta(root):
    """rel -> (is_dir, size, sha, attributes, mtime_ns)."""
    out = {}
    for base, dirs, files in os.walk(root):
        for name in dirs + files:
            p = os.path.join(base, name)
            rel = os.path.relpath(p, root)
            st = os.stat(p)
            isdir = os.path.isdir(p)
            out[rel] = (
                isdir,
                0 if isdir else st.st_size,
                "" if isdir else sha(p),
                getattr(st, "st_file_attributes", 0),
                0 if isdir else st.st_mtime_ns,   # directory times are not restored
            )
    return out


def set_attrs(path, attrs):
    if IS_WIN:
        ctypes.windll.kernel32.SetFileAttributesW(path, attrs)


def main():
    if not (os.path.exists(PACK) and os.path.exists(UNPACK)):
        print("build the tools first (build.bat or make)")
        return 2

    tmp = tempfile.mkdtemp(prefix="gcaparity")
    src = os.path.join(tmp, "src")
    tree = os.path.join(src, "tree")
    os.makedirs(os.path.join(tree, "empty"))
    os.makedirs(os.path.join(tree, "sub", "deep"))
    os.makedirs(os.path.join(tree, "sub", "empty2"))
    open(os.path.join(tree, "a.txt"), "wb").write(
        (b"the quick brown fox jumps over the lazy dog\n") * 500)
    rnd = random.Random(7)
    open(os.path.join(tree, "z.bin"), "wb").write(
        bytes(rnd.getrandbits(8) for _ in range(300000)))
    open(os.path.join(tree, "sub", "deep", "c.txt"), "wb").write(b"deep" * 900)
    open(os.path.join(tree, "hidden.txt"), "wb").write(b"hidden")
    open(os.path.join(tree, "ro.txt"), "wb").write(b"read only")
    open(os.path.join(tree, "noext"), "wb").write(b"no extension")
    open(os.path.join(tree, "zzz.aaa"), "wb").write(b"aaa" * 100)
    open(os.path.join(tree, "aaa.zzz"), "wb").write(b"zzz" * 100)
    open(os.path.join(tree, "empty.bin"), "wb").write(b"")
    old = 981173106  # 2001-02-03 04:05:06 UTC
    for p in (os.path.join(tree, "a.txt"),
              os.path.join(tree, "sub", "deep", "c.txt"),
              os.path.join(tree, "empty")):
        os.utime(p, (old, old))
    if IS_WIN:
        set_attrs(os.path.join(tree, "hidden.txt"), 0x2)
        set_attrs(os.path.join(tree, "ro.txt"), 0x1)
        set_attrs(os.path.join(tree, "empty"), 0x10 | 0x2)

    ok = bad = 0

    def check(label, cond):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("PASS %s" % label)
        else:
            bad += 1
            print("FAIL %s" % label)

    keyfile = os.path.join(tmp, "key.bin")
    open(keyfile, "wb").write(bytes(range(64)))

    modes = [
        ("a", "a", []),
        ("s", "s", ["-s"]),
        ("n", "n", ["-n"]),
    ]
    for tag, cmd, opts in modes:
        arc_g = os.path.join(tmp, "g_%s.gca" % tag)
        arc_o = os.path.join(tmp, "o_%s.gca" % tag)
        rc, _, err = sh([PACK] + opts + [arc_o, tree])
        check("pack %s" % tag, rc == 0)
        if HAVE_GCAC:
            rc, _, err = sh([GCAC, cmd, arc_g, tree])
            check("gcac.exe packs %s" % tag, rc == 0)
            check("%s archive is byte identical" % tag,
                  open(arc_g, "rb").read() == open(arc_o, "rb").read())

            out_g = os.path.join(tmp, "xg_%s" % tag)
            out_o = os.path.join(tmp, "xo_%s" % tag)
            rc, _, _ = sh([GCAC, "e", arc_o, out_g])
            check("gcac.exe extracts our %s archive" % tag,
                  rc == 0 and meta(os.path.join(out_g, "tree")) ==
                  meta(tree))
            rc, _, _ = sh([UNPACK, "e", arc_g, out_o])
            check("we extract gcac.exe's %s archive" % tag,
                  rc == 0 and meta(os.path.join(out_o, "tree")) ==
                  meta(tree))
            if HAVE_SDK:
                out_s = os.path.join(tmp, "xs_%s" % tag)
                rc, _, _ = sh([SDK, arc_o, out_s])
                check("GcaSDK extracts our %s archive" % tag,
                      rc == 0 and meta(os.path.join(out_s, "tree")) ==
                      meta(tree))
            if IS_WIN:
                em = meta(os.path.join(out_o, "tree"))
                check("%s attributes survive" % tag,
                      em["hidden.txt"][3] & 0x2 and
                      em["ro.txt"][3] & 0x1 and
                      em["empty"][0])
                check("%s timestamps survive" % tag,
                      em["a.txt"][4] // 10**9 == old)

    if HAVE_GCAC:
        for tag, extra, popts in (
                ("p", ["-p", "secret"], ["-p", "secret"]),
                ("k", ["-k", keyfile], ["-k", keyfile])):
            arc_g = os.path.join(tmp, "g_%s.gca" % tag)
            arc_o = os.path.join(tmp, "o_%s.gca" % tag)
            rc, _, _ = sh([GCAC, "a", arc_g] + extra + [tree])
            check("gcac.exe packs %s" % tag, rc == 0)
            rc, _, _ = sh([PACK] + popts + [arc_o, tree])
            check("pack %s" % tag, rc == 0)
            check("%s archive is byte identical" % tag,
                  open(arc_g, "rb").read() == open(arc_o, "rb").read())
            if HAVE_SDK:
                spell = ("secret" if tag == "p" else
                         hashlib.sha1(open(keyfile, "rb").read()).hexdigest())
                out_s = os.path.join(tmp, "xs_%s" % tag)
                rc, _, _ = sh([SDK, arc_o, out_s, spell])
                check("GcaSDK extracts encrypted %s archive" % tag,
                      rc == 0 and meta(os.path.join(out_s, "tree")) ==
                      meta(tree))

        # -o order list, with and without -d
        lst = os.path.join(tmp, "list.txt")
        open(lst, "w").write("tree\\z.bin\ntree\\a.txt\n"
                             "tree\\sub\\deep\\c.txt\n")
        for tag, dflag in (("o", []), ("d", ["-d"])):
            arc_g = os.path.join(tmp, "l%s_g.gca" % tag)
            arc_o = os.path.join(tmp, "l%s_o.gca" % tag)
            rc, _, _ = sh([GCAC, "a", arc_g] + dflag + ["-o", lst], cwd=src)
            check("gcac.exe packs -o%s" % (" -d" if dflag else ""), rc == 0)
            rc, _, _ = sh([PACK] + dflag + ["-o", lst, arc_o], cwd=src)
            check("pack -o%s" % (" -d" if dflag else ""), rc == 0)
            check("-o%s archive is byte identical" % (" -d" if dflag else ""),
                  open(arc_g, "rb").read() == open(arc_o, "rb").read())

    # list-only mode must not extract anything
    arc = os.path.join(tmp, "o_a.gca")
    empty_cwd = os.path.join(tmp, "emptycwd")
    os.makedirs(empty_cwd)
    rc, out, _ = sh([UNPACK, "l", arc], cwd=empty_cwd)
    check("list-only succeeds", rc == 0 and "filename" in out)
    check("list-only does not extract", os.listdir(empty_cwd) == [])

    # archive extension handling
    named = os.path.join(tmp, "named")
    shutil.copytree(tree, named)
    rc, _, _ = sh([PACK, named])
    check("source-only pack writes .gca",
          rc == 0 and os.path.exists(named + ".gca"))

    # corrupt input must not crash or hang
    data = bytearray(open(arc, "rb").read())
    data[len(data) // 2] ^= 0xFF
    badarc = os.path.join(tmp, "bad.gca")
    open(badarc, "wb").write(data)
    try:
        rc, _, _ = sh([UNPACK, "e", badarc, os.path.join(tmp, "badout")],
                      timeout=10)
        check("corrupt archive returns cleanly", rc in (0, 1, 2))
    except subprocess.TimeoutExpired:
        check("corrupt archive returns cleanly", False)

    print("\n%d checks OK, %d failed" % (ok, bad))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
