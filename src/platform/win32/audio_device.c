/* Windows audio: WASAPI, in shared mode and driven by its event, through
 * COM's C binding.
 *
 * ole32.dll, which creates the device enumerator and frees what COM hands
 * back, is loaded at run time from the system directory, and so is
 * avrt.dll, which puts a stream's thread in MMCSS's "Pro Audio" class; a
 * system without avrt.dll plays on at the thread's own high priority.
 * Nothing of either is linked.
 *
 * Each stream has a thread of its own, which does all of its COM: it opens
 * the endpoint, waits on the event WASAPI sets when the buffer has room,
 * and fills the room with the front's render. An endpoint notification
 * client wakes it when the default output changes or its device goes away,
 * and it opens the new default there, between two buffers, so that the
 * caller sees one format throughout: WASAPI converts the stream's floats to
 * whatever the device mixes in (AUTOCONVERTPCM). */
#include "audio_wasapi.h"

#include "../shared/audio_device.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "badger/utf8.h"

#define COBJMACROS
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <string.h>

/* What WASAPI times in: 100-nanosecond units. */
#define TB_WASAPI_UNITS_PER_SECOND 10000000.0

/* How long a stream with no device waits before it looks for one again,
 * when no notification says that one came: in milliseconds. */
#define TB_WASAPI_RETRY_MILLISECONDS 1000u

/* {BCDE0395-E52F-467C-8E3D-C4579291692E} */
static const GUID tb_wasapi_clsid_enumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
/* {A95664D2-9614-4F35-A746-DE8DB63617E6} */
static const GUID tb_wasapi_iid_enumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
/* {1CB9AD4C-DBFA-4C32-B178-C2F568A703B2} */
static const GUID tb_wasapi_iid_audio_client = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
/* {F294ACFC-3146-4483-A7BF-ADDCA7C260E2} */
static const GUID tb_wasapi_iid_render_client = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
/* {7991EEC9-7E89-4D85-8390-6C703CEC60C0} */
static const GUID tb_wasapi_iid_notification_client = {
    0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
/* {00000000-0000-0000-C000-000000000046} */
static const GUID tb_wasapi_iid_unknown = {
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: {00000003-0000-0010-8000-00AA00389B71} */
static const GUID tb_wasapi_subtype_float = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
/* PKEY_Device_FriendlyName: {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14 */
static const PROPERTYKEY tb_wasapi_key_friendly_name = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

typedef HRESULT(WINAPI tb_co_initialize_ex_function)(LPVOID reserved, DWORD model);
typedef void(WINAPI tb_co_uninitialize_function)(void);
typedef HRESULT(WINAPI tb_co_create_instance_function)(REFCLSID clsid, LPUNKNOWN outer,
                                                       DWORD context, REFIID iid, LPVOID *object);
typedef void(WINAPI tb_co_task_mem_free_function)(LPVOID memory);
typedef HRESULT(WINAPI tb_prop_variant_clear_function)(PROPVARIANT *value);
typedef HANDLE(WINAPI tb_av_set_function)(LPCWSTR task, LPDWORD index);
typedef BOOL(WINAPI tb_av_revert_function)(HANDLE task);

typedef struct tb_wasapi
{
    tb_library *ole32;
    tb_library *avrt; /* NULL where the system has no MMCSS. */
    tb_co_initialize_ex_function *co_initialize_ex;
    tb_co_uninitialize_function *co_uninitialize;
    tb_co_create_instance_function *co_create_instance;
    tb_co_task_mem_free_function *co_task_mem_free;
    tb_prop_variant_clear_function *prop_variant_clear;
    tb_av_set_function *av_set;
    tb_av_revert_function *av_revert;
} tb_wasapi;

typedef struct tb_wasapi_stream tb_wasapi_stream;

/* The endpoint notification client: a COM object of C, whose table is its
 * first member. It lives in its stream, which unregisters it before it
 * goes, so it counts no references. */
typedef struct tb_wasapi_notifier
{
    IMMNotificationClient client;
    tb_wasapi_stream *stream;
} tb_wasapi_notifier;

struct tb_wasapi_stream
{
    tb_wasapi *wasapi;
    tb_audio_device *device;
    tb_thread thread;
    /* Wakes the thread: to quit, or to move to another device. */
    HANDLE wake;
    tb_platform_atomic_u32 quit;
    tb_platform_atomic_u32 reroute;
    /* The device asked for, a copy; NULL for the default. Once that
     * device goes, the stream follows the default, and this is freed. */
    wchar_t *named_id;
    /* The identifier of the endpoint playing now, which the notifications
     * compare with; guarded by `id_mutex`, as they come on a thread of the
     * system's. */
    tb_mutex id_mutex;
    wchar_t *playing_id;
    tb_audio_format format;
    uint32_t period_frames; /* The caller's hint. */
    /* The opening's outcome, which the thread reports through `opened`. */
    tb_semaphore opened;
    tb_audio_device_result open_result;
    tb_audio_device_error *open_error;
    /* The thread's own. */
    tb_wasapi_notifier notifier;
    IMMDeviceEnumerator *enumerator;
    IMMDevice *endpoint;
    IAudioClient *client;
    IAudioRenderClient *render;
    HANDLE buffer_event;
    UINT32 buffer_frames;
};

/* ---------------------------------------------------------------------------
 * What needs no device.
 * ------------------------------------------------------------------------- */

WAVEFORMATEXTENSIBLE tb_wasapi_wave_format(uint32_t rate, uint32_t channels)
{
    WAVEFORMATEXTENSIBLE format = {0};
    const WORD frame_bytes = (WORD)(channels * sizeof(float));

    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = (WORD)channels;
    format.Format.nSamplesPerSec = rate;
    format.Format.nAvgBytesPerSec = rate * frame_bytes;
    format.Format.nBlockAlign = frame_bytes;
    format.Format.wBitsPerSample = 32;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = tb_audio_device_channel_mask(channels);
    format.SubFormat = tb_wasapi_subtype_float;
    return format;
}

bool tb_wasapi_same_id(const wchar_t *left, const wchar_t *right)
{
    size_t index = 0;

    if (left == NULL || right == NULL)
    {
        return left == right;
    }
    while (left[index] == right[index])
    {
        if (left[index] == L'\0')
        {
            return true;
        }
        index++;
    }
    return false;
}

static size_t tb_wasapi_wide_length(const wchar_t *text)
{
    size_t length = 0;

    while (text[length] != L'\0')
    {
        length++;
    }
    return length;
}

void tb_wasapi_text(const wchar_t *source, char *text, size_t capacity)
{
    char converted[TB_AUDIO_DEVICE_ID_CAPACITY * 4] = {0};
    size_t written = 0;

    TB_ASSERT(text != NULL);
    TB_ASSERT(capacity > 0);
    text[0] = '\0';
    if (source == NULL)
    {
        return;
    }
    /* A name longer than the buffer is cut, as any other name is: whole
     * characters, and none of a pair of surrogates split. */
    size_t length = tb_wasapi_wide_length(source);
    if (length > TB_AUDIO_DEVICE_ID_CAPACITY)
    {
        length = TB_AUDIO_DEVICE_ID_CAPACITY;
        if (source[length - 1] >= 0xd800 && source[length - 1] <= 0xdbff)
        {
            length--;
        }
    }
    if (!tb_utf8_from_utf16((const uint16_t *)source, length, converted, sizeof(converted),
                            &written))
    {
        return;
    }
    tb_audio_device_copy_name(text, capacity, converted, written);
}

uint32_t tb_wasapi_frames(LONGLONG duration, uint32_t rate)
{
    const double frames = (double)duration * (double)rate / TB_WASAPI_UNITS_PER_SECOND;

    if (duration <= 0)
    {
        return 0;
    }
    if (frames >= (double)UINT32_MAX)
    {
        return UINT32_MAX;
    }
    return (uint32_t)(frames + 0.5);
}

static LONGLONG tb_wasapi_duration(uint32_t frames, uint32_t rate)
{
    return (LONGLONG)((double)frames * TB_WASAPI_UNITS_PER_SECOND / (double)rate + 0.5);
}

/* ---------------------------------------------------------------------------
 * COM.
 * ------------------------------------------------------------------------- */

/* Join COM on the calling thread, in the multithreaded apartment unless the
 * thread is in another already, where COM serves as well. Returns whether
 * tb_wasapi_com_leave() must leave it. */
static bool tb_wasapi_com_enter(const tb_wasapi *wasapi)
{
    const HRESULT result = wasapi->co_initialize_ex(NULL, COINIT_MULTITHREADED);

    return SUCCEEDED(result);
}

static void tb_wasapi_com_leave(const tb_wasapi *wasapi, bool entered)
{
    if (entered)
    {
        wasapi->co_uninitialize();
    }
}

static IMMDeviceEnumerator *tb_wasapi_enumerator(const tb_wasapi *wasapi)
{
    IMMDeviceEnumerator *enumerator = NULL;
    const HRESULT result =
        wasapi->co_create_instance(&tb_wasapi_clsid_enumerator, NULL, CLSCTX_ALL,
                                   &tb_wasapi_iid_enumerator, (void **)&enumerator);

    return SUCCEEDED(result) ? enumerator : NULL;
}

/* A device's name for a player, into `name`; "" when it has none. */
static void tb_wasapi_endpoint_name(const tb_wasapi *wasapi, IMMDevice *endpoint, char *name,
                                    size_t capacity)
{
    IPropertyStore *store = NULL;
    PROPVARIANT value = {0};

    name[0] = '\0';
    if (FAILED(IMMDevice_OpenPropertyStore(endpoint, STGM_READ, &store)))
    {
        return;
    }
    if (SUCCEEDED(IPropertyStore_GetValue(store, &tb_wasapi_key_friendly_name, &value)))
    {
        if (value.vt == VT_LPWSTR)
        {
            tb_wasapi_text(value.pwszVal, name, capacity);
        }
        (void)wasapi->prop_variant_clear(&value);
    }
    IPropertyStore_Release(store);
}

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

static void tb_wasapi_close(void *native)
{
    tb_wasapi *const wasapi = native;

    tb_library_close(wasapi->avrt);
    tb_library_close(wasapi->ole32);
    tb_free(wasapi);
}

static void tb_wasapi_info(void *native, tb_audio_system_info *info)
{
    (void)native;
    memcpy(info->backend, "WASAPI", sizeof("WASAPI"));
    info->follows_default = true;
    info->reports_changes = true;
    info->knows_default = true;
}

/* coverage-off: listing and playing need an output device, which CI's runners have none of */
static void tb_wasapi_describe(const tb_wasapi *wasapi, IMMDevice *endpoint,
                               const wchar_t *default_id, tb_audio_device_info *info)
{
    LPWSTR id = NULL;
    IAudioClient *client = NULL;
    WAVEFORMATEX *mix = NULL;

    if (SUCCEEDED(IMMDevice_GetId(endpoint, &id)))
    {
        tb_wasapi_text(id, info->id, sizeof(info->id));
        info->is_default = tb_wasapi_same_id(id, default_id);
        wasapi->co_task_mem_free(id);
    }
    tb_wasapi_endpoint_name(wasapi, endpoint, info->name, sizeof(info->name));
    if (SUCCEEDED(IMMDevice_Activate(endpoint, &tb_wasapi_iid_audio_client, CLSCTX_ALL, NULL,
                                     (void **)&client)))
    {
        if (SUCCEEDED(IAudioClient_GetMixFormat(client, &mix)))
        {
            info->sample_rate = mix->nSamplesPerSec;
            info->channels = mix->nChannels;
            wasapi->co_task_mem_free(mix);
        }
        IAudioClient_Release(client);
    }
}

static size_t tb_wasapi_list(void *native, tb_audio_device_info *devices, size_t capacity)
{
    const tb_wasapi *const wasapi = native;
    const bool entered = tb_wasapi_com_enter(wasapi);
    IMMDeviceEnumerator *const enumerator = tb_wasapi_enumerator(wasapi);
    IMMDeviceCollection *collection = NULL;
    IMMDevice *default_endpoint = NULL;
    LPWSTR default_id = NULL;
    UINT count = 0;

    if (enumerator == NULL)
    {
        goto cleanup;
    }
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole,
                                                              &default_endpoint)))
    {
        (void)IMMDevice_GetId(default_endpoint, &default_id);
        IMMDevice_Release(default_endpoint);
    }
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE,
                                                      &collection)) ||
        FAILED(IMMDeviceCollection_GetCount(collection, &count)))
    {
        count = 0;
        goto cleanup;
    }
    for (UINT index = 0; index < count && index < capacity; index++)
    {
        IMMDevice *endpoint = NULL;
        if (SUCCEEDED(IMMDeviceCollection_Item(collection, index, &endpoint)))
        {
            tb_wasapi_describe(wasapi, endpoint, default_id, &devices[index]);
            IMMDevice_Release(endpoint);
        }
    }

cleanup:
    if (collection != NULL)
    {
        IMMDeviceCollection_Release(collection);
    }
    if (default_id != NULL)
    {
        wasapi->co_task_mem_free(default_id);
    }
    if (enumerator != NULL)
    {
        IMMDeviceEnumerator_Release(enumerator);
    }
    tb_wasapi_com_leave(wasapi, entered);
    return count;
}

/* ---------------------------------------------------------------------------
 * Notifications, on a thread of the system's.
 * ------------------------------------------------------------------------- */

static tb_wasapi_stream *tb_wasapi_notified(IMMNotificationClient *client)
{
    return ((tb_wasapi_notifier *)client)->stream;
}

static void tb_wasapi_wake_to_reroute(tb_wasapi_stream *stream)
{
    tb_platform_atomic_u32_store(&stream->reroute, 1u);
    (void)SetEvent(stream->wake);
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_query_interface(IMMNotificationClient *client,
                                                           REFIID iid, void **object)
{
    if (IsEqualIID(iid, &tb_wasapi_iid_unknown) ||
        IsEqualIID(iid, &tb_wasapi_iid_notification_client))
    {
        *object = client;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE tb_wasapi_add_ref(IMMNotificationClient *client)
{
    (void)client;
    return 1;
}

static ULONG STDMETHODCALLTYPE tb_wasapi_release(IMMNotificationClient *client)
{
    (void)client;
    return 1;
}

/* A device that went or came: the stream moves when the one it plays on
 * went, and looks again when it has none. */
static void tb_wasapi_device_changed(IMMNotificationClient *client, LPCWSTR id, bool gone)
{
    tb_wasapi_stream *const stream = tb_wasapi_notified(client);
    bool silent = false;
    bool playing = false;

    tb_mutex_lock(&stream->id_mutex);
    silent = stream->playing_id == NULL;
    playing = tb_wasapi_same_id(stream->playing_id, id);
    tb_mutex_unlock(&stream->id_mutex);
    if (silent || (playing && gone))
    {
        tb_wasapi_wake_to_reroute(stream);
    }
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_state_changed(IMMNotificationClient *client, LPCWSTR id,
                                                         DWORD state)
{
    tb_wasapi_device_changed(client, id, state != DEVICE_STATE_ACTIVE);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_added(IMMNotificationClient *client, LPCWSTR id)
{
    tb_wasapi_device_changed(client, id, false);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_removed(IMMNotificationClient *client, LPCWSTR id)
{
    tb_wasapi_device_changed(client, id, true);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_default_changed(IMMNotificationClient *client,
                                                           EDataFlow flow, ERole role, LPCWSTR id)
{
    tb_wasapi_stream *const stream = tb_wasapi_notified(client);
    bool follows = false;

    (void)id;
    tb_mutex_lock(&stream->id_mutex);
    follows = stream->named_id == NULL;
    tb_mutex_unlock(&stream->id_mutex);
    if (flow == eRender && role == eConsole && follows)
    {
        tb_wasapi_wake_to_reroute(stream);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tb_wasapi_property_changed(IMMNotificationClient *client,
                                                            LPCWSTR id, const PROPERTYKEY key)
{
    (void)client;
    (void)id;
    (void)key;
    return S_OK;
}

static IMMNotificationClientVtbl tb_wasapi_notifier_table = {
    tb_wasapi_query_interface, tb_wasapi_add_ref,         tb_wasapi_release,
    tb_wasapi_state_changed,   tb_wasapi_added,           tb_wasapi_removed,
    tb_wasapi_default_changed, tb_wasapi_property_changed};

/* ---------------------------------------------------------------------------
 * A stream, on its own thread.
 * ------------------------------------------------------------------------- */

static void tb_wasapi_set_playing_id(tb_wasapi_stream *stream, wchar_t *id)
{
    wchar_t *previous = NULL;

    tb_mutex_lock(&stream->id_mutex);
    previous = stream->playing_id;
    stream->playing_id = id;
    tb_mutex_unlock(&stream->id_mutex);
    if (previous != NULL)
    {
        stream->wasapi->co_task_mem_free(previous);
    }
}

static void tb_wasapi_disconnect(tb_wasapi_stream *stream)
{
    if (stream->client != NULL)
    {
        (void)IAudioClient_Stop(stream->client);
    }
    if (stream->render != NULL)
    {
        IAudioRenderClient_Release(stream->render);
        stream->render = NULL;
    }
    if (stream->client != NULL)
    {
        IAudioClient_Release(stream->client);
        stream->client = NULL;
    }
    if (stream->endpoint != NULL)
    {
        IMMDevice_Release(stream->endpoint);
        stream->endpoint = NULL;
    }
    tb_wasapi_set_playing_id(stream, NULL);
}

/* Fill the room the buffer has. Returns whether the device is still there. */
static bool tb_wasapi_fill(tb_wasapi_stream *stream)
{
    UINT32 padding = 0;
    BYTE *data = NULL;
    HRESULT result = IAudioClient_GetCurrentPadding(stream->client, &padding);

    if (FAILED(result))
    {
        return false;
    }
    const UINT32 room = stream->buffer_frames - padding;
    if (room == 0)
    {
        return true;
    }
    result = IAudioRenderClient_GetBuffer(stream->render, room, &data);
    if (FAILED(result))
    {
        return false;
    }
    tb_audio_device_render(stream->device, (float *)data, room, stream->format.channels);
    return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(stream->render, room, 0));
}

/* The endpoint to play on: the device asked for while it is there, else the
 * default. */
static tb_audio_device_result tb_wasapi_find_endpoint(tb_wasapi_stream *stream,
                                                      tb_audio_device_error *error)
{
    HRESULT result = S_OK;

    if (stream->named_id != NULL)
    {
        DWORD state = 0;
        result =
            IMMDeviceEnumerator_GetDevice(stream->enumerator, stream->named_id, &stream->endpoint);
        if (SUCCEEDED(result) && SUCCEEDED(IMMDevice_GetState(stream->endpoint, &state)) &&
            state == DEVICE_STATE_ACTIVE)
        {
            return TB_AUDIO_DEVICE_OK;
        }
        if (stream->endpoint != NULL)
        {
            IMMDevice_Release(stream->endpoint);
            stream->endpoint = NULL;
        }
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_NOT_FOUND,
                                    "WASAPI: no output device has that identifier");
    }
    result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(stream->enumerator, eRender, eConsole,
                                                         &stream->endpoint);
    if (FAILED(result))
    {
        stream->endpoint = NULL;
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                    "WASAPI: no output device (0x%08lx)", (unsigned long)result);
    }
    return TB_AUDIO_DEVICE_OK;
}

/* Open the client on the endpoint found, in the stream's format, and start
 * it with a buffer of silence or of the render's samples. */
static tb_audio_device_result tb_wasapi_start_client(tb_wasapi_stream *stream,
                                                     tb_audio_device_error *error)
{
    const tb_wasapi *const wasapi = stream->wasapi;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    WAVEFORMATEX *mix = NULL;
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    REFERENCE_TIME stream_latency = 0;
    LPWSTR id = NULL;
    HRESULT result = IMMDevice_Activate(stream->endpoint, &tb_wasapi_iid_audio_client, CLSCTX_ALL,
                                        NULL, (void **)&stream->client);

    if (FAILED(result))
    {
        stream->client = NULL;
        goto failed;
    }
    result = IAudioClient_GetMixFormat(stream->client, &mix);
    if (FAILED(result))
    {
        goto failed;
    }
    if (stream->format.sample_rate == 0)
    {
        stream->format.sample_rate = mix->nSamplesPerSec;
    }
    if (stream->format.channels == 0)
    {
        stream->format.channels = mix->nChannels < TB_AUDIO_DEVICE_MAX_CHANNELS
                                      ? mix->nChannels
                                      : TB_AUDIO_DEVICE_MAX_CHANNELS;
    }
    wasapi->co_task_mem_free(mix);
    const WAVEFORMATEXTENSIBLE format =
        tb_wasapi_wave_format(stream->format.sample_rate, stream->format.channels);
    const REFERENCE_TIME requested =
        stream->period_frames == 0
            ? 0
            : tb_wasapi_duration(stream->period_frames, stream->format.sample_rate);
    result = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED, flags, requested, 0,
                                     &format.Format, NULL);
    if (FAILED(result))
    {
        goto failed;
    }
    result = IAudioClient_SetEventHandle(stream->client, stream->buffer_event);
    if (FAILED(result))
    {
        goto failed;
    }
    result = IAudioClient_GetBufferSize(stream->client, &stream->buffer_frames);
    if (FAILED(result))
    {
        goto failed;
    }
    result = IAudioClient_GetService(stream->client, &tb_wasapi_iid_render_client,
                                     (void **)&stream->render);
    if (FAILED(result))
    {
        stream->render = NULL;
        goto failed;
    }
    (void)IAudioClient_GetDevicePeriod(stream->client, &default_period, &minimum_period);
    (void)IAudioClient_GetStreamLatency(stream->client, &stream_latency);
    const uint32_t rate = stream->format.sample_rate;
    const tb_audio_device_latency latency =
        tb_audio_device_latency_of(tb_wasapi_frames(default_period, rate), stream->buffer_frames,
                                   rate, (double)stream_latency / TB_WASAPI_UNITS_PER_SECOND);
    tb_audio_device_set_latency(stream->device, &latency);
    if (!tb_wasapi_fill(stream))
    {
        result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto failed;
    }
    result = IAudioClient_Start(stream->client);
    if (FAILED(result))
    {
        goto failed;
    }
    if (SUCCEEDED(IMMDevice_GetId(stream->endpoint, &id)))
    {
        tb_wasapi_set_playing_id(stream, id);
    }
    return TB_AUDIO_DEVICE_OK;

failed:
    tb_wasapi_disconnect(stream);
    return tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                "WASAPI: the output device refused the stream (0x%08lx)",
                                (unsigned long)result);
}

static tb_audio_device_result tb_wasapi_connect(tb_wasapi_stream *stream,
                                                tb_audio_device_error *error)
{
    const tb_audio_device_result result = tb_wasapi_find_endpoint(stream, error);

    if (result != TB_AUDIO_DEVICE_OK)
    {
        return result;
    }
    return tb_wasapi_start_client(stream, error);
}

/* Move to the device the stream should play on now, and say where. */
static void tb_wasapi_reconnect(tb_wasapi_stream *stream, bool *lost)
{
    char name[TB_AUDIO_DEVICE_NAME_CAPACITY] = {0};

    tb_wasapi_disconnect(stream);
    tb_audio_device_result result = tb_wasapi_connect(stream, NULL);
    if (result == TB_AUDIO_DEVICE_NOT_FOUND)
    {
        /* The device asked for went: from now on the stream follows the
         * default. */
        tb_mutex_lock(&stream->id_mutex);
        tb_free(stream->named_id);
        stream->named_id = NULL;
        tb_mutex_unlock(&stream->id_mutex);
        result = tb_wasapi_connect(stream, NULL);
    }
    if (result != TB_AUDIO_DEVICE_OK)
    {
        if (!*lost)
        {
            tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
        }
        *lost = true;
        return;
    }
    *lost = false;
    tb_wasapi_endpoint_name(stream->wasapi, stream->endpoint, name, sizeof(name));
    tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED, name);
}

static void tb_wasapi_serve(tb_wasapi_stream *stream)
{
    bool lost = false;

    while (tb_platform_atomic_u32_load(&stream->quit) == 0u)
    {
        const HANDLE handles[2] = {stream->wake, stream->buffer_event};
        const DWORD count = stream->client != NULL ? 2u : 1u;
        const DWORD timeout = stream->client != NULL ? INFINITE : TB_WASAPI_RETRY_MILLISECONDS;
        const DWORD woken = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (tb_platform_atomic_u32_load(&stream->quit) != 0u)
        {
            break;
        }
        const bool reroute = tb_platform_atomic_u32_exchange(&stream->reroute, 0u) != 0u;
        if (reroute || stream->client == NULL)
        {
            tb_wasapi_reconnect(stream, &lost);
            continue;
        }
        if (woken == WAIT_OBJECT_0 + 1u && !tb_wasapi_fill(stream))
        {
            /* The device went or changed under the stream. */
            tb_wasapi_reconnect(stream, &lost);
        }
    }
}

static void tb_wasapi_stream_main(void *argument)
{
    tb_wasapi_stream *const stream = argument;
    const tb_wasapi *const wasapi = stream->wasapi;
    const bool entered = tb_wasapi_com_enter(wasapi);
    HANDLE task = NULL;
    DWORD task_index = 0;
    bool registered = false;

    if (wasapi->av_set != NULL)
    {
        task = wasapi->av_set(L"Pro Audio", &task_index);
    }
    stream->enumerator = tb_wasapi_enumerator(wasapi);
    if (stream->enumerator == NULL)
    {
        stream->open_result =
            tb_audio_device_fail(stream->open_error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                 "WASAPI: the device enumerator could not be made");
        tb_semaphore_signal(&stream->opened, 1);
        goto cleanup;
    }
    stream->notifier.client.lpVtbl = &tb_wasapi_notifier_table;
    stream->notifier.stream = stream;
    registered = SUCCEEDED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(
        stream->enumerator, &stream->notifier.client));
    stream->open_result = tb_wasapi_connect(stream, stream->open_error);
    tb_semaphore_signal(&stream->opened, 1);
    if (stream->open_result != TB_AUDIO_DEVICE_OK)
    {
        goto cleanup;
    }
    tb_wasapi_serve(stream);

cleanup:
    tb_wasapi_disconnect(stream);
    if (registered)
    {
        (void)IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(stream->enumerator,
                                                                         &stream->notifier.client);
    }
    if (stream->enumerator != NULL)
    {
        IMMDeviceEnumerator_Release(stream->enumerator);
        stream->enumerator = NULL;
    }
    if (task != NULL)
    {
        (void)wasapi->av_revert(task);
    }
    tb_wasapi_com_leave(wasapi, entered);
}

static void tb_wasapi_free_stream(tb_wasapi_stream *stream)
{
    if (stream->wake != NULL)
    {
        (void)CloseHandle(stream->wake);
    }
    if (stream->buffer_event != NULL)
    {
        (void)CloseHandle(stream->buffer_event);
    }
    tb_free(stream->named_id);
    tb_free(stream);
}

/* A copy of a device's identifier, in UTF-16; NULL for "". */
static wchar_t *tb_wasapi_copy_id(const char *id)
{
    size_t units = 0;
    const size_t length = tb_str_length(id);
    wchar_t *copy = NULL;

    if (length == 0 || !tb_utf8_to_utf16(id, length, NULL, 0, &units))
    {
        return NULL;
    }
    copy = tb_calloc(units + 1, sizeof(*copy));
    (void)tb_utf8_to_utf16(id, length, (uint16_t *)copy, units, &units);
    return copy;
}

static tb_audio_device_result tb_wasapi_open(void *native, tb_audio_device_request *request,
                                             tb_audio_device *device, void **opened,
                                             tb_audio_device_error *error)
{
    tb_wasapi_stream *const stream = tb_calloc(1, sizeof(*stream));
    const tb_thread_options options = {.name = "audio device", .priority = TB_THREAD_PRIORITY_HIGH};

    stream->wasapi = native;
    stream->device = device;
    stream->format = request->format;
    stream->period_frames = request->period_frames;
    stream->open_error = error;
    if (request->device_id[0] != '\0')
    {
        stream->named_id = tb_wasapi_copy_id(request->device_id);
        if (stream->named_id == NULL)
        {
            tb_wasapi_free_stream(stream);
            return tb_audio_device_fail(error, TB_AUDIO_DEVICE_NOT_FOUND,
                                        "WASAPI: '%s' is not a device's identifier",
                                        request->device_id);
        }
    }
    stream->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    stream->buffer_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (stream->wake == NULL || stream->buffer_event == NULL)
    {
        tb_wasapi_free_stream(stream);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                    "WASAPI: no event for the stream");
    }
    tb_thread_create_with_options(&stream->thread, &options, tb_wasapi_stream_main, stream);
    tb_semaphore_wait(&stream->opened);
    stream->open_error = NULL;
    if (stream->open_result != TB_AUDIO_DEVICE_OK)
    {
        tb_thread_join(&stream->thread);
        const tb_audio_device_result result = stream->open_result;
        tb_wasapi_free_stream(stream);
        return result;
    }
    request->format = stream->format;
    *opened = stream;
    return TB_AUDIO_DEVICE_OK;
}

static void tb_wasapi_close_stream(void *native, void *opened)
{
    tb_wasapi_stream *const stream = opened;

    (void)native;
    tb_platform_atomic_u32_store(&stream->quit, 1u);
    (void)SetEvent(stream->wake);
    tb_thread_join(&stream->thread);
    tb_wasapi_free_stream(stream);
}
/* coverage-on */

static const tb_audio_system_ops tb_wasapi_ops = {tb_wasapi_close, tb_wasapi_info, tb_wasapi_list,
                                                  tb_wasapi_open, tb_wasapi_close_stream};

tb_audio_device_result tb_native_audio_open(const tb_audio_system_ops **ops, void **native,
                                            tb_audio_device_error *error)
{
    tb_wasapi *const wasapi = tb_calloc(1, sizeof(*wasapi));
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    const tb_library_binding ole32[] = {
        {"CoInitializeEx", (tb_library_function *)&wasapi->co_initialize_ex, true},
        {"CoUninitialize", (tb_library_function *)&wasapi->co_uninitialize, true},
        {"CoCreateInstance", (tb_library_function *)&wasapi->co_create_instance, true},
        {"CoTaskMemFree", (tb_library_function *)&wasapi->co_task_mem_free, true},
        {"PropVariantClear", (tb_library_function *)&wasapi->prop_variant_clear, true},
    };
    const tb_library_binding avrt[] = {
        {"AvSetMmThreadCharacteristicsW", (tb_library_function *)&wasapi->av_set, true},
        {"AvRevertMmThreadCharacteristics", (tb_library_function *)&wasapi->av_revert, true},
    };

    if (tb_library_open("ole32.dll", TB_LIBRARY_SEARCH_SYSTEM, &wasapi->ole32, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(wasapi->ole32, ole32, sizeof(ole32) / sizeof(ole32[0]), &library_error) !=
            TB_LIBRARY_OK)
    {
        /* coverage-off: ole32.dll is part of every Windows */
        tb_wasapi_close(wasapi);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "WASAPI: %s",
                                    library_error.message);
        /* coverage-on */
    }
    if (tb_library_open("avrt.dll", TB_LIBRARY_SEARCH_SYSTEM, &wasapi->avrt, NULL) ==
            TB_LIBRARY_OK &&
        tb_library_bind(wasapi->avrt, avrt, sizeof(avrt) / sizeof(avrt[0]), NULL) != TB_LIBRARY_OK)
    {
        /* coverage-off: avrt.dll is part of every Windows since Vista */
        tb_library_close(wasapi->avrt);
        wasapi->avrt = NULL;
        wasapi->av_set = NULL;
        wasapi->av_revert = NULL;
        /* coverage-on */
    }
    *ops = &tb_wasapi_ops;
    *native = wasapi;
    return TB_AUDIO_DEVICE_OK;
}
