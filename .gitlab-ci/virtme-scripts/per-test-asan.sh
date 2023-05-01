#!/bin/bash

export LSAN_OPTIONS="suppressions=../.gitlab-ci/leak-sanitizer.supp"
export ASAN_OPTIONS="detect_leaks=1"

exec "$@"
