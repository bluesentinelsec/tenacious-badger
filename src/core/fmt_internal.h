/* What fmt.c shares with the other sources of the formatter. Private to the
 * engine. */
#ifndef TB_FMT_INTERNAL_H
#define TB_FMT_INTERNAL_H

#include "badger/fmt.h"

#include <stdarg.h>

/* The conversions %f, %F, %e, %E, %g, %G, %a, and %A: takes a double. */
void tb_fmt_convert_float(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments);

#endif
