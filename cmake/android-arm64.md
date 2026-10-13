Use the NDK's own toolchain file (no custom toolchain needed):

    cmake -S . -B build-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
      -DPOKE_BUILD_TOOLS=OFF -DPOKE_BUILD_TESTS=OFF
