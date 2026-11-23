/* HLSL's intrinsic functions, as data: each one's name and the signatures
 * of its overloads, written in a small language that checking reads.
 *
 * A signature is `result(parameter, ...)`; overloads are separated by `;`.
 * A parameter may begin `out ` or `inout `, and `...` ends a list that
 * takes any further arguments. A type is a component and, in `<...>`, a
 * shape; without one it is a scalar.
 *
 * Components:
 *   float, uint, bool, int16_t, ...  that scalar
 *   numeric#1                        template 1, of a class of scalars:
 *       numeric  every number (not bool); otherwise float
 *       float    half, float and min16float; otherwise float
 *       fdouble  those and double; otherwise float
 *       double   double; otherwise double
 *       int      every integer; otherwise uint
 *       uint     every unsigned integer; otherwise uint
 *       atomic   int, uint, int64_t and uint64_t; nothing else
 *       b32      int, uint and float; nothing else
 *       b16      int16_t, uint16_t and half; nothing else
 *       any      every scalar
 *   #1                               template 1's component
 *   udt#1                            template 1, a struct of the shader's
 *   void, string, RayDesc, RaytracingAccelerationStructure  that type
 *
 * Shapes:
 *   <a1>  shape 1: a scalar, a vector or a matrix (`v1` no matrix, `m1` no vector)
 *   <3>   a vector of 3;  <3x4>  a matrix of 3 rows and 4 columns
 *   <N>   a vector of N;  <RxC>  a matrix of R rows and C columns, where
 *         each capital letter is a size its arguments decide
 *   `$1` is short for `#1<a1>`: template 1, component and shape.
 *
 * Arguments decide a template as DXC decides it. The components of the
 * arguments given for it combine as an operator's operands do; when the
 * result is not in its class, it becomes the class's fallback. The shapes
 * of its arguments combine to the smallest vector or matrix among them, and
 * a scalar takes any shape. A size letter takes the smallest size its
 * arguments give it. Then each argument converts to its parameter, so a
 * larger one is truncated, with a warning. */
#ifndef TB_SHADER_INTRINSICS_H
#define TB_SHADER_INTRINSICS_H

#include <stddef.h>

/* The functions of one argument of a float that keep its type. */
#define TB_SHADER_FLOAT_UNARY "$1(float#1<a1>)"
#define TB_SHADER_FLOAT_BINARY "$1(float#1<a1>, $1)"
#define TB_SHADER_FLOAT_TERNARY "$1(float#1<a1>, $1, $1)"
#define TB_SHADER_FLOAT_TEST "bool<a1>(float#1<a1>)"
#define TB_SHADER_ATOMIC "void(inout atomic#1, #1); void(inout atomic#1, #1, out #1)"
#define TB_SHADER_WAVE_NUMERIC "$1(numeric#1<a1>)"
#define TB_SHADER_WAVE_BITS "$1(int#1<a1>)"
#define TB_SHADER_MULTI_PREFIX_NUMERIC "$1(numeric#1<a1>, uint<4>)"
#define TB_SHADER_MULTI_PREFIX_BITS "$1(int#1<a1>, uint<4>)"

/* X(identifier, name, signatures). Overloads of one name are one entry. */
#define TB_SHADER_INTRINSIC_LIST(X)                                                                \
    /* Arithmetic. */                                                                              \
    X(ABS, "abs", "$1(numeric#1<a1>)")                                                             \
    X(MIN, "min", "$1(numeric#1<a1>, $1)")                                                         \
    X(MAX, "max", "$1(numeric#1<a1>, $1)")                                                         \
    X(CLAMP, "clamp", "$1(numeric#1<a1>, $1, $1)")                                                 \
    X(MAD, "mad", "$1(numeric#1<a1>, $1, $1)")                                                     \
    X(SIGN, "sign", "int<a1>(numeric#1<a1>)")                                                      \
    X(FMA, "fma", "$1(double#1<a1>, $1, $1)")                                                      \
    X(RCP, "rcp", "$1(fdouble#1<a1>)")                                                             \
    X(SATURATE, "saturate", TB_SHADER_FLOAT_UNARY)                                                 \
    X(SQRT, "sqrt", TB_SHADER_FLOAT_UNARY)                                                         \
    X(RSQRT, "rsqrt", TB_SHADER_FLOAT_UNARY)                                                       \
    X(EXP, "exp", TB_SHADER_FLOAT_UNARY)                                                           \
    X(EXP2, "exp2", TB_SHADER_FLOAT_UNARY)                                                         \
    X(LOG, "log", TB_SHADER_FLOAT_UNARY)                                                           \
    X(LOG2, "log2", TB_SHADER_FLOAT_UNARY)                                                         \
    X(LOG10, "log10", TB_SHADER_FLOAT_UNARY)                                                       \
    X(POW, "pow", TB_SHADER_FLOAT_BINARY)                                                          \
    X(FLOOR, "floor", TB_SHADER_FLOAT_UNARY)                                                       \
    X(CEIL, "ceil", TB_SHADER_FLOAT_UNARY)                                                         \
    X(ROUND, "round", TB_SHADER_FLOAT_UNARY)                                                       \
    X(TRUNC, "trunc", TB_SHADER_FLOAT_UNARY)                                                       \
    X(FRAC, "frac", TB_SHADER_FLOAT_UNARY)                                                         \
    X(FMOD, "fmod", TB_SHADER_FLOAT_BINARY)                                                        \
    X(MODF, "modf", "$1(float#1<a1>, out $1)")                                                     \
    X(FREXP, "frexp", "$1(float#1<a1>, out $1)")                                                   \
    X(LDEXP, "ldexp", TB_SHADER_FLOAT_BINARY)                                                      \
    X(LERP, "lerp", TB_SHADER_FLOAT_TERNARY)                                                       \
    X(STEP, "step", TB_SHADER_FLOAT_BINARY)                                                        \
    X(SMOOTHSTEP, "smoothstep", TB_SHADER_FLOAT_TERNARY)                                           \
    X(ISNAN, "isnan", TB_SHADER_FLOAT_TEST)                                                        \
    X(ISINF, "isinf", TB_SHADER_FLOAT_TEST)                                                        \
    X(ISFINITE, "isfinite", TB_SHADER_FLOAT_TEST)                                                  \
    /* Trigonometry. */                                                                            \
    X(SIN, "sin", TB_SHADER_FLOAT_UNARY)                                                           \
    X(COS, "cos", TB_SHADER_FLOAT_UNARY)                                                           \
    X(TAN, "tan", TB_SHADER_FLOAT_UNARY)                                                           \
    X(ASIN, "asin", TB_SHADER_FLOAT_UNARY)                                                         \
    X(ACOS, "acos", TB_SHADER_FLOAT_UNARY)                                                         \
    X(ATAN, "atan", TB_SHADER_FLOAT_UNARY)                                                         \
    X(ATAN2, "atan2", TB_SHADER_FLOAT_BINARY)                                                      \
    X(SINH, "sinh", TB_SHADER_FLOAT_UNARY)                                                         \
    X(COSH, "cosh", TB_SHADER_FLOAT_UNARY)                                                         \
    X(TANH, "tanh", TB_SHADER_FLOAT_UNARY)                                                         \
    X(SINCOS, "sincos", "void(float#1<a1>, out $1, out $1)")                                       \
    X(DEGREES, "degrees", TB_SHADER_FLOAT_UNARY)                                                   \
    X(RADIANS, "radians", TB_SHADER_FLOAT_UNARY)                                                   \
    /* Vectors and matrices. */                                                                    \
    X(DOT, "dot", "#1(numeric#1<N>, #1<N>)")                                                       \
    X(CROSS, "cross", "#1<3>(float#1<3>, #1<3>)")                                                  \
    X(LENGTH, "length", "#1(float#1<N>)")                                                          \
    X(DISTANCE, "distance", "#1(float#1<N>, #1<N>)")                                               \
    X(NORMALIZE, "normalize", "#1<N>(float#1<N>)")                                                 \
    X(REFLECT, "reflect", "#1<N>(float#1<N>, #1<N>)")                                              \
    X(REFRACT, "refract", "#1<N>(float#1<N>, #1<N>, #1)")                                          \
    X(FACEFORWARD, "faceforward", "#1<N>(float#1<N>, #1<N>, #1<N>)")                               \
    X(DST, "dst", "#1<4>(float#1<4>, #1<4>)")                                                      \
    X(LIT, "lit", "float<4>(float, float, float)")                                                 \
    X(DETERMINANT, "determinant", "#1(float#1<NxN>)")                                              \
    X(TRANSPOSE, "transpose", "#1<CxR>(any#1<RxC>)")                                               \
    X(MUL, "mul",                                                                                  \
      "#1(numeric#1, #1); #1<N>(numeric#1, #1<N>); #1<RxC>(numeric#1, #1<RxC>); "                  \
      "#1<N>(numeric#1<N>, #1); #1(numeric#1<N>, #1<N>); #1<C>(numeric#1<N>, #1<NxC>); "           \
      "#1<RxC>(numeric#1<RxC>, #1); #1<R>(numeric#1<RxN>, #1<N>); "                                \
      "#1<RxC>(numeric#1<RxN>, #1<NxC>)")                                                          \
    /* Logic. */                                                                                   \
    X(ALL, "all", "bool(any#1<a1>)")                                                               \
    X(ANY, "any", "bool(any#1<a1>)")                                                               \
    X(AND, "and", "bool<a1>(bool<a1>, bool<a1>)")                                                  \
    X(OR, "or", "bool<a1>(bool<a1>, bool<a1>)")                                                    \
    X(SELECT, "select", "#2<a1>(bool<a1>, any#2<a1>, #2<a1>)")                                     \
    /* Bits and reinterpretation. */                                                               \
    X(COUNTBITS, "countbits", "uint<a1>(int#1<a1>)")                                               \
    X(REVERSEBITS, "reversebits", "$1(uint#1<a1>)")                                                \
    X(FIRSTBITHIGH, "firstbithigh", "uint<a1>(int#1<a1>)")                                         \
    X(FIRSTBITLOW, "firstbitlow", "uint<a1>(int#1<a1>)")                                           \
    X(ASINT, "asint", "int<a1>(b32#1<a1>)")                                                        \
    X(ASUINT, "asuint", "uint<a1>(b32#1<a1>); void(double<a1>, out uint<a1>, out uint<a1>)")       \
    X(ASFLOAT, "asfloat", "float<a1>(b32#1<a1>)")                                                  \
    X(ASDOUBLE, "asdouble", "double<a1>(uint<a1>, uint<a1>)")                                      \
    X(ASFLOAT16, "asfloat16", "half<a1>(b16#1<a1>)")                                               \
    X(ASINT16, "asint16", "int16_t<a1>(b16#1<a1>)")                                                \
    X(ASUINT16, "asuint16", "uint16_t<a1>(b16#1<a1>)")                                             \
    X(F16TOF32, "f16tof32", "float<a1>(uint<a1>)")                                                 \
    X(F32TOF16, "f32tof16", "uint<a1>(float<a1>)")                                                 \
    X(D3DCOLORTOUBYTE4, "D3DCOLORtoUBYTE4", "int<4>(float<4>)")                                    \
    X(MSAD4, "msad4", "uint<4>(uint, uint<2>, uint<4>)")                                           \
    X(DOT4ADD_U8PACKED, "dot4add_u8packed", "uint(uint, uint, uint)")                              \
    X(DOT4ADD_I8PACKED, "dot4add_i8packed", "int(uint, uint, int)")                                \
    X(DOT2ADD, "dot2add", "float(half<2>, half<2>, float)")                                        \
    /* Derivatives and pixel shaders. */                                                           \
    X(DDX, "ddx", TB_SHADER_FLOAT_UNARY)                                                           \
    X(DDY, "ddy", TB_SHADER_FLOAT_UNARY)                                                           \
    X(DDX_COARSE, "ddx_coarse", TB_SHADER_FLOAT_UNARY)                                             \
    X(DDY_COARSE, "ddy_coarse", TB_SHADER_FLOAT_UNARY)                                             \
    X(DDX_FINE, "ddx_fine", TB_SHADER_FLOAT_UNARY)                                                 \
    X(DDY_FINE, "ddy_fine", TB_SHADER_FLOAT_UNARY)                                                 \
    X(FWIDTH, "fwidth", TB_SHADER_FLOAT_UNARY)                                                     \
    X(CLIP, "clip", "void(float#1<a1>)")                                                           \
    X(EVALUATE_ATTRIBUTE_AT_SAMPLE, "EvaluateAttributeAtSample", "$1(numeric#1<a1>, uint)")        \
    X(EVALUATE_ATTRIBUTE_CENTROID, "EvaluateAttributeCentroid", "$1(numeric#1<a1>)")               \
    X(EVALUATE_ATTRIBUTE_SNAPPED, "EvaluateAttributeSnapped", "$1(numeric#1<a1>, int<2>)")         \
    X(GET_ATTRIBUTE_AT_VERTEX, "GetAttributeAtVertex", "$1(numeric#1<a1>, uint)")                  \
    X(GET_RENDER_TARGET_SAMPLE_COUNT, "GetRenderTargetSampleCount", "uint()")                      \
    X(GET_RENDER_TARGET_SAMPLE_POSITION, "GetRenderTargetSamplePosition", "float<2>(int)")         \
    /* Barriers and atomics. */                                                                    \
    X(ALL_MEMORY_BARRIER, "AllMemoryBarrier", "void()")                                            \
    X(ALL_MEMORY_BARRIER_WITH_GROUP_SYNC, "AllMemoryBarrierWithGroupSync", "void()")               \
    X(DEVICE_MEMORY_BARRIER, "DeviceMemoryBarrier", "void()")                                      \
    X(DEVICE_MEMORY_BARRIER_WITH_GROUP_SYNC, "DeviceMemoryBarrierWithGroupSync", "void()")         \
    X(GROUP_MEMORY_BARRIER, "GroupMemoryBarrier", "void()")                                        \
    X(GROUP_MEMORY_BARRIER_WITH_GROUP_SYNC, "GroupMemoryBarrierWithGroupSync", "void()")           \
    X(INTERLOCKED_ADD, "InterlockedAdd", TB_SHADER_ATOMIC)                                         \
    X(INTERLOCKED_AND, "InterlockedAnd", TB_SHADER_ATOMIC)                                         \
    X(INTERLOCKED_OR, "InterlockedOr", TB_SHADER_ATOMIC)                                           \
    X(INTERLOCKED_XOR, "InterlockedXor", TB_SHADER_ATOMIC)                                         \
    X(INTERLOCKED_MIN, "InterlockedMin", TB_SHADER_ATOMIC)                                         \
    X(INTERLOCKED_MAX, "InterlockedMax", TB_SHADER_ATOMIC)                                         \
    X(INTERLOCKED_EXCHANGE, "InterlockedExchange", "void(inout atomic#1, #1, out #1)")             \
    X(INTERLOCKED_COMPARE_EXCHANGE, "InterlockedCompareExchange",                                  \
      "void(inout atomic#1, #1, #1, out #1)")                                                      \
    X(INTERLOCKED_COMPARE_STORE, "InterlockedCompareStore", "void(inout atomic#1, #1, #1)")        \
    X(INTERLOCKED_COMPARE_EXCHANGE_FLOAT_BITWISE, "InterlockedCompareExchangeFloatBitwise",        \
      "void(inout float, float, float, out float)")                                                \
    X(INTERLOCKED_COMPARE_STORE_FLOAT_BITWISE, "InterlockedCompareStoreFloatBitwise",              \
      "void(inout float, float, float)")                                                           \
    /* Waves and quads. */                                                                         \
    X(WAVE_IS_FIRST_LANE, "WaveIsFirstLane", "bool()")                                             \
    X(WAVE_GET_LANE_COUNT, "WaveGetLaneCount", "uint()")                                           \
    X(WAVE_GET_LANE_INDEX, "WaveGetLaneIndex", "uint()")                                           \
    X(WAVE_ACTIVE_ANY_TRUE, "WaveActiveAnyTrue", "bool(bool)")                                     \
    X(WAVE_ACTIVE_ALL_TRUE, "WaveActiveAllTrue", "bool(bool)")                                     \
    X(WAVE_ACTIVE_BALLOT, "WaveActiveBallot", "uint<4>(bool)")                                     \
    X(WAVE_ACTIVE_ALL_EQUAL, "WaveActiveAllEqual", "bool<a1>(any#1<a1>)")                          \
    X(WAVE_ACTIVE_COUNT_BITS, "WaveActiveCountBits", "uint(bool)")                                 \
    X(WAVE_ACTIVE_SUM, "WaveActiveSum", TB_SHADER_WAVE_NUMERIC)                                    \
    X(WAVE_ACTIVE_PRODUCT, "WaveActiveProduct", TB_SHADER_WAVE_NUMERIC)                            \
    X(WAVE_ACTIVE_MIN, "WaveActiveMin", TB_SHADER_WAVE_NUMERIC)                                    \
    X(WAVE_ACTIVE_MAX, "WaveActiveMax", TB_SHADER_WAVE_NUMERIC)                                    \
    X(WAVE_ACTIVE_BIT_AND, "WaveActiveBitAnd", TB_SHADER_WAVE_BITS)                                \
    X(WAVE_ACTIVE_BIT_OR, "WaveActiveBitOr", TB_SHADER_WAVE_BITS)                                  \
    X(WAVE_ACTIVE_BIT_XOR, "WaveActiveBitXor", TB_SHADER_WAVE_BITS)                                \
    X(WAVE_READ_LANE_AT, "WaveReadLaneAt", "$1(any#1<a1>, uint)")                                  \
    X(WAVE_READ_LANE_FIRST, "WaveReadLaneFirst", "$1(any#1<a1>)")                                  \
    X(WAVE_PREFIX_COUNT_BITS, "WavePrefixCountBits", "uint(bool)")                                 \
    X(WAVE_PREFIX_SUM, "WavePrefixSum", TB_SHADER_WAVE_NUMERIC)                                    \
    X(WAVE_PREFIX_PRODUCT, "WavePrefixProduct", TB_SHADER_WAVE_NUMERIC)                            \
    X(WAVE_MATCH, "WaveMatch", "uint<4>(any#1<a1>)")                                               \
    X(WAVE_MULTI_PREFIX_SUM, "WaveMultiPrefixSum", TB_SHADER_MULTI_PREFIX_NUMERIC)                 \
    X(WAVE_MULTI_PREFIX_PRODUCT, "WaveMultiPrefixProduct", TB_SHADER_MULTI_PREFIX_NUMERIC)         \
    X(WAVE_MULTI_PREFIX_BIT_AND, "WaveMultiPrefixBitAnd", TB_SHADER_MULTI_PREFIX_BITS)             \
    X(WAVE_MULTI_PREFIX_BIT_OR, "WaveMultiPrefixBitOr", TB_SHADER_MULTI_PREFIX_BITS)               \
    X(WAVE_MULTI_PREFIX_BIT_XOR, "WaveMultiPrefixBitXor", TB_SHADER_MULTI_PREFIX_BITS)             \
    X(WAVE_MULTI_PREFIX_COUNT_BITS, "WaveMultiPrefixCountBits", "uint(bool, uint<4>)")             \
    X(QUAD_READ_LANE_AT, "QuadReadLaneAt", "$1(any#1<a1>, uint)")                                  \
    X(QUAD_READ_ACROSS_X, "QuadReadAcrossX", "$1(any#1<a1>)")                                      \
    X(QUAD_READ_ACROSS_Y, "QuadReadAcrossY", "$1(any#1<a1>)")                                      \
    X(QUAD_READ_ACROSS_DIAGONAL, "QuadReadAcrossDiagonal", "$1(any#1<a1>)")                        \
    X(QUAD_ANY, "QuadAny", "bool(bool)")                                                           \
    X(QUAD_ALL, "QuadAll", "bool(bool)")                                                           \
    /* Ray tracing. */                                                                             \
    X(TRACE_RAY, "TraceRay",                                                                       \
      "void(RaytracingAccelerationStructure, uint, uint, uint, uint, uint, RayDesc, inout udt#1)") \
    X(REPORT_HIT, "ReportHit", "bool(float, uint, udt#1)")                                         \
    X(CALL_SHADER, "CallShader", "void(uint, inout udt#1)")                                        \
    X(IGNORE_HIT, "IgnoreHit", "void()")                                                           \
    X(ACCEPT_HIT_AND_END_SEARCH, "AcceptHitAndEndSearch", "void()")                                \
    X(DISPATCH_RAYS_INDEX, "DispatchRaysIndex", "uint<3>()")                                       \
    X(DISPATCH_RAYS_DIMENSIONS, "DispatchRaysDimensions", "uint<3>()")                             \
    X(WORLD_RAY_ORIGIN, "WorldRayOrigin", "float<3>()")                                            \
    X(WORLD_RAY_DIRECTION, "WorldRayDirection", "float<3>()")                                      \
    X(OBJECT_RAY_ORIGIN, "ObjectRayOrigin", "float<3>()")                                          \
    X(OBJECT_RAY_DIRECTION, "ObjectRayDirection", "float<3>()")                                    \
    X(RAY_T_MIN, "RayTMin", "float()")                                                             \
    X(RAY_T_CURRENT, "RayTCurrent", "float()")                                                     \
    X(RAY_FLAGS, "RayFlags", "uint()")                                                             \
    X(INSTANCE_INDEX, "InstanceIndex", "uint()")                                                   \
    X(INSTANCE_ID, "InstanceID", "uint()")                                                         \
    X(GEOMETRY_INDEX, "GeometryIndex", "uint()")                                                   \
    X(PRIMITIVE_INDEX, "PrimitiveIndex", "uint()")                                                 \
    X(HIT_KIND, "HitKind", "uint()")                                                               \
    X(OBJECT_TO_WORLD, "ObjectToWorld", "float<3x4>()")                                            \
    X(WORLD_TO_OBJECT, "WorldToObject", "float<3x4>()")                                            \
    X(OBJECT_TO_WORLD3X4, "ObjectToWorld3x4", "float<3x4>()")                                      \
    X(WORLD_TO_OBJECT3X4, "WorldToObject3x4", "float<3x4>()")                                      \
    X(OBJECT_TO_WORLD4X3, "ObjectToWorld4x3", "float<4x3>()")                                      \
    X(WORLD_TO_OBJECT4X3, "WorldToObject4x3", "float<4x3>()")                                      \
    /* Mesh and amplification shaders. */                                                          \
    X(SET_MESH_OUTPUT_COUNTS, "SetMeshOutputCounts", "void(uint, uint)")                           \
    X(DISPATCH_MESH, "DispatchMesh", "void(uint, uint, uint, udt#1)")                              \
    /* Resources and debugging. */                                                                 \
    X(NON_UNIFORM_RESOURCE_INDEX, "NonUniformResourceIndex", "uint(uint)")                         \
    X(PRINTF, "printf", "void(string, ...)")                                                       \
    X(ABORT, "abort", "void()")

typedef enum tb_shader_intrinsic
{
    TB_SHADER_INTRINSIC_NONE,
#define TB_SHADER_INTRINSIC_ENUMERATOR(identifier, name, signatures)                               \
    TB_SHADER_INTRINSIC_##identifier,
    TB_SHADER_INTRINSIC_LIST(TB_SHADER_INTRINSIC_ENUMERATOR)
#undef TB_SHADER_INTRINSIC_ENUMERATOR
    TB_SHADER_INTRINSIC_COUNT
} tb_shader_intrinsic;

typedef struct tb_shader_intrinsic_info
{
    tb_shader_intrinsic intrinsic;
    const char *name;
    const char *signatures;
} tb_shader_intrinsic_info;

/* An intrinsic's name and signatures; NULL for NONE or a value past the list. */
const tb_shader_intrinsic_info *tb_shader_intrinsic_info_of(tb_shader_intrinsic intrinsic);

#endif
