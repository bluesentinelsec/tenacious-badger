/* Private to the default adapter: the one way its functions fail. */
#ifndef TB_DEFAULT_NOT_IMPLEMENTED_H
#define TB_DEFAULT_NOT_IMPLEMENTED_H

/* Reports on the standard error stream that a function of a platform
 * interface is not implemented on this platform, and what to do about it,
 * then ends the process with a failure status. `interface_name` is the
 * interface as diagnostics name it, such as "platform.time", and
 * `function_name` the public function that was called. Both are required
 * non-null. Does not return. */
_Noreturn void tb_default_not_implemented(const char *interface_name, const char *function_name);

#endif
