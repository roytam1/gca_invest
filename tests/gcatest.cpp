/////////////////////////////////////////////////////////////////
// gcatest.cpp -- test driver for the GcaSDK reconstruction.
//
//   gcatest <archive.gca> [outdir] [spell]
//
// Lists the archive and extracts it, then reports what happened.
// With a third argument the archive is opened with that password
// (the spell), which is required for encrypted archives.
//
// The driver intentionally does NOT call CGca::Init(): the original
// SDK's callers never do, and the reconstruction has to cope with
// that (the work buffers are allocated on demand).
/////////////////////////////////////////////////////////////////
#include "GcaSDK.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    nsGcaSDK::CGca g;
    int i, n;

    if (argc < 2) {
        printf("usage: gcatest <archive.gca> [outdir] [spell]\n");
        return 2;
    }
    g.SetArcFilePath(argv[1]);
    if (argc > 2 && argv[2][0])
        g.SetOutputPath(argv[2]);
    if (argc > 3 && argv[3][0])
        g.SetSpell(argv[3]);

    if (!g.OpenArchive()) {
        printf("OPEN_FAILED\n");
        return 1;
    }
    n = g.GetNumFiles();
    printf("arcType=%d numFiles=%d\n", g.GetArcType(), n);
    for (i = 0; i < n; i++) {
        printf("  [%d] name=%-16s origSize=%-9lu storedSize=%-9lu "
               "crc=0x%08lx attr=0x%08lx\n",
               i, g.GetFileName(i).c_str(),
               (unsigned long)g.GetFileSize(i),
               (unsigned long)g.GetDataSize(i),
               (unsigned long)g.GetFileCRC(i),
               (unsigned long)g.GetFileAttributes(i));
        fflush(stdout);
    }
    if (!g.Extract()) {
        printf("EXTRACT_FAILED\n");
        return 1;
    }
    printf("EXTRACT_OK\n");
    return 0;
}
