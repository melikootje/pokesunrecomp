# Bring your own dump

This project never distributes game data and contains no keys or decryption code.

1. Dump the cartridge you own on your own console with community tools and produce a **decrypted**
   `.cci` (or a decrypted `.cxi`). Encrypted input is refused by `romtool` with a clear message.
2. Keep the dump private. Never upload, share or commit it (`.gitignore` blocks the usual extensions).
3. Inspect it: `romtool info game.cci`
4. Record a profile of *your* build so later runs refuse other builds/updates:
   `romtool profile game.cci > profiles/mine.profile` (profiles hold only a title id, sizes and hashes;
   `profiles/*.profile` is safe to keep locally).
5. Extract what the recompiler needs (`exefs/code.bin`, `exheader.bin`, `services.txt`, icon/banner):
   `romtool extract game.cci out/ --profile profiles/mine.profile`
   RomFS is never unpacked eagerly; use `romtool ls` / `romtool cat game.cci /path`.
6. Updates: pass your own decrypted update NCCH with `--update update.cxi`. The update's ExeFS/ExHeader
   replace the base ones and its RomFS overlays the base. This overlay model is based on the public
   documentation and is only verified with synthetic fixtures so far.

Notes
- ExeFS `.code` is compressed with Nintendo's *backward LZ*, not LZ11 (the original issue text said LZ11).
- The Ultra Sun title id/hash profile is intentionally not shipped: generate it from your own dump.
