/////////////////////////////////////////////////////////////////
// cgca -- a portable C implementation of the GCA archive format.
//
//   gcac_pack    [options] <archive.gca> <file or folder>...
//   gcac_unpack  [options] <archive.gca>
//
// Both tools read and write the container that gcac.exe (GCA
// GCompressionArchiver v0.9k, SYN 2000/2001) produces.  The archives
// they create are extracted correctly by gcac.exe, and the archives
// gcac.exe creates are extracted correctly by them.
//
// Build:
//
//   Windows (Visual C++ 6):   build.bat
//   anything else:            make
//
// The format is documented in ../GCA_FORMAT.md.
//
// Packer options
//   -s         solid mode: every member shares one payload stream
//   -n         no compression: the members are stored verbatim
//   -C         compress the directory block
//   -p <pw>    password for encryption
//   -k <file>  key file: the password becomes hex(SHA1(file)), exactly
//              like gcac.exe's -k switch
//
// Unpacker options
//   -o <dir>   output directory (default: the current directory)
//   -p <pw>    password for encryption
//   -k <file>  key file: the password becomes hex(SHA1(file))
//
// Notes
//   * The archive path is used exactly as given (gcac.exe appends
//     ".gca" to it).
//   * A file argument is stored under its base name; a folder argument
//     is walked recursively and its members are stored under the
//     folder's name, e.g. "sub\inner.bin" -- the same rule gcac.exe
//     uses.
//   * The archive may be embedded in a larger file (an SFX stub): the
//     unpacker locates the GCAX header automatically.
/////////////////////////////////////////////////////////////////
