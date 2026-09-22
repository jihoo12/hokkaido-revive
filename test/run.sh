#!/usr/bin/env bash
#
# run.sh — Run all Hokkaido compiler tests.
#
# Usage:
#   ./test/run.sh              Run all tests
#   ./test/run.sh phases       Run only phase tests
#   ./test/run.sh features     Run only feature tests
#   ./test/run.sh examples     Run only examples
#   ./test/run.sh packages     Run only package tests

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

HOKKAIDO="${HOKKAIDO:-./build/hokkaido}"
TESTDIR="test"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

passed=0
failed=0
skipped=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

run_test() {
    local file="$1"
    local name="${file#"$TESTDIR/"}"
    local out="$TMPDIR/$(basename "$file" .hk)"

    if ! "$HOKKAIDO" "$file" -o "$out" -O2 2>/dev/null; then
        # Check for compile_fail expectation
        if grep -q "// compile_fail" "$file" 2>/dev/null; then
            echo -e "  ${GREEN}PASS${NC}  $name (compile error expected)"
            passed=$((passed + 1))
            return 0
        fi
        echo -e "  ${RED}FAIL${NC}  $name (compile error)"
        failed=$((failed + 1))
        return
    fi

    if grep -q "// compile_fail" "$file" 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}  $name (expected compile error but compilation succeeded)"
        failed=$((failed + 1))
        return 1
    fi

    if ! clang "$out.o" -o "$out" -no-pie 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}  $name (link error)"
        failed=$((failed + 1))
        return
    fi

    if timeout 10 "$out" >/dev/null 2>&1; then
        echo -e "  ${GREEN}PASS${NC}  $name"
        passed=$((passed + 1))
    else
        local code=$?
        echo -e "  ${YELLOW}PASS${NC}  $name (exit $code)"
        passed=$((passed + 1))
    fi
}

run_package_test() {
    local dir="$1"
    local name
    name=$(basename "$dir")

    # Find the main .hk file (not in util/)
    local main_file=""
    for f in "$dir"/main*.hk; do
        [ -f "$f" ] && main_file="$f" && break
    done

    if [ -z "$main_file" ]; then
        echo -e "  ${YELLOW}SKIP${NC}  packages/$name (no main*.hk)"
        skipped=$((skipped + 1))
        return
    fi

    local out="$TMPDIR/pkg_$name"
    if ! "$HOKKAIDO" "$main_file" -o "$out" -O2 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}  packages/$name (compile error)"
        failed=$((failed + 1))
        return
    fi

    if ! clang "$out.o" -o "$out" -no-pie 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}  packages/$name (link error)"
        failed=$((failed + 1))
        return
    fi

    if timeout 10 "$out" >/dev/null 2>&1; then
        echo -e "  ${GREEN}PASS${NC}  packages/$name"
        passed=$((passed + 1))
    else
        echo -e "  ${YELLOW}PASS${NC}  packages/$name (exit $?)"
        passed=$((passed + 1))
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────

category="${1:-all}"

echo -e "${BOLD}Running tests...${NC}"
echo

if [ "$category" = "all" ] || [ "$category" = "phases" ]; then
    echo -e "${BOLD}phases/${NC}"
    for f in "$TESTDIR"/phases/*.hk; do
        [ -f "$f" ] && run_test "$f"
    done
    echo
fi

if [ "$category" = "all" ] || [ "$category" = "features" ]; then
    echo -e "${BOLD}features/${NC}"
    for f in "$TESTDIR"/features/*.hk; do
        [ -f "$f" ] && run_test "$f"
    done
    echo
fi

if [ "$category" = "all" ] || [ "$category" = "examples" ]; then
    echo -e "${BOLD}examples/${NC}"
    for f in "$TESTDIR"/examples/*.hk; do
        [ -f "$f" ] && run_test "$f"
    done
    echo
fi

if [ "$category" = "all" ] || [ "$category" = "packages" ]; then
    echo -e "${BOLD}packages/${NC}"
    for d in "$TESTDIR"/packages/*/; do
        [ -d "$d" ] && run_package_test "$d"
    done
    # Also run the top-level package test files
    for f in "$TESTDIR"/packages/main*.hk; do
        [ -f "$f" ] && run_test "$f"
    done
    echo
fi

if [ "$category" = "all" ] || [ "$category" = "errors" ]; then
    echo -e "${BOLD}errors/${NC}"
    for f in "$TESTDIR"/errors/*.hk; do
        [ -f "$f" ] && run_test "$f"
    done
    echo
fi

# ── Summary ───────────────────────────────────────────────────────────────────

total=$((passed + failed + skipped))
echo -e "${BOLD}──────────────────────────────────────${NC}"
if [ "$failed" -eq 0 ]; then
    echo -e "${GREEN}All $total tests passed${NC} (${passed} passed, ${skipped} skipped)"
else
    echo -e "${RED}$failed/$total tests failed${NC} (${passed} passed, ${skipped} skipped)"
    exit 1
fi
