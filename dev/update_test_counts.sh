#!/usr/bin/env bash

# Paths to test executables
BUILD_DIR="cmake-build-debug"
MOMUS_KAIROS="$BUILD_DIR/momus/kairos/momus_kairos"
MOMUS_HERMES="$BUILD_DIR/momus/hermes/momus_hermes"
MOMUS_HEPHAESTUS="$BUILD_DIR/momus/hephaestus/momus_hephaestus"
HADES_DIR="hades"

# 1. Count Kairos tests
if [ -f "$MOMUS_KAIROS" ]; then
    KAIROS_COUNT=$($MOMUS_KAIROS --gtest_list_tests | grep "^  " | wc -l)
else
    KAIROS_COUNT=0
    echo "Warning: $MOMUS_KAIROS not found. Build it first."
fi

# 2. Count Hermes tests
if [ -f "$MOMUS_HERMES" ]; then
    HERMES_COUNT=$($MOMUS_HERMES --gtest_list_tests | grep "^  " | wc -l)
else
    HERMES_COUNT=0
fi

# 3. Count Hephaestus tests
if [ -f "$MOMUS_HEPHAESTUS" ]; then
    HEPHAESTUS_COUNT=$($MOMUS_HEPHAESTUS --gtest_list_tests | grep "^  " | wc -l)
else
    HEPHAESTUS_COUNT=0
fi

# 4. Count Hades tests (from test-results.json if it exists)
if [ -f "$HADES_DIR/test-results.json" ]; then
    HADES_COUNT=$(cat "$HADES_DIR/test-results.json" | jq '.numTotalTests')
else
    HADES_COUNT=0
    echo "Warning: $HADES_DIR/test-results.json not found. Run 'pnpm test:run' in hades directory first."
fi

TOTAL_COUNT=$((KAIROS_COUNT + HERMES_COUNT + HEPHAESTUS_COUNT + HADES_COUNT))

echo "Test Summary:"
echo "  Kairos:     $KAIROS_COUNT"
echo "  Hades:      $HADES_COUNT"
echo "  Hermes:     $HERMES_COUNT"
echo "  Hephaestus: $HEPHAESTUS_COUNT"
echo "  Total:      $TOTAL_COUNT"

# 5. Update README.md
if [ -f "README.md" ]; then
    # Update individual counts
    sed -i "s/| \*\*Kairos\*\* | Alpha | \`momus_kairos\` | [0-9]\+ /| \*\*Kairos\*\* | Alpha | \`momus_kairos\` | $KAIROS_COUNT /" README.md
    sed -i "s/| \*\*Hades\*\* | Alpha | \`vitest\` | [0-9]\+ /| \*\*Hades\*\* | Alpha | \`vitest\` | $HADES_COUNT /" README.md
    sed -i "s/| \*\*Hermes\*\* | Alpha | \`momus_hermes\` | [0-9]\+ /| \*\*Hermes\*\* | Alpha | \`momus_hermes\` | $HERMES_COUNT /" README.md
    sed -i "s/| \*\*Hephaestus\*\* | Alpha | \`momus_hephaestus\` | [0-9]\+ /| \*\*Hephaestus\*\* | Alpha | \`momus_hephaestus\` | $HEPHAESTUS_COUNT /" README.md

    # Update total summary line
    # Matches: *Pantheon currently runs **435 automated tests** ...
    sed -i "s/\*Pantheon currently runs \*\*[0-9]\+ automated tests\*\*/\*Pantheon currently runs \*\*$TOTAL_COUNT automated tests\*\*/" README.md

    echo "README.md updated with $TOTAL_COUNT tests."
else
    echo "Error: README.md not found."
fi
