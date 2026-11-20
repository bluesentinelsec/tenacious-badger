/* A shared library for the tests of badger/library.h to load: one function
 * and two data, each with a value a test can recognize. It is built beside
 * the test executables, and exports every symbol of its own. */
#include "library_fixture.h"

int tb_library_fixture_add(int left, int right)
{
    return left + right;
}

int tb_library_fixture_answer = 42;

/* Not const: CMake's export of every symbol of a Windows library leaves
 * out read-only data. */
char tb_library_fixture_word[] = "badger";
