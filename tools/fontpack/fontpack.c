/* badger-fontpack: compress a font with the engine's own DEFLATE, and write
 * it as a C source that the engine library compiles in.
 *
 *     badger-fontpack <font.ttf> <output.c> <symbol>
 *
 * The output defines `const tb_font_blob <symbol>` (src/font/
 * font_embedded_data.h): the file as a zlib stream at the best level, and
 * the length it decompresses to. The stream is decompressed again and
 * compared with the file before anything is written, so a fault in the
 * compressor fails the build, not the game.
 *
 * The build runs it (cmake/EmbedFonts.cmake), so it is built for the machine
 * that builds, from the compressor's sources alone and the few functions of
 * the engine that they call (fontpack_support.c); it cannot link the engine
 * library, whose fonts it makes. It reads and writes files with the C
 * library, as a program that runs before the engine exists. */
#include "badger/deflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bytes written to a line of the array. */
#define FONTPACK_BYTES_PER_LINE 20
/* Characters a byte takes in the array: "255,". */
#define FONTPACK_CHARACTERS_PER_BYTE 4
/* Characters of the indent and the newline of a line of the array. */
#define FONTPACK_LINE_OVERHEAD 5
/* A font larger than this is not a font this engine should carry. */
#define FONTPACK_MAX_FONT ((size_t)64 << 20)

typedef struct fontpack_buffer
{
    unsigned char *bytes;
    size_t length;
} fontpack_buffer;

static void fontpack_fail(const char *message, const char *detail)
{
    fprintf(stderr, "badger-fontpack: %s%s\n", message, detail);
}

static void *fontpack_allocate(size_t size)
{
    void *memory = malloc(size == 0 ? 1 : size);
    if (memory == NULL)
    {
        fontpack_fail("out of memory", "");
        exit(EXIT_FAILURE);
    }
    return memory;
}

static int fontpack_read(const char *path, fontpack_buffer *file)
{
    int status = EXIT_FAILURE;
    FILE *stream = fopen(path, "rb");
    long size = 0;
    if (stream == NULL)
    {
        fontpack_fail("cannot open ", path);
        return EXIT_FAILURE;
    }
    if (fseek(stream, 0, SEEK_END) != 0)
    {
        fontpack_fail("cannot seek in ", path);
        goto cleanup;
    }
    size = ftell(stream);
    if (size <= 0 || (unsigned long)size > FONTPACK_MAX_FONT || fseek(stream, 0, SEEK_SET) != 0)
    {
        fontpack_fail("empty, too large, or unreadable: ", path);
        goto cleanup;
    }
    file->length = (size_t)size;
    file->bytes = fontpack_allocate(file->length);
    if (fread(file->bytes, 1, file->length, stream) != file->length)
    {
        fontpack_fail("cannot read ", path);
        goto cleanup;
    }
    status = EXIT_SUCCESS;
cleanup:
    fclose(stream);
    return status;
}

static int fontpack_compress(const fontpack_buffer *file, fontpack_buffer *stream)
{
    size_t capacity = tb_deflate_bound(file->length);
    size_t written = 0;
    unsigned char *check = NULL;
    int status = EXIT_FAILURE;
    tb_deflate_result result = TB_DEFLATE_OK;

    stream->bytes = fontpack_allocate(capacity);
    result = tb_deflate(TB_DEFLATE_ZLIB, TB_DEFLATE_BEST_LEVEL, file->bytes, file->length,
                        stream->bytes, capacity, &stream->length);
    if (result != TB_DEFLATE_OK)
    {
        fontpack_fail("cannot compress: ", tb_deflate_result_name(result));
        return EXIT_FAILURE;
    }
    check = fontpack_allocate(file->length);
    result =
        tb_inflate(TB_DEFLATE_ZLIB, stream->bytes, stream->length, check, file->length, &written);
    if (result != TB_DEFLATE_OK)
    {
        fontpack_fail("the stream does not decompress: ", tb_deflate_result_name(result));
        goto cleanup;
    }
    if (written != file->length || memcmp(check, file->bytes, file->length) != 0)
    {
        fontpack_fail("the stream decompresses to other bytes", "");
        goto cleanup;
    }
    status = EXIT_SUCCESS;
cleanup:
    free(check);
    return status;
}

/* The array's text, in one block, so that writing it is one call. */
static char *fontpack_format_array(const fontpack_buffer *stream, size_t *length)
{
    size_t lines = stream->length / FONTPACK_BYTES_PER_LINE + 1;
    size_t capacity =
        stream->length * FONTPACK_CHARACTERS_PER_BYTE + lines * FONTPACK_LINE_OVERHEAD + 1;
    char *text = fontpack_allocate(capacity);
    size_t used = 0;
    for (size_t index = 0; index < stream->length; index++)
    {
        unsigned value = stream->bytes[index];
        if (index % FONTPACK_BYTES_PER_LINE == 0)
        {
            memcpy(text + used, "\n    ", FONTPACK_LINE_OVERHEAD);
            used += FONTPACK_LINE_OVERHEAD;
        }
        if (value >= 100)
        {
            text[used++] = (char)('0' + value / 100);
        }
        if (value >= 10)
        {
            text[used++] = (char)('0' + value / 10 % 10);
        }
        text[used++] = (char)('0' + value % 10);
        text[used++] = ',';
    }
    text[used] = '\0';
    *length = used;
    return text;
}

static const char *fontpack_base_name(const char *path)
{
    const char *name = path;
    for (const char *cursor = path; *cursor != '\0'; cursor++)
    {
        if (*cursor == '/' || *cursor == '\\')
        {
            name = cursor + 1;
        }
    }
    return name;
}

static int fontpack_write(const char *path, const char *input, const char *symbol,
                          const fontpack_buffer *file, const fontpack_buffer *stream)
{
    size_t array_length = 0;
    char *array = fontpack_format_array(stream, &array_length);
    FILE *output = fopen(path, "wb");
    int status = EXIT_FAILURE;
    if (output == NULL)
    {
        fontpack_fail("cannot create ", path);
        free(array);
        return EXIT_FAILURE;
    }
    fprintf(output,
            "/* Generated by badger-fontpack from %s. Do not edit. */\n"
            "#include \"font_embedded_data.h\"\n\n"
            "static const unsigned char data[] = {%s\n};\n\n"
            "extern const tb_font_blob %s;\n\n"
            "const tb_font_blob %s = {data, sizeof data, %zu};\n",
            fontpack_base_name(input), array, symbol, symbol, file->length);
    if (fflush(output) != 0 || ferror(output) != 0)
    {
        fontpack_fail("cannot write ", path);
        goto cleanup;
    }
    status = EXIT_SUCCESS;
cleanup:
    if (fclose(output) != 0)
    {
        status = EXIT_FAILURE;
    }
    if (status != EXIT_SUCCESS)
    {
        remove(path);
    }
    free(array);
    return status;
}

int main(int argc, char **argv)
{
    fontpack_buffer file = {0};
    fontpack_buffer stream = {0};
    int status = EXIT_FAILURE;
    if (argc != 4)
    {
        fontpack_fail("usage: badger-fontpack <font.ttf> <output.c> <symbol>", "");
        return EXIT_FAILURE;
    }
    status = fontpack_read(argv[1], &file);
    if (status != EXIT_SUCCESS)
    {
        goto cleanup;
    }
    status = fontpack_compress(&file, &stream);
    if (status != EXIT_SUCCESS)
    {
        goto cleanup;
    }
    status = fontpack_write(argv[2], argv[1], argv[3], &file, &stream);
cleanup:
    free(stream.bytes);
    free(file.bytes);
    return status;
}
