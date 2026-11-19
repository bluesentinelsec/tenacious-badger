/* The layout of a package, version 1, shared by the reader and the writer.
 * docs/packages.md describes it field by field. */
#ifndef TB_CODEC_PACKAGE_INTERNAL_H
#define TB_CODEC_PACKAGE_INTERNAL_H

#include "badger/chacha20.h"
#include "badger/package.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_PACKAGE_VERSION 1u

/* The header, at offset 0. */
#define TB_PACKAGE_HEADER_SIZE ((size_t)64)
#define TB_PACKAGE_MAGIC "TBPK\r\n\x1a\n" /* 8 bytes: a text-mode copy breaks it. */
#define TB_PACKAGE_MAGIC_SIZE ((size_t)8)
#define TB_PACKAGE_HEADER_VERSION 8u /* u16 */
#define TB_PACKAGE_HEADER_FLAGS 10u  /* u16 */
#define TB_PACKAGE_HEADER_SALT 16u   /* 16 bytes */
#define TB_PACKAGE_HEADER_CHECK 32u  /* 8 bytes */
#define TB_PACKAGE_SALT_SIZE ((size_t)16)
#define TB_PACKAGE_CHECK_SIZE ((size_t)8)

/* Flags of the header. */
#define TB_PACKAGE_FLAG_OBFUSCATED 0x0001u
#define TB_PACKAGE_KNOWN_FLAGS TB_PACKAGE_FLAG_OBFUSCATED

/* The footer, the last bytes of the package. */
#define TB_PACKAGE_FOOTER_SIZE ((size_t)48)
#define TB_PACKAGE_FOOTER_INDEX_OFFSET 0u /* u64 */
#define TB_PACKAGE_FOOTER_INDEX_SIZE 8u   /* u64 */
#define TB_PACKAGE_FOOTER_COUNT 16u       /* u64 */
#define TB_PACKAGE_FOOTER_INDEX_CRC 24u   /* u32 */
#define TB_PACKAGE_FOOTER_MAGIC 40u       /* 8 bytes */
#define TB_PACKAGE_END_MAGIC "TBPKEND\n"

/* A record of the index, before its name. */
#define TB_PACKAGE_RECORD_SIZE ((size_t)32)
#define TB_PACKAGE_RECORD_NAME_LENGTH 0u /* u16 */
#define TB_PACKAGE_RECORD_CODEC 2u       /* u8 */
#define TB_PACKAGE_RECORD_CRC 4u         /* u32 */
#define TB_PACKAGE_RECORD_OFFSET 8u      /* u64 */
#define TB_PACKAGE_RECORD_STORED 16u     /* u64 */
#define TB_PACKAGE_RECORD_SIZE_FIELD 24u /* u64 */

/* Entries begin at offsets that are multiples of this. */
#define TB_PACKAGE_ALIGNMENT ((uint64_t)16)

/* The longest name. */
#define TB_PACKAGE_MAX_NAME ((size_t)0xFFFF)

/* DEFLATE expands at most 1032 to 1, with some room for small entries. */
#define TB_PACKAGE_MAX_RATIO UINT64_C(1032)
#define TB_PACKAGE_RATIO_SLACK UINT64_C(1024)

/* What a package's own key and its check are derived from. */
#define TB_PACKAGE_KEY_LABEL "Tenacious Badger package v1 key:"
#define TB_PACKAGE_CHECK_LABEL "Tenacious Badger package v1 check"

/* The package's key: HMAC-SHA256 of the label and the salt under the game's
 * key; and the first bytes of the check, HMAC-SHA256 of the check label
 * under the package's key. */
void tb_package_derive_key(const uint8_t game_key[TB_PACKAGE_KEY_SIZE],
                           const uint8_t salt[TB_PACKAGE_SALT_SIZE],
                           uint8_t package_key[TB_CHACHA20_KEY_SIZE],
                           uint8_t check[TB_PACKAGE_CHECK_SIZE]);

/* The nonce of every package: the key differs for each, by its salt. */
extern const uint8_t tb_package_nonce[TB_CHACHA20_NONCE_SIZE];

/* Clear memory that held a key, in a way no compiler removes. */
void tb_package_wipe(void *memory, size_t length);

/* Little-endian fields. */
uint16_t tb_package_get16(const unsigned char *bytes);
uint32_t tb_package_get32(const unsigned char *bytes);
uint64_t tb_package_get64(const unsigned char *bytes);
void tb_package_put16(unsigned char *bytes, uint32_t value);
void tb_package_put32(unsigned char *bytes, uint32_t value);
void tb_package_put64(unsigned char *bytes, uint64_t value);

#endif
