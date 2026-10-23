///////////////////////////////////////////////////////////////
// cgca -- a portable C implementation of the GCA archive format.
//
//   gcac_pack    [options] <archive.gca> <file or folder>...
//   gcac_pack    a|s|n [options] <archive.gca> <file or folder>
//   gcac_unpack  [options] <archive.gca> [extract folder]
//   gcac_unpack  e|l [options] <archive.gca> [extract folder]
//
// Both tools read and write the container that gcac.exe (GCA
// GCompressionArchiver v0.9k, SYN 2000/2001) produces.  With the
// same ordered member list their output is byte identical to
// gcac.exe, including folder archives, solid archives and the
// GCF1/GCF2 stage 1 choice.  Archives written by either side
// extract correctly with the other.
//
// Build:
//
//   Windows (Visual C++ 6):   build.bat
//   anything else:            make
//
//   python roundtrip.py       flat corpus, both directions, byte for byte
//   python parity.py          folders, empty dirs, attributes, CLI
//   python legacy.py          gcac_v09e fixtures, including encryption
//   python legacy_write.py    gcac_pack -L, read back by every tool
//
// The format is documented in ../GCA_FORMAT.md.  Two legacy archives
// (fixtures/ in the repository root) are used as test fixtures.
//
// Packer options
//   -s         solid mode: every member shares one payload stream
//   -n         no compression: payload and directory are stored verbatim
//   -C         compress the directory block when it pays off (default)
//   -L         write a legacy GCA0/GCA1 container (gcac_v09e layout)
//   -o <file>  order list: one path per line (or -o<file>)
//   -d         keep the directory part of -o entries (default: base name)
//   -p <pw>    password for encryption (or -p<pw>)
//   -k <file>  key file: the password becomes hex(SHA1(file)) (or -k<file>)
//
// Unpacker options
//   -l         list only
//   -o <dir>   output directory (default: the current directory)
//   -p <pw>    password for encryption (or -p<pw>)
//   -k <file>  key file: the password becomes hex(SHA1(file)) (or -k<file>)
//
// Options may appear before or after the archive.  "/s", "/p..." and
// the other slash forms work on Windows, like gcac.exe.
//
// Notes
//   * The archive name follows gcac.exe: the extension is replaced by
//     ".gca" (gcac_pack tree -> tree.gca; gcac_unpack foo finds foo.gca).
//   * A file argument is stored under its base name; a folder argument
//     is walked recursively and its members are stored under the
//     folder's name, e.g. "sub\inner.bin".  Leaf empty directories are
//     stored too, as gcac.exe does.  Members of a folder are ordered by
//     extension and base name, matching gcac.exe byte for byte.
//   * Extraction creates directory members as directories and restores
//     file attributes and timestamps (the timestamps are also restored
//     on POSIX systems).  Existing hidden/read-only files are replaced.
//   * The directory CRC and the legacy trailer CRC are validated; a
//     damaged archive is rejected instead of being decoded as garbage.
//   * The archive may be embedded in a larger file (an SFX stub): the
//     unpacker locates the GCAX header automatically.
//   * The unpacker also reads the legacy GCA0 and GCA1 containers (the
//     pre-0.9 layouts, see ../GCA_FORMAT.md section 7), including the
//     encrypted variant 6 written by gcac_v09e.exe.  A standalone GCA3
//     file is not supported: "GCA3" is the directory block magic, not
//     a file container, and gcac.exe's own handling of that case is
//     broken (see ../GCA_FORMAT.md section 7.5).
//   * -L writes legacy GCA0 (normal) / GCA1 (solid) archives with the
//     gcac_v09e layout; gcac.exe and gcac_v09e.exe read them.  The
//     bytes are not identical to v09e output because v09e leaves two
//     header/entry fields uninitialized; the meaningful fields and the
//     CRC agree.  Legacy archives cannot store empty directories.
///////////////////////////////////////////////////////////////
