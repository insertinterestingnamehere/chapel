#!/usr/bin/env bash
#
# Run -perflabel cc- performance tests on a chapcs machine with gasnet

UTIL_CRON_DIR=$(cd $(dirname ${BASH_SOURCE[0]}) ; pwd)

export CHPL_TEST_PERF_CONFIG_NAME='chapcs.comm-counts'

source $UTIL_CRON_DIR/common-perf.bash
source $UTIL_CRON_DIR/common-gasnet.bash

export CHPL_NIGHTLY_TEST_CONFIG_NAME="perf.comm-counts"

START_DATE=08/17/17

perf_args="-performance -numtrials 1 -startdate $START_DATE -perflabel cc-"

# "make check" will fail on chapcs because Gasnet wants to use ibv.
#  The warning message causes the good-output v. test-output comparison to fail.

#  -no-buildcheck skips the "make check"
nightly_args="${nightly_args} -no-buildcheck"

$UTIL_CRON_DIR/nightly -cron ${nightly_args} ${perf_args}
