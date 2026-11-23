/* The methods of HLSL's built-in objects, as data: `Sample` of a texture,
 * `Load` of a buffer, `Proceed` of a RayQuery. Each row names a method, the
 * traits an object must have for the row to be among its methods, and the
 * signatures of its overloads, in the language of shader_intrinsics.h, with
 * these additions:
 *
 *   T     the object's element: `float4` of `Texture2D`, `S` of `StructuredBuffer<S>`
 *   t     the element's component, which takes a shape: `t<4>` is a `float4` of
 *         a `Texture2D<float>`
 *   U     the type the call names (`Load<float2>`), or else what its argument
 *         is, a literal as `uint` or `float`; or else `uint`
 *   P, O, L  sizes the object decides, not the arguments: the components of
 *         a location to sample (P), of an offset or a gradient (O), and of a
 *         location to load (L)
 *   i32#1, i64#1  template 1, of `int` or `uint` (otherwise `uint`), or of
 *         `int64_t` or `uint64_t` (otherwise `uint64_t`)
 *
 * The rows follow DXC's own table of methods (gen_intrin_main.txt), grouped
 * by the traits the objects that have them share. */
#ifndef TB_SHADER_METHODS_H
#define TB_SHADER_METHODS_H

#include "shader_types.h"

#include <stdint.h>

/* What an object can do, which decides its methods. */
typedef enum tb_shader_trait
{
    TB_SHADER_TRAIT_SAMPLED = 1u << 0,         /* Sample, SampleLevel, levels of detail */
    TB_SHADER_TRAIT_COMPARED = 1u << 1,        /* SampleCmp and the comparison sampler */
    TB_SHADER_TRAIT_GATHERED = 1u << 2,        /* Gather */
    TB_SHADER_TRAIT_CUBE = 1u << 3,            /* No offsets: a clamp or a status in their place */
    TB_SHADER_TRAIT_OFFSET = 1u << 4,          /* Offsets of texels */
    TB_SHADER_TRAIT_LOAD_MIP = 1u << 5,        /* Load at a location and a mip level */
    TB_SHADER_TRAIT_LOAD_DIRECT = 1u << 6,     /* Load at a location */
    TB_SHADER_TRAIT_LOAD_SAMPLE = 1u << 7,     /* Load a sample at a location */
    TB_SHADER_TRAIT_WRITABLE = 1u << 8,        /* A UAV */
    TB_SHADER_TRAIT_FLOAT_SIZES = 1u << 9,     /* GetDimensions in floats too */
    TB_SHADER_TRAIT_DIMENSIONS_1 = 1u << 10,   /* GetDimensions writes 1 size, */
    TB_SHADER_TRAIT_DIMENSIONS_2 = 1u << 11,   /* 2, */
    TB_SHADER_TRAIT_DIMENSIONS_3 = 1u << 12,   /* 3, */
    TB_SHADER_TRAIT_DIMENSIONS_4 = 1u << 13,   /* or 4. */
    TB_SHADER_TRAIT_STRUCTURED = 1u << 14,     /* Load of a structured buffer */
    TB_SHADER_TRAIT_COUNTER = 1u << 15,        /* IncrementCounter */
    TB_SHADER_TRAIT_APPENDS = 1u << 16,        /* Append: a buffer, or a geometry shader's stream */
    TB_SHADER_TRAIT_CONSUMES = 1u << 17,       /* Consume */
    TB_SHADER_TRAIT_STREAM = 1u << 18,         /* RestartStrip */
    TB_SHADER_TRAIT_BYTE_ADDRESS = 1u << 19,   /* Load2, and Store and atomics when writable */
    TB_SHADER_TRAIT_FEEDBACK = 1u << 20,       /* WriteSamplerFeedback of a Texture2D */
    TB_SHADER_TRAIT_FEEDBACK_ARRAY = 1u << 21, /* ... of a Texture2DArray */
    TB_SHADER_TRAIT_RAY_QUERY = 1u << 22
} tb_shader_trait;

/* The traits of an object, and the sizes it decides. */
typedef struct tb_shader_object_methods
{
    uint32_t traits;
    uint32_t location; /* P: 0 when it is not sampled. */
    uint32_t offset;   /* O: 0 when it takes no offset or gradient. */
    uint32_t load;     /* L: 0 when it is not loaded at a location. */
} tb_shader_object_methods;

tb_shader_object_methods tb_shader_object_methods_of(tb_shader_object object);

/* Signatures shared by rows. */
#define TB_SHADER_OFFSETS_4 "int<O>, int<O>, int<O>, int<O>"
#define TB_SHADER_GATHER_COLOR(sampler, compare)                                                   \
    "t<4>(" sampler ", float<P>" compare ", int<O>); "                                             \
    "t<4>(" sampler ", float<P>" compare ", int<O>, out uint); "                                   \
    "t<4>(" sampler ", float<P>" compare ", " TB_SHADER_OFFSETS_4 "); "                            \
    "t<4>(" sampler ", float<P>" compare ", " TB_SHADER_OFFSETS_4 ", out uint)"
#define TB_SHADER_GATHER_ROWS(X, identifier, name, sampler, compare, offsets)                      \
    X(identifier, GATHERED, name, "t<4>(" sampler ", float<P>" compare ")")                        \
    X(identifier##_OFFSET, GATHERED | OFFSET, name, offsets)                                       \
    X(identifier##_CUBE, GATHERED | CUBE, name, "t<4>(" sampler ", float<P>" compare ", out uint)")
#define TB_SHADER_GATHER(X, identifier, name)                                                      \
    TB_SHADER_GATHER_ROWS(X, identifier, name, "SamplerState", "",                                 \
                          TB_SHADER_GATHER_COLOR("SamplerState", ""))
#define TB_SHADER_GATHER_CMP(X, identifier, name)                                                  \
    TB_SHADER_GATHER_ROWS(X, identifier, name, "SamplerComparisonState", ", float",                \
                          TB_SHADER_GATHER_COLOR("SamplerComparisonState", ", float"))
#define TB_SHADER_INTERLOCKED(type) "void(uint, " type "); void(uint, " type ", out " type ")"

/* X(identifier, traits, name, signatures). One name may have several rows;
 * an object has the overloads of each row whose traits it has, all of them. */
#define TB_SHADER_METHOD_LIST(X)                                                                   \
    /* Sampling: in each family, the overloads of every sampled texture, then                      \
     * those with offsets, then a cube's, which take no offset. */                                 \
    X(SAMPLE, SAMPLED, "Sample", "T(SamplerState, float<P>)")                                      \
    X(SAMPLE_OFFSET, SAMPLED | OFFSET, "Sample",                                                   \
      "T(SamplerState, float<P>, int<O>); T(SamplerState, float<P>, int<O>, float); "              \
      "T(SamplerState, float<P>, int<O>, float, out uint)")                                        \
    X(SAMPLE_CUBE, SAMPLED | CUBE, "Sample",                                                       \
      "T(SamplerState, float<P>, float); T(SamplerState, float<P>, float, out uint)")              \
    X(SAMPLE_BIAS, SAMPLED, "SampleBias", "T(SamplerState, float<P>, float)")                      \
    X(SAMPLE_BIAS_OFFSET, SAMPLED | OFFSET, "SampleBias",                                          \
      "T(SamplerState, float<P>, float, int<O>); "                                                 \
      "T(SamplerState, float<P>, float, int<O>, float); "                                          \
      "T(SamplerState, float<P>, float, int<O>, float, out uint)")                                 \
    X(SAMPLE_BIAS_CUBE, SAMPLED | CUBE, "SampleBias",                                              \
      "T(SamplerState, float<P>, float, float); "                                                  \
      "T(SamplerState, float<P>, float, float, out uint)")                                         \
    X(SAMPLE_LEVEL, SAMPLED, "SampleLevel", "T(SamplerState, float<P>, float)")                    \
    X(SAMPLE_LEVEL_OFFSET, SAMPLED | OFFSET, "SampleLevel",                                        \
      "T(SamplerState, float<P>, float, int<O>); "                                                 \
      "T(SamplerState, float<P>, float, int<O>, out uint)")                                        \
    X(SAMPLE_LEVEL_CUBE, SAMPLED | CUBE, "SampleLevel",                                            \
      "T(SamplerState, float<P>, float, out uint)")                                                \
    X(SAMPLE_GRAD, SAMPLED, "SampleGrad", "T(SamplerState, float<P>, float<O>, float<O>)")         \
    X(SAMPLE_GRAD_OFFSET, SAMPLED | OFFSET, "SampleGrad",                                          \
      "T(SamplerState, float<P>, float<O>, float<O>, int<O>); "                                    \
      "T(SamplerState, float<P>, float<O>, float<O>, int<O>, float); "                             \
      "T(SamplerState, float<P>, float<O>, float<O>, int<O>, float, out uint)")                    \
    X(SAMPLE_GRAD_CUBE, SAMPLED | CUBE, "SampleGrad",                                              \
      "T(SamplerState, float<P>, float<O>, float<O>, float); "                                     \
      "T(SamplerState, float<P>, float<O>, float<O>, float, out uint)")                            \
    X(SAMPLE_CMP, COMPARED, "SampleCmp", "t(SamplerComparisonState, float<P>, float)")             \
    X(SAMPLE_CMP_OFFSET, COMPARED | OFFSET, "SampleCmp",                                           \
      "t(SamplerComparisonState, float<P>, float, int<O>); "                                       \
      "t(SamplerComparisonState, float<P>, float, int<O>, float); "                                \
      "t(SamplerComparisonState, float<P>, float, int<O>, float, out uint)")                       \
    X(SAMPLE_CMP_CUBE, COMPARED | CUBE, "SampleCmp",                                               \
      "t(SamplerComparisonState, float<P>, float, float); "                                        \
      "t(SamplerComparisonState, float<P>, float, float, out uint)")                               \
    X(SAMPLE_CMP_LEVEL_ZERO, COMPARED, "SampleCmpLevelZero",                                       \
      "t(SamplerComparisonState, float<P>, float)")                                                \
    X(SAMPLE_CMP_LEVEL_ZERO_OFFSET, COMPARED | OFFSET, "SampleCmpLevelZero",                       \
      "t(SamplerComparisonState, float<P>, float, int<O>); "                                       \
      "t(SamplerComparisonState, float<P>, float, int<O>, out uint)")                              \
    X(SAMPLE_CMP_LEVEL_ZERO_CUBE, COMPARED | CUBE, "SampleCmpLevelZero",                           \
      "t(SamplerComparisonState, float<P>, float, out uint)")                                      \
    X(SAMPLE_CMP_LEVEL, COMPARED, "SampleCmpLevel",                                                \
      "t(SamplerComparisonState, float<P>, float, float)")                                         \
    X(SAMPLE_CMP_LEVEL_OFFSET, COMPARED | OFFSET, "SampleCmpLevel",                                \
      "t(SamplerComparisonState, float<P>, float, float, int<O>); "                                \
      "t(SamplerComparisonState, float<P>, float, float, int<O>, out uint)")                       \
    X(SAMPLE_CMP_LEVEL_CUBE, COMPARED | CUBE, "SampleCmpLevel",                                    \
      "t(SamplerComparisonState, float<P>, float, float, out uint)")                               \
    X(SAMPLE_CMP_BIAS, COMPARED, "SampleCmpBias",                                                  \
      "t(SamplerComparisonState, float<P>, float, float)")                                         \
    X(SAMPLE_CMP_BIAS_OFFSET, COMPARED | OFFSET, "SampleCmpBias",                                  \
      "t(SamplerComparisonState, float<P>, float, float, int<O>); "                                \
      "t(SamplerComparisonState, float<P>, float, float, int<O>, float); "                         \
      "t(SamplerComparisonState, float<P>, float, float, int<O>, float, out uint)")                \
    X(SAMPLE_CMP_BIAS_CUBE, COMPARED | CUBE, "SampleCmpBias",                                      \
      "t(SamplerComparisonState, float<P>, float, float, float); "                                 \
      "t(SamplerComparisonState, float<P>, float, float, float, out uint)")                        \
    X(SAMPLE_CMP_GRAD, COMPARED, "SampleCmpGrad",                                                  \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>)")                            \
    X(SAMPLE_CMP_GRAD_OFFSET, COMPARED | OFFSET, "SampleCmpGrad",                                  \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>, int<O>); "                   \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>, int<O>, float); "            \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>, int<O>, float, out uint)")   \
    X(SAMPLE_CMP_GRAD_CUBE, COMPARED | CUBE, "SampleCmpGrad",                                      \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>, float); "                    \
      "t(SamplerComparisonState, float<P>, float, float<O>, float<O>, float, out uint)")           \
    X(CALCULATE_LEVEL_OF_DETAIL, SAMPLED, "CalculateLevelOfDetail",                                \
      "float(SamplerState, float<O>)")                                                             \
    X(CALCULATE_LEVEL_OF_DETAIL_CMP, COMPARED, "CalculateLevelOfDetail",                           \
      "float(SamplerComparisonState, float<O>)")                                                   \
    X(CALCULATE_LEVEL_OF_DETAIL_UNCLAMPED, SAMPLED, "CalculateLevelOfDetailUnclamped",             \
      "float(SamplerState, float<O>)")                                                             \
    X(CALCULATE_LEVEL_OF_DETAIL_UNCLAMPED_CMP, COMPARED, "CalculateLevelOfDetailUnclamped",        \
      "float(SamplerComparisonState, float<O>)")                                                   \
    /* Gathering: Gather and GatherCmp take one offset; their colors, four too. */                 \
    TB_SHADER_GATHER_ROWS(X, GATHER, "Gather", "SamplerState", "",                                 \
                          "t<4>(SamplerState, float<P>, int<O>); "                                 \
                          "t<4>(SamplerState, float<P>, int<O>, out uint)")                        \
    TB_SHADER_GATHER(X, GATHER_RED, "GatherRed")                                                   \
    TB_SHADER_GATHER(X, GATHER_GREEN, "GatherGreen")                                               \
    TB_SHADER_GATHER(X, GATHER_BLUE, "GatherBlue")                                                 \
    TB_SHADER_GATHER(X, GATHER_ALPHA, "GatherAlpha")                                               \
    TB_SHADER_GATHER_ROWS(X, GATHER_CMP, "GatherCmp", "SamplerComparisonState", ", float",         \
                          "t<4>(SamplerComparisonState, float<P>, float, int<O>); "                \
                          "t<4>(SamplerComparisonState, float<P>, float, int<O>, out uint)")       \
    TB_SHADER_GATHER_CMP(X, GATHER_CMP_RED, "GatherCmpRed")                                        \
    TB_SHADER_GATHER_CMP(X, GATHER_CMP_GREEN, "GatherCmpGreen")                                    \
    TB_SHADER_GATHER_CMP(X, GATHER_CMP_BLUE, "GatherCmpBlue")                                      \
    TB_SHADER_GATHER_CMP(X, GATHER_CMP_ALPHA, "GatherCmpAlpha")                                    \
    X(GATHER_RAW, GATHERED | OFFSET, "GatherRaw",                                                  \
      "t<4>(SamplerState, float<P>); t<4>(SamplerState, float<P>, int<O>); "                       \
      "t<4>(SamplerState, float<P>, int<O>, out uint)")                                            \
    /* Sizes: of every object, in uints; of a texture, in floats too; of a                         \
     * sampled texture, of a mip level and with the number of levels. */                           \
    X(GET_DIMENSIONS_1, DIMENSIONS_1, "GetDimensions", "void(out uint)")                           \
    X(GET_DIMENSIONS_1_FLOAT, DIMENSIONS_1 | FLOAT_SIZES, "GetDimensions", "void(out float)")      \
    X(GET_DIMENSIONS_1_MIP, DIMENSIONS_1 | SAMPLED, "GetDimensions",                               \
      "void(uint, out uint, out uint); void(uint, out float, out float)")                          \
    X(GET_DIMENSIONS_2, DIMENSIONS_2, "GetDimensions", "void(out uint, out uint)")                 \
    X(GET_DIMENSIONS_2_FLOAT, DIMENSIONS_2 | FLOAT_SIZES, "GetDimensions",                         \
      "void(out float, out float)")                                                                \
    X(GET_DIMENSIONS_2_MIP, DIMENSIONS_2 | SAMPLED, "GetDimensions",                               \
      "void(uint, out uint, out uint, out uint); void(uint, out float, out float, out float)")     \
    X(GET_DIMENSIONS_3, DIMENSIONS_3, "GetDimensions", "void(out uint, out uint, out uint)")       \
    X(GET_DIMENSIONS_3_FLOAT, DIMENSIONS_3 | FLOAT_SIZES, "GetDimensions",                         \
      "void(out float, out float, out float)")                                                     \
    X(GET_DIMENSIONS_3_MIP, DIMENSIONS_3 | SAMPLED, "GetDimensions",                               \
      "void(uint, out uint, out uint, out uint, out uint); "                                       \
      "void(uint, out float, out float, out float, out float)")                                    \
    X(GET_DIMENSIONS_4, DIMENSIONS_4, "GetDimensions",                                             \
      "void(out uint, out uint, out uint, out uint)")                                              \
    X(GET_DIMENSIONS_4_FLOAT, DIMENSIONS_4 | FLOAT_SIZES, "GetDimensions",                         \
      "void(out float, out float, out float, out float)")                                          \
    /* Loading. */                                                                                 \
    X(LOAD_MIP, LOAD_MIP, "Load", "T(int<L>); T(int<L>, int<O>); T(int<L>, int<O>, out uint)")     \
    X(LOAD_DIRECT, LOAD_DIRECT, "Load", "T(int<L>); T(int<L>, out uint)")                          \
    X(LOAD_SAMPLE, LOAD_SAMPLE, "Load", "T(int<L>, int)")                                          \
    X(LOAD_SAMPLE_OFFSET, LOAD_SAMPLE | OFFSET, "Load",                                            \
      "T(int<L>, int, int<O>); T(int<L>, int, int<O>, out uint)")                                  \
    X(LOAD_SAMPLE_WRITABLE, LOAD_SAMPLE | WRITABLE, "Load", "T(int<L>, int, out uint)")            \
    X(GET_SAMPLE_POSITION, LOAD_SAMPLE, "GetSamplePosition", "float<2>(int)")                      \
    X(LOAD_STRUCTURED, STRUCTURED, "Load", "T(int); T(int, out uint)")                             \
    X(INCREMENT_COUNTER, COUNTER, "IncrementCounter", "uint()")                                    \
    X(DECREMENT_COUNTER, COUNTER, "DecrementCounter", "uint()")                                    \
    X(APPEND, APPENDS, "Append", "void(T)")                                                        \
    X(CONSUME, CONSUMES, "Consume", "T()")                                                         \
    X(RESTART_STRIP, STREAM, "RestartStrip", "void()")                                             \
    /* Byte-address buffers. */                                                                    \
    X(LOAD_BYTES, BYTE_ADDRESS, "Load", "U(uint); U(uint, out uint)")                              \
    X(LOAD2, BYTE_ADDRESS, "Load2", "uint<2>(uint); uint<2>(uint, out uint)")                      \
    X(LOAD3, BYTE_ADDRESS, "Load3", "uint<3>(uint); uint<3>(uint, out uint)")                      \
    X(LOAD4, BYTE_ADDRESS, "Load4", "uint<4>(uint); uint<4>(uint, out uint)")                      \
    X(STORE, BYTE_ADDRESS | WRITABLE, "Store", "void(uint, U)")                                    \
    X(STORE2, BYTE_ADDRESS | WRITABLE, "Store2", "void(uint, uint<2>)")                            \
    X(STORE3, BYTE_ADDRESS | WRITABLE, "Store3", "void(uint, uint<3>)")                            \
    X(STORE4, BYTE_ADDRESS | WRITABLE, "Store4", "void(uint, uint<4>)")                            \
    X(INTERLOCKED_ADD, BYTE_ADDRESS | WRITABLE, "InterlockedAdd", TB_SHADER_INTERLOCKED("uint"))   \
    X(INTERLOCKED_MIN, BYTE_ADDRESS | WRITABLE, "InterlockedMin",                                  \
      "void(uint, i32#1); void(uint, i32#1, out uint)")                                            \
    X(INTERLOCKED_MAX, BYTE_ADDRESS | WRITABLE, "InterlockedMax",                                  \
      "void(uint, i32#1); void(uint, i32#1, out uint)")                                            \
    X(INTERLOCKED_AND, BYTE_ADDRESS | WRITABLE, "InterlockedAnd", TB_SHADER_INTERLOCKED("uint"))   \
    X(INTERLOCKED_OR, BYTE_ADDRESS | WRITABLE, "InterlockedOr", TB_SHADER_INTERLOCKED("uint"))     \
    X(INTERLOCKED_XOR, BYTE_ADDRESS | WRITABLE, "InterlockedXor", TB_SHADER_INTERLOCKED("uint"))   \
    X(INTERLOCKED_COMPARE_STORE, BYTE_ADDRESS | WRITABLE, "InterlockedCompareStore",               \
      "void(uint, uint, uint)")                                                                    \
    X(INTERLOCKED_EXCHANGE, BYTE_ADDRESS | WRITABLE, "InterlockedExchange",                        \
      "void(uint, uint, out uint)")                                                                \
    X(INTERLOCKED_COMPARE_EXCHANGE, BYTE_ADDRESS | WRITABLE, "InterlockedCompareExchange",         \
      "void(uint, uint, uint, out uint)")                                                          \
    X(INTERLOCKED_ADD64, BYTE_ADDRESS | WRITABLE, "InterlockedAdd64",                              \
      TB_SHADER_INTERLOCKED("uint64_t"))                                                           \
    X(INTERLOCKED_MIN64, BYTE_ADDRESS | WRITABLE, "InterlockedMin64",                              \
      "void(uint, i64#1); void(uint, i64#1, out #1)")                                              \
    X(INTERLOCKED_MAX64, BYTE_ADDRESS | WRITABLE, "InterlockedMax64",                              \
      "void(uint, i64#1); void(uint, i64#1, out #1)")                                              \
    X(INTERLOCKED_AND64, BYTE_ADDRESS | WRITABLE, "InterlockedAnd64",                              \
      TB_SHADER_INTERLOCKED("uint64_t"))                                                           \
    X(INTERLOCKED_OR64, BYTE_ADDRESS | WRITABLE, "InterlockedOr64",                                \
      TB_SHADER_INTERLOCKED("uint64_t"))                                                           \
    X(INTERLOCKED_XOR64, BYTE_ADDRESS | WRITABLE, "InterlockedXor64",                              \
      TB_SHADER_INTERLOCKED("uint64_t"))                                                           \
    X(INTERLOCKED_COMPARE_STORE64, BYTE_ADDRESS | WRITABLE, "InterlockedCompareStore64",           \
      "void(uint, uint64_t, uint64_t)")                                                            \
    X(INTERLOCKED_EXCHANGE64, BYTE_ADDRESS | WRITABLE, "InterlockedExchange64",                    \
      "void(uint, i64#1, out #1)")                                                                 \
    X(INTERLOCKED_COMPARE_EXCHANGE64, BYTE_ADDRESS | WRITABLE, "InterlockedCompareExchange64",     \
      "void(uint, uint64_t, uint64_t, out uint64_t)")                                              \
    X(INTERLOCKED_EXCHANGE_FLOAT, BYTE_ADDRESS | WRITABLE, "InterlockedExchangeFloat",             \
      "void(uint, float, out float)")                                                              \
    X(INTERLOCKED_COMPARE_STORE_FLOAT_BITWISE, BYTE_ADDRESS | WRITABLE,                            \
      "InterlockedCompareStoreFloatBitwise", "void(uint, float, float)")                           \
    X(INTERLOCKED_COMPARE_EXCHANGE_FLOAT_BITWISE, BYTE_ADDRESS | WRITABLE,                         \
      "InterlockedCompareExchangeFloatBitwise", "void(uint, float, float, out float)")             \
    /* Sampler feedback. */                                                                        \
    X(WRITE_SAMPLER_FEEDBACK, FEEDBACK, "WriteSamplerFeedback",                                    \
      "void(Texture2D, SamplerState, float<P>); void(Texture2D, SamplerState, float<P>, float)")   \
    X(WRITE_SAMPLER_FEEDBACK_BIAS, FEEDBACK, "WriteSamplerFeedbackBias",                           \
      "void(Texture2D, SamplerState, float<P>, float); "                                           \
      "void(Texture2D, SamplerState, float<P>, float, float)")                                     \
    X(WRITE_SAMPLER_FEEDBACK_GRAD, FEEDBACK, "WriteSamplerFeedbackGrad",                           \
      "void(Texture2D, SamplerState, float<P>, float<O>, float<O>); "                              \
      "void(Texture2D, SamplerState, float<P>, float<O>, float<O>, float)")                        \
    X(WRITE_SAMPLER_FEEDBACK_LEVEL, FEEDBACK, "WriteSamplerFeedbackLevel",                         \
      "void(Texture2D, SamplerState, float<P>, float)")                                            \
    X(WRITE_SAMPLER_FEEDBACK_ARRAY, FEEDBACK_ARRAY, "WriteSamplerFeedback",                        \
      "void(Texture2DArray, SamplerState, float<P>); "                                             \
      "void(Texture2DArray, SamplerState, float<P>, float)")                                       \
    X(WRITE_SAMPLER_FEEDBACK_BIAS_ARRAY, FEEDBACK_ARRAY, "WriteSamplerFeedbackBias",               \
      "void(Texture2DArray, SamplerState, float<P>, float); "                                      \
      "void(Texture2DArray, SamplerState, float<P>, float, float)")                                \
    X(WRITE_SAMPLER_FEEDBACK_GRAD_ARRAY, FEEDBACK_ARRAY, "WriteSamplerFeedbackGrad",               \
      "void(Texture2DArray, SamplerState, float<P>, float<O>, float<O>); "                         \
      "void(Texture2DArray, SamplerState, float<P>, float<O>, float<O>, float)")                   \
    X(WRITE_SAMPLER_FEEDBACK_LEVEL_ARRAY, FEEDBACK_ARRAY, "WriteSamplerFeedbackLevel",             \
      "void(Texture2DArray, SamplerState, float<P>, float)")                                       \
    /* Inline ray tracing. */                                                                      \
    X(TRACE_RAY_INLINE, RAY_QUERY, "TraceRayInline",                                               \
      "void(RaytracingAccelerationStructure, uint, uint, RayDesc)")                                \
    X(PROCEED, RAY_QUERY, "Proceed", "bool()")                                                     \
    X(ABORT, RAY_QUERY, "Abort", "void()")                                                         \
    X(COMMIT_NON_OPAQUE_TRIANGLE_HIT, RAY_QUERY, "CommitNonOpaqueTriangleHit", "void()")           \
    X(COMMIT_PROCEDURAL_PRIMITIVE_HIT, RAY_QUERY, "CommitProceduralPrimitiveHit", "void(float)")   \
    X(COMMITTED_STATUS, RAY_QUERY, "CommittedStatus", "uint()")                                    \
    X(CANDIDATE_TYPE, RAY_QUERY, "CandidateType", "uint()")                                        \
    X(CANDIDATE_OBJECT_TO_WORLD3X4, RAY_QUERY, "CandidateObjectToWorld3x4", "float<3x4>()")        \
    X(CANDIDATE_OBJECT_TO_WORLD4X3, RAY_QUERY, "CandidateObjectToWorld4x3", "float<4x3>()")        \
    X(CANDIDATE_WORLD_TO_OBJECT3X4, RAY_QUERY, "CandidateWorldToObject3x4", "float<3x4>()")        \
    X(CANDIDATE_WORLD_TO_OBJECT4X3, RAY_QUERY, "CandidateWorldToObject4x3", "float<4x3>()")        \
    X(COMMITTED_OBJECT_TO_WORLD3X4, RAY_QUERY, "CommittedObjectToWorld3x4", "float<3x4>()")        \
    X(COMMITTED_OBJECT_TO_WORLD4X3, RAY_QUERY, "CommittedObjectToWorld4x3", "float<4x3>()")        \
    X(COMMITTED_WORLD_TO_OBJECT3X4, RAY_QUERY, "CommittedWorldToObject3x4", "float<3x4>()")        \
    X(COMMITTED_WORLD_TO_OBJECT4X3, RAY_QUERY, "CommittedWorldToObject4x3", "float<4x3>()")        \
    X(CANDIDATE_PROCEDURAL_PRIMITIVE_NON_OPAQUE, RAY_QUERY,                                        \
      "CandidateProceduralPrimitiveNonOpaque", "bool()")                                           \
    X(CANDIDATE_TRIANGLE_FRONT_FACE, RAY_QUERY, "CandidateTriangleFrontFace", "bool()")            \
    X(COMMITTED_TRIANGLE_FRONT_FACE, RAY_QUERY, "CommittedTriangleFrontFace", "bool()")            \
    X(CANDIDATE_TRIANGLE_BARYCENTRICS, RAY_QUERY, "CandidateTriangleBarycentrics", "float<2>()")   \
    X(COMMITTED_TRIANGLE_BARYCENTRICS, RAY_QUERY, "CommittedTriangleBarycentrics", "float<2>()")   \
    X(QUERY_RAY_FLAGS, RAY_QUERY, "RayFlags", "uint()")                                            \
    X(QUERY_WORLD_RAY_ORIGIN, RAY_QUERY, "WorldRayOrigin", "float<3>()")                           \
    X(QUERY_WORLD_RAY_DIRECTION, RAY_QUERY, "WorldRayDirection", "float<3>()")                     \
    X(QUERY_RAY_T_MIN, RAY_QUERY, "RayTMin", "float()")                                            \
    X(CANDIDATE_TRIANGLE_RAY_T, RAY_QUERY, "CandidateTriangleRayT", "float()")                     \
    X(COMMITTED_RAY_T, RAY_QUERY, "CommittedRayT", "float()")                                      \
    X(CANDIDATE_INSTANCE_INDEX, RAY_QUERY, "CandidateInstanceIndex", "uint()")                     \
    X(CANDIDATE_INSTANCE_ID, RAY_QUERY, "CandidateInstanceID", "uint()")                           \
    X(CANDIDATE_GEOMETRY_INDEX, RAY_QUERY, "CandidateGeometryIndex", "uint()")                     \
    X(CANDIDATE_PRIMITIVE_INDEX, RAY_QUERY, "CandidatePrimitiveIndex", "uint()")                   \
    X(CANDIDATE_OBJECT_RAY_ORIGIN, RAY_QUERY, "CandidateObjectRayOrigin", "float<3>()")            \
    X(CANDIDATE_OBJECT_RAY_DIRECTION, RAY_QUERY, "CandidateObjectRayDirection", "float<3>()")      \
    X(COMMITTED_INSTANCE_INDEX, RAY_QUERY, "CommittedInstanceIndex", "uint()")                     \
    X(COMMITTED_INSTANCE_ID, RAY_QUERY, "CommittedInstanceID", "uint()")                           \
    X(COMMITTED_GEOMETRY_INDEX, RAY_QUERY, "CommittedGeometryIndex", "uint()")                     \
    X(COMMITTED_PRIMITIVE_INDEX, RAY_QUERY, "CommittedPrimitiveIndex", "uint()")                   \
    X(COMMITTED_OBJECT_RAY_ORIGIN, RAY_QUERY, "CommittedObjectRayOrigin", "float<3>()")            \
    X(COMMITTED_OBJECT_RAY_DIRECTION, RAY_QUERY, "CommittedObjectRayDirection", "float<3>()")      \
    X(CANDIDATE_INSTANCE_CONTRIBUTION_TO_HIT_GROUP_INDEX, RAY_QUERY,                               \
      "CandidateInstanceContributionToHitGroupIndex", "uint()")                                    \
    X(COMMITTED_INSTANCE_CONTRIBUTION_TO_HIT_GROUP_INDEX, RAY_QUERY,                               \
      "CommittedInstanceContributionToHitGroupIndex", "uint()")

typedef enum tb_shader_method
{
    TB_SHADER_METHOD_NONE,
#define TB_SHADER_METHOD_ENUMERATOR(identifier, traits, name, signatures)                          \
    TB_SHADER_METHOD_##identifier,
    TB_SHADER_METHOD_LIST(TB_SHADER_METHOD_ENUMERATOR)
#undef TB_SHADER_METHOD_ENUMERATOR
    TB_SHADER_METHOD_COUNT
} tb_shader_method;

typedef struct tb_shader_method_info
{
    tb_shader_method method;
    uint32_t traits; /* The traits an object needs for this row. */
    const char *name;
    const char *signatures;
} tb_shader_method_info;

/* A row's name, traits and signatures; NULL for NONE or a value past the list. */
const tb_shader_method_info *tb_shader_method_info_of(tb_shader_method method);

#endif
