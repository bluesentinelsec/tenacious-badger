/* Captures as .bmp files: 32-bit BGRA, top row first, which every image
 * viewer opens and which needs no compression. */
#include "sandbox.h"

#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

#define SANDBOX_BMP_FILE_HEADER 14u
#define SANDBOX_BMP_INFO_HEADER 40u

static void sandbox_put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

bool sandbox_write_bmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height,
                       uint32_t pitch)
{
    const size_t row = (size_t)width * 4u;
    const size_t header = SANDBOX_BMP_FILE_HEADER + SANDBOX_BMP_INFO_HEADER;
    const size_t size = header + row * height;
    uint8_t *file = NULL;
    bool written = false;

    if (size > UINT32_MAX)
    {
        return false;
    }
    file = tb_calloc(size, 1);
    file[0] = 'B';
    file[1] = 'M';
    sandbox_put32(&file[2], (uint32_t)size);
    sandbox_put32(&file[10], (uint32_t)header);
    sandbox_put32(&file[14], SANDBOX_BMP_INFO_HEADER);
    sandbox_put32(&file[18], width);
    /* A negative height says the rows go top first. */
    sandbox_put32(&file[22], (uint32_t)-(int32_t)height);
    file[26] = 1;  /* planes */
    file[28] = 32; /* bits a pixel */
    sandbox_put32(&file[34], (uint32_t)(row * height));
    for (uint32_t y = 0; y < height; y++)
    {
        memcpy(&file[header + row * y], &pixels[(size_t)pitch * y], row);
    }
    written = tb_fs_write_file((tb_path_view){path, tb_str_length(path)}, file, size) == TB_FS_OK;
    tb_free(file);
    return written;
}
