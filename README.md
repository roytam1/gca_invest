# GCA archive tools

This repository documents and reimplements the GCA archive format used by
`gcac.exe` (GCA GCompressionArchiver 0.9k, SYN 2000/2001).

It contains a portable C packer/unpacker, a reconstructed C++ SDK, the
format specification, and legacy archives used as read-back fixtures.

## Status

| Area | Status |
|---|---|
| Modern GCAX/GCA3 container | Read and write |
| GCF0, GCF1 and GCF2 frames | Read and write |
| Plain, solid and stored (`n`) modes | Read and write |
| Password and key-file encryption | Read and write |
| Folder trees, directory members and metadata | Read and write |
| Legacy GCA0/GCA1 containers | Read; write with `gcac_pack -L` |
| Encrypted legacy version 6 | Read and write |
| GcaSDK reconstruction | Read modern and legacy containers |

For the same ordered member list, modern archives written by `gcac_pack` are
byte-identical to those written by `gcac.exe`, including folder ordering, the
GCF1/GCF2 stage-1 choice, solid mode, and encryption.

The audit found no remaining format, compression, or decompression **method**
gaps within the tested scope. The remaining limitations are engineering and
legacy-writer limits, listed below.

## Repository layout

| Path | Contents |
|---|---|
| `GCA_FORMAT.md` | Full format specification, including legacy and encryption layouts |
| `cgca/` | Portable C packer and unpacker |
| `GcaSDK/` | Reconstructed C++ SDK and decoder |
| `tests/gcatest.cpp` | Small command-line driver for the SDK |
| `fixtures/` | Modern and legacy GCA archives used by the tests |
| `gcac.exe` | Original 0.9k reference binary |
| `gcac.exe.c` | Hex-Rays decompiled reference |
| `gcac.exe.lst` | IDA disassembly listing |
| `build.bat` | Visual C++ 6 build for the SDK and `gcatest.exe` |
| `cgca/build.bat` | Visual C++ 6 build for the C tools |

The files under `fixtures/` include legacy archives generated with
`gcac_v09e.exe`: plain, solid, stored, encrypted, encrypted solid, and a
key-file archive. They are committed, so the legacy tests do not need the
legacy writer binary.

## Build

### C tools

Windows, Visual C++ 6:

```bat
cd cgca
build.bat
```

POSIX or another C compiler:

```sh
cd cgca
make
```

The VC6 build writes `cgca/build/gcac_pack.exe` and
`cgca/build/gcac_unpack.exe`. The Makefile writes the executables into
`cgca/`.

### SDK

From the repository root:

```bat
build.bat
```

This writes `build/gcasdk.lib` and, when `tests/gcatest.cpp` is present,
`build/gcatest.exe`.

## Quick start

```bat
cgca\build\gcac_pack.exe out.gca file.txt folder\
cgca\build\gcac_pack.exe a out.gca folder
cgca\build\gcac_pack.exe -s out.gca folder
cgca\build\gcac_pack.exe -n out.gca folder
cgca\build\gcac_pack.exe -p secret out.gca folder
cgca\build\gcac_pack.exe -L legacy.gca folder

cgca\build\gcac_unpack.exe l out.gca
cgca\build\gcac_unpack.exe e out.gca destination
```

`gcac_pack` also accepts the `gcac.exe`-style `a`, `s` and `n` command words;
`gcac_unpack` accepts `e` and `l`. Options may appear before or after the
archive, and Windows slash forms such as `/p...` are accepted.

See `cgca/README.md` for the complete option list.

## Compatibility findings

### Modern container

* The GCAX header, GCA3 directory, entry table, name pool, optional compressed
  directory body, and directory CRC are implemented in both directions.
* GCF0 (stored), GCF1 (24-byte header), and GCF2 (28-byte header) are decoded;
  the encoder selects the same stage-1 coder as `gcac.exe`.
* Folder members are ordered by extension, then base name, then full path;
  empty directories are stored as directory members.
* Extraction restores directory members, file attributes, and FILETIMEs.
* The modern directory CRC and the legacy trailer CRC are validated.
* Archives embedded in a larger file, such as an SFX stub, are located
  automatically by the unpacker.

### Legacy containers

* Both legacy directory variants are read: variant 2 with a plain rotated
  directory, and variant 3 with the compressed directory.
* The legacy stored mode, solid mode, and encrypted version 6 are read.
* Version 6 decrypts the directory with `<password>Header` and each payload
  with the member-specific or solid key.
* Files that `gcac_v09e.exe` leaves uninitialized are tolerated by readers.

`gcac_pack -L` writes variant 2 GCA0/GCA1 archives. They are readable by
`gcac.exe`, `gcac_v09e.exe`, `GcaSDK`, and `gcac_unpack`, but are not
byte-identical to `gcac_v09e.exe` output because v09e leaves the header
`+0x18` marker and entry `+0x14` bytes uninitialized. The meaningful fields
and the CRC agree.

Legacy writer limits:

* Variant 2 only; one-byte name lengths, so names are limited to 255 bytes.
* Empty directories are skipped.
* Solid legacy archives must be compressed; `-L -s -n` is rejected because
  the original readers cannot decode a solid raw stream.

### GcaSDK audit

The SDK reconstruction had these gaps, all fixed:

1. Legacy variant 2 attributes and FILETIMEs were associated with the wrong
   members because the rotated directory was parsed without shifting the
   metadata.
2. Encrypted legacy version 6 archives were not decrypted.
3. The modern directory CRC and legacy trailer CRC were not validated.
4. Solid archives whose first member was an empty file or directory failed
   before the shared stream was decoded.
5. Directory members are created as directories during extraction.

`GcaSDK` handles the modern container, legacy variants 2 and 3, raw and
GCF0/GCF1/GCF2 payloads, password encryption (including key-file derived
passwords), directory members, and CRC validation.

### Remaining limitations

| Limitation | Applies to |
|---|---|
| Decoder only, no archive writer | GcaSDK |
| 32-bit/whole-file design; no streaming or >4 GiB members | C tools, GcaSDK |
| Standalone `GCA3` container not supported (the original handling is broken) | C tools, GcaSDK |
| Legacy output is variant 2 only, skips empty dirs, rejects solid+raw | `gcac_pack -L` |
| Legacy output is interoperable but not byte-identical to v09e | `gcac_pack -L` |

## Tests

Run from `cgca/` after building both the C tools and the SDK:

```sh
python roundtrip.py
python parity.py
python legacy.py
python legacy_write.py
```

| Test | Covers | Last result |
|---|---|---|
| `roundtrip.py` | Flat corpus, both directions, byte comparisons | 9/9 |
| `parity.py` | Folders, empty dirs, metadata, CLI, byte identity, SDK extraction | 42/42 |
| `legacy.py` | v09e legacy fixtures, encryption, key file, SDK extraction | 39/39 |
| `legacy_write.py` | `gcac_pack -L` output read by every tool | 43/43 |

The counts assume `gcac.exe`, `gcac_v09e.exe`, and `build/gcatest.exe` are
present; checks that need a missing binary are skipped. Set
`GCAX_NO_GCAC=1` to run only the checks that do not need `gcac.exe`.

Legacy fixture passwords:

* `fixtures/gcacv09e_encrypted.gca` and
  `fixtures/gcacv09e_encrypted_solid.gca`: password `fixture`.
* `fixtures/gcacv09e_encrypted_key.gca`: key file
  `fixtures/gcacv09e_key.bin`.

## Format documentation

`GCA_FORMAT.md` covers:

1. Command line behavior
2. Modern GCAX/GCA3 file layout
3. Payload region
4. GCF0/GCF1/GCF2 frames
5. Compression pipeline
6. Encryption
7. Legacy GCA0/GCA1 containers and encrypted version 6
8. Implementations in this repository
9. Reproduction and verification notes
