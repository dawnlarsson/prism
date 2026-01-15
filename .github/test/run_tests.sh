#!/bin/bash
# Prism Test Runner
# Runs all test cases in test/case/ directory

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PRISM="${SCRIPT_DIR}/../prism"
CASE_DIR="${SCRIPT_DIR}/case"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

passed=0
failed=0
total=0

echo "=========================================="
echo "        Prism Compiler Test Suite        "
echo "=========================================="
echo ""

# Check if prism exists
if [ ! -f "$PRISM" ]; then
    echo -e "${YELLOW}Prism not found at $PRISM, building...${NC}"
    cc -o "$PRISM" "${SCRIPT_DIR}/../prism.c"
    if [ $? -ne 0 ]; then
        echo -e "${RED}Failed to build prism${NC}"
        exit 1
    fi
fi

for test_file in "$CASE_DIR"/*.c; do
    [ -f "$test_file" ] || continue
    
    test_name=$(basename "$test_file" .c)
    total=$((total + 1))
    
    printf "%-30s " "$test_name"
    
    # Run the test through prism
    output=$("$PRISM" "$test_file" 2>&1)
    exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}"
        passed=$((passed + 1))
    else
        echo -e "${RED}FAIL${NC}"
        failed=$((failed + 1))
        echo "  Exit code: $exit_code"
        echo "  Output: $output" | head -5
    fi
done

echo ""
echo "=========================================="
echo "Results: $passed/$total passed"
if [ $failed -gt 0 ]; then
    echo -e "${RED}$failed test(s) failed${NC}"
    exit 1
else
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
fi
