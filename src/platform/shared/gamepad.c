/* The front of the gamepad interface, the same on every platform: the
 * slots and their events, rumble's time and strength, the focus, and the
 * contracts. Each platform's backend reads the pads through its table. */
#define _CRT_SECURE_NO_WARNINGS /* getenv returns borrowed storage by design. */
#include "gamepad.h"

#include "badger/array.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/utf8.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The longest rumble: a longer one is cut to it, which keeps its end in
 * the clock's range. */
#define TB_GAMEPAD_LONGEST_RUMBLE_SECONDS 3600.0

#define TB_GAMEPAD_NANOSECONDS_PER_SECOND 1e9

/* What Steam sets to 1 for the games it starts on a Steam Deck. */
#define TB_GAMEPAD_STEAM_DECK_VARIABLE "SteamDeck"

/* A slot's rumble, as the game asked for it before the vibration scaled it. */
typedef struct tb_gamepad_motors
{
    bool running;
    float low_frequency;
    float high_frequency;
    uint64_t until; /* The clock's nanoseconds at which it stops. */
} tb_gamepad_motors;

struct tb_gamepad_system
{
    const tb_gamepad_ops *ops;
    void *native;
    tb_gamepad_clock *clock;
    tb_gamepad_pad pads[TB_GAMEPAD_SLOT_COUNT];
    uint64_t seen[TB_GAMEPAD_SLOT_COUNT]; /* Each slot's serial as the last poll saw it. */
    tb_gamepad_motors motors[TB_GAMEPAD_SLOT_COUNT];
    tb_array *events; /* tb_gamepad_event, a queue from `next_event`. */
    size_t next_event;
    float vibration;
    bool focused;
    bool steam_deck; /* It runs on a Steam Deck, for its pads' families. */
};

tb_gamepad_result tb_gamepad_fail(tb_gamepad_error *error, tb_gamepad_result result,
                                  const char *format, ...)
{
    va_list arguments;

    if (error == NULL)
    {
        return result;
    }
    error->result = result;
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    return result;
}

/* ---------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

static float tb_gamepad_clamp(float value, float lowest, float highest)
{
    if (value < lowest)
    {
        return lowest;
    }
    if (value > highest)
    {
        return highest;
    }
    return value;
}

float tb_gamepad_stick_value(int32_t value, int32_t minimum, int32_t maximum, bool negate)
{
    const int64_t low = minimum;
    const int64_t high = maximum;
    const int64_t center = low + (high - low + 1) / 2;
    const int64_t offset = (int64_t)value - center;
    double scaled = 0.0;

    if (high <= low)
    {
        return 0.0f;
    }
    if (offset >= 0)
    {
        scaled = high > center ? (double)offset / (double)(high - center) : 0.0;
    }
    else
    {
        scaled = (double)offset / (double)(center - low);
    }
    if (negate)
    {
        scaled = -scaled;
    }
    return tb_gamepad_clamp((float)scaled, -1.0f, 1.0f);
}

float tb_gamepad_trigger_value(int32_t value, int32_t minimum, int32_t maximum)
{
    const int64_t low = minimum;
    const int64_t high = maximum;

    if (high <= low)
    {
        return 0.0f;
    }
    return tb_gamepad_clamp((float)((double)((int64_t)value - low) / (double)(high - low)), 0.0f,
                            1.0f);
}

void tb_gamepad_copy_name(char *name, const char *source, size_t length)
{
    size_t written = 0;
    size_t offset = 0;

    TB_ASSERT(name != NULL);
    TB_ASSERT(source != NULL || length == 0);
    while (offset < length && source[offset] != '\0')
    {
        uint32_t scalar = 0;
        size_t next = offset + 1;
        size_t size = 1;
        const bool valid = tb_utf8_decode(source, length, offset, &scalar, &next);
        if (valid)
        {
            size = next - offset;
        }
        if (written + size >= TB_GAMEPAD_NAME_CAPACITY)
        {
            break;
        }
        if (valid)
        {
            memcpy(name + written, source + offset, size);
        }
        else
        {
            name[written] = '?';
        }
        written += size;
        offset = valid ? next : offset + 1;
    }
    name[written] = '\0';
}

/* ---------------------------------------------------------------------------
 * Families.
 * ------------------------------------------------------------------------- */

#define TB_GAMEPAD_STEAM_VENDOR 0x28deu
#define TB_GAMEPAD_STEAM_VIRTUAL_PRODUCT 0x11ffu

/* Whether `text` holds `word`, ignoring the case of ASCII letters; `word`
 * is lowercase. */
static bool tb_gamepad_has_word(const char *text, const char *word)
{
    const size_t length = strlen(word);

    for (const char *at = text; *at != '\0'; at++)
    {
        size_t matched = 0;
        while (matched < length && at[matched] != '\0')
        {
            const char upper = at[matched];
            const char lower = upper >= 'A' && upper <= 'Z' ? (char)(upper - 'A' + 'a') : upper;
            if (lower != word[matched])
            {
                break;
            }
            matched++;
        }
        if (matched == length)
        {
            return true;
        }
    }
    return false;
}

/* The words that name a family, the Steam Deck's first: "Steam Deck
 * Controller" is no Xbox pad, whatever else its name says. */
typedef struct tb_gamepad_family_word
{
    const char *word;
    tb_gamepad_family family;
} tb_gamepad_family_word;

static const tb_gamepad_family_word tb_gamepad_family_words[] = {
    {"steam deck", TB_GAMEPAD_FAMILY_STEAM_DECK},   {"xbox", TB_GAMEPAD_FAMILY_XBOX},
    {"dualsense", TB_GAMEPAD_FAMILY_PLAYSTATION},   {"dualshock", TB_GAMEPAD_FAMILY_PLAYSTATION},
    {"playstation", TB_GAMEPAD_FAMILY_PLAYSTATION}, {"nintendo", TB_GAMEPAD_FAMILY_NINTENDO},
    {"switch", TB_GAMEPAD_FAMILY_NINTENDO},         {"joy-con", TB_GAMEPAD_FAMILY_NINTENDO},
};

tb_gamepad_family tb_gamepad_family_from_name(const char *text)
{
    if (text == NULL)
    {
        return TB_GAMEPAD_FAMILY_GENERIC;
    }
    for (size_t index = 0;
         index < sizeof(tb_gamepad_family_words) / sizeof(tb_gamepad_family_words[0]); index++)
    {
        if (tb_gamepad_has_word(text, tb_gamepad_family_words[index].word))
        {
            return tb_gamepad_family_words[index].family;
        }
    }
    return TB_GAMEPAD_FAMILY_GENERIC;
}

tb_gamepad_family tb_gamepad_settle_family(const tb_gamepad_info *info, bool steam_deck)
{
    const bool steam_virtual = info->vendor == TB_GAMEPAD_STEAM_VENDOR &&
                               info->product == TB_GAMEPAD_STEAM_VIRTUAL_PRODUCT;
    const bool without_ids = info->vendor == 0 && info->product == 0;

    if (steam_deck && (steam_virtual || without_ids))
    {
        return TB_GAMEPAD_FAMILY_STEAM_DECK;
    }
    if (info->family != TB_GAMEPAD_FAMILY_GENERIC)
    {
        return info->family;
    }
    return tb_gamepad_family_from_usb(info->vendor, info->product);
}

bool tb_gamepad_steam_deck_variable(const char *value)
{
    return value != NULL && strcmp(value, "1") == 0;
}

/* One hexadecimal number of an entry, with or without 0x; `*end` is where
 * it ended, which is `text` when there was none. */
static unsigned long tb_gamepad_hex(const char *text, const char **end)
{
    char *stop = NULL;
    const unsigned long value = strtoul(text, &stop, 16);

    *end = stop;
    return value;
}

bool tb_gamepad_ignored(const char *list, uint16_t vendor, uint16_t product)
{
    const char *at = list;

    if (list == NULL)
    {
        return false;
    }
    while (*at != '\0')
    {
        const char *end = NULL;
        const unsigned long entry_vendor = tb_gamepad_hex(at, &end);
        unsigned long entry_product = 0;
        bool whole = end != at && *end == '/';
        if (whole)
        {
            at = end + 1;
            entry_product = tb_gamepad_hex(at, &end);
            whole = end != at;
        }
        if (whole && entry_vendor == vendor && entry_product == product)
        {
            return true;
        }
        while (*end != '\0' && *end != ',')
        {
            end++;
        }
        at = *end == ',' ? end + 1 : end;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

tb_gamepad_system *tb_gamepad_system_make(const tb_gamepad_ops *ops, void *native,
                                          tb_gamepad_clock *clock)
{
    tb_gamepad_system *const system = tb_calloc(1, sizeof(*system));

    TB_ASSERT(ops != NULL);
    TB_ASSERT(clock != NULL);
    system->ops = ops;
    system->native = native;
    system->clock = clock;
    system->events = TB_ARRAY_CREATE(NULL, tb_gamepad_event);
    system->vibration = 1.0f;
    system->focused = true;
    return system;
}

tb_gamepad_result tb_gamepad_system_open(tb_gamepad_system **system, tb_gamepad_error *error)
{
    const tb_gamepad_ops *ops = NULL;
    void *native = NULL;
    tb_gamepad_result result = TB_GAMEPAD_OK;

    TB_ASSERT(system != NULL);
    *system = NULL;
    if (error != NULL)
    {
        *error = (tb_gamepad_error)TB_GAMEPAD_ERROR_INIT;
    }
    result = tb_native_gamepad_open(&ops, &native, error);
    if (result != TB_GAMEPAD_OK)
    {
        return result;
    }
    *system = tb_gamepad_system_make(ops, native, tb_platform_clock_nanoseconds);
    (*system)->steam_deck = tb_gamepad_steam_deck_variable(getenv(TB_GAMEPAD_STEAM_DECK_VARIABLE));
    return TB_GAMEPAD_OK;
}

static void tb_gamepad_stop(tb_gamepad_system *system, size_t slot)
{
    tb_gamepad_motors *const motors = &system->motors[slot];

    if (!motors->running)
    {
        return;
    }
    *motors = (tb_gamepad_motors){0};
    if (system->pads[slot].serial != 0)
    {
        system->ops->rumble(system->native, slot, 0.0f, 0.0f, 0.0);
    }
}

static void tb_gamepad_stop_all(tb_gamepad_system *system)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gamepad_stop(system, slot);
    }
}

void tb_gamepad_system_close(tb_gamepad_system *system)
{
    if (system == NULL)
    {
        return;
    }
    tb_gamepad_stop_all(system);
    system->ops->close(system->native);
    tb_array_destroy(system->events);
    tb_free(system);
}

static void tb_gamepad_queue(tb_gamepad_system *system, tb_gamepad_event_type type, size_t slot)
{
    const tb_gamepad_event event = {type, slot};

    (void)tb_array_push(system->events, &event);
}

/* What the backend's poll changed of a slot: a pad that went, whose rumble
 * went with it, and a pad that came. */
static void tb_gamepad_compare(tb_gamepad_system *system, size_t slot)
{
    tb_gamepad_pad *const pad = &system->pads[slot];
    const uint64_t was = system->seen[slot];

    if (pad->serial == 0)
    {
        *pad = (tb_gamepad_pad){0};
    }
    if (was == pad->serial)
    {
        return;
    }
    if (was != 0)
    {
        system->motors[slot] = (tb_gamepad_motors){0};
        tb_gamepad_queue(system, TB_GAMEPAD_EVENT_DISCONNECTED, slot);
    }
    if (pad->serial != 0)
    {
        pad->state.connected = true;
        tb_gamepad_queue(system, TB_GAMEPAD_EVENT_CONNECTED, slot);
        if (system->ops->player != NULL)
        {
            system->ops->player(system->native, slot, slot);
        }
    }
    system->seen[slot] = pad->serial;
}

void tb_gamepad_system_poll(tb_gamepad_system *system)
{
    uint64_t now = 0;

    TB_ASSERT(system != NULL);
    now = system->clock();
    system->ops->poll(system->native, now, system->pads);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gamepad_compare(system, slot);
        if (system->motors[slot].running && now >= system->motors[slot].until)
        {
            tb_gamepad_stop(system, slot);
        }
    }
}

bool tb_gamepad_system_next_event(tb_gamepad_system *system, tb_gamepad_event *event)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(event != NULL);
    if (system->next_event == tb_array_length(system->events))
    {
        return false;
    }
    *event = *TB_ARRAY_AT(system->events, tb_gamepad_event, system->next_event);
    system->next_event++;
    if (system->next_event == tb_array_length(system->events))
    {
        tb_array_clear(system->events);
        system->next_event = 0;
    }
    return true;
}

void tb_gamepad_get_state(const tb_gamepad_system *system, size_t slot, tb_gamepad_state *state)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(state != NULL);
    TB_ASSERT_MSG(slot < TB_GAMEPAD_SLOT_COUNT, "a slot below TB_GAMEPAD_SLOT_COUNT");
    *state = system->pads[slot].state;
}

void tb_gamepad_get_info(const tb_gamepad_system *system, size_t slot, tb_gamepad_info *info)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(info != NULL);
    TB_ASSERT_MSG(slot < TB_GAMEPAD_SLOT_COUNT, "a slot below TB_GAMEPAD_SLOT_COUNT");
    *info = system->pads[slot].info;
    if (system->pads[slot].serial != 0)
    {
        info->family = tb_gamepad_settle_family(info, system->steam_deck);
    }
}

void tb_gamepad_system_set_steam_deck(tb_gamepad_system *system, bool steam_deck)
{
    TB_ASSERT(system != NULL);
    system->steam_deck = steam_deck;
}

/* ---------------------------------------------------------------------------
 * Rumble.
 * ------------------------------------------------------------------------- */

/* Send a slot's rumble to its pad, scaled by the vibration, for what is
 * left of its time; stop it when that leaves nothing to feel. */
static bool tb_gamepad_send(tb_gamepad_system *system, size_t slot, uint64_t now)
{
    tb_gamepad_motors *const motors = &system->motors[slot];
    const float low = motors->low_frequency * system->vibration;
    const float high = motors->high_frequency * system->vibration;

    if ((low <= 0.0f && high <= 0.0f) || now >= motors->until)
    {
        tb_gamepad_stop(system, slot);
        return false;
    }
    system->ops->rumble(system->native, slot, low, high,
                        (double)(motors->until - now) / TB_GAMEPAD_NANOSECONDS_PER_SECOND);
    return true;
}

bool tb_gamepad_rumble(tb_gamepad_system *system, size_t slot, const tb_gamepad_rumble_desc *rumble)
{
    const tb_gamepad_pad *pad = NULL;
    uint64_t now = 0;
    double seconds = 0.0;

    TB_ASSERT(system != NULL);
    TB_ASSERT(rumble != NULL);
    TB_ASSERT_MSG(slot < TB_GAMEPAD_SLOT_COUNT, "a slot below TB_GAMEPAD_SLOT_COUNT");
    TB_ASSERT_MSG(isfinite(rumble->seconds) && rumble->seconds >= 0.0,
                  "a finite length that is not negative");
    TB_ASSERT_MSG(!isnan(rumble->low_frequency) && !isnan(rumble->high_frequency), "motors");
    pad = &system->pads[slot];
    if (pad->serial == 0 || (pad->info.capabilities & TB_GAMEPAD_CAPABILITY_RUMBLE) == 0)
    {
        return false;
    }
    if (!system->focused)
    {
        tb_gamepad_stop(system, slot);
        return false;
    }
    now = system->clock();
    seconds = rumble->seconds < TB_GAMEPAD_LONGEST_RUMBLE_SECONDS
                  ? rumble->seconds
                  : TB_GAMEPAD_LONGEST_RUMBLE_SECONDS;
    system->motors[slot] =
        (tb_gamepad_motors){.running = true,
                            .low_frequency = tb_gamepad_clamp(rumble->low_frequency, 0.0f, 1.0f),
                            .high_frequency = tb_gamepad_clamp(rumble->high_frequency, 0.0f, 1.0f),
                            .until = now + (uint64_t)(seconds * TB_GAMEPAD_NANOSECONDS_PER_SECOND)};
    return tb_gamepad_send(system, slot, now);
}

void tb_gamepad_system_set_vibration(tb_gamepad_system *system, float scale)
{
    uint64_t now = 0;

    TB_ASSERT(system != NULL);
    TB_ASSERT_MSG(!isnan(scale), "a scale");
    system->vibration = tb_gamepad_clamp(scale, 0.0f, 1.0f);
    now = system->clock();
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (system->motors[slot].running)
        {
            (void)tb_gamepad_send(system, slot, now);
        }
    }
}

void tb_gamepad_system_set_focus(tb_gamepad_system *system, bool focused)
{
    TB_ASSERT(system != NULL);
    system->focused = focused;
    if (!focused)
    {
        tb_gamepad_stop_all(system);
    }
}
