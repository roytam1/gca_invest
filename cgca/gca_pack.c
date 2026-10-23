///////////////////////////////////////////////////////////////
// gca_pack.c -- create GCA archives.
//
//   gcac_pack [options] <archive.gca> <file or folder>...
//   gcac_pack a|s|n [options] <archive.gca> <file or folder>
//
//   -s        solid mode (all members share one payload stream)
//   -n        no compression (store the members verbatim)
//   -C        compress the directory block when it pays off (default)
//   -L        write a legacy GCA0/GCA1 container (gcac_v09e layout)
//   -o<file>  order list: one path per line (whitespace separated)
//   -d        keep the directory part of -o entries (default: base name)
//   -p <pw>   password for encryption
//   -k <file> key file: the password becomes hex(SHA1(file))
//
// The archive name follows gcac.exe: the extension is replaced by ".gca".
// A folder argument is walked recursively and its files are stored under
// the folder's name.  gcac.exe orders the members by extension and then
// by base name; this packer does the same, which makes folder archives
// byte identical to the ones gcac.exe produces.  -L writes the older
// GCA0/GCA1 layout read by gcac.exe and written by gcac_v09e.exe.
///////////////////////////////////////////////////////////////
#include "gca.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  ifndef INVALID_FILE_ATTRIBUTES
#    define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#  endif
#  ifndef FILE_ATTRIBUTE_REPARSE_POINT
#    define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#  endif
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <unistd.h>
#  include <ctype.h>
#endif

#ifdef _WIN32
#  define GCA_SEP "\\"
#else
#  define GCA_SEP "/"
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
/* path helpers                                                      */
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

/* Copy the base name without trailing separators. */
static void gca_base_copy(const char* path, char* out, size_t cap)
{
    size_t n = strlen(path);
    size_t s;
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/'))
        n--;
    s = n;
    while (s > 0 && path[s - 1] != '\\' && path[s - 1] != '/')
        s--;
    if (n - s >= cap)
        n = s + cap - 1;
    if (n > s)
        memcpy(out, path + s, n - s);
    out[n - s] = 0;
}

/* The stored name for a -d order-list entry: drop the drive and any
 * leading separator/dot, and use backslashes (gcac.exe does the same). */
static void gca_dir_name(const char* in, char* out, size_t cap)
{
    const char* p = in;
    size_t n = 0;
    if (p[0] && p[1] == ':')
        p += 2;
    while (*p == '\\' || *p == '/' || *p == '.')
        p++;
    while (*p && n + 1 < cap) {
        out[n++] = (*p == '/') ? '\\' : *p;
        p++;
    }
    out[n] = 0;
}

static int gca_abs_path(const char* p)
{
#ifdef _WIN32
    if (!p || !p[0])
        return 0;
    if (p[0] == '\\' || p[0] == '/')
        return 1;
    if (p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') ||
                        (p[0] >= 'a' && p[0] <= 'z')))
        return 1;
    return 0;
#else
    return p && p[0] == '/';
#endif
}

static int gca_join(char* out, size_t cap, const char* dir, const char* name)
{
    size_t a = strlen(dir), b = strlen(name);
    int sep = (a > 0 && dir[a - 1] != '\\' && dir[a - 1] != '/');
    if (a + (size_t)sep + b + 1 > cap)
        return -1;
    strcpy(out, dir);
    if (sep)
        strcat(out, GCA_SEP);
    strcat(out, name);
    return 0;
}

/* ---------------------------------------------------------------- */
/* folder walk                                                       */
/* gcac.exe collects the files of a folder (and its empty directories)
 * into one list and sorts it by extension and then by base name.  We
 * reproduce that order, so folder archives are byte identical. */
typedef struct {
    char* path;                 /* filesystem path */
    char* name;                 /* stored name */
    int is_dir;                 /* an empty directory member */
    unsigned long attr;
    unsigned long ft_lo;
    unsigned long ft_hi;
    size_t seq;
} gca_walk_item;

typedef struct {
    gca_walk_item* v;
    size_t n;
    size_t cap;
} gca_walk_list;

static void gca_walk_init(gca_walk_list* l)
{
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

static void gca_walk_free(gca_walk_list* l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (l->v[i].path) free(l->v[i].path);
        if (l->v[i].name) free(l->v[i].name);
    }
    if (l->v) free(l->v);
    gca_walk_init(l);
}

static int gca_walk_push(gca_walk_list* l, const char* path, const char* name,
                         int is_dir, unsigned long attr,
                         unsigned long ft_lo, unsigned long ft_hi)
{
    gca_walk_item* it;
    size_t np, nn;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        gca_walk_item* p = (gca_walk_item*)realloc(l->v,
                                                   sizeof(gca_walk_item) * cap);
        if (!p)
            return -1;
        l->v = p;
        l->cap = cap;
    }
    it = &l->v[l->n];
    np = strlen(path) + 1;
    nn = strlen(name) + 1;
    it->path = (char*)malloc(np);
    it->name = (char*)malloc(nn);
    if (!it->path || !it->name) {
        if (it->path) free(it->path);
        if (it->name) free(it->name);
        return -1;
    }
    memcpy(it->path, path, np);
    memcpy(it->name, name, nn);
    it->is_dir = is_dir;
    it->attr = attr;
    it->ft_lo = ft_lo;
    it->ft_hi = ft_hi;
    it->seq = l->n;
    l->n++;
    return 0;
}

static const char* gca_ext_of(const char* name)
{
    const char* base = gca_base_name(name);
    const char* dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

static int gca_walk_cmp(const void* a, const void* b)
{
    const gca_walk_item* x = (const gca_walk_item*)a;
    const gca_walk_item* y = (const gca_walk_item*)b;
    int c = strcmp(gca_ext_of(x->name), gca_ext_of(y->name));
    if (c)
        return c;
    c = strcmp(gca_base_name(x->name), gca_base_name(y->name));
    if (c)
        return c;
    c = strcmp(x->name, y->name);
    if (c)
        return c;
    if (x->seq < y->seq) return -1;
    if (x->seq > y->seq) return 1;
    return 0;
}

static int gca_collect(gca_walk_list* l, const char* root, const char* rel)
{
    size_t before;
    unsigned long attr, ft_lo, ft_hi;

    if (!gca_is_dir(root)) {
        gca_file_info(root, &attr, &ft_lo, &ft_hi);
        return gca_walk_push(l, root, rel, 0, attr, ft_lo, ft_hi);
    }

    before = l->n;
#ifdef _WIN32
    {
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
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
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
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue;
                if (gca_collect(l, child, childrel) != 0) {
                    FindClose(h);
                    return -1;
                }
            } else {
                gca_file_info(child, &attr, &ft_lo, &ft_hi);
                if (gca_walk_push(l, child, childrel, 0, attr,
                                  ft_lo, ft_hi) != 0) {
                    FindClose(h);
                    return -1;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* dp;
        struct dirent* de;
        dp = opendir(root);
        if (!dp)
            return -1;
        while ((de = readdir(dp)) != NULL) {
            char child[1024], childrel[1024];
            struct stat st;
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
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
            if (stat(child, &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                if (gca_collect(l, child, childrel) != 0) {
                    closedir(dp);
                    return -1;
                }
            } else {
                gca_file_info(child, &attr, &ft_lo, &ft_hi);
                if (gca_walk_push(l, child, childrel, 0, attr,
                                  ft_lo, ft_hi) != 0) {
                    closedir(dp);
                    return -1;
                }
            }
        }
        closedir(dp);
    }
#endif

    if (l->n == before) {
        /* a leaf empty directory is a member of its own */
        gca_file_info(root, &attr, &ft_lo, &ft_hi);
        attr |= 0x10;
        return gca_walk_push(l, root, rel, 1, attr, ft_lo, ft_hi);
    }
    return 0;
}

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

static int gca_add_walk(gca_writer* w, gca_walk_list* l)
{
    size_t i;
    if (l->n == 0)
        return 0;
    qsort(l->v, l->n, sizeof(gca_walk_item), gca_walk_cmp);
    for (i = 0; i < l->n; i++) {
        gca_walk_item* it = &l->v[i];
        if (it->is_dir) {
            if (gca_pack_add(w, it->name, NULL, 0, it->attr,
                             it->ft_lo, it->ft_hi) != 0) {
                fprintf(stderr, "out of memory\n");
                return -1;
            }
        } else {
            if (gca_add_file(w, it->path, it->name) != 0)
                return -1;
        }
    }
    return 0;
}

static int gca_add_named(gca_writer* w, const char* path, const char* name)
{
    gca_walk_list l;
    int rc;
    if (!gca_is_dir(path))
        return gca_add_file(w, path, name);
    gca_walk_init(&l);
    rc = gca_collect(&l, path, name);
    if (rc == 0)
        rc = gca_add_walk(w, &l);
    gca_walk_free(&l);
    return rc;
}

static int gca_add_tree(gca_writer* w, const char* root, const char* rel)
{
    char prefix[1024];
    if (gca_is_dir(root)) {
        char base[512];
        gca_base_copy(root, base, sizeof(base));
        if (!base[0])
            return -1;
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
        return gca_add_named(w, root, prefix);
    }
    if (!rel || !*rel) {
        char base[512];
        gca_base_copy(root, base, sizeof(base));
        if (!base[0])
            return -1;
        return gca_add_file(w, root, base);
    }
    return gca_add_file(w, root, rel);
}

/* ---------------------------------------------------------------- */
/* order list (-o)                                                   */
static void gca_list_name(const char* entry, int keep_dir,
                          char* out, size_t cap)
{
    if (keep_dir)
        gca_dir_name(entry, out, cap);
    else
        gca_base_copy(entry, out, cap);
}

static int gca_add_list_file(gca_writer* w, const char* listfile,
                             int keep_dir)
{
    unsigned char* data = NULL;
    size_t len = 0;
    size_t p = 0;
    char listdir[1024];
    char tok[1024];
    char* q;

    if (gca_read_content(listfile, &data, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", listfile);
        return -1;
    }
    strcpy(listdir, listfile);
    q = strrchr(listdir, '\\');
    if (!q) q = strrchr(listdir, '/');
    if (q)
        *q = 0;
    else
        strcpy(listdir, ".");

    while (p < len) {
        size_t start, n;
        char path[1024], name[1024];
        while (p < len && data[p] < 0x20)
            p++;
        start = p;
        while (p < len && data[p] >= 0x20)
            p++;
        n = p - start;
        if (n == 0)
            continue;
        if (n >= sizeof(tok))
            n = sizeof(tok) - 1;
        memcpy(tok, data + start, n);
        tok[n] = 0;

        if (gca_abs_path(tok)) {
            strcpy(path, tok);
        } else if (gca_path_exists(tok)) {
            /* gcac.exe resolves order-list entries from the current
               directory; keep that first for compatibility */
            strcpy(path, tok);
        } else if (gca_join(path, sizeof(path), listdir, tok) == 0 &&
                   gca_path_exists(path)) {
            /* fall back to the list file's own directory */
        } else {
            fprintf(stderr, "not found %s\n", tok);
            continue;
        }
        if (!gca_path_exists(path)) {
            fprintf(stderr, "not found %s\n", tok);
            continue;
        }
        gca_list_name(tok, keep_dir, name, sizeof(name));
        if (!name[0]) {
            fprintf(stderr, "bad name %s\n", tok);
            continue;
        }
        if (gca_add_named(w, path, name) != 0) {
            free(data);
            return -1;
        }
    }
    free(data);
    return 0;
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
        "  a, s, n    optional gcac.exe style command (archive/solid/none)\n"
        "  -s         solid mode (all members share one payload stream)\n"
        "  -n         no compression (store the members verbatim)\n"
        "  -C         compress the directory block when it pays off (default)\n"
        "  -L         write a legacy GCA0/GCA1 container (gcac_v09e layout)\n"
        "  -o <file>  order list: one path per line (or -o<file>)\n"
        "  -d         keep the directory part of -o entries\n"
        "  -p <pw>    password for encryption (or -p<pw>)\n"
        "  -k <file>  key file (or -k<file>)\n");
}

static int gca_push_arg(const char*** args, size_t* n, size_t* cap,
                        const char* p)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        const char** v = (const char**)realloc((void*)*args,
                                               sizeof(const char*) * c);
        if (!v)
            return -1;
        *args = v;
        *cap = c;
    }
    (*args)[(*n)++] = p;
    return 0;
}

int main(int argc, char** argv)
{
    gca_writer w;
    const char** pos = NULL;
    size_t npos = 0, cpos = 0;
    const char* archive = NULL;
    const char* listfile = NULL;
    const char* spell = NULL;
    char keyhex[41];
    char archive_buf[1024];
    int solid = 0, compress = 1, dircomp = 1, dircomp_opt = 0, keep_dir = 0;
    int legacy = 0, keyfile = 0;
    int cmd = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        int isopt = 0;
        char opt = 0;
        const char* val = NULL;

        if (a[0] == '-' && a[1]) {
            isopt = 1;
            opt = a[1];
            if (a[2])
                val = a + 2;
        }
#ifdef _WIN32
        else if (a[0] == '/' && a[1] &&
                 strchr("sSnNcCdDpPkKoO", a[1]) != NULL) {
            isopt = 1;
            opt = a[1];
            if (a[2])
                val = a + 2;
        }
#endif
        if (isopt) {
            int need = 0;
            char lower = (opt >= 'A' && opt <= 'Z') ?
                         (char)(opt - 'A' + 'a') : opt;
            if (lower == 'p' || lower == 'k' || lower == 'o')
                need = 1;
            if (need && !val) {
                if (i + 1 >= argc) { gca_usage(); free((void*)pos); return 2; }
                val = argv[++i];
            }
            if (lower == 's') {
                solid = 1;
            } else if (lower == 'n') {
                compress = 0;
            } else if (lower == 'c') {
                dircomp = 1;
                dircomp_opt = 1;
            } else if (lower == 'd') {
                keep_dir = 1;
            } else if (lower == 'l') {
                legacy = 1;
            } else if (lower == 'p') {
                spell = val;
                keyfile = 0;
            } else if (lower == 'k') {
                if (gca_load_keyfile(val, keyhex) != 0) {
                    fprintf(stderr, "cannot read key file %s\n", val);
                    free((void*)pos);
                    return 1;
                }
                spell = keyhex;
                keyfile = 1;
            } else if (lower == 'o') {
                listfile = val;
            } else {
                gca_usage();
                free((void*)pos);
                return 2;
            }
            continue;
        }
        if (gca_push_arg(&pos, &npos, &cpos, a) != 0) {
            fprintf(stderr, "out of memory\n");
            free((void*)pos);
            return 1;
        }
    }

    /* gcac.exe's a/s/n command word */
    if (npos >= 1 && pos[0][0] && !pos[0][1]) {
        char c = pos[0][0];
        if (c == 'a' || c == 'A') { cmd = 'a'; }
        else if (c == 's' || c == 'S') { cmd = 's'; solid = 1; }
        else if (c == 'n' || c == 'N') { cmd = 'n'; compress = 0; }
    }

    if (cmd) {
        if (npos < 2) {
            gca_usage();
            free((void*)pos);
            return 2;
        }
        archive = pos[1];
    } else {
        if (npos < 1) {
            gca_usage();
            free((void*)pos);
            return 2;
        }
        archive = pos[0];
    }

    /* gcac.exe's "n" command stores the directory uncompressed too */
    if (!compress && !dircomp_opt)
        dircomp = 0;

    if (!archive || gca_archive_path(archive, archive_buf,
                                     sizeof(archive_buf)) != 0) {
        fprintf(stderr, "bad archive name\n");
        free((void*)pos);
        return 2;
    }

    gca_writer_init(&w, solid, compress, dircomp, legacy, keyfile, spell);

    if (listfile) {
        if (gca_add_list_file(&w, listfile, keep_dir) != 0) {
            gca_writer_free(&w);
            free((void*)pos);
            return 1;
        }
    } else {
        size_t first;
        size_t k;
        if (cmd)
            first = (npos == 2) ? 1 : 2;
        else
            first = (npos == 1) ? 0 : 1;
        for (k = first; k < npos; k++) {
            if (gca_add_tree(&w, pos[k], "") != 0) {
                gca_writer_free(&w);
                free((void*)pos);
                return 1;
            }
        }
    }
    free((void*)pos);

    if (w.nsrc == 0) {
        fprintf(stderr, "nothing to pack\n");
        gca_writer_free(&w);
        return 1;
    }
    if (gca_pack_finish(&w, archive_buf) != 0) {
        fprintf(stderr, "cannot write %s\n", archive_buf);
        gca_writer_free(&w);
        return 1;
    }
    printf("created %s (%u member%s)\n", archive_buf, (unsigned)w.nsrc,
           w.nsrc == 1 ? "" : "s");
    gca_writer_free(&w);
    return 0;
}
