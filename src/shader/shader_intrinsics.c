/* The table of HLSL's intrinsic functions. */
#include "shader_intrinsics.h"

static const tb_shader_intrinsic_info intrinsics[] = {
    {TB_SHADER_INTRINSIC_NONE, "", ""},
#define TB_SHADER_INTRINSIC_INFO(identifier, name, signatures)                                     \
    {TB_SHADER_INTRINSIC_##identifier, name, signatures},
    TB_SHADER_INTRINSIC_LIST(TB_SHADER_INTRINSIC_INFO)
#undef TB_SHADER_INTRINSIC_INFO
};

const tb_shader_intrinsic_info *tb_shader_intrinsic_info_of(tb_shader_intrinsic intrinsic)
{
    if (intrinsic == TB_SHADER_INTRINSIC_NONE || (size_t)intrinsic >= TB_SHADER_INTRINSIC_COUNT)
    {
        return NULL;
    }
    return &intrinsics[intrinsic];
}
