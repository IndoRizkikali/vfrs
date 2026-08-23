#!/bin/bash
echo "Running Named Pipe Loopback Self-Test..."
python tests/loopback_test.py
exit_code=$?
if [ $exit_code -eq 0 ]; then
    echo "SUCCESS: Named Pipe Loopback Self-Test Passed!"
else
    echo "FAILURE: Named Pipe Loopback Self-Test Failed!"
fi
exit $exit_code
