/* debug 4: does a bare CFPreferencesSetValue on Background land? */
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>

int main(void)
{
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(
        NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFStringRef p = CFSTR("/tmp/TESTPATH-WALLINFO");

    CFDictionarySetValue(m, CFSTR("ImageFilePath"), p);
    CFPreferencesSetValue(CFSTR("Background"), m, CFSTR("com.apple.desktop"),
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFPreferencesAppSynchronize(CFSTR("com.apple.desktop"));
    printf("written\n");
    CFRelease(m);
    return 0;
}
