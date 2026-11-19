/* The layout of ZIP records (PKWARE's APPNOTE), shared by the reader and the writer. */
#ifndef TB_CODEC_ZIP_INTERNAL_H
#define TB_CODEC_ZIP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Signatures, as the little-endian numbers they are read as. */
#define TB_ZIP_LOCAL_SIGNATURE UINT32_C(0x04034B50)
#define TB_ZIP_CENTRAL_SIGNATURE UINT32_C(0x02014B50)
#define TB_ZIP_END_SIGNATURE UINT32_C(0x06054B50)
#define TB_ZIP_END64_SIGNATURE UINT32_C(0x06064B50)
#define TB_ZIP_LOCATOR_SIGNATURE UINT32_C(0x07064B50)
#define TB_ZIP_DESCRIPTOR_SIGNATURE UINT32_C(0x08074B50)

/* Fixed lengths of records, before their names, extra fields, and comments. */
#define TB_ZIP_LOCAL_LENGTH ((size_t)30)
#define TB_ZIP_CENTRAL_LENGTH ((size_t)46)
#define TB_ZIP_END_LENGTH ((size_t)22)
#define TB_ZIP_END64_LENGTH ((size_t)56)
#define TB_ZIP_LOCATOR_LENGTH ((size_t)20)

/* The longest a comment, a name, or an extra field can be. */
#define TB_ZIP_MAX_FIELD ((size_t)0xFFFF)

/* Field values that mean "see the ZIP64 record". */
#define TB_ZIP_MAX_16 UINT32_C(0xFFFF)
#define TB_ZIP_MAX_32 UINT64_C(0xFFFFFFFF)

/* General purpose flags. */
#define TB_ZIP_FLAG_ENCRYPTED 0x0001u
#define TB_ZIP_FLAG_DESCRIPTOR 0x0008u
#define TB_ZIP_FLAG_STRONG_ENCRYPTION 0x0040u
#define TB_ZIP_FLAG_UTF8 0x0800u
#define TB_ZIP_FLAG_MASKED_HEADERS 0x2000u

/* Extra fields. */
#define TB_ZIP_EXTRA_ZIP64 0x0001u
#define TB_ZIP_EXTRA_TIMESTAMP 0x5455u
#define TB_ZIP_EXTRA_UNICODE_PATH 0x7075u

/* The system that made an entry, in the high byte of "version made by". */
#define TB_ZIP_HOST_UNIX 3u

/* Unix file types, in the high 16 bits of the external attributes. */
#define TB_ZIP_UNIX_TYPE_MASK 0170000u
#define TB_ZIP_UNIX_DIRECTORY 0040000u
#define TB_ZIP_UNIX_FILE 0100000u
#define TB_ZIP_UNIX_SYMLINK 0120000u
#define TB_ZIP_UNIX_MODE_MASK 07777u

/* The MS-DOS directory attribute, in the low byte of the external attributes. */
#define TB_ZIP_DOS_DIRECTORY 0x10u

/* DEFLATE expands at most 1032 to 1; a claim beyond this ratio, with some
 * room for small entries, is not DEFLATE. */
#define TB_ZIP_MAX_RATIO UINT64_C(1032)
#define TB_ZIP_RATIO_SLACK UINT64_C(1024)

/* 1980-01-01 00:00:00 UTC, the first DOS time, and 2107-12-31 23:59:58, the last. */
#define TB_ZIP_DOS_FIRST INT64_C(315532800)
#define TB_ZIP_DOS_LAST INT64_C(4354819198)

static inline uint16_t tb_zip_get16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | (unsigned)bytes[1] << 8);
}

static inline uint32_t tb_zip_get32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static inline uint64_t tb_zip_get64(const unsigned char *bytes)
{
    return (uint64_t)tb_zip_get32(bytes) | (uint64_t)tb_zip_get32(bytes + 4) << 32;
}

static inline void tb_zip_put16(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static inline void tb_zip_put32(unsigned char *bytes, uint64_t value)
{
    for (size_t index = 0; index < 4; index++)
    {
        bytes[index] = (unsigned char)((value >> (8 * index)) & 0xFFu);
    }
}

static inline void tb_zip_put64(unsigned char *bytes, uint64_t value)
{
    tb_zip_put32(bytes, value & TB_ZIP_MAX_32);
    tb_zip_put32(bytes + 4, value >> 32);
}

/* Days from 1970-01-01 to a date of the proleptic Gregorian calendar, and back. */
int64_t tb_zip_days_from_civil(int64_t year, unsigned month, unsigned day);
void tb_zip_civil_from_days(int64_t days, int64_t *year, unsigned *month, unsigned *day);

#endif
