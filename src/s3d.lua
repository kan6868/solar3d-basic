-- plugin.s3d (Lua half). Embedded in the DLL / .so and run with the table of C functions as its argument.
local C = ...

local M = { version = C.version }

local display, graphics, system, io = display, graphics, system, io
local sqrt, sin, cos, pi, abs = math.sqrt, math.sin, math.cos, math.pi, math.abs

-- ---------------------------------------------------------------------------
-- Effect
-- Fog: v_S3D_Depth = camera depth (gl_Position.w; the shell transform passes it so fog is computed per pixel)
local FOG = [[
uniform P_DEFAULT vec4 u_S3D_Fog;      // rgb: color, a: 1 = enabled
uniform P_DEFAULT vec4 u_S3D_FogRange; // x: start, y: end

P_DEFAULT float S3D_FogFactor()
{
    P_DEFAULT float range = max( u_S3D_FogRange.y - u_S3D_FogRange.x, 0.0001 );
    return u_S3D_Fog.a * clamp( ( v_S3D_Depth - u_S3D_FogRange.x ) / range, 0.0, 1.0 );
}

// color is premultiplied by alpha
P_COLOR vec4 S3D_ApplyFog( P_COLOR vec4 c )
{
    return vec4( mix( c.rgb, u_S3D_Fog.rgb * c.a, S3D_FogFactor() ), c.a );
}
]]

-- Shadow map: v_S3D_ShadowPos (light clip coordinates) is declared and computed by the shell transform
local SHADOW = [[
uniform P_DEFAULT vec4 u_S3D_ShadowParams; // x: 0 off, 1/2/3/4 quality (1, 4, 9, 16 taps); y: bias; z: softness/size; w: darkness
uniform sampler2D u_S3D_ShadowMap;

P_DEFAULT float S3D_Unpack( P_DEFAULT vec3 c )
{
    return dot( c, vec3( 1.0, 1.0 / 255.0, 1.0 / 65025.0 ) );
}

P_DEFAULT float S3D_ShadowTap( P_DEFAULT vec2 uv, P_DEFAULT float z )
{
    return ( z > S3D_Unpack( texture2D( u_S3D_ShadowMap, uv ).rgb ) ) ? 0.0 : 1.0;
}

// lit fraction: 1 = lit, 0 = in shadow. PCF filter: 1 tap, 2x2, 3x3 or 4x4 depending on quality
P_DEFAULT float S3D_ShadowLit( P_DEFAULT float ndl )
{
    if ( u_S3D_ShadowParams.x < 0.5 ) { return 1.0; }
    P_DEFAULT vec3 p = v_S3D_ShadowPos.xyz / v_S3D_ShadowPos.w * 0.5 + 0.5;
    if ( p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0 ) { return 1.0; }
    P_DEFAULT float bias = u_S3D_ShadowParams.y * ( 1.0 + 3.0 * ( 1.0 - ndl ) ); // grazing surfaces need more bias
    P_DEFAULT float z = p.z - bias;
    P_DEFAULT float t = u_S3D_ShadowParams.z;
    if ( u_S3D_ShadowParams.x < 1.5 ) { return S3D_ShadowTap( p.xy, z ); }
    if ( u_S3D_ShadowParams.x < 2.5 )
    {
        P_DEFAULT float h = 0.5 * t; // 2x2 texels around the point
        return 0.25 * ( S3D_ShadowTap( p.xy + vec2( -h, -h ), z ) + S3D_ShadowTap( p.xy + vec2( h, -h ), z )
                      + S3D_ShadowTap( p.xy + vec2( -h, h ), z ) + S3D_ShadowTap( p.xy + vec2( h, h ), z ) );
    }
    P_DEFAULT float lit = 0.0;
    if ( u_S3D_ShadowParams.x < 3.5 )
    {
        for ( int i = -1; i <= 1; i++ )
        {
            for ( int j = -1; j <= 1; j++ )
            {
                lit += S3D_ShadowTap( p.xy + vec2( float( i ), float( j ) ) * t, z );
            }
        }
        return lit / 9.0;
    }
    for ( int i = 0; i < 4; i++ ) // 4x4 ("ultra")
    {
        for ( int j = 0; j < 4; j++ )
        {
            lit += S3D_ShadowTap( p.xy + ( vec2( float( i ), float( j ) ) - 1.5 ) * t, z );
        }
    }
    return lit / 16.0;
}

P_DEFAULT float S3D_ShadowVisibility( P_DEFAULT float ndl )
{
    return mix( 1.0 - u_S3D_ShadowParams.w, 1.0, S3D_ShadowLit( ndl ) );
}
]]

-- Style: hemisphere ambient, ambient occlusion (baked per vertex in CoronaVertexUserData.w + contact with the ground),
-- focus area (outside it colors fade to gray / a tint). v_S3D_World = world position, from the shell transform.
local STYLE = [[
uniform P_DEFAULT vec4 u_S3D_AmbientSky;    // rgb: ambient from above
uniform P_DEFAULT vec4 u_S3D_AmbientGround; // rgb: ambient from below
uniform P_DEFAULT vec4 u_S3D_AOParams;      // x: baked AO strength, y: ground contact strength, z: contact height, w: ground y
uniform P_DEFAULT vec4 u_S3D_Focus;         // xyz: center, w: radius
uniform P_DEFAULT vec4 u_S3D_FocusParams;   // x: falloff, y: saturation outside, z: color amount outside, w: 1 = on
uniform P_DEFAULT vec4 u_S3D_FocusColor;    // rgb: color outside

P_DEFAULT vec3 S3D_Ambient( P_DEFAULT vec3 n )
{
    return mix( u_S3D_AmbientGround.rgb, u_S3D_AmbientSky.rgb, n.y * 0.5 + 0.5 );
}

// 1 = open, 0 = fully occluded
P_DEFAULT float S3D_Occlusion( P_DEFAULT vec3 n )
{
    P_DEFAULT float ao = mix( 1.0, CoronaVertexUserData.w, u_S3D_AOParams.x );
    P_DEFAULT float h = clamp( ( v_S3D_World.y - u_S3D_AOParams.w ) / u_S3D_AOParams.z, 0.0, 1.0 );
    P_DEFAULT float contact = u_S3D_AOParams.y * ( 1.0 - h ) * ( 1.0 - h ) * ( 1.0 - max( n.y, 0.0 ) ); // walls near the ground
    return ao * ( 1.0 - contact );
}

// color is premultiplied by alpha
P_COLOR vec4 S3D_ApplyFocus( P_COLOR vec4 c )
{
    if ( u_S3D_FocusParams.w < 0.5 ) { return c; }
    P_DEFAULT float t = smoothstep( u_S3D_Focus.w, u_S3D_Focus.w + u_S3D_FocusParams.x, distance( v_S3D_World.xz, u_S3D_Focus.xz ) );
    P_DEFAULT float l = dot( c.rgb, vec3( 0.299, 0.587, 0.114 ) );
    P_DEFAULT vec3 outside = mix( vec3( l ), c.rgb, u_S3D_FocusParams.y );
    outside = mix( outside, u_S3D_FocusColor.rgb * ( 0.35 * c.a + 0.65 * l ), u_S3D_FocusParams.z );
    return vec4( mix( c.rgb, outside, t ), c.a );
}
]]

local FRAG_LIT = [[
uniform P_DEFAULT vec4 u_S3D_LightDir;
uniform P_DEFAULT vec4 u_S3D_LightColor;
]] .. FOG .. SHADOW .. STYLE .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_COLOR vec4 c = texture2D( CoronaSampler0, texCoord );
    P_DEFAULT vec3 n = normalize( CoronaVertexUserData.xyz );
    P_DEFAULT float ndl = dot( n, u_S3D_LightDir.xyz );
    P_DEFAULT float d = 0.0;
    if ( ndl > 0.0 ) { d = ndl * S3D_ShadowVisibility( ndl ); } // facing away from the light: skip the shadow map read
    P_DEFAULT float ao = S3D_Occlusion( n );
    P_DEFAULT vec3 light = u_S3D_LightDir.w * S3D_Ambient( n ) * ao + u_S3D_LightColor.rgb * d * mix( 1.0, ao, 0.5 );
    return S3D_ApplyFog( S3D_ApplyFocus( CoronaColorScale( vec4( c.rgb * light, c.a ) ) ) );
}
]]

-- Environment reflection (effect "reflect"): the object's paint is composite, paint2 = an equirectangular (360 x 180)
-- environment image, mapped like s3d.newSphere's UVs (u along the angle around Y from +Z, v = 0 straight up), so a sky
-- dome made with newSphere and the same image matches its reflections. Fresnel (Schlick): grazing angles reflect more.
local ENV = [[
uniform P_DEFAULT vec4 u_S3D_Eye; // xyz: camera position
uniform P_DEFAULT vec4 u_S3D_Env; // x: strength, y: F0 (reflection when seen head-on), z: blur (mip bias), w: rotation (radians)

P_COLOR vec3 S3D_EnvColor( P_DEFAULT vec3 r )
{
    P_DEFAULT float cs = cos( u_S3D_Env.w );
    P_DEFAULT float sn = sin( u_S3D_Env.w );
    r = vec3( cs * r.x + sn * r.z, r.y, cs * r.z - sn * r.x );
    // same mapping as newSphere's UVs; with mipmaps (blur) a thin seam can show where u wraps from 1 to 0
    P_DEFAULT vec2 uv = vec2( fract( atan( r.x, r.z ) * 0.15915494 ), acos( clamp( r.y, -1.0, 1.0 ) ) * 0.31830989 );
    return texture2D( CoronaSampler1, uv, u_S3D_Env.z ).rgb;
}

// lit: the lit color; base: the surface color; n: world normal. Returns the color with the reflection added.
P_COLOR vec3 S3D_ApplyReflection( P_COLOR vec3 lit, P_DEFAULT vec3 n, P_DEFAULT float ao, P_DEFAULT float alpha )
{
    P_DEFAULT vec3 v = normalize( u_S3D_Eye.xyz - v_S3D_World );
    P_DEFAULT float nv = max( dot( n, v ), 0.0 );
    P_DEFAULT float f = u_S3D_Env.y + ( 1.0 - u_S3D_Env.y ) * pow( 1.0 - nv, 5.0 );
    P_DEFAULT float k = clamp( f * u_S3D_Env.x, 0.0, 1.0 );
    P_COLOR vec3 env = S3D_EnvColor( reflect( -v, n ) ) * mix( 1.0, ao, 0.7 );
    return mix( lit, env * alpha, k );
}
]]

local FRAG_REFLECT = [[
uniform P_DEFAULT vec4 u_S3D_LightDir;
uniform P_DEFAULT vec4 u_S3D_LightColor;
]] .. FOG .. SHADOW .. STYLE .. ENV .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_COLOR vec4 c = texture2D( CoronaSampler0, texCoord );
    P_DEFAULT vec3 n = normalize( CoronaVertexUserData.xyz );
    P_DEFAULT float ndl = dot( n, u_S3D_LightDir.xyz );
    P_DEFAULT float d = 0.0;
    if ( ndl > 0.0 ) { d = ndl * S3D_ShadowVisibility( ndl ); }
    P_DEFAULT float ao = S3D_Occlusion( n );
    P_DEFAULT vec3 light = u_S3D_LightDir.w * S3D_Ambient( n ) * ao + u_S3D_LightColor.rgb * d * mix( 1.0, ao, 0.5 );
    P_COLOR vec4 lit = CoronaColorScale( vec4( c.rgb * light, c.a ) );
    // the tint (setFillColor) colors the surface, not the reflected sky; colors are premultiplied by alpha
    lit.rgb = S3D_ApplyReflection( lit.rgb, n, ao, lit.a );
    return S3D_ApplyFog( S3D_ApplyFocus( lit ) );
}
]]

-- Shadow pass: writes light-space depth (0..1) packed into 24 bits of RGB, alpha = 1 (normal blending keeps the value)
local FRAG_DEPTH = [[
P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_DEFAULT float z = clamp( v_S3D_ShadowPos.z / v_S3D_ShadowPos.w * 0.5 + 0.5, 0.0, 1.0 );
    P_DEFAULT vec3 enc = fract( z * vec3( 1.0, 255.0, 65025.0 ) );
    enc -= enc.yzz * vec3( 1.0 / 255.0, 1.0 / 255.0, 0.0 );
    return vec4( enc, 1.0 );
}
]]

local FRAG_UNLIT = FOG .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    return S3D_ApplyFog( CoronaColorScale( texture2D( CoronaSampler0, texCoord ) ) );
}
]]

-- Sprites (s3d.newSprite): alpha-tested, so they write depth without sorting (pixels below 50% alpha are cut out)
local FRAG_SPRITE = FOG .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_COLOR vec4 c = CoronaColorScale( texture2D( CoronaSampler0, texCoord ) );
    if ( c.a < 0.5 ) { discard; }
    return S3D_ApplyFog( c );
}
]]
local FRAG_SPRITE_LIT, spriteLitPatched = FRAG_LIT:gsub( "P_COLOR vec4 c = texture2D%( CoronaSampler0, texCoord %);",
    "P_COLOR vec4 c = texture2D( CoronaSampler0, texCoord );\n    if ( c.a * CoronaColorScale( vec4( 1.0 ) ).a < 0.5 ) { discard; }", 1 )
assert( spriteLitPatched == 1, "s3d: sprite shader patch failed" )
local FRAG_DEPTH_SPRITE = [[
P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    if ( texture2D( CoronaSampler0, texCoord ).a < 0.5 ) { discard; }
    P_DEFAULT float z = clamp( v_S3D_ShadowPos.z / v_S3D_ShadowPos.w * 0.5 + 0.5, 0.0, 1.0 );
    P_DEFAULT vec3 enc = fract( z * vec3( 1.0, 255.0, 65025.0 ) );
    enc -= enc.yzz * vec3( 1.0 / 255.0, 1.0 / 255.0, 0.0 );
    return vec4( enc, 1.0 );
}
]]

local FRAG_UNLIT_NOFOG = [[
P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    return CoronaColorScale( texture2D( CoronaSampler0, texCoord ) );
}
]]

-- Sky dome (newSky): no distance fog, but with fog on the sky fades into the fog color towards the horizon and below it,
-- so the fogged ground and the sky meet without an edge whatever the image. The dome is centered on the camera, so its
-- normal is the view direction. u_S3D_FogRange.z: height (sine of the angle above the horizon) where the fade ends.
local FRAG_SKY = FOG .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_COLOR vec4 c = CoronaColorScale( texture2D( CoronaSampler0, texCoord ) );
    P_DEFAULT float y = normalize( CoronaVertexUserData.xyz ).y;
    P_DEFAULT float t = u_S3D_Fog.a * ( 1.0 - smoothstep( 0.0, max( u_S3D_FogRange.z, 0.001 ), y ) );
    return vec4( mix( c.rgb, u_S3D_Fog.rgb * c.a, t ), c.a );
}
]]

-- Planar shadow: color comes from setFillColor; fades out in fog instead of being tinted by it
local FRAG_SHADOW = FOG .. [[

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    return CoronaColorScale( vec4( 1.0 ) ) * ( 1.0 - S3D_FogFactor() );
}
]]

local FRAG_NORMALS = [[
P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_DEFAULT vec3 n = normalize( CoronaVertexUserData.xyz );
    return CoronaColorScale( vec4( n * 0.5 + 0.5, 1.0 ) );
}
]]

-- Post-processing applied to the snapshot that holds the scene: FXAA + tone mapping + color grading + vignette
local FRAG_POST = [[
uniform P_DEFAULT vec4 u_UserData0; // exposure, contrast, saturation, gamma
uniform P_DEFAULT vec4 u_UserData1; // tint r, g, b, vignette
uniform P_DEFAULT vec4 u_UserData2; // fxaa (0/1), vignette softness, brightness, tone mapping (0 none, 1 ACES)

P_DEFAULT float S3D_Luma( P_DEFAULT vec3 c ) { return dot( c, vec3( 0.299, 0.587, 0.114 ) ); }

// FXAA "lite" (based on FXAA 3.x by Timothy Lottes)
P_DEFAULT vec3 S3D_Fxaa( P_UV vec2 uv, P_DEFAULT vec2 px )
{
    P_DEFAULT vec3 rgbNW = texture2D( CoronaSampler0, uv + vec2( -1.0, -1.0 ) * px ).rgb;
    P_DEFAULT vec3 rgbNE = texture2D( CoronaSampler0, uv + vec2( 1.0, -1.0 ) * px ).rgb;
    P_DEFAULT vec3 rgbSW = texture2D( CoronaSampler0, uv + vec2( -1.0, 1.0 ) * px ).rgb;
    P_DEFAULT vec3 rgbSE = texture2D( CoronaSampler0, uv + vec2( 1.0, 1.0 ) * px ).rgb;
    P_DEFAULT vec3 rgbM = texture2D( CoronaSampler0, uv ).rgb;
    P_DEFAULT float lNW = S3D_Luma( rgbNW ), lNE = S3D_Luma( rgbNE ), lSW = S3D_Luma( rgbSW ), lSE = S3D_Luma( rgbSE );
    P_DEFAULT float lM = S3D_Luma( rgbM );
    P_DEFAULT float lMin = min( lM, min( min( lNW, lNE ), min( lSW, lSE ) ) );
    P_DEFAULT float lMax = max( lM, max( max( lNW, lNE ), max( lSW, lSE ) ) );

    P_DEFAULT vec2 dir = vec2( -( ( lNW + lNE ) - ( lSW + lSE ) ), ( lNW + lSW ) - ( lNE + lSE ) );
    P_DEFAULT float reduce = max( ( lNW + lNE + lSW + lSE ) * 0.03125, 0.0078125 );
    P_DEFAULT float rcpMin = 1.0 / ( min( abs( dir.x ), abs( dir.y ) ) + reduce );
    dir = clamp( dir * rcpMin, vec2( -8.0 ), vec2( 8.0 ) ) * px;

    P_DEFAULT vec3 a = 0.5 * ( texture2D( CoronaSampler0, uv + dir * ( 1.0 / 3.0 - 0.5 ) ).rgb
                             + texture2D( CoronaSampler0, uv + dir * ( 2.0 / 3.0 - 0.5 ) ).rgb );
    P_DEFAULT vec3 b = a * 0.5 + 0.25 * ( texture2D( CoronaSampler0, uv + dir * -0.5 ).rgb
                                        + texture2D( CoronaSampler0, uv + dir * 0.5 ).rgb );
    P_DEFAULT float lB = S3D_Luma( b );
    return ( lB < lMin || lB > lMax ) ? a : b;
}

P_COLOR vec4 FragmentKernel( P_UV vec2 texCoord )
{
    P_COLOR vec4 src = texture2D( CoronaSampler0, texCoord );
    P_DEFAULT vec3 c = ( u_UserData2.x > 0.5 ) ? S3D_Fxaa( texCoord, CoronaTexelSize.xy ) : src.rgb;

    c = c * u_UserData0.x + vec3( u_UserData2.z );                      // exposure, brightness
    if ( u_UserData2.w > 0.5 )                                          // ACES filmic curve (Narkowicz fit)
    {
        c = max( c, vec3( 0.0 ) );
        c = clamp( ( c * ( 2.51 * c + 0.03 ) ) / ( c * ( 2.43 * c + 0.59 ) + 0.14 ), 0.0, 1.0 );
    }
    c = ( c - 0.5 ) * u_UserData0.y + 0.5;                              // contrast
    c = mix( vec3( S3D_Luma( c ) ), c, u_UserData0.z );                 // saturation
    c *= u_UserData1.rgb;                                               // tint
    c = pow( max( c, vec3( 0.0 ) ), vec3( 1.0 / max( u_UserData0.w, 0.01 ) ) ); // gamma

    P_DEFAULT float d = distance( texCoord, vec2( 0.5 ) ) * 1.41421;    // 0 at the center, 1 in the corners
    c *= 1.0 - u_UserData1.a * smoothstep( 1.0 - u_UserData2.y, 1.0, d );

    return CoronaColorScale( vec4( clamp( c, 0.0, 1.0 ) * src.a, src.a ) );
}
]]

M.EFFECT_LIT = "filter.s3d.lit"
M.EFFECT_UNLIT = "filter.s3d.unlit"
M.EFFECT_REFLECT = "filter.s3d.reflect"
M.EFFECT_SKY = "filter.s3d.sky"
M.EFFECT_SPRITE = "filter.s3d.sprite"
M.EFFECT_SPRITE_LIT = "filter.s3d.spritelit"
M.EFFECT_DEPTH_SPRITE = "filter.s3d.depthsprite"
M.EFFECT_LIT_INST = "filter.s3d.litinst"
M.EFFECT_UNLIT_INST = "filter.s3d.unlitinst"
M.EFFECT_DEPTH_INST = "filter.s3d.depthinst"
M.EFFECT_NORMALS = "filter.s3d.normals"
M.EFFECT_SHADOW = "filter.s3d.shadow"
M.EFFECT_DEPTH = "filter.s3d.depth"
M.EFFECT_POST = "filter.s3d.post"

local camera = { position = { 0, 2, 6 }, target = { 0, 0, 0 }, up = { 0, 1, 0 }, fov = 60, near = 0.1, far = 100 }
local light = { direction = { -0.4, -1, -0.6 }, ambient = 0.3, color = { 1, 1, 1 }, skyColor = { 1, 1, 1 }, groundColor = { 1, 1, 1 } }
local ao = { enabled = false, rays = 16, distance = 0.5, strength = 1, contact = 0, contactHeight = 1, groundY = 0 }
local aoCache = {}

local function sendAmbient()
    local s, g = light.skyColor, light.groundColor
    C.setAmbient( s[1], s[2], s[3], g[1], g[2], g[3], ao.strength, ao.contact, ao.contactHeight, ao.groundY )
end

local inited = false

-- Solar2D's Vulkan backend has no custom commands, named uniforms or shell transforms yet,
-- so the plugin only works on the OpenGL / OpenGL ES backend.
local backend = system.getInfo( "graphicsBackend" ) or "glBackend"
M.backend = backend
M.supported = ( backend ~= "vulkanBackend" )

function M.isSupported() return M.supported end

local function define( name, fragment, vertexExtension )
    graphics.defineEffect{
        language = "glsl", category = "filter", group = "s3d", name = name,
        shellTransform = "s3d", dataType = "s3d", vertexExtension = vertexExtension,
        fragment = fragment,
    }
end

-- GPU instancing (s3d.newInstances): one mesh drawn many times in one draw call; each instance has a position + scale,
-- a rotation quaternion and a color, applied in the vertex shader (see the shell transform in s3d.c).
local INSTANCE_EXTENSION = "s3dInst"
local function defineInstancing()
    local support = system.getInfo( "instancingSupport" )
    if not support or not graphics.defineVertexExtension then return false end
    local ok = graphics.defineVertexExtension{ name = INSTANCE_EXTENSION,
        { name = "s3dInstPose", type = "float", componentCount = 4, instancesToReplicate = 1 },  -- x, y, z, scale
        { name = "s3dInstRot", type = "float", componentCount = 4, instancesToReplicate = 1 },   -- quaternion x, y, z, w
        { name = "s3dInstColor", type = "float", componentCount = 4, instancesToReplicate = 1 }, -- r, g, b, a
    }
    return ok ~= false
end

-- GPU node animation (glTF): every vertex of an animated part carries the index of the node that moves it (per-vertex
-- vertex extension, no instancing needed); the node matrices go to the vertex shader as a uniform array (see s3d.c).
-- Each built-in effect a glTF part can use gets a "...node" variant.
local NODE_EXTENSION = "s3dNodes"
local NODE_MAX = 24
local NODE_VARIANT, NODE_BASE = {}, {}
local function defineNodes()
    if not graphics.defineVertexExtension then return false end
    local ok, res = pcall( graphics.defineVertexExtension, { name = NODE_EXTENSION,
        { name = "s3dNode", type = "float", componentCount = 1 },
    } )
    return ok and res ~= false
end
M.gpuAnimation = true -- s3d.setGPUAnimation( false ): later loadGLTF calls pose on the CPU
function M.setGPUAnimation( on ) M.gpuAnimation = on ~= false end

function M.init()
    if inited then return end
    inited = true
    if not M.supported then
        print( "WARNING: plugin.s3d: backend '" .. backend .. "' is not supported. Solar2D's Vulkan backend has no custom commands, "
            .. "named uniforms or shell transforms yet, so 3D objects will be hidden. Use the OpenGL backend: remove backend = \"wantVulkan\" "
            .. "from config.lua." )
    end
    C.init()
    display.setDefault( "enableDepth", true ) -- clear the depth buffer every frame
    -- on Vulkan, do not declare custom uniforms in the kernel (the Vulkan shader translator rejects them)
    define( "lit", M.supported and FRAG_LIT or FRAG_UNLIT_NOFOG )
    define( "unlit", M.supported and FRAG_UNLIT or FRAG_UNLIT_NOFOG )
    define( "reflect", M.supported and FRAG_REFLECT or FRAG_UNLIT_NOFOG )
    define( "sky", M.supported and FRAG_SKY or FRAG_UNLIT_NOFOG )
    define( "sprite", M.supported and FRAG_SPRITE or FRAG_UNLIT_NOFOG )
    define( "spritelit", M.supported and FRAG_SPRITE_LIT or FRAG_UNLIT_NOFOG )
    define( "depthsprite", M.supported and FRAG_DEPTH_SPRITE or FRAG_UNLIT_NOFOG )
    define( "normals", FRAG_NORMALS )
    define( "shadow", M.supported and FRAG_SHADOW or FRAG_UNLIT_NOFOG )
    define( "depth", M.supported and FRAG_DEPTH or FRAG_UNLIT_NOFOG )
    M.instancing = M.supported and defineInstancing() or false
    M.gpuNodes = M.supported and defineNodes() or false
    if M.gpuNodes then
        for _, v in ipairs{ { "lit", FRAG_LIT }, { "unlit", FRAG_UNLIT }, { "reflect", FRAG_REFLECT }, { "sprite", FRAG_SPRITE },
            { "spritelit", FRAG_SPRITE_LIT }, { "depthsprite", FRAG_DEPTH_SPRITE }, { "shadow", FRAG_SHADOW }, { "depth", FRAG_DEPTH } } do
            define( v[1] .. "node", v[2], NODE_EXTENSION )
            NODE_VARIANT["filter.s3d." .. v[1]] = "filter.s3d." .. v[1] .. "node"
            NODE_BASE["filter.s3d." .. v[1] .. "node"] = "filter.s3d." .. v[1]
        end
    end
    if M.instancing then
        define( "litinst", FRAG_LIT, INSTANCE_EXTENSION )
        define( "unlitinst", FRAG_UNLIT, INSTANCE_EXTENSION )
        define( "depthinst", FRAG_DEPTH, INSTANCE_EXTENSION )
    end
    graphics.defineEffect{
        language = "glsl", category = "filter", group = "s3d", name = "post",
        uniformData = {
            { name = "grade", type = "vec4", index = 0, default = { 1, 1, 1, 1 } },     -- exposure, contrast, saturation, gamma
            { name = "tint", type = "vec4", index = 1, default = { 1, 1, 1, 0 } },      -- r, g, b, vignette
            { name = "options", type = "vec4", index = 2, default = { 1, 0.5, 0, 0 } }, -- fxaa, vignette softness, brightness, tone mapping
        },
        fragment = FRAG_POST,
    }
    Runtime:addEventListener( "enterFrame", function( e ) C.beginFrame(); M._flushDynamic(); M._stepSprites( e.time ); M._stepModels( e.time ) end )
    M.setCamera( camera )
    M.setLight( light )
end

-- ---------------------------------------------------------------------------
-- Camera / light
local function v3( t, d ) t = t or d; return { t[1] or t.x or 0, t[2] or t.y or 0, t[3] or t.z or 0 } end

function M.setCamera( opts )
    opts = opts or {}
    camera.position = v3( opts.position, camera.position )
    camera.target = v3( opts.target, camera.target )
    camera.up = v3( opts.up, camera.up )
    camera.fov = opts.fov or camera.fov
    camera.near = opts.near or camera.near
    camera.far = opts.far or camera.far
    local aspect = opts.aspect or ( display.actualContentWidth / display.actualContentHeight )
    local p, t, u = camera.position, camera.target, camera.up
    C.setCamera( p[1], p[2], p[3], t[1], t[2], t[3], u[1], u[2], u[3], camera.fov, aspect, camera.near, camera.far )
    -- sky domes (s3d.newSky) stay centered on the camera, so they look infinitely far away
    for sky in pairs( M._skies ) do
        if sky.removeSelf then sky:moveTo( p[1], p[2], p[3] ) else M._skies[sky] = nil end
    end
end
M._skies = setmetatable( {}, { __mode = "k" } )

function M.getCamera()
    return {
        position = { unpack( camera.position ) }, target = { unpack( camera.target ) }, up = { unpack( camera.up ) },
        fov = camera.fov, near = camera.near, far = camera.far,
    }
end

-- Camera orbiting a target: yaw/pitch in degrees
function M.orbit( yaw, pitch, distance, target )
    target = v3( target, camera.target )
    local y, p = math.rad( yaw ), math.rad( pitch )
    local cp = cos( p )
    M.setCamera{
        target = target,
        position = { target[1] + distance * cp * sin( y ), target[2] + distance * sin( p ), target[3] + distance * cp * cos( y ) },
    }
end

function M.setLight( opts )
    opts = opts or {}
    light.direction = v3( opts.direction, light.direction )
    light.ambient = opts.ambient or light.ambient
    light.color = opts.color or light.color
    -- hemisphere ambient: surfaces facing up get skyColor, facing down groundColor (both times `ambient`)
    if opts.skyColor then light.skyColor = opts.skyColor end
    if opts.groundColor then light.groundColor = opts.groundColor end
    local d, c = light.direction, light.color
    C.setLight( d[1], d[2], d[3], light.ambient, c[1] or 1, c[2] or 1, c[3] or 1 )
    sendAmbient()
end

-- ---------------------------------------------------------------------------
-- Ambient occlusion
--   s3d.setAmbientOcclusion{ enabled = true, rays = 16, distance = 0.5, strength = 1, contact = 0.5, contactHeight = 1, groundY = 0 }
-- enabled: bake AO for OBJ models loaded from now on (opts.ao = true / false on a constructor overrides it).
-- Baking casts `rays` rays per vertex against the model itself, up to `distance` (model units); results are cached per file.
-- strength: how much the baked AO darkens (0..1). contact / contactHeight / groundY: walls darken near the ground plane
-- (computed in the shader, so it also works for moved objects; surfaces facing up are not affected).
function M.setAmbientOcclusion( opts )
    opts = opts or {}
    for k, v in pairs( opts ) do ao[k] = v end
    if opts.enabled == nil then ao.enabled = true end
    sendAmbient()
end

-- ---------------------------------------------------------------------------
-- Focus area: outside a circle (in XZ) lit objects fade to gray / a tint, like a diorama with a highlighted part.
--   s3d.setFocus{ center = { 0, 0, 0 }, radius = 10, falloff = 5, saturation = 0, amount = 0.5, color = { 0.7, 0.7, 0.8 } }
--   s3d.setFocus( false )
local focus = { enabled = false, center = { 0, 0, 0 }, radius = 10, falloff = 5, saturation = 0, amount = 0.5, color = { 0.7, 0.7, 0.8 } }
function M.setFocus( opts )
    if opts == false then
        focus.enabled = false
    else
        opts = opts or {}
        for k, v in pairs( opts ) do focus[k] = v end
        if opts.enabled == nil then focus.enabled = true end
    end
    local c, col = v3( focus.center, { 0, 0, 0 } ), focus.color
    C.setFocus( focus.enabled, c[1], c[2], c[3], focus.radius, focus.falloff, focus.saturation, focus.amount, col[1], col[2], col[3] )
end

-- ---------------------------------------------------------------------------
-- Fog
local fog = { color = { 0.7, 0.8, 0.9 }, start = 10, ["end"] = 40, enabled = false }

-- s3d.setFog{ color = { r, g, b }, start = 10, ["end"] = 40 } ; s3d.setFog( false ) turns it off
function M.setFog( opts )
    if opts == false then
        fog.enabled = false
    else
        opts = opts or {}
        fog.color = opts.color or fog.color
        fog.start = opts.start or fog.start
        fog["end"] = opts["end"] or opts.finish or fog["end"]
        fog.enabled = opts.enabled ~= false
        fog.skyHorizon = opts.skyHorizon or fog.skyHorizon
    end
    local c = fog.color
    C.setFog( c[1], c[2], c[3], fog.start, fog["end"], fog.enabled, fog.skyHorizon or 0.15 )
end

-- ---------------------------------------------------------------------------
-- Planar shadows: s3d.setShadow{ planeY = 0, color = { 0, 0, 0, 0.35 } }; obj:castShadow( true )
local shadowColor = { 0, 0, 0, 0.35 }
function M.setShadow( opts )
    opts = opts or {}
    if opts.color then shadowColor = opts.color end
    C.setShadowPlane( opts.planeY or 0 )
end

-- Shadow map (shadows fall on every object, including the caster itself), one directional light:
--   s3d.setShadowMap{ enabled = true, size = 1024, area = 10, center = { 0, 0, 0 }, distance = 30, bias = 0.0015, darkness = 0.6 }
-- area: half the side of the shadowed square (world units); center: center of that square; distance: light to center.
-- Once enabled, obj:castShadow( true ) uses the shadow map instead of planar shadows.
local IS_MOBILE = ( function()
    local p = system.getInfo( "platform" )
    return p == "android" or p == "ios" or p == "tvos"
end )()
local SHADOW_QUALITY = { low = 1, medium = 2, high = 3, ultra = 4 }
-- Default: 2x2 PCF on mobile, 3x3 on desktop
local shadowMap = { enabled = false, size = 1024, area = 10, center = { 0, 0, 0 }, distance = 30, bias = 0.0015, darkness = 0.6, softness = 1,
    quality = IS_MOBILE and "medium" or "high" }

local shadowMode = "planar" -- or "map" after s3d.setShadowMap{ enabled = true }

function M.setShadowMap( opts )
    opts = opts or {}
    for k, v in pairs( opts ) do shadowMap[k] = v end
    -- "enabled" omitted: the first call turns shadows on, later calls keep the current state (so changing center/quality never re-enables shadows that were turned off)
    if opts.enabled == nil and not shadowMap.configured then shadowMap.enabled = true end
    shadowMap.configured = true
    shadowMode = shadowMap.enabled and "map" or "planar"
    local c = v3( shadowMap.center, { 0, 0, 0 } )
    local q = SHADOW_QUALITY[shadowMap.quality] or tonumber( shadowMap.quality ) or 3
    C.setShadowMap( shadowMap.enabled, shadowMap.size, shadowMap.area, c[1], c[2], c[3], shadowMap.distance, shadowMap.bias, shadowMap.darkness, q,
        shadowMap.softness )
end

function M.getShadowMode() return shadowMode end

M.isMobile = IS_MOBILE

-- ---------------------------------------------------------------------------
-- Post-processing: draw the scene into a snapshot, then apply filter.s3d.post
--   local post = s3d.newPostProcess{ clearColor = { 0.5, 0.7, 0.9 }, fxaa = true, exposure = 1.1, ... }
--   s3d.newCube( post.scene, 1 )
--   post:set{ saturation = 1.2, vignette = 0.4 }
local POST_DEFAULTS = {
    fxaa = true, exposure = 1, contrast = 1, saturation = 1, gamma = 1, brightness = 0,
    tint = { 1, 1, 1 }, vignette = 0, vignetteSoftness = 0.5, toneMap = "none",
}
local TONE_MAPS = { none = 0, aces = 1 }

function M.newPostProcess( opts )
    M.init()
    opts = opts or {}
    local w = opts.width or display.actualContentWidth
    local h = opts.height or display.actualContentHeight
    local snap = display.newSnapshot( w, h )
    if opts.parent then opts.parent:insert( snap ) end
    snap.x = opts.x or display.contentCenterX
    snap.y = opts.y or display.contentCenterY
    local cc = opts.clearColor or { 0, 0, 0, 1 }
    snap.clearColor = { cc[1], cc[2], cc[3], cc[4] or 1 }
    snap.fill.effect = M.EFFECT_POST

    local post = { snapshot = snap, scene = M.newScene(), settings = {} }
    snap.group:insert( post.scene )
    for k, v in pairs( POST_DEFAULTS ) do post.settings[k] = v end

    function post:set( o )
        local st = self.settings
        for k, v in pairs( o or {} ) do st[k] = v end
        local fx = self.snapshot.fill.effect
        fx.grade = { st.exposure, st.contrast, st.saturation, st.gamma }
        fx.tint = { st.tint[1], st.tint[2], st.tint[3], st.vignette }
        fx.options = { st.fxaa and 1 or 0, st.vignetteSoftness, st.brightness, TONE_MAPS[st.toneMap or "none"] or 0 }
    end

    function post:setEnabled( on )
        self.snapshot.fill.effect = on and M.EFFECT_POST or nil
        if on then self:set() end
    end

    local function onFrame() if snap.invalidate then snap:invalidate() end end
    Runtime:addEventListener( "enterFrame", onFrame )
    function post:removeSelf()
        Runtime:removeEventListener( "enterFrame", onFrame )
        display.remove( self.snapshot )
    end

    post:set( opts )
    return post
end

-- ---------------------------------------------------------------------------
-- Projection / rays (Solar2D content coordinates)
function M.worldToScreen( x, y, z )
    local nx, ny, nz = C.project( x, y, z )
    if not nx then return nil end
    local sx = display.screenOriginX + ( nx * 0.5 + 0.5 ) * display.actualContentWidth
    local sy = display.screenOriginY + ( 0.5 - ny * 0.5 ) * display.actualContentHeight
    return sx, sy, nz
end

function M.screenToRay( sx, sy )
    local nx = ( sx - display.screenOriginX ) / display.actualContentWidth * 2 - 1
    local ny = 1 - ( sy - display.screenOriginY ) / display.actualContentHeight * 2
    local ox, oy, oz, dx, dy, dz = C.unproject( nx, ny )
    return { ox, oy, oz }, { dx, dy, dz }
end

-- intersection of a ray with the plane y = h; returns x, y, z or nil
function M.rayPlaneY( origin, dir, h )
    h = h or 0
    if abs( dir[2] ) < 1e-6 then return nil end
    local t = ( h - origin[2] ) / dir[2]
    if t < 0 then return nil end
    return origin[1] + dir[1] * t, h, origin[3] + dir[3] * t
end

-- ---------------------------------------------------------------------------
-- Mesh building
local Builder = {}
Builder.__index = Builder

local function newBuilder()
    return setmetatable( { P = {}, U = {}, N = {} }, Builder )
end

function Builder:vertex( x, y, z, u, v, nx, ny, nz )
    local P, U, N = self.P, self.U, self.N
    P[#P + 1] = x; P[#P + 1] = y; P[#P + 1] = z
    U[#U + 1] = u; U[#U + 1] = v
    N[#N + 1] = nx; N[#N + 1] = ny; N[#N + 1] = nz
end

-- a, b, c = { x, y, z, u, v, nx, ny, nz }; winding is flipped when needed so the front face (CCW) faces the normal
function Builder:tri( a, b, c )
    local e1x, e1y, e1z = b[1] - a[1], b[2] - a[2], b[3] - a[3]
    local e2x, e2y, e2z = c[1] - a[1], c[2] - a[2], c[3] - a[3]
    local fx, fy, fz = e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x
    local nx, ny, nz = a[6] + b[6] + c[6], a[7] + b[7] + c[7], a[8] + b[8] + c[8]
    if fx * nx + fy * ny + fz * nz < 0 then b, c = c, b end
    self:vertex( unpack( a ) ); self:vertex( unpack( b ) ); self:vertex( unpack( c ) )
end

local function setTransform( obj )
    local p, r, s = obj.position3d, obj.rotation3d, obj.scale3d
    local q = obj.quat3d
    local key, data
    if q then
        -- quaternion rotation (setPose): reuse one table so no garbage is created every frame
        data = obj._s3dPoseBuf
        if not data then data = {}; obj._s3dPoseBuf = data end
        data[1], data[2], data[3] = p[1], p[2], p[3]
        data[4], data[5], data[6], data[7] = q[1], q[2], q[3], q[4]
        data[8], data[9], data[10] = s[1], s[2], s[3]
        key = "s3dPose"
    else
        data = { p[1], p[2], p[3], r[1], r[2], r[3], s[1], s[2], s[3] }
        key = "s3dTRS"
    end
    obj.fill.effect[key] = data
    local sh = obj._s3dShadow
    if sh and sh.fill and sh.fill.effect then sh.fill.effect[key] = data end
    local ca = obj._s3dCaster
    if ca and ca.fill and ca.fill.effect then ca.fill.effect[key] = data end
end

local methods = {}

function methods:moveTo( x, y, z ) local p = self.position3d; p[1], p[2], p[3] = x, y, z; setTransform( self ); return self end
function methods:moveBy( dx, dy, dz ) local p = self.position3d; p[1], p[2], p[3] = p[1] + dx, p[2] + dy, p[3] + dz; setTransform( self ); return self end
function methods:rotateTo( rx, ry, rz ) local r = self.rotation3d; r[1], r[2], r[3] = rx, ry, rz; self.quat3d = nil; setTransform( self ); return self end
function methods:rotateBy( dx, dy, dz ) local r = self.rotation3d; r[1], r[2], r[3] = r[1] + dx, r[2] + dy, r[3] + dz; self.quat3d = nil; setTransform( self ); return self end
-- Set position + quaternion rotation (x, y, z, w) in one call, e.g. from plugin.box3d:
--   obj:setPose( body:getTransform() )
-- After setPose, rotation3d is stale; call rotateTo to go back to Euler angles.
function methods:setPose( x, y, z, qx, qy, qz, qw )
    local p = self.position3d
    p[1], p[2], p[3] = x, y, z
    if qx then
        local q = self.quat3d
        if not q then q = {}; self.quat3d = q end
        q[1], q[2], q[3], q[4] = qx, qy, qz, qw
    end
    setTransform( self )
    return self
end
function methods:setRotationQuat( qx, qy, qz, qw )
    local q = self.quat3d
    if not q then q = {}; self.quat3d = q end
    q[1], q[2], q[3], q[4] = qx, qy, qz, qw
    setTransform( self )
    return self
end
function methods:scaleTo( sx, sy, sz ) sy = sy or sx; sz = sz or sx; local s = self.scale3d; s[1], s[2], s[3] = sx, sy, sz; setTransform( self ); return self end
function methods:setCulling( on ) self:_setFlag( 2, not on ) return self end
function methods:setDepthTest( on ) self:_setFlag( 4, not on ) return self end
function methods:setDepthWrite( on ) self:_setFlag( 8, not on ) return self end
function methods:_setFlag( bit, value )
    local f = self._s3dFlags or 0
    local has = ( f % ( bit * 2 ) ) >= bit
    if value and not has then f = f + bit elseif not value and has then f = f - bit end
    self._s3dFlags = f
    self.fill.effect.s3dFlags = f
end
-- Environment reflection. r = { texture = "sky.png" (equirectangular, required the first time), baseDir, strength = 0.5,
-- fresnel = 0.04 (F0: share reflected head-on; ~0.02 water, 0.04 plastic / glass, 0.5..0.9 metals), blur = 0 (mip bias:
-- needs a mipmapped texture), rotation = 0 (degrees, turns the environment around Y) }. false / nil turns it off.
-- The object's paint becomes composite (paint1 = its texture, paint2 = the environment).
function methods:setReflection( r )
    if not r then
        if self._s3dEnvOn then
            self._s3dEnv, self._s3dEnvOn = nil, nil
            if self._s3dTexture then self.fill = { type = "image", filename = self._s3dTexture, baseDir = self._s3dBaseDir }
            else self.fill = { 1, 1, 1 } end
            if self._s3dColor then self:setFillColor( unpack( self._s3dColor ) ) end
            self:setEffect( M.EFFECT_LIT )
        end
        return self
    end
    local tex = r.texture or self._s3dEnvTexture
    if not tex then error( "s3d: setReflection needs a texture (an equirectangular environment image)", 2 ) end
    if not self._s3dTexture then error( "s3d: setReflection needs an object made with opts.texture", 2 ) end
    self._s3dEnv = { r.strength or 0.5, r.fresnel or 0.04, r.blur or 0, math.rad( r.rotation or 0 ) }
    if tex ~= self._s3dEnvTexture or not self._s3dEnvOn then
        -- the environment wraps around horizontally: load it with repeat wrapping
        local wx = display.getDefault( "textureWrapX" )
        display.setDefault( "textureWrapX", "repeat" )
        self.fill = { type = "composite",
            paint1 = { type = "image", filename = self._s3dTexture, baseDir = self._s3dBaseDir },
            paint2 = { type = "image", filename = tex, baseDir = r.baseDir } }
        display.setDefault( "textureWrapX", wx )
        if self._s3dColor then self:setFillColor( unpack( self._s3dColor ) ) end
        self._s3dEnvTexture, self._s3dEnvOn = tex, true
        self:setEffect( M.EFFECT_REFLECT )
    else
        self.fill.effect.s3dEnv = self._s3dEnv
    end
    return self
end

-- uniforms each built-in effect has (so the plugin never writes a uniform that does not exist)
local EFFECT_USES = {
    ["filter.s3d.lit"] = { light = true, fog = true, shadow = true, style = true },
    ["filter.s3d.reflect"] = { light = true, fog = true, shadow = true, style = true, env = true },
    ["filter.s3d.unlit"] = { light = false, fog = true, shadow = false },
    ["filter.s3d.sky"] = { light = false, fog = true, shadow = false },
    ["filter.s3d.sprite"] = { light = false, fog = true, shadow = false },
    ["filter.s3d.spritelit"] = { light = true, fog = true, shadow = true, style = true },
    ["filter.s3d.litinst"] = { light = true, fog = true, shadow = true, style = true },
    ["filter.s3d.unlitinst"] = { light = false, fog = true, shadow = false },
    ["filter.s3d.shadow"] = { light = false, fog = true, shadow = false },
    ["filter.s3d.normals"] = { light = false, fog = false, shadow = false },
    ["filter.s3d.depth"] = { light = false, fog = false, shadow = false },
}

-- uses = { light = bool, fog = bool, shadow = bool, style = bool } for custom effects (default: light and fog on, shadow and style off)
function methods:setEffect( name, uses )
    local nodes = self._s3dNodes
    if nodes then name = NODE_VARIANT[name] or name end -- GPU-animated glTF part: the effect's node variant
    uses = uses or EFFECT_USES[name] or EFFECT_USES[NODE_BASE[name] or ""] or { light = true, fog = true, shadow = false }
    local f = self._s3dFlags or 0
    f = f % 32 -- keep flags 1..16
    if uses.light == false then f = f + 32 end
    if uses.fog == false then f = f + 64 end
    if not uses.shadow then f = f + 1024 end
    if uses.style then f = f + 2048 end
    if uses.env then f = f + 4096 end
    if self._s3dBillboard then f = f + self._s3dBillboard end
    self._s3dFlags = f
    self.fill.effect = name
    self.fill.effect.s3dNormals = self._s3dNormals
    if self._s3dAO then self.fill.effect.s3dAO = self._s3dAO end
    if self._s3dEnv then self.fill.effect.s3dEnv = self._s3dEnv end
    if self._s3dMorph then self.fill.effect.s3dMorph = self._s3dMorph end -- animated glTF part: keep its pose
    if nodes then self.fill.effect.s3dNodes = nodes.m end
    self.fill.effect.s3dFlags = f
    setTransform( self )
    return self
end


local function degenerateMesh()
    local m = display.newMesh{ mode = "triangles", vertices = { 0, 0, 0, 1, 1, 0, 1, 1, 0 }, uvs = { 0, 0, 0, 0, 0, 0 }, hasZ = true }
    local ox, oy = m.path:getVertexOffset()
    m.x, m.y = ox, oy
    return m
end

-- GPU-animated glTF parts (and their shadow copies): the node index of every vertex, + 0 for the degenerate triangle
local function fillNodeIndices( mesh, idx )
    local d = mesh.fillExtendedData
    if not d then return end
    local n = #idx
    for i = 1, n do d:setAttributeValue( i, "s3dNode", idx[i] ) end
    for i = n + 1, n + 3 do d:setAttributeValue( i, "s3dNode", 0 ) end
end

-- Shadow pass layer of a group: [begin marker, copies..., end marker], always at the bottom (drawn first)
local function casterLayer( parent )
    local layer = parent._s3dCasterLayer
    if layer then return layer end
    layer = display.newGroup()
    parent:insert( 1, layer )
    parent._s3dCasterLayer = layer
    local base = 2 + 4 + 32 + 64 + 1024 -- no cull, no depth, no light / fog / shadow uniforms
    local b = degenerateMesh(); layer:insert( b )
    b.fill.effect = M.EFFECT_DEPTH; b.fill.effect.s3dFlags = base + 256
    local e = degenerateMesh(); layer:insert( e )
    e.fill.effect = M.EFFECT_DEPTH; e.fill.effect.s3dFlags = base + 512
    layer._s3dEnd = e
    return layer
end

local function addCaster( obj )
    local parent = obj.parent
    if not parent or obj._s3dCaster then return end
    local layer = casterLayer( parent )
    local b = obj._s3dGeom
    local c = display.newMesh{ mode = "triangles", vertices = b.P, uvs = b.U, hasZ = true }
    layer:insert( layer.numChildren, c ) -- right before the end marker
    local ox, oy = c.path:getVertexOffset()
    c.x, c.y = ox, oy
    if obj._s3dInst then c.fillExtension = INSTANCE_EXTENSION end
    local nodes = obj._s3dNodes
    if nodes then c.fillExtension = NODE_EXTENSION end
    if ( obj._s3dBillboard or obj._s3dAlphaCut ) and obj._s3dTexture then
        -- sprites and alpha-masked models: the caster needs the image (alpha cut-out) and, for sprites, the current frame's UVs
        c.fill = { type = "image", filename = obj._s3dTexture, baseDir = obj._s3dBaseDir }
        c.fill.effect = nodes and NODE_VARIANT[M.EFFECT_DEPTH_SPRITE] or M.EFFECT_DEPTH_SPRITE
        c.fill.effect.s3dFlags = 2 + 32 + 64 + 1024 + 128 + ( obj._s3dBillboard or 0 )
    else
        c.fill.effect = obj._s3dInst and M.EFFECT_DEPTH_INST or nodes and NODE_VARIANT[M.EFFECT_DEPTH] or M.EFFECT_DEPTH
        c.fill.effect.s3dFlags = 2 + 32 + 64 + 1024 + 128 -- no cull, shadow pass copy
    end
    if nodes then
        fillNodeIndices( c, nodes.idx )
        c.fill.effect.s3dMorph = obj._s3dMorph
        c.fill.effect.s3dNodes = nodes.m
    end
    obj._s3dCaster = c
    if obj._s3dBillboard and obj._s3dUV then obj:_applyUV( c ) end
    if obj._s3dInst then obj:_copyInstancesTo( c ) end
end

local function hookFinalize( obj )
    if obj._s3dFinalizeHooked then return end
    obj._s3dFinalizeHooked = true
    obj:addEventListener( "finalize", function()
        if obj._s3dShadow then display.remove( obj._s3dShadow ); obj._s3dShadow = nil end
        if obj._s3dCaster then display.remove( obj._s3dCaster ); obj._s3dCaster = nil end
    end )
end

function methods:castShadow( on )
    if on == false then
        if self._s3dShadow then display.remove( self._s3dShadow ); self._s3dShadow = nil end
        if self._s3dCaster then display.remove( self._s3dCaster ); self._s3dCaster = nil end
        return self
    end
    if not self._s3dGeom then return self end
    if self._s3dBlend then return self end -- see-through parts (glTF BLEND) do not cast shadows
    local parent = self.parent
    if not parent then return self end
    if shadowMode == "map" then
        addCaster( self )
        hookFinalize( self )
        setTransform( self )
        return self
    end
    if self._s3dShadow then return self end
    if self._s3dInst then return self end -- planar shadows are projected on the CPU: not available for instances
    if self._s3dBillboard then return self end -- sprites: shadow map mode only (planar shadows would be solid quads)
    local layer = parent._s3dShadowLayer
    if not layer then
        layer = display.newGroup()
        parent:insert( layer )
        parent._s3dShadowLayer = layer
    end
    local b = self._s3dGeom
    local sh = display.newMesh{ mode = "triangles", vertices = b.P, uvs = b.U, hasZ = true }
    layer:insert( sh )
    layer:toFront()
    local ox, oy = sh.path:getVertexOffset()
    sh.x, sh.y = ox, oy
    sh:setFillColor( unpack( shadowColor ) )
    local nodes = self._s3dNodes
    if nodes then sh.fillExtension = NODE_EXTENSION end
    sh.fill.effect = nodes and NODE_VARIANT[M.EFFECT_SHADOW] or M.EFFECT_SHADOW
    -- projected shadow + no light uniforms. Culling stays on: only triangles facing the light remain after projection,
    -- so convex objects never get two layers of shadow.
    sh.fill.effect.s3dFlags = 16 + 32
    if nodes then
        -- GPU-animated glTF part: the projection is folded into the node matrices (s3d.c)
        fillNodeIndices( sh, nodes.idx )
        sh.fill.effect.s3dMorph = self._s3dMorph
        sh.fill.effect.s3dNodes = nodes.m
    end
    self._s3dShadow = sh
    hookFinalize( self )
    setTransform( self )
    sh.isVisible = self.isVisible
    return self
end

local function finalize( parent, b, opts )
    opts = opts or {}
    -- degenerate triangle at the origin: keeps the object's 2D bounds around (0, 0) so Solar2D does not cull it by mistake
    if not opts._hasDegenerate then
        b:vertex( 0, 0, 0, 0, 0, 0, 0, 1 ); b:vertex( 1, 1, 0, 0, 0, 0, 0, 1 ); b:vertex( 1, 1, 0, 0, 0, 0, 0, 1 )
    end

    local mesh = display.newMesh{ mode = "triangles", vertices = b.P, uvs = b.U, hasZ = true }
    if not mesh then error( "s3d: display.newMesh failed", 3 ) end
    if opts._extension then mesh.fillExtension = opts._extension end -- before any effect that uses it
    if parent then
        parent:insert( mesh )
        -- see-through parts (glTF BLEND) are drawn after the opaque objects, then the planar shadows
        if parent._s3dBlendLayer then parent._s3dBlendLayer:toFront() end
        if parent._s3dShadowLayer then parent._s3dShadowLayer:toFront() end -- planar shadows are always drawn last
    end
    mesh._s3dGeom = b

    -- newMesh recenters the 2D bounds; undo that so vertex coordinates stay as given
    local ox, oy = mesh.path:getVertexOffset()
    mesh.x, mesh.y = ox, oy

    if opts.texture then
        mesh.fill = { type = "image", filename = opts.texture, baseDir = opts.baseDir }
    end
    if opts.color then mesh:setFillColor( unpack( opts.color ) ) end
    mesh._s3dTexture, mesh._s3dBaseDir, mesh._s3dColor = opts.texture, opts.baseDir, opts.color

    for k, f in pairs( methods ) do mesh[k] = f end
    mesh.position3d = { 0, 0, 0 }
    mesh.rotation3d = { 0, 0, 0 }
    mesh.scale3d = { 1, 1, 1 }
    mesh._s3dNormals = C.newBuffer( b.N )
    mesh._s3dFlags = 0
    mesh.vertexCount3d = #b.P / 3 - 3

    -- ambient occlusion: OBJ models when s3d.setAmbientOcclusion is on (opts.ao overrides), cached per file + scale
    local wantAO = opts.ao
    if wantAO == nil then wantAO = ao.enabled and opts._aoKey ~= nil end
    if opts._ao then
        mesh._s3dAO = opts._ao
    elseif wantAO and M.supported then
        local dist = opts.aoDistance or ao.distance
        local key = opts._aoKey and ( opts._aoKey .. "|" .. dist .. "|" .. ao.rays )
        local buf = key and aoCache[key]
        if not buf then
            buf = C.bakeAO( C.newBuffer( b.P ), mesh._s3dNormals, ao.rays, dist )
            if key then aoCache[key] = buf end
        end
        mesh._s3dAO = buf
    end

    if opts._preEffect then opts._preEffect( mesh ) end
    mesh:setEffect( opts.effect or ( opts.unlit and M.EFFECT_UNLIT ) or M.EFFECT_LIT )
    if opts.reflection then mesh:setReflection( opts.reflection ) end
    if not M.supported then mesh.isVisible = false end
    if opts.cull == false then mesh:setCulling( false ) end
    if opts.castShadow then mesh:castShadow( true ) end
    if opts.x or opts.y or opts.z then mesh:moveTo( opts.x or 0, opts.y or 0, opts.z or 0 ) end
    return mesh
end

-- ---------------------------------------------------------------------------
-- Primitive
local FACES = {
    { n = {  1, 0, 0 }, u = { 0, 0, -1 }, v = { 0, 1, 0 } },
    { n = { -1, 0, 0 }, u = { 0, 0,  1 }, v = { 0, 1, 0 } },
    { n = { 0,  1, 0 }, u = { 1, 0, 0 }, v = { 0, 0, -1 } },
    { n = { 0, -1, 0 }, u = { 1, 0, 0 }, v = { 0, 0,  1 } },
    { n = { 0, 0,  1 }, u = { 1, 0, 0 }, v = { 0, 1, 0 } },
    { n = { 0, 0, -1 }, u = { -1, 0, 0 }, v = { 0, 1, 0 } },
}

function M.newCube( parent, size, opts )
    M.init()
    local h = ( size or 1 ) * 0.5
    local b = newBuilder()
    for _, f in ipairs( FACES ) do
        local n, u, v = f.n, f.u, f.v
        local function corner( a, c )
            local su, sv = a * 2 - 1, c * 2 - 1
            return { h * ( n[1] + su * u[1] + sv * v[1] ), h * ( n[2] + su * u[2] + sv * v[2] ), h * ( n[3] + su * u[3] + sv * v[3] ),
                a, 1 - c, n[1], n[2], n[3] }
        end
        local p00, p10, p11, p01 = corner( 0, 0 ), corner( 1, 0 ), corner( 1, 1 ), corner( 0, 1 )
        b:tri( p00, p10, p11 ); b:tri( p00, p11, p01 )
    end
    return finalize( parent, b, opts )
end

function M.newSphere( parent, radius, segments, rings, opts )
    M.init()
    radius, segments, rings = radius or 0.5, segments or 24, rings or 16
    local b = newBuilder()
    local function pt( i, j )
        local th, ph = pi * i / rings, 2 * pi * j / segments
        local nx, ny, nz = sin( th ) * sin( ph ), cos( th ), sin( th ) * cos( ph )
        return { radius * nx, radius * ny, radius * nz, j / segments, i / rings, nx, ny, nz }
    end
    for i = 0, rings - 1 do
        for j = 0, segments - 1 do
            local a, c, d, e = pt( i, j ), pt( i + 1, j ), pt( i + 1, j + 1 ), pt( i, j + 1 )
            if i > 0 then b:tri( a, c, e ) end
            if i < rings - 1 then b:tri( e, c, d ) end
        end
    end
    return finalize( parent, b, opts )
end

-- Sky dome: a sphere seen from inside, textured with an equirectangular image (the same mapping as setReflection, so
-- reflections line up with it), unlit and without fog, drawn first and without depth writes so everything else covers
-- it, and kept centered on the camera. opts: radius (default 90% of the camera's far plane), segments (48), rings (24),
-- baseDir, color (tint). Remove it with display.remove( sky ).
function M.newSky( parent, texture, opts )
    M.init()
    opts = opts or {}
    local radius = opts.radius or camera.far * 0.9
    local sky = M.newSphere( parent, radius, opts.segments or 48, opts.rings or 24,
        { texture = texture, baseDir = opts.baseDir, color = opts.color, effect = M.EFFECT_SKY, cull = false } )
    sky:setDepthWrite( false )
    if parent and parent.insert then parent:insert( 1, sky ) end -- drawn before everything else in the group
    local p = camera.position
    sky:moveTo( p[1], p[2], p[3] )
    M._skies[sky] = true
    return sky
end

function M.newPlane( parent, width, depth, opts )
    M.init()
    opts = opts or {}
    local w, d = ( width or 1 ) * 0.5, ( depth or width or 1 ) * 0.5
    local rep = opts.uvRepeat or 1
    local b = newBuilder()
    local p00 = { -w, 0,  d, 0, rep, 0, 1, 0 }
    local p10 = {  w, 0,  d, rep, rep, 0, 1, 0 }
    local p11 = {  w, 0, -d, rep, 0, 0, 1, 0 }
    local p01 = { -w, 0, -d, 0, 0, 0, 1, 0 }
    b:tri( p00, p10, p11 ); b:tri( p00, p11, p01 )
    return finalize( parent, b, opts )
end

-- Capsule along the Y axis: a segment of length `height` between the two hemisphere centers (total height = height + 2 * radius)
function M.newCapsule( parent, radius, height, segments, rings, opts )
    M.init()
    radius, height, segments, rings = radius or 0.25, height or 1, segments or 20, rings or 6
    local h = height * 0.5
    local b = newBuilder()
    -- rings from the top pole to the bottom pole; the equator ring is repeated (+h and -h) to form the cylinder
    local rows = {}
    for k = 0, rings do rows[#rows + 1] = { pi * 0.5 * k / rings, h } end
    for k = rings, 2 * rings do rows[#rows + 1] = { pi * 0.5 * k / rings, -h } end
    local total = height + 2 * radius
    local function pt( r, j )
        local th, off = rows[r][1], rows[r][2]
        local ph = 2 * pi * j / segments
        local nx, ny, nz = sin( th ) * sin( ph ), cos( th ), sin( th ) * cos( ph )
        local y = radius * ny + off
        return { radius * nx, y, radius * nz, j / segments, 0.5 - y / total, nx, ny, nz }
    end
    for r = 1, #rows - 1 do
        for j = 0, segments - 1 do
            local a, c, d, e = pt( r, j ), pt( r + 1, j ), pt( r + 1, j + 1 ), pt( r, j + 1 )
            if r > 1 then b:tri( a, c, e ) end
            if r < #rows - 1 then b:tri( e, c, d ) end
        end
    end
    return finalize( parent, b, opts )
end

function M.newCylinder( parent, radius, height, segments, opts )
    M.init()
    radius, height, segments = radius or 0.5, height or 1, segments or 24
    local h = height * 0.5
    local b = newBuilder()
    for j = 0, segments - 1 do
        local a0, a1 = 2 * pi * j / segments, 2 * pi * ( j + 1 ) / segments
        local x0, z0, x1, z1 = sin( a0 ), cos( a0 ), sin( a1 ), cos( a1 )
        local u0, u1 = j / segments, ( j + 1 ) / segments
        local b0 = { radius * x0, -h, radius * z0, u0, 1, x0, 0, z0 }
        local b1 = { radius * x1, -h, radius * z1, u1, 1, x1, 0, z1 }
        local t0 = { radius * x0, h, radius * z0, u0, 0, x0, 0, z0 }
        local t1 = { radius * x1, h, radius * z1, u1, 0, x1, 0, z1 }
        b:tri( b0, b1, t1 ); b:tri( b0, t1, t0 )
        -- caps
        b:tri( { 0, h, 0, 0.5, 0.5, 0, 1, 0 }, { radius * x0, h, radius * z0, 0.5 + x0 * 0.5, 0.5 - z0 * 0.5, 0, 1, 0 },
            { radius * x1, h, radius * z1, 0.5 + x1 * 0.5, 0.5 - z1 * 0.5, 0, 1, 0 } )
        b:tri( { 0, -h, 0, 0.5, 0.5, 0, -1, 0 }, { radius * x0, -h, radius * z0, 0.5 + x0 * 0.5, 0.5 + z0 * 0.5, 0, -1, 0 },
            { radius * x1, -h, radius * z1, 0.5 + x1 * 0.5, 0.5 + z1 * 0.5, 0, -1, 0 } )
    end
    return finalize( parent, b, opts )
end

-- ---------------------------------------------------------------------------
-- Dynamic meshes: change the vertices every frame (water, terrain sculpting, cloth, soft bodies...).
-- The plugin keeps the vertex list in C; after an edit it rebuilds the triangle list and the normals in C and hands them to
-- the geometry writers as a morph source, so Lua only touches the vertices it changes. Shadow copies follow automatically.
local dynMethods = {}
local dirtyMeshes = setmetatable( {}, { __mode = "k" } )

local function dynApply( obj )
    local d = obj._s3dDyn
    local buf = d.morph
    local function put( o ) if o and o.fill and o.fill.effect then o.fill.effect.s3dMorph = buf end end
    put( obj ); put( obj._s3dCaster ); put( obj._s3dShadow )
end

local function dynPush( obj )
    local d = obj._s3dDyn
    C.dynUpdate( d.dyn, d.flat )
    dirtyMeshes[obj] = nil
    dynApply( obj )
end

-- called at the start of every frame (s3d.init): meshes edited with setVertex / setHeight since the last frame
local function flushDynamic()
    for obj in pairs( dirtyMeshes ) do
        if obj._s3dDyn and obj.fill then dynPush( obj ) else dirtyMeshes[obj] = nil end
    end
end
M._flushDynamic = flushDynamic

-- single vertices: applied by updateMesh(), or automatically at the start of the next frame
function dynMethods:setVertex( i, x, y, z ) C.dynSetVertex( self._s3dDyn.dyn, i, x, y, z ); dirtyMeshes[self] = true; return self end
function dynMethods:getVertex( i ) return C.dynGetVertex( self._s3dDyn.dyn, i ) end
-- many vertices at once ({ x, y, z, ... } starting at vertex `first`, default 1): applied immediately
function dynMethods:setVertices( list, first ) C.dynSetPositions( self._s3dDyn.dyn, list, first ); dynPush( self ); return self end
function dynMethods:updateMesh() dynPush( self ); return self end
function dynMethods:setNormals( mode ) self._s3dDyn.flat = ( mode == "flat" ); dynPush( self ); return self end
-- shadow copies and effect changes replace the effect instance: give the new ones the current shape
function dynMethods:castShadow( on ) methods.castShadow( self, on ); dynApply( self ); return self end
function dynMethods:setEffect( name, uses ) methods.setEffect( self, name, uses ); dynApply( self ); return self end

-- height fields (s3d.newGrid): column i = 0..cols (along +x), row j = 0..rows (along +z)
local gridMethods = {}
local function gridIndex( g, i, j ) return j * ( g.cols + 1 ) + i + 1 end
-- heights: { h, h, ... } row by row ((cols + 1) * (rows + 1) values, x fastest); optional scale and offset; applied immediately
function gridMethods:setHeights( heights, scale, offset ) C.dynSetHeights( self._s3dDyn.dyn, heights, scale, offset ); dynPush( self ); return self end
function gridMethods:setHeight( i, j, h ) C.dynSetVertex( self._s3dDyn.dyn, gridIndex( self, i, j ), nil, h, nil ); dirtyMeshes[self] = true; return self end
function gridMethods:getHeight( i, j ) local _, y = C.dynGetVertex( self._s3dDyn.dyn, gridIndex( self, i, j ) ); return y end
-- height at a point in the grid's own coordinates (bilinear between the 4 nearest vertices), nil outside
function gridMethods:heightAt( x, z )
    local fx = ( x / self.gridWidth + 0.5 ) * self.cols
    local fz = ( z / self.gridDepth + 0.5 ) * self.rows
    if fx < 0 or fz < 0 or fx > self.cols or fz > self.rows then return nil end
    local i, j = math.min( math.floor( fx ), self.cols - 1 ), math.min( math.floor( fz ), self.rows - 1 )
    local tx, tz = fx - i, fz - j
    local h00, h10 = self:getHeight( i, j ), self:getHeight( i + 1, j )
    local h01, h11 = self:getHeight( i, j + 1 ), self:getHeight( i + 1, j + 1 )
    return ( h00 * ( 1 - tx ) + h10 * tx ) * ( 1 - tz ) + ( h01 * ( 1 - tx ) + h11 * tx ) * tz
end
-- the vertex (column, row) nearest to a point in the grid's own coordinates, nil outside
function gridMethods:cellAt( x, z )
    local fx = ( x / self.gridWidth + 0.5 ) * self.cols
    local fz = ( z / self.gridDepth + 0.5 ) * self.rows
    if fx < -0.5 or fz < -0.5 or fx > self.cols + 0.5 or fz > self.rows + 0.5 then return nil end
    return math.max( 0, math.min( self.cols, math.floor( fx + 0.5 ) ) ), math.max( 0, math.min( self.rows, math.floor( fz + 0.5 ) ) )
end

local function newDynamic( parent, vertices, uvs, indices, opts, level )
    local dyn, outP, outN = C.newDynamic( vertices, indices )
    local b = newBuilder()
    local P, U, N = b.P, b.U, b.N
    local count = indices and ( #indices - #indices % 3 ) or ( #vertices / 3 - ( #vertices / 3 ) % 3 )
    for e = 1, count do
        local k = indices and indices[e] or e
        local p, t = 3 * ( k - 1 ), 2 * ( k - 1 )
        local n = #P
        P[n + 1], P[n + 2], P[n + 3] = vertices[p + 1], vertices[p + 2], vertices[p + 3]
        local m = #U
        U[m + 1], U[m + 2] = uvs and uvs[t + 1] or 0, uvs and uvs[t + 2] or 0
        N[n + 1], N[n + 2], N[n + 3] = 0, 1, 0
    end
    local o = {}
    for k, v in pairs( opts ) do o[k] = v end
    o.ao = false -- the shape changes: no baked occlusion (the shader's darkening near the floor still applies)
    local obj = finalize( parent, b, o )
    for k, f in pairs( dynMethods ) do obj[k] = f end
    obj.meshVertexCount = #vertices / 3
    obj._s3dDyn = { dyn = dyn, outP = outP, outN = outN, flat = ( opts.normals == "flat" ), morph = { outP, outP, 0, outN, outN } }
    dynPush( obj )
    return obj
end

-- opts.vertices = { x, y, z, ... }; opts.uvs = { u, v, ... } (optional); opts.indices = { 1, 2, 3, ... } (optional, 1-based,
-- three per triangle, front face counter-clockwise); opts.normals = "smooth" (default) or "flat"; plus the usual options
-- (texture, color, castShadow, cull, effect...)
function M.newMesh( parent, opts )
    M.init()
    opts = opts or {}
    if type( opts.vertices ) ~= "table" then error( "s3d.newMesh: opts.vertices (table of x, y, z) is required", 2 ) end
    return newDynamic( parent, opts.vertices, opts.uvs, opts.indices, opts, 2 )
end

-- Height field on the XZ plane, centered on the origin: cols x rows cells (default 32 x 32), (cols + 1) x (rows + 1) vertices.
-- opts.heights: table (row by row, x fastest) or function( x, z, i, j ) -> height; opts.uvRepeat like newPlane.
function M.newGrid( parent, width, depth, cols, rows, opts )
    M.init()
    opts = opts or {}
    width = width or 1; depth = depth or width
    cols = math.max( 1, math.floor( cols or 32 ) ); rows = math.max( 1, math.floor( rows or cols ) )
    local rep = opts.uvRepeat or 1
    local hs = opts.heights
    local V, UV, I = {}, {}, {}
    for j = 0, rows do
        for i = 0, cols do
            local x, z = ( i / cols - 0.5 ) * width, ( j / rows - 0.5 ) * depth
            local h = 0
            if type( hs ) == "function" then h = hs( x, z, i, j ) or 0
            elseif type( hs ) == "table" then h = hs[j * ( cols + 1 ) + i + 1] or 0 end
            V[#V + 1] = x; V[#V + 1] = h; V[#V + 1] = z
            UV[#UV + 1] = i / cols * rep; UV[#UV + 1] = j / rows * rep
        end
    end
    local stride = cols + 1
    for j = 0, rows - 1 do
        for i = 0, cols - 1 do
            local a = j * stride + i + 1
            local b, c, d = a + stride, a + 1, a + stride + 1
            I[#I + 1] = a; I[#I + 1] = b; I[#I + 1] = c
            I[#I + 1] = c; I[#I + 1] = b; I[#I + 1] = d
        end
    end
    local obj = newDynamic( parent, V, UV, I, opts, 2 )
    for k, f in pairs( gridMethods ) do obj[k] = f end
    obj.cols, obj.rows, obj.gridWidth, obj.gridDepth = cols, rows, width, depth
    return obj
end


-- ---------------------------------------------------------------------------
-- Instances: many copies of one model in a single draw call (GPU instancing).
-- s3d.newInstances( parent, model, count, opts ): model = any s3d object (its geometry, texture, color and baked AO are
-- used; the model itself is not changed, hide or remove it if you only need the copies). opts: castShadow (shadow map
-- mode only), cull, unlit. Instances start hidden (scale 0); place them with setInstance.
-- The object itself stays at the origin: positions given to setInstance are world coordinates.
-- Without GPU instancing (s3d.instancing == false) the same API makes count ordinary objects instead.
local instMethods = {}

local function quatFromEuler( rx, ry, rz )
    -- same rotation as rotateTo( rx, ry, rz ) (degrees): q = qy * qx * qz, i.e. Z first, then X, then Y
    local hx, hy, hz = math.rad( rx or 0 ) * 0.5, math.rad( ry or 0 ) * 0.5, math.rad( rz or 0 ) * 0.5
    local sx, cx, sy, cy, sz, cz = math.sin( hx ), math.cos( hx ), math.sin( hy ), math.cos( hy ), math.sin( hz ), math.cos( hz )
    -- qy * qx = ( cy sx, sy cx, -sy sx, cy cx )
    local ax, ay, az, aw = cy * sx, sy * cx, -sy * sx, cy * cx
    -- ( qy * qx ) * qz, qz = ( 0, 0, sz, cz )
    return ax * cz + ay * sz, ay * cz - ax * sz, aw * sz + az * cz, aw * cz - az * sz
end
M.quatFromEuler = quatFromEuler

-- what: 1 = position / scale, 2 = rotation, 4 = color (sum), nil = all; each attribute is one engine call
local function writeInstance( target, i, d, what )
    local ext = target.fillExtendedData
    what = what or 7
    if what % 2 == 1 then ext:setAttributeValue( i, "s3dInstPose", d[1], d[2], d[3], d[4] ) end
    if what % 4 >= 2 then ext:setAttributeValue( i, "s3dInstRot", d[5], d[6], d[7], d[8] ) end
    if what >= 4 then ext:setAttributeValue( i, "s3dInstColor", d[9], d[10], d[11], d[12] ) end
end

function instMethods:_copyInstancesTo( target )
    target.fillExtendedData.instances = self.instanceCount
    for i = 1, self.instanceCount do writeInstance( target, i, self._s3dInstData[i] ) end
end

local function instanceData( self, i )
    if i < 1 or i > self.instanceCount then error( "s3d: instance " .. tostring( i ) .. " out of range (1.." .. self.instanceCount .. ")", 3 ) end
    return self._s3dInstData[i]
end

local function flushInstance( self, i, d, what )
    writeInstance( self, i, d, what )
    if self._s3dCaster and what ~= 4 then writeInstance( self._s3dCaster, i, d, what ) end -- casters ignore color
end

-- position, rotation (degrees, like rotateTo) and uniform scale of instance i
function instMethods:setInstance( i, x, y, z, rx, ry, rz, scale )
    local d = instanceData( self, i )
    d[1], d[2], d[3], d[4] = x or d[1], y or d[2], z or d[3], scale or ( d[4] ~= 0 and d[4] ) or 1
    local what = 1
    if rx or ry or rz then d[5], d[6], d[7], d[8] = quatFromEuler( rx, ry, rz ); what = 3 end
    flushInstance( self, i, d, what )
    return self
end

-- position, rotation quaternion (e.g. from a physics engine) and uniform scale of instance i
function instMethods:setInstancePose( i, x, y, z, qx, qy, qz, qw, scale )
    local d = instanceData( self, i )
    d[1], d[2], d[3], d[4] = x, y, z, scale or ( d[4] ~= 0 and d[4] ) or 1
    d[5], d[6], d[7], d[8] = qx or 0, qy or 0, qz or 0, qw or 1
    flushInstance( self, i, d, 3 )
    return self
end

-- color of instance i, multiplied with the model's texture and fill color (alpha 0 hides it)
function instMethods:setInstanceColor( i, r, g, b, a )
    local d = instanceData( self, i )
    d[9], d[10], d[11], d[12] = r or 1, g or 1, b or 1, a or 1
    flushInstance( self, i, d, 4 )
    return self
end

function instMethods:getInstance( i )
    local d = instanceData( self, i )
    return d[1], d[2], d[3], d[4]
end

-- hide instance i (scale 0) without changing the others
function instMethods:hideInstance( i )
    local d = instanceData( self, i )
    d[4] = 0
    flushInstance( self, i, d, 1 )
    return self
end

-- fallback without GPU instancing: one ordinary object per instance, same methods
local fallbackMethods = {}
function fallbackMethods:setInstance( i, x, y, z, rx, ry, rz, scale )
    local o = self._s3dCopies[i]
    if not o then error( "s3d: instance " .. tostring( i ) .. " out of range", 2 ) end
    o.isVisible = true
    o:moveTo( x or o.position3d[1], y or o.position3d[2], z or o.position3d[3] )
    if rx or ry or rz then o:rotateTo( rx or 0, ry or 0, rz or 0 ) end
    if scale then o:scaleTo( scale, scale, scale ) end
    return self
end
function fallbackMethods:setInstancePose( i, x, y, z, qx, qy, qz, qw, scale )
    local o = self._s3dCopies[i]
    o.isVisible = true
    o:setPose( x, y, z, qx, qy, qz, qw )
    if scale then o:scaleTo( scale, scale, scale ) end
    return self
end
function fallbackMethods:setInstanceColor( i, r, g, b, a )
    local o = self._s3dCopies[i]
    o:setFillColor( r or 1, g or 1, b or 1, a or 1 )
    return self
end
function fallbackMethods:getInstance( i )
    local o = self._s3dCopies[i]
    return o.position3d[1], o.position3d[2], o.position3d[3], o.scale3d[1]
end
function fallbackMethods:hideInstance( i ) self._s3dCopies[i].isVisible = false; return self end

local function copyBuilder( src )
    local b = newBuilder()
    for k = 1, #src.P do b.P[k] = src.P[k] end
    for k = 1, #src.U do b.U[k] = src.U[k] end
    for k = 1, #src.N do b.N[k] = src.N[k] end
    return b
end

function M.newInstances( parent, model, count, opts )
    M.init()
    opts = opts or {}
    if type( model ) ~= "table" or not model._s3dGeom then error( "s3d.newInstances: model must be an s3d object", 2 ) end
    count = math.max( 1, math.floor( count or 1 ) )
    local o = {
        texture = model._s3dTexture, baseDir = model._s3dBaseDir, color = model._s3dColor,
        cull = opts.cull, _hasDegenerate = true, _ao = model._s3dAO,
    }

    if not M.instancing then
        -- one object per instance (same geometry and AO), hidden until placed
        local group = display.newGroup()
        if parent then parent:insert( group ) end
        group._s3dCopies = {}
        for i = 1, count do
            o.effect = opts.unlit and M.EFFECT_UNLIT or M.EFFECT_LIT
            local c = finalize( group, copyBuilder( model._s3dGeom ), o )
            if opts.castShadow then c:castShadow( true ) end
            c.isVisible = false
            group._s3dCopies[i] = c
        end
        for k, f in pairs( fallbackMethods ) do group[k] = f end
        group.instanceCount, group.instanced = count, false
        return group
    end

    o._extension = INSTANCE_EXTENSION
    o.effect = opts.unlit and M.EFFECT_UNLIT_INST or M.EFFECT_LIT_INST
    local obj = finalize( parent, copyBuilder( model._s3dGeom ), o )
    for k, f in pairs( instMethods ) do obj[k] = f end
    obj._s3dInst = true
    obj.instanceCount, obj.instanced = count, true
    obj._s3dInstData = {}
    obj.fillExtendedData.instances = count
    for i = 1, count do
        local d = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1 } -- scale 0: hidden until placed
        obj._s3dInstData[i] = d
        writeInstance( obj, i, d )
    end
    if opts.castShadow then obj:castShadow( true ) end
    return obj
end


-- ---------------------------------------------------------------------------
-- Sprites: textured quads in the 3D scene that face the camera (billboards), with frame animation from an image sheet.
-- s3d.newSprite( parent, filename, opts ): opts.width / height (world units, default 1 x 1 or the frame's aspect),
-- anchorX / anchorY (0..1, Solar2D style: 0 = left / top; default 0.5, 1 = feet on the ground), billboard = "upright"
-- (default: turns around Y only) | "full" (faces the camera completely) | false (a fixed quad facing +Z, use rotateTo),
-- sheet (image sheet, see below), sequences, lit (false: unlit; true: lit and shadowed), castShadow (shadow map mode),
-- color, baseDir. Pixels below 50% alpha are cut out, so sprites need no sorting.
-- sheet: { columns = 8, rows = 4 } (a grid, frames numbered row by row) or Solar2D's graphics.newImageSheet options
-- { frames = { { x, y, width, height }, ... }, sheetContentWidth = W, sheetContentHeight = H }.
-- sequences: Solar2D style, { name = "walk", start = 1, count = 8, time = 800, loopCount = 0, loopDirection = "forward" }
-- or { name = "idle", frames = { 9, 10, 9, 11 }, time = 600 }; loopCount 0 = forever, loopDirection "bounce" too.
local spriteMethods = {}
local playingSprites = setmetatable( {}, { __mode = "k" } )

local function sheetFrames( sheet )
    local frames = {}
    if not sheet then
        frames[1] = { 0, 0, 1, 1, 1 }
        return frames
    end
    if sheet.frames then
        local W, H = sheet.sheetContentWidth, sheet.sheetContentHeight
        if not W or not H then error( "s3d.newSprite: sheet.frames needs sheetContentWidth and sheetContentHeight", 3 ) end
        for i, f in ipairs( sheet.frames ) do
            frames[i] = { f.x / W, f.y / H, ( f.x + f.width ) / W, ( f.y + f.height ) / H, f.width / f.height }
        end
    else
        local cols, rows = sheet.columns or sheet.cols or 1, sheet.rows or 1
        local n = sheet.count or sheet.numFrames or cols * rows
        local aspect = sheet.aspect or 1
        for i = 1, n do
            local c, r = ( i - 1 ) % cols, math.floor( ( i - 1 ) / cols )
            frames[i] = { c / cols, r / rows, ( c + 1 ) / cols, ( r + 1 ) / rows, aspect }
        end
    end
    return frames
end

-- vertex order of the quad (see newSprite): top-left, bottom-left, bottom-right, top-left, bottom-right, top-right
local QUAD_U = { 1, 1, 3, 1, 3, 3 }
local QUAD_V = { 2, 4, 4, 2, 4, 2 }

function spriteMethods:_applyUV( target )
    local uv = self._s3dUV
    local u0, v0, u1, v1 = uv[1], uv[2], uv[3], uv[4]
    if self._s3dFlipX then u0, u1 = u1, u0 end
    if self._s3dFlipY then v0, v1 = v1, v0 end
    local q = { u0, v0, u1, v1 }
    local path = ( target or self ).path
    for i = 1, 6 do path:setUV( i, q[QUAD_U[i]], q[QUAD_V[i]] ) end
end

local function showFrame( self, index )
    local f = self._s3dFrames[index]
    if not f then error( "s3d: sprite frame " .. tostring( index ) .. " out of range (1.." .. #self._s3dFrames .. ")", 3 ) end
    self._s3dUV = f
    self:_applyUV()
    if self._s3dCaster then self:_applyUV( self._s3dCaster ) end
end

-- show a frame of the image sheet directly (stops the animation)
function spriteMethods:setSheetFrame( index )
    self:pause()
    showFrame( self, index )
    self.sheetFrame = index
    return self
end

function spriteMethods:setFlip( x, y )
    self._s3dFlipX, self._s3dFlipY = x and true or false, y and true or false
    self:_applyUV()
    if self._s3dCaster then self:_applyUV( self._s3dCaster ) end
    return self
end

local function seqFrame( seq, i ) return seq.frames and seq.frames[i] or ( seq.start + i - 1 ) end

local function dispatch( self, phase )
    if self.dispatchEvent then self:dispatchEvent{ name = "sprite", phase = phase, target = self, sequence = self.sequence, frame = self.frame } end
end

-- pick a sequence by name (shows its first frame, does not start playing)
function spriteMethods:setSequence( name )
    local seq = self._s3dSequences[name]
    if not seq then error( "s3d: unknown sprite sequence '" .. tostring( name ) .. "'", 2 ) end
    self._s3dSeq, self.sequence = seq, name
    self.numFrames = seq.frames and #seq.frames or seq.count
    self._s3dTime, self._s3dLoops, self._s3dDir = 0, 0, 1
    self.frame = 1
    self.sheetFrame = seqFrame( seq, 1 )
    showFrame( self, self.sheetFrame )
    return self
end

-- frame within the current sequence (1..numFrames)
function spriteMethods:setFrame( i )
    local seq = self._s3dSeq
    i = math.max( 1, math.min( self.numFrames, math.floor( i ) ) )
    self.frame = i
    self._s3dTime = ( i - 1 ) * ( seq.time or 1000 ) / self.numFrames
    self.sheetFrame = seqFrame( seq, i )
    showFrame( self, self.sheetFrame )
    return self
end

function spriteMethods:play()
    if not self.isPlaying then
        self.isPlaying = true
        playingSprites[self] = true
        dispatch( self, "began" )
    end
    return self
end

function spriteMethods:pause()
    self.isPlaying = false
    playingSprites[self] = nil
    return self
end

-- advance one sprite by dt milliseconds
local function stepSprite( self, dt )
    local seq = self._s3dSeq
    local n = self.numFrames
    if not seq or n <= 1 then return end
    local per = ( seq.time or ( n * 1000 / 15 ) ) / n -- default 15 frames per second
    self._s3dTime = self._s3dTime + dt * ( self.timeScale or 1 )
    local steps = math.floor( self._s3dTime / per )
    if steps <= 0 then return end
    self._s3dTime = self._s3dTime - steps * per
    local bounce = seq.loopDirection == "bounce"
    local loopCount = seq.loopCount or 0
    for _ = 1, steps do
        local nextFrame = self.frame + self._s3dDir
        if nextFrame > n or nextFrame < 1 then
            if bounce and self._s3dDir == 1 then
                self._s3dDir = -1; nextFrame = n - 1
            else
                self._s3dLoops = self._s3dLoops + 1
                if loopCount > 0 and self._s3dLoops >= loopCount then
                    self:pause()
                    dispatch( self, "ended" )
                    return
                end
                if bounce then self._s3dDir = 1; nextFrame = 2 else nextFrame = 1 end
                dispatch( self, "loop" )
            end
            nextFrame = math.max( 1, math.min( n, nextFrame ) )
        end
        self.frame = nextFrame
    end
    self.sheetFrame = seqFrame( seq, self.frame )
    showFrame( self, self.sheetFrame )
    dispatch( self, "next" )
end

local lastSpriteTime
-- called every frame by s3d.init's enterFrame listener
function M._stepSprites( time )
    local dt = lastSpriteTime and ( time - lastSpriteTime ) or 0
    lastSpriteTime = time
    if dt <= 0 or dt > 250 then return end -- first frame, or after a pause (app suspended)
    for spr in pairs( playingSprites ) do
        if spr.removeSelf and spr.isVisible ~= nil then stepSprite( spr, dt ) else playingSprites[spr] = nil end
    end
end

function M.newSprite( parent, filename, opts )
    M.init()
    opts = opts or {}
    if type( filename ) ~= "string" then error( "s3d.newSprite: filename (the image) is required", 2 ) end
    local frames = sheetFrames( opts.sheet )
    local aspect = frames[1][5] or 1
    local h = opts.height or ( opts.width and opts.width / aspect ) or 1
    local w = opts.width or h * aspect
    local ax, ay = opts.anchorX or 0.5, opts.anchorY or ( opts.billboard == false and 0.5 or 1 )
    local xl, xr, yt, yb = -ax * w, ( 1 - ax ) * w, ay * h, -( 1 - ay ) * h

    local b = newBuilder()
    local f = frames[1]
    b:vertex( xl, yt, 0, f[1], f[2], 0, 0, 1 ) -- top-left
    b:vertex( xl, yb, 0, f[1], f[4], 0, 0, 1 ) -- bottom-left
    b:vertex( xr, yb, 0, f[3], f[4], 0, 0, 1 ) -- bottom-right
    b:vertex( xl, yt, 0, f[1], f[2], 0, 0, 1 ) -- top-left
    b:vertex( xr, yb, 0, f[3], f[4], 0, 0, 1 ) -- bottom-right
    b:vertex( xr, yt, 0, f[3], f[2], 0, 0, 1 ) -- top-right

    local mode = opts.billboard
    if mode == nil then mode = "upright" end
    local bits = 0
    if mode == "upright" then bits = 8192 + 16384 elseif mode == "full" or mode == true then bits = 8192 end

    local o = { texture = filename, baseDir = opts.baseDir, color = opts.color, ao = false, cull = false,
        effect = opts.lit and M.EFFECT_SPRITE_LIT or M.EFFECT_SPRITE, x = opts.x, y = opts.y, z = opts.z }
    -- the billboard bits must be known when finalize sets the effect
    local spr
    o._preEffect = function( mesh ) mesh._s3dBillboard = bits ~= 0 and bits or nil end
    spr = finalize( parent, b, o )
    for k, fn in pairs( spriteMethods ) do spr[k] = fn end
    spr._s3dFrames = frames
    spr._s3dUV = frames[1]
    spr.spriteWidth, spr.spriteHeight = w, h
    spr._s3dSequences = {}
    local seqs = opts.sequences or { { name = "default", start = 1, count = #frames } }
    if seqs.name then seqs = { seqs } end
    for _, q in ipairs( seqs ) do
        local seq = {}
        for k, v in pairs( q ) do seq[k] = v end
        seq.start = seq.start or 1
        seq.count = seq.count or ( seq.frames and #seq.frames ) or ( #frames - seq.start + 1 )
        spr._s3dSequences[seq.name or "default"] = seq
        if not spr.sequence then spr:setSequence( seq.name or "default" ) end
    end
    if opts.castShadow then spr:castShadow( true ) end
    if opts.play ~= false and spr.numFrames > 1 and opts.sequences then spr:play() end
    return spr
end

-- ---------------------------------------------------------------------------
-- OBJ (Wavefront): v, vt, vn, f (polygons -> triangle fans). Materials are ignored.
-- Opens an OBJ file; also tries the name + ".txt" (Solar2D for Android always drops "*.obj" from the APK: the exclusion list
-- is hard-coded in create_build_properties.lua and build.settings cannot remove it). Name models "x.obj.txt".
local function openOBJ( filename, baseDir, level )
    local names = { filename }
    if type( filename ) == "string" and filename:lower():match( "%.obj$" ) then names[2] = filename .. ".txt" end
    for i = 1, #names do
        local path = system.pathForFile( names[i], baseDir )
        local f = path and io.open( path, "r" )
        if f then return f end
    end
    error( "s3d: cannot open " .. tostring( filename )
        .. ( names[2] and ( " or " .. names[2] .. " (Android builds drop *.obj files: rename it to " .. names[2] .. ")" ) or "" ), level + 1 )
end

-- Reads an OBJ into a builder (separate triangles, each vertex with position, uv, normal). Polygons become fans; missing vn gives flat normals.
local function parseOBJ( filename, opts, level )
    local f = openOBJ( filename, opts.baseDir or system.ResourceDirectory, level + 1 )
    local scale = opts.scale or 1
    local V, T, Nn = {}, {}, {}
    local b = newBuilder()
    local tris = 0

    local function idx( s, n )
        local i = tonumber( s )
        if not i then return nil end
        if i < 0 then i = n + i + 1 end
        return i
    end

    for line in f:lines() do
        local tag, rest = line:match( "^%s*(%S+)%s*(.*)$" )
        if tag == "v" then
            local x, y, z = rest:match( "(%S+)%s+(%S+)%s+(%S+)" )
            V[#V + 1] = { tonumber( x ) * scale, tonumber( y ) * scale, tonumber( z ) * scale }
        elseif tag == "vt" then
            local u, v = rest:match( "(%S+)%s+(%S+)" )
            T[#T + 1] = { tonumber( u ), 1 - tonumber( v ) }
        elseif tag == "vn" then
            local x, y, z = rest:match( "(%S+)%s+(%S+)%s+(%S+)" )
            Nn[#Nn + 1] = { tonumber( x ), tonumber( y ), tonumber( z ) }
        elseif tag == "f" then
            local face = {}
            for tok in rest:gmatch( "%S+" ) do
                local vi, ti, ni = tok:match( "^([^/]*)/?([^/]*)/?([^/]*)$" )
                face[#face + 1] = { idx( vi, #V ), idx( ti, #T ), idx( ni, #Nn ) }
            end
            for k = 2, #face - 1 do
                local c = { face[1], face[k], face[k + 1] }
                local p = {}
                for m = 1, 3 do p[m] = V[c[m][1]] end
                local e1 = { p[2][1] - p[1][1], p[2][2] - p[1][2], p[2][3] - p[1][3] }
                local e2 = { p[3][1] - p[1][1], p[3][2] - p[1][2], p[3][3] - p[1][3] }
                local fx, fy, fz = e1[2] * e2[3] - e1[3] * e2[2], e1[3] * e2[1] - e1[1] * e2[3], e1[1] * e2[2] - e1[2] * e2[1]
                local fl = sqrt( fx * fx + fy * fy + fz * fz )
                if fl > 0 then fx, fy, fz = fx / fl, fy / fl, fz / fl end
                for m = 1, 3 do
                    local t = c[m][2] and T[c[m][2]] or { 0, 0 }
                    local n = ( c[m][3] and Nn[c[m][3]] ) or { fx, fy, fz }
                    b:vertex( p[m][1], p[m][2], p[m][3], t[1], t[2], n[1], n[2], n[3] )
                end
                tris = tris + 1
            end
        end
    end
    f:close()
    if tris == 0 then error( "s3d: OBJ file has no faces: " .. tostring( filename ), level + 1 ) end
    return b
end

function M.loadOBJ( parent, filename, opts )
    M.init()
    opts = opts or {}
    opts._aoKey = tostring( filename ) .. "|" .. tostring( opts.scale or 1 )
    return finalize( parent, parseOBJ( filename, opts, 2 ), opts )
end

---------------------------------------------------------------------------------------------
-- glTF 2.0 (.glb, or .gltf with embedded data) through cgltf. Static models: the rest pose of every mesh node is baked
-- into one Solar2D mesh per material. Embedded images are written to the temporary directory and used as textures.
--
--   local golem = s3d.loadGLTF( scene, "assets/glbs/cinderheart_golem.glb", {
--       height = 2,            -- scale so the model is this tall (or scale = 0.5)
--       center = true,         -- put the model's feet at y = 0, centred on x / z (default true)
--       castShadow = true, unlit = false, x = 0, y = 0, z = 0,
--   } )
--   golem:moveTo( 3, 0, 0 ); golem:rotateTo( 0, 45, 0 ); golem:scaleTo( 1.5 )
--   golem.parts, golem.animations (names; playback comes later), golem.width3d, golem.height3d, golem.depth3d
---------------------------------------------------------------------------------------------
local gltfCache = {}    -- path -> parsed model (the arrays are copied for every load: finalize appends to them)
local gltfImages = {}   -- path .. "#" .. i -> file name in the temporary directory

local function gltfRead( filename, baseDir, level )
    local path = system.pathForFile( filename, baseDir )
    local f = path and io.open( path, "rb" )
    if not f then error( "s3d.loadGLTF: cannot open " .. tostring( filename ), level + 1 ) end
    local bytes = f:read( "*a" )
    f:close()
    return path, bytes
end

-- -> file name, baseDir of the texture. Embedded images (GLB buffer views, data: URIs) are written once to the
-- temporary directory; an external image (uri) is used where it is, relative to the glTF file, in the same baseDir.
local function gltfImage( key, img, filename, baseDir )
    if img.uri then
        local uri = img.uri:gsub( "%%(%x%x)", function( h ) return string.char( tonumber( h, 16 ) ) end ):gsub( "\\", "/" )
        local dir = filename:match( "^(.*[/\\])" ) or ""
        return dir:gsub( "\\", "/" ) .. uri, baseDir
    end
    local name = gltfImages[key]
    if name then return name, system.TemporaryDirectory end
    if not img.data then return nil end
    local ext = img.mime == "image/jpeg" and ".jpg" or ".png"
    local n = 0
    for _ in pairs( gltfImages ) do n = n + 1 end
    name = "s3d_gltf_" .. ( n + 1 ) .. "_" .. ( key:match( "([%w_%-]+)%.%w+#" ) or "img" ) .. ext
    local f = io.open( system.pathForFile( name, system.TemporaryDirectory ), "wb" )
    if not f then return nil end
    f:write( img.data )
    f:close()
    gltfImages[key] = name
    return name, system.TemporaryDirectory
end

-- glTF BLEND parts go into one group per parent, kept after the opaque objects (they do not write depth, so anything
-- drawn after them that is further away would cover them)
local function blendLayer( parent )
    if not parent then return nil end
    local layer = parent._s3dBlendLayer
    if layer then return layer end
    layer = display.newGroup()
    parent:insert( layer )
    parent._s3dBlendLayer = layer
    if parent._s3dShadowLayer then parent._s3dShadowLayer:toFront() end
    return layer
end

local function copyArray( a )
    local c = {}
    for i = 1, #a do c[i] = a[i] end
    return c
end

local modelMethods = {}
local function forward( name )
    modelMethods[name] = function( self, ... )
        for i = 1, #self.parts do local p = self.parts[i]; p[name]( p, ... ) end
        return self
    end
end
for _, name in ipairs{ "moveTo", "moveBy", "rotateTo", "rotateBy", "setPose", "setRotationQuat", "scaleTo",
    "setCulling", "setDepthTest", "setReflection" } do forward( name ) end

-- ---- node animation (glTF step 2): the rig (C) poses every part's vertices into buffers the parts use as morph source
local playingModels = setmetatable( {}, { __mode = "k" } )
local lastModelTime

local function applyMorph( part )
    local mo = part._s3dMorph
    if not mo then return end
    local nd = part._s3dNodes
    nd = nd and nd.m
    local function put( o )
        if o and o.fill and o.fill.effect then
            o.fill.effect.s3dMorph = mo
            if nd then o.fill.effect.s3dNodes = nd end -- GPU path: new node matrices (also redraws the object)
        end
    end
    put( part ); put( part._s3dCaster ); put( part._s3dShadow )
end

local function poseModel( self )
    local a = self._anim
    if not a then return end
    if a.fadeTo then
        C.gltfPose( self._rig, a.clip, a.time, a.fadeTo.clip, a.fadeTo.time, a.fadeTo.weight )
    else
        C.gltfPose( self._rig, a.clip, a.time )
    end
    for i = 1, #self.parts do applyMorph( self.parts[i] ) end
end

local function ensureRig( self )
    if self._rigReady then return true end
    if not self._rig then return false end
    for _, part in ipairs( self.parts ) do
        if not part._s3dNodes then -- GPU parts were attached when loaded
            local outP, outN = C.gltfAttach( self._rig, part._s3dGroup )
            part._s3dMorph = { outP, outP, 0, outN, outN }
        end
    end
    self._rigReady = true
    return true
end

local function clipIndex( self, name )
    if type( name ) == "number" then return self.animations[name] and name or nil end
    for i, n in ipairs( self.animations ) do if n == name then return i end end
    return nil
end

local function advance( self, st, dt, isMain )
    local dur = self._durations[st.clip] or 0
    st.time = st.time + dt * st.speed
    if dur <= 0 then st.time = 0; return false end
    if st.loop then
        st.time = st.time % dur
    elseif st.time >= dur then
        st.time = dur
        return isMain -- finished
    elseif st.time < 0 then
        st.time = 0
    end
    return false
end

local function stepModel( self, dt )
    local a = self._anim
    if not a or self._paused then return end
    local s = dt * 0.001 * ( self.timeScale or 1 )
    local finished = false
    if a.fadeTo then
        local f = a.fadeTo
        advance( self, a, s, false )
        finished = advance( self, f, s, true )
        f.weight = f.weight + s / f.fade
        if f.weight >= 1 then
            a.clip, a.time, a.loop, a.speed, a.onComplete = f.clip, f.time, f.loop, f.speed, f.onComplete
            a.fadeTo = nil
        end
    else
        finished = advance( self, a, s, true )
    end
    if self.isVisible ~= false then poseModel( self ) end
    self.animation, self.animationTime = self.animations[a.fadeTo and a.fadeTo.clip or a.clip], ( a.fadeTo or a ).time
    if finished then
        playingModels[self] = nil
        local cb = ( a.fadeTo or a ).onComplete
        if cb then cb( self, self.animation ) end
    end
end

function M._stepModels( time )
    local dt = lastModelTime and ( time - lastModelTime ) or 0
    lastModelTime = time
    if dt <= 0 or dt > 250 then return end
    for model in pairs( playingModels ) do
        if model.parts[1] and model.parts[1].removeSelf then stepModel( model, dt ) else playingModels[model] = nil end
    end
end

-- model:play( name or index, { loop = true, speed = 1, fade = 0.2 (seconds, cross-fade from the current clip),
--                              time = 0, onComplete = function( model, name ) end } )
function modelMethods:play( name, opts )
    opts = opts or {}
    local clip = clipIndex( self, name or 1 )
    if not clip then error( "s3d: model has no animation " .. tostring( name ), 2 ) end
    if not ensureRig( self ) then return self end
    local st = { clip = clip, time = opts.time or 0, loop = opts.loop ~= false, speed = opts.speed or 1, onComplete = opts.onComplete }
    local a = self._anim
    local fade = opts.fade or 0.2
    if a and fade > 0 and playingModels[self] and not self._paused then
        if a.fadeTo then -- already fading: the clip being faded in becomes the base
            local f = a.fadeTo
            if f.weight >= 0.5 then a.clip, a.time, a.loop, a.speed = f.clip, f.time, f.loop, f.speed end
        end
        st.fade, st.weight = fade, 0
        a.fadeTo = st
    else
        self._anim = st
    end
    self._paused = false
    playingModels[self] = true
    self.animation, self.animationTime = self.animations[clip], st.time
    poseModel( self )
    return self
end
function modelMethods:pause() self._paused = true; return self end
function modelMethods:resume() if self._anim then self._paused = false; playingModels[self] = true end; return self end
function modelMethods:isPlaying() return playingModels[self] ~= nil and not self._paused end
function modelMethods:setSpeed( s ) self.timeScale = s; return self end
-- jump to a time (seconds) of the current clip, also while paused
function modelMethods:setTime( t )
    local a = self._anim
    if not a then return self end
    a.fadeTo = nil
    a.time = t
    self.animationTime = t
    poseModel( self )
    return self
end
-- back to the rest pose (the file's bind pose, as loaded)
function modelMethods:stop()
    playingModels[self] = nil
    self._anim, self._paused, self.animation = nil, false, nil
    if self._rigReady then
        C.gltfPose( self._rig, 0, 0 )
        for i = 1, #self.parts do applyMorph( self.parts[i] ) end
    end
    return self
end
function modelMethods:getDuration( name )
    local i = clipIndex( self, name or ( self._anim and self._anim.clip ) or 1 )
    return i and self._durations[i] or 0
end
function modelMethods:castShadow( on )
    for i = 1, #self.parts do local p = self.parts[i]; p:castShadow( on ); applyMorph( p ) end
    return self
end
function modelMethods:setVisible( on ) for i = 1, #self.parts do self.parts[i].isVisible = on end; self.isVisible = on; return self end
function modelMethods:setFillColor( ... ) for i = 1, #self.parts do self.parts[i]:setFillColor( ... ) end; return self end
function modelMethods:removeSelf()
    playingModels[self] = nil
    for i = 1, #self.parts do display.remove( self.parts[i] ) end
    self.parts = {}
end

function M.loadGLTF( parent, filename, opts )
    M.init()
    opts = opts or {}
    local baseDir = opts.baseDir or system.ResourceDirectory
    local path = system.pathForFile( filename, baseDir ) or filename
    local data = gltfCache[path]
    if data and data.static and not opts.static then data = nil end -- cached without its animations
    if not data then
        local _, bytes = gltfRead( filename, baseDir, 2 )
        local ok, res = pcall( C.gltfLoad, bytes, opts.static )
        if not ok then error( tostring( res ) .. " (" .. tostring( filename ) .. ")", 2 ) end
        data = res
        if opts.static then data.static = true end
        gltfCache[path] = data
    end

    local bb = data.bounds
    local scale = opts.scale or 1
    if opts.height and bb[5] > bb[2] then scale = opts.height / ( bb[5] - bb[2] ) end
    local ox, oy, oz = 0, 0, 0
    if opts.center ~= false then ox, oy, oz = ( bb[1] + bb[4] ) * 0.5, bb[2], ( bb[3] + bb[6] ) * 0.5 end

    local model = { parts = {}, animations = data.animations, isVisible = true, filename = filename,
        width3d = ( bb[4] - bb[1] ) * scale, height3d = ( bb[5] - bb[2] ) * scale, depth3d = ( bb[6] - bb[3] ) * scale }
    for k, f in pairs( modelMethods ) do model[k] = f end
    if data.rig and not opts.static then
        model._rig = C.gltfCloneRig( data.rig )
        model._durations = C.gltfDurations( model._rig )
        C.gltfSetOrigin( model._rig, ox, oy, oz, scale )
    else
        model._durations = {}
    end
    -- GPU node animation: parts moved by at most NODE_MAX nodes keep their vertices in node space and get the node
    -- matrices every frame; others (and every part when it is off) are posed on the CPU
    local gpu = model._rig and M.gpuNodes and M.gpuAnimation and opts.gpuAnimation ~= false
    local anyGPU = false

    -- opaque parts first, blended ones last (drawn over what is behind them)
    local order = {}
    for gi, g in ipairs( data.groups ) do
        local mat = data.materials[g.material] or {}
        order[#order + 1] = { g = g, mat = mat, blend = mat.alphaMode == "BLEND" and 1 or 0, i = gi }
    end
    table.sort( order, function( a, b ) if a.blend ~= b.blend then return a.blend < b.blend end return a.i < b.i end )

    for _, e in ipairs( order ) do
        local g, mat = e.g, e.mat
        local b = newBuilder()
        local P = copyArray( g.P )
        for i = 1, #P, 3 do
            P[i] = ( P[i] - ox ) * scale; P[i + 1] = ( P[i + 1] - oy ) * scale; P[i + 2] = ( P[i + 2] - oz ) * scale
        end
        b.P, b.U, b.N = P, copyArray( g.U ), copyArray( g.N )

        local tex, texDir
        if mat.image and data.images[mat.image] then
            tex, texDir = gltfImage( path .. "#" .. mat.image, data.images[mat.image], filename, baseDir )
        end
        local mask = mat.alphaMode == "MASK"
        local col = mat.color or { 1, 1, 1, 1 }
        local o = {
            texture = tex, baseDir = tex and texDir or nil,
            color = ( col[1] ~= 1 or col[2] ~= 1 or col[3] ~= 1 or col[4] ~= 1 ) and { col[1], col[2], col[3], col[4] } or nil,
            -- BLEND keeps back faces culled even when double-sided: without sorting, the far side of a see-through box
            -- would be blended over its near side (Blockbench marks every material double-sided)
            cull = ( e.blend == 1 or not mat.doubleSided ) and opts.cull ~= false,
            effect = opts.effect or ( mask and ( opts.unlit and M.EFFECT_SPRITE or M.EFFECT_SPRITE_LIT ) ) or ( opts.unlit and M.EFFECT_UNLIT or M.EFFECT_LIT ),
            ao = opts.ao, aoDistance = opts.aoDistance, _aoKey = path .. "|" .. e.i .. "|" .. scale,
        }
        if o.cull == true then o.cull = nil end
        local nodeInfo
        if gpu then
            local m, lp, ln, idx, map = C.gltfAttachNodes( model._rig, e.i, NODE_MAX )
            if m and #idx * 3 == #P then nodeInfo = { m = m, idx = idx, map = map, morph = { lp, lp, 0, ln, ln } } end
        end
        if nodeInfo then o._extension = NODE_EXTENSION; anyGPU = true end
        o._preEffect = function( mesh )
            if mask and tex then mesh._s3dAlphaCut = true end
            if nodeInfo then mesh._s3dNodes, mesh._s3dMorph = nodeInfo, nodeInfo.morph end
        end

        -- pixel-art textures (Blockbench): nearest filtering, as the file's sampler asks
        local mag, minf
        if tex and mat.nearest then
            mag, minf = display.getDefault( "magTextureFilter" ), display.getDefault( "minTextureFilter" )
            display.setDefault( "magTextureFilter", "nearest" ); display.setDefault( "minTextureFilter", "nearest" )
        end
        local part = finalize( e.blend == 1 and blendLayer( parent ) or parent, b, o )
        if mag then display.setDefault( "magTextureFilter", mag ); display.setDefault( "minTextureFilter", minf ) end
        if e.blend == 1 then part:setDepthWrite( false ); part._s3dBlend = true end
        if nodeInfo then fillNodeIndices( part, nodeInfo.idx ) end
        part._s3dMaterial = mat.name
        part._s3dGroup = e.i
        part.gpuAnimated = nodeInfo ~= nil
        model.parts[#model.parts + 1] = part
    end
    if anyGPU then
        -- the GPU parts show the rest pose until an animation plays
        C.gltfPose( model._rig, 0, 0 )
        for i = 1, #model.parts do applyMorph( model.parts[i] ) end
    end
    model.gpuAnimated = anyGPU

    if opts.castShadow then model:castShadow( true ) end
    if opts.x or opts.y or opts.z then model:moveTo( opts.x or 0, opts.y or 0, opts.z or 0 ) end
    if opts.animation and model._rig then model:play( opts.animation, { fade = 0, loop = opts.loop, speed = opts.speed } ) end
    return model
end

---------------------------------------------------------------------------------------------
-- Frame animation from a sequence of OBJ files (e.g. Blender: File > Export > Wavefront with "Animation" checked).
--
--   local hero = s3d.loadOBJAnimation( scene, "assets/run/run_%06d.obj", {
--       first = 1, last = 24,           -- frame numbers in the file names (fills %d / %06d); or frames = { "a.obj", "b.obj" }
--       fps = 24, loop = true, interpolate = true, autoPlay = true,
--       clips = { run = { 1, 16 }, jump = { 17, 24, loop = false, fps = 30 } },   -- indices of the loaded frames, from 1
--       texture = "assets/hero.png", castShadow = true, scale = 1,
--       onComplete = function( obj, clip ) end,                                   -- a non-looping clip finished
--   } )
--   hero:play( "jump" ), hero:pause(), hero:resume(), hero:stop(), hero:gotoFrame( 3.5 )
--   hero:setSpeed( 2 ), hero:isPlaying(), hero.frameCount, hero.currentFrame, hero.clip
--
-- All frames must share the same topology (same faces in the same order, as when exported from one mesh). Positions and
-- normals of two neighbouring frames are interpolated in the geometry writer (C), so Lua only changes 5 values per frame.
---------------------------------------------------------------------------------------------
local animMethods = {}

local function animApply( obj )
    local a = obj._s3dAnim
    local n = #a.frames
    local f = a.time * a.fps -- (fractional) frame within the clip, from 0
    local c0, c1 = a.clipFirst, a.clipLast
    local len = c1 - c0 + 1
    local i, t
    if a.loop then
        f = f % len
        i = math.floor( f )
        t = f - i
        local j = ( i + 1 ) % len
        a.buf[1], a.buf[2] = a.frames[c0 + i].p, a.frames[c0 + j].p
        a.buf[4], a.buf[5] = a.frames[c0 + i].n, a.frames[c0 + j].n
    else
        if f >= len - 1 then f = len - 1 end
        i = math.floor( f )
        t = f - i
        local j = math.min( i + 1, len - 1 )
        a.buf[1], a.buf[2] = a.frames[c0 + i].p, a.frames[c0 + j].p
        a.buf[4], a.buf[5] = a.frames[c0 + i].n, a.frames[c0 + j].n
    end
    if not a.interpolate then t = 0 end
    a.buf[3] = t
    obj.currentFrame = c0 + f
    local function put( o ) if o and o.fill and o.fill.effect then o.fill.effect.s3dMorph = a.buf end end
    put( obj ); put( obj._s3dCaster ); put( obj._s3dShadow )
    return n
end

local function animTick( obj, dt )
    local a = obj._s3dAnim
    if not a.playing then
        -- shadow copies may be created later (castShadow): give them the current morph even while paused
        if obj._s3dCaster ~= a.seenCaster or obj._s3dShadow ~= a.seenShadow then
            a.seenCaster, a.seenShadow = obj._s3dCaster, obj._s3dShadow
            animApply( obj )
        end
        return
    end
    a.time = a.time + dt * a.speed
    local len = a.clipLast - a.clipFirst + 1
    if not a.loop and a.time * a.fps >= len - 1 then
        a.time = ( len - 1 ) / a.fps
        a.playing = false
        animApply( obj )
        if a.onComplete then a.onComplete( obj, a.clipName ) end
        obj:dispatchEvent{ name = "animation", phase = "ended", target = obj, clip = a.clipName }
        return
    end
    animApply( obj )
end

function animMethods:play( clip )
    local a = self._s3dAnim
    local c = clip and a.clips[clip]
    if clip and not c then error( "s3d: no clip named " .. tostring( clip ), 2 ) end
    a.clipName = clip
    a.clipFirst = c and c[1] or 1
    a.clipLast = c and c[2] or #a.frames
    a.fps = ( c and c.fps ) or a.baseFps
    if c and c.loop ~= nil then a.loop = c.loop else a.loop = a.baseLoop end
    a.time = 0
    a.playing = true
    self.clip = clip
    animApply( self )
    return self
end
function animMethods:pause() self._s3dAnim.playing = false; return self end
function animMethods:resume() self._s3dAnim.playing = true; return self end
function animMethods:stop() local a = self._s3dAnim; a.playing = false; a.time = 0; animApply( self ); return self end
function animMethods:isPlaying() return self._s3dAnim.playing end
function animMethods:setSpeed( s ) self._s3dAnim.speed = s or 1; return self end
function animMethods:setFPS( fps ) local a = self._s3dAnim; a.time = a.time * a.fps / fps; a.fps = fps; return self end
function animMethods:setInterpolation( on ) self._s3dAnim.interpolate = on and true or false; animApply( self ); return self end
-- frame counted from 1 within the current clip; fractions are allowed (2.5 = halfway between frames 2 and 3)
function animMethods:gotoFrame( frame )
    local a = self._s3dAnim
    a.time = ( ( frame or 1 ) - 1 ) / a.fps
    animApply( self )
    return self
end

function M.loadOBJAnimation( parent, source, opts )
    M.init()
    opts = opts or {}
    local files = {}
    if type( source ) == "table" then
        files = source
    elseif opts.frames then
        files = opts.frames
    else
        local first, last = opts.first or 1, opts.last
        if not last then error( "s3d.loadOBJAnimation: opts.last (last frame) is required with a file name pattern", 2 ) end
        for k = first, last do files[#files + 1] = string.format( source, k ) end
    end
    if #files == 0 then error( "s3d.loadOBJAnimation: no frames", 2 ) end

    local frames, firstBuilder = {}, nil
    for k, name in ipairs( files ) do
        local b = parseOBJ( name, opts, 2 )
        if not firstBuilder then
            firstBuilder = b
        elseif #b.P ~= #firstBuilder.P then
            error( string.format( "s3d.loadOBJAnimation: frame %d (%s) has %d vertices, frame 1 has %d: all frames must share the same topology",
                k, name, #b.P / 3, #firstBuilder.P / 3 ), 2 )
        end
        frames[k] = { p = C.newBuffer( b.P ), n = C.newBuffer( b.N ) }
    end

    local obj = finalize( parent, firstBuilder, opts )
    for k, fn in pairs( animMethods ) do obj[k] = fn end
    obj.frameCount = #frames
    obj._s3dAnim = {
        frames = frames, clips = opts.clips or {}, buf = { false, false, 0, false, false },
        baseFps = opts.fps or 24, baseLoop = opts.loop ~= false, fps = opts.fps or 24, loop = opts.loop ~= false,
        interpolate = opts.interpolate ~= false, speed = 1, time = 0, playing = false,
        clipFirst = 1, clipLast = #frames, onComplete = opts.onComplete,
    }
    -- animApply writes the morph to the object and to its shadow copies (caster / planar shadow) every frame
    local last = system.getTimer()
    local function onFrame( e )
        local now = e.time
        animTick( obj, ( now - last ) / 1000 )
        last = now
    end
    Runtime:addEventListener( "enterFrame", onFrame )
    obj:addEventListener( "finalize", function() Runtime:removeEventListener( "enterFrame", onFrame ) end )
    if opts.autoPlay ~= false then obj:play( opts.clip ) else animApply( obj ) end
    return obj
end

-- 3D group: simply a display group at the origin (0, 0), not rotated or scaled.
-- Do not set x/y/rotation/xScale on 3D objects or their groups: use moveTo/rotateTo/scaleTo.
function M.newScene( parent )
    M.init()
    local g = display.newGroup()
    if parent then parent:insert( g ) end
    return g
end

return M
