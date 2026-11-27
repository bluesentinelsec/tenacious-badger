/* Objective-C method calls use the method's exact C ABI, including 64-bit
 * NSUInteger and by-value structs. No framework contributes a link dependency. */
#ifndef TB_METAL_INTERNAL_H
#define TB_METAL_INTERNAL_H
#include "../rhi_backend.h"
#include "badger/library.h"
#include "badger/thread.h"

typedef struct tb_metal_device
{
    tb_library *metal;
    tb_library *objc;
    void *(*get_class)(const char *);
    void *(*selector)(const char *);
    tb_library_function send;
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
    void *gpu;
    void *queue;
    tb_mutex lock;
    tb_condition finished;
    uint64_t completed;
    bool lost;
} tb_metal_device;

typedef struct tb_metal_pipeline
{
    void *state;
    unsigned long primitive;
} tb_metal_pipeline;

/* Cast objc_msgSend to a fixed method signature before passing arguments:
 * using variadic dispatch passes them on the stack on Apple silicon. */
#define TB_METAL_METHOD(d, result, ...) ((result (*)(void *, void *, __VA_ARGS__))(d)->send)
static inline void *tb_metal_get(tb_metal_device *d, void *object, const char *method)
{
    return ((void *(*)(void *, void *))d->send)(object, d->selector(method));
}
static inline void tb_metal_call(tb_metal_device *d, void *object, const char *method)
{
    ((void (*)(void *, void *))d->send)(object, d->selector(method));
}
static inline void tb_metal_set(tb_metal_device *d, void *object, const char *method, void *value)
{
    TB_METAL_METHOD(d, void, void *)(object, d->selector(method), value);
}
static inline void tb_metal_number(tb_metal_device *d, void *object, const char *method,
                                   unsigned long value)
{
    TB_METAL_METHOD(d, void, unsigned long)(object, d->selector(method), value);
}
static inline void *tb_metal_color(tb_metal_device *d, void *desc, unsigned long index)
{
    return TB_METAL_METHOD(d, void *, unsigned long)(
        tb_metal_get(d, desc, "colorAttachments"), d->selector("objectAtIndexedSubscript:"), index);
}

extern const tb_rhi_backend_table tb_rhi_metal_backend;
tb_rhi_result tb_metal_device_create(const tb_rhi_device_options *options, void **state,
                                     tb_rhi_device_info *info, tb_rhi_error *error);
void tb_metal_device_destroy(void *state);
tb_rhi_result tb_metal_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_metal_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size);
void tb_metal_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size);
tb_rhi_result tb_metal_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error);
tb_rhi_result tb_metal_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error);
tb_rhi_result tb_metal_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *pipeline,
                                                void **object, tb_rhi_error *error);
void tb_metal_pipeline_destroy(void *state, void *object);
void *tb_metal_list_begin(void *state);
void tb_metal_list_discard(void *state, void *list);
tb_rhi_result tb_metal_submit(void *state, void *const *lists, size_t count, uint64_t submission);
uint64_t tb_metal_completed(void *state);
tb_rhi_result tb_metal_wait(void *state, uint64_t submission);
void tb_metal_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count);
void tb_metal_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass);
void tb_metal_cmd_end_pass(void *list);
void tb_metal_cmd_viewport(void *list, float x, float y, float width, float height,
                           float near_depth, float far_depth);
void tb_metal_cmd_scissor(void *list, tb_rhi_rect rect);
void tb_metal_cmd_pipeline(void *list, void *pipeline, bool compute);
void tb_metal_cmd_push_constants(void *list, const void *data, size_t size);
void tb_metal_cmd_constant_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset);
void tb_metal_cmd_draw(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance);
void tb_metal_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size);
void tb_metal_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_metal_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_metal_cmd_begin_marker(void *list, const char *name);
void tb_metal_cmd_end_marker(void *list);
void tb_metal_release(void *state, void *object);
#endif
