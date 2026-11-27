/* What the WASAPI backend makes of formats, identifiers, names and times,
 * without a device: the stream's WAVE_FORMAT_EXTENSIBLE, which WASAPI is
 * given whatever the device mixes in; identifiers compared as the
 * notifications need; the system's UTF-16 names in UTF-8, cut to fit; and
 * WASAPI's 100-nanosecond units as frames. */
#include "platform/win32/audio_wasapi.h"

#include "badger/audio_device.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT's first field: the format tag of floats. */
#define FLOAT_SUBTYPE_TAG 3u

TB_TEST(platform_audio_wasapi, streams_are_floats_in_the_standard_layout)
{
    const WAVEFORMATEXTENSIBLE stereo = tb_wasapi_wave_format(48000, 2);
    const WAVEFORMATEXTENSIBLE surround = tb_wasapi_wave_format(44100, 6);

    TB_TEST_EXPECT_HEX(stereo.Format.wFormatTag, ==, WAVE_FORMAT_EXTENSIBLE);
    TB_TEST_EXPECT_UINT(stereo.Format.nChannels, ==, 2);
    TB_TEST_EXPECT_UINT(stereo.Format.nSamplesPerSec, ==, 48000);
    TB_TEST_EXPECT_UINT(stereo.Format.nBlockAlign, ==, 8);
    TB_TEST_EXPECT_UINT(stereo.Format.nAvgBytesPerSec, ==, 48000u * 8u);
    TB_TEST_EXPECT_UINT(stereo.Format.wBitsPerSample, ==, 32);
    TB_TEST_EXPECT_UINT(stereo.Format.cbSize, ==, 22);
    TB_TEST_EXPECT_UINT(stereo.Samples.wValidBitsPerSample, ==, 32);
    TB_TEST_EXPECT_HEX(stereo.dwChannelMask, ==, 0x3u);
    TB_TEST_EXPECT_HEX(stereo.SubFormat.Data1, ==, FLOAT_SUBTYPE_TAG);
    TB_TEST_EXPECT_HEX(surround.dwChannelMask, ==, 0x3fu);
    TB_TEST_EXPECT_UINT(surround.Format.nBlockAlign, ==, 24);
}

TB_TEST(platform_audio_wasapi, identifiers_compare_whole)
{
    TB_TEST_EXPECT(tb_wasapi_same_id(L"{0.0.0.00000000}.{a}", L"{0.0.0.00000000}.{a}"));
    TB_TEST_EXPECT(!tb_wasapi_same_id(L"{a}", L"{a}b"));
    TB_TEST_EXPECT(!tb_wasapi_same_id(L"{a}b", L"{a}"));
    TB_TEST_EXPECT(!tb_wasapi_same_id(L"{a}", NULL));
    TB_TEST_EXPECT(tb_wasapi_same_id(NULL, NULL));
}

TB_TEST(platform_audio_wasapi, names_become_utf8)
{
    char text[TB_AUDIO_DEVICE_NAME_CAPACITY] = {0};
    wchar_t long_name[400] = {0};
    const wchar_t unpaired[] = {L'a', 0xd800, L'b', 0};

    tb_wasapi_text(L"Speakers (Realtek\x00ae Audio)", text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "Speakers (Realtek\xc2\xae Audio)");
    tb_wasapi_text(NULL, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "");
    tb_wasapi_text(unpaired, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "");

    /* Longer than any name: cut at a character, never in a pair. */
    for (size_t index = 0; index + 2 < TB_TEST_COUNT_OF(long_name); index += 2)
    {
        long_name[index] = 0xd83d; /* U+1F50A, a speaker, four bytes */
        long_name[index + 1] = 0xdd0a;
    }
    tb_wasapi_text(long_name, text, sizeof(text));
    TB_TEST_EXPECT_UINT(tb_str_length(text) % 4, ==, 0);
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, sizeof(text) - 4);
    /* Shifted by one, so that the 256th unit begins a pair the cut would split. */
    long_name[0] = L'x';
    for (size_t index = 1; index + 2 < TB_TEST_COUNT_OF(long_name); index += 2)
    {
        long_name[index] = 0xd83d;
        long_name[index + 1] = 0xdd0a;
    }
    tb_wasapi_text(long_name, text, sizeof(text));
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, 1u + 4u * 31u);
}

TB_TEST(platform_audio_wasapi, times_become_frames)
{
    TB_TEST_EXPECT_UINT(tb_wasapi_frames(100000, 48000), ==, 480);
    TB_TEST_EXPECT_UINT(tb_wasapi_frames(30000, 44100), ==, 132);
    TB_TEST_EXPECT_UINT(tb_wasapi_frames(0, 48000), ==, 0);
    TB_TEST_EXPECT_UINT(tb_wasapi_frames(-5, 48000), ==, 0);
    TB_TEST_EXPECT_UINT(tb_wasapi_frames(INT64_MAX, 384000), ==, UINT32_MAX);
}
