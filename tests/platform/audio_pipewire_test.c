/* What the PipeWire backend writes and reads without PipeWire: a stream's
 * format as the bytes of an SPA pod, word for word what PipeWire 1.4's
 * spa_format_audio_raw_build() writes for the same format, and the default
 * sink's name out of the "default" metadata's JSON. */
#include "platform/linux/audio_pipewire.h"

#include "tb_test.h"

#include <string.h>

/* spa_format_audio_raw_build(SPA_PARAM_EnumFormat) of F32, 48000 Hz, FL FR. */
static const uint32_t stereo_words[] = {
    0xa0,    0xf, 0x40003, 3,                                   /* object, Format, EnumFormat */
    1,       0,   4,       3, 1,      0,                        /* mediaType: audio */
    2,       0,   4,       3, 1,      0,                        /* mediaSubtype: raw */
    0x10001, 0,   4,       3, 0x11b,  0,                        /* format: F32 */
    0x10003, 0,   4,       4, 0xbb80, 0,                        /* rate: 48000 */
    0x10004, 0,   4,       4, 2,      0,                        /* channels: 2 */
    0x10005, 0,   0x10,    0xd, 4,    3, 3, 4};                 /* position: FL FR */

TB_TEST(platform_audio_pipewire, the_format_pod_is_spa_s)
{
    uint64_t pod[TB_PIPEWIRE_FORMAT_POD_CAPACITY / sizeof(uint64_t)] = {0};
    const size_t size = tb_pipewire_format_pod(48000, 2, pod);

    TB_TEST_REQUIRE_UINT(size, ==, sizeof(stereo_words));
    TB_TEST_EXPECT_BYTES(pod, stereo_words, sizeof(stereo_words));
}

/* 5.1 is FL FR FC LFE RL RR, six positions after the array's child: an even
 * count of words, so no padding. Mono is FC, padded to 8 bytes. */
TB_TEST(platform_audio_pipewire, positions_follow_the_layout)
{
    uint64_t pod[TB_PIPEWIRE_FORMAT_POD_CAPACITY / sizeof(uint64_t)] = {0};
    const uint32_t *const words = (const uint32_t *)(const void *)pod;
    static const uint32_t surround[] = {0x10005, 0, 0x20, 0xd, 4, 3, 3, 4, 5, 6, 12, 13};

    size_t size = tb_pipewire_format_pod(44100, 6, pod);
    TB_TEST_REQUIRE_UINT(size, ==, (4u + 5u * 6u + 12u) * 4u);
    TB_TEST_EXPECT_UINT(words[0], ==, size - 8u);
    TB_TEST_EXPECT_UINT(words[4 + 3 * 6 + 4], ==, 44100);
    TB_TEST_EXPECT_BYTES(words + 4 + 5 * 6, surround, sizeof(surround));

    size = tb_pipewire_format_pod(48000, 1, pod);
    TB_TEST_REQUIRE_UINT(size % 8u, ==, 0);
    TB_TEST_EXPECT_UINT(words[4 + 5 * 6 + 2], ==, 12); /* the array: child header and one */
    TB_TEST_EXPECT_UINT(words[4 + 5 * 6 + 6], ==, 5);  /* FC */

    size = tb_pipewire_format_pod(48000, 8, pod);
    TB_TEST_EXPECT_UINT(words[(size / 4u) - 2], ==, 7); /* SL, then SR */
    TB_TEST_EXPECT_UINT(words[(size / 4u) - 1], ==, 8);
    TB_TEST_EXPECT_UINT(tb_pipewire_position(0x100u), ==, 11);
    TB_TEST_EXPECT_UINT(tb_pipewire_position(0x80u), ==, 0);
}

TB_TEST(platform_audio_pipewire, the_default_sink_s_name_is_read)
{
    char name[32] = {0};

    TB_TEST_EXPECT(tb_pipewire_default_name("{\"name\":\"alsa_output.hdmi\"}", name, sizeof(name)));
    TB_TEST_EXPECT_STRING(name, ==, "alsa_output.hdmi");
    TB_TEST_EXPECT(tb_pipewire_default_name("{ \"name\" :\t\"x\" }", name, sizeof(name)));
    TB_TEST_EXPECT_STRING(name, ==, "x");

    TB_TEST_EXPECT(!tb_pipewire_default_name(NULL, name, sizeof(name)));
    TB_TEST_EXPECT_STRING(name, ==, "");
    TB_TEST_EXPECT(!tb_pipewire_default_name("{}", name, sizeof(name)));
    TB_TEST_EXPECT(!tb_pipewire_default_name("{\"name\" 1}", name, sizeof(name)));
    TB_TEST_EXPECT(!tb_pipewire_default_name("{\"name\": 1}", name, sizeof(name)));
    TB_TEST_EXPECT(!tb_pipewire_default_name("{\"name\": \"open", name, sizeof(name)));
    TB_TEST_EXPECT(!tb_pipewire_default_name("{\"name\": \"a\\\"b\"}", name, sizeof(name)));
    TB_TEST_EXPECT(!tb_pipewire_default_name(
        "{\"name\": \"a name much longer than the thirty-two bytes of the buffer\"}", name,
        sizeof(name)));
    TB_TEST_EXPECT_STRING(name, ==, "");
}
