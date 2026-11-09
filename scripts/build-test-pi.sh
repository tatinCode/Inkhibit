# !bin/bash
cmake -S . -B build \
    -DINKHIBIT_BUILD_HARDWARE=ON \
    -DBUILD_TESTING=ON

cmake --build build --parallel 2
