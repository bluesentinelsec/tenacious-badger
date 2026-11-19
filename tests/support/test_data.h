/* Files that tests embed: tb_embed_test_data in tests/CMakeLists.txt writes a
 * C source that defines an array of these from a directory of tests/data. A
 * test declares the array it reads:
 *
 *     extern const tb_test_file tb_example_files[];
 *     extern const size_t tb_example_files_count;
 */
#ifndef TB_TEST_DATA_H
#define TB_TEST_DATA_H

#include <stddef.h>

/** One embedded file. */
typedef struct tb_test_file
{
    const char *name;           /**< Its name within its directory. */
    const unsigned char *bytes; /**< Its bytes, followed by a zero not counted in length. */
    size_t length;              /**< Its length in bytes. */
} tb_test_file;

#endif
