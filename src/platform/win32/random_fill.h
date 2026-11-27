/* The Windows provider fills a whole chunk or returns a failing NTSTATUS. */
#ifndef TB_WIN32_RANDOM_FILL_H
#define TB_WIN32_RANDOM_FILL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>

#include <stdbool.h>
#include <stddef.h>

typedef NTSTATUS WINAPI tb_win32_gen_random(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer,
                                            ULONG length, ULONG flags);

bool tb_win32_random_fill(unsigned char *output, size_t length, ULONG chunk_limit,
                          tb_win32_gen_random *generate);

#endif
