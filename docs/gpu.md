# GPU: GX engine and PICA200 pipeline

Code: `runtime/src/gpu.cpp` (transfer engine, screens), `pica.cpp` (command lists, vertex stage),
`pica_shader.cpp` (vertex shader interpreter), `pica_raster.cpp` (clipping, rasterizer, fragment pipeline),
`pica_tex.cpp` (texture formats). The renderer is a **software reference implementation**: it exists to establish
correct behaviour against the real game and to give tests something to run on; a Vulkan backend (roadmap #8) will
reuse the decoded state. Register layouts come from the public 3dbrew GPU documentation; each one used is checked
against the real game's command lists where noted.

## Pipeline
`gsp::Gpu` GX commands (`runtime/src/gsp_service.cpp`) → `Gpu`: memory fill, display transfer (8x8/32x32 tiling,
RGBA8/RGB8/RGB565/RGB5A1/RGBA4, crop, flip, 2x1/2x2 downscale), texture copy, DMA; `ProcessCommandList` → `Pica`:
register file + FIFOs (float uniforms in float24 or float32, shader code, operand descriptors, fixed attributes) →
draw: vertex fetch → vertex shader → output map → primitive assembly (list/strip/fan) → near-plane clip →
viewport → cull → rasterize → texture units 0–2 → 6 TEV stages → alpha test → stencil/depth → blend → tiled write.
At VBlank the GSP latches the framebuffer-info structure; `Gpu::capture` reads the scanned-out framebuffer
(columns of 240 pixels, bottom pixel first) as an upright image.

## Verified against the real game (Ultra Sun USA, boot_probe)
- GX commands: 44k memory fills + 22k display transfers execute with **zero rejections**; the game's flags
  (`0x1004`: RGBA8 → RGB8, crop, tiled → linear), 256×512 → 240×400 transfers decode as documented.
- The game renders into a 256×512 RGBA8 target at physical `0x18000000` with a 24+8 depth buffer, using textured
  quads (a 6-vertex draw, position built in the shader from a corner index), texture unit 0 from VRAM, TEV
  "replace" from texture 0 and alpha blending.
- Addresses in command lists are **physical** (`>>3`). The SDK builds the physical address of linear-heap data as
  `virtual + GetProcessInfo(self, 20)`; the kernel returns `0x0C000000` for that type (it returned 0, which made
  every vertex/texture address look virtual). Guest code: conversion at `0x172be4`, call site `0x1055ec`.

## Assumptions not yet confirmed by pictures
- Texture memory row 0 is `v = 0` (checked only on synthetic data).
- Depth: `d = z/w * scale + offset`, clamped to [0, 1]; only the near plane is clipped (`z >= -w`).
- Face culling: 1 keeps clockwise, 2 keeps counter-clockwise (screen space, y up).
- Colour buffer row 0 is the bottom; no vertical flip is applied (flip bit of `FB_DIM` assumed set).
- ETC1 block byte order (the whole 8-byte block reversed relative to the standard layout) and the ETC1A4 alpha
  nibble order are taken from documentation and tested on synthetic blocks only.

## Not implemented (counted in `PicaStats::unsupported*` and printed by `boot_probe`)
Fragment lighting, fog, procedural textures, gas/shadow modes, cube/projection/shadow textures, geometry shaders
and immediate-mode vertices, mipmapping, logic ops other than copy, `LITP`, the 16-bit float24 mantissa in shaders.

## Testing
`gpu_test` (transfer engine), `pica_shader_test` (ISA, hand-assembled programs), `pica_test` (command lists,
uploads, a full draw checked pixel by pixel, texturing, blending, alpha test, scissor, depth, culling, clipping,
texture formats and wrapping). No game data is used; the real-game checks above are local (`boot_probe`).
