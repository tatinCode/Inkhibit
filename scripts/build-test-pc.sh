#!/usr/bin/env bash
cmake -S . -B build-tests \
    -DINKHIBIT_BUILD_HARDWARE=OFF \
    -DBUILD_TESTING=ON

cmake --build build-tests --parallel
