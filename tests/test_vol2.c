/* debug 2: dump raw getattrlist volume buffer */
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <errno.h>

static void dump(const char *label, unsigned char *buf, int n)
{
    int i;
    printf("%s:\n", label);
    for (i = 0; i < n; i++) {
        printf("%02x ", buf[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");
}

int main(void)
{
    struct attrlist al;
    unsigned char abuf[512];
    memset(abuf, 0xCC, sizeof abuf);

    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;
    if (getattrlist("/", &al, abuf, sizeof abuf, 0) == 0) {
        attrreference_t *ar = (attrreference_t *)abuf;
        dump("INFO|NAME buffer", abuf, 64);
        printf("attr0: len=%u off=%u\n", (unsigned)ar->attr_length,
               (unsigned)ar->attr_dataoffset);
    } else {
        printf("getattrlist INFO|NAME failed: errno=%d\n", errno);
    }

    memset(abuf, 0xCC, sizeof abuf);
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_NAME;
    if (getattrlist("/", &al, abuf, sizeof abuf, 0) == 0) {
        attrreference_t *ar = (attrreference_t *)abuf;
        dump("NAME only buffer", abuf, 64);
        printf("attr0: len=%u off=%u\n", (unsigned)ar->attr_length,
               (unsigned)ar->attr_dataoffset);
    } else {
        printf("getattrlist NAME failed: errno=%d\n", errno);
    }
    return 0;
}
