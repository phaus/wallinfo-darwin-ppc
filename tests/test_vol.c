/* debug: why does getattrlist(ATTR_VOL_NAME) fail on "/" ? */
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <errno.h>
#include <sys/mount.h>

int main(void)
{
    struct attrlist al;
    unsigned char abuf[512];
    struct statfs sfs;

    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;
    if (getattrlist("/", &al, abuf, sizeof abuf, 0) == 0) {
        attrreference_t *ar = (attrreference_t *)abuf;
        char name[256];
        size_t l = ar->attr_length;
        if (l > sizeof name - 1) l = sizeof name - 1;
        memcpy(name, (char *)ar + ar->attr_dataoffset, l);
        name[l] = 0;
        printf("volname: '%s' (len %u)\n", name, (unsigned)ar->attr_length);
    } else {
        printf("getattrlist failed: errno=%d (%s)\n", errno, strerror(errno));
    }
    if (statfs("/", &sfs) == 0)
        printf("statfs: from=%s on=%s\n", sfs.f_mntfromname, sfs.f_mntonname);
    return 0;
}
