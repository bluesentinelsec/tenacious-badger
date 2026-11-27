#ifndef TB_SPRITE_INTERNAL_H
#define TB_SPRITE_INTERNAL_H
#include "badger/array.h"
#include "badger/sprite.h"
#include "badger/table.h"

struct tb_sprite_sheet
{
    tb_allocator allocator;
    char *image;
    tb_array *frames;
    tb_array *animations;
    tb_table *frame_names;
    tb_table *animation_names;
};

/* Undo a partial import, grid, or atlas while keeping earlier entries and their names. */
void tb_sprite_sheet_truncate(tb_sprite_sheet *sheet, size_t frames, size_t animations);
#endif
