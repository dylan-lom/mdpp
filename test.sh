#!/usr/bin/env sh
#
# Something I quickly cobbled together to run test cases

set -e

delim='========================='

run_test() {
    file="$1"
    dest="$2"
    casename="$(echo "$file" | rev | cut -d '.' -f2- | cut -d '/' -f1 | rev)"
    printf "%-35s" "Running test case \`$casename\`..."

    args_end="$(grep -n "$delim" "$file" | head -n 1 | cut -d ':' -f1)"
    input_end="$(grep -n "$delim" "$file" | tail -n 1 | cut -d ':' -f1)"

    if [ $(grep "$delim" "$file" | wc -l) -lt 2 ]; then
        echo "ERROR: Invalid test file '$file': missing delimiter" > /dev/stderr
        echo "INFO:    Test files require two delimiters, indicating end of args and input sections" > /dev/stderr
        echo "INFO:    Delimiter is: '$delim'" > /dev/stderr
        exit 1
    fi

    # FIXME: This seems flakey...
    args="$(head -n $(expr $args_end - 1) "$file")"
    input="$(head -n $(expr $input_end - 1) "$file" | tail -n $(expr $input_end - $args_end - 1))" 
    output="$(tail -n $(expr $(wc -l < "$file") - $input_end) "$file")"

    echo "$output" > "$dest/$casename.expected"
    echo "$input" | ./mdpp $args > "$dest/$casename.actual"

    if ! (diff -q \
            --label "$casename.expected" "$dest/$casename.expected" \
            --label "$casename.actual" "$dest/$casename.actual" > /dev/null); then
        printf "    [FAIL]\n"

        # Because we use set -e we need to handle diff's non-zero exit code here
        diff -up \
            --label "$casename.expected" "$dest/$casename.expected" \
            --label "$casename.actual" "$dest/$casename.actual" > /dev/stderr \
            || true
        return 1
    else
        printf "    [OK]\n"
        return 0
    fi
}

if [ ! -x "./mdpp" ]; then
    echo "ERROR: Executable './mdpp' not found!" > /dev/stderr
    echo "ERROR: Please compile the project and ensure it is executable." > /dev/stderr
    exit 1
fi

dest="$(mktemp -d)"

if [ "$#" -eq 0 ]; then
    echo "ERROR: No test cases were provided! Test cases must be specified via CLI arguments" > /dev/stderr
    echo "NOTE:  Usage example: \`./test.sh tests/*.test\`" > /dev/stderr
    exit 1
fi

total="$#"
failed=0
while [ "$#" -gt 0 ]; do
    run_test "$1" "$dest" || failed="$(($failed + 1))"
    shift
done

rm -r "$dest"

if [ "$failed" -gt 0 ]; then
    echo "$failed/$total test cases failed!"
    exit 1
else
    echo "All test cases passed!"
    exit 0
fi

