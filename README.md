# S3DBasic

Project for **plugin.s3d**, a native plugin that adds basic 3D to [Solar2D](https://solar2d.com): 3D meshes with
depth testing, lighting, shadows, fog, post-processing and OBJ models. It is built only on Solar2D's public native API,
so it works with a stock Solar2D engine, and 3D objects are ordinary display objects that mix with your 2D UI.

- **API reference:** [docs/API.md](docs/API.md) (every function, option and default)

## What the demo shows

- primitives (cube, sphere, cylinder), an OBJ model (torus) and a textured floor,
- a soft shadow map with switchable quality, fog, and post-processing (FXAA, ACES tone mapping, color grading, vignette),
- the soft "diorama" look: hemisphere ambient light, ambient occlusion, soft shadows and a focus area around the camera
  target (colors fade to lavender gray further away), with buttons to compare it with plain lighting,
- frame animation from OBJ sequences (a waving flag and a swimming fish),
- a herd of animals walking around the scene (`animals.lua`, see below),
- a light that orbits the scene, with a small "sun" marking where it is,
- camera controls and touch picking.

## Controls

| Input | Action |
| --- | --- |
| Drag | Orbit the camera |
| D-pad (bottom left), arrow keys, WASD | Move the camera target over the floor |
| Mouse wheel, `+` / `−` buttons, `+` / `-` keys | Zoom |
| Tap the floor | Move the red ball there |
| Top buttons | Toggle FXAA, fog, shadows, color grading, Style, AO, sun orbit, animation |
| Shadow button (blue) | Cycle shadow quality: low (1 sample), medium (2x2), high (3x3), ultra (4x4) |

**Style** switches between the soft look (hemisphere light, AO, soft shadows, tone mapping, focus area) and the plain
lighting of earlier versions. **AO** turns ambient occlusion on or off in either style; the AO baked when the torus
loads stays cached, so switching is instant.

## Files

| File | Contents |
| --- | --- |
| `main.lua` | Scene setup, look settings (`STYLE` / `PLAIN` tables), UI, camera, main loop. |
| `animals.lua` | Walking animals: builds walk / hop / waddle / fly cycles by rotating each OBJ group (legs, tail, wings) around its joint, writes the frames to `system.TemporaryDirectory` and plays them with `s3d.loadOBJAnimation`. |
| `docs/API.md` | plugin.s3d API reference. |
| `assets/anim/*.obj.txt` | Flag (30 frames) and fish (24 frames) animations. |
| `assets/anim/animals/` | [Kenney Animal Kit](https://kenney.nl) (CC0). The `.obj.txt` copies are what the demo loads, because Android builds drop `*.obj` files. |
| `config.lua`, `build.settings` | Landscape, 1280 x 720 content area. |
