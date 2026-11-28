/** @file rhi_shader_library.h
 * @brief Create portable shaders and rebuild their pipelines from a destination's cooked library.
 * @par Threading Thread-safe under the RHI resource lifetime contract. Both devices, the
 * source pipeline and the borrowed library must outlive a call that uses them.
 */
#ifndef TB_RHI_SHADER_LIBRARY_H
#define TB_RHI_SHADER_LIBRARY_H

#include "badger/rhi.h"
#include "badger/shader_library.h"

/** @brief The shader target a backend consumes, or #TB_SHADER_TARGET_NONE if it has no cooked
 * format. */
TB_API tb_shader_target tb_rhi_backend_shader_target(tb_rhi_backend backend);

/**
 * @brief Make a shader while retaining its virtual path and the cooked keys for its permutation.
 * @param device Required destination device.
 * @param request The exact source, entry, stage, defines and flags used to produce the code.
 * The target must match the device. Sources are borrowed during this call only.
 * @param code Nonempty compiled bytes for this device, or a placeholder for null. Copied.
 * @param size Bytes of code.
 * @param shader Receives the owned shader handle, or zero on failure. Pipelines retain its
 * identity.
 * @param error Optional diagnostic.
 * @return #TB_RHI_OK, or an error for an invalid request or rejected shader.
 */
TB_API tb_rhi_result tb_rhi_shader_create_compiled(tb_rhi_device *device,
                                                   const tb_shader_compile_request *request,
                                                   const void *code, size_t size,
                                                   tb_rhi_shader *shader, tb_rhi_error *error);

/**
 * @brief Build a graphics or compute pipeline on another device using only its cooked shader
 * library.
 * @param source Device owning the live pipeline.
 * @param pipeline A live pipeline with stages made by tb_rhi_shader_create_compiled() or an earlier
 * library transfer. Original shader handles may already have been destroyed.
 * @param destination Device to receive the independently owned pipeline. May equal source.
 * @param library Open library containing the destination variants. Borrowed for the call.
 * @param made Receives a new pipeline owned by destination, or zero on failure.
 * @param error Optional diagnostic identifying a missing stage or backend failure.
 * @return #TB_RHI_OK, #TB_RHI_UNAVAILABLE for missing variants or an unsupported target, or a
 * destination creation error. The source pipeline stays usable on every outcome.
 */
TB_API tb_rhi_result tb_rhi_pipeline_create_from_library(
    tb_rhi_device *source, tb_rhi_pipeline pipeline, tb_rhi_device *destination,
    const tb_shader_library *library, tb_rhi_pipeline *made, tb_rhi_error *error);

#endif
