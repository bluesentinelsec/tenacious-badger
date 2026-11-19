/* What the TAR reader and writer share: header checksums and result names. */
#include "badger/tar.h"

#include "tar_internal.h"

void tb_tar_header_sums(const unsigned char *header, uint32_t *unsigned_sum, int32_t *signed_sum)
{
    uint32_t sum = 0;
    int32_t signed_total = 0;
    for (size_t index = 0; index < TB_TAR_BLOCK; index++)
    {
        const int inside =
            index >= TB_TAR_CHECKSUM && index < TB_TAR_CHECKSUM + TB_TAR_CHECKSUM_WIDTH;
        const unsigned char byte = inside ? (unsigned char)' ' : header[index];
        sum += byte;
        signed_total += (int32_t)(signed char)byte;
    }
    *unsigned_sum = sum;
    *signed_sum = signed_total;
}

const char *tb_tar_result_name(tb_tar_result result)
{
    switch (result)
    {
    case TB_TAR_OK:
        return "ok";
    case TB_TAR_END:
        return "end";
    case TB_TAR_INVALID:
        return "invalid";
    case TB_TAR_MALFORMED:
        return "malformed";
    case TB_TAR_TRUNCATED:
        return "truncated";
    case TB_TAR_TOO_LARGE:
        return "too large";
    case TB_TAR_UNSUPPORTED:
        return "unsupported";
    case TB_TAR_INPUT_FAILED:
        return "input failed";
    case TB_TAR_OUTPUT_FAILED:
        return "output failed";
    default:
        return "unknown result";
    }
}
