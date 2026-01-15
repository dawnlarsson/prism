#!/bin/bash
# Prism Verbose Test Runner
# Shows output of each test for manual verification

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PRISM="${SCRIPT_DIR}/../prism"
CASE_DIR="${SCRIPT_DIR}/case"

echo "=========================================="
echo "    Prism Compiler Test Suite (Verbose)  "
echo "=========================================="

# Build prism if needed
if [ ! -f "$PRISM" ]; then
    cc -o "$PRISM" "${SCRIPT_DIR}/../prism.c"
fi

for test_file in "$CASE_DIR"/defer*.c; do
    [ -f "$test_file" ] || continue
    
    test_name=$(basename "$test_file" .c)
    
    echo ""
    echo "=========================================="
    echo "TEST: $test_name"
    echo "=========================================="
    
    "$PRISM" "$test_file"
    echo ""
done
