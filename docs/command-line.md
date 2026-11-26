# Command lines

`badger/cli.h` reads a program's command line into its variables, writes
its help, and says what is wrong with a command line that is wrong. It is
modeled on Python's argparse and on the command lines of `git`, `npm`, and
`cargo`: a program has options, arguments, and commands, and each command
has its own.

## Declaring a command line

A program declares what it accepts as data. Each option or argument is a
`tb_cli_option` whose destination is one of the program's variables; which
destination is set says what kind of entry it is:

| Destination | Takes | Writes |
| --- | --- | --- |
| `flag` | nothing | `true` |
| `counter` | nothing | one more each time, for `-vvv` |
| `text` | any text | the argument itself, borrowed |
| `integer` | `42`, `-7`, `0x2A`, within `minimum` to `maximum` when they differ | the value |
| `number` | a finite decimal | the value |
| `choice` | one of `choices` | its index |
| `texts` | any text, each time it is given | appended to an array of `capacity`, with `*count` |

An entry with a `short_name`, `long_name`, or `single_dash` is an option. An entry without any of these is
a positional argument, named by `name`. An argument with `texts` takes all
the arguments that are left, and comes last.

```c
static int64_t jobs = 4;   /* The default: what the variable holds. */
static const char *target = NULL;
static bool release = false;

static const tb_cli_option build_options[] = {
    {.short_name = 'j', .long_name = "jobs", .integer = &jobs, .minimum = 1, .maximum = 64,
     .name = "N", .help = "Run N jobs at once."},
    {.long_name = "release", .flag = &release, .help = "Build optimized."},
    {.name = "TARGET", .text = &target, .required = true, .help = "What to build."},
};
```

A `tb_cli_command` holds a table of entries and a table of the commands below
it. The program is the outermost command; its `name` is the program's name,
or, when NULL, the first argument's file name without `.exe`. A program with
a `version` accepts `--version`. A command may name a function to `run`.

## Reading it

```c
tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
if (tb_cli_parse(&program, (size_t)count, arguments, &outcome) != TB_CLI_OK)
{
    return tb_cli_finish(&outcome);
}
/* outcome.command is the command chosen; the variables hold the values. */
```

`tb_cli_run()` does both, then calls the chosen command's `run`, and returns
the status to exit with.

The command line may hold:

- **Options:**
  - short ones, which group: `-v`, `-vq`;
  - long ones: `--verbose`;
  - a value in either form: `-j4`, `-j 4`, `--jobs=4`, `--jobs 4`.
- **Arguments,** in the order the command declares them, between and after
  options.
- **`--`,** after which everything is an argument. `-` alone and negative
  numbers are arguments too.
- **A command's name,** which chooses it. After it, the options of the
  program and of every command chosen on the way are still accepted; a
  command's own option hides one of the same name above it.

A value that begins with `-`, other than a negative number, is not taken as
the value of the option before it: `--output --verbose` is an error, so a
forgotten value is caught. Write `--output=--verbose` for that name.

Use `.single_dash = "c:v"` for `-c:v libx264`. An exact match wins over a
short-option group. Other groups and attached values such as `-xzf archive.tar.gz`
and `-Iinclude` keep their usual meaning. For hundreds of sibling commands,
sort the table by `strcmp(name)` and set `.commands_sorted = true` on its parent
to use binary search.

## Environment and config files

A destination starts with its caller-supplied default. Set an option's
`.environment` and `.config_key` to opt into other sources. The first
available source supplies the value in this order: command line,
environment, config file, destination default. Normal conversion,
range and choice checks also apply to values from those sources.

```c
const char *path = NULL, *profile = "local";
tb_fs_buffer storage = {0};
const tb_cli_option options[] = {
    {.long_name = "config", .text = &path},
    {.long_name = "profile", .text = &profile,
     .environment = "APP_PROFILE", .config_key = "profile"},
};
const tb_cli_command program = {
    .name = "tool", .options = options, .option_count = TB_CLI_COUNT_OF(options),
    .config_path = &path, .config_storage = &storage,
};
```

With `profile = "staging"` in the file, `tool --config settings.conf`
selects staging. Setting `APP_PROFILE=prod` selects prod, and adding
`--profile test` selects test. Read the destinations after a successful
parse. Call `tb_fs_buffer_free(&storage)` after using them, including on
parse errors. Argument and environment strings also borrow their source
storage. Keep the environment stable while using borrowed values.

Config files support flat `key=value` lines, optional surrounding quotes,
blank lines, and whole-line `#` comments. The file limit is 1 MiB. The first
matching key supplies the value for an option. Unbound keys are ignored.
Flags accept `true` and `false`. A repeated text option takes its values from
the command line when present, or one fallback string. A parse without a
config file reads argv into caller-provided storage without allocating.

## Shell completion

Call `tb_cli_write_completion(&program, shell, sink, context)` to write a
registration script for `bash`, `zsh`, `fish`, or `powershell`. Source the
output in that shell (after `compinit` for Zsh). The generated script offers
commands and option names for the selected command, including inherited
options. The function returns false if the shell or a declaration's spelling
cannot be represented. See [the acceptance tests](../tests/core/cli_benchmark_test.c)
for complete declarations and command lines.

## What the user sees

`-h` and `--help` are accepted by every command. The help is wrapped at 80
columns:

```
usage: tool build [options] TARGET [FILE...]

Build a target.

arguments:
  TARGET                      What to build.
  FILE                        Sources to build it from.

options:
  -h, --help                  Show this help and exit.
  -j, --jobs N                Run N jobs at once. (1 to 64; default: 4)
      --release               Build optimized.

options of tool:
  -v, --verbose               Say more.
```

The help shows each option's range, choices, how often it may repeat, and
its default. A default is shown for text, integers, numbers, and choices
that are not required.

A wrong command line gets the usage and one line that says what is wrong.
Where a name is near what was typed, it is offered:

```
usage: tool [options] <command> ...
tool: error: unknown option '--jbos'; did you mean '--jobs'?
```

`tb_cli_finish()` writes the help or version to standard output and an error
to standard error. It returns `EXIT_SUCCESS`, or `TB_CLI_EXIT_USAGE` (2) after
an error, as argparse and most command-line programs do. Nothing here ends
the process: the program returns the status from `main`.
`tb_cli_write_help()` and `tb_cli_write_usage()` write to any sink.

## Contracts

A declaration that cannot be read is a contract violation, caught by an
assertion when it is parsed. That includes:

- an entry with no destination or two;
- `-h`, `--help`, or `--version` declared by the program;
- a name used twice in one command;
- a required argument after an optional one;
- an argument after one that takes all the rest;
- a command with both arguments and commands.

A command declares at most 128 entries, and commands nest at most 8 deep.

Messages and help are UTF-8. Arguments are not checked for UTF-8, because a
file name on Linux or macOS need not be. On Windows, the C runtime gives
`main` its arguments in the system's code page, not in UTF-8: get them from
`tb_process_get_arguments()` ([command-line arguments](platforms.md#command-line-arguments))
and parse those.

```c
int main(int count, char **values)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;
    int status = EXIT_FAILURE;
    if (!tb_process_get_arguments(count, values, &arguments))
    {
        return EXIT_FAILURE;
    }
    status = tb_cli_run(&program, arguments.count, arguments.values, NULL);
    tb_process_release_arguments(&arguments);
    return status;
}
```

## The engine's programs

Burrow and the sandbox declare their own command lines with this module,
and share some options by name, so a habit learned on one works on the
other: `--renderer NAME`, `--frames COUNT`, `--set NAME=VALUE`, and
`--log-file FILE`. The last sends standard output and standard error to the
file before anything is written ([logging](logging.md#writing-the-log-to-a-file)),
so such a program parses its command line with `tb_cli_parse()`, moves its
output, and then runs the command chosen, where `tb_cli_run()` would do the
first and the last together:

```c
tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
if (tb_cli_parse(&program, arguments.count, arguments.values, &outcome) != TB_CLI_OK)
{
    return tb_cli_finish(&outcome);
}
if (log_file != NULL && !tb_process_write_output_to(log_file))
{
    (void)tb_fmt_print_error("program: error: --log-file %s: the file could not be made\n",
                             log_file);
    return EXIT_FAILURE;
}
return outcome.command->run(&outcome, NULL);
```

## Checking the benchmark ladder

Run the following against a built tree. The acceptance cases check parsed
values and selected commands through the public API.

| Benchmark | Coverage |
| --- | --- |
| 1, 2, 3, 9, 11 | The `core_cli.*` suite covers defaults, conversion, required arguments, clusters, variadic arguments, command help, usage and typo suggestions. |
| 4, 5, 6, 8, 13 | The `cli_ladder.*` suite checks compiler include/define accumulation, Cargo choices and diagnostics, both FFmpeg single-dash spellings, and Git, tar, curl and Docker parsing. |
| 7 | The kubectl case selects every one of 512 children and checks both get and apply. The release benchmark measures the same number of siblings in declaration order and sorted order. |
| 10 | The Docker case declares a subcommand with two short flags, one long flag, three value options and a required image in fewer than 50 lines. |
| 12 | Generate Bash, Zsh, Fish and PowerShell registrations from the tree. Tests check subcommand and inherited option scope. |
| 14 | The random-argv case mixes arbitrary byte strings with valid options, clusters, malformed tokens and empty arguments. Each repetition uses a new seed. |
| 15 | The layered-source case reads a temporary file and checks default, file, environment and CLI precedence. |

```text
tb_tests "core_cli.*" "cli_ladder.*"
tb_bench --repeats 5 cli
tb_tests_sanitized --repeat 1000 cli_ladder.random_argv
```

The last command parses two million generated argument vectors. On Windows
the sanitizer build uses AddressSanitizer. Use a release build for timing,
since debug builds also check declarations on every parse. Rebuild binary
consumers when upgrading to 0.4.0 because the option and command structures
gained fields.
