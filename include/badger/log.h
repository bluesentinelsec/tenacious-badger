/** @file log.h
 * @brief Synchronous, thread-safe logging with replaceable sinks.
 *
 * Each logger serializes configuration, filtering, formatting, and callbacks.
 * A callback borrows its record only until it returns; it must not retain it,
 * leave by longjmp, or wait for another thread that needs a logger. Calls into
 * any logger from a callback/conversion return TB_LOG_REENTRANT. This prevents
 * nested logger lock cycles. Logging is not signal-safe.
 *
 * Sink contexts and the optional category filter are borrowed until configure
 * returns after replacing them, or destroy returns. Configure waits for prior
 * callbacks. Sharing a sink across loggers requires caller synchronization.
 */
#ifndef TB_LOG_H
#define TB_LOG_H
#include "badger/api.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/** @brief Severity in increasing order; FATAL labels a record and never aborts. */
typedef enum tb_log_level
{
    TB_LOG_TRACE,   /**< Detailed tracing. */
    TB_LOG_DEBUG,   /**< Developer diagnostics. */
    TB_LOG_INFO,    /**< Ordinary progress. */
    TB_LOG_WARNING, /**< Recoverable problem. */
    TB_LOG_ERROR,   /**< Operation failed. */
    TB_LOG_FATAL,   /**< Application cannot continue; caller decides how to exit. */
    TB_LOG_OFF      /**< Filter everything; invalid as a record severity. */
} tb_log_level;
/** @brief Result of a logging operation. */
typedef enum tb_log_result
{
    TB_LOG_OK,        /**< Accepted by the sink. */
    TB_LOG_FILTERED,  /**< Rejected by configured severity/category; no callback. */
    TB_LOG_INVALID,   /**< Invalid level, UTF-8, embedded NUL, or configuration. */
    TB_LOG_IO,        /**< Sink write/flush failed; a partial record may exist. */
    TB_LOG_REENTRANT, /**< Called inside a logger callback or format conversion. */
    TB_LOG_TOO_LARGE  /**< Formatted message exceeds TB_LOG_MESSAGE_CAPACITY - 1. */
} tb_log_result;
/** @brief Stack buffer size for formatted messages, including their terminator. */
#define TB_LOG_MESSAGE_CAPACITY 4096
/** @brief One borrowed UTF-8 record; no embedded NUL in category or message. */
typedef struct tb_log_record
{
    tb_log_level level;   /**< Record severity. */
    const char *category; /**< Non-null terminated label, possibly empty. */
    const char *message;  /**< Non-null bounded message; need not be terminated. */
    size_t length;        /**< Message bytes, excluding any terminator. */
} tb_log_record;
/** @brief Accept one record synchronously; false reports a write failure.
 * @param context Borrowed sink state, possibly NULL.
 * @param record Borrowed for this call only; required non-null.
 */
typedef bool tb_log_sink_write(void *context, const tb_log_record *record);
/** @brief Flush a sink; false reports failure. Context is borrowed, possibly NULL. */
typedef bool tb_log_sink_flush(void *context);
/** @brief Borrowed configuration copied into the logger by configure. */
typedef struct tb_log_config
{
    tb_log_level minimum;            /**< Accept this severity and above, or OFF. */
    const char *category;            /**< NULL accepts all; otherwise exact UTF-8 category match. */
    tb_log_sink_write *write_record; /**< Required callback. */
    tb_log_sink_flush *flush;        /**< Optional; NULL means no buffering to flush. */
    void *context;                   /**< Borrowed by callbacks until configuration is replaced. */
} tb_log_config;
/** @brief INFO and above, all categories, standard error sink. */
#define TB_LOG_CONFIG_INIT {TB_LOG_INFO, NULL, tb_log_stderr_write, tb_log_stderr_flush, NULL}
/** @brief Opaque logger owned by its creator. */
typedef struct tb_log tb_log;
/** @brief Create a default logger; never NULL, allocation exhaustion is fatal.
 * @return Owned logger; destroy after all users have stopped. Thread-safe.
 */
TB_API tb_log *tb_log_create(void);
/** @brief Flush and destroy; NULL succeeds. Returns IO if the final flush fails.
 * The logger is destroyed even on failure. Exclusive lifetime access required;
 * calling from a callback returns REENTRANT without destroying it.
 */
TB_API tb_log_result tb_log_destroy(tb_log *log);
/** @brief Atomically replace configuration; required pointers, thread-safe.
 * Validates minimum, write callback, and UTF-8 category before changing state.
 * Does not flush the previous sink; call flush first when required. Borrowed
 * configuration fields must stay alive until replaced. Returns OK, INVALID,
 * or REENTRANT. Once it returns OK, old borrowed state can be released.
 */
TB_API tb_log_result tb_log_configure(tb_log *log, const tb_log_config *config);
/** @brief Submit bounded UTF-8; NULL message only at zero length; thread-safe.
 * @param log Required logger.
 * @param level TRACE through FATAL.
 * @param category Required terminated UTF-8 label.
 * @param message Borrowed bytes for this call, without embedded NUL.
 * @param length Number of bytes.
 * @return OK, FILTERED, INVALID, IO, or REENTRANT. Filtered messages are not
 * inspected. Sinks may emit partial output on IO; the logger remains usable.
 */
TB_API tb_log_result tb_log_write(tb_log *log, tb_log_level level, const char *category,
                                  const char *message, size_t length);
/** @brief Format with badger/fmt.h, then submit; required format and category.
 * Thread-safe; filtered records are not formatted. In addition to write results,
 * returns TOO_LARGE without emission if the stack buffer cannot hold the text.
 * Formats are trusted program strings; no allocation occurs during formatting.
 */
TB_API tb_log_result tb_log_printf(tb_log *log, tb_log_level level, const char *category,
                                   const char *format, ...);
/** @brief va_list form of printf; does not consume arguments. Same contracts. */
TB_API tb_log_result tb_log_vprintf(tb_log *log, tb_log_level level, const char *category,
                                    const char *format, va_list arguments);
/** @brief Flush synchronously; required logger; thread-safe. OK, IO, or REENTRANT. */
TB_API tb_log_result tb_log_flush(tb_log *log);
/** @brief Built-in stderr record sink: `[LEVEL] [category] message\n`.
 * Ignores context. Serializes only through its owning logger; other stderr users
 * may interleave. Flushes stderr on every record and reports its error state.
 */
TB_API bool tb_log_stderr_write(void *context, const tb_log_record *record);
/** @brief Flush stderr only; context ignored. Reports prior stream errors too. */
TB_API bool tb_log_stderr_flush(void *context);
/** @brief Built-in file record sink, with the same format as stderr.
 * Context is a required borrowed open tb_file* with write access, exclusively
 * accessed by this logger until reconfigured/destroyed. Never closes the file;
 * writes at its current cursor. The caller opens, positions, and closes it.
 */
TB_API bool tb_log_file_write(void *context, const tb_log_record *record);
/** @brief Flush the required borrowed tb_file* context; never closes it. */
TB_API bool tb_log_file_flush(void *context);
#endif
