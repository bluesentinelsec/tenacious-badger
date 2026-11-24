#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* getenv returns borrowed storage by design. */
#endif
/* Real command lines from the CLI acceptance ladder. Each case checks where
 * the values landed as well as whether parsing succeeded. */
#include "badger/cli.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "tb_test.h"
#include <stdlib.h>

#define OPTIONS(a) .options = a, .option_count = TB_CLI_COUNT_OF(a)
#define COMMANDS(a) .commands = a, .command_count = TB_CLI_COUNT_OF(a)
#define PARSE(cmd, ...)                                                                            \
    tb_cli_parse(&(cmd), TB_CLI_COUNT_OF(((const char *[]){__VA_ARGS__})),                         \
                 (const char *[]){__VA_ARGS__}, &out)

TB_TEST(cli_ladder, compiler_and_multimedia_options)
{
    tb_cli_outcome out = {0};
    const char *includes[2] = {0}, *defines[2] = {0}, *source = NULL;
    size_t ni = 0, nd = 0;
    int64_t optimization = 0;
    bool warnings = false;
    const tb_cli_option flags[] = {
        {.short_name = 'O', .integer = &optimization},
        {.single_dash = "Wall", .flag = &warnings},
        {.short_name = 'I', .texts = includes, .capacity = 2, .count = &ni},
        {.short_name = 'D', .texts = defines, .capacity = 2, .count = &nd},
        {.name = "source", .text = &source, .required = true}};
    const tb_cli_command compiler = {.name = "clang", OPTIONS(flags)};
    TB_TEST_REQUIRE(PARSE(compiler, "clang", "-O2", "-Wall", "-Iinclude", "-Ithirdparty",
                          "-DDEBUG=1", "src/main.c") == TB_CLI_OK);
    TB_TEST_EXPECT(optimization == 2 && warnings && ni == 2 && nd == 1);
    TB_TEST_EXPECT_STRING(includes[0], ==, "include");
    TB_TEST_EXPECT_STRING(includes[1], ==, "thirdparty");
    TB_TEST_EXPECT_STRING(defines[0], ==, "DEBUG=1");
    TB_TEST_EXPECT_STRING(source, ==, "src/main.c");
    const char *input = NULL, *filter = NULL, *codec = NULL, *output = NULL;
    const tb_cli_option media[] = {{.short_name = 'i', .text = &input},
                                   {.single_dash = "vf", .text = &filter},
                                   {.single_dash = "c:v", .text = &codec},
                                   {.name = "output", .text = &output}};
    const tb_cli_command ffmpeg = {.name = "ffmpeg", OPTIONS(media)};
    TB_TEST_REQUIRE(PARSE(ffmpeg, "ffmpeg", "-i", "input.mp4", "-vf", "scale=1920:1080", "-c:v",
                          "libx264", "output.mp4") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(filter, ==, "scale=1920:1080");
    TB_TEST_EXPECT_STRING(codec, ==, "libx264");
    TB_TEST_EXPECT_STRING(input, ==, "input.mp4");
    TB_TEST_EXPECT_STRING(output, ==, "output.mp4");
}

TB_TEST(cli_ladder, git_and_docker)
{
    tb_cli_outcome out = {0};
    const char *remote = NULL, *branch = NULL, *message = NULL;
    const tb_cli_option repo[] = {{.name = "repository", .text = &remote, .required = true}};
    const tb_cli_option push[] = {{.name = "remote", .text = &remote},
                                  {.name = "branch", .text = &branch}};
    const tb_cli_option commit[] = {{.short_name = 'm', .text = &message}};
    const tb_cli_option checkout[] = {{.name = "branch", .text = &branch, .required = true}};
    const tb_cli_command verbs[] = {{.name = "clone", OPTIONS(repo)},
                                    {.name = "pull", OPTIONS(push)},
                                    {.name = "push", OPTIONS(push)},
                                    {.name = "commit", OPTIONS(commit)},
                                    {.name = "checkout", OPTIONS(checkout)}};
    const tb_cli_command git = {.name = "git", COMMANDS(verbs)};
    TB_TEST_REQUIRE(PARSE(git, "git", "clone", "repo") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(remote, ==, "repo");
    TB_TEST_REQUIRE(PARSE(git, "git", "pull") == TB_CLI_OK);
    TB_TEST_REQUIRE(PARSE(git, "git", "push", "origin", "main") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(remote, ==, "origin");
    TB_TEST_EXPECT_STRING(branch, ==, "main");
    TB_TEST_REQUIRE(PARSE(git, "git", "commit", "-m", "a message") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(message, ==, "a message");
    TB_TEST_REQUIRE(PARSE(git, "git", "checkout", "topic") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(branch, ==, "topic");
    bool rm = false, interactive = false, tty = false;
    const char *name = NULL, *ports[2] = {0}, *volume = NULL, *image = NULL;
    size_t n = 0;
    const tb_cli_option options[] = {
        {.long_name = "rm", .flag = &rm},
        {.short_name = 'i', .flag = &interactive},
        {.short_name = 't', .flag = &tty},
        {.long_name = "name", .text = &name},
        {.short_name = 'p', .texts = ports, .capacity = 2, .count = &n},
        {.short_name = 'v', .text = &volume},
        {.name = "image", .text = &image, .required = true}};
    const tb_cli_command commands[] = {{.name = "run", OPTIONS(options)}};
    const tb_cli_command docker = {.name = "docker", COMMANDS(commands)};
    TB_TEST_REQUIRE(PARSE(docker, "docker", "run", "--rm", "-it", "--name", "mycontainer", "-p",
                          "8080:80", "-v", "some dir:/app", "nginx", "-p", "443:443") == TB_CLI_OK);
    TB_TEST_EXPECT(rm && interactive && tty && n == 2);
    TB_TEST_EXPECT_STRING(image, ==, "nginx");
    TB_TEST_EXPECT_STRING(name, ==, "mycontainer");
    TB_TEST_EXPECT_STRING(volume, ==, "some dir:/app");
    TB_TEST_EXPECT_STRING(ports[0], ==, "8080:80");
    TB_TEST_EXPECT_STRING(ports[1], ==, "443:443");
}

TB_TEST(cli_ladder, gnu_tar_curl_and_cargo)
{
    tb_cli_outcome out = {0};
    bool a = false, b = false, c = false;
    const char *files[3] = {0}, *archive = NULL, *method = NULL, *url = NULL;
    size_t n = 0;
    const tb_cli_option grep_opts[] = {
        {.short_name = 'r', .flag = &a},
        {.short_name = 'i', .flag = &b},
        {.short_name = 'n', .flag = &c},
        {.name = "word", .texts = files, .capacity = 3, .count = &n}};
    const tb_cli_command grep = {.name = "grep", OPTIONS(grep_opts)};
    TB_TEST_REQUIRE(PARSE(grep, "grep", "-rin", "hello", "src/") == TB_CLI_OK);
    TB_TEST_EXPECT(a && b && c && n == 2);
    TB_TEST_EXPECT_STRING(files[0], ==, "hello");
    TB_TEST_EXPECT_STRING(files[1], ==, "src/");
    a = false;
    b = false;
    const tb_cli_option tar_opts[] = {{.short_name = 'x', .flag = &a},
                                      {.short_name = 'z', .flag = &b},
                                      {.short_name = 'f', .text = &archive}};
    const tb_cli_command tar = {.name = "tar", OPTIONS(tar_opts)};
    TB_TEST_REQUIRE(PARSE(tar, "tar", "-xzf", "file.tar.gz") == TB_CLI_OK);
    TB_TEST_EXPECT(a && b);
    TB_TEST_EXPECT_STRING(archive, ==, "file.tar.gz");
    n = 0;
    const tb_cli_option curl_opts[] = {
        {.short_name = 'H', .texts = files, .capacity = 3, .count = &n},
        {.short_name = 'X', .text = &method},
        {.name = "URL", .text = &url}};
    const tb_cli_command curl = {.name = "curl", OPTIONS(curl_opts)};
    TB_TEST_REQUIRE(PARSE(curl, "curl", "-H", "Auth: x", "-X", "POST", "url") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(files[0], ==, "Auth: x");
    TB_TEST_EXPECT_STRING(method, ==, "POST");
    TB_TEST_EXPECT_STRING(url, ==, "url");
    size_t target = 1;
    const char *targets[] = {"x86_64-pc-linux-gnu", "aarch64-apple-darwin"};
    const tb_cli_option cargo_opts[] = {
        {.long_name = "release", .flag = &c},
        {.long_name = "target", .choice = &target, .choices = targets, .choice_count = 2}};
    const tb_cli_command verbs[] = {{.name = "build", OPTIONS(cargo_opts)}};
    const tb_cli_command cargo = {.name = "cargo", COMMANDS(verbs)};
    TB_TEST_REQUIRE(PARSE(cargo, "cargo", "build", "--release", "--target",
                          "x86_64-pc-linux-gnu") == TB_CLI_OK);
    TB_TEST_EXPECT_UINT(target, ==, 0);
    TB_TEST_REQUIRE(PARSE(cargo, "cargo", "build", "--target", "x86") == TB_CLI_ERROR);
    TB_TEST_EXPECT(tb_str_find(out.message, 0, TB_STR_END, targets[0]) != TB_STR_NOT_FOUND &&
                   tb_str_find(out.message, 0, TB_STR_END, targets[1]) != TB_STR_NOT_FOUND);
}

TB_TEST(cli_ladder, large_sorted_command_tree)
{
    tb_cli_outcome out = {0};
    char names[512][16] = {0};
    tb_cli_command resources[512] = {0};
    const char *ns = NULL, *output = NULL, *file = NULL;
    const tb_cli_option opts[] = {{.long_name = "namespace", .text = &ns},
                                  {.long_name = "output", .text = &output}};
    for (size_t i = 0; i < 511; i++)
    {
        (void)tb_fmt_buffer(names[i], sizeof(names[i]), "object-%03zu", i);
        resources[i].name = names[i];
    }
    resources[511] = (tb_cli_command){.name = "pods", OPTIONS(opts)};
    const tb_cli_option apply[] = {{.short_name = 'f', .text = &file}};
    const tb_cli_command verbs[] = {{.name = "apply", OPTIONS(apply)},
                                    {.name = "get", COMMANDS(resources), .commands_sorted = true}};
    const tb_cli_command kubectl = {.name = "kubectl", COMMANDS(verbs)};
    TB_TEST_REQUIRE(PARSE(kubectl, "kubectl", "get", "pods", "--namespace", "prod", "--output",
                          "json") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(ns, ==, "prod");
    TB_TEST_EXPECT_STRING(output, ==, "json");
    TB_TEST_REQUIRE(PARSE(kubectl, "kubectl", "apply", "-f", "deployment.yaml") == TB_CLI_OK);
    TB_TEST_EXPECT_STRING(file, ==, "deployment.yaml");
    for (size_t i = 0; i < 511; i++)
    {
        TB_TEST_REQUIRE(PARSE(kubectl, "kubectl", "get", names[i]) == TB_CLI_OK);
        TB_TEST_REQUIRE(out.command == &resources[i]);
    }
}

TB_TEST(cli_ladder, random_argv)
{
    const char *pool[] = {
        "--",       "------", "-vvvvvvvvvvvvv", "",      "--=--",
        "--input=", "-v",     "--input",        "value", "-j4",
        "-j",       "-0x1",   "--help",         "-",     "--jobs=9223372036854775808"};
    for (size_t trial = 0; trial < 2000; trial++)
    {
        char random[80] = {0};
        size_t length = (size_t)tb_test_random_below(sizeof(random));
        tb_test_random_bytes(random, length);
        const char *argv[10] = {"app"};
        size_t argc = 1 + (size_t)tb_test_random_below(10);
        for (size_t i = 1; i < argc; i++)
        {
            size_t k = (size_t)tb_test_random_below(TB_CLI_COUNT_OF(pool) + 1);
            argv[i] = k == TB_CLI_COUNT_OF(pool) ? random : pool[k];
        }
        const char *inputs[3] = {0}, *file = NULL;
        size_t count = 0, verbose = 0;
        int64_t jobs = 5;
        const tb_cli_option opts[] = {
            {.short_name = 'v', .counter = &verbose},
            {.short_name = 'j', .long_name = "jobs", .integer = &jobs},
            {.long_name = "input", .texts = inputs, .capacity = 3, .count = &count},
            {.name = "file", .text = &file}};
        const tb_cli_command app = {.name = "app", OPTIONS(opts)};
        tb_cli_outcome out = {0};
        tb_cli_result result = tb_cli_parse(&app, argc, argv, &out);
        TB_TEST_REQUIRE(out.result == result && out.command == &app && count <= 3);
        TB_TEST_REQUIRE((result == TB_CLI_ERROR) == (out.message[0] != '\0'));
    }
}

TB_TEST(cli_ladder, cli_over_environment_over_file_over_default)
{
    char root[32768] = {0}, path[32768] = {0};
    size_t required = 0;
    TB_TEST_REQUIRE(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)) == TB_FS_OK);
    tb_path_view dir = {root, tb_str_length(root)};
    TB_TEST_REQUIRE(tb_path_join(tb_fs_native_style(), dir, (tb_path_view){"config.toml", 11}, path,
                                 sizeof(path), &required) == TB_PATH_OK);
    tb_path_view filename = {path, tb_str_length(path)};
    const char contents[] = "# flat config\ncount = 7\nprofile = 'from file'\n";
    TB_TEST_REQUIRE(tb_fs_write_file(filename, contents, sizeof(contents) - 1) == TB_FS_OK);
    tb_fs_buffer storage = {0};
    tb_cli_outcome out = {0};
    const char *config = NULL, *profile = NULL;
    int64_t count = 5;
    tb_cli_option opts[] = {{.long_name = "config", .text = &config},
                            {.long_name = "count", .config_key = "count", .integer = &count},
                            {.long_name = "profile", .config_key = "profile", .text = &profile}};
    const tb_cli_command app = {
        .name = "app", OPTIONS(opts), .config_path = &config, .config_storage = &storage};
    TB_TEST_EXPECT(PARSE(app, "app") == TB_CLI_OK && count == 5);
    TB_TEST_EXPECT(PARSE(app, "app", "--config", path) == TB_CLI_OK && count == 7);
    TB_TEST_EXPECT_STRING(profile, ==, "from file");
    tb_fs_buffer_free(&storage);
    opts[2].environment = "PATH";
    if (getenv("PATH") != NULL)
    {
        TB_TEST_EXPECT(PARSE(app, "app", "--config", path) == TB_CLI_OK);
        TB_TEST_EXPECT_STRING(profile, ==, getenv("PATH"));
        tb_fs_buffer_free(&storage);
    }
    TB_TEST_EXPECT(PARSE(app, "app", "--profile", "staging", "--config", path, "--count=9") ==
                       TB_CLI_OK &&
                   count == 9);
    TB_TEST_EXPECT_STRING(profile, ==, "staging");
    tb_fs_buffer_free(&storage);
    TB_TEST_EXPECT(tb_fs_remove(filename) == TB_FS_OK);
    TB_TEST_EXPECT(PARSE(app, "app", "--config", path) == TB_CLI_ERROR);
    TB_TEST_EXPECT(tb_fs_remove(dir) == TB_FS_OK);
}
