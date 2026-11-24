/* Loading a shared library and finding its symbols, on every platform. The
 * library is tests/platform/library_fixture.c, which the build puts beside
 * this executable as TB_LIBRARY_FIXTURE_FILE; each platform's own tests load
 * one of the system's libraries.
 *
 * Every failure must come back to the caller as a result and a message that
 * names what failed: nothing here may end the process, and a search must
 * never find a library anywhere but where it says it looks. */
#include "badger/assert.h"
#include "badger/file.h"
#include "badger/library.h"

#include "fs.h"
#include "library_fixture.h"
#include "tb_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifndef TB_LIBRARY_FIXTURE_FILE
#error "The build names the fixture library's file in TB_LIBRARY_FIXTURE_FILE"
#endif

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

static bool starts_with(const char *text, const char *start)
{
    const size_t length = text_length(start);
    return text_length(text) >= length && memcmp(text, start, length) == 0;
}

/* `first` and `second` joined into `output`, which has room. */
static void join(char *output, size_t capacity, const char *first, const char *second)
{
    const size_t left = text_length(first);
    const size_t right = text_length(second);
    TB_TEST_REQUIRE(left + right + 1 <= capacity);
    memcpy(output, first, left);
    memcpy(output + left, second, right + 1);
}

/* The fixture's absolute path: the executable's directory and its name. */
static void fixture_path(char *output, size_t capacity)
{
    char directory[4096] = {0};
    size_t required = 0;
    TB_TEST_REQUIRE_UINT(
        tb_fs_get_location(TB_FS_EXECUTABLE, directory, sizeof(directory), &required), ==,
        TB_FS_OK);
    join(output, capacity, directory, TB_LIBRARY_FIXTURE_FILE);
}

static tb_library *open_fixture(void)
{
    tb_library *library = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    const tb_library_result result =
        tb_library_open(TB_LIBRARY_FIXTURE_FILE, TB_LIBRARY_SEARCH_APPLICATION, &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_POINTER(library, !=, NULL);
    return library;
}

/* The fixture's function, called through what was found. */
static int add_through(tb_library_function function, int left, int right)
{
    tb_library_fixture_add_function *const add = (tb_library_fixture_add_function *)function;
    return add(left, right);
}

TB_TEST(platform_library, a_library_beside_the_executable_loads_and_its_symbols_are_found)
{
    tb_library *const library = open_fixture();
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_function function = NULL;
    void *answer = NULL;
    void *word = NULL;

    TB_TEST_REQUIRE_UINT(
        tb_library_find_function(library, TB_LIBRARY_FIXTURE_ADD, &function, &error), ==,
        TB_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    TB_TEST_EXPECT_INT(add_through(function, 2, 3), ==, 5);

    TB_TEST_REQUIRE_UINT(tb_library_find_data(library, TB_LIBRARY_FIXTURE_ANSWER, &answer, &error),
                         ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_INT(*(const int *)answer, ==, 42);
    TB_TEST_REQUIRE_UINT(tb_library_find_data(library, TB_LIBRARY_FIXTURE_WORD, &word, NULL), ==,
                         TB_LIBRARY_OK);
    TB_TEST_EXPECT_STRING((const char *)word, ==, "badger");
    tb_library_close(library);
}

TB_TEST(platform_library, a_library_loads_from_its_absolute_path)
{
    char path[4352] = {0};
    tb_library *library = NULL;
    tb_library_function function = NULL;

    fixture_path(path, sizeof(path));
    TB_TEST_REQUIRE_UINT(tb_library_open(path, TB_LIBRARY_SEARCH_PATH, &library, NULL), ==,
                         TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(library, TB_LIBRARY_FIXTURE_ADD, &function, NULL),
                         ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_INT(add_through(function, 40, 2), ==, 42);
    tb_library_close(library);
}

/* The system counts the opens of one library: closing one leaves the other
 * and what was found through it working. */
TB_TEST(platform_library, a_library_opened_twice_stays_loaded_until_both_are_closed)
{
    tb_library *const first = open_fixture();
    tb_library *const second = open_fixture();
    tb_library_function function = NULL;

    TB_TEST_REQUIRE_UINT(tb_library_find_function(second, TB_LIBRARY_FIXTURE_ADD, &function, NULL),
                         ==, TB_LIBRARY_OK);
    tb_library_close(first);
    TB_TEST_EXPECT_INT(add_through(function, 1, 1), ==, 2);
    tb_library_close(second);
}

TB_TEST(platform_library, closing_null_does_nothing)
{
    tb_library_close(NULL);
}

/* The fixture is beside the executable, but the system's libraries are not
 * looked for there. */
TB_TEST(platform_library, the_system_search_does_not_look_beside_the_executable)
{
    tb_library *library = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;

    TB_TEST_EXPECT_UINT(
        tb_library_open(TB_LIBRARY_FIXTURE_FILE, TB_LIBRARY_SEARCH_SYSTEM, &library, &error), ==,
        TB_LIBRARY_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_UNAVAILABLE);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_EXPECT(starts_with(error.message,
                               TB_LIBRARY_FIXTURE_FILE ": not loaded from the system's libraries"));
}

TB_TEST(platform_library, a_missing_library_is_unavailable_and_named)
{
    tb_library *library = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;

    TB_TEST_EXPECT_UINT(
        tb_library_open("tb_no_such_library", TB_LIBRARY_SEARCH_APPLICATION, &library, &error), ==,
        TB_LIBRARY_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_EXPECT(starts_with(error.message,
                               "tb_no_such_library: not loaded from the application's directory"));
    /* The system says why, after the engine's words. */
    TB_TEST_EXPECT_UINT(text_length(error.message), >,
                        text_length("tb_no_such_library: not loaded from the application's "
                                    "directory: "));

    TB_TEST_EXPECT_UINT(
        tb_library_open("tb_no_such_library", TB_LIBRARY_SEARCH_SYSTEM, &library, NULL), ==,
        TB_LIBRARY_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
}

/* A file that is there but is no library fails as one that is missing does,
 * with the system's reason. */
TB_TEST(platform_library, a_file_that_is_not_a_library_is_unavailable)
{
    static const char text[] = "This is not a shared library.\n";
    char path[4096] = {0};
    tb_file file = {0};
    size_t written = 0;
    tb_library *library = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    const tb_path_view temporary = {0};

    TB_TEST_REQUIRE_UINT(tb_fs_temp_file(temporary, path, sizeof(path), &file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_write(&file, text, sizeof(text) - 1, &written), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);

    TB_TEST_EXPECT_UINT(tb_library_open(path, TB_LIBRARY_SEARCH_PATH, &library, &error), ==,
                        TB_LIBRARY_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_EXPECT(starts_with(error.message, path));
    TB_TEST_EXPECT_UINT(text_length(error.message), >, text_length(path) + 20);
    TB_TEST_EXPECT_UINT(tb_fs_remove((tb_path_view){path, text_length(path)}), ==, TB_FS_OK);
}

/* A name that a search cannot use is refused before anything is looked
 * for. */
TB_TEST(platform_library, a_name_with_a_directory_is_not_a_file_name)
{
    static const char *const names[] = {"",     ".",         "..",           "directory/library",
                                        "a\\b", "c:library", "\xFF.library", "..\\library"};
    static const tb_library_search searches[] = {TB_LIBRARY_SEARCH_SYSTEM,
                                                 TB_LIBRARY_SEARCH_APPLICATION};

    for (size_t search = 0; search < TB_TEST_COUNT_OF(searches); search++)
    {
        for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
        {
            tb_library *library = NULL;
            tb_library_error error = TB_LIBRARY_ERROR_INIT;

            TB_TEST_CONTEXT("search %zu, name %zu", search, index);
            TB_TEST_EXPECT_UINT(tb_library_open(names[index], searches[search], &library, &error),
                                ==, TB_LIBRARY_INVALID);
            TB_TEST_EXPECT_POINTER(library, ==, NULL);
            TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_INVALID);
            TB_TEST_EXPECT(starts_with(error.message, names[index]));
        }
    }
}

/* A relative path would be found from the working directory, which is no
 * place the program chose: it is refused, though the fixture is there when
 * the tests run beside it. */
TB_TEST(platform_library, an_exact_path_must_be_absolute)
{
    static const char *const names[] = {TB_LIBRARY_FIXTURE_FILE, "./" TB_LIBRARY_FIXTURE_FILE,
                                        "bin/" TB_LIBRARY_FIXTURE_FILE, "", "/\xC3"};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        tb_library *library = NULL;
        tb_library_error error = TB_LIBRARY_ERROR_INIT;

        TB_TEST_CONTEXT("name %zu", index);
        TB_TEST_EXPECT_UINT(tb_library_open(names[index], TB_LIBRARY_SEARCH_PATH, &library, &error),
                            ==, TB_LIBRARY_INVALID);
        TB_TEST_EXPECT_POINTER(library, ==, NULL);
    }
}

TB_TEST(platform_library, a_missing_symbol_is_named_with_its_library)
{
    tb_library *const library = open_fixture();
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_function function = NULL;
    void *data = NULL;

    TB_TEST_EXPECT_UINT(
        tb_library_find_function(library, "tb_library_fixture_absent", &function, &error), ==,
        TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT(function == NULL);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          TB_LIBRARY_FIXTURE_FILE ": missing tb_library_fixture_absent");

    TB_TEST_EXPECT_UINT(tb_library_find_data(library, "tb_library_fixture_absent", &data, &error),
                        ==, TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT_POINTER(data, ==, NULL);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          TB_LIBRARY_FIXTURE_FILE ": missing tb_library_fixture_absent");

    /* Success clears an error left from before. */
    TB_TEST_REQUIRE_UINT(
        tb_library_find_function(library, TB_LIBRARY_FIXTURE_ADD, &function, &error), ==,
        TB_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    tb_library_close(library);
}

TB_TEST(platform_library, binding_sets_every_function_and_leaves_a_missing_optional_one_null)
{
    tb_library *const library = open_fixture();
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_function add = NULL;
    tb_library_function absent = NULL;
    const tb_library_binding bindings[] = {
        {TB_LIBRARY_FIXTURE_ADD, &add, true},
        {"tb_library_fixture_absent", &absent, false},
    };

    TB_TEST_REQUIRE_UINT(tb_library_bind(library, bindings, TB_TEST_COUNT_OF(bindings), &error), ==,
                         TB_LIBRARY_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    TB_TEST_REQUIRE(add != NULL);
    TB_TEST_EXPECT_INT(add_through(add, 20, 22), ==, 42);
    TB_TEST_EXPECT(absent == NULL);
    TB_TEST_EXPECT_UINT(tb_library_bind(library, NULL, 0, NULL), ==, TB_LIBRARY_OK);
    tb_library_close(library);
}

TB_TEST(platform_library, binding_names_every_missing_required_function)
{
    tb_library *const library = open_fixture();
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_function first = NULL;
    tb_library_function add = NULL;
    tb_library_function optional = NULL;
    tb_library_function second = NULL;
    const tb_library_binding bindings[] = {
        {"tb_library_fixture_first", &first, true},
        {TB_LIBRARY_FIXTURE_ADD, &add, true},
        {"tb_library_fixture_optional", &optional, false},
        {"tb_library_fixture_second", &second, true},
    };

    TB_TEST_EXPECT_UINT(tb_library_bind(library, bindings, TB_TEST_COUNT_OF(bindings), &error), ==,
                        TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          TB_LIBRARY_FIXTURE_FILE
                          ": missing tb_library_fixture_first, tb_library_fixture_second");
    /* What was found is set even so. */
    TB_TEST_EXPECT(add != NULL);
    TB_TEST_EXPECT(first == NULL && optional == NULL && second == NULL);
    tb_library_close(library);
}

/* A message longer than its room is cut at a character, and terminated. */
TB_TEST(platform_library, a_long_message_is_cut_at_a_character)
{
    enum
    {
        BINDINGS = 40
    };
    /* Two bytes a character, so a cut at an odd count of bytes would split
     * one. */
    static const char name[] = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    tb_library *const library = open_fixture();
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_function functions[BINDINGS] = {0};
    tb_library_binding bindings[BINDINGS] = {0};

    for (size_t index = 0; index < BINDINGS; index++)
    {
        bindings[index].name = name;
        bindings[index].function = &functions[index];
        bindings[index].required = true;
    }
    TB_TEST_EXPECT_UINT(tb_library_bind(library, bindings, BINDINGS, &error), ==,
                        TB_LIBRARY_SYMBOL_MISSING);
    TB_TEST_EXPECT_UINT(text_length(error.message), <, TB_LIBRARY_MESSAGE_CAPACITY);
    TB_TEST_EXPECT_UINT(text_length(error.message), >, TB_LIBRARY_MESSAGE_CAPACITY - 4);
    TB_TEST_EXPECT(tb_native_fs_text_valid(error.message, text_length(error.message)));
    tb_library_close(library);
}

static void open_without_a_name(void *context)
{
    tb_library *library = NULL;
    (void)context;
    (void)tb_library_open(NULL, TB_LIBRARY_SEARCH_SYSTEM, &library, NULL);
}

TB_TEST(platform_library, a_null_name_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    TB_TEST_EXPECT_TRAP(open_without_a_name, NULL, "assertion failed");
}
