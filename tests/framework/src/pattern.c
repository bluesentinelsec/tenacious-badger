#include "internal.h"

bool tb_test_pattern_matches(const char *pattern, const char *text)
{
    /* Where to resume after the most recent '*': it first matches nothing and
     * absorbs one more character of text each time the rest fails to match. */
    const char *resume_pattern = NULL;
    const char *resume_text = NULL;

    while (*text != '\0')
    {
        if (*pattern == '*')
        {
            pattern++;
            resume_pattern = pattern;
            resume_text = text;
            continue;
        }
        if (*pattern == '?' || *pattern == *text)
        {
            pattern++;
            text++;
            continue;
        }
        if (resume_pattern == NULL)
        {
            return false;
        }
        resume_text++;
        pattern = resume_pattern;
        text = resume_text;
    }
    while (*pattern == '*')
    {
        pattern++;
    }
    return *pattern == '\0';
}
