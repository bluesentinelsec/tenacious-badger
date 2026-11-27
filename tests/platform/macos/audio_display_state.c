#include "../audio_display_state.h"

#include "badger/library.h"

#include <CoreGraphics/CGDirectDisplay.h>
#include <CoreGraphics/CGDisplayConfiguration.h>

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
