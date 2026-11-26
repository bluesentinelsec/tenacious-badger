/* One backend over several: each child reads its own pads into slots of
 * its own, and the mux gives each pad a slot of the system. */
#include "gamepad_mux.h"

#include "badger/assert.h"
#include "badger/memory.h"

/* A child's slot that no system slot holds. */
#define TB_GAMEPAD_MUX_NONE SIZE_MAX

typedef struct tb_gamepad_mux_source
{
    tb_gamepad_mux_child child;
    tb_gamepad_pad pads[TB_GAMEPAD_SLOT_COUNT]; /* As the child left them. */
    size_t slots[TB_GAMEPAD_SLOT_COUNT];        /* The system's slot of each, or NONE. */
    uint64_t serials[TB_GAMEPAD_SLOT_COUNT];    /* The child's serial the slot was given for. */
} tb_gamepad_mux_source;

/* Which child's pad a system slot holds. */
typedef struct tb_gamepad_mux_owner
{
    bool used;
    size_t source;
    size_t pad;
    uint64_t serial; /* The mux's serial of the connection. */
} tb_gamepad_mux_owner;

typedef struct tb_gamepad_mux
{
    tb_gamepad_mux_source sources[TB_GAMEPAD_MUX_CAPACITY];
    size_t count;
    tb_gamepad_mux_owner owners[TB_GAMEPAD_SLOT_COUNT];
    uint64_t serial;
} tb_gamepad_mux;

/* Free the system slots of pads that went, or that their child replaced. */
static void tb_gamepad_mux_release(tb_gamepad_mux *mux, tb_gamepad_pad *pads)
{
    for (size_t source = 0; source < mux->count; source++)
    {
        tb_gamepad_mux_source *const from = &mux->sources[source];
        for (size_t pad = 0; pad < TB_GAMEPAD_SLOT_COUNT; pad++)
        {
            const size_t slot = from->slots[pad];
            if (slot == TB_GAMEPAD_MUX_NONE || from->pads[pad].serial == from->serials[pad])
            {
                continue;
            }
            mux->owners[slot] = (tb_gamepad_mux_owner){0};
            pads[slot] = (tb_gamepad_pad){0};
            from->slots[pad] = TB_GAMEPAD_MUX_NONE;
        }
    }
}

static size_t tb_gamepad_mux_free_slot(const tb_gamepad_mux *mux)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (!mux->owners[slot].used)
        {
            return slot;
        }
    }
    return TB_GAMEPAD_MUX_NONE;
}

/* Give each new pad the lowest free slot, and copy every held pad. */
static void tb_gamepad_mux_assign(tb_gamepad_mux *mux, tb_gamepad_pad *pads)
{
    for (size_t source = 0; source < mux->count; source++)
    {
        tb_gamepad_mux_source *const from = &mux->sources[source];
        for (size_t pad = 0; pad < TB_GAMEPAD_SLOT_COUNT; pad++)
        {
            size_t slot = from->slots[pad];
            if (from->pads[pad].serial == 0)
            {
                continue;
            }
            if (slot == TB_GAMEPAD_MUX_NONE)
            {
                slot = tb_gamepad_mux_free_slot(mux);
                if (slot == TB_GAMEPAD_MUX_NONE)
                {
                    continue;
                }
                mux->serial++;
                mux->owners[slot] = (tb_gamepad_mux_owner){
                    .used = true, .source = source, .pad = pad, .serial = mux->serial};
                from->slots[pad] = slot;
                from->serials[pad] = from->pads[pad].serial;
            }
            pads[slot] = from->pads[pad];
            pads[slot].serial = mux->owners[slot].serial;
        }
    }
}

static void tb_gamepad_mux_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    tb_gamepad_mux *const mux = native;

    for (size_t source = 0; source < mux->count; source++)
    {
        tb_gamepad_mux_source *const from = &mux->sources[source];
        from->child.ops->poll(from->child.native, now, from->pads);
    }
    tb_gamepad_mux_release(mux, pads);
    tb_gamepad_mux_assign(mux, pads);
}

static void tb_gamepad_mux_rumble(void *native, size_t slot, float low_frequency,
                                  float high_frequency, double seconds)
{
    tb_gamepad_mux *const mux = native;
    const tb_gamepad_mux_owner *owner = NULL;
    const tb_gamepad_mux_source *from = NULL;

    TB_ASSERT(slot < TB_GAMEPAD_SLOT_COUNT);
    owner = &mux->owners[slot];
    TB_ASSERT_MSG(owner->used, "a slot with a pad");
    from = &mux->sources[owner->source];
    from->child.ops->rumble(from->child.native, owner->pad, low_frequency, high_frequency, seconds);
}

static void tb_gamepad_mux_player(void *native, size_t slot, size_t player)
{
    tb_gamepad_mux *const mux = native;
    const tb_gamepad_mux_owner *owner = NULL;
    const tb_gamepad_mux_source *from = NULL;

    TB_ASSERT(slot < TB_GAMEPAD_SLOT_COUNT);
    owner = &mux->owners[slot];
    TB_ASSERT_MSG(owner->used, "a slot with a pad");
    from = &mux->sources[owner->source];
    if (from->child.ops->player != NULL)
    {
        from->child.ops->player(from->child.native, owner->pad, player);
    }
}

static void tb_gamepad_mux_close(void *native)
{
    tb_gamepad_mux *const mux = native;

    for (size_t source = 0; source < mux->count; source++)
    {
        mux->sources[source].child.ops->close(mux->sources[source].child.native);
    }
    tb_free(mux);
}

const tb_gamepad_ops tb_gamepad_mux_ops = {
    .close = tb_gamepad_mux_close,
    .poll = tb_gamepad_mux_poll,
    .rumble = tb_gamepad_mux_rumble,
    .player = tb_gamepad_mux_player,
};

void *tb_gamepad_mux_make(const tb_gamepad_mux_child *children, size_t count)
{
    tb_gamepad_mux *const mux = tb_calloc(1, sizeof(*mux));

    TB_ASSERT(children != NULL);
    TB_ASSERT_MSG(count > 0 && count <= TB_GAMEPAD_MUX_CAPACITY, "1 to 4 backends");
    mux->count = count;
    for (size_t source = 0; source < count; source++)
    {
        TB_ASSERT(children[source].ops != NULL);
        mux->sources[source].child = children[source];
        for (size_t pad = 0; pad < TB_GAMEPAD_SLOT_COUNT; pad++)
        {
            mux->sources[source].slots[pad] = TB_GAMEPAD_MUX_NONE;
        }
    }
    return mux;
}
