#include "badger/engine_settings.h"
#include "badger/mixer.h"
#include "badger/platform.h"
#include "badger/thread.h"
#include "tb_test.h"

static const tb_audio_format stereo = {.sample_rate = 8000, .channels = 2};

static tb_mixer_clip *make_clip(tb_mixer *mixer, const float *values, size_t frames,
                                const tb_sound_loop *loop)
{
    const tb_sound_spec spec = {.sample_rate = 8000, .channels = 1, .format = TB_SOUND_FORMAT_F32};
    tb_sound *sound = NULL;
    tb_mixer_clip *clip = NULL;
    TB_TEST_REQUIRE_UINT(
        tb_sound_create_copy(&spec, values, frames * sizeof(*values), NULL, &sound), ==,
        TB_SOUND_OK);
    if (loop != NULL)
    {
        TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, loop), ==, TB_SOUND_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_mixer_prepare_clip(mixer, sound, &clip), ==, TB_SOUND_OK);
    tb_sound_destroy(sound);
    return clip;
}

TB_TEST(audio_mixer, converted_clips_outlive_the_source_and_finish_at_the_last_frame)
{
    const tb_audio_format format = {.sample_rate = 16000, .channels = 2};
    tb_mixer *const mixer = tb_mixer_create(&format, 2);
    const float source[] = {-1.0f, 1.0f};
    tb_mixer_clip *const clip = make_clip(mixer, source, 2, NULL);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.pan = 1.0f;
    desc.gain = 0.5f;
    const tb_mixer_voice voice = tb_mixer_play(mixer, clip, &desc);
    float output[12] = {0};
    const float expected[] = {0, -0.5f, 0, 0, 0, 0.5f, 0, 0.5f, 0, 0, 0, 0};
    TB_TEST_REQUIRE(tb_mixer_voice_active(mixer, voice));
    TB_TEST_EXPECT(!tb_mixer_clip_destroy(clip));
    tb_mixer_render(mixer, output, 2, 2);
    tb_mixer_render(mixer, output + 4, 4, 2);
    for (size_t i = 0; i < TB_TEST_COUNT_OF(output); i++)
    {
        TB_TEST_EXPECT_FLOAT(output[i], ==, expected[i]);
    }
    TB_TEST_EXPECT(!tb_mixer_voice_active(mixer, voice));
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
    tb_mixer_destroy(mixer);
}

static void expect_loop(tb_sound_loop_kind kind, uint32_t count, size_t start, size_t end,
                        const float *expected, size_t frames, float pitch)
{
    const tb_audio_format mono = {.sample_rate = 8000, .channels = 1};
    tb_mixer *const mixer = tb_mixer_create(&mono, 1);
    const float source[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const tb_sound_loop loop = {.start = start, .end = end, .kind = kind, .play_count = count};
    tb_mixer_clip *const clip = make_clip(mixer, source, 5, &loop);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.loop = true;
    desc.pitch = pitch;
    const tb_mixer_voice voice = tb_mixer_play(mixer, clip, &desc);
    TB_TEST_REQUIRE(voice.serial != 0);
    for (size_t i = 0; i < frames; i++)
    {
        float sample = 0.0f;
        tb_mixer_render(mixer, &sample, 1, 1);
        TB_TEST_EXPECT_FLOAT(sample, ==, expected[i]);
    }
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
}

TB_TEST(audio_mixer, sampler_loops_preserve_intro_cycles_and_tail)
{
    const float forward[] = {0, .25f, .5f, .75f, .25f, .5f, .75f, 1, 0};
    const float backward[] = {0, .75f, .5f, .25f, .75f, .5f, .25f, 1, 0};
    const float ping[] = {0, .25f, .5f, .75f, .5f, .25f, .5f, .75f, .5f, 1, 0};
    const float single[] = {0, .25f, .5f, .5f, .5f, .5f};
    const float fractional[] = {0, .125f, .25f, .375f, .5f, .625f, .75f, .5f, .25f};
    expect_loop(TB_SOUND_LOOP_FORWARD, 2, 1, 4, forward, TB_TEST_COUNT_OF(forward), 1);
    expect_loop(TB_SOUND_LOOP_BACKWARD, 2, 1, 4, backward, TB_TEST_COUNT_OF(backward), 1);
    expect_loop(TB_SOUND_LOOP_PING_PONG, 2, 1, 4, ping, TB_TEST_COUNT_OF(ping), 1);
    expect_loop(TB_SOUND_LOOP_PING_PONG, 0, 2, 3, single, TB_TEST_COUNT_OF(single), 1);
    expect_loop(TB_SOUND_LOOP_FORWARD, 0, 1, 4, fractional, TB_TEST_COUNT_OF(fractional), 0.5f);
}

TB_TEST(audio_mixer, crossfades_and_ramps_are_measured_in_output_frames)
{
    const float one[] = {1};
    const float minus[] = {-1};
    tb_mixer *const mixer = tb_mixer_create(&stereo, 2);
    tb_mixer_clip *const a = make_clip(mixer, one, 1, NULL);
    tb_mixer_clip *const b = make_clip(mixer, minus, 1, NULL);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.bus = TB_MIXER_MUSIC;
    desc.loop = true;
    float out[4] = {0};
    const tb_mixer_voice old = tb_mixer_play(mixer, a, &desc);
    tb_mixer_render(mixer, out, 1, 2);
    TB_TEST_EXPECT_FLOAT(out[0], ==, 1);
    const tb_mixer_voice next = tb_mixer_crossfade(mixer, old, b, &desc, 2);
    TB_TEST_REQUIRE(next.serial != 0);
    tb_mixer_render(mixer, out, 2, 2);
    TB_TEST_EXPECT_FLOAT(out[0], ==, 0);
    TB_TEST_EXPECT_FLOAT(out[2], ==, -1);
    TB_TEST_EXPECT(!tb_mixer_voice_active(mixer, old));
    TB_TEST_EXPECT(tb_mixer_clip_destroy(a));
    TB_TEST_REQUIRE(tb_mixer_fade(mixer, next, 0.5f, 0));
    tb_mixer_render(mixer, out, 1, 2);
    TB_TEST_EXPECT_FLOAT(out[0], ==, -0.5f);
    TB_TEST_REQUIRE(tb_mixer_stop(mixer, next, 2));
    tb_mixer_render(mixer, out, 2, 2);
    TB_TEST_EXPECT_FLOAT(out[0], ==, -.25f);
    TB_TEST_EXPECT_FLOAT(out[2], ==, 0);
    TB_TEST_EXPECT(!tb_mixer_voice_active(mixer, next));
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(b));
}

TB_TEST(audio_mixer, voice_stealing_and_queue_pressure_preserve_handles_and_ownership)
{
    const float sample[] = {.75f};
    tb_mixer *const mixer = tb_mixer_create(&stereo, 1);
    tb_mixer_clip *const clip = make_clip(mixer, sample, 1, NULL);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.loop = true;
    const tb_mixer_voice first = tb_mixer_play(mixer, clip, &desc);
    TB_TEST_EXPECT_UINT(tb_mixer_crossfade(mixer, first, clip, &desc, 8).serial, ==, 0);
    tb_mixer_voice latest = tb_mixer_play(mixer, clip, &desc);
    TB_TEST_REQUIRE(latest.serial != 0);
    TB_TEST_EXPECT(!tb_mixer_fade(mixer, first, 1, 0));
    size_t accepted = 0;
    for (size_t i = 0; i < 1024; i++)
    {
        const tb_mixer_voice attempt = tb_mixer_play(mixer, clip, &desc);
        if (attempt.serial == 0)
        {
            break;
        }
        latest = attempt;
        accepted++;
    }
    TB_TEST_EXPECT_UINT(accepted, >, 0);
    TB_TEST_EXPECT_UINT(accepted, <, 1024);
    TB_TEST_EXPECT(tb_mixer_voice_active(mixer, latest));
    tb_mixer_render(mixer, NULL, 0, 2);
    TB_TEST_EXPECT(tb_mixer_stop(mixer, latest, 0));
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
}

TB_TEST(audio_mixer, bus_settings_scale_the_sum_and_master_clips_at_full_scale)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_engine_settings_add(settings, NULL, &engine);
    tb_mixer *const mixer = tb_mixer_create(&stereo, 2);
    const float one[] = {1};
    tb_mixer_clip *const clip = make_clip(mixer, one, 1, NULL);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.loop = true;
    float output[2] = {0};
    (void)tb_mixer_play(mixer, clip, &desc);
    desc.bus = TB_MIXER_INTERFACE;
    (void)tb_mixer_play(mixer, clip, &desc);
    tb_mixer_render(mixer, output, 1, 2);
    TB_TEST_EXPECT_FLOAT(output[0], ==, 1);
    (void)tb_setting_set_number(engine.audio.master_volume, .5);
    (void)tb_setting_set_number(engine.audio.interface_volume, .25);
    (void)tb_setting_set_number(engine.audio.effects_volume, .5);
    (void)tb_setting_set_boolean(engine.audio.mute_when_unfocused, true);
    tb_mixer_apply_settings(mixer, &engine, true);
    tb_mixer_render(mixer, output, 1, 2);
    TB_TEST_EXPECT_FLOAT(output[0], ==, .375f);
    tb_mixer_apply_settings(mixer, &engine, false);
    tb_mixer_render(mixer, output, 1, 2);
    TB_TEST_EXPECT_FLOAT(output[0], ==, 0);
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
    tb_settings_destroy(settings);
}

typedef struct render_worker
{
    tb_mixer *mixer;
    tb_platform_atomic_u32 done;
} render_worker;

static void consume(void *context)
{
    render_worker *const worker = context;
    float out[16] = {0};
    while (tb_platform_atomic_u32_load(&worker->done) == 0)
    {
        tb_mixer_render(worker->mixer, out, 8, 2);
    }
}

TB_TEST(audio_mixer, queued_and_stolen_clips_are_released_after_concurrent_render)
{
    const float sample[] = {.5f};
    const tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    tb_mixer *const mixer = tb_mixer_create(&stereo, 4);
    tb_mixer_clip *const clip = make_clip(mixer, sample, 1, NULL);
    render_worker worker = {.mixer = mixer};
    tb_thread thread = {0};
    tb_thread_create(&thread, consume, &worker);
    for (size_t i = 0; i < 2048; i++)
    {
        while (tb_mixer_play(mixer, clip, &desc).serial == 0)
        {
            tb_thread_yield();
        }
    }
    tb_platform_atomic_u32_store(&worker.done, 1);
    tb_thread_join(&thread);
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
}

TB_TEST(audio_mixer, invalid_requests_preserve_an_existing_voice)
{
    const tb_audio_format invalid = {0};
    const tb_audio_format mono = {.sample_rate = 8000, .channels = 1};
    TB_TEST_EXPECT_POINTER(tb_mixer_create(&invalid, 1), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_mixer_create(&mono, 0), ==, NULL);
    tb_mixer *const mixer = tb_mixer_create(&stereo, 1);
    tb_mixer *const other = tb_mixer_create(&mono, 1);
    const float one[] = {1};
    tb_mixer_clip *const clip = make_clip(mixer, one, 1, NULL);
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_U8, .sample_rate = 8000, .channels = 1};
    tb_sound *empty = NULL;
    tb_mixer_clip *unused = NULL;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 0, NULL, &empty), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_mixer_prepare_clip(mixer, empty, &unused), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_POINTER(unused, ==, NULL);
    tb_sound_destroy(empty);
    tb_mixer_play_desc desc = TB_MIXER_PLAY_INIT;
    desc.loop = true;
    const tb_mixer_voice old = tb_mixer_play(mixer, clip, &desc);
    TB_TEST_EXPECT_UINT(tb_mixer_play(other, clip, &desc).serial, ==, 0);
    desc.loop_index = 3;
    TB_TEST_EXPECT_UINT(tb_mixer_play(mixer, clip, &desc).serial, ==, 0);
    desc.loop_index = 0;
    desc.bus = TB_MIXER_MASTER;
    TB_TEST_EXPECT_UINT(tb_mixer_play(mixer, clip, &desc).serial, ==, 0);
    desc.bus = TB_MIXER_MUSIC;
    desc.pitch = 0;
    TB_TEST_EXPECT_UINT(tb_mixer_play(mixer, clip, &desc).serial, ==, 0);
    desc.pitch = 1;
    desc.pan = -1.0f;
    TB_TEST_EXPECT(!tb_mixer_fade(mixer, old, 2, 0));
    const tb_mixer_voice replacement = tb_mixer_crossfade(mixer, old, clip, &desc, 0);
    TB_TEST_REQUIRE(replacement.serial != 0);
    TB_TEST_EXPECT(!tb_mixer_voice_active(mixer, old));
    float output[2] = {0};
    tb_mixer_render(mixer, output, 1, 2);
    TB_TEST_EXPECT_FLOAT(output[0], ==, 1);
    TB_TEST_EXPECT_FLOAT(output[1], ==, 0);
    tb_mixer_destroy(other);
    tb_mixer_destroy(mixer);
    TB_TEST_EXPECT(tb_mixer_clip_destroy(clip));
    TB_TEST_EXPECT(tb_mixer_clip_destroy(NULL));
    tb_mixer_destroy(NULL);
}
