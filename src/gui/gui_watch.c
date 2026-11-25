/* Hot reload: a widget loaded from a layout file is loaded again when the
 * file changes. Each update looks at the files whose interval has passed,
 * through the VFS, so that the same code serves every platform and every
 * mount; a file's bytes are hashed, and a change is built anew in the old
 * widget's place. A file that fails keeps the old widget. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* Seconds between looks at a file unless chosen. */
#define TB_GUI_WATCH_INTERVAL 0.5
/* Watches a GUI first has room for. */
#define TB_GUI_FIRST_WATCHES 4u

struct tb_gui_watch_entry
{
    tb_widget widget;
    struct tb_vfs *vfs;
    char *path; /* owned */
    double interval;
    double next;   /* the GUI's time of the next look */
    bool read;     /* the file was read, and `hash` is of its bytes */
    uint64_t hash; /* of the bytes last loaded or tried */
    tb_gui_reload_callback *callback;
    void *user;
};

static size_t tb_gui_watch_index(const tb_gui *gui, tb_widget widget)
{
    for (size_t index = 0; index < gui->watch_count; index++)
    {
        if (tb_widget_equal(gui->watches[index].widget, widget))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

static void tb_gui_watch_remove(tb_gui *gui, size_t index)
{
    tb_str_free(NULL, gui->watches[index].path);
    memmove(&gui->watches[index], &gui->watches[index + 1],
            (gui->watch_count - index - 1) * sizeof(*gui->watches));
    gui->watch_count--;
}

/* The hash of a file's bytes now; false when it cannot be read, as while
 * an editor replaces it. */
static bool tb_gui_watch_hash(const tb_gui_watch_entry *entry, char **bytes, size_t *length,
                              uint64_t *hash)
{
    tb_gui_file_error ignored = {0};

    if (tb_gui_read_file(entry->vfs, entry->path, bytes, length, &ignored) != TB_GUI_FILE_OK)
    {
        return false;
    }
    *hash = tb_hash_bytes(*bytes, *length);
    return true;
}

bool tb_gui_watch(tb_widget widget, struct tb_vfs *vfs, const char *path,
                  const tb_gui_watch_options *options)
{
    static const tb_gui_watch_options defaults = {0};
    const tb_gui_watch_options *const chosen = options != NULL ? options : &defaults;
    tb_gui *const gui = widget.gui;
    tb_gui_watch_entry entry = {0};
    char *bytes = NULL;
    size_t length = 0;

    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    if (!tb_widget_alive(widget) || !tb_widget_alive(tb_widget_parent(widget)))
    {
        return false;
    }
    (void)tb_gui_unwatch(widget);
    entry = (tb_gui_watch_entry){
        .widget = widget,
        .vfs = vfs,
        .path = tb_str_dup(NULL, path),
        .interval = chosen->interval > 0.0 ? chosen->interval : TB_GUI_WATCH_INTERVAL,
        .callback = chosen->callback,
        .user = chosen->user,
    };
    entry.next = gui->time + entry.interval;
    entry.read = tb_gui_watch_hash(&entry, &bytes, &length, &entry.hash);
    tb_free(bytes);
    if (gui->watch_count == gui->watch_capacity)
    {
        gui->watch_capacity =
            gui->watch_capacity == 0 ? TB_GUI_FIRST_WATCHES : gui->watch_capacity * 2;
        gui->watches = tb_realloc_array(gui->watches, gui->watch_capacity, sizeof(*gui->watches));
    }
    gui->watches[gui->watch_count] = entry;
    gui->watch_count++;
    return true;
}

bool tb_gui_unwatch(tb_widget widget)
{
    size_t index = 0;

    if (widget.gui == NULL)
    {
        return false;
    }
    index = tb_gui_watch_index(widget.gui, widget);
    if (index == SIZE_MAX)
    {
        return false;
    }
    tb_gui_watch_remove(widget.gui, index);
    return true;
}

/* Build a file's new bytes in its widget's place. The watch's widget is the
 * new one on success; the callback is called last, as it may watch and
 * unwatch, which moves the watches. */
static void tb_gui_watch_reload(tb_gui *gui, size_t index, const char *bytes, size_t length)
{
    tb_gui_watch_entry *const entry = &gui->watches[index];
    const tb_widget old = entry->widget;
    tb_gui_reload_callback *const callback = entry->callback;
    void *const user = entry->user;
    tb_gui_file_error error = {0};
    tb_widget made = TB_WIDGET_NONE;
    const tb_gui_file_result result =
        tb_gui_load(tb_widget_parent(old), bytes, length, entry->path, &made, &error);

    if (result != TB_GUI_FILE_OK)
    {
        if (callback != NULL)
        {
            callback(old, &error, user);
        }
        return;
    }
    tb_gui_move_before(gui, made.index, old.index);
    tb_widget_destroy(old);
    entry->widget = made;
    if (callback != NULL)
    {
        callback(made, NULL, user);
    }
}

void tb_gui_watches_poll(tb_gui *gui)
{
    size_t index = 0;

    while (index < gui->watch_count)
    {
        tb_gui_watch_entry *const entry = &gui->watches[index];
        char *bytes = NULL;
        size_t length = 0;
        uint64_t hash = 0;
        if (!tb_widget_alive(entry->widget))
        {
            /* Destroyed other than by a reload: the watch ends with it. */
            tb_gui_watch_remove(gui, index);
            continue;
        }
        index++;
        if (gui->time < entry->next)
        {
            continue;
        }
        entry->next = gui->time + entry->interval;
        if (!tb_gui_watch_hash(entry, &bytes, &length, &hash))
        {
            continue;
        }
        if (!entry->read || hash != entry->hash)
        {
            entry->read = true;
            entry->hash = hash;
            tb_gui_watch_reload(gui, index - 1, bytes, length);
        }
        tb_free(bytes);
    }
}

void tb_gui_watches_free(tb_gui *gui)
{
    for (size_t index = 0; index < gui->watch_count; index++)
    {
        tb_str_free(NULL, gui->watches[index].path);
    }
    tb_free(gui->watches);
    gui->watches = NULL;
    gui->watch_count = 0;
    gui->watch_capacity = 0;
}
