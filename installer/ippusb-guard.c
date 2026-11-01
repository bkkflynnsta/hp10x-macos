/*
 * hp10x-ippusb-guard - keep macOS's ippusbd off HP Laser 103/107/108 printers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These printers advertise IPP-over-USB, but their implementation returns empty
 * responses. When macOS creates its per-printer ippusbd job, ippusbd claims both USB
 * interfaces and the normal usb:// queue goes "offline". For each connected printer
 * this tool disables and removes the job
 *     system/com.apple.print.ippusb.<vendor>.<product>.<serial>
 * Run by launchd on HP USB attach (IOKit matching event), and once at install.
 * Options: --once (check now and exit), --dry-run (print what would be disabled).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dispatch/dispatch.h>
#include <fnmatch.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <xpc/xpc.h>

#define HP_VENDOR 0x03f0

extern char **environ;

static int get_string(io_service_t dev, CFStringRef key, char *buf, size_t cap)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0);
    int ok = v && CFGetTypeID(v) == CFStringGetTypeID() &&
             CFStringGetCString(v, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    if (v)
        CFRelease(v);
    return ok;
}

static int run_launchctl(const char *verb, const char *target)
{
    char *argv[] = { "/bin/launchctl", (char *)verb, (char *)target, NULL };
    pid_t pid;
    int status = -1;

    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
        return -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int dry_run;

/* Returns the number of matching printers found. */
static int guard_printers(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    int vid = HP_VENDOR, found = 0;
    io_iterator_t it;
    io_service_t dev;

    /* IOUSBHostDevice only matches idVendor inside IOPropertyMatch. */
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFDictionarySetValue(props, CFSTR(kUSBVendorID), n);
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(n);
    CFRelease(props);
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS)
        return 0;

    while ((dev = IOIteratorNext(it))) {
        char product[128], vendor[128], serial[128], target[512];

        if (get_string(dev, CFSTR("USB Product Name"), product, sizeof(product)) &&
            !fnmatch("HP Laser 10[378]*", product, 0) &&
            get_string(dev, CFSTR("USB Vendor Name"), vendor, sizeof(vendor)) &&
            get_string(dev, CFSTR("USB Serial Number"), serial, sizeof(serial))) {
            found++;
            snprintf(target, sizeof(target), "system/com.apple.print.ippusb.%s.%s.%s",
                     vendor, product, serial);
            if (dry_run) {
                printf("would disable %s\n", target);
                IOObjectRelease(dev);
                continue;
            }
            run_launchctl("disable", target);
            /* Removing the job also stops an ippusbd that already holds the printer. */
            if (run_launchctl("bootout", target) == 0)
                syslog(LOG_NOTICE, "removed ippusbd job for %s (%s)", product, serial);
        }
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    return found;
}

int main(int argc, char **argv)
{
    openlog("hp10x-ippusb-guard", LOG_PID, LOG_DAEMON);

    dry_run = argc > 1 && !strcmp(argv[1], "--dry-run");
    if (argc > 1 && (dry_run || !strcmp(argv[1], "--once"))) {
        int n = guard_printers();
        printf("hp10x-ippusb-guard: checked %d printer(s)\n", n);
        return 0;
    }

    /* launchd relaunches us until the matching event is consumed. */
    xpc_set_event_stream_handler("com.apple.iokit.matching", dispatch_get_main_queue(),
                                 ^(xpc_object_t event) { (void)event; });

    guard_printers();
    /* macOS may create the ippusbd job just after the device appears: check again. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        guard_printers();
        exit(0);
    });
    dispatch_main();
}
