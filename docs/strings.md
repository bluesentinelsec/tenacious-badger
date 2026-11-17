# Strings

Text inside the engine is UTF-8. The string modules each have one public header
under `include/badger/`, which is its API reference.

| Module | Header | Use it for |
| --- | --- | --- |
| [C strings](#c-strings) | `str.h` | Searching, comparing, and building NUL-terminated strings. |
| [Text](#text) | `text.h` | A length and a pointer: substrings without copying, any bytes, no terminator. |
| [Formatting](#formatting) | `fmt.h` | printf-style output to a buffer, a new string, a stream, or any function. |
| [Atoms](#atoms) | `atom.h`, `hash.h` | Names stored once, so that equal names are the same pointer. |

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

## Text

`text.h` is the string type for code that takes strings apart. A `tb_text` is a
length and a pointer to bytes that do not change. It is a small value, passed
and returned like an integer; it needs no terminator and may hold any byte; and
a part of a text is another text that points into the same bytes, so taking a
substring copies nothing and allocates nothing.

```c
const tb_text path = tb_text_from_cstr("assets/textures/stone.png");
const size_t slash = tb_text_rfind_char(path, 0, TB_TEXT_END, '/');
const tb_text name = tb_text_sub(path, slash + 1, TB_TEXT_END); /* "stone.png", not copied */

if (tb_text_ends_with(name, TB_TEXT(".png")))
{
    ...
}
```

| Need | Use |
| --- | --- |
| The text of a literal | `TB_TEXT("...")`, or `TB_TEXT_INIT("...")` to initialize a constant |
| The text of a C string or of bytes | `tb_text_from_cstr(string)`, `tb_text_make(bytes, length)` |
| A part, without copying | `tb_text_sub(text, begin, end)`, `tb_text_trim(text, set)` |
| Compare | `tb_text_compare`, `tb_text_equal`, `tb_text_starts_with`, `tb_text_ends_with`, `tb_text_matches_at` |
| Search | `tb_text_find_char`, `tb_text_find_any`, `tb_text_find`, `tb_text_span`, each with an `r` form |
| A copy that owns its bytes | `tb_text_copy(allocator, text)`, `tb_text_to_cstr(allocator, text)` |
| Build | `tb_text_cat`, `tb_text_join`, `tb_text_repeat`, `tb_text_reverse`, `tb_text_map` |
| Release what was built | `tb_text_free(allocator, text)` |
| Format | `%T`, after `tb_text_register_format()` |

The searches take the same arguments, in the same order, as their C string
counterparts, with texts where those take strings.

**Borrowed or owned.** A text is valid for as long as the bytes it points to.
The functions that make a text without an allocator (`tb_text_from_cstr`,
`tb_text_make`, `tb_text_sub`, `tb_text_trim`) borrow bytes that someone else
owns. The functions that take an allocator produce new bytes, which the caller
owns. The usual pattern is to take texts apart freely while their source is
alive, and to copy a text into an arena when it must outlive its source:

```c
static tb_text asset_name(const tb_allocator *allocator, tb_text path)
{
    const size_t slash = tb_text_rfind_char(path, 0, TB_TEXT_END, '/');
    const size_t begin = slash == TB_TEXT_NOT_FOUND ? 0 : slash + 1;

    return tb_text_copy(allocator, tb_text_sub(path, begin, TB_TEXT_END));
}
```

**The empty text is the zero value.** `tb_text name = {0};` is a valid, empty
text. It has no bytes, and an empty result never allocates.

**A text is not a C string.** It has no terminator, so its `bytes` cannot be
handed to a function that expects one. `tb_text_to_cstr` makes a terminated
copy, and `%T` formats a text directly.

**Sets.** A set of bytes is a text: `TB_TEXT_WHITESPACE`, `TB_TEXT_DIGITS`,
`TB_TEXT_UPPERCASE`, and `TB_TEXT_LOWERCASE` are predefined, and
`tb_text_trim(line, TB_TEXT_WHITESPACE)` is the common use.

**Formatting.** `tb_text_register_format()`, called once at startup, adds the
`%T` conversion. It takes a `tb_text` by value; the width pads and the precision
cuts, as for `%s`:

```c
tb_fmt_print("loading %T (%zu bytes)\n", name, name.length);
```

## Formatting

`fmt.h` is printf-style formatting that writes anywhere and can be taught new
conversions. Logging, the console, error messages, and string building all go
through it. It does not use the C library's printf, so a format produces the
same bytes on every platform and never depends on the process locale.

```c
char label[32] = {0};
char *message = NULL;

(void)tb_fmt_buffer(label, sizeof(label), "%s #%03d", name, index);
message = tb_fmt_string(NULL, "loaded %zu of %zu assets", loaded, total);
tb_fmt_print("%s\n", message);
tb_str_free(NULL, message);
```

| Destination | Function |
| --- | --- |
| A buffer you supply | `tb_fmt_buffer(buffer, size, format, ...)` |
| A new string | `tb_fmt_string(allocator, format, ...)` |
| Standard output, standard error | `tb_fmt_print(format, ...)`, `tb_fmt_print_error(format, ...)` |
| Any function that accepts bytes | `tb_fmt_write(sink, context, format, ...)` |

Each has a form that takes a `va_list`, with a `v` after `tb_fmt_`, for writing
your own formatting functions.

**Printed text is delivered by `tb_fmt_flush`.** Standard output may hold text
back until there is enough of it. Call `tb_fmt_flush()` before anything that
must find it delivered, such as waiting for input or ending the process
abnormally; a failed assertion does so itself. It returns `false` once a
stream has refused output. The program's own `fflush(stdout)` is not a
substitute: on Windows the engine's shared library has a C library of its
own, with its own streams.

**A buffer is never overrun and always terminated.** `tb_fmt_buffer` returns
the length of the whole output, so `length < size` means it fit, and the same
call with a size of 0 measures without writing.

**A new string is exactly as long as its text**, from whichever allocator you
choose. Release it with `tb_str_free`, or format into an arena.

### Format strings

A conversion is `%`, then optional flags, width, precision, and length, then a
conversion character, as in C.

| Conversion | Argument | Output |
| --- | --- | --- |
| `%d`, `%i` | signed integer | decimal |
| `%u` | unsigned integer | decimal |
| `%x`, `%X` | unsigned integer | hexadecimal; `#` adds `0x` or `0X` |
| `%o` | unsigned integer | octal; `#` adds a leading `0` |
| `%c` | `int` | the byte with that value |
| `%s` | `const char *` | the string; `(null)` for a null pointer |
| `%p` | `void *` | `0x` and the address in hexadecimal, the same on every platform |
| `%%` | none | a percent sign |

| Part | Values |
| --- | --- |
| Flags | `-` left-justify, `+` always show a sign, space, `0` pad with zeros, `#` alternate form |
| Width | a number, or `*` to take an `int` argument |
| Precision | `.` and a number, or `.*`: least digits of an integer, most bytes of a string |
| Length | `hh`, `h`, `l`, `ll`, `j`, `z`, `t`, for the integer conversions |

The integer, character, and string conversions behave exactly as the C standard
specifies for printf; the tests compare them with the C library on tens of
thousands of random formats.

**Floating point is not converted yet.** `%f`, `%e`, `%g`, and `%a` are
reserved for a conversion that is correct and the same on every platform, which
is a project of its own (#49). Until then they are invalid.

**A format string is part of the program.** An unknown conversion, or a format
that ends inside one, is a contract violation: it fails an assertion, and in
builds without assertions is written out as it stands. Never pass text from
outside the program as a format; pass it as an argument to `"%s"`.

**No compile-time checking.** Compilers can check printf formats only against
the conversions they know, which would reject the ones a program registers.
Format calls are therefore not checked by the compiler; the assertion catches
an invalid format the first time it runs.

### Conversions of your own

`tb_fmt_register` adds a conversion character for a type of your own:

```c
static void format_vector(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const vector *const value = va_arg(*arguments, const vector *);
    char text[64] = {0};
    const size_t length = tb_fmt_buffer(text, sizeof(text), "(%d, %d)", value->x, value->y);

    tb_fmt_put_padded(output, spec, text, length);
}

(void)tb_fmt_register('V', format_vector);
tb_fmt_print("moved to %V\n", &position);
```

The conversion takes its own arguments with `va_arg`, reads the flags, width,
and precision from the spec, and writes with `tb_fmt_put` or
`tb_fmt_put_padded`. Any ASCII letter that C does not already use in a
conversion can be registered. Register conversions during single-threaded
startup; formatting itself is thread-safe.

## Atoms

`atom.h` stores each string once. Interning a string returns its atom: a
pointer to the one copy of those bytes that the table keeps. Two atoms of the
same table are equal exactly when their pointers are equal, so comparing names
costs one comparison however long they are. Atoms are for names that are
compared far more often than they are made: console variables and commands,
assets, components.

```c
tb_atom_table *const names = tb_atom_table_create(NULL);
const char *const fov = tb_atom_string(names, "r_fov");

if (tb_atom_string(names, typed_name) == fov) /* one pointer comparison */
{
    ...
}
tb_atom_table_destroy(names); /* every atom goes with it */
```

| Need | Use |
| --- | --- |
| A table of atoms | `tb_atom_table_create(allocator)`, `tb_atom_table_destroy(table)` |
| A table that every thread may use at once | `tb_atom_table_create_shared(allocator)` |
| The atom of a C string | `tb_atom_string(table, string)` |
| The atom of any bytes | `tb_atom_new(table, bytes, length)` |
| The atom of an integer, as decimal digits | `tb_atom_int(table, value)` |
| Look without creating | `tb_atom_find(table, bytes, length)`, NULL when there is none |
| Length and hash, in constant time | `tb_atom_length(atom)`, `tb_atom_hash(atom)` |
| How many atoms a table holds | `tb_atom_table_count(table)` |

**An atom is a `const char *`.** It points to the bytes followed by a
terminator, so an atom can be passed to anything that takes a C string. It may
hold any bytes, zero bytes included; `tb_atom_length` gives the true length.
The atom of a text is `tb_atom_new(table, text.bytes, text.length)`.

**Atoms live as long as their table.** They are never released one by one, so
an atom can be kept anywhere without counting references. Destroying the table
releases them all. Give a table an arena, and the arena releases everything at
once. Atoms share blocks of memory that start at a kilobyte and double up to
64 KiB, so interning a new name seldom reaches the allocator and a table of any
size is released in a few steps.

**Each table is its own world.** Equal strings interned in two tables are two
different pointers. Compare atoms only with atoms of the same table.

**Atoms are ready-made keys.** An atom is unique, so a hash table can use its
address as the key, and `tb_atom_hash` returns the hash of its bytes without
reading them again.

**Threading.** Atoms that already exist can be compared and read from any
thread. A table from `tb_atom_table_create` is externally synchronized:
interning from several threads needs a lock around it. A table from
`tb_atom_table_create_shared` synchronizes itself, and every function works
on it as on any table, so modules and worker threads can intern into one
table of names:

```c
tb_atom_table *const names = tb_atom_table_create_shared(NULL);

/* On any thread, at any time: */
const char *const name = tb_atom_string(names, "r_fov");
```

Equal bytes give the same pointer whichever threads intern them, and however
many do so at the same moment.

**A shared table is searched without a lock.** Nothing that a search reads is
ever changed or released while the table lives: an atom is complete before a
slot points to it, a slot keeps its atom, and an array of slots that the table
has outgrown is kept until the table is destroyed, for the threads that are
still searching it. So threads that intern names that are already atoms, which
is nearly every call, do not wait for one another. Only a name that is new
takes the table's lock, looks once more for an atom that another thread added
meanwhile, and adds it.

| `tb_bench atom`, nanoseconds for each call, a table of 100,000 names | Not shared | Shared | Shared, on each of 8 threads at once |
| --- | --- | --- | --- |
| Intern a name that is an atom | 30 | 30 | 54 to 67 |
| Intern a new name | 57 to 62 | 69 to 72 | 850 to 990 |

Release, MSVC 19.50, i9-14900HX, on the eight performance cores. Looking a
name up costs a shared table nothing, and eight threads together look up
about four times as many as one. New names are another matter: the eight wait
in line for one lock, and together add fewer in a second than one thread
alone. Share a table for names that are looked up far more often than they
are made, which is what atoms are for.

The kept arrays cost a shared table about twice the memory for slots of a
table that is not shared. Its allocator is called only under the table's
lock, so an allocator that nothing else uses need not be thread-safe. Creating
and destroying a shared table are still for one thread.

### Hashing

`hash.h` has the hash functions behind atoms and hashed collections, for code
that needs a hash of its own:

| Key | Function | Algorithm |
| --- | --- | --- |
| Bytes, strings | `tb_hash_bytes(bytes, length)`, `tb_hash_string(string)` | XXH64, the 64-bit function of xxHash, seeded with zero |
| Integers | `tb_hash_u64(value)` | The SplitMix64 finalizer |
| Addresses | `tb_hash_pointer(pointer)` | The same, on the address |

The results are the same on every platform and in every run, so they can be
stored. They are not randomized: do not use them for keys that someone hostile
chooses.
