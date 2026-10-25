# pokesunrecomp

Static recompilation tooling and HLE runtime for running a user-owned copy of **Pokémon Ultra Sun**
(3DS, USA) as a native **Android arm64** application. See the roadmap in issue #1.

**No game data is included or ever will be.** You supply your own decrypted dump. Read
[docs/legal-policy.md](docs/legal-policy.md) first.

## Status
The recompiled game boots, loads its static modules and runs the language-select scene; the 3D pipeline is a
software reference renderer under development. Measured status and evidence: [docs/boot-status.md](docs/boot-status.md),
[docs/ldr-ro.md](docs/ldr-ro.md), [docs/gpu.md](docs/gpu.md).

## Scope
- Host tooling (`tools/`): ROM container parsing/extraction, analysis, recompiler.
- Runtime (`runtime/`): HLE kernel/services, GPU, audio.
- Android app (`android/`): arm64-v8a shell, dual-screen (AYN Thor) support.

## Non-goals
Piracy, distributing game data or keys, online play against Nintendo services.

## Build (host: macOS/Linux)
```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Android: see [docs/android-build.md](docs/android-build.md).

## Contributing
Original code only, tests with synthetic fixtures, `-Wall -Wextra -Werror` clean, no game material.
See [CONTRIBUTING.md](CONTRIBUTING.md).

License: MIT (original code). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
