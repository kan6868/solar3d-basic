/*
 * s3d.c - a small 3D plugin for Solar2D, built only on the public native APIs (no engine changes).
 *
 * How it works:
 *   - A mesh is display.newMesh{ mode = "triangles", hasZ = true } ("indexed" meshes are not used: they live
 *     on the GPU, so geometry writers are never called for them).
 *   - The "filter.s3d.*" effects carry the "s3d" data type:
 *       before(): installs geometry writers for "position" (multiplies by the model matrix) and "userData"
 *       (transformed normal), enables the depth/cull state block, draws, issues the camera/light uniform command, disables the state block.
 *   - Shell transform "s3d": a_Position becomes a vec3, gl_Position = u_S3D_ViewProj * vec4(a_Position, 1).
 *   - Matrix math uses cglm.
 *
 * Build:
 *   Windows (Simulator / Win32): define S3D_DYNAMIC; every Lua/Corona/GL function is resolved at run time
 *   (GetProcAddress on already loaded modules), so neither lua.lib nor CoronaLabs.Corona.Native.lib is needed.
 *   Android: no S3D_DYNAMIC; links directly against liblua.so, libcorona.so and libGLESv2.so.
 */

#define CGLM_ALL_UNALIGNED
#include <cglm/cglm.h>

#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"
#include "s3d_corona.h"

#define S3D_VERSION "0.12.0"

/* ========================================================================== */
/* Function binding: resolved at run time on Windows, linked directly on Android            */
/* ========================================================================== */

#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE  0x0B44
#define GL_BACK       0x0405
#define GL_CCW        0x0901
#define GL_CW         0x0900
#define GL_LEQUAL     0x0203

#ifdef S3D_DYNAMIC

#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>

#define S3D_EXPORT __declspec(dllexport)
#define GLAPIENTRY_ __stdcall

static void * s3d_find_symbol( const char * name )
{
    HMODULE mods[1024];
    DWORD needed = 0, i;
    void * p = (void *)GetProcAddress( GetModuleHandleA( NULL ), name );

    if (p) return p;

    if (K32EnumProcessModules( GetCurrentProcess(), mods, sizeof( mods ), &needed ))
    {
        DWORD n = needed / sizeof( HMODULE );

        if (n > 1024) n = 1024;
        for (i = 0; i < n; ++i)
        {
            p = (void *)GetProcAddress( mods[i], name );
            if (p) return p;
        }
    }
    return NULL;
}

/* --- Corona --- */
#define S3D_DECL_PTR( ret, name, params ) typedef ret ( * name##_t ) params; static name##_t p_##name;
S3D_CORONA_FUNCS( S3D_DECL_PTR )
#define CORONA( name ) p_##name

/* --- OpenGL 1.1 (opengl32.dll) --- */
typedef void ( GLAPIENTRY_ * glEnable_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glDepthFunc_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glDepthMask_t )( unsigned char );
typedef void ( GLAPIENTRY_ * glCullFace_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glFrontFace_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glGetIntegerv_t )( unsigned int, int * );
typedef void ( GLAPIENTRY_ * glClear_t )( unsigned int );
typedef unsigned int ( GLAPIENTRY_ * glGetError_t )( void );
static glGetError_t p_glGetError;
typedef void ( GLAPIENTRY_ * glPolygonOffset_t )( float, float );
static glPolygonOffset_t p_glPolygonOffset;
typedef void ( GLAPIENTRY_ * glStencilFunc_t )( unsigned int, int, unsigned int );
typedef void ( GLAPIENTRY_ * glStencilOp_t )( unsigned int, unsigned int, unsigned int );
typedef void ( GLAPIENTRY_ * glStencilMask_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glClearStencil_t )( int );
static glStencilFunc_t p_glStencilFunc;
static glStencilOp_t p_glStencilOp;
static glStencilMask_t p_glStencilMask;
static glClearStencil_t p_glClearStencil;
static glEnable_t p_glEnable, p_glDisable;
static glDepthFunc_t p_glDepthFunc;
static glDepthMask_t p_glDepthMask;
static glCullFace_t p_glCullFace;
static glFrontFace_t p_glFrontFace;
static glGetIntegerv_t p_glGetIntegerv;
static glClear_t p_glClear;

/* OpenGL 2.0+/3.0 (FBO): fetched with wglGetProcAddress once a context exists, i.e. on the render thread */
typedef void ( GLAPIENTRY_ * glGenRenderbuffers_t )( int, unsigned int * );
typedef void ( GLAPIENTRY_ * glBindRenderbuffer_t )( unsigned int, unsigned int );
typedef void ( GLAPIENTRY_ * glRenderbufferStorage_t )( unsigned int, unsigned int, int, int );
typedef void ( GLAPIENTRY_ * glFramebufferRenderbuffer_t )( unsigned int, unsigned int, unsigned int, unsigned int );
typedef void ( GLAPIENTRY_ * glDeleteRenderbuffers_t )( int, const unsigned int * );
static glGenRenderbuffers_t p_glGenRenderbuffers;
static glBindRenderbuffer_t p_glBindRenderbuffer;
static glRenderbufferStorage_t p_glRenderbufferStorage;
static glFramebufferRenderbuffer_t p_glFramebufferRenderbuffer;
static glDeleteRenderbuffers_t p_glDeleteRenderbuffers;

/* OpenGL 1.1 (opengl32.dll), used by the shadow map */
typedef void ( GLAPIENTRY_ * glGenTextures_t )( int, unsigned int * );
typedef void ( GLAPIENTRY_ * glBindTexture_t )( unsigned int, unsigned int );
typedef void ( GLAPIENTRY_ * glTexImage2D_t )( unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void * );
typedef void ( GLAPIENTRY_ * glTexParameteri_t )( unsigned int, unsigned int, int );
typedef void ( GLAPIENTRY_ * glViewport_t )( int, int, int, int );
typedef void ( GLAPIENTRY_ * glClearColor_t )( float, float, float, float );
typedef void ( GLAPIENTRY_ * glGetFloatv_t )( unsigned int, float * );
static glGenTextures_t p_glGenTextures;
static glBindTexture_t p_glBindTexture;
static glTexImage2D_t p_glTexImage2D;
static glTexParameteri_t p_glTexParameteri;
static glViewport_t p_glViewport;
static glClearColor_t p_glClearColor;
static glGetFloatv_t p_glGetFloatv;

/* OpenGL 2.0 / 3.0 (wglGetProcAddress) */
typedef void ( GLAPIENTRY_ * glGenFramebuffers_t )( int, unsigned int * );
typedef void ( GLAPIENTRY_ * glBindFramebuffer_t )( unsigned int, unsigned int );
typedef void ( GLAPIENTRY_ * glFramebufferTexture2D_t )( unsigned int, unsigned int, unsigned int, unsigned int, int );
typedef void ( GLAPIENTRY_ * glActiveTexture_t )( unsigned int );
typedef int ( GLAPIENTRY_ * glGetUniformLocation_t )( unsigned int, const char * );
typedef void ( GLAPIENTRY_ * glUniform1i_t )( int, int );
typedef unsigned int ( GLAPIENTRY_ * glCheckFramebufferStatus_t )( unsigned int );
static glCheckFramebufferStatus_t p_glCheckFramebufferStatus;
static glGenFramebuffers_t p_glGenFramebuffers;
static glBindFramebuffer_t p_glBindFramebuffer;
static glFramebufferTexture2D_t p_glFramebufferTexture2D;
static glActiveTexture_t p_glActiveTexture;
static glGetUniformLocation_t p_glGetUniformLocation;
static glUniform1i_t p_glUniform1i;
typedef unsigned char ( GLAPIENTRY_ * glIsProgram_t )( unsigned int );
typedef void ( GLAPIENTRY_ * glUseProgram_t )( unsigned int );
static glIsProgram_t p_glIsProgram;
static glUseProgram_t p_glUseProgram;

/* --- Lua 5.1 (lua.dll): redefine the functions we use and forward them through pointers --- */
#define S3D_LUA_FUNCS(X) \
    X( int, lua_gettop, ( lua_State * L ), ( L ) ) \
    X( void, lua_settop, ( lua_State * L, int i ), ( L, i ) ) \
    X( void, lua_pushvalue, ( lua_State * L, int i ), ( L, i ) ) \
    X( int, lua_type, ( lua_State * L, int i ), ( L, i ) ) \
    X( int, lua_isnumber, ( lua_State * L, int i ), ( L, i ) ) \
    X( lua_Number, lua_tonumber, ( lua_State * L, int i ), ( L, i ) ) \
    X( int, lua_toboolean, ( lua_State * L, int i ), ( L, i ) ) \
    X( const char *, lua_tolstring, ( lua_State * L, int i, size_t * len ), ( L, i, len ) ) \
    X( size_t, lua_objlen, ( lua_State * L, int i ), ( L, i ) ) \
    X( void *, lua_touserdata, ( lua_State * L, int i ), ( L, i ) ) \
    X( void, lua_pushnil, ( lua_State * L ), ( L ) ) \
    X( void, lua_pushnumber, ( lua_State * L, lua_Number n ), ( L, n ) ) \
    X( void, lua_pushstring, ( lua_State * L, const char * s ), ( L, s ) ) \
    X( void, lua_pushlstring, ( lua_State * L, const char * s, size_t n ), ( L, s, n ) ) \
    X( void, lua_pushboolean, ( lua_State * L, int b ), ( L, b ) ) \
    X( void, lua_pushcclosure, ( lua_State * L, lua_CFunction f, int n ), ( L, f, n ) ) \
    X( void, lua_createtable, ( lua_State * L, int a, int b ), ( L, a, b ) ) \
    X( void *, lua_newuserdata, ( lua_State * L, size_t sz ), ( L, sz ) ) \
    X( void, lua_getfield, ( lua_State * L, int i, const char * k ), ( L, i, k ) ) \
    X( void, lua_setfield, ( lua_State * L, int i, const char * k ), ( L, i, k ) ) \
    X( void, lua_rawgeti, ( lua_State * L, int i, int n ), ( L, i, n ) ) \
    X( void, lua_rawseti, ( lua_State * L, int i, int n ), ( L, i, n ) ) \
    X( void, lua_call, ( lua_State * L, int a, int r ), ( L, a, r ) ) \
    X( int, lua_error, ( lua_State * L ), ( L ) ) \
    X( int, luaL_loadbuffer, ( lua_State * L, const char * b, size_t sz, const char * name ), ( L, b, sz, name ) )

#define S3D_LUA_PTR( ret, name, params, args ) typedef ret ( * name##_t ) params; static name##_t p_##name;
S3D_LUA_FUNCS( S3D_LUA_PTR )
#define S3D_LUA_STUB( ret, name, params, args ) ret name params { return ( ret ) p_##name args; }
#define S3D_LUA_STUB_VOID( ret, name, params, args ) ret name params { p_##name args; }

int lua_gettop( lua_State * L ) { return p_lua_gettop( L ); }
void lua_settop( lua_State * L, int i ) { p_lua_settop( L, i ); }
void lua_pushvalue( lua_State * L, int i ) { p_lua_pushvalue( L, i ); }
int lua_type( lua_State * L, int i ) { return p_lua_type( L, i ); }
int lua_isnumber( lua_State * L, int i ) { return p_lua_isnumber( L, i ); }
lua_Number lua_tonumber( lua_State * L, int i ) { return p_lua_tonumber( L, i ); }
int lua_toboolean( lua_State * L, int i ) { return p_lua_toboolean( L, i ); }
const char * lua_tolstring( lua_State * L, int i, size_t * len ) { return p_lua_tolstring( L, i, len ); }
size_t lua_objlen( lua_State * L, int i ) { return p_lua_objlen( L, i ); }
void * lua_touserdata( lua_State * L, int i ) { return p_lua_touserdata( L, i ); }
void lua_pushnil( lua_State * L ) { p_lua_pushnil( L ); }
void lua_pushnumber( lua_State * L, lua_Number n ) { p_lua_pushnumber( L, n ); }
void lua_pushstring( lua_State * L, const char * s ) { p_lua_pushstring( L, s ); }
void lua_pushlstring( lua_State * L, const char * s, size_t n ) { p_lua_pushlstring( L, s, n ); }
void lua_pushboolean( lua_State * L, int b ) { p_lua_pushboolean( L, b ); }
void lua_pushcclosure( lua_State * L, lua_CFunction f, int n ) { p_lua_pushcclosure( L, f, n ); }
void lua_createtable( lua_State * L, int a, int b ) { p_lua_createtable( L, a, b ); }
void * lua_newuserdata( lua_State * L, size_t sz ) { return p_lua_newuserdata( L, sz ); }
void lua_getfield( lua_State * L, int i, const char * k ) { p_lua_getfield( L, i, k ); }
void lua_setfield( lua_State * L, int i, const char * k ) { p_lua_setfield( L, i, k ); }
void lua_rawgeti( lua_State * L, int i, int n ) { p_lua_rawgeti( L, i, n ); }
void lua_rawseti( lua_State * L, int i, int n ) { p_lua_rawseti( L, i, n ); }
void lua_call( lua_State * L, int a, int r ) { p_lua_call( L, a, r ); }
int lua_error( lua_State * L ) { return p_lua_error( L ); }
int luaL_loadbuffer( lua_State * L, const char * b, size_t sz, const char * name ) { return p_luaL_loadbuffer( L, b, sz, name ); }

static const char * s3d_bind_all( void )
{
    HMODULE gl;

#define S3D_BIND_CORONA( ret, name, params ) p_##name = ( name##_t )s3d_find_symbol( #name ); if (!p_##name) return #name;
    S3D_CORONA_FUNCS( S3D_BIND_CORONA )
#define S3D_BIND_LUA( ret, name, params, args ) p_##name = ( name##_t )s3d_find_symbol( #name ); if (!p_##name) return #name;
    S3D_LUA_FUNCS( S3D_BIND_LUA )

    gl = GetModuleHandleA( "opengl32.dll" );
    if (!gl) gl = LoadLibraryA( "opengl32.dll" );
    if (!gl) return "opengl32.dll";
    p_glEnable = (glEnable_t)GetProcAddress( gl, "glEnable" );
    p_glDisable = (glEnable_t)GetProcAddress( gl, "glDisable" );
    p_glDepthFunc = (glDepthFunc_t)GetProcAddress( gl, "glDepthFunc" );
    p_glDepthMask = (glDepthMask_t)GetProcAddress( gl, "glDepthMask" );
    p_glCullFace = (glCullFace_t)GetProcAddress( gl, "glCullFace" );
    p_glFrontFace = (glFrontFace_t)GetProcAddress( gl, "glFrontFace" );
    p_glGetIntegerv = (glGetIntegerv_t)GetProcAddress( gl, "glGetIntegerv" );
    p_glClear = (glClear_t)GetProcAddress( gl, "glClear" );
    p_glGetError = (glGetError_t)GetProcAddress( gl, "glGetError" );
    p_glPolygonOffset = (glPolygonOffset_t)GetProcAddress( gl, "glPolygonOffset" );
    p_glStencilFunc = (glStencilFunc_t)GetProcAddress( gl, "glStencilFunc" );
    p_glStencilOp = (glStencilOp_t)GetProcAddress( gl, "glStencilOp" );
    p_glStencilMask = (glStencilMask_t)GetProcAddress( gl, "glStencilMask" );
    p_glClearStencil = (glClearStencil_t)GetProcAddress( gl, "glClearStencil" );
    p_glGenTextures = (glGenTextures_t)GetProcAddress( gl, "glGenTextures" );
    p_glBindTexture = (glBindTexture_t)GetProcAddress( gl, "glBindTexture" );
    p_glTexImage2D = (glTexImage2D_t)GetProcAddress( gl, "glTexImage2D" );
    p_glTexParameteri = (glTexParameteri_t)GetProcAddress( gl, "glTexParameteri" );
    p_glViewport = (glViewport_t)GetProcAddress( gl, "glViewport" );
    p_glClearColor = (glClearColor_t)GetProcAddress( gl, "glClearColor" );
    p_glGetFloatv = (glGetFloatv_t)GetProcAddress( gl, "glGetFloatv" );
    if (!p_glEnable || !p_glDisable || !p_glDepthFunc || !p_glDepthMask || !p_glCullFace || !p_glFrontFace
        || !p_glGetIntegerv || !p_glClear || !p_glGetError || !p_glPolygonOffset
        || !p_glStencilFunc || !p_glStencilOp || !p_glStencilMask || !p_glClearStencil
        || !p_glGenTextures || !p_glBindTexture || !p_glTexImage2D || !p_glTexParameteri
        || !p_glViewport || !p_glClearColor || !p_glGetFloatv) return "glEnable";
    return NULL;
}

#define GLCALL( name ) p_##name

#else /* direct linking (Android, iOS, Windows builds with import libraries) */

#define S3D_PROTO( ret, name, params ) extern ret name params;
S3D_CORONA_FUNCS( S3D_PROTO )
#if defined( S3D_OWN_GLES2 )
  #include "s3d_gles2.h" /* builds without the NDK */
#elif defined( __ANDROID__ )
  #include <GLES2/gl2.h>
#elif defined( __APPLE__ )
  #include <OpenGLES/ES2/gl.h>
#else
  #include <GL/gl.h>
#endif

#if defined( _WIN32 )
  #define S3D_EXPORT __declspec(dllexport)
#else
  #define S3D_EXPORT __attribute__((visibility("default")))
#endif

#define CORONA( name ) name
#define GLCALL( name ) name
static const char * s3d_bind_all( void ) { return NULL; }

#endif

/* ========================================================================== */
/* Lua helpers                                                                  */
/* ========================================================================== */

static int s3d_error( lua_State * L, const char * fmt, ... )
{
    char buf[512];
    va_list ap;

    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );
    lua_pushstring( L, buf );
    return lua_error( L );
}

static float s3d_optnum( lua_State * L, int i, float def )
{
    return lua_isnumber( L, i ) ? (float)lua_tonumber( L, i ) : def;
}

/* t[n] (1-based) of the table at absolute index idx */
static float s3d_tablenum( lua_State * L, int idx, int n, float def )
{
    float v;

    lua_rawgeti( L, idx, n );
    v = lua_isnumber( L, -1 ) ? (float)lua_tonumber( L, -1 ) : def;
    lua_settop( L, -2 );
    return v;
}

/* ========================================================================== */
/* Global state                                                                 */
/* ========================================================================== */

/* Sent to the render thread every time a 3D object is drawn (placed right before the draw). */
typedef struct FrameUniforms {
    float viewProj[16];
    float lightDir[4];   /* xyz: direction TOWARDS the light (normalized), w: ambient */
    float lightColor[4]; /* rgb, a: unused */
    float fog[4];        /* rgb: color, a: 1 = enabled */
    float fogRange[4];   /* x: start, y: end (distance to the camera), z: sky horizon fade */
    /* "style" uniforms (effects with kFlag_StyleU): hemisphere ambient, ambient occlusion, focus area */
    float ambSky[4];     /* rgb: ambient color from above (multiplied by lightDir.w), a: unused */
    float ambGround[4];  /* rgb: ambient color from below, a: unused */
    float aoParams[4];   /* x: baked AO strength, y: ground contact strength, z: ground contact height, w: ground y */
    float focus[4];      /* xyz: center, w: radius (world units, measured in XZ) */
    float focusParams[4];/* x: falloff width, y: saturation outside, z: color amount outside, w: 1 = on */
    float focusColor[4]; /* rgb: color outside the focus area, a: unused */
    float camPos[4];     /* camera position: fog distance on the CPU, u_S3D_Eye for reflections */
} FrameUniforms;

/* camera axes for billboards (s3d.newSprite), from the view matrix in setCamera */
static float gBillRight[3] = { 1, 0, 0 }, gBillUp[3] = { 0, 1, 0 }, gBillBack[3] = { 0, 0, 1 };
static float gBillRightY[3] = { 1, 0, 0 }, gBillBackY[3] = { 0, 0, 1 }; /* horizontal versions (upright sprites) */

typedef struct DepthState {
    unsigned char depthTest;
    unsigned char depthWrite;
    unsigned char cull;      /* 0 = none, 1 = cull back faces */
    unsigned char depthLess; /* 1 = GL_LESS (planar shadows: never blend twice) */
    unsigned char offset;    /* 1 = polygon offset towards the camera (shadow wins over the coplanar floor) */
    unsigned char pad[3];
} DepthState;

typedef struct StateCommand {
    DepthState s;
    unsigned int frame;
} StateCommand;

enum {
    kFlag_HasModel  = 1u << 0,
    kFlag_NoCull    = 1u << 1,
    kFlag_NoDepth   = 1u << 2,
    kFlag_NoDepthWr = 1u << 3,
    kFlag_Shadow    = 1u << 4,
    kFlag_NoLightU  = 1u << 5,  /* effect has no u_S3D_LightDir / u_S3D_LightColor */
    kFlag_NoFogU    = 1u << 6,  /* effect has no u_S3D_Fog / u_S3D_FogRange */
    kFlag_Caster    = 1u << 7,  /* copy drawn into the shadow map (camera = light) */
    kFlag_PassBegin = 1u << 8,  /* marker: switch to the shadow map FBO */
    kFlag_PassEnd   = 1u << 9,  /* marker: restore Solar2D's FBO */
    kFlag_NoShadowU = 1u << 10, /* effect does not sample the shadow map */
    kFlag_StyleU    = 1u << 11, /* effect has the style uniforms (u_S3D_AmbientSky ... u_S3D_FocusColor) */
    kFlag_EnvU      = 1u << 12, /* effect has the reflection uniforms (u_S3D_Eye, u_S3D_Env) */
    kFlag_Billboard = 1u << 13, /* sprite: the quad (x, y) faces the camera, centered on the model's position */
    kFlag_BillboardY= 1u << 14  /* with kFlag_Billboard: only turns around Y (stays upright: trees, characters) */
};

typedef struct UniformCommand {
    FrameUniforms f;
    float lightVP[16];
    float shadowParams[4]; /* x: enabled, y: bias, z: 1/size, w: darkness */
    float env[4];          /* reflection of this object: x: strength, y: F0, z: blur (mip bias), w: rotation (radians) */
    unsigned int flags;
} UniformCommand;

typedef struct PassCommand {
    int begin;
    int size;
} PassCommand;

#define GL_LESS                0x0201
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_STENCIL_TEST        0x0B90
#define GL_EQUAL               0x0202
#define GL_KEEP                0x1E00
#define GL_INCR                0x1E02
#define GL_STENCIL_BUFFER_BIT  0x00000400
#define GL_STENCIL_ATTACHMENT  0x8D20
#define GL_DEPTH24_STENCIL8    0x88F0
#define GL_VIEWPORT            0x0BA2
#define GL_DEPTH_BUFFER_BIT    0x00000100
#define GL_FRAMEBUFFER         0x8D40
#define GL_RENDERBUFFER        0x8D41
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DEPTH_ATTACHMENT    0x8D00
#define GL_DEPTH_COMPONENT16   0x81A5
#define GL_DEPTH_COMPONENT24   0x81A6
#define GL_COLOR_ATTACHMENT0   0x8CE0
#define GL_TEXTURE_2D          0x0DE1
#define GL_RGBA                0x1908
#define GL_UNSIGNED_BYTE       0x1401
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_NEAREST             0x2600
#define GL_CLAMP_TO_EDGE       0x812F
#define GL_COLOR_BUFFER_BIT    0x00004000
#define GL_COLOR_CLEAR_VALUE   0x0C22
#define GL_ACTIVE_TEXTURE      0x84E0
#define GL_TEXTURE0            0x84C0
#define GL_CURRENT_PROGRAM     0x8B8D
#define GL_TEXTURE_BINDING_2D  0x8069
#define S3D_SHADOW_UNIT        7

/* The extraSpace of each effect instance (allocated and zeroed by Solar2D). It must not hold heap pointers
   that need freeing, because Solar2D may memcpy an instance when cloning it. */
typedef struct Instance {
    float model[16];
    float normalMat[9];   /* inverse-transpose 3x3 of the model matrix */
    const float * normals;   /* points into a Lua userdata kept alive by the display object */
    unsigned int normalCount;
    unsigned int flags;
    /* frame animation (loadOBJAnimation): position / normal = lerp( A, B, morphT ); buffers are owned by Lua */
    const float * morphPA;
    const float * morphPB;
    const float * morphNA;
    const float * morphNB;
    unsigned int morphCount; /* number of vertices in the buffers */
    float morphT;
    /* baked ambient occlusion, one value per vertex (s3d.bakeAO); written to CoronaVertexUserData.w (1 when absent) */
    const float * ao;
    unsigned int aoCount;
    /* environment reflection (effect filter.s3d.reflect): strength, F0, blur, rotation */
    float env[4];
} Instance;

static FrameUniforms gFrame;
static unsigned long gStateBlockID, gStateCmdID, gUniformCmdID;
static const DepthState kDepthOff = { 0, 1, 0, 0, 0, { 0, 0, 0 } };
static unsigned int gFrameId = 1;
static float gShadowMat[16];
static float gShadowPlaneY = 0.0f;

/* shadow map */
static unsigned long gPassCmdID;
static float gLightVP[16];
static float gShadowParams[4] = { 0, 0.003f, 1.0f / 1024, 0.6f };
static int gShadowSize = 1024;
static float gShadowCenter[3] = { 0, 0, 0 };
static float gShadowArea = 10, gShadowDist = 30;
static unsigned int gShadowFbo, gShadowTex, gShadowRb;
static int gShadowAlloc;
static int gSavedFbo, gSavedViewport[4];
static float gSavedClear[4];


/* ========================================================================== */
/* Commands that run on the render thread                                       */
/* ========================================================================== */

/* --- Solar2D FBOs (snapshot, canvas) have no depth buffer: the plugin attaches its own renderbuffer --- */
typedef struct FboDepth {
    int fbo;
    unsigned int rb;
    int w, h;
    unsigned int frame;
    int hasStencil;
} FboDepth;

#define S3D_MAX_FBOS 16
static FboDepth gFbos[S3D_MAX_FBOS];

static int s3d_gl_fbo_procs( void )
{
#ifdef S3D_DYNAMIC
    static int tried = 0;

    if (!tried)
    {
        HMODULE gl = GetModuleHandleA( "opengl32.dll" );
        typedef PROC ( WINAPI * wglGetProcAddress_t )( LPCSTR );
        wglGetProcAddress_t getProc = gl ? (wglGetProcAddress_t)GetProcAddress( gl, "wglGetProcAddress" ) : NULL;

        tried = 1;
        if (getProc)
        {
#define S3D_GLPROC( name ) p_##name = ( name##_t )getProc( #name ); if (!p_##name) p_##name = ( name##_t )getProc( #name "EXT" );
            S3D_GLPROC( glGenRenderbuffers )
            S3D_GLPROC( glBindRenderbuffer )
            S3D_GLPROC( glRenderbufferStorage )
            S3D_GLPROC( glFramebufferRenderbuffer )
            S3D_GLPROC( glDeleteRenderbuffers )
            S3D_GLPROC( glGenFramebuffers )
            S3D_GLPROC( glBindFramebuffer )
            S3D_GLPROC( glFramebufferTexture2D )
            S3D_GLPROC( glActiveTexture )
            S3D_GLPROC( glGetUniformLocation )
            S3D_GLPROC( glUniform1i )
            S3D_GLPROC( glCheckFramebufferStatus )
            S3D_GLPROC( glIsProgram )
            S3D_GLPROC( glUseProgram )
        }
    }
    return p_glGenRenderbuffers && p_glBindRenderbuffer && p_glRenderbufferStorage && p_glFramebufferRenderbuffer && p_glDeleteRenderbuffers
        && p_glGenFramebuffers && p_glBindFramebuffer && p_glFramebufferTexture2D && p_glActiveTexture
        && p_glGetUniformLocation && p_glUniform1i && p_glIsProgram && p_glUseProgram;
#else
    return 1;
#endif
}

/* Drawing into an FBO: make sure it has depth and clear the depth once per frame. Returns 1 when in an FBO. */
static int s3d_prepare_fbo_depth( unsigned int frame )
{
    int fbo = 0, vp[4] = { 0, 0, 0, 0 }, i, slot = -1, oldest = 0;
    FboDepth * e;

    GLCALL( glGetIntegerv )( GL_FRAMEBUFFER_BINDING, &fbo );
    if (0 == fbo) return 0;
    if (gShadowFbo && (unsigned int)fbo == gShadowFbo) return 1; /* shadow pass: depth is managed by the plugin */
    if (!s3d_gl_fbo_procs()) return 1;

    GLCALL( glGetIntegerv )( GL_VIEWPORT, vp );
    for (i = 0; i < S3D_MAX_FBOS; ++i)
    {
        if (gFbos[i].fbo == fbo) { slot = i; break; }
        if (gFbos[i].frame < gFbos[oldest].frame) oldest = i;
    }
    if (slot < 0)
    {
        slot = oldest;
        if (gFbos[slot].rb) GLCALL( glDeleteRenderbuffers )( 1, &gFbos[slot].rb );
        memset( &gFbos[slot], 0, sizeof( FboDepth ) );
        gFbos[slot].fbo = fbo;
    }
    e = &gFbos[slot];

    if (e->frame == frame && e->rb) return 1; /* already prepared this frame */

    if (!e->rb || e->w != vp[2] || e->h != vp[3])
    {
        if (e->rb) GLCALL( glDeleteRenderbuffers )( 1, &e->rb );
        GLCALL( glGenRenderbuffers )( 1, &e->rb );
        GLCALL( glBindRenderbuffer )( GL_RENDERBUFFER, e->rb );
        while (GLCALL( glGetError )()) {} /* drop stale errors */
        /* prefer depth 24 + stencil 8 (stencil keeps planar shadows from blending twice), then depth 24, then depth 16 */
        e->hasStencil = 1;
        GLCALL( glRenderbufferStorage )( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, vp[2], vp[3] );
        if (GLCALL( glGetError )())
        {
            e->hasStencil = 0;
            GLCALL( glRenderbufferStorage )( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, vp[2], vp[3] );
            if (GLCALL( glGetError )())
            {
                GLCALL( glRenderbufferStorage )( GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, vp[2], vp[3] );
            }
        }
        GLCALL( glBindRenderbuffer )( GL_RENDERBUFFER, 0 );
        e->w = vp[2]; e->h = vp[3];
    }
    /* re-attach every frame: Solar2D may have reused the FBO id */
    GLCALL( glFramebufferRenderbuffer )( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e->rb );
    GLCALL( glFramebufferRenderbuffer )( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, e->hasStencil ? e->rb : 0 );
    GLCALL( glDepthMask )( 1 );
    GLCALL( glStencilMask )( 0xFF );
    GLCALL( glClearStencil )( 0 );
    GLCALL( glClear )( GL_DEPTH_BUFFER_BIT | ( e->hasStencil ? GL_STENCIL_BUFFER_BIT : 0 ) );
    e->frame = frame;
    return 1;
}

static void s3d_state_reader( const CoronaCommandBuffer * cb, const unsigned char * from, unsigned int size )
{
    StateCommand c;
    int inFbo = 0;

    (void)cb;
    if (size < sizeof( StateCommand )) return;
    memcpy( &c, from, sizeof( c ) );

    if (c.s.depthTest)
    {
        inFbo = s3d_prepare_fbo_depth( c.frame );
        GLCALL( glEnable )( GL_DEPTH_TEST );
        GLCALL( glDepthFunc )( c.s.depthLess ? GL_LESS : GL_LEQUAL );
    }
    else
    {
        GLCALL( glDisable )( GL_DEPTH_TEST );
    }
    GLCALL( glDepthMask )( c.s.depthWrite ? 1 : 0 );

    if (c.s.offset)
    {
        GLCALL( glEnable )( GL_POLYGON_OFFSET_FILL );
        GLCALL( glPolygonOffset )( -1.0f, -2.0f );
        /* each pixel receives a planar shadow only once per frame (when the framebuffer has stencil) */
        GLCALL( glEnable )( GL_STENCIL_TEST );
        GLCALL( glStencilMask )( 0xFF );
        GLCALL( glStencilFunc )( GL_EQUAL, 0, 0xFF );
        GLCALL( glStencilOp )( GL_KEEP, GL_KEEP, GL_INCR );
    }
    else
    {
        GLCALL( glDisable )( GL_POLYGON_OFFSET_FILL );
        GLCALL( glDisable )( GL_STENCIL_TEST );
    }

    if (c.s.cull)
    {
        if (!c.s.depthTest) GLCALL( glGetIntegerv )( GL_FRAMEBUFFER_BINDING, &inFbo );
        GLCALL( glEnable )( GL_CULL_FACE );
        /* inside an FBO the shader flips y, so the front-face winding flips too */
        GLCALL( glFrontFace )( inFbo ? GL_CW : GL_CCW );
        GLCALL( glCullFace )( GL_BACK );
    }
    else
    {
        GLCALL( glDisable )( GL_CULL_FACE );
    }
}

/* (Re)create the shadow map FBO: RGBA8 texture holding packed depth + a depth renderbuffer */
static int s3d_ensure_shadow_target( int size )
{
    if (!s3d_gl_fbo_procs()) return 0;
    if (gShadowFbo && gShadowAlloc == size) return 1;

    if (!gShadowFbo)
    {
        GLCALL( glGenFramebuffers )( 1, &gShadowFbo );
        GLCALL( glGenTextures )( 1, &gShadowTex );
        GLCALL( glGenRenderbuffers )( 1, &gShadowRb );
    }

    GLCALL( glBindTexture )( GL_TEXTURE_2D, gShadowTex );
    GLCALL( glTexImage2D )( GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
    GLCALL( glTexParameteri )( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    GLCALL( glTexParameteri )( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    GLCALL( glTexParameteri )( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    GLCALL( glTexParameteri )( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    GLCALL( glBindRenderbuffer )( GL_RENDERBUFFER, gShadowRb );
    while (GLCALL( glGetError )()) {}
    GLCALL( glRenderbufferStorage )( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size );
    if (GLCALL( glGetError )()) GLCALL( glRenderbufferStorage )( GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, size, size );
    GLCALL( glBindRenderbuffer )( GL_RENDERBUFFER, 0 );

    GLCALL( glBindFramebuffer )( GL_FRAMEBUFFER, gShadowFbo );
    GLCALL( glFramebufferTexture2D )( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gShadowTex, 0 );
    GLCALL( glFramebufferRenderbuffer )( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gShadowRb );
    gShadowAlloc = size;
    return 1;
}

/*
 * Set u_S3D_ShadowMap = unit 7 on every program that has this sampler.
 * This cannot be done inside the uniform command: when it runs, the program of the next draw is not bound yet
 * (e.g. the object drawn right after the shadow pass would set it on the depth program). Sampler values are stored
 * per program, so each program only needs it once; programs are rescanned to catch newly created ones.
 */
#define S3D_MAX_PROGRAM_SCAN 2048
static unsigned char gProgramDone[S3D_MAX_PROGRAM_SCAN];

static unsigned int gProgramScanFrame; /* frames since init, used to throttle the scan */
static unsigned int gProgramMaxId;     /* largest program id seen so far */

static void s3d_assign_shadow_samplers( void )
{
    int prev = 0;
    unsigned int id, limit;
    int switched = 0;

    if (!s3d_gl_fbo_procs()) return;
    /* glIsProgram is not cheap on mobile drivers: scan every frame for the first 30 frames (Solar2D is creating
       programs then), then once every 30 frames, and only up to the largest id seen + 64 */
    ++gProgramScanFrame;
    if (gProgramScanFrame > 30 && ( gProgramScanFrame % 30 ) != 0) return;
    limit = gProgramMaxId + 64;
    if (limit < 256) limit = 256;
    if (limit > S3D_MAX_PROGRAM_SCAN) limit = S3D_MAX_PROGRAM_SCAN;
    while (GLCALL( glGetError )() != 0) {}
    GLCALL( glGetIntegerv )( GL_CURRENT_PROGRAM, &prev );
    for (id = 1; id < limit; ++id)
    {
        if (!GLCALL( glIsProgram )( id )) { gProgramDone[id] = 0; continue; }
        if (id > gProgramMaxId) gProgramMaxId = id;
        if (gProgramDone[id]) continue;
        gProgramDone[id] = 1;
        {
            int loc = GLCALL( glGetUniformLocation )( id, "u_S3D_ShadowMap" );

            if (GLCALL( glGetError )() != 0) { gProgramDone[id] = 0; continue; } /* not linked yet: retry next frame */
            if (loc < 0) continue;
            GLCALL( glUseProgram )( id );
            GLCALL( glUniform1i )( loc, S3D_SHADOW_UNIT );
            switched = 1;
        }
    }
    if (switched) GLCALL( glUseProgram )( (unsigned int)prev );
    while (GLCALL( glGetError )() != 0) {} /* ignore errors from programs that are not linked yet */
}

static void s3d_pass_reader( const CoronaCommandBuffer * cb, const unsigned char * from, unsigned int size )
{
    PassCommand pc;

    (void)cb;
    if (size < sizeof( PassCommand )) return;
    memcpy( &pc, from, sizeof( pc ) );
    if (!s3d_gl_fbo_procs()) return;

    if (pc.begin)
    {
        int activeUnit = GL_TEXTURE0, boundTex = 0;

        GLCALL( glGetIntegerv )( GL_FRAMEBUFFER_BINDING, &gSavedFbo );
        GLCALL( glGetIntegerv )( GL_VIEWPORT, gSavedViewport );
        GLCALL( glGetFloatv )( GL_COLOR_CLEAR_VALUE, gSavedClear );

        /* create the texture on the shadow map unit so Solar2D's bound textures are left alone */
        GLCALL( glGetIntegerv )( GL_ACTIVE_TEXTURE, &activeUnit );
        GLCALL( glActiveTexture )( GL_TEXTURE0 + S3D_SHADOW_UNIT );
        GLCALL( glGetIntegerv )( GL_TEXTURE_BINDING_2D, &boundTex );
        if (!s3d_ensure_shadow_target( pc.size ))
        {
            GLCALL( glActiveTexture )( (unsigned int)activeUnit );
            return;
        }
        GLCALL( glBindTexture )( GL_TEXTURE_2D, 0 ); /* do not sample the texture being written */
        GLCALL( glActiveTexture )( (unsigned int)activeUnit );

        GLCALL( glBindFramebuffer )( GL_FRAMEBUFFER, gShadowFbo );
        GLCALL( glViewport )( 0, 0, pc.size, pc.size );
        GLCALL( glClearColor )( 1, 1, 1, 1 ); /* farthest depth */
        GLCALL( glDepthMask )( 1 );
        GLCALL( glClear )( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
        (void)boundTex;
    }
    else if (gShadowFbo)
    {
        int activeUnit = GL_TEXTURE0;

        GLCALL( glBindFramebuffer )( GL_FRAMEBUFFER, (unsigned int)gSavedFbo );
        GLCALL( glViewport )( gSavedViewport[0], gSavedViewport[1], gSavedViewport[2], gSavedViewport[3] );
        GLCALL( glClearColor )( gSavedClear[0], gSavedClear[1], gSavedClear[2], gSavedClear[3] );

        GLCALL( glGetIntegerv )( GL_ACTIVE_TEXTURE, &activeUnit );
        GLCALL( glActiveTexture )( GL_TEXTURE0 + S3D_SHADOW_UNIT );
        GLCALL( glBindTexture )( GL_TEXTURE_2D, gShadowTex );
        GLCALL( glActiveTexture )( (unsigned int)activeUnit );
        s3d_assign_shadow_samplers();
    }
}


static void s3d_uniform_reader( const CoronaCommandBuffer * cb, const unsigned char * from, unsigned int size )
{
    CoronaWriteUniformParams p;
    unsigned int flags;
    const unsigned char * f = from + offsetof( UniformCommand, f );

    if (size < sizeof( UniformCommand )) return;
    memcpy( &flags, from + offsetof( UniformCommand, flags ), sizeof( flags ) );
    p.useOffset = 0;
    /* copy drawn in the shadow pass: the "camera" is the light */
    p.u.data = ( flags & kFlag_Caster ) ? from + offsetof( UniformCommand, lightVP ) : f + offsetof( FrameUniforms, viewProj );
    CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_ViewProj", &p, 16 * sizeof( float ) );
    if (( flags & kFlag_Caster ) || !( flags & kFlag_NoShadowU ))
    {
        p.u.data = from + offsetof( UniformCommand, lightVP );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_LightVP", &p, 16 * sizeof( float ) );
    }
    if (!( flags & kFlag_Caster ) && !( flags & kFlag_NoShadowU ))
    {
        p.u.data = from + offsetof( UniformCommand, shadowParams );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_ShadowParams", &p, 4 * sizeof( float ) );
    }
    if (!( flags & kFlag_NoLightU ))
    {
        p.u.data = f + offsetof( FrameUniforms, lightDir );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_LightDir", &p, 4 * sizeof( float ) );
        p.u.data = f + offsetof( FrameUniforms, lightColor );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_LightColor", &p, 4 * sizeof( float ) );
    }
    if (!( flags & kFlag_NoFogU ))
    {
        p.u.data = f + offsetof( FrameUniforms, fog );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_Fog", &p, 4 * sizeof( float ) );
        p.u.data = f + offsetof( FrameUniforms, fogRange );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_FogRange", &p, 4 * sizeof( float ) );
    }
    if (!( flags & kFlag_Caster ) && ( flags & kFlag_StyleU ))
    {
        static const struct { const char * name; size_t offset; } kStyle[] = {
            { "u_S3D_AmbientSky", offsetof( FrameUniforms, ambSky ) },
            { "u_S3D_AmbientGround", offsetof( FrameUniforms, ambGround ) },
            { "u_S3D_AOParams", offsetof( FrameUniforms, aoParams ) },
            { "u_S3D_Focus", offsetof( FrameUniforms, focus ) },
            { "u_S3D_FocusParams", offsetof( FrameUniforms, focusParams ) },
            { "u_S3D_FocusColor", offsetof( FrameUniforms, focusColor ) },
        };
        unsigned int i;

        for (i = 0; i < sizeof( kStyle ) / sizeof( kStyle[0] ); ++i)
        {
            p.u.data = f + kStyle[i].offset;
            CORONA( CoronaCommandBufferWriteNamedUniform )( cb, kStyle[i].name, &p, 4 * sizeof( float ) );
        }
    }
    if (!( flags & kFlag_Caster ) && ( flags & kFlag_EnvU ))
    {
        p.u.data = f + offsetof( FrameUniforms, camPos );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_Eye", &p, 4 * sizeof( float ) );
        p.u.data = from + offsetof( UniformCommand, env );
        CORONA( CoronaCommandBufferWriteNamedUniform )( cb, "u_S3D_Env", &p, 4 * sizeof( float ) );
    }
}

/* State block handler: called while the command buffer is built; issues a command so the render thread changes GL state. */
static void s3d_state_dirty( const CoronaCommandBuffer * cb, const CoronaRenderer * renderer,
    const void * newContents, const void * oldContents, unsigned int size, int restore, void * userData )
{
    StateCommand c;

    (void)cb; (void)oldContents; (void)size; (void)restore; (void)userData;
    memcpy( &c.s, newContents, sizeof( DepthState ) );
    c.frame = gFrameId;
    CORONA( CoronaRendererIssueCommand )( renderer, gStateCmdID, &c, sizeof( c ) );
}

/* ========================================================================== */
/* Geometry writer                                                              */
/* ========================================================================== */

/* "position", update kind: dest already holds the original coordinates (x, y, z); multiply by the model matrix. */
static void s3d_position_writer( void * dest, const void * context, const CoronaGeometryMappingLayout * layout,
    unsigned int index, unsigned int n )
{
    const Instance * inst = (const Instance *)context;
    unsigned char * d = (unsigned char *)dest;
    unsigned int k;

    for (k = 0; k < n; ++k, d += layout->outStride)
    {
        float * p = (float *)d;
        unsigned int vi = index + k;
        vec4 in, out;

        if (inst->morphPA && vi < inst->morphCount)
        {
            const float * a = inst->morphPA + 3 * vi;
            const float * b = inst->morphPB + 3 * vi;
            float t = inst->morphT;

            in[0] = a[0] + ( b[0] - a[0] ) * t;
            in[1] = a[1] + ( b[1] - a[1] ) * t;
            in[2] = a[2] + ( b[2] - a[2] ) * t;
        }
        else
        {
            in[0] = p[0]; in[1] = p[1]; in[2] = p[2];
        }
        in[3] = 1.0f;
        if (inst->flags & kFlag_Billboard)
        {
            /* sprite: (x, y) of the quad along the camera's right and up axes, scaled like the model */
            const float * m = inst->model;
            const int upright = ( inst->flags & kFlag_BillboardY ) != 0;
            const float * r = upright ? gBillRightY : gBillRight;
            static const float kUp[3] = { 0.0f, 1.0f, 0.0f };
            const float * u = upright ? kUp : gBillUp;
            float sx = 1.0f, sy = 1.0f, c[3] = { 0.0f, 0.0f, 0.0f };
            int j;

            if (inst->flags & kFlag_HasModel)
            {
                c[0] = m[12]; c[1] = m[13]; c[2] = m[14];
                sx = sqrtf( m[0] * m[0] + m[1] * m[1] + m[2] * m[2] );
                sy = sqrtf( m[4] * m[4] + m[5] * m[5] + m[6] * m[6] );
            }
            for (j = 0; j < 3; ++j) out[j] = c[j] + r[j] * in[0] * sx + u[j] * in[1] * sy;
            out[3] = 1.0f;
        }
        else if (inst->flags & kFlag_HasModel)
        {
            glm_mat4_mulv( *(mat4 *)inst->model, in, out );
        }
        else
        {
            glm_vec4_copy( in, out );
        }

        if (inst->flags & kFlag_Shadow)
        {
            /* project onto the plane y = planeY along the light direction, lifted slightly to avoid z-fighting with the floor */
            vec4 s;

            glm_mat4_mulv( *(mat4 *)gShadowMat, out, s );
            if (s[3] != 0.0f) glm_vec4_scale( s, 1.0f / s[3], s );
            out[0] = s[0]; out[1] = gShadowPlaneY + 0.0005f; /* polygon offset handles the rest */ out[2] = s[2];
        }

        p[0] = out[0]; p[1] = out[1]; p[2] = out[2];

    }
}

/* "userData" (ux, uy, uz, uw): transformed normal in CoronaVertexUserData.xyz, baked ambient occlusion in .w (1 = open) */
static void s3d_normal_writer( void * dest, const void * context, const CoronaGeometryMappingLayout * layout,
    unsigned int index, unsigned int n )
{
    const Instance * inst = (const Instance *)context;
    unsigned char * d = (unsigned char *)dest;
    unsigned int k;

    for (k = 0; k < n; ++k, d += layout->outStride)
    {
        float * u = (float *)d;
        unsigned int vi = index + k;
        vec3 nIn = { 0.0f, 0.0f, 1.0f }, nOut;

        if (inst->morphNA && vi < inst->morphCount)
        {
            const float * a = inst->morphNA + 3 * vi;
            const float * b = inst->morphNB + 3 * vi;
            float t = inst->morphT;

            nIn[0] = a[0] + ( b[0] - a[0] ) * t;
            nIn[1] = a[1] + ( b[1] - a[1] ) * t;
            nIn[2] = a[2] + ( b[2] - a[2] ) * t;
            glm_vec3_normalize( nIn );
        }
        else if (inst->normals && vi < inst->normalCount)
        {
            nIn[0] = inst->normals[3 * vi];
            nIn[1] = inst->normals[3 * vi + 1];
            nIn[2] = inst->normals[3 * vi + 2];
        }
        if (inst->flags & kFlag_Billboard)
        {
            glm_vec3_copy( ( inst->flags & kFlag_BillboardY ) ? gBillBackY : gBillBack, nOut ); /* faces the camera */
        }
        else if (inst->flags & kFlag_HasModel)
        {
            glm_mat3_mulv( *(mat3 *)inst->normalMat, nIn, nOut );
        }
        else
        {
            glm_vec3_copy( nIn, nOut );
        }
        u[0] = nOut[0]; u[1] = nOut[1]; u[2] = nOut[2];
        u[3] = ( inst->ao && vi < inst->aoCount ) ? inst->ao[vi] : 1.0f;
    }
}

/* ========================================================================== */
/* Effect data type                                                             */
/* ========================================================================== */

static void s3d_before( const CoronaShader * shader, void * userData, const CoronaRenderer * renderer, const CoronaRenderData * renderData )
{
    Instance * inst = (Instance *)userData;
    DepthState on;

    if (inst->flags & ( kFlag_PassBegin | kFlag_PassEnd ))
    {
        /* Marker: draw a degenerate triangle so Solar2D flushes pending draws, then switch the FBO */
        PassCommand pc;

        CORONA( CoronaRendererWriteStateBlock )( renderer, gStateBlockID, &kDepthOff, sizeof( kDepthOff ) );
        CORONA( CoronaShaderRawDraw )( shader, renderData, renderer );
        if (!gShadowParams[0]) return;
        pc.begin = ( inst->flags & kFlag_PassBegin ) ? 1 : 0;
        pc.size = gShadowSize;
        CORONA( CoronaRendererIssueCommand )( renderer, gPassCmdID, &pc, sizeof( pc ) );
        return;
    }
    if (( inst->flags & kFlag_Caster ) && !gShadowParams[0]) return; /* shadow map is off */

    on.depthTest = ( inst->flags & kFlag_NoDepth ) ? 0 : 1;
    on.depthWrite = ( inst->flags & kFlag_NoDepthWr ) ? 0 : 1;
    on.cull = ( inst->flags & kFlag_NoCull ) ? 0 : 1;
    on.depthLess = ( inst->flags & kFlag_Shadow ) ? 1 : 0;
    on.offset = on.depthLess;
    on.pad[0] = on.pad[1] = on.pad[2] = 0;

    /* writers only apply inside before(), so the object must be drawn here (ignoreOriginal = 1) */
    CORONA( CoronaGeometrySetComponentWriter )( renderer, "position", s3d_position_writer, inst, 1 );
    CORONA( CoronaGeometrySetComponentWriter )( renderer, "userData", s3d_normal_writer, inst, 1 );
    CORONA( CoronaRendererWriteStateBlock )( renderer, gStateBlockID, &on, sizeof( on ) );

    CORONA( CoronaShaderRawDraw )( shader, renderData, renderer );

    /* After RawDraw the object's shader is bound and the draw is still waiting to be flushed, so the uniforms arrive in time. */
    {
        UniformCommand uc;

        uc.f = gFrame;
        memcpy( uc.lightVP, gLightVP, sizeof( uc.lightVP ) );
        memcpy( uc.shadowParams, gShadowParams, sizeof( uc.shadowParams ) );
        uc.flags = inst->flags;
        memcpy( uc.env, inst->env, sizeof( uc.env ) );
        CORONA( CoronaRendererIssueCommand )( renderer, gUniformCmdID, &uc, sizeof( uc ) );
    }

    /* restore the default state so 2D objects drawn later are not affected */
    CORONA( CoronaRendererWriteStateBlock )( renderer, gStateBlockID, &kDepthOff, sizeof( kDepthOff ) );
}

enum { kData_TRS = 0, kData_Normals, kData_Flags, kData_Matrix, kData_Pose, kData_Morph, kData_AO, kData_Env };

static int s3d_data_index( const char * name )
{
    if (0 == strcmp( name, "s3dTRS" )) return kData_TRS;
    if (0 == strcmp( name, "s3dNormals" )) return kData_Normals;
    if (0 == strcmp( name, "s3dFlags" )) return kData_Flags;
    if (0 == strcmp( name, "s3dMatrix" )) return kData_Matrix;
    if (0 == strcmp( name, "s3dPose" )) return kData_Pose;
    if (0 == strcmp( name, "s3dMorph" )) return kData_Morph;
    if (0 == strcmp( name, "s3dAO" )) return kData_AO;
    if (0 == strcmp( name, "s3dEnv" )) return kData_Env;
    return -1;
}

static void s3d_update_normal_matrix( Instance * inst )
{
    mat3 m3;

    glm_mat4_pick3( *(mat4 *)inst->model, m3 );
    glm_mat3_inv( m3, m3 );
    glm_mat3_transpose( m3 );
    memcpy( inst->normalMat, m3, sizeof( inst->normalMat ) );
}

static int s3d_get_data( lua_State * L, int dataIndex, void * userData, int * hadError )
{
    const Instance * inst = (const Instance *)userData;

    (void)hadError;
    if (kData_Flags == dataIndex)
    {
        lua_pushnumber( L, inst->flags );
        return 1;
    }
    return 0;
}

static int s3d_set_data( lua_State * L, int dataIndex, int valueIndex, void * userData, int * shouldInvalidate, int * hadError )
{
    Instance * inst = (Instance *)userData;

    if (valueIndex < 0) valueIndex = lua_gettop( L ) + valueIndex + 1;

    switch (dataIndex)
    {
    case kData_TRS: /* { x, y, z, rx, ry, rz (degrees), sx, sy, sz } */
        if (LUA_TTABLE != lua_type( L, valueIndex ))
        {
            *hadError = 1;
            lua_pushstring( L, "s3dTRS must be a table of 9 numbers" );
            return 1;
        }
        else
        {
            vec3 t, r, s;
            mat4 m, rot;

            t[0] = s3d_tablenum( L, valueIndex, 1, 0 ); t[1] = s3d_tablenum( L, valueIndex, 2, 0 ); t[2] = s3d_tablenum( L, valueIndex, 3, 0 );
            r[0] = glm_rad( s3d_tablenum( L, valueIndex, 4, 0 ) );
            r[1] = glm_rad( s3d_tablenum( L, valueIndex, 5, 0 ) );
            r[2] = glm_rad( s3d_tablenum( L, valueIndex, 6, 0 ) );
            s[0] = s3d_tablenum( L, valueIndex, 7, 1 ); s[1] = s3d_tablenum( L, valueIndex, 8, 1 ); s[2] = s3d_tablenum( L, valueIndex, 9, 1 );

            /* model = T * R(yaw y, pitch x, roll z) * S */
            glm_translate_make( m, t );
            glm_euler_yxz( r, rot );
            glm_mat4_mul( m, rot, m );
            glm_scale( m, s );
            memcpy( inst->model, m, sizeof( inst->model ) );
            inst->flags |= kFlag_HasModel;
            s3d_update_normal_matrix( inst );
        }
        break;
    case kData_Pose: /* { x, y, z, qx, qy, qz, qw, sx, sy, sz }: rotation as a quaternion (e.g. from physics) */
        if (LUA_TTABLE != lua_type( L, valueIndex ))
        {
            *hadError = 1;
            lua_pushstring( L, "s3dPose must be a table of 10 numbers" );
            return 1;
        }
        else
        {
            vec3 t, s;
            versor q;
            mat4 m, rot;

            t[0] = s3d_tablenum( L, valueIndex, 1, 0 ); t[1] = s3d_tablenum( L, valueIndex, 2, 0 ); t[2] = s3d_tablenum( L, valueIndex, 3, 0 );
            q[0] = s3d_tablenum( L, valueIndex, 4, 0 ); q[1] = s3d_tablenum( L, valueIndex, 5, 0 );
            q[2] = s3d_tablenum( L, valueIndex, 6, 0 ); q[3] = s3d_tablenum( L, valueIndex, 7, 1 );
            s[0] = s3d_tablenum( L, valueIndex, 8, 1 ); s[1] = s3d_tablenum( L, valueIndex, 9, 1 ); s[2] = s3d_tablenum( L, valueIndex, 10, 1 );
            glm_quat_normalize( q );
            glm_translate_make( m, t );
            glm_quat_mat4( q, rot );
            glm_mat4_mul( m, rot, m );
            glm_scale( m, s );
            memcpy( inst->model, m, sizeof( inst->model ) );
            inst->flags |= kFlag_HasModel;
            s3d_update_normal_matrix( inst );
        }
        break;
    case kData_Matrix: /* 16 numbers, column-major */
        if (LUA_TTABLE != lua_type( L, valueIndex ))
        {
            *hadError = 1;
            lua_pushstring( L, "s3dMatrix must be a table of 16 numbers" );
            return 1;
        }
        else
        {
            int i;

            for (i = 0; i < 16; ++i) inst->model[i] = s3d_tablenum( L, valueIndex, i + 1, ( i % 5 ) ? 0.0f : 1.0f );
            inst->flags |= kFlag_HasModel;
            s3d_update_normal_matrix( inst );
        }
        break;
    case kData_Morph: /* { posA, posB, t, nrmA, nrmB } (buffers from s3d.newBuffer) or false to turn it off */
        if (LUA_TTABLE != lua_type( L, valueIndex ))
        {
            inst->morphPA = inst->morphPB = inst->morphNA = inst->morphNB = NULL;
            inst->morphCount = 0;
        }
        else
        {
            const float * pa;
            const float * pb;
            unsigned int na, nb;

            lua_rawgeti( L, valueIndex, 1 ); pa = (const float *)lua_touserdata( L, -1 ); na = pa ? (unsigned int)( lua_objlen( L, -1 ) / ( 3 * sizeof( float ) ) ) : 0; lua_pop( L, 1 );
            lua_rawgeti( L, valueIndex, 2 ); pb = (const float *)lua_touserdata( L, -1 ); nb = pb ? (unsigned int)( lua_objlen( L, -1 ) / ( 3 * sizeof( float ) ) ) : 0; lua_pop( L, 1 );
            if (!pa || !pb || na != nb)
            {
                *hadError = 1;
                lua_pushstring( L, "s3dMorph: needs two position buffers with the same vertex count" );
                return 1;
            }
            inst->morphPA = pa;
            inst->morphPB = pb;
            inst->morphCount = na;
            inst->morphT = s3d_tablenum( L, valueIndex, 3, 0 );
            lua_rawgeti( L, valueIndex, 4 ); inst->morphNA = (const float *)lua_touserdata( L, -1 ); lua_pop( L, 1 );
            lua_rawgeti( L, valueIndex, 5 ); inst->morphNB = (const float *)lua_touserdata( L, -1 ); lua_pop( L, 1 );
            if (!inst->morphNA || !inst->morphNB) inst->morphNA = inst->morphNB = NULL;
        }
        break;
    case kData_Normals: /* userdata from s3d.newBuffer */
        inst->normals = (const float *)lua_touserdata( L, valueIndex );
        inst->normalCount = inst->normals ? (unsigned int)( lua_objlen( L, valueIndex ) / ( 3 * sizeof( float ) ) ) : 0;
        break;
    case kData_AO: /* userdata from s3d.bakeAO (one float per vertex), or false */
        inst->ao = (const float *)lua_touserdata( L, valueIndex );
        inst->aoCount = inst->ao ? (unsigned int)( lua_objlen( L, valueIndex ) / sizeof( float ) ) : 0;
        break;
    case kData_Flags:
        inst->flags = ( inst->flags & kFlag_HasModel ) | ( (unsigned int)lua_tonumber( L, valueIndex ) & ~kFlag_HasModel );
        break;
    case kData_Env: /* { strength, F0, blur, rotation (radians) } */
        if (LUA_TTABLE != lua_type( L, valueIndex )) return 0;
        inst->env[0] = s3d_tablenum( L, valueIndex, 1, 0.5f );
        inst->env[1] = s3d_tablenum( L, valueIndex, 2, 0.04f );
        inst->env[2] = s3d_tablenum( L, valueIndex, 3, 0.0f );
        inst->env[3] = s3d_tablenum( L, valueIndex, 4, 0.0f );
        break;
    default:
        return 0;
    }

    *shouldInvalidate = 1;
    return 1;
}

/* ========================================================================== */
/* Shell transform                                                              */
/* ========================================================================== */

/* replace the first occurrence of `from` with `to`; returns a new (malloc'ed) string or NULL */
static char * s3d_replace( const char * src, const char * from, const char * to )
{
    const char * at = strstr( src, from );
    size_t a, lf, lt, ls;
    char * out;

    if (!at) return NULL;
    a = (size_t)( at - src ); lf = strlen( from ); lt = strlen( to ); ls = strlen( src );
    out = (char *)malloc( ls - lf + lt + 1 );
    if (!out) return NULL;
    memcpy( out, src, a );
    memcpy( out + a, to, lt );
    memcpy( out + a + lt, at + lf, ls - a - lf + 1 );
    return out;
}

static char * s3d_transform_vertex( const char * src, int instanced )
{
    /* When drawing into an FBO (snapshot/canvas), Solar2D's 2D projection flips y (m[1][1] > 0): flip to match */
    /* Light space (shadow map) is not flipped: casters (ViewProj == LightVP) draw straight into the plugin's FBO */
    static const char * kPosition = "gl_Position = u_S3D_ViewProj * vec4( a_Position, 1.0 );\n"
        "    bool s3dCaster = u_S3D_ViewProj[0] == u_S3D_LightVP[0] && u_S3D_ViewProj[3] == u_S3D_LightVP[3];\n"
        "    if ( !s3dCaster && u_ViewProjectionMatrix[1][1] > 0.0 ) { gl_Position.y = -gl_Position.y; }\n"
        "    v_S3D_ShadowPos = u_S3D_LightVP * vec4( a_Position, 1.0 );\n"
        "    v_S3D_Depth = gl_Position.w;\n"
        "    v_S3D_World = a_Position;";
    /* Instanced (vertex extension "s3dInst", s3d.newInstances): a_Position / a_UserData are in the model's own space;
       each instance has a position + uniform scale (a_s3dInstPose), a rotation quaternion (a_s3dInstRot) and a color
       (a_s3dInstColor). v' = v + w t + q x t, t = 2 q x v rotates by a unit quaternion. */
    static const char * kPositionInstanced =
        "vec3 s3dL = a_Position * a_s3dInstPose.w;\n"
        "    vec3 s3dT = 2.0 * cross( a_s3dInstRot.xyz, s3dL );\n"
        "    vec3 s3dW = s3dL + a_s3dInstRot.w * s3dT + cross( a_s3dInstRot.xyz, s3dT ) + a_s3dInstPose.xyz;\n"
        "    vec3 s3dN = 2.0 * cross( a_s3dInstRot.xyz, a_UserData.xyz );\n"
        "    v_UserData.xyz = a_UserData.xyz + a_s3dInstRot.w * s3dN + cross( a_s3dInstRot.xyz, s3dN );\n"
        "    v_ColorScale = a_ColorScale * vec4( a_s3dInstColor.rgb * a_s3dInstColor.a, a_s3dInstColor.a );\n"
        "    gl_Position = u_S3D_ViewProj * vec4( s3dW, 1.0 );\n"
        "    bool s3dCaster = u_S3D_ViewProj[0] == u_S3D_LightVP[0] && u_S3D_ViewProj[3] == u_S3D_LightVP[3];\n"
        "    if ( !s3dCaster && u_ViewProjectionMatrix[1][1] > 0.0 ) { gl_Position.y = -gl_Position.y; }\n"
        "    v_S3D_ShadowPos = u_S3D_LightVP * vec4( s3dW, 1.0 );\n"
        "    v_S3D_Depth = gl_Position.w;\n"
        "    v_S3D_World = s3dW;";
    const char * position = instanced ? kPositionInstanced : kPosition;
    char * a, * b, * c, * d, * gp, * semi;
    size_t pre, total;

    a = s3d_replace( src, "attribute vec2 a_Position;", "attribute vec3 a_Position;" );
    if (!a) return NULL;
    b = s3d_replace( a, "VertexKernel( a_Position )", "VertexKernel( a_Position.xy )" );
    free( a );
    if (!b) return NULL;
    c = s3d_replace( b, "void main()", "" ); /* make sure main exists */
    if (!c) { free( b ); return NULL; }
    free( c );

    /* insert the uniform declarations right before "void main()" */
    c = s3d_replace( b, "void main()", "uniform P_DEFAULT mat4 u_S3D_ViewProj;\nuniform P_DEFAULT mat4 u_S3D_LightVP;\n"
        "varying P_DEFAULT vec4 v_S3D_ShadowPos;\nvarying P_DEFAULT float v_S3D_Depth;\nvarying P_DEFAULT vec3 v_S3D_World;\nvoid main()" );
    free( b );
    if (!c) return NULL;

    /* replace the final gl_Position assignment */
    gp = NULL;
    {
        char * p = strstr( c, "gl_Position" );

        while (p) { gp = p; p = strstr( p + 1, "gl_Position" ); }
    }
    if (!gp) { free( c ); return NULL; }
    semi = strchr( gp, ';' );
    if (!semi) { free( c ); return NULL; }
    pre = (size_t)( gp - c );
    total = pre + strlen( position ) + strlen( semi + 1 ) + 1;
    d = (char *)malloc( total );
    if (!d) { free( c ); return NULL; }
    memcpy( d, c, pre );
    strcpy( d + pre, position );
    strcat( d + pre, semi + 1 );
    free( c );
    return d;
}

static const char ** s3d_shell_begin( CoronaShellTransformParams * params, void * workSpace, void * key )
{
    char ** slot = (char **)workSpace;
    unsigned int i;

    (void)key;
    *slot = NULL;
    if (0 == strcmp( params->type, "fragment" ))
    {
        for (i = 0; i < params->nsources; ++i)
        {
            if (params->hints[i] && 0 == strcmp( params->hints[i], "fragmentSource" ))
            {
                char * out = s3d_replace( params->sources[i], "void main()", "varying P_DEFAULT vec4 v_S3D_ShadowPos;\nvarying P_DEFAULT float v_S3D_Depth;\nvarying P_DEFAULT vec3 v_S3D_World;\nvoid main()" );

                if (out) { *slot = out; params->sources[i] = out; }
                break;
            }
        }
        return params->sources;
    }
    if (0 != strcmp( params->type, "vertex" )) return params->sources;

    {
        int instanced = 0;

        /* effects using the "s3dInst" vertex extension get its attributes declared in their own source segment */
        for (i = 0; i < params->nsources; ++i)
        {
            if (params->hints[i] && 0 == strcmp( params->hints[i], "extensionAttributes" ) && params->sources[i]
                && strstr( params->sources[i], "a_s3dInstPose" ))
            {
                instanced = 1;
            }
        }
    for (i = 0; i < params->nsources; ++i)
    {
        if (params->hints[i] && 0 == strcmp( params->hints[i], "vertexSource" ))
        {
            char * out = s3d_transform_vertex( params->sources[i], instanced );

            if (out)
            {
                *slot = out;
                params->sources[i] = out;
            }
            else
            {
                printf( "[s3d] shell transform: unrecognized shell shader (backend is not OpenGL?)\n" );
            }
            break;
        }
    }
    }
    return params->sources;
}

static void s3d_shell_finish( void * workSpace, void * key )
{
    char ** slot = (char **)workSpace;

    (void)key;
    free( *slot );
    *slot = NULL;
}

/* ========================================================================== */
/* Lua functions                                                                */
/* ========================================================================== */

static CoronaEffectCallbacks gCallbacks;
static CoronaShellTransform gShell;

/* s3d._init(): registers the shell transform, data type, state block and commands. Call before defineEffect. */
static int s3d_l_init( lua_State * L )
{
    CoronaStateBlock block;
    CoronaCommand cmd;

    /* style defaults: white ambient from every side, AO at full strength, no ground contact, no focus area */
    {
        int i;

        for (i = 0; i < 3; ++i) gFrame.ambSky[i] = gFrame.ambGround[i] = gFrame.focusColor[i] = 1.0f;
        gFrame.aoParams[0] = 1.0f; gFrame.aoParams[1] = 0.0f; gFrame.aoParams[2] = 1.0f; gFrame.aoParams[3] = 0.0f;
        gFrame.focusParams[0] = 1.0f; gFrame.focusParams[1] = 1.0f; gFrame.focusParams[2] = 0.0f; gFrame.focusParams[3] = 0.0f;
    }

    memset( &gShell, 0, sizeof( gShell ) );
    gShell.size = sizeof( CoronaShellTransform );
    gShell.workSpace = sizeof( char * );
    gShell.begin = s3d_shell_begin;
    gShell.finish = s3d_shell_finish;
    if (!CORONA( CoronaShaderRegisterShellTransform )( L, "s3d", &gShell ))
    {
        /* already registered in this Lua state: skip */
    }

    memset( &gCallbacks, 0, sizeof( gCallbacks ) );
    gCallbacks.size = sizeof( CoronaEffectCallbacks );
    gCallbacks.drawParams.ignoreOriginal = 1;
    gCallbacks.drawParams.before = s3d_before;
    gCallbacks.getDataIndex = s3d_data_index;
    gCallbacks.getData = s3d_get_data;
    gCallbacks.setData = s3d_set_data;
    gCallbacks.extraSpace = sizeof( Instance );
    CORONA( CoronaShaderRegisterEffectDataType )( L, "s3d", &gCallbacks );

    memset( &cmd, 0, sizeof( cmd ) );
    cmd.reader = s3d_state_reader;
    if (!CORONA( CoronaRendererRegisterCommand )( L, &cmd, &gStateCmdID )) return s3d_error( L, "s3d: could not register the state command" );

    cmd.reader = s3d_uniform_reader;
    if (!CORONA( CoronaRendererRegisterCommand )( L, &cmd, &gUniformCmdID )) return s3d_error( L, "s3d: could not register the uniform command" );

    cmd.reader = s3d_pass_reader;
    if (!CORONA( CoronaRendererRegisterCommand )( L, &cmd, &gPassCmdID )) return s3d_error( L, "s3d: could not register the shadow pass command" );

    /* new Lua state (relaunch): GL objects from the old context may be gone */
    gShadowFbo = gShadowTex = gShadowRb = 0;
    memset( gProgramDone, 0, sizeof( gProgramDone ) );
    gProgramScanFrame = 0;
    gProgramMaxId = 0;
    gShadowAlloc = 0;
    memset( gFbos, 0, sizeof( gFbos ) );

    memset( &block, 0, sizeof( block ) );
    block.blockSize = sizeof( DepthState );
    block.defaultContents = (void *)&kDepthOff;
    block.stateDirty = s3d_state_dirty;
    if (!CORONA( CoronaRendererRegisterStateBlock )( L, &block, &gStateBlockID )) return s3d_error( L, "s3d: could not register the state block" );

    lua_pushboolean( L, 1 );
    return 1;
}

/* Planar shadow matrix for a directional light: M = (P.L) I - L P^T, P = (0, 1, 0, -y), L = (direction to the light, 0) */
static void s3d_update_shadow_matrix( void )
{
    float P[4], Lv[4], dot;
    int r, c;

    P[0] = 0; P[1] = 1; P[2] = 0; P[3] = -gShadowPlaneY;
    Lv[0] = gFrame.lightDir[0]; Lv[1] = gFrame.lightDir[1]; Lv[2] = gFrame.lightDir[2]; Lv[3] = 0;
    if (Lv[1] < 0.05f) Lv[1] = 0.05f; /* light below the plane: avoid dividing by zero */
    dot = P[0] * Lv[0] + P[1] * Lv[1] + P[2] * Lv[2] + P[3] * Lv[3];
    for (c = 0; c < 4; ++c)
        for (r = 0; r < 4; ++r)
            gShadowMat[c * 4 + r] = ( r == c ? dot : 0.0f ) - Lv[r] * P[c]; /* column-major */
}

/* Light matrix for the shadow map: ortho around the center, looking along the light direction */
static void s3d_update_light_vp( void )
{
    vec3 dir, eye, center, up = { 0, 1, 0 };
    mat4 view, proj, vp;

    dir[0] = gFrame.lightDir[0]; dir[1] = gFrame.lightDir[1]; dir[2] = gFrame.lightDir[2]; /* direction to the light */
    if (glm_vec3_norm( dir ) < 1e-6f) { dir[0] = 0; dir[1] = 1; dir[2] = 0; }
    glm_vec3_normalize( dir );
    if (fabsf( dir[1] ) > 0.99f) { up[0] = 0; up[1] = 0; up[2] = 1; }
    glm_vec3_copy( gShadowCenter, center );
    glm_vec3_scale( dir, gShadowDist, eye );
    glm_vec3_add( center, eye, eye );
    glm_lookat( eye, center, up, view );
    glm_ortho( -gShadowArea, gShadowArea, -gShadowArea, gShadowArea, 0.05f, gShadowDist * 2.0f, proj );
    glm_mat4_mul( proj, view, vp );
    memcpy( gLightVP, vp, sizeof( gLightVP ) );
}

/* s3d._setShadowMap( enabled, size, area, cx, cy, cz, distance, bias, darkness, quality, softness ) */
static int s3d_l_set_shadow_map( lua_State * L )
{
    int size = (int)s3d_optnum( L, 2, 1024 );

    if (size < 64) size = 64;
    if (size > 4096) size = 4096;
    gShadowSize = size;
    gShadowArea = s3d_optnum( L, 3, 10 );
    gShadowCenter[0] = s3d_optnum( L, 4, 0 ); gShadowCenter[1] = s3d_optnum( L, 5, 0 ); gShadowCenter[2] = s3d_optnum( L, 6, 0 );
    gShadowDist = s3d_optnum( L, 7, 30 );
    {
        /* x: 0 = off, 1/2/3/4 = filter quality (1, 4, 9, 16 texture reads) */
        int quality = (int)s3d_optnum( L, 10, 3 );

        if (quality < 1) quality = 1;
        if (quality > 4) quality = 4;
        gShadowParams[0] = lua_toboolean( L, 1 ) ? (float)quality : 0.0f;
    }
    gShadowParams[1] = s3d_optnum( L, 8, 0.003f );
    /* z: distance between PCF taps in shadow-map UV (softness 1 = one texel) */
    gShadowParams[2] = s3d_optnum( L, 11, 1.0f ) / (float)size;
    gShadowParams[3] = s3d_optnum( L, 9, 0.6f );
    s3d_update_light_vp();
    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}


/* s3d._setAmbient( skyR, skyG, skyB, groundR, groundG, groundB, aoStrength, contactStrength, contactHeight, groundY ) */
static int s3d_l_set_ambient( lua_State * L )
{
    gFrame.ambSky[0] = s3d_optnum( L, 1, 1 ); gFrame.ambSky[1] = s3d_optnum( L, 2, 1 ); gFrame.ambSky[2] = s3d_optnum( L, 3, 1 );
    gFrame.ambGround[0] = s3d_optnum( L, 4, 1 ); gFrame.ambGround[1] = s3d_optnum( L, 5, 1 ); gFrame.ambGround[2] = s3d_optnum( L, 6, 1 );
    gFrame.aoParams[0] = s3d_optnum( L, 7, 1 );
    gFrame.aoParams[1] = s3d_optnum( L, 8, 0 );
    gFrame.aoParams[2] = s3d_optnum( L, 9, 1 );
    gFrame.aoParams[3] = s3d_optnum( L, 10, 0 );
    if (gFrame.aoParams[2] < 0.001f) gFrame.aoParams[2] = 0.001f;
    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* s3d._setFocus( enabled, cx, cy, cz, radius, falloff, saturation, amount, r, g, b ) */
static int s3d_l_set_focus( lua_State * L )
{
    gFrame.focusParams[3] = lua_toboolean( L, 1 ) ? 1.0f : 0.0f;
    gFrame.focus[0] = s3d_optnum( L, 2, 0 ); gFrame.focus[1] = s3d_optnum( L, 3, 0 ); gFrame.focus[2] = s3d_optnum( L, 4, 0 );
    gFrame.focus[3] = s3d_optnum( L, 5, 10 );
    gFrame.focusParams[0] = s3d_optnum( L, 6, 5 );
    gFrame.focusParams[1] = s3d_optnum( L, 7, 0 );
    gFrame.focusParams[2] = s3d_optnum( L, 8, 0.5f );
    gFrame.focusColor[0] = s3d_optnum( L, 9, 0.7f ); gFrame.focusColor[1] = s3d_optnum( L, 10, 0.7f ); gFrame.focusColor[2] = s3d_optnum( L, 11, 0.8f );
    if (gFrame.focusParams[0] < 0.001f) gFrame.focusParams[0] = 0.001f;
    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* Möller-Trumbore: distance along the ray to the triangle, or -1 */
static float s3d_ray_tri( const float * o, const float * d, const float * a, const float * b, const float * c )
{
    float e1[3], e2[3], pv[3], tv[3], qv[3], det, inv, u, v, t;

    e1[0] = b[0] - a[0]; e1[1] = b[1] - a[1]; e1[2] = b[2] - a[2];
    e2[0] = c[0] - a[0]; e2[1] = c[1] - a[1]; e2[2] = c[2] - a[2];
    pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
    det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (det > -1e-8f && det < 1e-8f) return -1.0f; /* parallel; both sides count (models are not always closed) */
    inv = 1.0f / det;
    tv[0] = o[0] - a[0]; tv[1] = o[1] - a[1]; tv[2] = o[2] - a[2];
    u = ( tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2] ) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
    v = ( d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2] ) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    t = ( e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2] ) * inv;
    return t;
}

/* vertices with the same position and normal (shared by neighbouring triangles) are baked once */
typedef struct AOKey { int q[6]; unsigned int v; } AOKey;

static int s3d_ao_key_cmp( const void * a, const void * b )
{
    const AOKey * x = (const AOKey *)a, * y = (const AOKey *)b;
    int i;

    for (i = 0; i < 6; ++i) if (x->q[i] != y->q[i]) return x->q[i] < y->q[i] ? -1 : 1;
    return 0;
}

/* s3d._bakeAO( positions, normals, rays, distance [, groundY] ) -> buffer with one occlusion value per vertex (1 = open, 0 = closed)
   positions / normals: buffers from s3d.newBuffer, 3 floats per vertex, every 3 vertices one triangle (non-indexed).
   For each vertex, `rays` directions over the hemisphere around its normal (cosine weighted, the same pattern everywhere so
   the result is smooth) are tested against the model's own triangles within `distance`; a hit closer counts more.
   groundY (optional): a ground plane at that height (model space) also occludes, so the bottoms of walls get darker. */
static int s3d_l_bake_ao( lua_State * L )
{
    const float * P = (const float *)lua_touserdata( L, 1 );
    const float * N = (const float *)lua_touserdata( L, 2 );
    int rays = (int)s3d_optnum( L, 3, 16 );
    float maxDist = s3d_optnum( L, 4, 0.5f );
    int hasGround = lua_isnumber( L, 5 );
    float groundY = hasGround ? (float)lua_tonumber( L, 5 ) : 0.0f;
    unsigned int nv, nt, v, t, k, ncand;
    float * out, * dirs, * centers;
    unsigned int * cand, kv;
    AOKey * keys;
    const float golden = 2.39996323f; /* golden angle */

    if (!P || !N) return s3d_error( L, "s3d.bakeAO: expected two buffers" );
    nv = (unsigned int)( lua_objlen( L, 1 ) / ( 3 * sizeof( float ) ) );
    if (lua_objlen( L, 2 ) / ( 3 * sizeof( float ) ) < nv) return s3d_error( L, "s3d.bakeAO: fewer normals than positions" );
    if (rays < 1) rays = 1;
    if (rays > 64) rays = 64;
    if (maxDist <= 0.0f) maxDist = 0.5f;
    nt = nv / 3;

    out = (float *)lua_newuserdata( L, ( nv ? nv : 1 ) * sizeof( float ) );
    dirs = (float *)malloc( sizeof( float ) * 3 * (size_t)rays );
    centers = (float *)malloc( sizeof( float ) * 4 * (size_t)( nt ? nt : 1 ) );
    cand = (unsigned int *)malloc( sizeof( unsigned int ) * (size_t)( nt ? nt : 1 ) );
    keys = (AOKey *)malloc( sizeof( AOKey ) * (size_t)( nv ? nv : 1 ) );
    if (!dirs || !centers || !cand || !keys)
    {
        free( dirs ); free( centers ); free( cand ); free( keys );
        return s3d_error( L, "s3d.bakeAO: out of memory" );
    }
    for (v = 0; v < nv; ++v)
    {
        int i;

        for (i = 0; i < 3; ++i)
        {
            keys[v].q[i] = (int)floorf( P[3 * v + i] * 1000.0f + 0.5f );
            keys[v].q[3 + i] = (int)floorf( N[3 * v + i] * 100.0f + 0.5f );
        }
        keys[v].v = v;
    }
    qsort( keys, nv, sizeof( AOKey ), s3d_ao_key_cmp );

    /* hemisphere around +Z, cosine weighted (Fibonacci spiral) */
    for (k = 0; k < (unsigned int)rays; ++k)
    {
        float r = sqrtf( ( (float)k + 0.5f ) / (float)rays ), a = golden * (float)k;

        dirs[3 * k] = r * cosf( a ); dirs[3 * k + 1] = r * sinf( a ); dirs[3 * k + 2] = sqrtf( 1.0f - r * r );
    }
    /* bounding sphere of each triangle */
    for (t = 0; t < nt; ++t)
    {
        const float * a = P + 9 * t, * b = a + 3, * c = a + 6;
        float * s = centers + 4 * t, r2 = 0.0f, dx, dy, dz;
        int i;

        s[0] = ( a[0] + b[0] + c[0] ) / 3.0f; s[1] = ( a[1] + b[1] + c[1] ) / 3.0f; s[2] = ( a[2] + b[2] + c[2] ) / 3.0f;
        for (i = 0; i < 3; ++i)
        {
            const float * q = a + 3 * i;
            float d2;

            dx = q[0] - s[0]; dy = q[1] - s[1]; dz = q[2] - s[2];
            d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) r2 = d2;
        }
        s[3] = sqrtf( r2 );
    }

    for (kv = 0; kv < nv; ++kv)
    {
        const float * p;
        float n[3], tx[3], ty[3], o[3], len, occl = 0.0f;

        v = keys[kv].v;
        if (kv > 0 && 0 == s3d_ao_key_cmp( &keys[kv], &keys[kv - 1] )) { out[v] = out[keys[kv - 1].v]; continue; }
        p = P + 3 * v;
        n[0] = N[3 * v]; n[1] = N[3 * v + 1]; n[2] = N[3 * v + 2];
        len = sqrtf( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
        if (len < 1e-6f) { out[v] = 1.0f; continue; }
        n[0] /= len; n[1] /= len; n[2] /= len;
        /* tangent frame */
        if (fabsf( n[1] ) < 0.9f) { tx[0] = n[2]; tx[1] = 0; tx[2] = -n[0]; } else { tx[0] = 0; tx[1] = -n[2]; tx[2] = n[1]; }
        len = sqrtf( tx[0] * tx[0] + tx[1] * tx[1] + tx[2] * tx[2] );
        tx[0] /= len; tx[1] /= len; tx[2] /= len;
        ty[0] = n[1] * tx[2] - n[2] * tx[1]; ty[1] = n[2] * tx[0] - n[0] * tx[2]; ty[2] = n[0] * tx[1] - n[1] * tx[0];
        /* start slightly off the surface */
        o[0] = p[0] + n[0] * maxDist * 0.02f; o[1] = p[1] + n[1] * maxDist * 0.02f; o[2] = p[2] + n[2] * maxDist * 0.02f;

        /* triangles that can be hit within maxDist (not the vertex's own triangle) */
        ncand = 0;
        for (t = 0; t < nt; ++t)
        {
            const float * s = centers + 4 * t;
            float dx = s[0] - o[0], dy = s[1] - o[1], dz = s[2] - o[2], reach = maxDist + s[3];

            if (t == v / 3) continue;
            if (dx * dx + dy * dy + dz * dz <= reach * reach) cand[ncand++] = t;
        }

        for (k = 0; k < (unsigned int)rays; ++k)
        {
            const float * l = dirs + 3 * k;
            float d[3], best = maxDist;
            unsigned int c;

            d[0] = tx[0] * l[0] + ty[0] * l[1] + n[0] * l[2];
            d[1] = tx[1] * l[0] + ty[1] * l[1] + n[1] * l[2];
            d[2] = tx[2] * l[0] + ty[2] * l[1] + n[2] * l[2];
            for (c = 0; c < ncand; ++c)
            {
                const float * a = P + 9 * cand[c];
                float h = s3d_ray_tri( o, d, a, a + 3, a + 6 );

                if (h > 0.0f && h < best) best = h;
            }
            if (hasGround && d[1] < -1e-4f)
            {
                float h = ( groundY - o[1] ) / d[1];

                if (h > 0.0f && h < best) best = h;
            }
            if (best < maxDist) occl += 1.0f - best / maxDist;
        }
        out[v] = 1.0f - occl / (float)rays;
    }

    free( dirs ); free( centers ); free( cand ); free( keys );
    return 1;
}

/* s3d._setCamera( ex, ey, ez, tx, ty, tz, ux, uy, uz, fovDeg, aspect, near, far ) */
static int s3d_l_set_camera( lua_State * L )
{
    vec3 eye, target, up;
    mat4 view, proj, vp;

    eye[0] = s3d_optnum( L, 1, 0 ); eye[1] = s3d_optnum( L, 2, 0 ); eye[2] = s3d_optnum( L, 3, 5 );
    target[0] = s3d_optnum( L, 4, 0 ); target[1] = s3d_optnum( L, 5, 0 ); target[2] = s3d_optnum( L, 6, 0 );
    up[0] = s3d_optnum( L, 7, 0 ); up[1] = s3d_optnum( L, 8, 1 ); up[2] = s3d_optnum( L, 9, 0 );

    glm_lookat( eye, target, up, view );
    glm_perspective( glm_rad( s3d_optnum( L, 10, 60 ) ), s3d_optnum( L, 11, 1 ), s3d_optnum( L, 12, 0.1f ), s3d_optnum( L, 13, 100 ), proj );
    glm_mat4_mul( proj, view, vp );
    memcpy( gFrame.viewProj, vp, sizeof( gFrame.viewProj ) );
    gFrame.camPos[0] = eye[0]; gFrame.camPos[1] = eye[1]; gFrame.camPos[2] = eye[2]; gFrame.camPos[3] = 1.0f;

    /* the rows of the view matrix are the camera's right, up and back axes (cglm matrices are column-major) */
    {
        int i;
        float h;

        for (i = 0; i < 3; ++i) { gBillRight[i] = view[i][0]; gBillUp[i] = view[i][1]; gBillBack[i] = view[i][2]; }
        h = sqrtf( gBillRight[0] * gBillRight[0] + gBillRight[2] * gBillRight[2] );
        if (h > 1e-5f) { gBillRightY[0] = gBillRight[0] / h; gBillRightY[1] = 0.0f; gBillRightY[2] = gBillRight[2] / h; }
        /* back, horizontal = right x up(0, 1, 0) rotated: ( -right.z, 0, right.x ) */
        gBillBackY[0] = -gBillRightY[2]; gBillBackY[1] = 0.0f; gBillBackY[2] = gBillRightY[0];
    }

    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* s3d._setLight( dx, dy, dz, ambient, r, g, b ): (dx, dy, dz) is the direction the light travels */
static int s3d_l_set_light( lua_State * L )
{
    vec3 d;

    d[0] = -s3d_optnum( L, 1, -0.4f ); d[1] = -s3d_optnum( L, 2, -1.0f ); d[2] = -s3d_optnum( L, 3, -0.6f );
    glm_vec3_normalize( d );
    gFrame.lightDir[0] = d[0]; gFrame.lightDir[1] = d[1]; gFrame.lightDir[2] = d[2];
    gFrame.lightDir[3] = s3d_optnum( L, 4, 0.3f );
    gFrame.lightColor[0] = s3d_optnum( L, 5, 1 );
    gFrame.lightColor[1] = s3d_optnum( L, 6, 1 );
    gFrame.lightColor[2] = s3d_optnum( L, 7, 1 );
    gFrame.lightColor[3] = 1;
    s3d_update_shadow_matrix();
    s3d_update_light_vp();

    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* s3d._setFog( r, g, b, start, end, enabled ) */
static int s3d_l_set_fog( lua_State * L )
{
    gFrame.fog[0] = s3d_optnum( L, 1, 0.7f );
    gFrame.fog[1] = s3d_optnum( L, 2, 0.8f );
    gFrame.fog[2] = s3d_optnum( L, 3, 0.9f );
    gFrame.fogRange[0] = s3d_optnum( L, 4, 10 );
    gFrame.fogRange[1] = s3d_optnum( L, 5, 40 );
    gFrame.fog[3] = lua_toboolean( L, 6 ) ? 1.0f : 0.0f;
    gFrame.fogRange[2] = s3d_optnum( L, 7, 0.15f ); /* sky domes fade into the fog color below this height (sin of the angle) */
    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* s3d._setShadowPlane( y ) */
static int s3d_l_set_shadow_plane( lua_State * L )
{
    gShadowPlaneY = s3d_optnum( L, 1, 0 );
    s3d_update_shadow_matrix();
    CORONA( CoronaRendererInvalidate )( L );
    return 0;
}

/* s3d._beginFrame(): called every enterFrame, so we know when an FBO's depth must be cleared */
static int s3d_l_begin_frame( lua_State * L )
{
    (void)L;
    ++gFrameId;
    return 0;
}

/* ========================================================================== */
/* Dynamic meshes                                                               */
/* ========================================================================== */
/* A dynamic mesh keeps its own vertex list (indexed, nv vertices) and the triangle list Solar2D draws (non-indexed, ne
   vertices, three per triangle). Lua edits the vertex list, then s3d._dynUpdate expands it into the two output buffers
   (positions, normals) that the object uses as its morph source, so the geometry writers pick the new shape up. */
#define S3D_DYN_MAGIC 0x53334459u /* "S3DY" */

typedef struct DynMesh {
    unsigned int magic;
    unsigned int nv;    /* source vertices */
    unsigned int ne;    /* expanded vertices (3 per triangle) */
    float * outP;       /* ne * 3, a userdata owned by Lua (kept alive next to this one) */
    float * outN;       /* ne * 3, idem */
    float * src;        /* nv * 3 */
    float * vn;         /* nv * 3, smooth normals */
    unsigned int * idx; /* ne */
} DynMesh;

static DynMesh * s3d_check_dyn( lua_State * L, int i )
{
    DynMesh * d = (DynMesh *)lua_touserdata( L, i );

    if (!d || lua_objlen( L, i ) < sizeof( DynMesh ) || S3D_DYN_MAGIC != d->magic)
    {
        s3d_error( L, "s3d: expected a dynamic mesh" );
        return NULL;
    }
    return d;
}

/* s3d._newDynamic( positions, indices ) -> dyn, outPositions, outNormals
   positions: { x, y, z, ... }; indices: { i1, i2, i3, ... } (1-based, three per triangle, front face counter-clockwise),
   or nil for an unindexed list (every three vertices one triangle). */
static int s3d_l_new_dynamic( lua_State * L )
{
    unsigned int nv, ne, i;
    int hasIdx = LUA_TTABLE == lua_type( L, 2 );
    size_t bytes;
    unsigned char * mem;
    DynMesh * d;

    if (LUA_TTABLE != lua_type( L, 1 )) return s3d_error( L, "s3d.newMesh: vertices must be a table of numbers" );
    nv = (unsigned int)( lua_objlen( L, 1 ) / 3 );
    ne = hasIdx ? (unsigned int)lua_objlen( L, 2 ) : nv;
    ne -= ne % 3;
    if (nv < 3 || ne < 3) return s3d_error( L, "s3d.newMesh: needs at least one triangle" );

    bytes = sizeof( DynMesh ) + sizeof( float ) * 6 * nv + sizeof( unsigned int ) * ne;
    mem = (unsigned char *)lua_newuserdata( L, bytes );
    memset( mem, 0, bytes );
    d = (DynMesh *)mem;
    d->magic = S3D_DYN_MAGIC;
    d->nv = nv;
    d->ne = ne;
    d->src = (float *)( mem + sizeof( DynMesh ) );
    d->vn = d->src + 3 * nv;
    d->idx = (unsigned int *)( d->vn + 3 * nv );
    for (i = 0; i < 3 * nv; ++i) d->src[i] = s3d_tablenum( L, 1, (int)i + 1, 0 );
    for (i = 0; i < ne; ++i)
    {
        unsigned int k = hasIdx ? (unsigned int)s3d_tablenum( L, 2, (int)i + 1, 1 ) - 1u : i;
        if (k >= nv) return s3d_error( L, "s3d.newMesh: index %u at position %u is out of range (1..%u)", k + 1u, i + 1u, nv );
        d->idx[i] = k;
    }
    d->outP = (float *)lua_newuserdata( L, sizeof( float ) * 3 * ne );
    d->outN = (float *)lua_newuserdata( L, sizeof( float ) * 3 * ne );
    return 3;
}

/* s3d._dynSetPositions( dyn, { x, y, z, ... } [, first] ): copies a run of vertices, starting at vertex `first` (1-based) */
static int s3d_l_dyn_set_positions( lua_State * L )
{
    DynMesh * d = s3d_check_dyn( L, 1 );
    unsigned int first = (unsigned int)s3d_optnum( L, 3, 1 ), n, i;

    if (LUA_TTABLE != lua_type( L, 2 )) return s3d_error( L, "s3d: setVertices needs a table of numbers" );
    if (first < 1) first = 1;
    n = (unsigned int)lua_objlen( L, 2 );
    if (3 * ( first - 1 ) + n > 3 * d->nv) n = 3 * d->nv - 3 * ( first - 1 );
    for (i = 0; i < n; ++i) d->src[3 * ( first - 1 ) + i] = s3d_tablenum( L, 2, (int)i + 1, 0 );
    return 0;
}

/* s3d._dynSetHeights( dyn, { y, y, ... } [, scale, offset] ): sets only the y of every vertex (height fields) */
static int s3d_l_dyn_set_heights( lua_State * L )
{
    DynMesh * d = s3d_check_dyn( L, 1 );
    float scale = s3d_optnum( L, 3, 1 ), offset = s3d_optnum( L, 4, 0 );
    unsigned int n, i;

    if (LUA_TTABLE != lua_type( L, 2 )) return s3d_error( L, "s3d: setHeights needs a table of numbers" );
    n = (unsigned int)lua_objlen( L, 2 );
    if (n > d->nv) n = d->nv;
    for (i = 0; i < n; ++i) d->src[3 * i + 1] = offset + scale * s3d_tablenum( L, 2, (int)i + 1, 0 );
    return 0;
}

/* s3d._dynSetVertex( dyn, i, x, y, z ) (1-based) */
static int s3d_l_dyn_set_vertex( lua_State * L )
{
    DynMesh * d = s3d_check_dyn( L, 1 );
    unsigned int i = (unsigned int)s3d_optnum( L, 2, 0 );

    if (i < 1 || i > d->nv) return s3d_error( L, "s3d: vertex %u is out of range (1..%u)", i, d->nv );
    --i;
    if (lua_isnumber( L, 3 )) d->src[3 * i] = (float)lua_tonumber( L, 3 );
    if (lua_isnumber( L, 4 )) d->src[3 * i + 1] = (float)lua_tonumber( L, 4 );
    if (lua_isnumber( L, 5 )) d->src[3 * i + 2] = (float)lua_tonumber( L, 5 );
    return 0;
}

/* s3d._dynGetVertex( dyn, i ) -> x, y, z (1-based) */
static int s3d_l_dyn_get_vertex( lua_State * L )
{
    DynMesh * d = s3d_check_dyn( L, 1 );
    unsigned int i = (unsigned int)s3d_optnum( L, 2, 0 );

    if (i < 1 || i > d->nv) return 0;
    --i;
    lua_pushnumber( L, d->src[3 * i] );
    lua_pushnumber( L, d->src[3 * i + 1] );
    lua_pushnumber( L, d->src[3 * i + 2] );
    return 3;
}

/* s3d._dynUpdate( dyn [, flat] ): expands the vertices into the output buffers and recomputes the normals:
   smooth (default) = area-weighted average over the triangles sharing a vertex index; flat = one normal per triangle */
static int s3d_l_dyn_update( lua_State * L )
{
    DynMesh * d = s3d_check_dyn( L, 1 );
    int flat = lua_toboolean( L, 2 );
    unsigned int t, i;

    if (!flat) memset( d->vn, 0, sizeof( float ) * 3 * d->nv );
    for (t = 0; t < d->ne; t += 3)
    {
        unsigned int ia = d->idx[t], ib = d->idx[t + 1], ic = d->idx[t + 2];
        const float * a = d->src + 3 * ia;
        const float * b = d->src + 3 * ib;
        const float * c = d->src + 3 * ic;
        float e1[3], e2[3], fn[3];

        e1[0] = b[0] - a[0]; e1[1] = b[1] - a[1]; e1[2] = b[2] - a[2];
        e2[0] = c[0] - a[0]; e2[1] = c[1] - a[1]; e2[2] = c[2] - a[2];
        fn[0] = e1[1] * e2[2] - e1[2] * e2[1];
        fn[1] = e1[2] * e2[0] - e1[0] * e2[2];
        fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        memcpy( d->outP + 3 * t, a, sizeof( float ) * 3 );
        memcpy( d->outP + 3 * t + 3, b, sizeof( float ) * 3 );
        memcpy( d->outP + 3 * t + 6, c, sizeof( float ) * 3 );
        if (flat)
        {
            float len = sqrtf( fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2] );
            unsigned int k;

            if (len > 0.0f) { fn[0] /= len; fn[1] /= len; fn[2] /= len; } else { fn[0] = 0; fn[1] = 1; fn[2] = 0; }
            for (k = 0; k < 3; ++k) memcpy( d->outN + 3 * ( t + k ), fn, sizeof( float ) * 3 );
        }
        else
        {
            /* the cross product's length is twice the area, so big triangles weigh more */
            for (i = 0; i < 3; ++i) { d->vn[3 * ia + i] += fn[i]; d->vn[3 * ib + i] += fn[i]; d->vn[3 * ic + i] += fn[i]; }
        }
    }
    if (!flat)
    {
        for (i = 0; i < d->nv; ++i)
        {
            float * n = d->vn + 3 * i;
            float len = sqrtf( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );

            if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; } else { n[0] = 0; n[1] = 1; n[2] = 0; }
        }
        for (t = 0; t < d->ne; ++t) memcpy( d->outN + 3 * t, d->vn + 3 * d->idx[t], sizeof( float ) * 3 );
    }
    return 0;
}


/* ========================================================================== */
/* glTF 2.0 (cgltf, MIT, https://github.com/jkuhlmann/cgltf)                    */
/* ========================================================================== */
/* s3d._gltfLoad( bytes ) -> model table. Self-contained files: .glb, or .gltf with base64 data: URIs (external .bin
   files cannot be opened from an Android APK). The rest pose is baked: every mesh node's primitives are transformed by
   the node's world matrix and gathered per material into plain triangle lists (P, U, N), one Solar2D mesh each. */
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

typedef struct GltfGroup {
    const cgltf_material * material;
    float * P, * U, * N;
    unsigned int * node; /* animated models: per vertex, the node it belongs to */
    float * LP, * LN;    /* idem: position and normal in that node's space */
    size_t count, cap;   /* vertices */
} GltfGroup;

/* ---- node animation (rigid parts, no skins) ---- */
#define S3D_RIG_MAGIC 0x53335247u /* "S3RG" */
enum { kPath_T = 0, kPath_R, kPath_S };
enum { kInterp_Linear = 0, kInterp_Step, kInterp_Cubic };

typedef struct RigNode {
    int parent;
    int hasMatrix;
    float t[3], r[4], s[3];
    float m[16];
} RigNode;

typedef struct RigChannel {
    int node, path, interp;
    unsigned int keys, comps;
    float * times;
    float * values; /* keys * comps, or keys * 3 * comps for cubic splines (in-tangent, value, out-tangent) */
} RigChannel;

typedef struct RigClip {
    float duration;
    unsigned int first, count; /* channels */
} RigClip;

typedef struct RigPart {
    unsigned int count;
    unsigned int * node;
    float * lp, * ln;
    float * outP, * outN; /* Lua-owned buffers (the mesh's morph source) */
} RigPart;

typedef struct Rig {
    unsigned int magic;
    unsigned int nodeCount, clipCount, partCount;
    float origin[3], scale;
    RigNode * nodes;
    RigClip * clips;
    RigChannel * channels;
    RigPart * parts;
    float * trsA, * trsB; /* nodeCount * 10: t, r, s */
    float * world;        /* nodeCount * 16 */
    float * normal;       /* nodeCount * 9 */
    unsigned char * state;
} Rig;

static int s3d_gltf_group_reserve( GltfGroup * g, size_t more, int rigged )
{
    if (g->count + more <= g->cap) return 1;
    {
        size_t cap = g->cap ? g->cap * 2 : 1024;
        float * P, * U, * N;

        while (cap < g->count + more) cap *= 2;
        P = (float *)realloc( g->P, cap * 3 * sizeof( float ) ); if (P) g->P = P;
        U = (float *)realloc( g->U, cap * 2 * sizeof( float ) ); if (U) g->U = U;
        N = (float *)realloc( g->N, cap * 3 * sizeof( float ) ); if (N) g->N = N;
        if (!P || !U || !N) return 0;
        if (rigged)
        {
            unsigned int * nd = (unsigned int *)realloc( g->node, cap * sizeof( unsigned int ) );
            float * lp, * ln;
            if (nd) g->node = nd;
            lp = (float *)realloc( g->LP, cap * 3 * sizeof( float ) ); if (lp) g->LP = lp;
            ln = (float *)realloc( g->LN, cap * 3 * sizeof( float ) ); if (ln) g->LN = ln;
            if (!nd || !lp || !ln) return 0;
        }
        g->cap = cap;
    }
    return 1;
}

static void s3d_push_float_array( lua_State * L, const float * a, size_t n )
{
    size_t i;

    lua_createtable( L, (int)n, 0 );
    for (i = 0; i < n; ++i)
    {
        lua_pushnumber( L, a[i] );
        lua_rawseti( L, -2, (int)i + 1 );
    }
}

static int s3d_gltf_index_of( const void * base, const void * item, size_t size ) { return (int)( ( (const char *)item - (const char *)base ) / size ); }


static void s3d_gltf_free_groups( GltfGroup * groups, size_t ng )
{
    size_t gi;
    for (gi = 0; gi < ng; ++gi)
    {
        free( groups[gi].P ); free( groups[gi].U ); free( groups[gi].N );
        free( groups[gi].node ); free( groups[gi].LP ); free( groups[gi].LN );
    }
    free( groups );
}

static Rig * s3d_check_rig( lua_State * L, int i )
{
    Rig * r = (Rig *)lua_touserdata( L, i );
    if (!r || lua_objlen( L, i ) < sizeof( Rig ) || S3D_RIG_MAGIC != r->magic)
    {
        s3d_error( L, "s3d: expected an animated glTF model" );
        return NULL;
    }
    return r;
}

static size_t s3d_align( size_t n ) { return ( n + 15u ) & ~(size_t)15u; }
static void * s3d_take( unsigned char ** cur, size_t bytes ) { void * r = *cur; *cur += s3d_align( bytes ); return r; }

/* Builds the rig userdata (one block, owned by Lua) and pushes it; the parts' output buffers are attached later. */
static void s3d_gltf_push_rig( lua_State * L, const cgltf_data * data, const GltfGroup * groups, size_t ng )
{
    size_t bytes, i, c, np = 0, nch = 0, nf = 0, nv = 0, gi;
    unsigned char * mem, * cur;
    Rig * rig;

    for (gi = 0; gi < ng; ++gi) if (groups[gi].count) { ++np; nv += groups[gi].count; }
    for (i = 0; i < data->animations_count; ++i)
    {
        const cgltf_animation * an = &data->animations[i];
        for (c = 0; c < an->channels_count; ++c)
        {
            const cgltf_animation_channel * ch = &an->channels[c];
            if (!ch->target_node || !ch->sampler || ch->target_path == cgltf_animation_path_type_weights) continue;
            if (ch->target_path == cgltf_animation_path_type_invalid) continue;
            ++nch;
            nf += ch->sampler->input->count + cgltf_num_components( ch->sampler->output->type ) * ch->sampler->output->count;
        }
    }
    bytes = s3d_align( sizeof( Rig ) )
        + s3d_align( sizeof( RigNode ) * data->nodes_count )
        + s3d_align( sizeof( RigClip ) * data->animations_count )
        + s3d_align( sizeof( RigChannel ) * nch )
        + s3d_align( sizeof( RigPart ) * np )
        + s3d_align( sizeof( float ) * nf )
        + s3d_align( nv * ( sizeof( unsigned int ) + 6 * sizeof( float ) ) )
        + s3d_align( sizeof( float ) * data->nodes_count * ( 10 + 10 + 16 + 9 ) )
        + s3d_align( data->nodes_count + 1 );
    mem = (unsigned char *)lua_newuserdata( L, bytes );
    memset( mem, 0, bytes );
    cur = mem;
#define S3D_TAKE( type, n ) ( (type *)s3d_take( &cur, sizeof( type ) * (n) ) )
    rig = S3D_TAKE( Rig, 1 );
    rig->magic = S3D_RIG_MAGIC;
    rig->nodeCount = (unsigned int)data->nodes_count;
    rig->clipCount = (unsigned int)data->animations_count;
    rig->partCount = (unsigned int)np;
    rig->scale = 1.0f;
    rig->nodes = S3D_TAKE( RigNode, data->nodes_count );
    rig->clips = S3D_TAKE( RigClip, data->animations_count );
    rig->channels = S3D_TAKE( RigChannel, nch );
    rig->parts = S3D_TAKE( RigPart, np );
    {
        float * pool = S3D_TAKE( float, nf );
        unsigned char * vmem = S3D_TAKE( unsigned char, nv * ( sizeof( unsigned int ) + 6 * sizeof( float ) ) );
        float * work = S3D_TAKE( float, data->nodes_count * ( 10 + 10 + 16 + 9 ) );
        unsigned int k = 0, p = 0;

        rig->state = S3D_TAKE( unsigned char, data->nodes_count + 1 );
        rig->trsA = work;
        rig->trsB = work + 10 * data->nodes_count;
        rig->world = work + 20 * data->nodes_count;
        rig->normal = work + 36 * data->nodes_count;

        for (i = 0; i < data->nodes_count; ++i)
        {
            const cgltf_node * n = &data->nodes[i];
            RigNode * rn = &rig->nodes[i];
            rn->parent = n->parent ? s3d_gltf_index_of( data->nodes, n->parent, sizeof( cgltf_node ) ) : -1;
            rn->hasMatrix = n->has_matrix ? 1 : 0;
            if (n->has_matrix) memcpy( rn->m, n->matrix, sizeof( rn->m ) );
            rn->t[0] = rn->t[1] = rn->t[2] = 0.0f;
            rn->r[0] = rn->r[1] = rn->r[2] = 0.0f; rn->r[3] = 1.0f;
            rn->s[0] = rn->s[1] = rn->s[2] = 1.0f;
            if (n->has_translation) memcpy( rn->t, n->translation, sizeof( rn->t ) );
            if (n->has_rotation) memcpy( rn->r, n->rotation, sizeof( rn->r ) );
            if (n->has_scale) memcpy( rn->s, n->scale, sizeof( rn->s ) );
        }
        for (i = 0; i < data->animations_count; ++i)
        {
            const cgltf_animation * an = &data->animations[i];
            RigClip * clip = &rig->clips[i];
            clip->first = k;
            for (c = 0; c < an->channels_count; ++c)
            {
                const cgltf_animation_channel * ch = &an->channels[c];
                RigChannel * rc;
                size_t keys, comps;
                if (!ch->target_node || !ch->sampler || ch->target_path == cgltf_animation_path_type_weights) continue;
                if (ch->target_path == cgltf_animation_path_type_invalid) continue;
                rc = &rig->channels[k++];
                keys = ch->sampler->input->count;
                comps = cgltf_num_components( ch->sampler->output->type );
                rc->node = s3d_gltf_index_of( data->nodes, ch->target_node, sizeof( cgltf_node ) );
                rc->path = ch->target_path == cgltf_animation_path_type_translation ? kPath_T
                    : ch->target_path == cgltf_animation_path_type_rotation ? kPath_R : kPath_S;
                rc->interp = ch->sampler->interpolation == cgltf_interpolation_type_step ? kInterp_Step
                    : ch->sampler->interpolation == cgltf_interpolation_type_cubic_spline ? kInterp_Cubic : kInterp_Linear;
                rc->keys = (unsigned int)keys;
                rc->comps = (unsigned int)comps;
                rc->times = pool; pool += keys;
                rc->values = pool; pool += comps * ch->sampler->output->count;
                cgltf_accessor_unpack_floats( ch->sampler->input, rc->times, keys );
                cgltf_accessor_unpack_floats( ch->sampler->output, rc->values, comps * ch->sampler->output->count );
                if (rc->interp == kInterp_Cubic && ch->sampler->output->count < 3 * keys) rc->interp = kInterp_Linear;
                if (keys && rc->times[keys - 1] > clip->duration) clip->duration = rc->times[keys - 1];
            }
            clip->count = k - clip->first;
        }
        for (gi = 0; gi < ng; ++gi)
        {
            const GltfGroup * g = &groups[gi];
            RigPart * part;
            if (!g->count) continue;
            part = &rig->parts[p++];
            part->count = (unsigned int)g->count;
            part->node = (unsigned int *)vmem; vmem += g->count * sizeof( unsigned int );
            part->lp = (float *)vmem; vmem += g->count * 3 * sizeof( float );
            part->ln = (float *)vmem; vmem += g->count * 3 * sizeof( float );
            memcpy( part->node, g->node, g->count * sizeof( unsigned int ) );
            memcpy( part->lp, g->LP, g->count * 3 * sizeof( float ) );
            memcpy( part->ln, g->LN, g->count * 3 * sizeof( float ) );
        }
    }
#undef S3D_TAKE
}

/* value of channel ch at time t into out (comps floats) */
static void s3d_rig_sample( const RigChannel * ch, float t, float * out )
{
    unsigned int n = ch->keys, c = ch->comps, lo = 0, hi, k;
    float u, dt;
    const float * a, * b;

    if (!n) return;
    if (t <= ch->times[0] || n == 1)
    {
        a = ch->interp == kInterp_Cubic ? ch->values + c : ch->values;
        memcpy( out, a, c * sizeof( float ) );
        return;
    }
    if (t >= ch->times[n - 1])
    {
        a = ch->interp == kInterp_Cubic ? ch->values + ( 3 * ( n - 1 ) + 1 ) * c : ch->values + ( n - 1 ) * c;
        memcpy( out, a, c * sizeof( float ) );
        return;
    }
    hi = n - 1;
    while (hi - lo > 1) { unsigned int mid = ( lo + hi ) / 2; if (ch->times[mid] <= t) lo = mid; else hi = mid; }
    dt = ch->times[hi] - ch->times[lo];
    u = dt > 0.0f ? ( t - ch->times[lo] ) / dt : 0.0f;
    if (ch->interp == kInterp_Step)
    {
        memcpy( out, ch->values + lo * c, c * sizeof( float ) );
        return;
    }
    if (ch->interp == kInterp_Cubic)
    {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        const float * v0 = ch->values + ( 3 * lo + 1 ) * c, * b0 = ch->values + ( 3 * lo + 2 ) * c;
        const float * v1 = ch->values + ( 3 * hi + 1 ) * c, * a1 = ch->values + ( 3 * hi ) * c;
        for (k = 0; k < c; ++k) out[k] = h00 * v0[k] + h10 * dt * b0[k] + h01 * v1[k] + h11 * dt * a1[k];
    }
    else
    {
        a = ch->values + lo * c;
        b = ch->values + hi * c;
        if (ch->path == kPath_R && c == 4)
        {
            versor qa, qb, q;
            memcpy( qa, a, sizeof( qa ) ); memcpy( qb, b, sizeof( qb ) );
            glm_quat_slerp( qa, qb, u, q );
            memcpy( out, q, sizeof( q ) );
            return;
        }
        for (k = 0; k < c; ++k) out[k] = a[k] + ( b[k] - a[k] ) * u;
    }
    if (ch->path == kPath_R && c == 4)
    {
        float l = sqrtf( out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3] );
        if (l > 0.0f) for (k = 0; k < 4; ++k) out[k] /= l;
    }
}

/* trs[node * 10] = t(3), r(4), s(3): rest pose, then the clip's channels at time t */
static void s3d_rig_local( const Rig * rig, int clip, float t, float * trs )
{
    unsigned int i;

    for (i = 0; i < rig->nodeCount; ++i)
    {
        const RigNode * n = &rig->nodes[i];
        memcpy( trs + 10 * i, n->t, 3 * sizeof( float ) );
        memcpy( trs + 10 * i + 3, n->r, 4 * sizeof( float ) );
        memcpy( trs + 10 * i + 7, n->s, 3 * sizeof( float ) );
    }
    if (clip < 0 || (unsigned int)clip >= rig->clipCount) return;
    {
        const RigClip * c = &rig->clips[clip];
        for (i = 0; i < c->count; ++i)
        {
            const RigChannel * ch = &rig->channels[c->first + i];
            float * dst = trs + 10 * ch->node + ( ch->path == kPath_T ? 0 : ch->path == kPath_R ? 3 : 7 );
            float v[4];
            if ((unsigned int)ch->node >= rig->nodeCount) continue;
            if (ch->comps != ( ch->path == kPath_R ? 4u : 3u )) continue;
            s3d_rig_sample( ch, t, v );
            memcpy( dst, v, ch->comps * sizeof( float ) );
        }
    }
}

static void s3d_rig_world( Rig * rig, unsigned int i )
{
    const RigNode * n = &rig->nodes[i];
    float * w = rig->world + 16 * i;
    mat4 local;

    if (rig->state[i]) return;
    if (n->hasMatrix) memcpy( local, n->m, sizeof( local ) );
    else
    {
        const float * trs = rig->trsA + 10 * i;
        versor q;
        vec3 s;
        mat4 r;
        memcpy( q, trs + 3, sizeof( q ) );
        memcpy( s, trs + 7, sizeof( s ) );
        glm_translate_make( local, (float *)trs );
        glm_quat_mat4( q, r );
        glm_mat4_mul( local, r, local );
        glm_scale( local, s );
    }
    if (n->parent >= 0 && (unsigned int)n->parent < rig->nodeCount && n->parent != (int)i)
    {
        rig->state[i] = 2; /* in progress: guards against cycles in broken files */
        if (rig->state[n->parent] != 2) s3d_rig_world( rig, (unsigned int)n->parent );
        {
            mat4 pw; /* userdata is only 8-byte aligned: copy before the (SIMD) multiplication */
            memcpy( pw, rig->world + 16 * n->parent, sizeof( pw ) );
            glm_mat4_mul( pw, local, local );
        }
    }
    memcpy( w, local, sizeof( local ) );
    {
        mat3 nm;
        glm_mat4_pick3( local, nm );
        glm_mat3_inv( nm, nm );
        glm_mat3_transpose( nm );
        memcpy( rig->normal + 9 * i, nm, sizeof( nm ) );
    }
    rig->state[i] = 1;
}

/* s3d._gltfSetOrigin( rig, ox, oy, oz, scale ): output = ( world - origin ) * scale, as the static parts */
static int s3d_l_gltf_set_origin( lua_State * L )
{
    Rig * rig = s3d_check_rig( L, 1 );
    rig->origin[0] = s3d_optnum( L, 2, 0 );
    rig->origin[1] = s3d_optnum( L, 3, 0 );
    rig->origin[2] = s3d_optnum( L, 4, 0 );
    rig->scale = s3d_optnum( L, 5, 1 );
    return 0;
}

/* s3d._gltfAttach( rig, part (1-based) ) -> outP, outN: the buffers the part's mesh uses as its morph source */
static int s3d_l_gltf_attach( lua_State * L )
{
    Rig * rig = s3d_check_rig( L, 1 );
    unsigned int p = (unsigned int)s3d_optnum( L, 2, 0 );
    RigPart * part;

    if (p < 1 || p > rig->partCount) return s3d_error( L, "s3d: part %u is out of range", p );
    part = &rig->parts[p - 1];
    part->outP = (float *)lua_newuserdata( L, sizeof( float ) * 3 * part->count );
    part->outN = (float *)lua_newuserdata( L, sizeof( float ) * 3 * part->count );
    return 2;
}

/* s3d._gltfDuration( rig ) -> { seconds per clip } */
static int s3d_l_gltf_durations( lua_State * L )
{
    Rig * rig = s3d_check_rig( L, 1 );
    unsigned int i;
    lua_createtable( L, (int)rig->clipCount, 0 );
    for (i = 0; i < rig->clipCount; ++i) { lua_pushnumber( L, rig->clips[i].duration ); lua_rawseti( L, -2, (int)i + 1 ); }
    return 1;
}

/* s3d._gltfCloneRig( rig ) -> a copy for another model instance (own pose and output buffers, none attached yet) */
static int s3d_l_gltf_clone_rig( lua_State * L )
{
    Rig * src = s3d_check_rig( L, 1 );
    size_t bytes = lua_objlen( L, 1 ), i;
    unsigned char * mem = (unsigned char *)lua_newuserdata( L, bytes );
    const unsigned char * base = (const unsigned char *)src;
    Rig * r;

    memcpy( mem, src, bytes );
    r = (Rig *)mem;
#define S3D_RELOC( ptr ) do { if (ptr) ( ptr ) = (void *)( mem + ( (const unsigned char *)( ptr ) - base ) ); } while (0)
    S3D_RELOC( r->nodes ); S3D_RELOC( r->clips ); S3D_RELOC( r->channels ); S3D_RELOC( r->parts );
    S3D_RELOC( r->trsA ); S3D_RELOC( r->trsB ); S3D_RELOC( r->world ); S3D_RELOC( r->normal ); S3D_RELOC( r->state );
    {
        unsigned int c, n = 0;
        for (c = 0; c < r->clipCount; ++c) if (r->clips[c].first + r->clips[c].count > n) n = r->clips[c].first + r->clips[c].count;
        for (c = 0; c < n; ++c) { S3D_RELOC( r->channels[c].times ); S3D_RELOC( r->channels[c].values ); }
    }
    for (i = 0; i < r->partCount; ++i)
    {
        RigPart * part = &r->parts[i];
        S3D_RELOC( part->node ); S3D_RELOC( part->lp ); S3D_RELOC( part->ln );
        part->outP = part->outN = NULL;
    }
#undef S3D_RELOC
    return 1;
}

/* s3d._gltfPose( rig, clip, time [, clip2, time2, weight] ): clips are 1-based (0 = rest pose); the second clip is
   blended over the first with `weight` (cross-fades). Writes every attached part's positions and normals. */
static int s3d_l_gltf_pose( lua_State * L )
{
    Rig * rig = s3d_check_rig( L, 1 );
    int clipA = (int)s3d_optnum( L, 2, 0 ) - 1, clipB = (int)s3d_optnum( L, 4, 0 ) - 1;
    float tA = s3d_optnum( L, 3, 0 ), tB = s3d_optnum( L, 5, 0 ), w = s3d_optnum( L, 6, 0 );
    unsigned int i, p;

    s3d_rig_local( rig, clipA, tA, rig->trsA );
    if (clipB >= 0 && w > 0.0f)
    {
        s3d_rig_local( rig, clipB, tB, rig->trsB );
        if (w > 1.0f) w = 1.0f;
        for (i = 0; i < rig->nodeCount; ++i)
        {
            float * a = rig->trsA + 10 * i;
            const float * b = rig->trsB + 10 * i;
            versor qa, qb, q;
            int k;
            for (k = 0; k < 3; ++k) { a[k] += ( b[k] - a[k] ) * w; a[7 + k] += ( b[7 + k] - a[7 + k] ) * w; }
            memcpy( qa, a + 3, sizeof( qa ) ); memcpy( qb, b + 3, sizeof( qb ) );
            glm_quat_slerp( qa, qb, w, q );
            memcpy( a + 3, q, sizeof( q ) );
        }
    }
    memset( rig->state, 0, rig->nodeCount );
    for (i = 0; i < rig->nodeCount; ++i) s3d_rig_world( rig, i );

    for (p = 0; p < rig->partCount; ++p)
    {
        const RigPart * part = &rig->parts[p];
        float * op = part->outP, * on = part->outN;
        float sc = rig->scale;
        unsigned int v;

        if (!op || !on) continue;
        for (v = 0; v < part->count; ++v)
        {
            unsigned int ni = part->node[v];
            const float * m, * nm, * lp = part->lp + 3 * v, * ln = part->ln + 3 * v;
            float x, y, z, l;

            if (ni >= rig->nodeCount) ni = 0;
            m = rig->world + 16 * ni;
            nm = rig->normal + 9 * ni;
            x = m[0] * lp[0] + m[4] * lp[1] + m[8] * lp[2] + m[12];
            y = m[1] * lp[0] + m[5] * lp[1] + m[9] * lp[2] + m[13];
            z = m[2] * lp[0] + m[6] * lp[1] + m[10] * lp[2] + m[14];
            op[3 * v] = ( x - rig->origin[0] ) * sc;
            op[3 * v + 1] = ( y - rig->origin[1] ) * sc;
            op[3 * v + 2] = ( z - rig->origin[2] ) * sc;
            x = nm[0] * ln[0] + nm[3] * ln[1] + nm[6] * ln[2];
            y = nm[1] * ln[0] + nm[4] * ln[1] + nm[7] * ln[2];
            z = nm[2] * ln[0] + nm[5] * ln[1] + nm[8] * ln[2];
            l = sqrtf( x * x + y * y + z * z );
            if (l > 0.0f) { x /= l; y /= l; z /= l; }
            on[3 * v] = x; on[3 * v + 1] = y; on[3 * v + 2] = z;
        }
    }
    return 0;
}

static int s3d_l_gltf_load( lua_State * L )
{
    size_t len = 0, gi, ng = 0, i;
    const char * bytes = lua_tolstring( L, 1, &len );
    cgltf_options options;
    cgltf_data * data = NULL;
    cgltf_result r;
    GltfGroup * groups;
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
    char err[160];
    int rigged;

    if (!bytes || len < 12) return s3d_error( L, "s3d.loadGLTF: empty file" );
    memset( &options, 0, sizeof( options ) );
    r = cgltf_parse( &options, bytes, len, &data );
    if (r != cgltf_result_success) return s3d_error( L, "s3d.loadGLTF: not a valid glTF / GLB file (cgltf error %d)", (int)r );
    r = cgltf_load_buffers( &options, data, "" );
    if (r != cgltf_result_success)
    {
        cgltf_free( data );
        return s3d_error( L, "s3d.loadGLTF: could not load the buffers (cgltf error %d): use .glb, or .gltf with embedded data", (int)r );
    }

    /* one group per material (+1 for primitives without one) */
    groups = (GltfGroup *)calloc( data->materials_count + 1, sizeof( GltfGroup ) );
    if (!groups) { cgltf_free( data ); return s3d_error( L, "s3d.loadGLTF: out of memory" ); }
    for (i = 0; i < data->materials_count; ++i) groups[i].material = &data->materials[i];
    ng = data->materials_count + 1;
    err[0] = 0;
    rigged = data->animations_count > 0 && !lua_toboolean( L, 2 ); /* arg 2: static (skip the animation data) */

    for (i = 0; i < data->nodes_count && !err[0]; ++i)
    {
        const cgltf_node * node = &data->nodes[i];
        float world[16];
        mat4 m;
        mat3 nm;
        int flip;
        size_t pi;

        if (!node->mesh) continue;
        cgltf_node_transform_world( node, world );
        memcpy( m, world, sizeof( world ) ); /* both column-major */
        glm_mat4_pick3( m, nm );
        flip = glm_mat3_det( nm ) < 0.0f;
        glm_mat3_inv( nm, nm );
        glm_mat3_transpose( nm );

        for (pi = 0; pi < node->mesh->primitives_count; ++pi)
        {
            const cgltf_primitive * prim = &node->mesh->primitives[pi];
            const cgltf_accessor * pos = NULL, * nor = NULL, * uv = NULL;
            size_t a, n, t;
            GltfGroup * g;

            if (prim->type != cgltf_primitive_type_triangles) continue;
            for (a = 0; a < prim->attributes_count; ++a)
            {
                const cgltf_attribute * at = &prim->attributes[a];
                if (at->type == cgltf_attribute_type_position) pos = at->data;
                else if (at->type == cgltf_attribute_type_normal) nor = at->data;
                else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv = at->data;
            }
            if (!pos) continue;
            g = prim->material ? &groups[s3d_gltf_index_of( data->materials, prim->material, sizeof( cgltf_material ) )] : &groups[ng - 1];
            n = prim->indices ? prim->indices->count : pos->count;
            n -= n % 3;
            if (!s3d_gltf_group_reserve( g, n, rigged )) { strcpy( err, "out of memory" ); break; }
            for (t = 0; t < n; t += 3)
            {
                size_t k;
                vec3 p[3], nn[3], lpos[3], lnor[3];
                float tc[3][2];

                for (k = 0; k < 3; ++k)
                {
                    size_t vi = prim->indices ? cgltf_accessor_read_index( prim->indices, t + k ) : t + k;
                    vec4 lp = { 0, 0, 0, 1 }, wp;
                    vec3 ln = { 0, 1, 0 };

                    cgltf_accessor_read_float( pos, vi, lp, 3 );
                    glm_vec3_copy( lp, lpos[k] );
                    glm_mat4_mulv( m, lp, wp );
                    glm_vec3_copy( wp, p[k] );
                    if (nor) cgltf_accessor_read_float( nor, vi, ln, 3 );
                    glm_vec3_copy( ln, lnor[k] );
                    glm_mat3_mulv( nm, ln, nn[k] );
                    glm_vec3_normalize( nn[k] );
                    tc[k][0] = tc[k][1] = 0.0f;
                    if (uv) cgltf_accessor_read_float( uv, vi, tc[k], 2 );
                }
                if (!nor)
                {
                    /* flat normal from the triangle */
                    vec3 e1, e2, fn;
                    glm_vec3_sub( p[1], p[0], e1 ); glm_vec3_sub( p[2], p[0], e2 ); glm_vec3_cross( e1, e2, fn ); glm_vec3_normalize( fn );
                    if (flip) glm_vec3_negate( fn );
                    glm_vec3_copy( fn, nn[0] ); glm_vec3_copy( fn, nn[1] ); glm_vec3_copy( fn, nn[2] );
                    /* in the node's space: the normal matrix maps the local cross product to the front-face normal,
                       mirrored nodes included */
                    glm_vec3_sub( lpos[1], lpos[0], e1 ); glm_vec3_sub( lpos[2], lpos[0], e2 ); glm_vec3_cross( e1, e2, fn ); glm_vec3_normalize( fn );
                    glm_vec3_copy( fn, lnor[0] ); glm_vec3_copy( fn, lnor[1] ); glm_vec3_copy( fn, lnor[2] );
                }
                for (k = 0; k < 3; ++k)
                {
                    size_t src = flip ? 2 - k : k; /* mirrored nodes: keep counter-clockwise front faces */
                    size_t o = g->count;
                    int c;

                    for (c = 0; c < 3; ++c)
                    {
                        g->P[3 * o + c] = p[src][c];
                        g->N[3 * o + c] = nn[src][c];
                        if (p[src][c] < bmin[c]) bmin[c] = p[src][c];
                        if (p[src][c] > bmax[c]) bmax[c] = p[src][c];
                    }
                    g->U[2 * o] = tc[src][0];
                    g->U[2 * o + 1] = tc[src][1];
                    if (rigged)
                    {
                        g->node[o] = (unsigned int)i;
                        memcpy( g->LP + 3 * o, lpos[src], 3 * sizeof( float ) );
                        memcpy( g->LN + 3 * o, lnor[src], 3 * sizeof( float ) );
                    }
                    g->count++;
                }
            }
        }
    }
    if (err[0])
    {
        s3d_gltf_free_groups( groups, ng ); cgltf_free( data );
        return s3d_error( L, "s3d.loadGLTF: %s", err );
    }

    lua_createtable( L, 0, 6 );

    /* groups = { { material = i (1-based, 0 = none), P = {}, U = {}, N = {} }, ... } */
    lua_createtable( L, (int)ng, 0 );
    {
        int out = 0;
        for (gi = 0; gi < ng; ++gi)
        {
            GltfGroup * g = &groups[gi];
            if (!g->count) continue;
            lua_createtable( L, 0, 4 );
            lua_pushnumber( L, g->material ? (lua_Number)( gi + 1 ) : 0 ); lua_setfield( L, -2, "material" );
            s3d_push_float_array( L, g->P, g->count * 3 ); lua_setfield( L, -2, "P" );
            s3d_push_float_array( L, g->U, g->count * 2 ); lua_setfield( L, -2, "U" );
            s3d_push_float_array( L, g->N, g->count * 3 ); lua_setfield( L, -2, "N" );
            lua_rawseti( L, -2, ++out );
        }
    }
    lua_setfield( L, -2, "groups" );

    /* materials = { { name, image (1-based), color = { r, g, b, a }, alphaMode, alphaCutoff, doubleSided, nearest } } */
    lua_createtable( L, (int)data->materials_count, 0 );
    for (i = 0; i < data->materials_count; ++i)
    {
        const cgltf_material * mt = &data->materials[i];
        const cgltf_texture * tex = mt->has_pbr_metallic_roughness ? mt->pbr_metallic_roughness.base_color_texture.texture : NULL;
        const float * f = mt->has_pbr_metallic_roughness ? mt->pbr_metallic_roughness.base_color_factor : NULL;

        lua_createtable( L, 0, 7 );
        lua_pushstring( L, mt->name ? mt->name : "" ); lua_setfield( L, -2, "name" );
        if (tex && tex->image) { lua_pushnumber( L, s3d_gltf_index_of( data->images, tex->image, sizeof( cgltf_image ) ) + 1 ); lua_setfield( L, -2, "image" ); }
        if (tex && tex->sampler)
        {
            lua_pushboolean( L, tex->sampler->mag_filter == 9728 ); /* GL_NEAREST: pixel art (Blockbench) */
            lua_setfield( L, -2, "nearest" );
        }
        lua_createtable( L, 4, 0 );
        {
            int c;
            for (c = 0; c < 4; ++c) { lua_pushnumber( L, f ? f[c] : 1.0f ); lua_rawseti( L, -2, c + 1 ); }
        }
        lua_setfield( L, -2, "color" );
        lua_pushstring( L, mt->alpha_mode == cgltf_alpha_mode_mask ? "MASK" : mt->alpha_mode == cgltf_alpha_mode_blend ? "BLEND" : "OPAQUE" );
        lua_setfield( L, -2, "alphaMode" );
        lua_pushnumber( L, mt->alpha_cutoff ); lua_setfield( L, -2, "alphaCutoff" );
        lua_pushboolean( L, mt->double_sided ); lua_setfield( L, -2, "doubleSided" );
        lua_rawseti( L, -2, (int)i + 1 );
    }
    lua_setfield( L, -2, "materials" );

    /* images = { { data = "...", mime = "image/png", name }, ... } (embedded in the GLB or data: URIs) */
    lua_createtable( L, (int)data->images_count, 0 );
    for (i = 0; i < data->images_count; ++i)
    {
        const cgltf_image * im = &data->images[i];
        const char * mime = im->mime_type ? im->mime_type : "image/png";

        lua_createtable( L, 0, 3 );
        if (im->buffer_view && im->buffer_view->buffer->data)
        {
            lua_pushlstring( L, (const char *)im->buffer_view->buffer->data + im->buffer_view->offset, im->buffer_view->size );
            lua_setfield( L, -2, "data" );
        }
        else if (im->uri && 0 == strncmp( im->uri, "data:", 5 ))
        {
            const char * comma = strchr( im->uri, ',' );
            if (comma)
            {
                size_t n = strlen( comma + 1 ), size = n / 4 * 3;
                void * out = NULL;
                while (n > 0 && comma[n] == '=') { --n; --size; }
                if (cgltf_load_buffer_base64( &options, size, comma + 1, &out ) == cgltf_result_success)
                {
                    lua_pushlstring( L, (const char *)out, size );
                    lua_setfield( L, -2, "data" );
                    free( out );
                }
            }
            if (0 == strncmp( im->uri, "data:image/jpeg", 15 )) mime = "image/jpeg";
        }
        else if (im->uri)
        {
            lua_pushstring( L, im->uri ); lua_setfield( L, -2, "uri" ); /* external image: loaded by Solar2D by name */
        }
        lua_pushstring( L, mime ); lua_setfield( L, -2, "mime" );
        lua_rawseti( L, -2, (int)i + 1 );
    }
    lua_setfield( L, -2, "images" );

    /* animations = { "name", ... } (played in a later version) */
    lua_createtable( L, (int)data->animations_count, 0 );
    for (i = 0; i < data->animations_count; ++i)
    {
        lua_pushstring( L, data->animations[i].name ? data->animations[i].name : "" );
        lua_rawseti( L, -2, (int)i + 1 );
    }
    lua_setfield( L, -2, "animations" );

    lua_createtable( L, 6, 0 );
    for (i = 0; i < 3; ++i)
    {
        lua_pushnumber( L, bmin[i] <= bmax[i] ? bmin[i] : 0 ); lua_rawseti( L, -2, (int)i + 1 );
    }
    for (i = 0; i < 3; ++i)
    {
        lua_pushnumber( L, bmin[i] <= bmax[i] ? bmax[i] : 0 ); lua_rawseti( L, -2, (int)i + 4 );
    }
    lua_setfield( L, -2, "bounds" );

    if (rigged)
    {
        s3d_gltf_push_rig( L, data, groups, ng );
        lua_setfield( L, -2, "rig" );
    }

    s3d_gltf_free_groups( groups, ng );
    cgltf_free( data );
    return 1;
}

/* s3d._newBuffer( { numbers... } ) -> userdata float[] */
static int s3d_l_new_buffer( lua_State * L )
{
    size_t n, i;
    float * buf;

    if (LUA_TTABLE != lua_type( L, 1 )) return s3d_error( L, "s3d.newBuffer: expected a table of numbers" );
    n = lua_objlen( L, 1 );
    buf = (float *)lua_newuserdata( L, ( n ? n : 1 ) * sizeof( float ) );
    for (i = 0; i < n; ++i) buf[i] = s3d_tablenum( L, 1, (int)i + 1, 0 );
    return 1;
}

/* s3d._project( x, y, z ) -> sx, sy (NDC -1..1), depth ; handy for touch / raycasts */
static int s3d_l_project( lua_State * L )
{
    vec4 p, out;

    p[0] = s3d_optnum( L, 1, 0 ); p[1] = s3d_optnum( L, 2, 0 ); p[2] = s3d_optnum( L, 3, 0 ); p[3] = 1;
    glm_mat4_mulv( *(mat4 *)gFrame.viewProj, p, out );
    if (out[3] == 0.0f) return 0;
    lua_pushnumber( L, out[0] / out[3] );
    lua_pushnumber( L, out[1] / out[3] );
    lua_pushnumber( L, out[2] / out[3] );
    return 3;
}

/* s3d._unproject( ndcX, ndcY ) -> ox, oy, oz, dx, dy, dz : ray from the camera through a screen point */
static int s3d_l_unproject( lua_State * L )
{
    mat4 inv;
    vec4 a, b, pa, pb;
    vec3 dir;

    glm_mat4_inv( *(mat4 *)gFrame.viewProj, inv );
    a[0] = b[0] = s3d_optnum( L, 1, 0 );
    a[1] = b[1] = s3d_optnum( L, 2, 0 );
    a[2] = -1; b[2] = 1; a[3] = b[3] = 1;
    glm_mat4_mulv( inv, a, pa );
    glm_mat4_mulv( inv, b, pb );
    glm_vec4_scale( pa, 1.0f / pa[3], pa );
    glm_vec4_scale( pb, 1.0f / pb[3], pb );
    glm_vec3_sub( pb, pa, dir );
    glm_vec3_normalize( dir );
    lua_pushnumber( L, pa[0] ); lua_pushnumber( L, pa[1] ); lua_pushnumber( L, pa[2] );
    lua_pushnumber( L, dir[0] ); lua_pushnumber( L, dir[1] ); lua_pushnumber( L, dir[2] );
    return 6;
}

/* ========================================================================== */
/* Entry point                                                                  */
/* ========================================================================== */

#include "s3d_lua.h" /* const char kS3DLua[]: the Lua half of the plugin */

S3D_EXPORT int luaopen_plugin_s3d( lua_State * L )
{
    const char * missing = s3d_bind_all();

    if (missing)
    {
#ifdef S3D_DYNAMIC
        if (p_lua_pushstring && p_lua_error)
        {
            char buf[256];

            snprintf( buf, sizeof( buf ), "plugin.s3d: function '%s' not found in the process", missing );
            p_lua_pushstring( L, buf );
            return p_lua_error( L );
        }
#endif
        return 0;
    }

    if (0 != luaL_loadbuffer( L, kS3DLua, sizeof( kS3DLua ) - 1, "=plugin.s3d" ))
    {
        return lua_error( L );
    }

    /* the Lua half is a function that takes the table of C functions and returns the module */
    lua_createtable( L, 0, 8 );
    lua_pushcclosure( L, s3d_l_init, 0 ); lua_setfield( L, -2, "init" );
    lua_pushcclosure( L, s3d_l_set_camera, 0 ); lua_setfield( L, -2, "setCamera" );
    lua_pushcclosure( L, s3d_l_set_light, 0 ); lua_setfield( L, -2, "setLight" );
    lua_pushcclosure( L, s3d_l_new_buffer, 0 ); lua_setfield( L, -2, "newBuffer" );
    lua_pushcclosure( L, s3d_l_project, 0 ); lua_setfield( L, -2, "project" );
    lua_pushcclosure( L, s3d_l_unproject, 0 ); lua_setfield( L, -2, "unproject" );
    lua_pushcclosure( L, s3d_l_set_fog, 0 ); lua_setfield( L, -2, "setFog" );
    lua_pushcclosure( L, s3d_l_set_shadow_plane, 0 ); lua_setfield( L, -2, "setShadowPlane" );
    lua_pushcclosure( L, s3d_l_begin_frame, 0 ); lua_setfield( L, -2, "beginFrame" );
    lua_pushcclosure( L, s3d_l_set_shadow_map, 0 ); lua_setfield( L, -2, "setShadowMap" );
    lua_pushcclosure( L, s3d_l_set_ambient, 0 ); lua_setfield( L, -2, "setAmbient" );
    lua_pushcclosure( L, s3d_l_set_focus, 0 ); lua_setfield( L, -2, "setFocus" );
    lua_pushcclosure( L, s3d_l_bake_ao, 0 ); lua_setfield( L, -2, "bakeAO" );
    lua_pushcclosure( L, s3d_l_new_dynamic, 0 ); lua_setfield( L, -2, "newDynamic" );
    lua_pushcclosure( L, s3d_l_gltf_load, 0 ); lua_setfield( L, -2, "gltfLoad" );
    lua_pushcclosure( L, s3d_l_gltf_set_origin, 0 ); lua_setfield( L, -2, "gltfSetOrigin" );
    lua_pushcclosure( L, s3d_l_gltf_attach, 0 ); lua_setfield( L, -2, "gltfAttach" );
    lua_pushcclosure( L, s3d_l_gltf_durations, 0 ); lua_setfield( L, -2, "gltfDurations" );
    lua_pushcclosure( L, s3d_l_gltf_pose, 0 ); lua_setfield( L, -2, "gltfPose" );
    lua_pushcclosure( L, s3d_l_gltf_clone_rig, 0 ); lua_setfield( L, -2, "gltfCloneRig" );
    lua_pushcclosure( L, s3d_l_dyn_set_positions, 0 ); lua_setfield( L, -2, "dynSetPositions" );
    lua_pushcclosure( L, s3d_l_dyn_set_heights, 0 ); lua_setfield( L, -2, "dynSetHeights" );
    lua_pushcclosure( L, s3d_l_dyn_set_vertex, 0 ); lua_setfield( L, -2, "dynSetVertex" );
    lua_pushcclosure( L, s3d_l_dyn_get_vertex, 0 ); lua_setfield( L, -2, "dynGetVertex" );
    lua_pushcclosure( L, s3d_l_dyn_update, 0 ); lua_setfield( L, -2, "dynUpdate" );
    lua_pushstring( L, S3D_VERSION ); lua_setfield( L, -2, "version" );
    lua_call( L, 1, 1 );
    return 1;
}
