# cgca

A portable C implementation of the GCA archive format used by `gcac.exe`
(GCA GCompressionArchiver 0.9k, SYN 2000/2001).

The directory contains two command-line tools:

* `gcac_pack` creates archives.
* `gcac_unpack` lists or extracts archives.

Both tools read the modern GCAX/GCA3 container and the legacy GCA0/GCA1
containers. `gcac_pack` writes modern archives by default and legacy archives
with `-L`.

## Build

Windows, Visual C++ 6:

```bat
build.bat
```

This writes:

```text
build\gcac_pack.exe
build\gcac_unpack.exe
```

POSIX or another C compiler:

```sh
make
```

The Makefile writes `gcac_pack` and `gcac_unpack` into this directory.

## Usage

```text
gcac_pack   [options] <archive.gca> <file or folder>...
gcac_pack   a|s|n [options] <archive.gca> <file or folder>
gcac_unpack [options] <archive.gca> [extract folder]
gcac_unpack e|l [options] <archive.gca> [extract folder]
```

Examples:

```bat
gcac_pack out.gca file.txt folder\
gcac_pack a out.gca folder
gcac_pack -s solid.gca folder
gcac_pack -n stored.gca folder
gcac_pack -p secret encrypted.gca folder
gcac_pack -k key.bin encrypted.gca folder
gcac_pack -L legacy.gca folder

gcac_unpack l out.gca
gcac_unpack e out.gca destination
gcac_unpack out.gca destination
```

Options may appear before or after the archive. Windows slash forms such as
`/s`, `/p...` and `/l` are accepted, matching `gcac.exe`.

### Command words

`gcac_pack` accepts the `gcac.exe`-style command words:

* `a` - archive with compression (the default)
* `s` - archive in solid mode
* `n` - archive without compression

`gcac_unpack` accepts:

* `e` - extract
* `l` - list only

### Packer options

| Option | Meaning |
|---|---|
| `-s` | Solid mode: all members share one payload stream |
| `-n` | No compression: store members verbatim; the directory is also stored uncompressed unless `-C` is given |
| `-C` | Compress the directory block when it pays off (default) |
| `-L` | Write a legacy GCA0/GCA1 container |
| `-o <file>`, `-o<file>` | Order list: one path per line |
| `-d` | Keep the directory part of `-o` entries (default is base name) |
| `-p <pw>`, `-p<pw>` | Password for encryption |
| `-k <file>`, `-k<file>` | Key file; the password becomes `hex(SHA1(file))` |

### Unpacker options

| Option | Meaning |
|---|---|
| `-l` | List only |
| `-o <dir>` | Output directory (default: current directory) |
| `-p <pw>`, `-p<pw>` | Password for encryption |
| `-k <file>`, `-k<file>` | Key file; the password becomes `hex(SHA1(file))` |

## Packing rules

* The archive name follows `gcac.exe`: the extension is replaced by `.gca`.
  For example, `gcac_pack tree` writes `tree.gca`, and `gcac_unpack foo`
  finds `foo.gca`. With one positional argument, that argument is the source
  and also supplies the archive base name.
* A file argument is stored under its base name.
* A folder argument is walked recursively. Its members are stored under the
  folder's name, for example `sub\inner.bin`.
* Leaf empty directories are stored as directory members, as `gcac.exe` does.
* Members of a folder are ordered by extension, then base name, then full
  path. This matches `gcac.exe` and makes modern folder archives
  byte-identical for the same ordered member list.
* The encoder selects GCF1 or GCF2 using the same stage-1 decision as
  `gcac.exe`.

## Extraction rules

* Directory members are created as directories.
* File attributes and timestamps are restored. The timestamps are also
  restored on POSIX systems.
* Existing hidden or read-only files are replaced.
* The modern directory CRC and the legacy trailer CRC are validated. A
  damaged archive is rejected instead of being decoded as garbage.
* An archive embedded in a larger file, such as an SFX stub, is located
  automatically.
* Archives or members larger than the 32-bit size limits are rejected.

## Legacy support

`gcac_unpack` reads the legacy GCA0 and GCA1 containers:

* Variant 2, with the plain rotated directory.
* Variant 3, with the compressed directory.
* The stored (`n`) and solid modes.
* Encrypted version 6, including the `-p` password and `-k` key-file forms
  written by `gcac_v09e.exe`.

`gcac_pack -L` writes legacy GCA0 (normal) or GCA1 (solid) archives using the
variant 2 layout:

```bat
gcac_pack -L legacy.gca folder
gcac_pack -L -s legacy_solid.gca folder
gcac_pack -L -n legacy_stored.gca folder
gcac_pack -L -p secret legacy_encrypted.gca folder
gcac_pack -L -k key.bin legacy_key.gca folder
```

Legacy caveats:

* The output is readable by `gcac.exe`, `gcac_v09e.exe`, `GcaSDK`, and
  `gcac_unpack`.
* It is not byte-identical to `gcac_v09e.exe` output. v09e leaves the header
  `+0x18` marker and entry `+0x14` bytes uninitialized; this writer uses fixed
  values. The meaningful fields and the CRC agree.
* Only variant 2 is written.
* Names use one-byte lengths, so stored names are limited to 255 bytes.
* Empty directories are skipped.
* Solid legacy archives must be compressed. `-L -s -n` is rejected because the
  original readers cannot decode a solid raw stream.

## Compatibility

For the same ordered member list, modern archives written by `gcac_pack` are
byte-identical to those written by `gcac.exe`, including folder archives,
solid archives, `n` mode, and password/key-file encryption. Archives written
by either side extract correctly with the other.

A standalone `GCA3` file is not supported. `GCA3` is the directory block magic,
not a file container, and `gcac.exe`'s own handling of that case is broken.
See `../GCA_FORMAT.md` section 7.5.

## Tests

Run from this directory:

```sh
python roundtrip.py
python parity.py
python legacy.py
python legacy_write.py
```

| Test | Covers |
|---|---|
| `roundtrip.py` | Flat corpus, both directions, byte comparisons |
| `parity.py` | Folders, empty directories, attributes, CLI, byte identity, SDK extraction |
| `legacy.py` | `gcac_v09e` fixtures, encryption, key file, SDK extraction |
| `legacy_write.py` | `gcac_pack -L` output read back by every tool |

The tests use Python 3. They expect the C tools under `build/` and optionally
the original `gcac.exe`, the legacy `gcac_v09e.exe`, and the SDK test driver
at `../build/gcatest.exe`. Checks that need a missing binary are skipped.

Set `GCAX_NO_GCAC=1` to run without `gcac.exe`.

## Format reference

The complete format specification is in `../GCA_FORMAT.md`. It covers the
modern container, frames, compression pipeline, encryption, legacy layouts,
and verification notes.
