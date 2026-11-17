"""Check that every public function states its threading contract.

    python tools/check_contracts.py              check include/badger/*.h
    python tools/check_contracts.py --self-test  check the checker

AGENTS.md requires every public API to document one of three contracts:

    thread-safe               internally synchronized, or immutable
    stage-owned               called only from a documented stage or thread
    externally synchronized   the caller serializes access

A function states its contract in its own comment, under `@par Threading`. A
header whose functions nearly all have the same contract states it once, under
`@par Threading` in the comment that opens the file, and that covers every
function of the header that says nothing of its own.

The check does not judge whether a contract is true. It fails when a function
is covered by no statement at all, or by one that uses none of the three
terms, because then a caller cannot tell which of the three it is.
"""
import re
import sys
from pathlib import Path

TERMS = re.compile(r'thread-safe|stage-owned|externally\s+(\*\s+)?synchronized', re.IGNORECASE)
COMMENT = re.compile(r'/\*\*.*?\*/', re.DOTALL)
DECLARATION = re.compile(r'^TB_API\b[^\n]*?\b(tb_[a-z0-9_]+)\s*\(', re.MULTILINE)
# A paragraph ends at the next tag that starts a line of the comment, or at
# the end of the comment.
PARAGRAPH = re.compile(r'@par\s+Threading\b(.*?)(?=\n\s*\*?\s*@|\*/)', re.DOTALL)


def stated_contract(comment):
    """Whether a comment has a `@par Threading` paragraph that uses one of the terms."""
    paragraph = PARAGRAPH.search(comment)
    return paragraph is not None and TERMS.search(paragraph.group(1)) is not None


def uncovered(text):
    """The functions of a header that no threading contract covers, with their lines."""
    comments = list(COMMENT.finditer(text))
    file_comment = next((found for found in comments if '@file' in found.group(0)), None)
    has_default = file_comment is not None and stated_contract(file_comment.group(0))
    faults = []
    for declaration in DECLARATION.finditer(text):
        # The comment of a declaration is the one that ends just before it.
        before = text[:declaration.start()].rstrip()
        own = next((found for found in reversed(comments)
                    if found.end() == len(before)), None)
        if own is not None and stated_contract(own.group(0)):
            continue
        if has_default:
            continue
        faults.append((text.count('\n', 0, declaration.start()) + 1, declaration.group(1)))
    return faults


def check(root):
    faults = []
    count = 0
    for header in sorted((root / 'include' / 'badger').glob('*.h')):
        text = header.read_text(encoding='utf-8')
        count += len(DECLARATION.findall(text))
        for line, name in uncovered(text):
            faults.append(f'{header.relative_to(root).as_posix()}:{line}: {name} has no '
                          'threading contract: say thread-safe, stage-owned, or externally '
                          'synchronized under "@par Threading", in its comment or in the '
                          "header's")
    return faults, count


def self_test():
    with_default = '''/** @file a.h
 * @brief A.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call.
 */
/** @brief One. */
TB_API void tb_a_one(void);
/**
 * @brief Two.
 * @par Threading Thread-safe.
 */
TB_API int *tb_a_two(int value);
'''
    without_default = '''/** @file b.h
 * @brief B. All of it is safe, in other words.
 */
/** @brief One. */
TB_API void tb_b_one(void);

/**
 * @brief Two.
 * @par Threading Stage-owned: call during startup.
 * @return Nothing.
 */
TB_API void tb_b_two(void);
/** @brief Three.
 * @par Threading
 * Safe from any thread.
 */
TB_API const char *tb_b_three(void);
TB_API void tb_b_four(void);
/**
 * @brief Five.
 * @par Errors None.
 * @par Threading Externally
 * synchronized.
 */
TB_API _Noreturn void tb_b_five(const char *text,
                                int value);
'''
    vague_default = '''/** @file c.h
 * @par Threading
 * Take care.
 */
TB_API void tb_c_one(void);
'''
    answers = [
        (uncovered(with_default), []),
        (uncovered(without_default), [(5, 'tb_b_one'), (17, 'tb_b_three'), (18, 'tb_b_four')]),
        (uncovered(vague_default), [(5, 'tb_c_one')]),
        (stated_contract('/** @par Threading Thread-safe. */'), True),
        (stated_contract('/** @par Errors Thread-safe is not the point. */'), False),
        (stated_contract('/** @par Threading None.\n * @par Errors thread-safe */'), False),
    ]
    wrong = [(actual, wanted) for actual, wanted in answers if actual != wanted]
    for actual, wanted in wrong:
        print(f'self-test: got {actual!r}, expected {wanted!r}')
    return wrong


def main():
    if sys.argv[1:] == ['--self-test']:
        if self_test():
            sys.exit(1)
        print('The contract check gives the known answers.')
        return
    faults, count = check(Path(__file__).resolve().parent.parent)
    for fault in faults:
        print(fault)
    if faults:
        sys.exit(f'{len(faults)} of {count} public functions state no threading contract.')
    print(f'Checked {count} public functions: each states a threading contract.')


main()
