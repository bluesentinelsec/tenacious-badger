/* The versioned native format uses frame names for every animation reference. */
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/str.h"
#include "sprite_internal.h"

static void key(tb_json_writer *writer, const char *name)
{
    (void)tb_json_write_key(writer, name, tb_str_length(name));
}
static void string(tb_json_writer *writer, const char *name, const char *value)
{
    key(writer, name);
    (void)tb_json_write_string(writer, value, tb_str_length(value));
}
static void number(tb_json_writer *writer, const char *name, uint32_t value)
{
    key(writer, name);
    (void)tb_json_write_uint64(writer, value);
}

static void write_frame(tb_json_writer *writer, const tb_sprite_frame *frame)
{
    (void)tb_json_write_begin_object(writer);
    string(writer, "name", frame->name);
    key(writer, "frame");
    (void)tb_json_write_begin_object(writer);
    number(writer, "x", (uint32_t)frame->rect.x);
    number(writer, "y", (uint32_t)frame->rect.y);
    number(writer, "w", frame->rect.width);
    number(writer, "h", frame->rect.height);
    (void)tb_json_write_end_object(writer);
    key(writer, "sourceSize");
    (void)tb_json_write_begin_object(writer);
    number(writer, "w", frame->source_width);
    number(writer, "h", frame->source_height);
    (void)tb_json_write_end_object(writer);
    key(writer, "spriteSourceSize");
    (void)tb_json_write_begin_object(writer);
    number(writer, "x", (uint32_t)frame->offset_x);
    number(writer, "y", (uint32_t)frame->offset_y);
    (void)tb_json_write_end_object(writer);
    key(writer, "pivot");
    (void)tb_json_write_begin_object(writer);
    key(writer, "x");
    (void)tb_json_write_double(writer, (double)frame->pivot_x);
    key(writer, "y");
    (void)tb_json_write_double(writer, (double)frame->pivot_y);
    (void)tb_json_write_end_object(writer);
    (void)tb_json_write_end_object(writer);
}

tb_sprite_result tb_sprite_sheet_save(const tb_sprite_sheet *sheet, tb_stream *sink,
                                      tb_sprite_error *error)
{
    TB_ASSERT(sheet != NULL && sink != NULL);
    tb_json_writer *const writer = tb_json_writer_create(sink, TB_JSON_INDENTED, &sheet->allocator);
    (void)tb_json_write_begin_object(writer);
    string(writer, "format", TB_SPRITE_FILE_FORMAT);
    number(writer, "version", TB_SPRITE_FILE_VERSION);
    string(writer, "image", tb_sprite_sheet_image(sheet));
    key(writer, "frames");
    (void)tb_json_write_begin_array(writer);
    for (size_t i = 0; i < tb_sprite_sheet_frame_count(sheet); i++)
    {
        write_frame(writer, tb_sprite_sheet_frame(sheet, i));
    }
    (void)tb_json_write_end_array(writer);
    key(writer, "animations");
    (void)tb_json_write_begin_array(writer);
    for (size_t i = 0; i < tb_sprite_sheet_animation_count(sheet); i++)
    {
        const tb_sprite_animation *const animation = tb_sprite_sheet_animation(sheet, i);
        static const char *const loops[TB_SPRITE_LOOP_COUNT] = {"loop", "once", "ping-pong"};
        (void)tb_json_write_begin_object(writer);
        string(writer, "name", animation->name);
        string(writer, "loop", loops[animation->loop]);
        key(writer, "steps");
        (void)tb_json_write_begin_array(writer);
        for (size_t j = 0; j < animation->step_count; j++)
        {
            const tb_sprite_step *const step = &animation->steps[j];
            (void)tb_json_write_begin_object(writer);
            string(writer, "frame", tb_sprite_sheet_frame(sheet, step->frame)->name);
            number(writer, "duration", step->duration_ms);
            (void)tb_json_write_end_object(writer);
        }
        (void)tb_json_write_end_array(writer);
        (void)tb_json_write_end_object(writer);
    }
    (void)tb_json_write_end_array(writer);
    (void)tb_json_write_end_object(writer);
    const tb_json_result result = tb_json_writer_finish(writer);
    tb_json_writer_destroy(writer);
    const tb_sprite_result outcome = result == TB_JSON_OK ? TB_SPRITE_OK : TB_SPRITE_OUTPUT_FAILED;
    if (error != NULL)
    {
        *error = (tb_sprite_error){
            .result = outcome, .reason = result == TB_JSON_OK ? "" : tb_json_result_name(result)};
        (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s", error->reason);
    }
    return outcome;
}
