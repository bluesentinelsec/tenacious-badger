/* The layout of a TAR header, shared by the reader and the writer. */
#ifndef TB_CODEC_TAR_INTERNAL_H
#define TB_CODEC_TAR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Everything in an archive is in blocks of this many bytes. */
#define TB_TAR_BLOCK ((size_t)512)

/* Fields of a ustar header: offset and width in bytes. */
#define TB_TAR_NAME 0u
#define TB_TAR_NAME_WIDTH 100u
#define TB_TAR_MODE 100u
#define TB_TAR_MODE_WIDTH 8u
#define TB_TAR_UID 108u
#define TB_TAR_GID 116u
#define TB_TAR_ID_WIDTH 8u
#define TB_TAR_SIZE 124u
#define TB_TAR_SIZE_WIDTH 12u
#define TB_TAR_MTIME 136u
#define TB_TAR_MTIME_WIDTH 12u
#define TB_TAR_CHECKSUM 148u
#define TB_TAR_CHECKSUM_WIDTH 8u
#define TB_TAR_TYPE 156u
#define TB_TAR_LINK 157u
#define TB_TAR_LINK_WIDTH 100u
#define TB_TAR_MAGIC 257u
#define TB_TAR_MAGIC_WIDTH 6u
#define TB_TAR_VERSION 263u
#define TB_TAR_VERSION_WIDTH 2u
#define TB_TAR_DEVMAJOR 329u
#define TB_TAR_DEVMINOR 337u
#define TB_TAR_DEV_WIDTH 8u
#define TB_TAR_PREFIX 345u
#define TB_TAR_PREFIX_WIDTH 155u

/* The largest number of 11 octal digits, the most a size or a time holds in
 * a ustar header: 8 GiB less one byte, or a time in the year 2242. */
#define TB_TAR_OCTAL_11_MAX UINT64_C(077777777777)

/* Permission bits, and the set-id and sticky bits. */
#define TB_TAR_MODE_MASK 07777u

/* The sum of a header's bytes with its checksum field read as spaces, the
 * bytes taken as unsigned, which POSIX defines, and as signed, which some old
 * writers used. */
void tb_tar_header_sums(const unsigned char *header, uint32_t *unsigned_sum, int32_t *signed_sum);

#endif
