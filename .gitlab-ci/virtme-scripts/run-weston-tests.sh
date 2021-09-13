#!/bin/bash

# folders that are necessary to run Weston tests
mkdir -p /tmp/tests
mkdir -p /tmp/.X11-unix
chmod -R 0700 /tmp

# set environment variables to run Weston tests
export XDG_RUNTIME_DIR=/tmp/tests
export WESTON_TEST_SUITE_DRM_DEVICE=card0
export LIBSEAT_BACKEND=seatd

# ninja test depends on meson, and meson itself looks for its modules on folder
# $HOME/.local/lib/pythonX.Y/site-packages (the Python version may differ).
# virtme starts with HOME=/tmp/roothome, but as we installed meson on user root,
# meson can not find its modules. So we change the HOME env var to fix that.
export HOME=/root
export PATH=$HOME/.local/bin:$PATH

# start seatd and wait for its socket to be available before running the test
/usr/local/bin/seatd &
while ! [ -e /run/seatd.sock ]; do sleep 0.1; done

export ASAN_OPTIONS=detect_leaks=0,atexit=1

# run the tests and save the exit status
# we give ourselves a very generous timeout multiplier due to ASan overhead
meson test --no-rebuild --timeout-multiplier 4
TEST_RES=$?

# create a file to keep the result of this script:
#   - 0 means the script succeeded
#   - 1 means the tests failed, so the job itself should fail
TESTS_RES_PATH=$(pwd)/tests-res.txt
echo $TEST_RES > $TESTS_RES_PATH

# shutdown virtme
sync
poweroff -f
