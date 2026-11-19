/* The virtual file system: names it refuses; directories on disk, read live;
 * packages, ZIP, and TAR archives from files and from memory; priorities and
 * mount points; a source of one's own; and many threads reading at once. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/package.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/tar.h"
#include "badger/thread.h"
#include "badger/vfs.h"
#include "badger/zip.h"
#include "tb_test.h"

#include <stdbool.h>
#include <string.h>

static const uint8_t game_key[TB_PACKAGE_KEY_SIZE] = {7, 1, 4, 2, 8, 5, 7, 1, 4, 2, 8, 5,
                                                      7, 1, 4, 2, 8, 5, 7, 1, 4, 2, 8, 5};

/* The files every archive in these tests holds. */
typedef struct file_sample
{
    const char *name;
    const char *text;
} file_sample;

static const file_sample files[] = {
    {"readme.txt", "read me\n"},
    {"textures/rock.txt", "a rock, grey and round\n"},
    {"textures/sky/day.txt", "blue\n"},
    {"sounds/wind.txt", "whoosh\n"},
};

static size_t text_length(const file_sample *file)
{
    return tb_str_length(file->text);
}

/* -------------------------------------------------------------------------
 * Archives made in memory, which the caller frees with tb_stream_close.
 * ------------------------------------------------------------------------- */

typedef enum archive_kind
{
    ARCHIVE_PACKAGE,
    ARCHIVE_OBFUSCATED_PACKAGE,
    ARCHIVE_ZIP,
    ARCHIVE_TAR,
    ARCHIVE_KINDS
} archive_kind;

static const char *const archive_names[ARCHIVE_KINDS] = {"package", "obfuscated package", "ZIP",
                                                         "TAR"};

static const unsigned char *make_archive(archive_kind kind, const file_sample *samples,
                                         size_t count, tb_stream *buffer, size_t *length)
{
    tb_stream_open_buffer(NULL, buffer);
    if (kind == ARCHIVE_PACKAGE || kind == ARCHIVE_OBFUSCATED_PACKAGE)
    {
        tb_package_writer *writer = NULL;
        TB_TEST_REQUIRE_UINT(tb_package_writer_create(
                                 buffer, kind == ARCHIVE_PACKAGE ? NULL : game_key, NULL, &writer),
                             ==, TB_PACKAGE_OK);
        for (size_t index = 0; index < count; index++)
        {
            TB_TEST_REQUIRE_UINT(
                tb_package_add(writer, samples[index].name, tb_str_length(samples[index].name),
                               samples[index].text, text_length(&samples[index]), 6),
                ==, TB_PACKAGE_OK);
        }
        TB_TEST_REQUIRE_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OK);
        tb_package_writer_destroy(writer);
    }
    else if (kind == ARCHIVE_ZIP)
    {
        tb_zip_writer *const writer = tb_zip_writer_create(buffer, NULL);
        for (size_t index = 0; index < count; index++)
        {
            const tb_zip_entry entry = {.name = samples[index].name,
                                        .name_length = tb_str_length(samples[index].name),
                                        .type = TB_ZIP_FILE,
                                        .method = TB_ZIP_DEFLATED,
                                        .size = text_length(&samples[index])};
            TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 6), ==, TB_ZIP_OK);
            TB_TEST_REQUIRE_UINT(
                tb_zip_write_data(writer, samples[index].text, text_length(&samples[index])), ==,
                TB_ZIP_OK);
        }
        TB_TEST_REQUIRE_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
        tb_zip_writer_destroy(writer);
    }
    else
    {
        tb_tar_writer *const writer = tb_tar_writer_create(buffer, NULL);
        for (size_t index = 0; index < count; index++)
        {
            const tb_tar_entry entry = {.name = samples[index].name,
                                        .name_length = tb_str_length(samples[index].name),
                                        .link = "",
                                        .type = TB_TAR_FILE,
                                        .size = text_length(&samples[index])};
            TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
            TB_TEST_REQUIRE_UINT(
                tb_tar_write_data(writer, samples[index].text, text_length(&samples[index])), ==,
                TB_TAR_OK);
        }
        TB_TEST_REQUIRE_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
        tb_tar_writer_destroy(writer);
    }
    return tb_stream_buffer_bytes(buffer, length);
}

/* Read a file of the VFS whole into `bytes`; return its length. */
static size_t read_file(tb_vfs *vfs, const char *name, char *bytes, size_t capacity)
{
    tb_stream stream = {0};
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_vfs_open(vfs, name, tb_str_length(name), NULL, &stream), ==, TB_VFS_OK);
    for (;;)
    {
        size_t count = 0;
        TB_TEST_REQUIRE_UINT(tb_stream_read(&stream, bytes + length, capacity - length, &count), ==,
                             TB_STREAM_OK);
        if (count == 0)
        {
            break;
        }
        length += count;
    }
    (void)tb_stream_close(&stream);
    return length;
}

static void expect_text(tb_vfs *vfs, const char *name, const char *text)
{
    char bytes[256] = {0};
    const size_t length = read_file(vfs, name, bytes, sizeof(bytes));
    TB_TEST_CONTEXT("%s", name);
    TB_TEST_EXPECT_UINT(length, ==, tb_str_length(text));
    TB_TEST_EXPECT_BYTES(bytes, text, length);
}

/* What a listing gave, as "name/" for a directory and "name=size" for a
 * file, joined by spaces. */
typedef struct listing
{
    char text[1024];
    size_t length;
} listing;

static void collect(void *context, const char *name, size_t length, const tb_vfs_info *info)
{
    listing *const list = context;
    list->length +=
        info->kind == TB_VFS_DIRECTORY
            ? tb_fmt_buffer(list->text + list->length, sizeof(list->text) - list->length, "%s%.*s/",
                            list->length == 0 ? "" : " ", (int)length, name)
            : tb_fmt_buffer(list->text + list->length, sizeof(list->text) - list->length,
                            "%s%.*s=%llu", list->length == 0 ? "" : " ", (int)length, name,
                            (unsigned long long)info->size);
}

static void expect_listing(tb_vfs *vfs, const char *directory, const char *expected)
{
    listing list = {0};
    TB_TEST_CONTEXT("listing %s", directory);
    TB_TEST_REQUIRE_UINT(tb_vfs_list(vfs, directory, tb_str_length(directory), collect, &list), ==,
                         TB_VFS_OK);
    TB_TEST_EXPECT_STRING(list.text, ==, expected);
}

static void expect_samples(tb_vfs *vfs, const char *prefix)
{
    char name[256] = {0};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(files); index++)
    {
        (void)tb_fmt_buffer(name, sizeof(name), "%s%s", prefix, files[index].name);
        expect_text(vfs, name, files[index].text);
    }
}

/* -------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

TB_TEST(vfs, names_that_could_leave_a_mount_or_mean_something_else_are_refused)
{
    static const char *const refused[] = {
        "/a",   "a/",   "a//b", ".",   "..",  "./a",     "a/./b",
        "a/..", "../a", "a\\b", "C:a", "a:b", "bad\xff", "\xc3",
    };
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_stream stream = {0};
    tb_vfs_info info = {0};
    listing list = {0};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(refused); index++)
    {
        const size_t length = tb_str_length(refused[index]);
        TB_TEST_CONTEXT("%s", refused[index]);
        TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, refused[index], length, NULL, &stream), ==,
                            TB_VFS_INVALID);
        TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, refused[index], length, &info), ==, TB_VFS_INVALID);
        TB_TEST_EXPECT_UINT(tb_vfs_list(vfs, refused[index], length, collect, &list), ==,
                            TB_VFS_INVALID);
    }
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "a\0b", 3, NULL, &stream), ==, TB_VFS_INVALID);
    /* The root opens as nothing, but is a directory, mounts or none. */
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "", 0, NULL, &stream), ==, TB_VFS_INVALID);
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "", 0, &info), ==, TB_VFS_OK);
    TB_TEST_EXPECT_UINT(info.kind, ==, TB_VFS_DIRECTORY);
    TB_TEST_EXPECT_UINT(tb_vfs_list(vfs, "", 0, collect, &list), ==, TB_VFS_OK);
    TB_TEST_EXPECT_UINT(list.length, ==, 0);
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "a", 1, &info), ==, TB_VFS_NOT_FOUND);
    tb_vfs_destroy(vfs);
}

/* -------------------------------------------------------------------------
 * Archives.
 * ------------------------------------------------------------------------- */

TB_TEST(vfs, every_kind_of_archive_mounts_from_memory)
{
    for (int kind = 0; kind < ARCHIVE_KINDS; kind++)
    {
        tb_stream buffer = {0};
        size_t length = 0;
        const unsigned char *const bytes =
            make_archive((archive_kind)kind, files, TB_TEST_COUNT_OF(files), &buffer, &length);
        tb_vfs *const vfs = tb_vfs_create(NULL);
        tb_vfs_info info = {0};
        tb_stream stream = {0};
        TB_TEST_CONTEXT("%s", archive_names[kind]);
        TB_TEST_REQUIRE_UINT(
            tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length,
                                        kind == ARCHIVE_OBFUSCATED_PACKAGE ? game_key : NULL, 0),
            ==, TB_VFS_OK);
        expect_samples(vfs, "");
        /* Directories the names imply, though no archive here lists them. */
        expect_listing(vfs, "", "readme.txt=8 sounds/ textures/");
        expect_listing(vfs, "textures", "rock.txt=23 sky/");
        expect_listing(vfs, "textures/sky", "day.txt=5");
        TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "textures/sky", 12, &info), ==, TB_VFS_OK);
        TB_TEST_EXPECT_UINT(info.kind, ==, TB_VFS_DIRECTORY);
        TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "sounds/wind.txt", 15, &info), ==, TB_VFS_OK);
        TB_TEST_EXPECT_UINT(info.kind, ==, TB_VFS_FILE);
        TB_TEST_EXPECT_UINT(info.size, ==, 7);
        TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "textures", 8, NULL, &stream), ==, TB_VFS_NOT_A_FILE);
        TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "textures/stone.txt", 18, NULL, &stream), ==,
                            TB_VFS_NOT_FOUND);
        TB_TEST_EXPECT_UINT(tb_vfs_list(vfs, "readme.txt", 10, collect, &(listing){0}), ==,
                            TB_VFS_NOT_A_DIRECTORY);
        TB_TEST_EXPECT_UINT(tb_vfs_list(vfs, "music", 5, collect, &(listing){0}), ==,
                            TB_VFS_NOT_FOUND);
        TB_TEST_EXPECT_POINTER(stream.functions, ==, NULL);
        tb_vfs_destroy(vfs);
        (void)tb_stream_close(&buffer);
    }
}

TB_TEST(vfs, what_cannot_be_mounted_is_refused_and_says_why)
{
    static const unsigned char gzip[16] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    static const unsigned char text[600] = "This is not an archive of any kind at all.";
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_stream buffer = {0};
    size_t length = 0;
    const unsigned char *bytes = NULL;
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, text, sizeof(text), NULL, 0), ==,
                        TB_VFS_UNKNOWN_FORMAT);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, text, 4, NULL, 0), ==,
                        TB_VFS_UNKNOWN_FORMAT);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, gzip, sizeof(gzip), NULL, 0), ==,
                        TB_VFS_UNSUPPORTED);
    /* An obfuscated package without its key, or with another. */
    bytes = make_archive(ARCHIVE_OBFUSCATED_PACKAGE, files, 1, &buffer, &length);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length, NULL, 0), ==,
                        TB_VFS_KEY_REQUIRED);
    {
        uint8_t wrong[TB_PACKAGE_KEY_SIZE] = {0};
        TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length, wrong, 0), ==,
                            TB_VFS_WRONG_KEY);
    }
    /* A mount point the VFS refuses. */
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "../up", 5, bytes, length, game_key, 0),
                        ==, TB_VFS_INVALID);
    (void)tb_stream_close(&buffer);
    /* A damaged archive. */
    bytes = make_archive(ARCHIVE_ZIP, files, 2, &buffer, &length);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length - 1, NULL, 0), ==,
                        TB_VFS_MALFORMED);
    (void)tb_stream_close(&buffer);
    /* Nothing was mounted. */
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "readme.txt", 10, &(tb_vfs_info){0}), ==,
                        TB_VFS_NOT_FOUND);
    tb_vfs_destroy(vfs);
}

/* An archive with a name the VFS refuses is not mounted. The ZIP writer
 * refuses such names too, so the name is put in after. */
TB_TEST(vfs, an_archive_with_an_unsafe_name_is_not_mounted)
{
    static const char *const unsafe[] = {"../evil.txt", "a\\..\\..\\x", "C:temp.txt"};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(unsafe); index++)
    {
        const size_t name_length = tb_str_length(unsafe[index]);
        char placeholder[32] = {0};
        file_sample sample = {placeholder, "evil\n"};
        tb_stream buffer = {0};
        size_t length = 0;
        static unsigned char bytes[1024];
        tb_vfs *const vfs = tb_vfs_create(NULL);
        size_t replaced = 0;
        TB_TEST_CONTEXT("%s", unsafe[index]);
        memset(placeholder, 'q', name_length);
        {
            const unsigned char *const made =
                make_archive(ARCHIVE_ZIP, &sample, 1, &buffer, &length);
            TB_TEST_REQUIRE_UINT(length, <=, sizeof(bytes));
            memcpy(bytes, made, length);
        }
        for (size_t at = 0; at + name_length <= length; at++)
        {
            if (memcmp(bytes + at, placeholder, name_length) == 0)
            {
                memcpy(bytes + at, unsafe[index], name_length);
                replaced++;
            }
        }
        TB_TEST_REQUIRE_UINT(replaced, ==, 2);
        TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length, NULL, 0), ==,
                            TB_VFS_UNSAFE_NAME);
        tb_vfs_destroy(vfs);
        (void)tb_stream_close(&buffer);
    }
}

/* -------------------------------------------------------------------------
 * Directories and files on disk.
 * ------------------------------------------------------------------------- */

typedef struct scratch
{
    char root[32768];
    tb_path_style style;
} scratch;

/* The native path of a name under the scratch root. */
static char *scratch_path(const scratch *place, const char *name)
{
    tb_path_result result = TB_PATH_OK;
    char *const path = tb_path_rooted_join_new(
        NULL, place->style, (tb_path_view){place->root, tb_str_length(place->root)},
        (tb_path_view){name, tb_str_length(name)}, &result);
    TB_TEST_REQUIRE_POINTER(path, !=, NULL);
    return path;
}

static void scratch_write(const scratch *place, const char *name, const char *text)
{
    char *const path = scratch_path(place, name);
    tb_path_result result = TB_PATH_OK;
    char *const parent =
        tb_path_parent_new(NULL, place->style, (tb_path_view){path, tb_str_length(path)}, &result);
    TB_TEST_REQUIRE_POINTER(parent, !=, NULL);
    TB_TEST_REQUIRE_UINT(tb_fs_make_directories((tb_path_view){parent, tb_str_length(parent)}), ==,
                         TB_FS_OK);
    TB_TEST_REQUIRE_UINT(
        tb_fs_write_file((tb_path_view){path, tb_str_length(path)}, text, tb_str_length(text)), ==,
        TB_FS_OK);
    tb_str_free(NULL, parent);
    tb_str_free(NULL, path);
}

static void scratch_open(scratch *place)
{
    place->style = tb_fs_native_style();
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, place->root, sizeof(place->root)),
                         ==, TB_FS_OK);
}

/* Remove what the tests put in the scratch directory, deepest first. */
static void scratch_remove(const scratch *place, const char *const *names, size_t count)
{
    for (size_t index = count; index > 0; index--)
    {
        char *const path = scratch_path(place, names[index - 1]);
        (void)tb_fs_remove((tb_path_view){path, tb_str_length(path)});
        tb_str_free(NULL, path);
    }
    (void)tb_fs_remove((tb_path_view){place->root, tb_str_length(place->root)});
}

TB_TEST(vfs, a_directory_mount_reads_the_disk_live)
{
    static const char *const made[] = {
        "readme.txt",           "textures", "textures/rock.txt", "textures/sky",
        "textures/sky/day.txt", "sounds",   "sounds/wind.txt",   "late.txt"};
    static scratch place;
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_vfs_info info = {0};
    tb_stream stream = {0};
    scratch_open(&place);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(files); index++)
    {
        scratch_write(&place, files[index].name, files[index].text);
    }
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_directory(vfs, "", 0, place.root, 0), ==, TB_VFS_OK);
    expect_samples(vfs, "");
    expect_listing(vfs, "", "readme.txt=8 sounds/ textures/");
    expect_listing(vfs, "textures", "rock.txt=23 sky/");
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "textures", 8, NULL, &stream), ==, TB_VFS_NOT_A_FILE);
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "missing.txt", 11, NULL, &stream), ==, TB_VFS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "readme.txt/x", 12, NULL, &stream), ==, TB_VFS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_vfs_list(vfs, "readme.txt", 10, collect, &(listing){0}), ==,
                        TB_VFS_NOT_A_DIRECTORY);
    /* A file added after the mount is seen. */
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "late.txt", 8, &info), ==, TB_VFS_NOT_FOUND);
    scratch_write(&place, "late.txt", "late\n");
    expect_text(vfs, "late.txt", "late\n");
    tb_vfs_destroy(vfs);
    scratch_remove(&place, made, TB_TEST_COUNT_OF(made));
}

TB_TEST(vfs, a_directory_that_cannot_be_mounted_says_why)
{
    static scratch place;
    static const char *const made[] = {"file.txt"};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    char *path = NULL;
    scratch_open(&place);
    scratch_write(&place, "file.txt", "x");
    path = scratch_path(&place, "file.txt");
    TB_TEST_EXPECT_UINT(tb_vfs_mount_directory(vfs, "", 0, path, 0), ==, TB_VFS_NOT_A_DIRECTORY);
    tb_str_free(NULL, path);
    path = scratch_path(&place, "missing");
    TB_TEST_EXPECT_UINT(tb_vfs_mount_directory(vfs, "", 0, path, 0), ==, TB_VFS_NOT_FOUND);
    tb_str_free(NULL, path);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_directory(vfs, "", 0, "relative/path", 0), ==, TB_VFS_INVALID);
    TB_TEST_EXPECT_UINT(tb_vfs_mount_directory(vfs, "a//b", 4, place.root, 0), ==, TB_VFS_INVALID);
    tb_vfs_destroy(vfs);
    scratch_remove(&place, made, TB_TEST_COUNT_OF(made));
}

TB_TEST(vfs, an_archive_file_mounts_and_stays_open)
{
    static scratch place;
    static const char *const made[] = {"game.pkg"};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_stream buffer = {0};
    size_t length = 0;
    const unsigned char *bytes =
        make_archive(ARCHIVE_OBFUSCATED_PACKAGE, files, TB_TEST_COUNT_OF(files), &buffer, &length);
    char *path = NULL;
    scratch_open(&place);
    path = scratch_path(&place, "game.pkg");
    TB_TEST_REQUIRE_UINT(tb_fs_write_file((tb_path_view){path, tb_str_length(path)}, bytes, length),
                         ==, TB_FS_OK);
    (void)tb_stream_close(&buffer);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_file(vfs, "", 0, path, game_key, 0), ==, TB_VFS_OK);
    expect_samples(vfs, "");
    tb_str_free(NULL, path);
    path = scratch_path(&place, "missing.pkg");
    TB_TEST_EXPECT_UINT(tb_vfs_mount_archive_file(vfs, "", 0, path, game_key, 0), ==,
                        TB_VFS_NOT_FOUND);
    tb_str_free(NULL, path);
    tb_vfs_destroy(vfs);
    scratch_remove(&place, made, TB_TEST_COUNT_OF(made));
}

/* -------------------------------------------------------------------------
 * Priorities and mount points.
 * ------------------------------------------------------------------------- */

TB_TEST(vfs, a_higher_priority_or_a_later_mount_wins)
{
    static const file_sample base[] = {
        {"a.txt", "base a\n"}, {"b.txt", "base b\n"}, {"dir/c.txt", "base c\n"}};
    static const file_sample mod[] = {{"a.txt", "mod a\n"}, {"dir/d.txt", "mod d\n"}};
    static const file_sample patch[] = {{"b.txt", "patch b\n"}};
    tb_stream base_buffer = {0};
    tb_stream mod_buffer = {0};
    tb_stream patch_buffer = {0};
    size_t base_length = 0;
    size_t mod_length = 0;
    size_t patch_length = 0;
    const unsigned char *const base_bytes =
        make_archive(ARCHIVE_PACKAGE, base, TB_TEST_COUNT_OF(base), &base_buffer, &base_length);
    const unsigned char *const mod_bytes =
        make_archive(ARCHIVE_ZIP, mod, TB_TEST_COUNT_OF(mod), &mod_buffer, &mod_length);
    const unsigned char *const patch_bytes =
        make_archive(ARCHIVE_TAR, patch, TB_TEST_COUNT_OF(patch), &patch_buffer, &patch_length);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    /* The mod is mounted first, at a higher priority; the patch last, at the base's. */
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, mod_bytes, mod_length, NULL, 10),
                         ==, TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, base_bytes, base_length, NULL, 0),
                         ==, TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(
        tb_vfs_mount_archive_memory(vfs, "", 0, patch_bytes, patch_length, NULL, 0), ==, TB_VFS_OK);
    expect_text(vfs, "a.txt", "mod a\n");
    expect_text(vfs, "b.txt", "patch b\n");
    expect_text(vfs, "dir/c.txt", "base c\n");
    expect_text(vfs, "dir/d.txt", "mod d\n");
    /* Each name once, as the mount that wins tells it. */
    expect_listing(vfs, "", "a.txt=6 b.txt=8 dir/");
    expect_listing(vfs, "dir", "c.txt=7 d.txt=6");
    tb_vfs_destroy(vfs);
    (void)tb_stream_close(&patch_buffer);
    (void)tb_stream_close(&mod_buffer);
    (void)tb_stream_close(&base_buffer);
}

TB_TEST(vfs, mount_points_place_archives_in_the_namespace)
{
    tb_stream buffer = {0};
    size_t length = 0;
    const unsigned char *const bytes =
        make_archive(ARCHIVE_ZIP, files, TB_TEST_COUNT_OF(files), &buffer, &length);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_vfs_info info = {0};
    tb_stream stream = {0};
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(vfs, "mods/one", 8, bytes, length, NULL, 0),
                         ==, TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(vfs, "mods/two", 8, bytes, length, NULL, 0),
                         ==, TB_VFS_OK);
    expect_samples(vfs, "mods/one/");
    expect_samples(vfs, "mods/two/");
    expect_listing(vfs, "", "mods/");
    expect_listing(vfs, "mods", "one/ two/");
    expect_listing(vfs, "mods/one", "readme.txt=8 sounds/ textures/");
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "mods", 4, &info), ==, TB_VFS_OK);
    TB_TEST_EXPECT_UINT(info.kind, ==, TB_VFS_DIRECTORY);
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "mods/one", 8, &info), ==, TB_VFS_OK);
    TB_TEST_EXPECT_UINT(info.kind, ==, TB_VFS_DIRECTORY);
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "mods", 4, NULL, &stream), ==, TB_VFS_NOT_A_FILE);
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "mods/one", 8, NULL, &stream), ==, TB_VFS_NOT_A_FILE);
    TB_TEST_EXPECT_UINT(tb_vfs_open(vfs, "readme.txt", 10, NULL, &stream), ==, TB_VFS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "mod", 3, &info), ==, TB_VFS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_vfs_query(vfs, "mods/three", 10, &info), ==, TB_VFS_NOT_FOUND);
    tb_vfs_destroy(vfs);
    (void)tb_stream_close(&buffer);
}

/* -------------------------------------------------------------------------
 * A source of one's own.
 * ------------------------------------------------------------------------- */

typedef struct greeting_source
{
    size_t releases;
} greeting_source;

static tb_vfs_result greeting_open(void *state, const char *name, size_t length,
                                   const tb_allocator *allocator, tb_stream *stream)
{
    (void)state;
    if (length != 5 || memcmp(name, "hello", 5) != 0)
    {
        return TB_VFS_NOT_FOUND;
    }
    tb_stream_open_memory("hi there", 8, allocator, stream);
    return TB_VFS_OK;
}

static tb_vfs_result greeting_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    (void)state;
    if (length == 0)
    {
        *info = (tb_vfs_info){.kind = TB_VFS_DIRECTORY};
        return TB_VFS_OK;
    }
    if (length != 5 || memcmp(name, "hello", 5) != 0)
    {
        return TB_VFS_NOT_FOUND;
    }
    *info = (tb_vfs_info){.kind = TB_VFS_FILE, .size = 8};
    return TB_VFS_OK;
}

static tb_vfs_result greeting_list(void *state, const char *directory, size_t length,
                                   tb_vfs_list_function *function, void *context)
{
    const tb_vfs_info info = {.kind = TB_VFS_FILE, .size = 8};
    (void)state;
    (void)directory;
    if (length != 0)
    {
        return TB_VFS_NOT_FOUND;
    }
    function(context, "hello", 5, &info);
    return TB_VFS_OK;
}

static void greeting_release(void *state)
{
    greeting_source *const source = state;
    source->releases++;
}

static const tb_vfs_source_functions greeting_functions = {
    .open_file = greeting_open,
    .query = greeting_query,
    .list = greeting_list,
    .release = greeting_release,
};

TB_TEST(vfs, a_source_of_ones_own_mounts_like_any_other)
{
    greeting_source state = {0};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    const tb_vfs_source source = {&greeting_functions, &state};
    TB_TEST_EXPECT_UINT(tb_vfs_mount(vfs, "/bad", 4, source, 0), ==, TB_VFS_INVALID);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "greetings", 9, source, 0), ==, TB_VFS_OK);
    expect_text(vfs, "greetings/hello", "hi there");
    expect_listing(vfs, "greetings", "hello=8");
    expect_listing(vfs, "", "greetings/");
    tb_vfs_destroy(vfs);
    TB_TEST_EXPECT_UINT(state.releases, ==, 1);
}

/* -------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

typedef struct reader_job
{
    tb_vfs *vfs;
    size_t failures;
} reader_job;

static void read_everything(void *context)
{
    reader_job *const job = context;
    static const char *const prefixes[] = {"plain/", "hidden/", "zip/", "tar/"};
    char name[128] = {0};
    for (size_t round = 0; round < 25; round++)
    {
        for (size_t prefix = 0; prefix < TB_TEST_COUNT_OF(prefixes); prefix++)
        {
            for (size_t index = 0; index < TB_TEST_COUNT_OF(files); index++)
            {
                char bytes[64] = {0};
                tb_stream stream = {0};
                size_t length = 0;
                const size_t name_length =
                    tb_fmt_buffer(name, sizeof(name), "%s%s", prefixes[prefix], files[index].name);
                if (tb_vfs_open(job->vfs, name, name_length, NULL, &stream) != TB_VFS_OK ||
                    tb_stream_read_exact(&stream, bytes, text_length(&files[index])) !=
                        TB_STREAM_OK ||
                    memcmp(bytes, files[index].text, text_length(&files[index])) != 0)
                {
                    job->failures++;
                }
                if (stream.functions != NULL)
                {
                    (void)tb_stream_close(&stream);
                }
                (void)length;
            }
        }
    }
}

TB_TEST(vfs, many_threads_read_every_kind_of_mount_at_once)
{
    enum
    {
        THREADS = 8
    };
    static const char *const points[ARCHIVE_KINDS] = {"plain", "hidden", "zip", "tar"};
    tb_stream buffers[ARCHIVE_KINDS] = {0};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_thread threads[THREADS] = {0};
    reader_job jobs[THREADS] = {0};
    for (int kind = 0; kind < ARCHIVE_KINDS; kind++)
    {
        size_t length = 0;
        const unsigned char *const bytes = make_archive(
            (archive_kind)kind, files, TB_TEST_COUNT_OF(files), &buffers[kind], &length);
        TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(
                                 vfs, points[kind], tb_str_length(points[kind]), bytes, length,
                                 kind == ARCHIVE_OBFUSCATED_PACKAGE ? game_key : NULL, 0),
                             ==, TB_VFS_OK);
    }
    for (size_t index = 0; index < THREADS; index++)
    {
        jobs[index] = (reader_job){.vfs = vfs};
        tb_thread_create(&threads[index], read_everything, &jobs[index]);
    }
    for (size_t index = 0; index < THREADS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(jobs[index].failures, ==, 0);
    }
    tb_vfs_destroy(vfs);
    for (int kind = 0; kind < ARCHIVE_KINDS; kind++)
    {
        (void)tb_stream_close(&buffers[kind]);
    }
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void vfs_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static const tb_vfs_source_functions incomplete = {.open_file = greeting_open};
    static unsigned char memory[64 * 1024];
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_vfs *vfs = tb_vfs_create(&allocator);
    tb_stream stream = {0};
    tb_stream_open_memory("x", 1, &allocator, &stream);
    switch (choice)
    {
    case 0:
        (void)tb_vfs_open(NULL, "a", 1, NULL, &stream);
        break;
    case 1:
        /* The stream is already open. */
        (void)tb_vfs_open(vfs, "a", 1, NULL, &stream);
        break;
    case 2:
        (void)tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&incomplete, NULL}, 0);
        break;
    case 3:
        (void)tb_vfs_list(vfs, "", 0, NULL, NULL);
        break;
    default:
        (void)tb_vfs_mount_directory(vfs, "", 0, NULL, 0);
        break;
    }
    (void)tb_stream_close(&stream);
    tb_vfs_destroy(vfs);
    tb_arena_destroy(arena);
}

TB_TEST(vfs, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 5; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(vfs_contract, &choice, "assertion failed");
    }
}
