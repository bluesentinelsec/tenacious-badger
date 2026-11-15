# Strings

Text inside the engine is UTF-8. The string modules each have one public header
under `include/badger/`, which is its API reference.

| Module | Header | Use it for |
| --- | --- | --- |
| [C strings](#c-strings) | `str.h` | Searching, comparing, and building NUL-terminated strings. |

## What they have in common

- **Positions are byte indexes from 0.** A part of a string is given as `begin`
  and `end` and holds the bytes from `begin` up to, but not including, `end`.
  An empty range has `begin == end`, and `0` to the length is the whole string.
- **A search returns the index of what it found**, or a `NOT_FOUND` constant.
  Backward searches are named `rfind` and return the last match in the range.
- **New strings come from an allocator.** A function that builds a string takes
  a `const tb_allocator *`, NULL for the engine's heap, and never returns NULL.
  See [Choosing the allocator of one object](memory.md#choosing-the-allocator-of-one-object).
  Building strings in an arena, and releasing the arena, is the usual way to
  work with temporary strings.
- **Misuse is a contract violation.** A NULL string or a range outside the
  string fails a `TB_ASSERT` in builds with assertions.

### UTF-8 without decoding

The functions here work on bytes and never decode UTF-8. That is enough for
most of what code does with text, because of how UTF-8 is built:

- **Searching is exact.** No character's encoding appears inside another's, so
  a bytewise search for valid UTF-8 finds only real occurrences.
- **Comparing orders by code point.** Comparing bytes as unsigned values puts
  UTF-8 strings in code point order.
- **Joining, copying, and repeating** valid strings gives valid strings.

Three things need care:

- **A position counts bytes, not characters.** `é` is two bytes and `日` is
  three.
- **Cutting inside a character** gives invalid UTF-8. Positions that come from a
  search are always at a character boundary.
- **Reversing and mapping are for ASCII.** They work byte by byte.

Validating UTF-8, and counting or stepping through code points, is a separate
module that is not here yet (#48).

## C strings

`str.h` works on NUL-terminated strings: the form string literals, the C
library, and the operating system use.

```c
const size_t dot = tb_str_rfind_char(file_name, 0, TB_STR_END, '.');

if (dot != TB_STR_NOT_FOUND && tb_str_equal(file_name + dot, ".png"))
{
    char *const stem = tb_str_sub(NULL, file_name, 0, dot);

    load_texture(stem);
    tb_str_free(NULL, stem);
}
```

| Need | Use |
| --- | --- |
| Length | `tb_str_length(string)` |
| Compare | `tb_str_compare(left, right)`, `tb_str_equal(left, right)` |
| Test the ends | `tb_str_starts_with(string, prefix)`, `tb_str_ends_with(string, suffix)` |
| Test a position | `tb_str_matches_at(string, position, pattern)` |
| Find a byte | `tb_str_find_char`, `tb_str_rfind_char` |
| Find any byte of a set | `tb_str_find_any`, `tb_str_rfind_any` |
| Find a substring | `tb_str_find`, `tb_str_rfind` |
| Skip a run of a set | `tb_str_span`, `tb_str_rspan` |
| Copy | `tb_str_dup(allocator, string)`, `tb_str_sub(allocator, string, begin, end)` |
| Join | `tb_str_cat(allocator, left, right)`, `tb_str_join(allocator, strings, count, separator)` |
| Repeat, reverse, map | `tb_str_repeat`, `tb_str_reverse`, `tb_str_map` |
| Release | `tb_str_free(allocator, string)` |

Every search takes the string, a range, and what to look for:
`tb_str_find(string, begin, end, pattern)`.

**`TB_STR_END` is the end of the string.** Pass it as `end` to search or copy
through to the terminator without measuring the string first.

**To walk every occurrence**, search again from one past the last result:

```c
for (size_t slash = tb_str_find_char(path, 0, TB_STR_END, '/'); slash != TB_STR_NOT_FOUND;
     slash = tb_str_find_char(path, slash + 1, TB_STR_END, '/'))
{
    ...
}
```

**A set is a string of the bytes that belong to it.** `tb_str_find_any(path, 0,
TB_STR_END, "/\\")` finds the first separator of either kind, and
`tb_str_span(line, 0, TB_STR_END, " \t")` measures leading white space.

**Releasing.** `tb_str_free` works out the size of the string from its length,
so the length must be what it was when the string was built. A string that was
shortened by writing a terminator into it is released with `tb_allocator_free`
and its original size, or was allocated in an arena and is not released at all.
