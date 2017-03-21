#!/bin/bash
ENVOY_BUILD_SHA=d3a3d93f304fe1cf28023b293913649e2024f297

# Script that lists all the steps take by the CI system when doing Envoy builds.
set -e

# Lint travis file.
travis lint .travis.yml --skip-completion-check

# Do a build matrix with different types of builds docs, coverage, normal, etc.
if [ $TEST_TYPE == "docs" ]
then
  echo "docs build..."
  make docs
  ./docs/publish.sh
  exit 0
else
  docker run -t -i -v $TRAVIS_BUILD_DIR:/source lyft/envoy-build:$ENVOY_BUILD_SHA /bin/bash -c "cd /source && ci/do_ci.sh $TEST_TYPE"
fi

# The following scripts are only relevant on a `normal` run.
# This script build a lyft/envoy image an that image is pushed on merge to master.
./ci/docker_push.sh
# This script runs on every PRs normal run to test the docker examples.
./ci/verify_examples.sh
