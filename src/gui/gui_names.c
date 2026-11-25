/* Objects named for layout files: a file refers to a command by its name,
 * and the GUI finds the command by the name, and the name by the command.
 * The registry holds values of any named type, each type a namespace of
 * its own, so that other kinds of objects join it by their type. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* Names a GUI first has room for. */
#define TB_GUI_FIRST_NAMES 16u

bool tb_gui_type_is_named(tb_gui_type type)
{
    return type == TB_GUI_TYPE_COMMAND;
}

/* Whether two values of one named type are the same object. */
static bool tb_gui_named_same(const tb_gui_value *a, const tb_gui_value *b)
{
    if (a->type != b->type)
    {
        return false;
    }
    if (a->type == TB_GUI_TYPE_COMMAND)
    {
        return a->command.callback == b->command.callback && a->command.user == b->command.user;
    }
    return false;
}

static size_t tb_gui_named_index(const tb_gui *gui, tb_gui_type type, tb_text name)
{
    for (size_t index = 0; index < gui->name_count; index++)
    {
        const tb_gui_named *const named = &gui->names[index];
        if (named->value.type == type && tb_text_equal(tb_text_from_cstr(named->name), name))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

bool tb_gui_named_find(const tb_gui *gui, tb_gui_type type, tb_text name, tb_gui_value *value)
{
    const size_t index = tb_gui_named_index(gui, type, name);

    if (index == SIZE_MAX)
    {
        return false;
    }
    *value = gui->names[index].value;
    return true;
}

const char *tb_gui_named_name_of(const tb_gui *gui, const tb_gui_value *value)
{
    for (size_t index = 0; index < gui->name_count; index++)
    {
        if (tb_gui_named_same(&gui->names[index].value, value))
        {
            return gui->names[index].name;
        }
    }
    return NULL;
}

/* Add a name of an object of a named type; false when the name is taken. */
static bool tb_gui_named_add(tb_gui *gui, const char *name, const tb_gui_value *value)
{
    TB_ASSERT(tb_gui_type_is_named(value->type));
    if (tb_gui_named_index(gui, value->type, tb_text_from_cstr(name)) != SIZE_MAX)
    {
        return false;
    }
    if (gui->name_count == gui->name_capacity)
    {
        gui->name_capacity = gui->name_capacity == 0 ? TB_GUI_FIRST_NAMES : gui->name_capacity * 2;
        gui->names = tb_realloc_array(gui->names, gui->name_capacity, sizeof(*gui->names));
    }
    gui->names[gui->name_count] = (tb_gui_named){.name = tb_str_dup(NULL, name), .value = *value};
    gui->name_count++;
    return true;
}

static bool tb_gui_named_remove(tb_gui *gui, tb_gui_type type, const char *name)
{
    const size_t index = tb_gui_named_index(gui, type, tb_text_from_cstr(name));

    if (index == SIZE_MAX)
    {
        return false;
    }
    tb_str_free(NULL, gui->names[index].name);
    /* Kept in the order registered: the first of two names of one object wins. */
    memmove(&gui->names[index], &gui->names[index + 1],
            (gui->name_count - index - 1) * sizeof(*gui->names));
    gui->name_count--;
    return true;
}

void tb_gui_names_free(tb_gui *gui)
{
    for (size_t index = 0; index < gui->name_count; index++)
    {
        tb_str_free(NULL, gui->names[index].name);
    }
    tb_free(gui->names);
    gui->names = NULL;
    gui->name_count = 0;
    gui->name_capacity = 0;
}

/* ---------------------------------------------------------------------------
 * Commands.
 * ------------------------------------------------------------------------- */

bool tb_gui_register_command(tb_gui *gui, const char *name, tb_gui_callback *callback, void *user)
{
    const tb_gui_value value = tb_gui_command_of(callback, user);

    TB_ASSERT(gui != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT(callback != NULL);
    return tb_gui_named_add(gui, name, &value);
}

bool tb_gui_unregister_command(tb_gui *gui, const char *name)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(name != NULL);
    return tb_gui_named_remove(gui, TB_GUI_TYPE_COMMAND, name);
}

bool tb_gui_find_command(const tb_gui *gui, const char *name, tb_gui_command *command)
{
    tb_gui_value value = {0};

    TB_ASSERT(gui != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT(command != NULL);
    if (!tb_gui_named_find(gui, TB_GUI_TYPE_COMMAND, tb_text_from_cstr(name), &value))
    {
        return false;
    }
    *command = value.command;
    return true;
}
