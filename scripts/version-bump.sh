#!/bin/sh
# Bump VERSION from hashed product sources (main.c + src/**/*.c).
# test/, docs/, include/ do not count.
#
#   minor  — if hash != .version-stamp, increment MINOR (0.1.0 -> 0.2.0)
#            first run (no stamp): record hash, do not bump
#   major  — always increment MAJOR and reset minor/patch (0.2.0 -> 1.0.0)
set -eu

kind=${1:-minor}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

VERSION_FILE=VERSION
STAMP=.version-stamp

hash_src() {
    {
        find src -name '*.c' -type f 2>/dev/null | sort
        printf '%s\n' main.c
    } | xargs -r sha256sum | sha256sum | awk '{ print $1 }'
}

read_ver() {
    tr -d ' \t\n' < "$VERSION_FILE"
}

write_ver() {
    printf '%s\n' "$1" > "$VERSION_FILE"
}

bump_minor() {
    old=$(read_ver)
    major=${old%%.*}
    rest=${old#*.}
    minor=${rest%%.*}
    minor=$((minor + 1))
    write_ver "${major}.${minor}.0"
}

bump_major() {
    old=$(read_ver)
    major=${old%%.*}
    major=$((major + 1))
    write_ver "${major}.0.0"
}

newhash=$(hash_src)

if [ "$kind" = major ]; then
    old=$(read_ver)
    bump_major
    printf '%s\n' "$newhash" > "$STAMP"
    printf 'version %s -> %s (major)\n' "$old" "$(read_ver)"
    exit 0
fi

if [ "$kind" != minor ]; then
    printf 'usage: %s minor|major\n' "$0" >&2
    exit 2
fi

if [ ! -f "$STAMP" ]; then
    printf '%s\n' "$newhash" > "$STAMP"
    printf 'version %s (baseline, no bump)\n' "$(read_ver)"
    exit 0
fi

oldhash=$(tr -d ' \t\n' < "$STAMP")
if [ "$newhash" = "$oldhash" ]; then
    printf 'version %s (src unchanged)\n' "$(read_ver)"
    exit 0
fi

old=$(read_ver)
bump_minor
printf '%s\n' "$newhash" > "$STAMP"
printf 'version %s -> %s (src changed)\n' "$old" "$(read_ver)"
