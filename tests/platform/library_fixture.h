/* What tests/platform/library_fixture.c exports, by name and type. The
 * tests find them by name: they never link the fixture. */
#ifndef TB_LIBRARY_FIXTURE_H
#define TB_LIBRARY_FIXTURE_H

#define TB_LIBRARY_FIXTURE_ADD "tb_library_fixture_add"
#define TB_LIBRARY_FIXTURE_ANSWER "tb_library_fixture_answer"
#define TB_LIBRARY_FIXTURE_WORD "tb_library_fixture_word"

/* The type of the fixture's function. */
typedef int tb_library_fixture_add_function(int left, int right);

int tb_library_fixture_add(int left, int right);
extern int tb_library_fixture_answer;
extern char tb_library_fixture_word[];

#endif
