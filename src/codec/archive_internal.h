/* What the archive formats share. */
#ifndef TB_CODEC_ARCHIVE_INTERNAL_H
#define TB_CODEC_ARCHIVE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/* Whether a name is text a writer may store: not empty, UTF-8, and without NUL. */
bool tb_archive_is_text(const char *text, size_t length);

/* Whether a name is a relative path a writer may store: text, whose
 * components are neither empty, ".", nor "..", so it has no slash at either
 * end and none doubled. */
bool tb_archive_is_relative_path(const char *name, size_t length);

#endif
