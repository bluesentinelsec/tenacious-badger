/* Private to the Windows adapter, and to its tests: the arguments of a
 * command line as UTF-8. */
#ifndef TB_WIN32_PROCESS_ARGUMENTS_H
#define TB_WIN32_PROCESS_ARGUMENTS_H

#include "badger/process.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Splits `command_line` as CommandLineToArgvW does, converts each argument
 * to UTF-8, and fills `arguments` with them, in one allocation from malloc
 * that tb_process_release_arguments() frees. An unpaired surrogate becomes
 * U+FFFD. Returns false when the system is out of memory, leaving
 * `arguments` as it was. Both are required. */
bool tb_win32_arguments_from(const WCHAR *command_line, tb_arguments *arguments);

#endif
