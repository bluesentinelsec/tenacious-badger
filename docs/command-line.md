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

An entry with a `short_name` or `long_name` is an option; one with neither is
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

The parser allocates nothing. Text values point into the arguments, which
must outlive their use.

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
`main` its arguments in the system's code page, not in UTF-8.
