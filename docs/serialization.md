# Serialization

The `serialize` module reads and writes structured data. Its code lives in
`src/serialize/`. Everything is the engine's own C; no parsing library is used.

## JSON

`badger/json.h` reads JSON as RFC 8259 defines it, in two ways, and writes it:

- **A reader** (`tb_json_reader_create`, `tb_json_reader_next`) reports each
  value as an event: a null, a boolean, a number, a string, a key, or the
  start or end of an array or object. It allocates once, a stack as deep as
  the nesting allowed, and never copies the text: a string's token points into
  the text, escapes and all, and `tb_json_unescape` decodes it.
- **A document** (`tb_json_parse`) is the whole text as a tree of values in an
  arena of the caller's. Strings are decoded, numbers keep their text, and the
  tree does not borrow the text. `tb_json_find` looks a member up in time
  logarithmic in the object's size; `tb_json_member_at` walks members in the
  order of the text. `tb_json_offset_of` says where a value began in the
  text, for a caller that checks what a document means, as
  [layout files](gui.md#layout-files) do, to say where a value it refuses is.

### Untrusted text

Configuration, mods, and downloads reach the parser, so it treats text as
hostile:

- **No recursion.** The reader and the document builder keep explicit stacks,
  so no nesting exhausts the C stack. Nesting beyond `max_depth` (256 by
  default) is `TB_JSON_TOO_DEEP`, and text beyond `max_length` (256 MiB by
  default) is `TB_JSON_TOO_LARGE`.
- **Strict strings.** Invalid UTF-8, control characters, unknown escapes,
  bad `\u` escapes, and surrogates that are not a high-low pair are errors.
- **Strict numbers.** Leading zeros, `1.`, `.1`, `+1`, `1e`, hexadecimal,
  `Infinity`, and `NaN` are errors.
- **Duplicate keys** are `TB_JSON_DUPLICATE_KEY` unless allowed. Members are
  sorted by key once per object (heap sort, `O(n log n)` for any keys), which
  finds every duplicate and serves lookups; a text built to make lookups
  quadratic gains nothing.
- **Errors say where.** `tb_json_error` holds the result, the byte offset, the
  line and column (in bytes, from 1), and a short reason.

### Numbers

A number keeps its text. `tb_json_as_int64` and `tb_json_as_uint64` read an
integer exactly when it has no fraction or exponent and fits; `-0` is zero.
`tb_json_as_double` gives the nearest double through `badger/decimal.h`, and
refuses numbers too large for one. `tb_json_as_text` gives the text itself.

### Relaxed forms

Files people edit may opt into `TB_JSON_ALLOW_COMMENTS` (`//` and `/* */`),
`TB_JSON_ALLOW_TRAILING_COMMAS`, and `TB_JSON_ALLOW_DUPLICATE_KEYS` (every
member kept; a lookup finds the last). All are off by default.

### Threading and memory

A reader is externally synchronized. A document is immutable, so any number
of threads may read it, for as long as its arena is neither reset nor
destroyed. Parsing into an arena that cannot grow is `TB_JSON_NO_MEMORY`; a
mark taken before the call and rewound after releases a failed document's
parts. The work space for open containers is on the heap and freed before
`tb_json_parse` returns.

### Writing

A writer (`tb_json_writer_create`) writes one document to a
[stream](streams.md), through a buffer of 4 KiB, in one of two styles:
`TB_JSON_COMPACT`, with no space at all, or `TB_JSON_INDENTED`, a value per
line indented two spaces a level. It writes nulls, booleans, `int64_t` and
`uint64_t` exactly, doubles as their shortest text that reads back as the same
double, number text checked against RFC 8259's grammar, strings, keys, and
arrays and objects. `tb_json_write_value` writes a whole document, without
recursion; numbers keep their text.

- **What JSON cannot hold is a result.** Infinity, NaN, strings or keys that
  are not UTF-8, and number text that is not a JSON number are
  `TB_JSON_INVALID`, nothing is written, and the document can go on.
- **Misuse is a contract violation.** A key outside an object, a value in an
  object without a key, an end that does not match its beginning, a second
  value at the top, and a finish before the document is complete fail an
  assertion.
- **Strings escape what JSON requires**: `"`, `\`, and control characters,
  as `\b`, `\f`, `\n`, `\r`, `\t`, or `\u00XX`. Everything else, including
  UTF-8 and `/`, is written as it is.
- **A sink that refuses** makes the writer return `TB_JSON_OUTPUT_FAILED` from
  then on, including from `tb_json_writer_finish`, which passes the last of the
  buffer to the sink.

Whatever a writer writes, the reader reads back: the tests parse random
documents, write them in both styles, parse them again, and require the second
compact writing to equal the first byte for byte.

### Conformance

The tests read all 318 texts of
[JSONTestSuite](https://github.com/nst/JSONTestSuite)'s parsing corpus, kept in
`tests/data/jsontestsuite`. Every text RFC 8259 says to accept is accepted, and
writes back exactly; the two with duplicate keys need
`TB_JSON_ALLOW_DUPLICATE_KEYS`. Every text it says to reject is rejected. Of
the texts it leaves to the parser:

- **Numbers beyond a double's range** are accepted. The text is kept, and
  converting it is the caller's choice.
- **Strings that do not decode to UTF-8** are rejected: an escaped surrogate
  without its partner, bytes that are not UTF-8, and text in UTF-16 or
  Latin-1.
- **500 nested arrays** are deeper than the default limit.
- **A byte order mark** is rejected; RFC 8259 forbids writing one.

`tb_bench json` times the reader, the document, and the writer on an asset
manifest.
