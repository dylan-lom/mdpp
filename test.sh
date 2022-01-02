#!/usr/bin/env sh
#
# Something I quickly cobbled together to run test cases

set -e

delim='========================='

if [ ! -x "./mdpp" ]; then
    echo "ERROR: Executable './mdpp' not found!" > /dev/stderr
    echo "ERROR: Please compile the project and ensure it is executable." > /dev/stderr
    exit 1
fi

dest="$(mktemp -d)"
failed=0

for f in tests/*.test; do
    casename="$(echo "$f" | rev | cut -d '.' -f2- | cut -d '/' -f1 | rev)"
    echo "Running test case \`$casename\`..."

    args_end="$(grep -n "$delim" "$f" | head -n 1 | cut -d ':' -f1)"
    input_end="$(grep -n "$delim" "$f" | tail -n 1 | cut -d ':' -f1)"

    if [ $(grep "$delim" "$f" | wc -l) -lt 2 ]; then
        echo "ERROR: Invalid test file '$f': missing delimiter" > /dev/stderr
        echo "INFO:    Test files require two delimiters, indicating end of args and input sections" > /dev/stderr
        echo "INFO:    Delimiter is: '$delim'" > /dev/stderr
        exit 1
    fi

    # FIXME: This seems flakey...
    args="$(head -n $(expr $args_end - 1) "$f")"
    input="$(head -n $(expr $input_end - 1) "$f" | tail -n $(expr $input_end - $args_end - 1))" 
    output="$(tail -n $(expr $(wc -l < "$f") - $input_end) "$f")"

    echo "$output" > "$dest/$casename.expected"
    echo "$input" | ./mdpp $args > "$dest/$casename.actual"

    if ! (diff -q \
            --label "$casename.expected" "$dest/$casename.expected" \
            --label "$casename.actual" "$dest/$casename.actual"); then
        # Because we use set -e we need to handle diff's non-zero exit code here
        diff -up \
            --label "$casename.expected" "$dest/$casename.expected" \
            --label "$casename.actual" "$dest/$casename.actual" > /dev/stderr \
            || true

        md5sum "$dest/$casename."*

        failed="$(expr $failed + 1)"
        echo "$dest"
        exit 1
    fi
done

rm -r "$dest"

if [ "$failed" -gt 0 ]; then
    total="$(ls -1 tests/*.test | wc -l)"
    echo "$failed/$total test cases failed!"
    exit 1
else
    echo "All test cases passed!"
    exit 0
fi

