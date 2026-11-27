#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/math.h"
#include "badger/memory.h"
#include "badger/mixer.h"
#include "sandbox.h"

#define TONE_RATE 48000u
#define TONE_FRAMES (TONE_RATE / 4u)
#define TONE_RAMP (TONE_RATE / 100u)
#define TONE_FREQUENCY 440.0f
#define TONE_PEAK 0.025f

typedef struct audio_scene
{
    tb_audio_system *system;
    tb_audio_device *device;
    tb_mixer *mixer;
    tb_mixer_clip *tone;
    tb_mixer_voice voice;
    bool focused;
    char status[TB_AUDIO_DEVICE_MESSAGE_CAPACITY + 64];
} audio_scene;

static bool prepare_tone(audio_scene *scene)
{
    const tb_sound_spec spec = {
        .format = TB_SOUND_FORMAT_F32, .sample_rate = TONE_RATE, .channels = 1};
    tb_sound *sound = NULL;
    if (tb_sound_create(&spec, TONE_FRAMES, NULL, &sound) != TB_SOUND_OK)
    {
        return false;
    }
    for (size_t i = 0; i < TONE_FRAMES; i++)
    {
        const float phase = 2.0f * TB_PI * TONE_FREQUENCY * (float)i / (float)TONE_RATE;
        tb_sound_set_sample(sound, i, 0, TONE_PEAK * sinf(phase));
    }
    tb_sound_fade(sound, 0, TONE_RAMP, 0.0f, 1.0f);
    tb_sound_fade(sound, TONE_FRAMES - TONE_RAMP, TONE_RAMP, 1.0f, 0.0f);
    const tb_sound_result result = tb_mixer_prepare_clip(scene->mixer, sound, &scene->tone);
    tb_sound_destroy(sound);
    return result == TB_SOUND_OK;
}

bool audio_scene_create(sandbox *box, void **state)
{
    audio_scene *const scene = tb_calloc(1, sizeof(*scene));
    const tb_audio_format format = {.sample_rate = TONE_RATE, .channels = 2};
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
    char device_id[TB_AUDIO_DEVICE_ID_CAPACITY] = {0};
    *state = scene;
    tb_window_state window = {0};
    tb_window_state_of(box->window, &window);
    scene->focused = window.focused;
    scene->mixer = tb_mixer_create(&format, 1);
    if (!prepare_tone(scene))
    {
        return false;
    }
    (void)tb_setting_text(box->engine->audio.output_device, device_id, sizeof(device_id));
    const tb_audio_device_desc desc = {.device_id = device_id,
                                       .format = format,
                                       .render = tb_mixer_render,
                                       .context = scene->mixer,
                                       .stream_name = "Badger sandbox"};
    if (tb_audio_system_open(&scene->system, &error) != TB_AUDIO_DEVICE_OK ||
        tb_audio_device_open(scene->system, &desc, &scene->device, &error) != TB_AUDIO_DEVICE_OK)
    {
        (void)tb_fmt_buffer(scene->status, sizeof(scene->status), "Audio unavailable: %s",
                            error.message);
    }
    else
    {
        tb_audio_system_info info = {0};
        tb_audio_device_latency latency = {0};
        tb_audio_system_get_info(scene->system, &info);
        tb_audio_device_get_latency(scene->device, &latency);
        (void)tb_fmt_buffer(scene->status, sizeof(scene->status),
                            "%s | %u Hz stereo | estimated latency %.1f ms", info.backend,
                            TONE_RATE, latency.seconds * 1000.0);
        tb_mixer_apply_settings(scene->mixer, box->engine, scene->focused);
        tb_audio_device_start(scene->device);
    }
    (void)tb_fmt_print("badger-sandbox: audio: %s\n", scene->status);
    return true;
}

bool audio_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    audio_scene *const scene = state;
    if (event->type == TB_WINDOW_EVENT_FOCUS_GAINED || event->type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        scene->focused = event->type == TB_WINDOW_EVENT_FOCUS_GAINED;
        tb_mixer_apply_settings(scene->mixer, box->engine, scene->focused);
    }
    if (event->type == TB_WINDOW_EVENT_KEY_DOWN && event->key == TB_KEY_SPACE && !event->repeat)
    {
        if (scene->focused && scene->device != NULL &&
            !tb_mixer_voice_active(scene->mixer, scene->voice))
        {
            const tb_mixer_play_desc play = TB_MIXER_PLAY_INIT;
            scene->voice = tb_mixer_play(scene->mixer, scene->tone, &play);
        }
        return true;
    }
    return false;
}

void audio_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    audio_scene *const scene = state;
    tb_audio_device_event event = {0};
    (void)frame;
    tb_mixer_apply_settings(scene->mixer, box->engine, scene->focused);
    while (scene->device != NULL && tb_audio_device_next_event(scene->device, &event))
    {
        if (event.type == TB_AUDIO_DEVICE_EVENT_LOST)
        {
            (void)tb_fmt_buffer(scene->status, sizeof(scene->status), "Audio output disconnected");
        }
        else
        {
            (void)tb_fmt_buffer(scene->status, sizeof(scene->status), "Audio output: %s",
                                event.device_name);
        }
    }
    if (box->fonts[SANDBOX_FONT_BODY] == NULL)
    {
        return;
    }
    const tb_canvas_view view = tb_canvas_view_logical(box->renderer);
    tb_canvas_set_view(box->renderer, &view);
    const float size = box->specs[SANDBOX_FONT_BODY].size;
    const tb_vec4 color = {.9f, .9f, .95f, 1};
    tb_canvas_draw_text(box->renderer, box->fonts[SANDBOX_FONT_BODY],
                        tb_text_from_cstr(scene->status), size, (tb_vec2){24, 32}, color, NULL);
    if (scene->device != NULL)
    {
        tb_canvas_draw_text(
            box->renderer, box->fonts[SANDBOX_FONT_BODY],
            tb_text_from_cstr("Space: play a quiet 440 Hz tone for a quarter second"), size,
            (tb_vec2){24, 32 + size * 1.5f}, color, NULL);
    }
}

void audio_scene_destroy(sandbox *box, void *state)
{
    audio_scene *const scene = state;
    (void)box;
    if (scene == NULL)
    {
        return;
    }
    tb_audio_device_close(scene->device);
    tb_audio_system_close(scene->system);
    tb_mixer_destroy(scene->mixer);
    (void)tb_mixer_clip_destroy(scene->tone);
    tb_free(scene);
}
