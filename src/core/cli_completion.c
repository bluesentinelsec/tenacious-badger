/* Native completion scripts track the selected command and consume option values. */
#include "badger/cli.h"
#include "badger/str.h"
typedef enum shell_kind
{
    BASH,
    ZSH,
    FISH,
    POWERSHELL
} shell_kind;
typedef struct generator
{
    shell_kind shell;
    tb_fmt_sink *sink;
    void *context;
    const tb_cli_command *path[TB_CLI_DEPTH];
    size_t next;
} generator;

static bool safe_word(const char *s)
{
    const char *alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:";
    return s != NULL && *s != '\0' && s[tb_str_span(s, 0, TB_STR_END, alphabet)] == '\0';
}

static bool supported(const tb_cli_command *cmd, size_t depth)
{
    if (depth >= TB_CLI_DEPTH || !safe_word(cmd->name))
    {
        return false;
    }
    for (size_t i = 0; i < cmd->option_count; i++)
    {
        const tb_cli_option *opt = &cmd->options[i];
        if ((opt->single_dash && !safe_word(opt->single_dash)) ||
            (opt->long_name && !safe_word(opt->long_name)))
        {
            return false;
        }
        if (opt->short_name)
        {
            const char short_name[] = {opt->short_name, 0};
            if (!safe_word(short_name))
            {
                return false;
            }
        }
    }
    for (size_t i = 0; i < cmd->command_count; i++)
    {
        if (!supported(&cmd->commands[i], depth + 1))
        {
            return false;
        }
    }
    return true;
}

static void token(generator *g, size_t state, const char *prefix, const char *name, bool candidates,
                  bool takes_value, size_t child)
{
    if (candidates)
    {
        (void)tb_fmt_write(g->sink, g->context, " %s%s", prefix, name);
        return;
    }
    const char *format = "'%zu/%s%s') %s=%zu ;;\n";
    if (g->shell == FISH)
    {
        format = "case '%zu/%s%s'\nset %s %zu\n";
    }
    if (g->shell == POWERSHELL)
    {
        format = "'%zu/%s%s' { $%s=%zu; break }\n";
    }
    (void)tb_fmt_write(g->sink, g->context, format, state, prefix, name, child ? "node" : "skip",
                       child ? child : (size_t)takes_value);
}

static void states(generator *g, const tb_cli_command *cmd, size_t depth, size_t state,
                   bool candidates)
{
    g->path[depth] = cmd;
    if (candidates)
    {
        const char *format = "'%zu') opts='-h --help";
        if (g->shell == FISH)
        {
            format = "case '%zu'\nset opts -h --help";
        }
        if (g->shell == POWERSHELL)
        {
            format = "'%zu' { $opts='-h --help";
        }
        (void)tb_fmt_write(g->sink, g->context, format, state);
        if (g->path[0]->version)
        {
            (void)tb_fmt_write(g->sink, g->context, " --version");
        }
    }
    for (size_t d = depth + 1; d > 0; d--)
    {
        const tb_cli_command *level = g->path[d - 1];
        for (size_t i = 0; i < level->option_count; i++)
        {
            const tb_cli_option *o = &level->options[i];
            bool value = !o->flag && !o->counter;
            if (o->short_name)
            {
                char name[] = {o->short_name, 0};
                token(g, state, "-", name, candidates, value, 0);
            }
            if (o->long_name)
            {
                token(g, state, "--", o->long_name, candidates, value, 0);
            }
            if (o->single_dash)
            {
                token(g, state, "-", o->single_dash, candidates, value, 0);
            }
        }
    }
    size_t first = g->next;
    g->next += cmd->command_count;
    for (size_t i = 0; i < cmd->command_count; i++)
    {
        token(g, state, "", cmd->commands[i].name, candidates, false, first + i);
    }
    if (candidates)
    {
        const char *end = "' ;;\n";
        if (g->shell == FISH)
        {
            end = "\n";
        }
        if (g->shell == POWERSHELL)
        {
            end = "' -split ' '; break }\n";
        }
        (void)tb_fmt_write(g->sink, g->context, "%s", end);
    }
    for (size_t i = 0; i < cmd->command_count; i++)
    {
        states(g, &cmd->commands[i], depth + 1, first + i, candidates);
    }
}

bool tb_cli_write_completion(const tb_cli_command *program, const char *shell, tb_fmt_sink *sink,
                             void *context)
{
    generator g = {.sink = sink, .context = context, .next = 1};
    if (!program || !shell || !sink || !supported(program, 0))
    {
        return false;
    }
    const char *name = program->name;
    const char *begin = NULL;
    const char *middle = NULL;
    const char *end = NULL;
    if (tb_str_equal(shell, "fish"))
    {
        g.shell = FISH;
        begin = "function __tb_%s\nset -l node 0\nset -l skip 0\nset -l opts\nset -l words "
                "(commandline -opc)\nfor w in $words[2..-1]\nif test $skip = 1\nset skip "
                "0\ncontinue\nend\nif test \"$w\" = --\nreturn\nend\nswitch \"$node/$w\"\n";
        middle = "end\nend\nif test $skip = 1\nreturn\nend\nswitch $node\n";
        end = "end\nprintf '%%s\\n' $opts\nend\ncomplete -c '%s' -f -a '(__tb_%s)'\n";
    }
    else if (tb_str_equal(shell, "powershell"))
    {
        g.shell = POWERSHELL;
        begin =
            "Register-ArgumentCompleter -Native -CommandName '%s' -ScriptBlock { "
            "param($wordToComplete,$commandAst,$cursorPosition)\n$node=0; $skip=0\nforeach ($e in "
            "($commandAst.CommandElements | Select-Object -Skip 1)) { if ($e.Extent.EndOffset -ge "
            "$cursorPosition) { break }; if ($skip) { $skip=0; continue }; $w=$e.Extent.Text; if "
            "($w -eq '--') { return }; switch -CaseSensitive (\"$node/$w\") {\n";
        middle = "} }\nif ($skip) { return }; switch ($node) {\n";
        end = "}\n$opts | Select-Object -Unique | Where-Object { $_.StartsWith($wordToComplete) } "
              "| ForEach-Object { [System.Management.Automation.CompletionResult]::new($_,$_, "
              "'ParameterValue', $_) }\n}\n";
    }
    else if (tb_str_equal(shell, "bash") || tb_str_equal(shell, "zsh"))
    {
        g.shell = tb_str_equal(shell, "bash") ? BASH : ZSH;
        begin = g.shell == BASH ? "__tb_%s() { local node=0 skip=0 opts w i; COMPREPLY=(); for "
                                  "((i=1;i<COMP_CWORD;i++)); do w=${COMP_WORDS[i]};\n"
                                : "__tb_%s() { local node=0 skip=0 opts w i; for "
                                  "((i=2;i<CURRENT;i++)); do w=${words[i]};\n";
        middle = "esac; done; ((skip)) && return; case $node in\n";
        end = g.shell == BASH
                  ? "esac; for w in $opts; do [[ $w == \"${COMP_WORDS[COMP_CWORD]}\"* ]] && "
                    "COMPREPLY+=(\"$w\"); done; return 0; }\ncomplete -F __tb_%s -- '%s'\n"
                  : "esac; compadd -- ${=opts}; }\ncompdef __tb_%s '%s'\n";
    }
    else
    {
        return false;
    }
    (void)tb_fmt_write(sink, context, begin, name);
    if (g.shell == BASH || g.shell == ZSH)
    {
        (void)tb_fmt_write(sink, context,
                           "if ((skip)); then skip=0; continue; fi; [[ $w == -- ]] && return; case "
                           "\"$node/$w\" in\n");
    }
    states(&g, program, 0, 0, false);
    (void)tb_fmt_write(sink, context, "%s", middle);
    g.next = 1;
    states(&g, program, 0, 0, true);
    (void)tb_fmt_write(sink, context, end, name, name);
    return true;
}
