# Android build (arm64-v8a only)

Host tooling is not built for Android; only `runtime/` is. The recompile step runs on the PC
(ahead-of-time) and its generated output is linked locally; nothing derived from game data is committed.

```sh
export ANDROID_NDK=/path/to/ndk
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DPOKE_BUILD_TOOLS=OFF -DPOKE_BUILD_TESTS=OFF
cmake --build build-android
```
The Gradle app shell in `android/` is not implemented yet (issue #15).
