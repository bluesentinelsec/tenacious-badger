#include "../audio_display_state.h"

#include "badger/library.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGDirectDisplay.h>
#include <CoreGraphics/CGDisplayConfiguration.h>
#include <IOKit/IOKitLib.h>

tb_test_audio_display tb_test_audio_display_state(void)
{
    tb_library *graphics = NULL;
    CGDirectDisplayID (*main_display)(void) = NULL;
    boolean_t (*asleep)(CGDirectDisplayID) = NULL;
    const tb_library_binding bindings[] = {
        {"CGMainDisplayID", (tb_library_function *)&main_display, true},
        {"CGDisplayIsAsleep", (tb_library_function *)&asleep, true}};
    tb_test_audio_display state = TB_TEST_AUDIO_DISPLAY_UNKNOWN;
    if (tb_library_open("CoreGraphics.framework/CoreGraphics", TB_LIBRARY_SEARCH_SYSTEM, &graphics,
                        NULL) == TB_LIBRARY_OK &&
        tb_library_bind(graphics, bindings, sizeof(bindings) / sizeof(bindings[0]), NULL) ==
            TB_LIBRARY_OK)
    {
        const CGDirectDisplayID display = main_display();
        if (display != kCGNullDirectDisplay)
        {
            state =
                asleep(display) != 0 ? TB_TEST_AUDIO_DISPLAY_ASLEEP : TB_TEST_AUDIO_DISPLAY_AWAKE;
        }
    }
    tb_library_close(graphics);
    return state;
}

/* IOPMrootDomain publishes XNU's kIOPMSystemCapabilityAudio in its
 * System Capabilities bitmask. A dark wake can power networking and the
 * CPU while the audio hardware is still unavailable to applications. */
tb_test_audio_power tb_test_audio_power_state(void)
{
    const int64_t audio_capability = 0x04;
    tb_test_audio_power state = TB_TEST_AUDIO_POWER_UNKNOWN;
    tb_library *io = NULL;
    tb_library *foundation = NULL;
    CFMutableDictionaryRef (*matching)(const char *) = NULL;
    io_service_t (*find)(mach_port_t, CFDictionaryRef) = NULL;
    CFTypeRef (*property)(io_registry_entry_t, CFStringRef, CFAllocatorRef, IOOptionBits) = NULL;
    kern_return_t (*release_object)(io_object_t) = NULL;
    CFStringRef (*string)(CFAllocatorRef, const char *, CFStringEncoding) = NULL;
    CFTypeID (*type_id)(CFTypeRef) = NULL;
    CFTypeID (*number_id)(void) = NULL;
    Boolean (*number_value)(CFNumberRef, CFNumberType, void *) = NULL;
    void (*release)(CFTypeRef) = NULL;
    const tb_library_binding io_bindings[] = {
        {"IOServiceMatching", (tb_library_function *)&matching, true},
        {"IOServiceGetMatchingService", (tb_library_function *)&find, true},
        {"IORegistryEntryCreateCFProperty", (tb_library_function *)&property, true},
        {"IOObjectRelease", (tb_library_function *)&release_object, true}};
    const tb_library_binding cf_bindings[] = {
        {"CFStringCreateWithCString", (tb_library_function *)&string, true},
        {"CFGetTypeID", (tb_library_function *)&type_id, true},
        {"CFNumberGetTypeID", (tb_library_function *)&number_id, true},
        {"CFNumberGetValue", (tb_library_function *)&number_value, true},
        {"CFRelease", (tb_library_function *)&release, true}};
    io_service_t root = IO_OBJECT_NULL;
    CFStringRef key = NULL;
    CFTypeRef value = NULL;
    int64_t capabilities = 0;
    if (tb_library_open("IOKit.framework/IOKit", TB_LIBRARY_SEARCH_SYSTEM, &io, NULL) !=
            TB_LIBRARY_OK ||
        tb_library_bind(io, io_bindings, sizeof(io_bindings) / sizeof(io_bindings[0]), NULL) !=
            TB_LIBRARY_OK ||
        tb_library_open("CoreFoundation.framework/CoreFoundation", TB_LIBRARY_SEARCH_SYSTEM,
                        &foundation, NULL) != TB_LIBRARY_OK ||
        tb_library_bind(foundation, cf_bindings, sizeof(cf_bindings) / sizeof(cf_bindings[0]),
                        NULL) != TB_LIBRARY_OK)
    {
        goto cleanup;
    }
    CFMutableDictionaryRef const query = matching("IOPMrootDomain");
    root = query != NULL ? find(MACH_PORT_NULL, query) : IO_OBJECT_NULL;
    if (root == IO_OBJECT_NULL)
    {
        goto cleanup;
    }
    key = string(NULL, "System Capabilities", kCFStringEncodingUTF8);
    if (key == NULL)
    {
        goto cleanup;
    }
    value = property(root, key, NULL, 0);
    if (value != NULL && type_id(value) == number_id() &&
        number_value((CFNumberRef)value, kCFNumberSInt64Type, &capabilities) && capabilities >= 0)
    {
        state = (capabilities & audio_capability) != 0 ? TB_TEST_AUDIO_POWER_ON
                                                       : TB_TEST_AUDIO_POWER_OFF;
    }

cleanup:
    if (value != NULL)
    {
        release(value);
    }
    if (key != NULL)
    {
        release(key);
    }
    if (root != IO_OBJECT_NULL)
    {
        (void)release_object(root);
    }
    tb_library_close(foundation);
    tb_library_close(io);
    return state;
}
