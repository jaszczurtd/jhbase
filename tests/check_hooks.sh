#!/usr/bin/env bash
# Exercise the tracked git hooks in a scratch repository: a copy of the current
# tree must commit under them, the commit-msg rules must hold, staged text must
# come out normalized and formatted, and a changed vendored Ryu file must be
# refused by the staged-tree check. Needs git, perl, cmake and clang-format.
set -euo pipefail
jb_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/jhbase-hooks-XXXXXX")"
trap 'rm -rf "$work"' EXIT
repo="$work/repo"
mkdir -p "$repo"
# Tracked and untracked-but-not-ignored files, as a commit would record them.
git -C "$jb_root" ls-files --cached --others --exclude-standard -z |
  tar -C "$jb_root" --null --verbatim-files-from --no-recursion -T - -cf - |
  tar -C "$repo" -xf -
git -C "$repo" init -q
git -C "$repo" config user.name "hooks check"
git -C "$repo" config user.email "hooks@example.invalid"
git -C "$repo" config core.hooksPath .githooks
chmod +x "$repo/.githooks/pre-commit" "$repo/.githooks/commit-msg"
commit() { git -C "$repo" commit -q -m "$1" >"$work/commit.log" 2>&1; }
fail() { printf 'check_hooks: %s\n' "$1" >&2; cat "$work/commit.log" >&2 || true; exit 1; }

git -C "$repo" add -A
commit "feat: scratch copy of the tree" || fail "the current tree does not commit under the hooks"

# commit-msg: the Conventional Commits subset, the subject length, trailers.
printf 'int x;\n' > "$repo/probe.h"; git -C "$repo" add probe.h
! commit "update stuff" || fail "a subject without type(scope): was accepted"
! commit "feat: $(printf 'x%.0s' $(seq 1 100))" || fail "a subject above 100 characters was accepted"
commit "$(printf 'feat(probe): add header\n\nCo-Authored-By: Someone <x@example.invalid>\n')" ||
  fail "a valid subject with a trailer was refused"
git -C "$repo" log -1 --format=%B | grep -qi "co-authored-by" && fail "the Co-Authored-By trailer was kept"

# pre-commit: normalization and formatting of staged files.
printf 'one  \r\nit\xe2\x80\x99s \xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x80\x94 done\xe2\x80\xa6' > "$repo/notes.md"
printf 'int  main( void ){return   0;}' > "$repo/probe.c"
git -C "$repo" add notes.md probe.c
commit "feat(probe): add notes and source" || fail "normalizable files were refused"
expected_notes=$(printf "one\nit's \"quoted\" - done...\n" | od -c)
[ "$(git -C "$repo" show HEAD:notes.md | od -c)" = "$expected_notes" ] || fail "notes.md was not normalized"
[ "$(git -C "$repo" show HEAD:probe.c)" = "$(printf 'int  main( void ){return   0;}' | clang-format "--assume-filename=$repo/probe.c")" ] ||
  fail "probe.c was not clang-formatted"

# pre-commit: the staged tree is checked, so a changed vendored Ryu file is refused.
printf '\n/* local edit */\n' >> "$repo/third_party/ryu/ryu/common.h"
git -C "$repo" add third_party/ryu/ryu/common.h
! commit "fix(ryu): local edit" || fail "a modified vendored Ryu file was accepted"
grep -q "Commit blocked" "$work/commit.log" || fail "the refusal did not name the staged-tree check"
git -C "$repo" checkout -q HEAD -- third_party/ryu/ryu/common.h
git -C "$repo" reset -q
# Downloaded data must not enter a normal commit alongside its pin.
mkdir -p "$repo/tests/fixtures/unicode"
for file in NormalizationTest.txt LICENSE.txt; do
  printf 'downloaded data\n' > "$repo/tests/fixtures/unicode/$file"
  git -C "$repo" add -A
  if git -C "$repo" ls-files --error-unmatch "tests/fixtures/unicode/$file" >/dev/null 2>&1; then
    fail "downloaded Unicode data would enter the commit"
  fi
done
# The staged snapshot validates the pin without downloading the data.
sed -i 's/^UNICODE_TEST_NORMALIZATION_SHA256=.*/UNICODE_TEST_NORMALIZATION_SHA256=00/' \
  "$repo/third_party/unicode_test_version.conf"
git -C "$repo" add third_party/unicode_test_version.conf
! commit "fix(unicode): invalid test pin" || fail "an invalid Unicode data pin was accepted"
grep -q "Commit blocked" "$work/commit.log" || fail "Unicode refusal bypassed the staged-tree check"
printf '%s\n' 'check_hooks: PASS (commit-msg rules, normalization, clang-format, staged-tree check)'
