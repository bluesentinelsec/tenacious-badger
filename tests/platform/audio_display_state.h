/* Display power is a clue when a default audio output accepts a stream but
 * stops calling it. Audio power is a separate system capability. */
#ifndef TB_TEST_AUDIO_DISPLAY_STATE_H
#define TB_TEST_AUDIO_DISPLAY_STATE_H

typedef enum tb_test_audio_display
{
    TB_TEST_AUDIO_DISPLAY_UNKNOWN,
    TB_TEST_AUDIO_DISPLAY_AWAKE,
    TB_TEST_AUDIO_DISPLAY_ASLEEP
} tb_test_audio_display;

tb_test_audio_display tb_test_audio_display_state(void);

typedef enum tb_test_audio_power
{
    TB_TEST_AUDIO_POWER_UNKNOWN,
    TB_TEST_AUDIO_POWER_OFF,
    TB_TEST_AUDIO_POWER_ON
} tb_test_audio_power;

tb_test_audio_power tb_test_audio_power_state(void);

static inline const char *tb_test_audio_power_name(tb_test_audio_power power)
{
    switch (power)
    {
    case TB_TEST_AUDIO_POWER_ON:
        return "on";
    case TB_TEST_AUDIO_POWER_OFF:
        return "off";
    case TB_TEST_AUDIO_POWER_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

#endif
