/* The PipeWire backend's standard C: a stream's format as an SPA pod, and
 * the default sink's name from its metadata.
 *
 * A pod is a little-endian-of-the-machine sequence of 32-bit words: a
 * header of its size (past the header) and type, then its body, padded to
 * 8 bytes. A format is an object pod whose body is the object's type and
 * the parameter's id, then properties, each a key, flags and a pod of its
 * value. The numbers are those of spa/param/audio/raw.h,
 * spa/param/format.h and spa/utils/type.h, which are part of PipeWire's
 * stable protocol; tests/platform/audio_pipewire_test.c checks the bytes
 * against what spa_format_audio_raw_build() writes. */
#include "audio_pipewire.h"

#include "../shared/audio_device.h"

#include "badger/assert.h"
#include "badger/str.h"

#include <string.h>

/* SPA's types of pods. */
#define TB_SPA_TYPE_ID 3u
#define TB_SPA_TYPE_INT 4u
#define TB_SPA_TYPE_ARRAY 13u
#define TB_SPA_TYPE_OBJECT 15u
#define TB_SPA_TYPE_OBJECT_FORMAT 0x40003u

/* SPA_PARAM_EnumFormat. */
#define TB_SPA_PARAM_ENUM_FORMAT 3u

/* The keys of a format, and their values. */
#define TB_SPA_FORMAT_MEDIA_TYPE 1u
#define TB_SPA_FORMAT_MEDIA_SUBTYPE 2u
#define TB_SPA_FORMAT_AUDIO_FORMAT 0x10001u
#define TB_SPA_FORMAT_AUDIO_RATE 0x10003u
#define TB_SPA_FORMAT_AUDIO_CHANNELS 0x10004u
#define TB_SPA_FORMAT_AUDIO_POSITION 0x10005u
#define TB_SPA_MEDIA_TYPE_AUDIO 1u
#define TB_SPA_MEDIA_SUBTYPE_RAW 1u
/* SPA_AUDIO_FORMAT_F32, in the machine's byte order: F32_LE on every
 * machine the engine supports. */
#define TB_SPA_AUDIO_FORMAT_F32 0x11bu

/* Words of a pod's header, and of a property's key and flags. */
#define TB_SPA_HEADER_WORDS 2u

typedef struct tb_pod_writer
{
    uint32_t *words;
    size_t count;
    size_t capacity;
} tb_pod_writer;

static void tb_pod_word(tb_pod_writer *writer, uint32_t word)
{
    TB_ASSERT(writer->count < writer->capacity);
    writer->words[writer->count] = word;
    writer->count++;
}

/* A property whose value is one word of a type: its key, no flags, a pod of
 * 4 bytes, and the word padded to 8. */
static void tb_pod_property(tb_pod_writer *writer, uint32_t key, uint32_t type, uint32_t value)
{
    tb_pod_word(writer, key);
    tb_pod_word(writer, 0);
    tb_pod_word(writer, sizeof(uint32_t));
    tb_pod_word(writer, type);
    tb_pod_word(writer, value);
    tb_pod_word(writer, 0);
}

uint32_t tb_pipewire_position(uint32_t speaker)
{
    switch (speaker)
    {
    case 0x1u:
        return 3; /* FL */
    case 0x2u:
        return 4; /* FR */
    case 0x4u:
        return 5; /* FC */
    case 0x8u:
        return 6; /* LFE */
    case 0x10u:
        return 12; /* RL: WAVE's back left */
    case 0x20u:
        return 13; /* RR */
    case 0x100u:
        return 11; /* RC */
    case 0x200u:
        return 7; /* SL */
    case 0x400u:
        return 8; /* SR */
    default:
        return 0;
    }
}

size_t tb_pipewire_format_pod(uint32_t rate, uint32_t channels, uint64_t *pod)
{
    tb_pod_writer writer = {(uint32_t *)(void *)pod, 0,
                            TB_PIPEWIRE_FORMAT_POD_CAPACITY / sizeof(uint32_t)};
    const uint32_t mask = tb_audio_device_channel_mask(channels);

    TB_ASSERT(pod != NULL);
    TB_ASSERT(channels >= 1 && channels <= TB_AUDIO_DEVICE_MAX_CHANNELS);
    tb_pod_word(&writer, 0); /* The size, when it is known. */
    tb_pod_word(&writer, TB_SPA_TYPE_OBJECT);
    tb_pod_word(&writer, TB_SPA_TYPE_OBJECT_FORMAT);
    tb_pod_word(&writer, TB_SPA_PARAM_ENUM_FORMAT);
    tb_pod_property(&writer, TB_SPA_FORMAT_MEDIA_TYPE, TB_SPA_TYPE_ID, TB_SPA_MEDIA_TYPE_AUDIO);
    tb_pod_property(&writer, TB_SPA_FORMAT_MEDIA_SUBTYPE, TB_SPA_TYPE_ID, TB_SPA_MEDIA_SUBTYPE_RAW);
    tb_pod_property(&writer, TB_SPA_FORMAT_AUDIO_FORMAT, TB_SPA_TYPE_ID, TB_SPA_AUDIO_FORMAT_F32);
    tb_pod_property(&writer, TB_SPA_FORMAT_AUDIO_RATE, TB_SPA_TYPE_INT, rate);
    tb_pod_property(&writer, TB_SPA_FORMAT_AUDIO_CHANNELS, TB_SPA_TYPE_INT, channels);
    /* The positions: an array pod of an Id child, then a word for each
     * channel, padded to 8 bytes. */
    tb_pod_word(&writer, TB_SPA_FORMAT_AUDIO_POSITION);
    tb_pod_word(&writer, 0);
    tb_pod_word(&writer, (uint32_t)((TB_SPA_HEADER_WORDS + channels) * sizeof(uint32_t)));
    tb_pod_word(&writer, TB_SPA_TYPE_ARRAY);
    tb_pod_word(&writer, sizeof(uint32_t));
    tb_pod_word(&writer, TB_SPA_TYPE_ID);
    for (uint32_t bit = 0; bit < 32u; bit++)
    {
        const uint32_t speaker = 1u << bit;
        if ((mask & speaker) != 0)
        {
            tb_pod_word(&writer, tb_pipewire_position(speaker));
        }
    }
    if ((writer.count % 2u) != 0)
    {
        tb_pod_word(&writer, 0);
    }
    writer.words[0] = (uint32_t)((writer.count - TB_SPA_HEADER_WORDS) * sizeof(uint32_t));
    return writer.count * sizeof(uint32_t);
}

/* Skip the white space of JSON from `at`. */
static const char *tb_json_space(const char *at)
{
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
    {
        at++;
    }
    return at;
}

bool tb_pipewire_default_name(const char *value, char *name, size_t capacity)
{
    static const char key[] = "\"name\"";
    const char *at = NULL;
    size_t length = 0;

    TB_ASSERT(name != NULL);
    TB_ASSERT(capacity > 0);
    name[0] = '\0';
    if (value == NULL)
    {
        return false;
    }
    const size_t found = tb_str_find(value, 0, TB_STR_END, key);
    if (found == TB_STR_NOT_FOUND)
    {
        return false;
    }
    at = tb_json_space(value + found + sizeof(key) - 1);
    if (*at != ':')
    {
        return false;
    }
    at = tb_json_space(at + 1);
    if (*at != '"')
    {
        return false;
    }
    at++;
    /* Node names have no escapes; a name with one is not taken. */
    while (at[length] != '"')
    {
        if (at[length] == '\0' || at[length] == '\\' || length + 1 >= capacity)
        {
            return false;
        }
        length++;
    }
    memcpy(name, at, length);
    name[length] = '\0';
    return true;
}
