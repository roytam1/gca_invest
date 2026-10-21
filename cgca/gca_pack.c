/////////////////////////////////////////////////////////////////
// gca_pack.c -- create GCA archives.
//
//   gcac_pack [options] <archive.gca> <file or folder>...
//
//   -s        solid mode (all members share one payload stream)
//   -n        no compression (store the members verbatim)
//   -C        compress the directory block
//   -p <pw>   password for encryption
//   -k <file> key file: the password becomes hex(SHA1(file))
//
// The archive path is used exactly as given.  A folder argument is
// walked recursively and its files are stored under their relative
// path, with backslashes, the way gcac.exe does it.
/////////////////////////////////////////////////////////////////
#include "gca.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  ifndef INVALID_FILE_ATTRIBUTES
#    define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#  endif
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <unistd.h>
#endif

/* ---------------------------------------------------------------- */
/* file metadata                                                     */
#ifndef _WIN32
/* 64 bit helpers built from 32 bit words, so that no "long long" is
 * needed (MSVC6 does not have it). */
static void gca_mul32x32(unsigned long a, unsigned long b,
                         unsigned long* hi, unsigned long* lo)
{
    unsigned long a0 = a & 0xFFFF, a1 = a >> 16;
    unsigned long b0 = b & 0xFFFF, b1 = b >> 16;
    unsigned long p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    unsigned long mid = (p0 >> 16) + (p1 & 0xFFFF) + (p2 & 0xFFFF);
    *lo = (p0 & 0xFFFF) | (mid << 16);
    *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

/* unix seconds -> windows FILETIME (100 ns units since 1601) */
static void gca_unix_to_filetime(unsigned long secs,
                                 unsigned long* hi, unsigned long* lo)
{
    unsigned long shi, slo, mhi, mlo;
    /* 11644473600 = 2 * 2^32 + 2651514368 */
    slo = secs + 2651514368UL;
    shi = 2UL;
    if (slo < 2651514368UL)
        shi++;
    /* multiply by 10000000 = 2^7 * 78125 */
    gca_mul32x32(slo, 78125UL, &mhi, &mlo);
    shi = shi * 78125UL + mhi;
    slo = mlo;
    *lo = slo << 7;
    *hi = (shi << 7) | (slo >> 25);
}
#endif

static void gca_file_info(const char* path, unsigned long* attr,
                          unsigned long* ft_lo, unsigned long* ft_hi)
{
    *attr = 0;
    *ft_lo = 0;
    *ft_hi = 0;
#ifdef _WIN32
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
            *attr = fad.dwFileAttributes;
            *ft_lo = fad.ftLastWriteTime.dwLowDateTime;
            *ft_hi = fad.ftLastWriteTime.dwHighDateTime;
        }
    }
#else
    {
        struct stat st;
        if (stat(path, &st) == 0) {
            unsigned long hi, lo;
            *attr = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
            /* unix seconds -> windows FILETIME (100ns since 1601) */
            gca_unix_to_filetime((unsigned long)st.st_mtime, &hi, &lo);
            *ft_lo = lo;
            *ft_hi = hi;
        }
    }
#endif
}

static int gca_is_dir(const char* path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
}

static int gca_read_content(const char* path, unsigned char** data, size_t* len)
{
    FILE* fp;
    long n;
    unsigned char* buf;
    *data = NULL;
    *len = 0;
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return -1; }
    buf = (unsigned char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(fp); return -1; }
    if (n && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *data = buf;
    *len = (size_t)n;
    return 0;
}

/* ---------------------------------------------------------------- */
/* directory walking                                                 */
static int gca_add_file(gca_writer* w, const char* path, const char* name)
{
    unsigned char* data = NULL;
    size_t len = 0;
    unsigned long attr, ft_lo, ft_hi;
    int rc;

    if (gca_read_content(path, &data, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    gca_file_info(path, &attr, &ft_lo, &ft_hi);
    rc = gca_pack_add(w, name, data, len, attr, ft_lo, ft_hi);
    free(data);
    if (rc != 0) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    return 0;
}

static int gca_add_tree(gca_writer* w, const char* root, const char* rel);

static int gca_add_dir(gca_writer* w, const char* root, const char* rel)
{
#ifdef _WIN32
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (strlen(root) + 3 >= sizeof(pattern))
        return -1;
    strcpy(pattern, root);
    if (pattern[strlen(pattern) - 1] != '\\')
        strcat(pattern, "\\");
    strcat(pattern, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        char child[1024], childrel[1024];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (strlen(root) + strlen(fd.cFileName) + 2 >= sizeof(child))
            continue;
        strcpy(child, root);
        if (child[strlen(child) - 1] != '\\')
            strcat(child, "\\");
        strcat(child, fd.cFileName);
        if (strlen(rel) + strlen(fd.cFileName) + 2 >= sizeof(childrel))
            continue;
        strcpy(childrel, rel);
        if (childrel[0])
            strcat(childrel, "\\");
        strcat(childrel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (gca_add_tree(w, child, childrel) != 0) { FindClose(h); return -1; }
        } else {
            if (gca_add_file(w, child, childrel) != 0) { FindClose(h); return -1; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR* dp;
    struct dirent* de;
    char child[1024], childrel[1024];
    dp = opendir(root);
    if (!dp)
        return -1;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strlen(root) + strlen(de->d_name) + 2 >= sizeof(child))
            continue;
        strcpy(child, root);
        if (child[strlen(child) - 1] != '/')
            strcat(child, "/");
        strcat(child, de->d_name);
        if (strlen(rel) + strlen(de->d_name) + 2 >= sizeof(childrel))
            continue;
        strcpy(childrel, rel);
        if (childrel[0])
            strcat(childrel, "\\");
        strcat(childrel, de->d_name);
        if (gca_is_dir(child)) {
            if (gca_add_tree(w, child, childrel) != 0) { closedir(dp); return -1; }
        } else {
            if (gca_add_file(w, child, childrel) != 0) { closedir(dp); return -1; }
        }
    }
    closedir(dp);
    return 0;
#endif
}

/* The name a member is stored under: the base name of a file argument,
 * the path relative to the archive root for a folder argument. */
static const char* gca_base_name(const char* path)
{
    const char* p = path;
    const char* q;
    for (q = path; *q; q++) {
        if (*q == '\\' || *q == '/' || *q == ':')
            p = q + 1;
    }
    return p;
}

static int gca_add_tree(gca_writer* w, const char* root, const char* rel)
{
    if (gca_is_dir(root)) {
        /* gcac.exe stores the members of a folder under the folder's own
           name, e.g. "sub\inner.bin" */
        char prefix[1024];
        const char* base = gca_base_name(root);
        if (rel && *rel) {
            if (strlen(rel) + strlen(base) + 2 >= sizeof(prefix))
                return -1;
            strcpy(prefix, rel);
            strcat(prefix, "\\");
            strcat(prefix, base);
        } else {
            if (strlen(base) + 1 >= sizeof(prefix))
                return -1;
            strcpy(prefix, base);
        }
        return gca_add_dir(w, root, prefix);
    }
    if (!rel || !*rel)
        rel = gca_base_name(root);
    return gca_add_file(w, root, rel);
}

/* ---------------------------------------------------------------- */
static int gca_load_keyfile(const char* path, char* out41)
{
    unsigned char* data = NULL;
    size_t len = 0;
    if (gca_read_content(path, &data, &len) != 0)
        return -1;
    gca_sha1_hex(data, len, out41);
    free(data);
    return 0;
}

static void gca_usage(void)
{
    fprintf(stderr,
        "usage: gcac_pack [options] <archive.gca> <file or folder>...\n"
        "\n"
        "  -s         solid mode (all members share one payload stream)\n"
        "  -n         no compression (store the members verbatim)\n"
        "  -C         compress the directory block\n"
        "  -p <pw>    password for encryption\n"
        "  -k <file>  key file: the password becomes hex(SHA1(file))\n");
}

int main(int argc, char** argv)
{
    gca_writer w;
    const char* archive = NULL;
    const char* spell = NULL;
    char keyhex[41];
    int solid = 0, compress = 1, dircomp = 0;
    int i, first = 0;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (strcmp(a, "-s") == 0) { solid = 1; continue; }
            if (strcmp(a, "-n") == 0) { compress = 0; continue; }
            if (strcmp(a, "-C") == 0) { dircomp = 1; continue; }
            if (strcmp(a, "-p") == 0) {
                if (++i >= argc) { gca_usage(); return 2; }
                spell = argv[i];
                continue;
            }
            if (strcmp(a, "-k") == 0) {
                if (++i >= argc) { gca_usage(); return 2; }
                if (gca_load_keyfile(argv[i], keyhex) != 0) {
                    fprintf(stderr, "cannot read key file %s\n", argv[i]);
                    return 1;
                }
                spell = keyhex;
                continue;
            }
            gca_usage();
            return 2;
        }
        if (!archive) { archive = a; first = i + 1; continue; }
    }
    if (!archive || first >= argc) {
        gca_usage();
        return 2;
    }

    gca_writer_init(&w, solid, compress, dircomp, spell);
    for (i = first; i < argc; i++) {
        if (gca_add_tree(&w, argv[i], "") != 0) {
            gca_writer_free(&w);
            return 1;
        }
    }
    if (w.nsrc == 0) {
        fprintf(stderr, "nothing to pack\n");
        gca_writer_free(&w);
        return 1;
    }
    if (gca_pack_finish(&w, archive) != 0) {
        fprintf(stderr, "cannot write %s\n", archive);
        gca_writer_free(&w);
        return 1;
    }
    printf("created %s (%u member%s)\n", archive, (unsigned)w.nsrc,
           w.nsrc == 1 ? "" : "s");
    gca_writer_free(&w);
    return 0;
}
