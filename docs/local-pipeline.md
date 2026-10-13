# Local recompilation pipeline (bring your own dump)

Everything below runs on your machine. The generated C is derived from game code: keep it outside the repository
(or in an ignored directory such as `generated/`) and never commit or share it.

```sh
cmake -S . -B build -G Ninja && cmake --build build

# 1. decrypted dump -> code image, then a Ghidra function map (see tools/analysis and analysis/README)
build/tools/romtool/romtool extract game.cci out/ --profile profiles/mine.profile
#    funcmap.tsv comes from tools/analysis/ghidra_scripts/ExportFunctionMap.java (headless Ghidra)
#    classification.json from tools/analysis/classify.py (literal-pool ranges)

# 2. discover blocks and emit C (local only)
build/tools/recomp/recomp_discover out/exefs/code.bin funcmap.tsv classification.json
build/tools/recomp/recomp_translate out/exefs/code.bin funcmap.tsv classification.json /path/to/generated 2000

# 3. link the generated code into the probe and run it
cmake -S . -B build-probe -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-O1 \
      -DPOKE_BUILD_TESTS=OFF -DRECOMP_GENERATED_DIR=/path/to/generated
cmake --build build-probe --target boot_probe
build-probe/tools/recomp/boot_probe out/exefs/code.bin
```
`boot_probe` uses a throwaway fake kernel that is only good enough to watch start-up; the real HLE kernel and
services are issues #9 and #10.
