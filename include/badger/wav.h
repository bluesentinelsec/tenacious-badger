/** @file wav.h
 * @brief WAV files (`.wav`): read into sounds (badger/sound.h), whole or a
 * block of frames at a time, and written from them.
 *
 * @code
 * tb_sound *jump = NULL;
 * tb_wav_error error = {0};
 * if (tb_wav_load_file(vfs, "sounds/jump.wav", NULL, &jump, &error) != TB_WAV_OK)
 * {
 *     tb_log_error("audio", "%s", error.message);  // "sounds/jump.wav: fmt  chunk at byte 12: ..."
 * }
 * @endcode
 *
 * **What is read.** A RIFF file of form `WAVE`, or an RF64 or BW64 file,
 * whose `fmt ` chunk is one of:
 *
 * | Format tag | Bits | Read as |
 * | --- | --- | --- |
 * | WAVE_FORMAT_PCM (1) | 1 to 8 | U8 |
 * | WAVE_FORMAT_PCM | 9 to 16 | S16 |
 * | WAVE_FORMAT_PCM | 17 to 24 | S24 |
 * | WAVE_FORMAT_PCM | 25 to 32 | S32 |
 * | WAVE_FORMAT_IEEE_FLOAT (3) | 32 | F32 |
 * | WAVE_FORMAT_IEEE_FLOAT | 64 | F32, rounded; NaN becomes silence, and the rest is clamped to the
 * range of a float | | WAVE_FORMAT_ALAW (6), WAVE_FORMAT_MULAW (7) | 8 | S16, expanded as G.711
 * says | | WAVE_FORMAT_IMA_ADPCM (0x11) | 4 | S16, decoded as the IMA's recommendation says | |
 * WAVE_FORMAT_EXTENSIBLE (0xFFFE) | as above | as its sub-format, with its channel mask |
 *
 * Samples whose bits do not fill their bytes are read by their bytes: a
 * 12-bit sample in two bytes is S16. Formats are kept as close to the file
 * as a sound can hold, so that writing a sound back gives the same samples;
 * a mixer converts what it plays once, when it loads it.
 *
 * The `smpl` chunk gives the sound its loops; a loop that does not lie
 * within the sound's frames is dropped, and loops past #TB_SOUND_MAX_LOOPS
 * too. The `fact` chunk bounds the frames of IMA ADPCM. Every other chunk
 * is skipped: `LIST`, `cue `, `bext`, `JUNK`, and any of a name unknown.
 *
 * **The bytes are untrusted.** Every size is checked against the bytes
 * there are, and against the limits of badger/sound.h; a file that breaks
 * the format is MALFORMED with the chunk and byte at fault, never a read
 * past the end. A chunk of an odd size is followed by one byte of padding,
 * as RIFF requires; the padding of the last chunk may be missing.
 *
 * **What is written.** A sound's samples in its own format: U8, S16, S24
 * and S32 as WAVE_FORMAT_PCM and F32 as WAVE_FORMAT_IEEE_FLOAT, inside
 * WAVE_FORMAT_EXTENSIBLE where the format's documentation requires it:
 * for more than 2 channels, for more than 16 bits, or for a channel mask
 * other than the default. Loops are written as a `smpl` chunk.
 *
 * **Streaming.** A tb_wav_reader decodes a file a block of frames at a
 * time, from a stream it borrows, so music plays without being read whole;
 * tb_wav_read_stream() is a reader that reads everything.
 *
 * @par Threading
 * tb_wav_read(), tb_wav_read_stream(), tb_wav_load_file() and
 * tb_wav_write() are thread-safe for different sounds and streams; each
 * stream they are given is externally synchronized, as streams are. A
 * reader is externally synchronized. tb_wav_result_name() is thread-safe.
 */
#ifndef TB_WAV_H
#define TB_WAV_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/sound.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

struct tb_vfs;

/** @brief What happened, or why not. */
typedef enum tb_wav_result
{
    TB_WAV_OK,            /**< Success. */
    TB_WAV_NOT_FOUND,     /**< The VFS has no such file. */
    TB_WAV_INPUT_FAILED,  /**< The stream or the VFS could not be read. */
    TB_WAV_MALFORMED,     /**< The bytes are not a WAV file, or are damaged or cut short. */
    TB_WAV_UNSUPPORTED,   /**< A valid file this reader cannot read, such as MP3 in a WAV. */
    TB_WAV_TOO_LARGE,     /**< Past a limit: of badger/sound.h, or 4 GiB of samples to write. */
    TB_WAV_NO_MEMORY,     /**< There is no memory for the samples. */
    TB_WAV_OUTPUT_FAILED, /**< The stream refused what was written. */
    TB_WAV_UNSEEKABLE     /**< A reader was asked to seek in a stream that cannot. */
} tb_wav_result;

/** @brief Bytes of the message of #tb_wav_error. */
#define TB_WAV_ERROR_CAPACITY 256

/** @brief Where a WAV file is wrong, and why. */
typedef struct tb_wav_error
{
    tb_wav_result result; /**< As the function returned. */
    /** The four letters of the chunk at fault, such as "fmt "; "" when the
     * fault is in none, as for a file that is not RIFF. */
    char chunk[5];
    uint64_t offset;    /**< The byte of the file at fault, from 0 at its start. */
    const char *reason; /**< A short phrase: "no channels"; static; "" for none. */
    /** All of it, for a log: "jump.wav: fmt  chunk at byte 20: no channels". */
    char message[TB_WAV_ERROR_CAPACITY];
} tb_wav_error;

/**
 * @brief Read a WAV file from memory into a sound that owns its samples.
 * @param bytes The file; NULL only at length zero. Not kept.
 * @param length Bytes of the file.
 * @param allocator Where the sound lives, or NULL for the heap.
 * @param[out] sound Required; the sound on OK; NULL otherwise.
 * @param[out] error Optional; why it failed.
 * @return OK, MALFORMED, UNSUPPORTED, TOO_LARGE, or NO_MEMORY.
 */
TB_API tb_wav_result tb_wav_read(const void *bytes, size_t length, const tb_allocator *allocator,
                                 tb_sound **sound, tb_wav_error *error);

/**
 * @brief Read a WAV file from a stream, from its position, into a sound.
 *
 * A stream that can seek is read as a file is: chunks after the samples,
 * such as `smpl`, are found. One that cannot is read in order, and what
 * follows the `data` chunk is not read.
 * @param stream Required; open for reading. Left after what was read.
 * @param allocator Where the sound lives, or NULL for the heap.
 * @param[out] sound Required; the sound on OK; NULL otherwise.
 * @param[out] error Optional; why it failed.
 * @return As tb_wav_read(), or INPUT_FAILED when the stream fails.
 */
TB_API tb_wav_result tb_wav_read_stream(tb_stream *stream, const tb_allocator *allocator,
                                        tb_sound **sound, tb_wav_error *error);

/**
 * @brief tb_wav_read_stream() of a file read through a VFS.
 * @param vfs Required. @param path The file's name in the VFS; required.
 * @param allocator Where the sound lives, or NULL for the heap.
 * @param[out] sound Required. @param[out] error Optional; its message starts with `path`.
 * @return As tb_wav_read_stream(), or NOT_FOUND.
 */
TB_API tb_wav_result tb_wav_load_file(struct tb_vfs *vfs, const char *path,
                                      const tb_allocator *allocator, tb_sound **sound,
                                      tb_wav_error *error);

/**
 * @brief Write a sound as a WAV file, at a stream's position.
 * @param sound Required.
 * @param stream Required; open for writing.
 * @param[out] error Optional; why it failed.
 * @return OK; TOO_LARGE for samples of 4 GiB or more, before anything is
 * written; OUTPUT_FAILED when the stream refuses, having written part.
 */
TB_API tb_wav_result tb_wav_write(const tb_sound *sound, tb_stream *stream, tb_wav_error *error);

/* -------------------------------------------------------------------------
 * Reading a block at a time.
 * ------------------------------------------------------------------------- */

/** @brief What a reader found in a file's header. */
typedef struct tb_wav_info
{
    tb_sound_spec spec;       /**< What the frames are decoded to. */
    size_t frame_count;       /**< Frames in the file. */
    uint16_t format_tag;      /**< The file's own format: 1 for PCM, 0x11 for IMA ADPCM. */
    uint16_t bits_per_sample; /**< As the `fmt ` chunk says. */
    size_t loop_count;        /**< Loops in `loops`. */
    tb_sound_loop loops[TB_SOUND_MAX_LOOPS]; /**< From the `smpl` chunk. */
} tb_wav_info;

/** @brief A WAV file open for decoding a block at a time. Opaque. */
typedef struct tb_wav_reader tb_wav_reader;

/**
 * @brief Read a WAV file's header from a stream, ready to decode its frames.
 * @param stream Required; open for reading, at the file's first byte.
 * Borrowed until the reader is closed; the reader alone moves it meanwhile.
 * @param allocator Where the reader lives, or NULL for the heap.
 * @param[out] reader Required; the reader on OK, for tb_wav_reader_close(); NULL otherwise.
 * @param[out] error Optional; why it failed.
 * @return OK, MALFORMED, UNSUPPORTED, TOO_LARGE, or INPUT_FAILED.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_wav_result tb_wav_reader_open(tb_stream *stream, const tb_allocator *allocator,
                                        tb_wav_reader **reader, tb_wav_error *error);

/** @brief What the reader found. @param reader Required. @param[out] info Required. */
TB_API void tb_wav_reader_info(const tb_wav_reader *reader, tb_wav_info *info);

/** @brief The frame the next read decodes first. @param reader Required. */
TB_API size_t tb_wav_reader_position(const tb_wav_reader *reader);

/**
 * @brief Decode the next frames, in the format of the info's spec.
 * @param reader Required.
 * @param[out] samples Required unless `length` is 0; interleaved frames.
 * @param length Bytes of `samples`; whole frames of it are filled.
 * @param[out] frames Required; frames decoded: fewer than asked only at the
 * end of the file, and 0 there.
 * @param[out] error Optional; why it failed.
 * @return OK; MALFORMED when the file ends before its frames do;
 * INPUT_FAILED when the stream fails.
 */
TB_API tb_wav_result tb_wav_reader_read(tb_wav_reader *reader, void *samples, size_t length,
                                        size_t *frames, tb_wav_error *error);

/**
 * @brief Move to a frame, such as a loop's start, for the next read.
 * @param reader Required. @param frame At most the frame count.
 * @param[out] error Optional; why it failed.
 * @return OK; UNSEEKABLE for a stream that cannot seek; INPUT_FAILED or
 * MALFORMED as for a read.
 */
TB_API tb_wav_result tb_wav_reader_seek(tb_wav_reader *reader, size_t frame, tb_wav_error *error);

/** @brief Close a reader; its stream is the caller's again. @param reader Optional. */
TB_API void tb_wav_reader_close(tb_wav_reader *reader);

/** @brief A short name of a result, such as "malformed". */
TB_API const char *tb_wav_result_name(tb_wav_result result);

#endif
