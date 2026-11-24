/* Time complete argv parses, including selection of a child command. */
#include "badger/cli.h"
#include "tb_bench.h"

#define COMMAND_COUNT 512

typedef struct lookup_case
{
    tb_cli_command root;
    size_t parses;
} lookup_case;

static void parse_commands(void *context)
{
    const lookup_case *sample = context;
    size_t found = 0;
    for (size_t i = 0; i < sample->parses; i++)
    {
        const tb_cli_command *child = &sample->root.commands[i % COMMAND_COUNT];
        const char *arguments[] = {"tool", child->name};
        tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
        if (tb_cli_parse(&sample->root, 2, arguments, &outcome) == TB_CLI_OK &&
            outcome.command == child)
        {
            found++;
        }
    }
    tb_bench_keep(found);
}

void tb_bench_cli(tb_bench *bench)
{
    if (!tb_bench_wants(bench, "cli"))
    {
        return;
    }
    tb_cli_command children[COMMAND_COUNT] = {0};
    char names[COMMAND_COUNT][16] = {0};
    for (size_t i = 0; i < COMMAND_COUNT; i++)
    {
        (void)tb_fmt_buffer(names[i], sizeof(names[i]), "cmd-%03zu", i);
        children[i].name = names[i];
    }
    lookup_case linear = {
        .root = {.name = "tool", .commands = children, .command_count = COMMAND_COUNT},
        .parses = tb_bench_size(bench, 100000)};
    lookup_case sorted = linear;
    sorted.root.commands_sorted = true;
    tb_bench_case cases[] = {{.name = "parse a command among 512 siblings",
                              .variant = "declaration order",
                              .run = parse_commands,
                              .operations = (double)linear.parses,
                              .context = &linear},
                             {.name = "parse a command among 512 siblings",
                              .variant = "sorted names",
                              .run = parse_commands,
                              .operations = (double)sorted.parses,
                              .context = &sorted}};
    tb_bench_measure(bench, "cli", cases, TB_CLI_COUNT_OF(cases));
}
