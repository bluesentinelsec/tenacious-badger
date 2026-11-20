/* Private to the Linux and macOS adapters: what each system's signal context
 * says about where a crash happened, which POSIX leaves to the system. */
#ifndef TB_POSIX_CRASH_CONTEXT_H
#define TB_POSIX_CRASH_CONTEXT_H

/* The address of the instruction that raised a signal, from the context a
 * handler is given, or NULL where the system or processor is not known. */
void *tb_posix_crash_instruction(const void *context);

#endif
