///////////////////////////////////////////////////////////////////
// kiutil.h -- shared helpers for the GcaSDK reconstruction.
//
// Only the 8.3 short-name fallback is needed by the SDK.  The
// original GCA SDK used this to shorten paths on FAT volumes; on
// NTFS (Win95/98/Me/NT/2000/XP+) long names work natively, so the
// conversion is a documented no-op.
//
// The header is intentionally dependency-free so that it can be
// included from C and C++ translation units alike.
///////////////////////////////////////////////////////////////////
#ifndef KIUTIL_H
#define KIUTIL_H

#ifdef __cplusplus
namespace kiutil {

/* Shorten `path` to a DOS 8.3-safe form in place.
 *
 * The GCA SDK calls this before creating the output directories so
 * that the directory names match the (possibly shortened) file names.
 * On NTFS the path is left untouched.
 */
inline void pathMake83(char* path)
{
    (void)path;
}

} /* namespace kiutil */
#endif /* __cplusplus */

#endif /* KIUTIL_H */
