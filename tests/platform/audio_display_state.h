/* Display power is a clue when a default audio output accepts a stream but
 * stops calling it. The test still requires callbacks in every state. */
#ifndef TB_TEST_AUDIO_DISPLAY_STATE_H
#define TB_TEST_AUDIO_DISPLAY_STATE_H

typedef enum tb_test_audio_display
{
    TB_TEST_AUDIO_DISPLAY_UNKNOWN,
    TB_TEST_AUDIO_DISPLAY_AWAKE,
    TB_TEST_AUDIO_DISPLAY_ASLEEP
} tb_test_audio_display;

tb_test_audio_display tb_test_audio_display_state(void);

#endif
