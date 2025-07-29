#!/bin/bash
#
# FIO style checker
# Checks for common style issues in the codebase
#

EXIT_CODE=0
VERBOSE=${VERBOSE:-0}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print errors
error() {
    echo -e "${RED}ERROR:${NC} $1"
    EXIT_CODE=1
}

# Function to print warnings
warning() {
    echo -e "${YELLOW}WARNING:${NC} $1"
}

# Function to print success
success() {
    echo -e "${GREEN}OK:${NC} $1"
}

# Check for trailing whitespace
check_trailing_whitespace() {
    local files_with_trailing_space=""

    # Check C files, headers, and makefiles
    while IFS= read -r -d '' file; do
        if grep -l '[[:space:]]$' "$file" > /dev/null 2>&1; then
            files_with_trailing_space="${files_with_trailing_space}${file}\n"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "Trailing whitespace in $file:"
                grep -n '[[:space:]]$' "$file" | head -5
            fi
        fi
    done < <(find . -name "*.c" -o -name "*.h" -o -name "Makefile" -o -name "*.md" | \
             grep -v "^\./\.git" | \
             grep -v "^\./build" | \
             grep -v "^\./mock-tests/build" | \
             tr '\n' '\0')

    if [ -n "$files_with_trailing_space" ]; then
        error "Files with trailing whitespace:"
        echo -e "$files_with_trailing_space"
        return 1
    else
        success "No trailing whitespace found"
        return 0
    fi
}

# Check for missing newline at end of file
check_missing_newline() {
    local files_without_newline=""

    while IFS= read -r -d '' file; do
        if [ -f "$file" ] && [ -s "$file" ]; then
            # Check if last character is a newline
            if [ "$(tail -c 1 "$file" | wc -l)" -eq 0 ]; then
                files_without_newline="${files_without_newline}${file}\n"
            fi
        fi
    done < <(find . -name "*.c" -o -name "*.h" -o -name "Makefile" -o -name "*.md" | \
             grep -v "^\./\.git" | \
             grep -v "^\./build" | \
             grep -v "^\./mock-tests/build" | \
             tr '\n' '\0')

    if [ -n "$files_without_newline" ]; then
        error "Files without newline at end:"
        echo -e "$files_without_newline"
        return 1
    else
        success "All files end with newline"
        return 0
    fi
}

# Check for tabs in C files (FIO uses tabs)
check_indentation() {
    local files_with_spaces=""

    # FIO uses tabs for indentation in C files
    while IFS= read -r -d '' file; do
        # Check for lines that start with spaces (not tabs)
        if grep -l '^[[:space:]]\{1,\}[^[:space:]*]' "$file" | grep -v '^[[:space:]]*\*' > /dev/null 2>&1; then
            # Exclude comment continuation lines
            if grep '^[[:space:]]\{1,\}[^[:space:]*]' "$file" | grep -v '^ \*' > /dev/null 2>&1; then
                files_with_spaces="${files_with_spaces}${file}\n"
                if [ "$VERBOSE" -eq 1 ]; then
                    echo "Space indentation in $file:"
                    grep -n '^[[:space:]]\{1,\}[^[:space:]*]' "$file" | grep -v '^ \*' | head -5
                fi
            fi
        fi
    done < <(find . -name "*.c" -o -name "*.h" | \
             grep -v "^\./\.git" | \
             grep -v "^\./build" | \
             grep -v "^\./mock-tests/build" | \
             tr '\n' '\0')

    if [ -n "$files_with_spaces" ]; then
        warning "Files with space indentation (FIO uses tabs):"
        echo -e "$files_with_spaces"
        # This is a warning, not an error
        return 0
    else
        success "Indentation style consistent"
        return 0
    fi
}

# Check for lines longer than 80 characters
check_line_length() {
    local long_lines_found=0

    while IFS= read -r -d '' file; do
        if grep -n '^.\{81,\}' "$file" > /dev/null 2>&1; then
            if [ "$VERBOSE" -eq 1 ]; then
                echo "Long lines in $file:"
                grep -n '^.\{81,\}' "$file" | head -5
            fi
            long_lines_found=1
        fi
    done < <(find . -name "*.c" -o -name "*.h" | \
             grep -v "^\./\.git" | \
             grep -v "^\./build" | \
             grep -v "^\./mock-tests/build" | \
             tr '\n' '\0')

    if [ "$long_lines_found" -eq 1 ]; then
        warning "Files with lines > 80 characters found (use VERBOSE=1 to see)"
    else
        success "Line length check passed"
    fi

    return 0
}

# Main
echo "FIO Style Checker"
echo "================="
echo

check_trailing_whitespace
check_missing_newline
check_indentation
check_line_length

echo
if [ "$EXIT_CODE" -eq 0 ]; then
    success "All style checks passed!"
else
    error "Style check failed with $EXIT_CODE error(s)"
fi

exit $EXIT_CODE