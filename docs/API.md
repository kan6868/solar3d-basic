# plugin.s3d API reference

Version 0.12.0.

```lua
local s3d = require( "plugin.s3d" )
```

- [Conventions](#conventions)
- [Module](#module): `version`, `backend`, `supported`, `isSupported`, `isMobile`, `init`
- [Scenes](#scenes): `newScene`, `newPostProcess`
- [Creating objects](#creating-objects): `newCube`, `newSphere`, `newCylinder`, `newCapsule`, `newPlane`, `loadOBJ`, `newSky`
- [Object methods](#object-methods): transform, render state, shadows, effects, reflections
- [Frame animation](#frame-animation): `loadOBJAnimation` and its methods
- [Dynamic meshes](#dynamic-meshes): `newMesh`, `newGrid` and their methods
- [Instances](#instances): `newInstances`, many copies of a model in one draw call
- [Sprites](#sprites): `newSprite`, camera-facing images and image-sheet animation
- [Camera](#camera): `setCamera`, `getCamera`, `orbit`
- [Light](#light): `setLight`
- [Fog](#fog): `setFog`
- [Shadows](#shadows): `setShadowMap`, `setShadow`, `getShadowMode`
- [Picking and projection](#picking-and-projection): `worldToScreen`, `screenToRay`, `rayPlaneY`
- [Effects](#effects): built-in effects, writing your own

## Conventions

- **Right-handed, Y up.** Units are arbitrary; the demos use meters.
- **Angles are in degrees.**
- **Euler rotation order:** `rotateTo( rx, ry, rz )` builds `R = Ry(yaw) * Rx(pitch) * Rz(roll)`; the model matrix is
  `T * R * S`.
- **Vectors** are passed as `{ x, y, z }` (`{ x = .., y = .., z = .. }` also works for options).
- **Colors** are `{ r, g, b [, a] }` in 0..1.
- Methods that change an object return the object, so calls can be chained: `obj:moveTo( 0, 1, 0 ):rotateTo( 0, 45, 0 )`.
- 3D objects are ordinary Solar2D display objects (meshes). **Do not set `x`, `y`, `rotation`, `xScale`, `yScale`
  on them or on the groups that contain them**; use the 3D methods instead. `setFillColor`, `alpha`, `isVisible`,
  `removeSelf`, `toFront` and event listeners work as usual.
- The first call to any constructor runs `s3d.init()` automatically.

## Module

| Field / function | Description |
| --- | --- |
| `s3d.version` | Plugin version string, e.g. `"0.6.0"`. |
| `s3d.backend` | Solar2D graphics backend (`system.getInfo( "graphicsBackend" )`), e.g. `"glBackend"`. |
| `s3d.supported` | `false` on the Vulkan backend. 3D objects are then created hidden and a warning is printed. |
| `s3d.isSupported()` | Returns `s3d.supported`. |
| `s3d.isMobile` | `true` on Android, iOS and tvOS. Handy for picking shadow quality. |
| `s3d.instancing` | `true` when GPU instancing is available (set by `init`; currently Windows only, `false` on Android). See [Instances](#instances). |
| `s3d.init()` | Registers the effects and render hooks. Called automatically; calling it again does nothing. Enables `display.setDefault( "enableDepth", true )`. |
| `s3d.EFFECT_LIT`, `EFFECT_UNLIT`, `EFFECT_REFLECT`, `EFFECT_SKY`, `EFFECT_SPRITE`, `EFFECT_SPRITE_LIT`, `EFFECT_DEPTH_SPRITE`, `EFFECT_LIT_INST`, `EFFECT_UNLIT_INST`, `EFFECT_DEPTH_INST`, `EFFECT_NORMALS`, `EFFECT_SHADOW`, `EFFECT_DEPTH`, `EFFECT_POST` | Effect names (`"filter.s3d.lit"`, ...). See [Effects](#effects). |

## Scenes

### `s3d.newScene( [parent] )`

Returns a display group for 3D objects, inserted into `parent` when given. It is a plain group at (0, 0); leave its
position, rotation and scale alone. Shadow helper layers are added to it automatically.

### `s3d.newPostProcess( [opts] )`

Creates a full-screen snapshot that the scene is drawn into, filtered by `filter.s3d.post`. Draw your 3D objects in
`post.scene`; keep 2D UI outside it so it is not post-processed. The snapshot is invalidated every frame.

| Option | Default | Description |
| --- | --- | --- |
| `clearColor` | `{ 0, 0, 0, 1 }` | Background (sky) color. |
| `fxaa` | `true` | Fast approximate anti-aliasing. |
| `exposure` | `1` | Brightness multiplier. |
| `contrast` | `1` | Contrast around mid-gray. |
| `saturation` | `1` | 0 = grayscale. |
| `gamma` | `1` | Gamma correction. |
| `brightness` | `0` | Added to the color. |
| `tint` | `{ 1, 1, 1 }` | Color multiplier. |
| `vignette` | `0` | Corner darkening, 0..1. |
| `vignetteSoftness` | `0.5` | Width of the vignette falloff. |
| `toneMap` | `"none"` | `"aces"`: filmic curve after `exposure`, so bright areas roll off softly instead of clipping. |
| `width`, `height` | content size | Snapshot size. |
| `x`, `y` | content center | Snapshot position. |
| `parent` | stage | Group to insert the snapshot into. |

Returns a table:

| Field / method | Description |
| --- | --- |
| `post.scene` | Group to put 3D objects in (like `s3d.newScene()`). |
| `post.snapshot` | The `display.newSnapshot` object. |
| `post.settings` | Current settings. |
| `post:set( opts )` | Change any of the color options above at run time. |
| `post:setEnabled( on )` | Turn the filter off (the scene is still drawn) or back on. |
| `post:removeSelf()` | Remove the snapshot and its frame listener. |

```lua
local post = s3d.newPostProcess{ clearColor = { 0.62, 0.76, 0.9 }, exposure = 1.05, vignette = 0.3 }
post:set{ saturation = 0.5 }
```

## Creating objects

All constructors take a `parent` group and an optional `opts` table and return a 3D object.

| Function | Shape |
| --- | --- |
| `s3d.newCube( parent, size, [opts] )` | Cube with side `size` (default 1), centered at the origin. |
| `s3d.newSphere( parent, radius, segments, rings, [opts] )` | UV sphere. Defaults: radius 0.5, 24 segments, 16 rings. |
| `s3d.newCylinder( parent, radius, height, segments, [opts] )` | Capped cylinder along Y, centered. Defaults: 0.5, 1, 24. |
| `s3d.newCapsule( parent, radius, height, segments, rings, [opts] )` | Capsule along Y. `height` is the distance between the hemisphere centers (total height = `height + 2 * radius`). Defaults: 0.25, 1, 20, 6. |
| `s3d.newPlane( parent, width, depth, [opts] )` | Plane in XZ facing +Y, centered. `opts.uvRepeat` repeats the texture (set `display.setDefault( "textureWrapX"/"textureWrapY", "repeat" )` before loading it). |
| `s3d.loadOBJ( parent, filename, [opts] )` | Wavefront OBJ model. |
| `s3d.newSky( parent, texture, [opts] )` | Sky dome, see [Sky](#sky). |

### Common options

| Option | Description |
| --- | --- |
| `texture` | Image file for the object's fill. For plain colored objects the demo uses a small white image plus `color`. |
| `baseDir` | Base directory for `texture` (and for the OBJ file in `loadOBJ`). Default `system.ResourceDirectory`. |
| `color` | `{ r, g, b [, a] }` passed to `setFillColor`. |
| `unlit` | `true` uses `filter.s3d.unlit` (no lighting). |
| `effect` | Any effect name, e.g. `"filter.s3d.normals"` or your own. |
| `cull` | `false` draws both sides (for open meshes like flags). |
| `castShadow` | `true` casts a shadow (shadow map or planar, see [Shadows](#shadows)). |
| `reflection` | Table for [`setReflection`](#reflections), e.g. `{ texture = "sky.png" }`. Needs `texture`. |
| `x`, `y`, `z` | Initial position. |

### Sky

`s3d.newSky( parent, texture, [opts] )` puts an equirectangular (2:1) image on a sphere seen from inside. The dome is
unlit and has no distance fog (with fog on it fades into the fog color towards the horizon, see `setFog`'s
`skyHorizon`), is moved to the front of `parent`'s drawing order (index 1, drawn first), does not write depth (so
every object covers it), and follows the camera, so it looks infinitely far away. It uses the same mapping as
[`setReflection`](#reflections): reflections of the same image line up with the sky.

| Option | Default | Description |
| --- | --- | --- |
| `radius` | 90% of the camera's far plane | Must stay inside the far plane. |
| `segments`, `rings` | `48`, `24` | Sphere resolution. |
| `baseDir` | `system.ResourceDirectory` | Base directory for `texture`. |
| `color` | | Tint, `{ r, g, b }`. |

Load the image with `display.setDefault( "textureWrapX", "repeat" )` so it wraps without a seam; reflections then reuse
the same texture. Set the fog color to the image's horizon color so fogged objects blend into the sky; below the
horizon the image is seen past the edge of your ground. Remove the sky with `display.remove( sky )`.

### OBJ files

`loadOBJ` reads `v`, `vt`, `vn` and `f` lines. Polygons are split into triangle fans; faces without `vn` get flat
normals; materials (`mtllib`, `usemtl`) and groups are ignored. Extra option: `scale` multiplies every position.

Solar2D for Android always drops `*.obj` files from the APK. Name your models `model.obj.txt`; when a name ends in
`.obj`, `loadOBJ` also tries the same name with `.txt` appended, so `s3d.loadOBJ( scene, "model.obj" )` works on every
platform.

### glTF files

```lua
local golem = s3d.loadGLTF( scene, "assets/glbs/golem.glb", { height = 2, castShadow = true, x = 3 } )
golem:moveTo( 0, 0, -4 ):rotateTo( 0, 45, 0 )
print( golem.height3d, #golem.parts, table.concat( golem.animations, ", " ) )
```

`s3d.loadGLTF( parent, filename, [opts] )` loads a glTF 2.0 model through [cgltf](https://github.com/jkuhlmann/cgltf).
Use `.glb` files, or `.gltf` files whose buffers are embedded (`data:` URIs); separate `.bin` files are not read.
Images may be embedded (GLB, `data:` URIs) or **external files** (`"uri": "Textures/colormap.png"`): an external image
is loaded relative to the glTF file, from the same `baseDir` (e.g. Kenney kits exported from Unity share one
`Textures/colormap.png`). `KHR_texture_transform` is ignored (only its default, identity, is supported).

What is loaded:

- every node with a mesh, in its **rest pose** (the node hierarchy's transforms are baked into the vertices);
- triangle primitives with `POSITION`, `NORMAL` (flat normals when missing) and `TEXCOORD_0`;
- one Solar2D mesh per material (`model.parts`), opaque parts first;
- the base colour texture (embedded PNG / JPEG, written once to `system.TemporaryDirectory`) and `baseColorFactor`;
- `alphaMode`: `MASK` uses an alpha cut-out (`filter.s3d.spritelit`, threshold 0.5) and cut-out shadows, `BLEND`
  parts are drawn without depth writes, with back faces culled (even when double-sided) and without shadows, in a
  group kept after the parent's other s3d objects; `doubleSided` turns culling off for the other modes;
- a `NEAREST` sampler (Blockbench pixel art) gives nearest texture filtering.

- node animations (translation, rotation, scale; linear, step and cubic-spline keys): see below.

Not yet: skins, morph targets, normal / metallic-roughness / emissive maps, cameras and lights. Sparse accessors are read; KHR extensions are ignored.

Options: `scale` (or `height`: scale so the model is this tall), `center` (default `true`: the bounding box's bottom
centre goes to the origin; `false` keeps the file's origin), `castShadow`, `unlit`, `effect`, `cull`, `ao`,
`aoDistance`, `baseDir`, `x`, `y`, `z`. A file is parsed once and cached; later loads only copy the arrays.

The returned model is a table (not a display object): `moveTo`, `moveBy`, `rotateTo`, `rotateBy`, `setPose`,
`setRotationQuat`, `scaleTo`, `castShadow`, `setCulling`, `setDepthTest`, `setReflection`, `setFillColor`,
`setVisible( on )` and `removeSelf()` apply to every part. Fields: `parts`, `animations`, `filename`, `width3d`,
`height3d`, `depth3d` (size after scaling).

#### Animation

```lua
local yeti = s3d.loadGLTF( scene, "assets/glbs/yeti.glb", { height = 2, animation = "idle" } )
yeti:play( "walk", { fade = 0.25 } )                                     -- cross-fade from the current clip
yeti:play( "attack_1", { loop = false, onComplete = function( model, name ) model:play( "idle" ) end } )
```

Blockbench-style animations move whole nodes (rigid parts). Every frame the plugin samples the clip's channels, builds
the nodes' world matrices and transforms each part's vertices in C (about 0.02–0.08 ms per model on a desktop for
2 000–9 000 vertices); the parts stay one draw call per material. Shadow casters follow the animation.

- `model:play( name | index, [opts] )`: `loop` (default `true`), `speed` (default 1), `fade` (seconds, default 0.2:
  cross-fade from the clip that is playing; 0 = switch at once), `time` (start time), `onComplete( model, name )`
  (a non-looping clip reached its end; the last frame stays).
- `model:pause()`, `model:resume()`, `model:isPlaying()`, `model:setSpeed( s )` (multiplies every clip's speed),
  `model:setTime( seconds )` (also while paused), `model:stop()` (back to the rest pose),
  `model:getDuration( [name] )`.
- Fields: `model.animation` (current clip name), `model.animationTime` (seconds).
- Load options: `animation` (play this clip at once), `loop`, `speed`; `static = true` skips the animation data (less
  memory, no `play`).

Each loaded copy has its own pose; the file is parsed once. Invisible models (`setVisible( false )`) keep their clock
running but are not posed. Ambient occlusion (when enabled) is baked for the rest pose.

## Object methods

### Transform

| Method | Description |
| --- | --- |
| `obj:moveTo( x, y, z )` | Set the position. |
| `obj:moveBy( dx, dy, dz )` | Move by an offset. |
| `obj:rotateTo( rx, ry, rz )` | Set the Euler rotation in degrees (pitch X, yaw Y, roll Z; see Conventions). |
| `obj:rotateBy( dx, dy, dz )` | Add to the Euler rotation. |
| `obj:scaleTo( sx, [sy, sz] )` | Set the scale; `sy` and `sz` default to `sx`. |
| `obj:setPose( x, y, z, [qx, qy, qz, qw] )` | Set position and quaternion rotation in one call. Made for physics: `obj:setPose( body:getTransform() )`. |
| `obj:setRotationQuat( qx, qy, qz, qw )` | Set only the quaternion rotation. |

After `setPose` / `setRotationQuat`, the object uses the quaternion until the next `rotateTo` / `rotateBy`.

Readable fields: `obj.position3d`, `obj.rotation3d`, `obj.scale3d` (tables `{ x, y, z }`, do not modify them directly),
`obj.quat3d` (or `nil`), `obj.vertexCount3d`.

### Render state

| Method | Description |
| --- | --- |
| `obj:setCulling( on )` | Back-face culling (default on). |
| `obj:setDepthTest( on )` | Depth testing (default on). |
| `obj:setDepthWrite( on )` | Depth writing (default on). Turn off for transparent objects. |
| `obj:setEffect( name, [uses] )` | Switch the effect. See [Custom effects](#custom-effects) for `uses`. |
| `obj:castShadow( on )` | Start or stop casting a shadow. The object must already be in a group. |
| `obj:setReflection( r )` | Mirror an environment image on the surface; `false` turns it off. See [Reflections](#reflections). |

### Reflections

`obj:setReflection{ texture = "sky.png", strength = 0.5, fresnel = 0.04, blur = 0, rotation = 0 }` mirrors an
**equirectangular** (360° x 180°, usually 2:1) environment image on the object, like chrome, glass, polished paint or
water reflecting the sky. Grazing angles reflect more than surfaces seen head-on (Schlick's Fresnel term).

| Field | Default | Description |
| --- | --- | --- |
| `texture` | required the first time | Equirectangular image. Later calls can leave it out to change only the other fields. |
| `baseDir` | `system.ResourceDirectory` | Base directory for `texture`. |
| `strength` | `0.5` | Overall amount, 0..1. |
| `fresnel` | `0.04` | Share reflected when seen head-on (F0): about 0.02 water, 0.04 glass and plastic, 0.5-0.9 metals. |
| `blur` | `0` | Mipmap bias for rough surfaces. Needs the image loaded with mipmaps (otherwise no effect); a thin seam can show where the image wraps around. |
| `rotation` | `0` | Turns the environment around the Y axis, in degrees. |

- The image is mapped like `s3d.newSphere`'s UVs: the center column faces +Z, the top row is straight up. A sky dome
  made with `newSphere` (`unlit = true, cull = false`) and the same image therefore lines up with the reflections.
- The object needs `opts.texture` (the demos use a small white image plus `color`): its paint becomes composite,
  `paint1` = its texture and `paint2` = the environment. `setFillColor` tints the surface but not the reflection.
- Effect: `filter.s3d.reflect` (lit + reflection). Fog, shadows, ambient occlusion and the focus area still apply.
- Only the environment image is reflected, not the other objects in the scene.
- Cost: one extra texture read and a few math operations per pixel.

## Frame animation

### `s3d.loadOBJAnimation( parent, source, [opts] )`

Loads a sequence of OBJ files as one animated object. Every frame must have the same topology (the same faces in the
same order), which is what Blender produces with **File > Export > Wavefront (.obj)** and **Animation** checked. Positions
and normals of two neighbouring frames are interpolated in C, so playback costs a handful of Lua operations per frame.

`source` is either a `string.format` pattern such as `"anim/run_%04d.obj"` (with `opts.first` and `opts.last`) or a list
of file names.

| Option | Default | Description |
| --- | --- | --- |
| `first`, `last` | `1`, required | Frame numbers substituted into the pattern. |
| `frames` | | List of file names, instead of a pattern. |
| `fps` | `24` | Playback rate. |
| `loop` | `true` | Loop the whole sequence (clips can override). |
| `interpolate` | `true` | Blend between frames; `false` steps from frame to frame. |
| `autoPlay` | `true` | Start playing right away. |
| `clip` | | Clip to start with. |
| `clips` | `{}` | Named ranges: `{ run = { 1, 16 }, jump = { 17, 24, loop = false, fps = 30 } }`. Indices count loaded frames from 1. |
| `onComplete` | | `function( obj, clipName )`, called when a non-looping clip ends. |
| `scale` | `1` | Multiplies every position. |

All [common options](#common-options) apply too (`texture`, `castShadow`, ...).

| Method / field | Description |
| --- | --- |
| `obj:play( [clip] )` | Play a clip, or the whole sequence when `clip` is `nil`, from its start. |
| `obj:pause()`, `obj:resume()` | Pause / resume. |
| `obj:stop()` | Stop and rewind to the first frame of the clip. |
| `obj:gotoFrame( frame )` | Jump to a frame of the current clip, counted from 1; fractions blend (`2.5`). |
| `obj:setSpeed( s )` | Playback speed multiplier (default 1). |
| `obj:setFPS( fps )` | Change the frame rate. |
| `obj:setInterpolation( on )` | Turn blending on or off. |
| `obj:isPlaying()` | `true` while playing. |
| `obj.frameCount` | Number of loaded frames. |
| `obj.currentFrame` | Current (fractional) frame index. |
| `obj.clip` | Current clip name or `nil`. |

When a non-looping clip ends, the object also dispatches `{ name = "animation", phase = "ended", clip = clipName }`.

```lua
local hero = s3d.loadOBJAnimation( scene, "assets/hero/hero_%04d.obj", {
    first = 1, last = 40, fps = 24, texture = "assets/hero.png", castShadow = true,
    clips = { idle = { 1, 20 }, wave = { 21, 40, loop = false } },
    clip = "idle",
} )
hero:addEventListener( "animation", function( e ) if e.clip == "wave" then hero:play( "idle" ) end end )
hero:play( "wave" )
```

Memory: each frame keeps one position and one normal buffer (24 bytes per vertex), so a 3,000-vertex model with 60
frames uses about 4 MB.

## Dynamic meshes

Meshes whose vertices you change while the app runs: water, terrain sculpting, cloth, soft bodies, procedural shapes.
The plugin keeps the vertex list in C. After an edit it rebuilds the triangle list and the normals in C and hands them to
the geometry writers, so Lua only touches the values that change and shading and shadows follow the new shape.
A 65 x 65 height field (4,225 vertices, 8,192 triangles) takes about 0.15 ms per update on a desktop CPU, on top of
whatever your own Lua code does to compute the heights.

### `s3d.newMesh( parent, opts )`

| Option | Default | Description |
| --- | --- | --- |
| `vertices` | required | `{ x, y, z, x, y, z, ... }` |
| `uvs` | all `0` | `{ u, v, ... }`, one pair per vertex. |
| `indices` | | `{ i1, i2, i3, ... }`, 1-based, three per triangle, front face counter-clockwise. Without indices every three vertices form one triangle. |
| `normals` | `"smooth"` | `"smooth"`: averaged over the triangles that share a vertex index (weighted by area). `"flat"`: one normal per triangle. |

All [common options](#common-options) apply too, except baked ambient occlusion (the shape changes). The shader's
darkening of walls near the floor still works.

```lua
-- a quad that bends
local quad = s3d.newMesh( scene, {
    vertices = { -1, 0, -1,  1, 0, -1,  1, 0, 1,  -1, 0, 1 },
    uvs = { 0, 0,  1, 0,  1, 1,  0, 1 },
    indices = { 1, 4, 3,  1, 3, 2 },
    texture = "assets/crate.png", cull = false,
} )
Runtime:addEventListener( "enterFrame", function( e )
    quad:setVertex( 3, 1, 0.5 * math.sin( e.time / 300 ), 1 )
end )
```

### `s3d.newGrid( parent, width, depth, [cols], [rows], [opts] )`

A height field on the XZ plane, centered on the object's origin: `cols x rows` cells (default 32 x 32),
`(cols + 1) x (rows + 1)` vertices. Column `i` goes from 0 to `cols` along +X, row `j` from 0 to `rows` along +Z.
Vertices are numbered row by row, X fastest: vertex `j * ( cols + 1 ) + i + 1`.

| Option | Default | Description |
| --- | --- | --- |
| `heights` | all `0` | A table in vertex order, or `function( x, z, i, j ) return height end`. |
| `uvRepeat` | `1` | How many times the texture repeats across the grid (as `newPlane`). |

Plus every `newMesh` option except `vertices`, `uvs` and `indices`.

### Methods

| Method / field | Description |
| --- | --- |
| `obj:setVertices( list, [first] )` | Copy `{ x, y, z, ... }` into the vertices starting at vertex `first` (default 1). Applied immediately. |
| `obj:setVertex( i, x, y, z )` | Move one vertex (1-based). `nil` keeps a coordinate. Applied by `updateMesh()` or at the start of the next frame. |
| `obj:getVertex( i )` | Returns `x, y, z` of a vertex, in the object's own coordinates. |
| `obj:updateMesh()` | Apply pending `setVertex` / `setHeight` edits now. |
| `obj:setNormals( mode )` | Switch between `"smooth"` and `"flat"` normals. |
| `obj.meshVertexCount` | Number of vertices you edit (`vertexCount3d` counts the drawn, unindexed vertices). |
| `grid:setHeights( heights, [scale], [offset] )` | Set the height of every vertex from a table in vertex order (`offset + scale * h`). Applied immediately. The fastest way to animate a grid: one call per frame. |
| `grid:setHeight( i, j, h )` | One vertex, by column and row (0-based). Applied like `setVertex`. |
| `grid:getHeight( i, j )` | Height of a vertex. |
| `grid:heightAt( x, z )` | Height at a point in the grid's own coordinates, interpolated between the 4 nearest vertices; `nil` outside. |
| `grid:cellAt( x, z )` | Column and row of the vertex nearest to a point in the grid's own coordinates; `nil` outside. |
| `grid.cols`, `grid.rows`, `grid.gridWidth`, `grid.gridDepth` | Size of the grid. |

Edits with `setVertex` / `setHeight` made in an `enterFrame` listener show up one frame later unless you call
`updateMesh()` after them. `setVertices` and `setHeights` apply right away.

```lua
-- ripples: a two-buffer wave simulation, written to the grid once per frame
local N = 49
local pond = s3d.newGrid( scene, 4, 4, N - 1, N - 1, { texture = "assets/white.png", color = { 0.3, 0.58, 0.78 } } )
local cur, prev = {}, {}
for k = 1, N * N do cur[k], prev[k] = 0, 0 end
Runtime:addEventListener( "enterFrame", function()
    for j = 1, N - 2 do
        for i = 1, N - 2 do
            local k = j * N + i + 1
            prev[k] = ( ( cur[k - 1] + cur[k + 1] + cur[k - N] + cur[k + N] ) * 0.5 - prev[k] ) * 0.985
        end
    end
    cur, prev = prev, cur
    pond:setHeights( cur )
end )
-- drop a stone: cur[ j * N + i + 1 ] = -0.05
```

Memory: about 24 bytes per vertex and 84 bytes per triangle in C, plus the display object and its Lua vertex tables.

## Sprites

### `s3d.newSprite( parent, filename, [opts] )`

An image in the 3D scene, on a quad that faces the camera (a billboard): trees, grass, characters and items in
2.5D style, smoke and fire, markers. Pixels below 50% alpha are cut out, so sprites write depth and need no sorting.
The quad is turned towards the camera in C every frame; it is still an ordinary s3d object (`moveTo`, `scaleTo`,
`setFillColor`, `isVisible`, `castShadow`...).

| Option | Default | Description |
| --- | --- | --- |
| `width`, `height` | `1` high, width from the frame's aspect | Size in world units. |
| `anchorX`, `anchorY` | `0.5`, `1` | Point of the image placed at the sprite's position (Solar2D style: 0 = left / top). The default puts the bottom on the ground. `0.5` for things floating in the air. |
| `billboard` | `"upright"` | `"upright"`: turns around Y only, stays vertical (trees, characters). `"full"`: always faces the camera (particles, coins, markers). `false`: a fixed quad facing +Z, turned with `rotateTo` (signs, decals on walls). |
| `sheet` | single image | Image sheet: a grid `{ columns = 8, rows = 4, [count], [aspect] }` (frames numbered row by row; `aspect` = frame width / height in pixels, default 1), or the options of `graphics.newImageSheet`: `{ frames = { { x, y, width, height }, ... }, sheetContentWidth, sheetContentHeight }` (TexturePacker data works as is). |
| `sequences` | all frames | Solar2D style: `{ name = "walk", start = 1, count = 8, time = 800, loopCount = 0, loopDirection = "forward" }` or `{ name = "idle", frames = { 9, 10, 11 }, time = 600 }`. One table or a list. `loopCount` 0 = forever; `loopDirection` `"bounce"` plays forward then backward. Without `time`: 15 frames per second. |
| `play` | `true` when `sequences` is given | Start the first sequence right away. |
| `lit` | `false` | `true`: lit like `filter.s3d.lit` (sun, ambient, shadows on the sprite), the normal facing the camera. |
| `castShadow` | `false` | Shadow with the image's outline, in shadow map mode only. The shadow is cast by the quad as it faces the camera. |
| `color`, `baseDir`, `x`, `y`, `z` | | As for other objects. |

```lua
local tree = s3d.newSprite( scene, "tree.png", { height = 3, x = 4, z = -2, castShadow = true } )

local hero = s3d.newSprite( scene, "hero.png", { height = 1.6, sheet = { columns = 8, rows = 2 },
    sequences = {
        { name = "walk", start = 1, count = 8, time = 800 },
        { name = "jump", start = 9, count = 6, time = 500, loopCount = 1 },
    } } )
hero:moveTo( 0, 0, 3 )
hero:setFlip( true )            -- walking to the left
hero:setSequence( "jump" ); hero:play()
hero:addEventListener( "sprite", function( e ) if e.phase == "ended" then hero:setSequence( "walk" ); hero:play() end end )
```

| Method / field | Description |
| --- | --- |
| `spr:setSequence( name )` | Pick a sequence; shows its first frame (call `play()` to start it). |
| `spr:play()`, `spr:pause()` | Start / stop the animation. |
| `spr:setFrame( i )` | Frame `i` of the current sequence. |
| `spr:setSheetFrame( i )` | Frame `i` of the image sheet, outside any sequence (pauses). |
| `spr:setFlip( x, y )` | Mirror the image horizontally / vertically. |
| `spr.sequence`, `spr.frame`, `spr.numFrames`, `spr.sheetFrame`, `spr.isPlaying` | Current state. |
| `spr.timeScale` | Playback speed multiplier (default 1). |
| `spr.spriteWidth`, `spr.spriteHeight` | Size in world units. |
| `"sprite"` event | `phase` = `"began"`, `"next"`, `"loop"`, `"ended"`, with `target`, `sequence`, `frame`, like Solar2D sprites. |

Each sprite is one draw call; animation updates 6 UVs when the frame changes. For hundreds of identical static sprites
(a forest), a model drawn with `newInstances` is cheaper. Planar shadows are not available for sprites.

## Instances

### `s3d.newInstances( parent, model, count, [opts] )`

> **Platform support: GPU instancing currently works on Windows (win32 Simulator / desktop) only.** On Android,
> Solar2D reports no instancing (`system.getInfo( "instancingSupport" )` is `false`), so `newInstances` falls back to
> ordinary objects there (see **Fallback** below). Solar2D creates an OpenGL ES 2.0 context and only looks for the
> ES 2 extensions (`GL_EXT_instanced_arrays`, `GL_ANGLE_instanced_arrays`, `GL_EXT_draw_instanced`), which current
> ES 3.x drivers usually do not list, because instancing is core in ES 3.0 (`GLGeometry::SupportsInstancing`,
> `librtt/Renderer/Rtt_GLGeometry.cpp`). Supporting Android needs an engine change, not a plugin change.

Draws `count` copies of a model with **GPU instancing**: one mesh, one draw call, and each copy's position, rotation,
scale and color applied in the vertex shader. For forests, rocks, fences, crowds, particles: 10,000 cubes cost one
draw call instead of 10,000, and the CPU transforms the model's vertices once instead of once per copy.

- `model`: any s3d object (`newCube`, `loadOBJ`, ...). Its geometry, texture, fill color and baked ambient occlusion
  are used; the model itself is left as it is (make it outside the scene, or hide it).
- `opts.castShadow`: copies cast shadows in shadow map mode (`setShadowMap`). Planar shadows are not available.
- `opts.cull`, `opts.unlit`: as for other objects.
- Copies start hidden (scale 0) and white; place them with `setInstance`.
- The object itself stays at the origin: positions are world coordinates. Do not `moveTo` / `rotateTo` it.

```lua
local tree = s3d.loadOBJ( nil, "tree.obj.txt", { texture = "colormap.png" } )
tree.isVisible = false
local forest = s3d.newInstances( scene, tree, 2000, { castShadow = true } )
for i = 1, 2000 do
    forest:setInstance( i, math.random( -50, 50 ), 0, math.random( -50, 50 ), 0, math.random( 360 ), 0, 0.8 + math.random() * 0.4 )
end
```

| Method / field | Description |
| --- | --- |
| `inst:setInstance( i, x, y, z, [rx, ry, rz], [scale] )` | Position, rotation (degrees, same order as `rotateTo`) and uniform scale of copy `i` (1-based). Leaving out the rotation keeps it; leaving out the scale keeps it (1 for a hidden copy). |
| `inst:setInstancePose( i, x, y, z, qx, qy, qz, qw, [scale] )` | Same with a quaternion (physics engines). |
| `inst:setInstanceColor( i, r, g, b, [a] )` | Multiplies the model's texture and fill color. |
| `inst:getInstance( i )` | Returns `x, y, z, scale`. |
| `inst:hideInstance( i )` | Hides copy `i` (scale 0). |
| `inst.instanceCount` | Number of copies. |
| `inst.instanced` | `true` with GPU instancing, `false` for the fallback below. |
| `s3d.quatFromEuler( rx, ry, rz )` | Quaternion of the same rotation as `rotateTo( rx, ry, rz )`. |

**Cost of updates.** Each call writes the copy's data through Solar2D's vertex extension API (one call for the
position, one more if the rotation changes, one for the color). That is fine for copies placed once, or a few hundred
moving every frame; moving thousands every frame costs several milliseconds of Lua per frame on a desktop and more on
phones (Solar2D cannot write instance data from C yet).

**Fallback.** `s3d.instancing` is `false` when `system.getInfo( "instancingSupport" )` reports no instancing (Android,
see the note above) or on the Vulkan backend. `newInstances` then makes `count` ordinary objects in a group, with the
same methods: the code keeps working, only slower.

**Limits.** No culling per copy (all of them are drawn); one texture and effect per instance object; no planar
shadows. Effects: `filter.s3d.litinst`, `unlitinst`, `depthinst` (vertex extension `s3dInst`: attributes
`s3dInstPose` = x, y, z, scale; `s3dInstRot` = quaternion; `s3dInstColor`).

## Camera

### `s3d.setCamera( opts )`

| Option | Default | Description |
| --- | --- | --- |
| `position` | `{ 0, 2, 6 }` | Camera position. |
| `target` | `{ 0, 0, 0 }` | Point to look at. |
| `up` | `{ 0, 1, 0 }` | Up vector. |
| `fov` | `60` | Vertical field of view in degrees. |
| `near`, `far` | `0.1`, `100` | Clip planes. |
| `aspect` | screen | Aspect ratio; defaults to `display.actualContentWidth / display.actualContentHeight`. |

Only the given options change.

### `s3d.getCamera()`

Returns a copy: `{ position, target, up, fov, near, far }`.

### `s3d.orbit( yaw, pitch, distance, [target] )`

Places the camera on a sphere around `target` (default: the current target). `yaw = 0` puts the camera on the +Z
side; positive `pitch` looks down from above.

## Light

### `s3d.setLight( opts )`

One directional light.

| Option | Default | Description |
| --- | --- | --- |
| `direction` | `{ -0.4, -1, -0.6 }` | Direction the light travels (from the light towards the scene). |
| `ambient` | `0.3` | Light that reaches every surface. |
| `color` | `{ 1, 1, 1 }` | Light color. |
| `skyColor` | `{ 1, 1, 1 }` | Ambient color on surfaces facing up (multiplied by `ambient`). |
| `groundColor` | `{ 1, 1, 1 }` | Ambient color on surfaces facing down; sides get a blend. |

Changing the light also updates the shadow map's light matrix. A cool `skyColor` with a warm `groundColor` gives soft,
outdoor-looking shading without any extra cost.

### `s3d.setAmbientOcclusion( opts )`

Darkens creases, corners and the bottoms of walls (`filter.s3d.lit` only). Two parts:

- **Baked AO**: for OBJ models loaded after `enabled = true`, the plugin casts `rays` rays from every vertex against the
  model itself, up to `distance`, and stores the result per vertex (`CoronaVertexUserData.w`). Results are cached per
  file and scale, so a model used many times is baked once. Pass `ao = true` / `false` to a constructor to force it on
  or off for one object (primitives are not baked unless `ao = true`). Frame animations are not baked.
- **Ground contact**: computed in the shader from the world height, so it follows moved objects: walls get darker within
  `contactHeight` of `groundY`. Surfaces facing up are not affected.

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | `true` when the call omits it | Bake AO for OBJ models loaded from now on. |
| `rays` | `16` | Rays per vertex (1..64). 12 is usually enough. |
| `distance` | `0.5` | How far occluders count, in model units (after `scale`). |
| `strength` | `1` | How much the baked AO darkens, 0..1. Run-time setting. |
| `contact` | `0` | Ground contact darkening, 0..1. Run-time setting. |
| `contactHeight` | `1` | Height over which the contact darkening fades out. |
| `groundY` | `0` | Height of the ground. |

Baking time grows with vertices × nearby triangles: about 10–20 ms for a 700-triangle track piece and 170 ms for a
2,700-triangle locomotive on a desktop CPU (16 rays). Call it once before loading the level.

### `s3d.setFocus( opts | false )`

Outside a circle (measured in XZ) lit objects lose their colors and fade to a tint: a diorama look where the play area
stands out. Move `center` with the camera every frame if needed; it is cheap.

| Option | Default | Description |
| --- | --- | --- |
| `center` | `{ 0, 0, 0 }` | Center of the focus area. |
| `radius` | `10` | Full colors inside this radius. |
| `falloff` | `5` | Width of the fade. |
| `saturation` | `0` | Saturation outside (0 = gray, 1 = unchanged). |
| `amount` | `0.5` | How much of `color` replaces the gray outside. |
| `color` | `{ 0.7, 0.7, 0.8 }` | Tint outside (shading is kept). |

`s3d.setFocus( false )` turns it off.

## Fog

### `s3d.setFog( opts | false )`

Linear fog by camera depth, computed per pixel. Applies to the `lit`, `unlit` and planar `shadow` effects.

| Option | Default | Description |
| --- | --- | --- |
| `color` | `{ 0.7, 0.8, 0.9 }` | Fog color; usually the same as the post-process `clearColor`. |
| `start` | `10` | Distance where fog starts. |
| `["end"]` (or `finish`) | `40` | Distance where fog is complete. |
| `skyHorizon` | `0.15` | Sky domes (`newSky`) fade into the fog color from this height down to the horizon, and stay fog-colored below it (sine of the angle: 0.15 is about 9 degrees). |

`s3d.setFog( false )` turns fog off.

With a sky dome, use the image's color at the horizon as the fog color, and make the ground reach past the fog's `end`:
the fogged ground then meets the sky without an edge.

## Shadows

There are two kinds. **Shadow maps** cast onto every lit object, including the caster itself. **Planar shadows**
flatten each caster onto one horizontal plane: cheap, but they only fall on the floor. `obj:castShadow( true )` (or
`castShadow = true` when creating) uses whichever mode is active **when the object starts casting**, so enable the shadow
map before creating casters.

### `s3d.setShadowMap( opts )`

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | see below | `false` turns shadow maps off. |
| `size` | `1024` | Shadow texture size, 64..4096. |
| `area` | `10` | Half the side of the square that receives shadows, around `center`. |
| `center` | `{ 0, 0, 0 }` | Center of that square. Move it with the camera to keep shadows around the player. |
| `distance` | `30` | Distance from the light to `center` (orthographic projection). |
| `bias` | `0.0015` | Fights "shadow acne"; grazing surfaces get up to 4x more automatically. |
| `darkness` | `0.6` | 0 = no visible shadow, 1 = the directional light is fully blocked (ambient remains). |
| `quality` | mobile `"medium"`, desktop `"high"` | `"low"` (1 sample), `"medium"` (2x2 PCF), `"high"` (3x3 PCF), `"ultra"` (4x4 PCF). |
| `softness` | `1` | Distance between filter samples, in shadow-map texels. 2–3 with `"high"` / `"ultra"` gives soft edges. |

Only the given keys change. When `enabled` is omitted, the first call turns shadow maps on and later calls keep the
current state, so you can move `center` every frame without re-enabling shadows that were turned off.

Every object using `filter.s3d.lit` receives shadows, whether or not it casts one.

**Performance:** the main cost on phones is texture reads on every shadow-receiving pixel (a floor covers the whole
screen). `size = 1024` with `quality = "medium"` is a good balance on mobile; `"low"` is the cheapest but has jagged edges.

```lua
s3d.setShadowMap{ size = s3d.isMobile and 1024 or 2048, quality = s3d.isMobile and "medium" or "high",
    area = 12, center = { 0, 0, -3 }, distance = 30, bias = 0.0015, darkness = 0.7 }
```

### `s3d.setShadow( opts )`

Settings for planar shadows.

| Option | Default | Description |
| --- | --- | --- |
| `planeY` | `0` | Height of the plane that receives shadows. |
| `color` | `{ 0, 0, 0, 0.35 }` | Shadow color; alpha is the strength. |

### `s3d.getShadowMode()`

Returns `"map"` while shadow maps are enabled, otherwise `"planar"`.

## Picking and projection

All screen coordinates are Solar2D content coordinates (the same as touch events).

| Function | Returns |
| --- | --- |
| `s3d.worldToScreen( x, y, z )` | `sx, sy, depth` (depth in NDC, -1..1), or `nil` when the point cannot be projected. |
| `s3d.screenToRay( sx, sy )` | `origin, direction` (two `{ x, y, z }` tables) of the ray from the camera through the point. |
| `s3d.rayPlaneY( origin, direction, [h] )` | `x, y, z` where the ray hits the plane `y = h` (default 0), or `nil` if it never does. |

```lua
Runtime:addEventListener( "tap", function( e )
    local o, d = s3d.screenToRay( e.x, e.y )
    local x, y, z = s3d.rayPlaneY( o, d, 0 )
    if x then ball:moveTo( x, 0.5, z ) end
end )
```

## Effects

### Built-in effects

| Effect | Use |
| --- | --- |
| `filter.s3d.lit` | Default. Texture × (ambient + light × shadow), plus fog. |
| `filter.s3d.unlit` | Texture and fill color only, plus fog. Good for skies, glowing objects, the demo's sun. |
| `filter.s3d.reflect` | `lit` plus an environment reflection; set by `setReflection` (needs a composite paint). |
| `filter.s3d.sky` | Texture and fill color only, no fog. Used by `newSky`. |
| `filter.s3d.sprite`, `spritelit`, `depthsprite` | Sprites (`newSprite`): unlit / lit with 50% alpha cut-out, and their shadow-pass version. |
| `filter.s3d.litinst`, `unlitinst`, `depthinst` | `lit`, `unlit` and shadow-pass `depth` for `newInstances` (vertex extension `s3dInst`). |
| `filter.s3d.normals` | Shows normals as colors, for debugging. |
| `filter.s3d.shadow` | Used internally for planar shadows. |
| `filter.s3d.depth` | Used internally for the shadow map pass. |
| `filter.s3d.post` | Used by `newPostProcess`. |

### Custom effects

Any effect defined with `shellTransform = "s3d"` and `dataType = "s3d"` gets the 3D transform:

```lua
graphics.defineEffect{
    category = "filter", group = "game", name = "toon",
    shellTransform = "s3d", dataType = "s3d",
    fragment = [[
uniform P_DEFAULT vec4 u_S3D_LightDir;   // xyz: direction towards the light, w: ambient
uniform P_DEFAULT vec4 u_S3D_LightColor;
P_COLOR vec4 FragmentKernel( P_UV vec2 uv )
{
    P_DEFAULT vec3 n = normalize( CoronaVertexUserData.xyz );   // world-space normal
    P_DEFAULT float d = step( 0.4, dot( n, u_S3D_LightDir.xyz ) );
    return CoronaColorScale( texture2D( CoronaSampler0, uv ) * ( 0.5 + 0.5 * d ) );
}
]]
}
obj:setEffect( "filter.game.toon", { light = true, fog = false, shadow = false } )
```

The second argument of `setEffect` tells the plugin which uniforms the effect declares, so it never writes a uniform
that does not exist (Solar2D would print a warning every frame). Defaults: `light = true`, `fog = true`, `shadow = false`,
`style = false` (`style = true`: the effect declares all six `u_S3D_Ambient*` / `u_S3D_AOParams` / `u_S3D_Focus*` uniforms),
`env = false` (`env = true`: the effect declares `u_S3D_Eye` and `u_S3D_Env`).

What the plugin provides:

| Name | Kind | Meaning |
| --- | --- | --- |
| `CoronaVertexUserData.xyz` | vertex data | World-space normal (not normalized after interpolation). |
| `CoronaVertexUserData.w` | vertex data | Baked ambient occlusion (1 = open, 0 = closed); 1 when not baked. |
| `u_S3D_LightDir` | uniform `vec4` | xyz: direction towards the light (normalized), w: ambient. Needs `light = true`. |
| `u_S3D_LightColor` | uniform `vec4` | rgb: light color. Needs `light = true`. |
| `u_S3D_Fog` | uniform `vec4` | rgb: fog color, a: 1 when fog is on. Needs `fog = true`. |
| `u_S3D_FogRange` | uniform `vec4` | x: start, y: end. Needs `fog = true`. |
| `v_S3D_Depth` | varying `float` | Camera depth of the pixel (for fog). |
| `u_S3D_ShadowParams` | uniform `vec4` | x: 0 off / 1, 2, 3, 4 quality; y: bias; z: softness / size; w: darkness. Needs `shadow = true`. |
| `u_S3D_ShadowMap` | `sampler2D` | Shadow map (RGBA8, depth packed in RGB). Needs `shadow = true`. |
| `v_S3D_ShadowPos` | varying `vec4` | Light-space clip position of the pixel. |
| `v_S3D_World` | varying `vec3` | World position of the pixel. |
| `u_S3D_AmbientSky`, `u_S3D_AmbientGround` | uniform `vec4` | rgb: hemisphere ambient colors. Needs `style = true`. |
| `u_S3D_AOParams` | uniform `vec4` | x: AO strength, y: contact strength, z: contact height, w: ground y. Needs `style = true`. |
| `u_S3D_Focus`, `u_S3D_FocusParams`, `u_S3D_FocusColor` | uniform `vec4` | Focus area (see `setFocus`). Needs `style = true`. |
| `u_S3D_Eye` | uniform `vec4` | xyz: camera position. Needs `env = true`. |
| `u_S3D_Env` | uniform `vec4` | This object's reflection: x: strength, y: F0, z: blur, w: rotation (radians). Needs `env = true`. |

The varyings are declared by the shell transform; do not declare them again. The fog and shadow helpers used by the
built-in effects (`S3D_ApplyFog`, `S3D_ShadowLit`, `S3D_ShadowVisibility`) are in `src/s3d.lua` and can be copied into
your own shaders, as can the style helpers (`S3D_Ambient`, `S3D_Occlusion`, `S3D_ApplyFocus`) and the reflection helpers
(`S3D_EnvColor`, `S3D_ApplyReflection`).
