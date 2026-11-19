# JSONTestSuite

The `test_parsing` corpus of [JSONTestSuite](https://github.com/nst/JSONTestSuite)
by Nicolas Seriot, copied unchanged from commit
`1ef36fa01286573e846ac449e8683f8833c5b26a`. It is test data, licensed under
the MIT license in [LICENSE](LICENSE); none of it is built into the engine.

Each file is one text a JSON parser reads, and its prefix says what RFC 8259
asks of the parser:

- `y_`: accept it.
- `n_`: reject it.
- `i_`: either; the parser decides.

`tests/serialize/json_suite_test.c` reads every file. It requires the `y_` and
`n_` outcomes and pins this parser's choice for each `i_` file. To update the
corpus, replace `test_parsing` with a later commit's, record that commit here,
and settle any new `i_` file in the test.
