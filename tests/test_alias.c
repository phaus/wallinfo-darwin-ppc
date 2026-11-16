/* debug 3b: alias blob via FSNewAlias (and FSNewAliasMinimal for compare) */
#include <stdio.h>
#include <string.h>
#include <CoreServices/CoreServices.h>

static void dump(const char *label, unsigned char *p, int n)
{
    int i;
    printf("%s (%d bytes):\n", label, n);
    for (i = 0; i < n; i++) {
        printf("%02x", p[i]);
        if ((i & 15) == 15) printf("\n"); else printf(" ");
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/Library/Desktop Pictures/Aqua Blue.jpg";
    FSRef ref;
    AliasHandle alias = NULL;
    OSErr err;
    int len;

    if ((err = FSPathMakeRef((const UInt8 *)path, &ref, NULL)) != noErr) {
        printf("FSPathMakeRef: %d\n", err);
        return 1;
    }
    err = FSNewAlias(NULL, &ref, &alias);
    printf("FSNewAlias: err=%d\n", err);
    if (err == noErr && alias) {
        len = GetHandleSize((Handle)alias);
        HLock((Handle)alias);
        dump("FSNewAlias", (unsigned char *)*alias, len);
        HUnlock((Handle)alias);
        DisposeHandle((Handle)alias);
    }
    alias = NULL;
    err = 0;
    (void)err;
    return 0;
}
