/* gles2.c - the fixed-function OpenGL the viewer draws with, on OpenGL ES 2.0 / 3.0 (port extra, WoodyRE's shim with
 * lighting, fog, sphere maps and the texture matrix added; see GL/gl.h). The state the shaders need is kept here and
 * handed over at each draw: the matrices, the lights, the fog, the texture units, the alpha test, the client arrays (as
 * vertex attributes 0..4) and the current colour / normal / texcoords for the arrays that are off. */
#define RAMPAGE_GLES_IMPL
#include "GL/gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the client arrays -------------------------------------------------------------------------------------------- */
enum { A_VERT, A_COL, A_TC0, A_TC1, A_NRM, A_N };          /* = the attribute locations */
typedef struct { int on; GLint size; GLenum type; GLsizei stride; const void *p; GLuint buf; } Arr;   /* buf: p is its offset */
static Arr g_arr[A_N];
static GLenum g_unit = GL_TEXTURE0;                        /* glClientActiveTexture */

static int arr_of(GLenum array)
{
    return array == GL_VERTEX_ARRAY ? A_VERT : array == GL_COLOR_ARRAY ? A_COL : array == GL_NORMAL_ARRAY ? A_NRM :
           array == GL_TEXTURE_COORD_ARRAY ? (g_unit == GL_TEXTURE1 ? A_TC1 : A_TC0) : -1;
}
void gles_enable_client_state(GLenum array) { int a = arr_of(array); if (a >= 0) g_arr[a].on = 1; }
void gles_disable_client_state(GLenum array) { int a = arr_of(array); if (a >= 0) g_arr[a].on = 0; }
void gles_client_active_texture(GLenum unit) { g_unit = unit; }
/* ---- buffer objects: the bindings as the viewer made them (an array takes the GL_ARRAY_BUFFER bound at its pointer
 * call, as in GL; flush binds each one's for its glVertexAttribPointer) and, on a bare ES 2.0 (no 32-bit indices), a
 * copy of each buffer's data for gles_draw_elements to read */
static GLuint g_abuf, g_ebuf, g_es_abuf;                   /* the viewer's GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER; ES's */
static struct { void *p; size_t n; } *g_copy; static GLuint g_ncopy;
static int uint_ok(void);
static void es_array_buffer(GLuint b) { if (b != g_es_abuf) { glBindBuffer(GL_ARRAY_BUFFER, b); g_es_abuf = b; } }
void gles_bind_buffer(GLenum target, GLuint b)
{
    if (target == GL_ARRAY_BUFFER) { g_abuf = b; es_array_buffer(b); return; }
    if (target == GL_ELEMENT_ARRAY_BUFFER) g_ebuf = b;
    glBindBuffer(target, b);
}
void gles_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    glBufferData(target, size, data, usage);
    GLuint b = target == GL_ARRAY_BUFFER ? g_abuf : target == GL_ELEMENT_ARRAY_BUFFER ? g_ebuf : 0;
    if (!b || uint_ok()) return;
    if (b >= g_ncopy) {
        GLuint n = b + 64; void *t = realloc(g_copy, sizeof *g_copy * n); if (!t) return;
        g_copy = t; memset(g_copy + g_ncopy, 0, sizeof *g_copy * (n - g_ncopy)); g_ncopy = n;
    }
    void *c = realloc(g_copy[b].p, size > 0 ? (size_t)size : 1); if (!c) return;
    if (data) memcpy(c, data, (size_t)size);
    g_copy[b].p = c; g_copy[b].n = (size_t)size;
}
void gles_delete_buffers(GLsizei n, const GLuint *b)
{
    for (GLsizei i = 0; i < n; i++) {
        if (b[i] < g_ncopy) { free(g_copy[b[i]].p); g_copy[b[i]].p = NULL; g_copy[b[i]].n = 0; }
        if (b[i] == g_abuf) g_abuf = 0;
        if (b[i] == g_es_abuf) g_es_abuf = 0;
        if (b[i] == g_ebuf) g_ebuf = 0;
    }
    glDeleteBuffers(n, b);
}
static const void *copy_at(GLuint b, const void *p) { return b < g_ncopy && g_copy[b].p ? (const unsigned char *)g_copy[b].p + (size_t)p : NULL; }

static void track(int a, GLint size, GLenum type, GLsizei stride, const void *p) { g_arr[a].size = size; g_arr[a].type = type; g_arr[a].stride = stride; g_arr[a].p = p; g_arr[a].buf = g_abuf; }
void gles_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *p) { track(A_VERT, size, type, stride, p); }
void gles_normal_pointer(GLenum type, GLsizei stride, const void *p) { track(A_NRM, 3, type, stride, p); }
void gles_color_pointer(GLint size, GLenum type, GLsizei stride, const void *p) { track(A_COL, size, type, stride, p); }
void gles_tex_coord_pointer(GLint size, GLenum type, GLsizei stride, const void *p) { track(g_unit == GL_TEXTURE1 ? A_TC1 : A_TC0, size, type, stride, p); }

/* glPushClientAttrib / glPopClientAttrib (any mask = the arrays) */
#define CDEPTH 4
static struct { Arr arr[A_N]; GLenum unit; } g_carr[CDEPTH]; static int g_csp;
void gles_push_client_attrib(GLbitfield mask)
{
    (void)mask;
    if (g_csp == CDEPTH) { printf("gles: glPushClientAttrib stack full\n"); return; }
    memcpy(g_carr[g_csp].arr, g_arr, sizeof g_arr); g_carr[g_csp++].unit = g_unit;
}
void gles_pop_client_attrib(void) { if (g_csp) { g_csp--; memcpy(g_arr, g_carr[g_csp].arr, sizeof g_arr); g_unit = g_carr[g_csp].unit; } }

/* ---- the fixed-function state ------------------------------------------------------------------------------------- */
typedef struct { float pos[4], dif[4], amb[4]; } Light;   /* pos: eye space (GL_POSITION under the modelview then) */
static struct {
    GLboolean tex[2]; GLint env[2]; float scale[2];        /* per texture unit: GL_TEXTURE_2D, GL_TEXTURE_ENV_MODE, GL_RGB_SCALE */
    GLboolean atest; GLenum afunc; float aref;
    float col[4], tc[2][2], nrm[3];                        /* the current colour, texcoords and normal */
    GLenum active;                                         /* glActiveTexture */
    GLint vp[4], blend[2]; int have_vp;
    Light light[4]; float model_amb[4]; GLenum cmat;       /* the lights, GL_LIGHT_MODEL_AMBIENT, glColorMaterial */
    GLenum fog_mode; float fog_col[4], fog_start, fog_end, fog_density;
    GLenum gen[2];                                         /* GL_TEXTURE_GEN_MODE of S and T */
    struct { GLenum cap; GLboolean on; } caps[48]; int ncaps;
} S = { .env = { GL_MODULATE, GL_MODULATE }, .scale = { 1, 1 }, .afunc = GL_ALWAYS, .col = { 1, 1, 1, 1 }, .nrm = { 0, 0, 1 },
        .active = GL_TEXTURE0, .blend = { GL_ONE, GL_ZERO },
        .light = { { { 0, 0, 1, 0 }, { 1, 1, 1, 1 }, { 0, 0, 0, 1 } }, { { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 } },
                   { { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 } }, { { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 } } },
        .model_amb = { 0.2f, 0.2f, 0.2f, 1 }, .cmat = GL_AMBIENT_AND_DIFFUSE,
        .fog_mode = GL_EXP, .fog_end = 1, .fog_density = 1, .gen = { 0x2401 /* GL_EYE_LINEAR */, 0x2401 } };

static int ff_cap(GLenum cap)                              /* a cap the shaders do (or ES does not know) */
{
    return cap == GL_TEXTURE_2D || cap == GL_ALPHA_TEST || cap == GL_LIGHTING || (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) ||
           cap == GL_FOG || cap == GL_NORMALIZE || cap == GL_RESCALE_NORMAL || cap == GL_COLOR_MATERIAL ||
           cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T || cap == 0x0C62 || cap == 0x0C63 /* GEN_R, GEN_Q */ ||
           cap == GL_LINE_SMOOTH || cap == GL_POINT_SMOOTH || cap == GL_POLYGON_SMOOTH || cap == GL_MULTISAMPLE;
}
static GLboolean *cap_slot(GLenum cap)
{
    if (cap == GL_TEXTURE_2D) return &S.tex[S.active == GL_TEXTURE1];
    if (cap == GL_ALPHA_TEST) return &S.atest;
    for (int i = 0; i < S.ncaps; i++) if (S.caps[i].cap == cap) return &S.caps[i].on;
    static GLboolean spare;
    if (S.ncaps == (int)(sizeof S.caps / sizeof *S.caps)) return &spare;
    S.caps[S.ncaps].cap = cap; S.caps[S.ncaps].on = cap == GL_DITHER;   /* the GL defaults */
    return &S.caps[S.ncaps++].on;
}
static int on(GLenum cap) { return *cap_slot(cap); }
void gles_enable(GLenum cap) { *cap_slot(cap) = GL_TRUE; if (!ff_cap(cap)) glEnable(cap); }
void gles_disable(GLenum cap) { *cap_slot(cap) = GL_FALSE; if (!ff_cap(cap)) glDisable(cap); }
GLboolean gles_is_enabled(GLenum cap) { return *cap_slot(cap); }
void gles_active_texture(GLenum unit) { S.active = unit; glActiveTexture(unit); }
void gles_viewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; S.have_vp = 1; glViewport(x, y, w, h); }
void gles_blend_func(GLenum src, GLenum dst) { S.blend[0] = (GLint)src; S.blend[1] = (GLint)dst; glBlendFunc(src, dst); }
void gles_tex_envi(GLenum target, GLenum pname, GLint v)
{
    int u = S.active == GL_TEXTURE1;
    if (target != GL_TEXTURE_ENV) return;
    if (pname == GL_TEXTURE_ENV_MODE) S.env[u] = v;
    else if (pname == GL_RGB_SCALE) S.scale[u] = (float)v;
    /* GL_COMBINE_RGB / _ALPHA and the sources: only texture x primary colour (GL_MODULATE) is combined */
}
void gles_tex_envf(GLenum target, GLenum pname, GLfloat v)
{
    if (target == GL_TEXTURE_ENV && pname == GL_RGB_SCALE) S.scale[S.active == GL_TEXTURE1] = v;
    else gles_tex_envi(target, pname, (GLint)v);
}
void gles_get_tex_enviv(GLenum target, GLenum pname, GLint *v)
{
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) *v = S.env[S.active == GL_TEXTURE1]; else *v = 0;
}
void gles_tex_geni(GLenum coord, GLenum pname, GLint param)
{
    if (pname == GL_TEXTURE_GEN_MODE && (coord == GL_S || coord == GL_T)) S.gen[coord == GL_T] = (GLenum)param;
}
void gles_alpha_func(GLenum func, GLfloat ref) { S.afunc = func; S.aref = ref < 0 ? 0 : ref > 1 ? 1 : ref; }
void gles_color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);   /* below, with glBegin */

/* ---- the matrices (column-major, as GL): modelview, projection, texture of units 0 and 1 ----------------------------- */
#define MDEPTH 32
static float g_m[4][MDEPTH][16]; static int g_sp[4], g_mode, g_minit;
static float *top(int k) { return g_m[k][g_sp[k]]; }
static float *cur(void) { return top(g_mode); }
static void ident(float *m) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_init(void) { if (!g_minit) { g_minit = 1; for (int k = 0; k < 4; k++) ident(g_m[k][0]); } }
static void mul(float *r, const float *a, const float *b)  /* r = a * b */
{
    float t[16];
    for (int c = 0; c < 4; c++) for (int w = 0; w < 4; w++) t[c * 4 + w] = a[w] * b[c * 4] + a[4 + w] * b[c * 4 + 1] + a[8 + w] * b[c * 4 + 2] + a[12 + w] * b[c * 4 + 3];
    memcpy(r, t, sizeof t);
}
void gles_matrix_mode(GLenum mode) { mat_init(); g_mode = mode == GL_PROJECTION ? 1 : mode == GL_TEXTURE ? 2 + (S.active == GL_TEXTURE1) : 0; }
void gles_load_identity(void) { mat_init(); ident(cur()); }
void gles_load_matrixf(const GLfloat *m) { mat_init(); memcpy(cur(), m, 16 * sizeof(float)); }
void gles_mult_matrixf(const GLfloat *m) { mat_init(); mul(cur(), cur(), m); }
void gles_translatef(GLfloat x, GLfloat y, GLfloat z) { float t[16]; ident(t); t[12] = x; t[13] = y; t[14] = z; gles_mult_matrixf(t); }
void gles_scalef(GLfloat x, GLfloat y, GLfloat z) { float t[16]; ident(t); t[0] = x; t[5] = y; t[10] = z; gles_mult_matrixf(t); }
void gles_ortho(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
    float o[16]; ident(o);
    o[0] = 2 / (r - l); o[5] = 2 / (t - b); o[10] = -2 / (f - n);
    o[12] = -(r + l) / (r - l); o[13] = -(t + b) / (t - b); o[14] = -(f + n) / (f - n);
    gles_mult_matrixf(o);
}
void gles_push_matrix(void) { mat_init(); if (g_sp[g_mode] + 1 < MDEPTH) { memcpy(g_m[g_mode][g_sp[g_mode] + 1], cur(), 16 * sizeof(float)); g_sp[g_mode]++; } }
void gles_pop_matrix(void) { if (g_sp[g_mode] > 0) g_sp[g_mode]--; }

/* ---- lights and fog ------------------------------------------------------------------------------------------------ */
void gles_lightfv(GLenum light, GLenum pname, const GLfloat *v)
{
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 4) return;
    Light *l = &S.light[light - GL_LIGHT0];
    if (pname == GL_POSITION) {                            /* into eye space now, as GL does */
        mat_init();
        const float *m = top(0);
        for (int k = 0; k < 4; k++) l->pos[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
    } else if (pname == GL_DIFFUSE) memcpy(l->dif, v, sizeof l->dif);
    else if (pname == GL_AMBIENT) memcpy(l->amb, v, sizeof l->amb);
    /* GL_SPECULAR, spots, attenuation: not there (the materials have no specular) */
}
void gles_light_modelfv(GLenum pname, const GLfloat *v) { if (pname == GL_LIGHT_MODEL_AMBIENT) memcpy(S.model_amb, v, sizeof S.model_amb); }
void gles_light_modeli(GLenum pname, GLint v) { (void)pname; (void)v; }   /* two-sided, local viewer: off is all there is */
void gles_color_material(GLenum face, GLenum mode) { (void)face; S.cmat = mode; }
void gles_fogf(GLenum pname, GLfloat v)
{
    if (pname == GL_FOG_MODE) S.fog_mode = (GLenum)v;
    else if (pname == GL_FOG_START) S.fog_start = v;
    else if (pname == GL_FOG_END) S.fog_end = v;
    else if (pname == GL_FOG_DENSITY) S.fog_density = v;
}
void gles_fogi(GLenum pname, GLint v) { if (pname == GL_FOG_MODE) S.fog_mode = (GLenum)v; else gles_fogf(pname, (GLfloat)v); }
void gles_fogfv(GLenum pname, const GLfloat *v) { if (pname == GL_FOG_COLOR) memcpy(S.fog_col, v, sizeof S.fog_col); else gles_fogf(pname, v[0]); }

void gles_get_integerv(GLenum pname, GLint *v)
{
    if (pname == GL_VIEWPORT && S.have_vp) memcpy(v, S.vp, sizeof S.vp);
    else if (pname == GL_BLEND_SRC) *v = S.blend[0];
    else if (pname == GL_BLEND_DST) *v = S.blend[1];
    else glGetIntegerv(pname, v);
}
void gles_get_floatv(GLenum pname, GLfloat *v)
{
    mat_init();
    if (pname == GL_MODELVIEW_MATRIX) memcpy(v, top(0), 16 * sizeof(float));
    else if (pname == GL_PROJECTION_MATRIX) memcpy(v, top(1), 16 * sizeof(float));
    else if (pname == GL_TEXTURE_MATRIX) memcpy(v, top(2 + (S.active == GL_TEXTURE1)), 16 * sizeof(float));
    else if (pname == GL_FOG_COLOR) memcpy(v, S.fog_col, sizeof S.fog_col);
    else if (pname == GL_CURRENT_COLOR) memcpy(v, S.col, sizeof S.col);
    else glGetFloatv(pname, v);
}

/* ---- the shaders: one pair, the fixed-function stages switched by uniforms ----------------------------------------- */
static const char *k_vs =
    "attribute vec4 a_pos; attribute vec4 a_col; attribute vec4 a_tc0; attribute vec4 a_tc1; attribute vec3 a_nrm;\n"
    "uniform mat4 u_mvp; uniform mat4 u_mv; uniform mat4 u_tm; uniform mat3 u_nm;\n"
    "uniform vec4 u_mode;\n"                                 /* lighting, normalize, sphere map, colour material (1 amb+dif 2 amb 3 dif) */
    "uniform vec4 u_amb;\n"                                  /* the light model's + the lights' ambient */
    "uniform vec4 u_ldir[4]; uniform vec4 u_ldif[4];\n"      /* to the light (eye space, unit) and its diffuse; off = 0 */
    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1; varying float v_z;\n"
    "void main() {\n"
    "  vec4 e = u_mv * a_pos;\n"
    "  gl_Position = u_mvp * a_pos;\n"
    "  v_z = abs(e.z);\n"                                    /* the fog's distance: the eye plane's, as Mesa and the others */
    "  vec3 n = u_nm * a_nrm;\n"
    "  if (u_mode.y > 0.5) n = normalize(n);\n"
    "  vec4 c = a_col;\n"
    "  if (u_mode.x > 0.5) {\n"                              /* emission 0 + amb x (model + lights' ambient) + dif x sum(N.L x light) */
    "    vec4 amb = u_mode.w > 0.5 && u_mode.w < 2.5 ? a_col : vec4(0.2, 0.2, 0.2, 1.0);\n"
    "    vec4 dif = u_mode.w > 0.5 && (u_mode.w < 1.5 || u_mode.w > 2.5) ? a_col : vec4(0.8, 0.8, 0.8, 1.0);\n"
    "    vec3 d = vec3(0.0);\n"
    "    for (int i = 0; i < 4; i++) d += u_ldif[i].rgb * max(dot(n, u_ldir[i].xyz), 0.0);\n"
    "    c = vec4(amb.rgb * u_amb.rgb + dif.rgb * d, dif.a);\n"
    "  }\n"
    "  v_col = clamp(c, 0.0, 1.0);\n"
    "  vec4 t = a_tc0;\n"
    "  if (u_mode.z > 0.5) {\n"                              /* GL_SPHERE_MAP: the reflection of the eye ray */
    "    vec3 r = reflect(normalize(e.xyz), n);\n"
    "    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n"
    "    t.xy = r.xy / m + 0.5;\n"
    "  }\n"
    "  v_tc0 = (u_tm * t).xy; v_tc1 = a_tc1.xy;\n"
    "}\n";
static const char *k_fs =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "uniform sampler2D u_t0; uniform sampler2D u_t1;\n"
    "uniform vec4 u_tex;\n"                                  /* unit 0 on, unit 1 on, unit 0 scale, unit 1 scale */
    "uniform vec4 u_env;\n"                                  /* unit 0 mode, unit 1 mode, alpha test function, alpha ref */
    "uniform vec4 u_fog; uniform vec3 u_fogc;\n"             /* mode (0 off 1 linear 2 exp 3 exp2), end, 1 / (end - start), density */
    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1; varying float v_z;\n"
    "vec4 stage(vec4 c, vec4 t, float mode, float s) {\n"   /* 0 modulate (combine: x scale), 1 replace, 2 add, 3 decal */
    "  if (mode < 0.5) return vec4(min(c.rgb * t.rgb * s, 1.0), c.a * t.a);\n"
    "  if (mode < 1.5) return t;\n"
    "  if (mode < 2.5) return vec4(min(c.rgb + t.rgb, 1.0), c.a * t.a);\n"
    "  return vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (u_tex.x > 0.5) c = stage(c, texture2D(u_t0, v_tc0), u_env.x, u_tex.z);\n"
    "  if (u_tex.y > 0.5) c = stage(c, texture2D(u_t1, v_tc1), u_env.y, u_tex.w);\n"
    "  if (u_fog.x > 0.5) {\n"
    "    float f = u_fog.x < 1.5 ? (u_fog.y - v_z) * u_fog.z : u_fog.x < 2.5 ? exp(-u_fog.w * v_z) : exp(-(u_fog.w * v_z) * (u_fog.w * v_z));\n"
    "    c.rgb = mix(u_fogc, c.rgb, clamp(f, 0.0, 1.0));\n"
    "  }\n"
    "  float f = u_env.z, a = c.a, r = u_env.w;\n"          /* 1 GEQUAL 2 GREATER 3 LESS 4 LEQUAL 5 EQUAL 6 NOTEQUAL 7 NEVER */
    "  if (f > 0.5) { bool ok = f < 1.5 ? a >= r : f < 2.5 ? a > r : f < 3.5 ? a < r : f < 4.5 ? a <= r : f < 5.5 ? a == r : f < 6.5 ? a != r : false;\n"
    "                 if (!ok) discard; }\n"
    "  gl_FragColor = c;\n"
    "}\n";
static GLuint g_prog; static GLint u_mvp, u_mv, u_tm, u_nm, u_mode, u_amb, u_ldir, u_ldif, u_tex, u_env, u_fog, u_fogc;
static int g_es3 = -1;
static GLuint g_user;                                      /* glUseProgram of a shader pass (0 = the shim's own) */
static int es3(void)
{
    if (g_es3 < 0) { const char *v = (const char *)glGetString(GL_VERSION); g_es3 = v && strstr(v, "OpenGL ES ") && strstr(v, "OpenGL ES ")[10] >= '3'; }
    return g_es3;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type); GLint ok = 0; char log[1024];
    glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, sizeof log, NULL, log); printf("gles: shader: %s\n", log); }
    return s;
}
static void prog_init(void)
{
    GLuint p = glCreateProgram(); GLint ok = 0; char log[1024];
    glAttachShader(p, shader(GL_VERTEX_SHADER, k_vs)); glAttachShader(p, shader(GL_FRAGMENT_SHADER, k_fs));
    glBindAttribLocation(p, A_VERT, "a_pos"); glBindAttribLocation(p, A_COL, "a_col"); glBindAttribLocation(p, A_TC0, "a_tc0");
    glBindAttribLocation(p, A_TC1, "a_tc1"); glBindAttribLocation(p, A_NRM, "a_nrm");
    glLinkProgram(p); glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(p, sizeof log, NULL, log); printf("gles: program: %s\n", log); }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_t0"), 0); glUniform1i(glGetUniformLocation(p, "u_t1"), 1);
    u_mvp = glGetUniformLocation(p, "u_mvp"); u_mv = glGetUniformLocation(p, "u_mv"); u_tm = glGetUniformLocation(p, "u_tm");
    u_nm = glGetUniformLocation(p, "u_nm"); u_mode = glGetUniformLocation(p, "u_mode"); u_amb = glGetUniformLocation(p, "u_amb");
    u_ldir = glGetUniformLocation(p, "u_ldir"); u_ldif = glGetUniformLocation(p, "u_ldif");
    u_tex = glGetUniformLocation(p, "u_tex"); u_env = glGetUniformLocation(p, "u_env");
    u_fog = glGetUniformLocation(p, "u_fog"); u_fogc = glGetUniformLocation(p, "u_fogc");
    g_prog = p; es3();
}
static float env_code(GLint e) { return e == GL_REPLACE ? 1.0f : e == GL_ADD ? 2.0f : e == GL_DECAL ? 3.0f : 0.0f; }
static float afunc_code(void)
{
    if (!S.atest) return 0;
    switch (S.afunc) { case GL_GEQUAL: return 1; case GL_GREATER: return 2; case GL_LESS: return 3; case GL_LEQUAL: return 4;
                       case GL_EQUAL: return 5; case GL_NOTEQUAL: return 6; case GL_NEVER: return 7; default: return 0; }
}
/* the normal matrix: the inverse transpose of the modelview's 3 x 3 (its cofactors over the determinant) */
static void normal_matrix(const float *m, float *n)
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
    float c10 = c * h - b * i, c11 = a * i - c * g, c12 = b * g - a * h;
    float c20 = b * f - c * e, c21 = c * d - a * f, c22 = a * e - b * d;
    float det = a * c00 + b * c01 + c * c02, k = det != 0 ? 1 / det : 0;
    /* column-major: n[col * 3 + row] = cof[row][col] / det, the transpose of the inverse being cof / det */
    n[0] = c00 * k; n[1] = c10 * k; n[2] = c20 * k;
    n[3] = c01 * k; n[4] = c11 * k; n[5] = c21 * k;
    n[6] = c02 * k; n[7] = c12 * k; n[8] = c22 * k;
}
static void flush(const Arr *arr)                          /* the state of the next draw */
{
    mat_init();
    if (!g_prog) { prog_init(); if (g_user) glUseProgram(g_user); }
    if (!g_user) {                                         /* an own program: only the arrays (the positions = attribute 0) */
        glUseProgram(g_prog);
        float mvp[16], nm[9]; mul(mvp, top(1), top(0)); glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(u_mv, 1, GL_FALSE, top(0)); glUniformMatrix4fv(u_tm, 1, GL_FALSE, top(2));
        int lit = on(GL_LIGHTING), gen = on(GL_TEXTURE_GEN_S) && on(GL_TEXTURE_GEN_T) && S.gen[0] == GL_SPHERE_MAP && S.gen[1] == GL_SPHERE_MAP;
        int cm = !on(GL_COLOR_MATERIAL) ? 0 : S.cmat == GL_AMBIENT_AND_DIFFUSE ? 1 : S.cmat == GL_AMBIENT ? 2 : S.cmat == GL_DIFFUSE ? 3 : 0;
        int norm = on(GL_NORMALIZE) || on(GL_RESCALE_NORMAL);
        if (lit || gen) { normal_matrix(top(0), nm); glUniformMatrix3fv(u_nm, 1, GL_FALSE, nm); }
        glUniform4f(u_mode, lit ? 1.0f : 0.0f, norm ? 1.0f : 0.0f, gen ? 1.0f : 0.0f, (float)cm);
        if (lit) {
            float amb[4] = { S.model_amb[0], S.model_amb[1], S.model_amb[2], 1 }, dir[4][4] = { { 0 } }, dif[4][4] = { { 0 } };
            for (int i = 0; i < 4; i++) {
                const Light *l = &S.light[i];
                if (!on(GL_LIGHT0 + i)) continue;
                float len = sqrtf(l->pos[0] * l->pos[0] + l->pos[1] * l->pos[1] + l->pos[2] * l->pos[2]);
                for (int k = 0; k < 3; k++) { amb[k] += l->amb[k]; dif[i][k] = l->dif[k]; dir[i][k] = len > 0 ? l->pos[k] / len : 0; }
            }
            glUniform4fv(u_amb, 1, amb); glUniform4fv(u_ldir, 4, dir[0]); glUniform4fv(u_ldif, 4, dif[0]);
        }
        glUniform4f(u_tex, S.tex[0] ? 1.0f : 0.0f, S.tex[1] ? 1.0f : 0.0f, S.env[0] == GL_COMBINE ? S.scale[0] : 1.0f, S.env[1] == GL_COMBINE ? S.scale[1] : 1.0f);
        glUniform4f(u_env, env_code(S.env[0]), env_code(S.env[1]), afunc_code(), S.aref);
        float fm = !on(GL_FOG) ? 0.0f : S.fog_mode == GL_LINEAR ? 1.0f : S.fog_mode == GL_EXP ? 2.0f : S.fog_mode == GL_EXP2 ? 3.0f : 0.0f;
        glUniform4f(u_fog, fm, S.fog_end, S.fog_end != S.fog_start ? 1 / (S.fog_end - S.fog_start) : 0.0f, S.fog_density);
        if (fm > 0) glUniform3f(u_fogc, S.fog_col[0], S.fog_col[1], S.fog_col[2]);
    }
    for (int a = 0; a < A_N; a++) {
        if (arr[a].on && (arr[a].p || arr[a].buf)) {
            glEnableVertexAttribArray((GLuint)a);
            es_array_buffer(arr[a].buf);
            glVertexAttribPointer((GLuint)a, arr[a].size, arr[a].type, (a == A_COL || a == A_NRM) && arr[a].type != GL_FLOAT, arr[a].stride, arr[a].p);
        } else {
            glDisableVertexAttribArray((GLuint)a);
            if (a == A_COL) glVertexAttrib4fv(A_COL, S.col);
            else if (a == A_VERT) glVertexAttrib4f(A_VERT, 0, 0, 0, 1);
            else if (a == A_NRM) glVertexAttrib4f(A_NRM, S.nrm[0], S.nrm[1], S.nrm[2], 1);
            else glVertexAttrib4f((GLuint)a, S.tc[a - A_TC0][0], S.tc[a - A_TC0][1], 0, 1);
        }
    }
    es_array_buffer(g_abuf);
}
void gles_draw_arrays(GLenum mode, GLint first, GLsizei count) { flush(g_arr); glDrawArrays(mode, first, count); }
void gles_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    flush(g_arr);
    if (type != GL_UNSIGNED_INT || uint_ok()) { glDrawElements(mode, count, type, idx); return; }
    /* a bare ES 2.0: 16 bit indices, or the vertices gathered (from the buffers' copies when they are in buffers) */
    const GLuint *in = (const GLuint *)(g_ebuf ? copy_at(g_ebuf, idx) : idx);
    if (!in) return;
    static GLushort *s; static GLsizei cap; GLuint mx = 0;
    for (GLsizei i = 0; i < count; i++) if (in[i] > mx) mx = in[i];
    if (mx < 65536) {
        if (count > cap) { GLushort *t = (GLushort *)realloc(s, (size_t)count * sizeof *t); if (!t) return; s = t; cap = count; }
        for (GLsizei i = 0; i < count; i++) s[i] = (GLushort)in[i];
        if (g_ebuf) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDrawElements(mode, count, GL_UNSIGNED_SHORT, s);
        if (g_ebuf) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebuf);
        return;
    }
    static unsigned char *buf[A_N]; static size_t bcap[A_N]; Arr g[A_N]; memcpy(g, g_arr, sizeof g);
    for (int a = 0; a < A_N; a++) {
        if (!g[a].on || (!g[a].p && !g[a].buf)) continue;
        if (g[a].buf) { g[a].p = copy_at(g[a].buf, g[a].p); g[a].buf = 0; if (!g[a].p) return; }
        size_t es = (size_t)g[a].size * (g[a].type == GL_UNSIGNED_BYTE || g[a].type == GL_BYTE ? 1 : g[a].type == GL_SHORT || g[a].type == GL_UNSIGNED_SHORT ? 2 : 4);
        size_t st = g[a].stride ? (size_t)g[a].stride : es;
        if ((size_t)count * es > bcap[a]) { unsigned char *t = (unsigned char *)realloc(buf[a], (size_t)count * es); if (!t) return; buf[a] = t; bcap[a] = (size_t)count * es; }
        for (GLsizei i = 0; i < count; i++) memcpy(buf[a] + (size_t)i * es, (const unsigned char *)g[a].p + in[i] * st, es);
        g[a].stride = 0; g[a].p = buf[a];
    }
    flush(g); glDrawArrays(mode, 0, count);
}
static int uint_ok(void)
{
    static int has_uint = -1;
    if (has_uint < 0) { const char *e = (const char *)glGetString(GL_EXTENSIONS); has_uint = es3() || (e && strstr(e, "GL_OES_element_index_uint")); }
    return has_uint;
}

/* ---- glBegin / glEnd ------------------------------------------------------------------------------------------------- */
static struct { GLenum mode; int in, tc1; float *p, *t0, *t1, *c; int n, cap; GLushort *qi; int qcap; } I;

void gles_begin(GLenum mode) { I.mode = mode; I.in = 1; I.n = 0; I.tc1 = 0; }
void gles_vertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (I.n == I.cap) {
        int c = I.cap ? I.cap * 2 : 256; void *a[4] = { I.p, I.t0, I.t1, I.c }; static const size_t k[4] = { 12, 8, 8, 16 };
        for (int i = 0; i < 4; i++) { void *t = realloc(a[i], (size_t)c * k[i]); if (!t) return; a[i] = t; }
        I.p = (float *)a[0]; I.t0 = (float *)a[1]; I.t1 = (float *)a[2]; I.c = (float *)a[3]; I.cap = c;
    }
    size_t n = (size_t)I.n++;
    I.p[n * 3] = x; I.p[n * 3 + 1] = y; I.p[n * 3 + 2] = z;
    I.t0[n * 2] = S.tc[0][0]; I.t0[n * 2 + 1] = S.tc[0][1]; I.t1[n * 2] = S.tc[1][0]; I.t1[n * 2 + 1] = S.tc[1][1];
    memcpy(I.c + n * 4, S.col, sizeof S.col);
}
void gles_texcoord2f(GLfloat s, GLfloat t) { S.tc[0][0] = s; S.tc[0][1] = t; }
void gles_multitexcoord2f(GLenum unit, GLfloat s, GLfloat t) { int k = unit == GL_TEXTURE1; S.tc[k][0] = s; S.tc[k][1] = t; if (k && I.in) I.tc1 = 1; }
void gles_color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.col[0] = r; S.col[1] = g; S.col[2] = b; S.col[3] = a; }
void gles_normal3f(GLfloat x, GLfloat y, GLfloat z) { S.nrm[0] = x; S.nrm[1] = y; S.nrm[2] = z; }
void gles_end(void)
{
    I.in = 0;
    int n = I.n;
    if (I.mode == GL_QUADS) n -= n % 4;
    if (n <= 0) return;
    /* (the current normal of the last glNormal for every vertex: nothing lit is drawn between glBegin and glEnd) */
    Arr a[A_N] = { { 1, 3, GL_FLOAT, 0, I.p }, { 1, 4, GL_FLOAT, 0, I.c }, { 1, 2, GL_FLOAT, 0, I.t0 }, { I.tc1, 2, GL_FLOAT, 0, I.t1 }, { 0, 3, GL_FLOAT, 0, NULL } };
    flush(a);
    if (I.mode == GL_QUADS) {                               /* every quad as two triangles */
        int ni = n / 4 * 6;
        if (n > 65536) return;
        if (ni > I.qcap) { GLushort *q = (GLushort *)realloc(I.qi, (size_t)ni * sizeof *q); if (!q) return; I.qi = q; I.qcap = ni; }
        for (int q = 0, o = 0; q < n; q += 4) { I.qi[o++] = (GLushort)q; I.qi[o++] = (GLushort)(q + 1); I.qi[o++] = (GLushort)(q + 2);
                                                I.qi[o++] = (GLushort)q; I.qi[o++] = (GLushort)(q + 2); I.qi[o++] = (GLushort)(q + 3); }
        glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, I.qi);
    } else glDrawArrays(I.mode == GL_POLYGON ? GL_TRIANGLE_FAN : I.mode == GL_QUAD_STRIP ? GL_TRIANGLE_STRIP : I.mode, 0, n);
}

/* ---- the rest -------------------------------------------------------------------------------------------------------- */
void gles_tex_parameteri(GLenum target, GLenum pname, GLint param)
{
    if (pname == GL_TEXTURE_MAX_LEVEL && !es3()) return;    /* ES 3 has it; on ES 2 the whole chain is used */
    if (param == GL_CLAMP) param = GL_CLAMP_TO_EDGE;
    glTexParameteri(target, pname, param);
}
void gles_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px)
{
    if (format != GL_RGB || type != GL_UNSIGNED_BYTE) { glReadPixels(x, y, w, h, format, type, px); return; }
    unsigned char *t = (unsigned char *)malloc((size_t)w * h * 4), *o = (unsigned char *)px; if (!t) return;
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, t);
    for (size_t i = 0; i < (size_t)w * h; i++) { o[i * 3] = t[i * 4]; o[i * 3 + 1] = t[i * 4 + 1]; o[i * 3 + 2] = t[i * 4 + 2]; }
    free(t);
}
/* GL_EXTENSIONS also names the desktop framebuffer object extensions: ES 2.0 has their entry points in its core (gles_proc
 * hands them out under either name), which is what gl_offscreen and the shader passes ask for */
const GLubyte *gles_get_string(GLenum name)
{
    static char *ext, glsl[160];
    if (name == GL_SHADING_LANGUAGE_VERSION) {             /* what gles_shader_source takes (below) */
        const char *v = (const char *)glGetString(name);
        snprintf(glsl, sizeof glsl, "%s (%s, through src/gles)", es3() ? "1.30" : "1.10", v ? v : "?");
        return (const GLubyte *)glsl;
    }
    if (name != GL_EXTENSIONS) return glGetString(name);
    if (!ext) {
        const char *e = (const char *)glGetString(GL_EXTENSIONS), *add = " GL_EXT_framebuffer_object GL_ARB_framebuffer_object";
        size_t n = e ? strlen(e) : 0;
        if (!(ext = (char *)malloc(n + strlen(add) + 1))) return (const GLubyte *)e;
        memcpy(ext, e ? e : "", n); strcpy(ext + n, add);
    }
    return (const GLubyte *)ext;
}

/* ---- the shader passes (own programs) ------------------------------------------------------------------------------- */
void gles_use_program(GLuint p) { g_user = p; glUseProgram(p ? p : g_prog); }

/* their desktop GLSL on ES: #version 120 as ES 1.00, #version 130 as ES 3.00 (on an ES 3 context), with default
 * precisions (highp: a lowp depth sampler bands), gl_Vertex as the attribute a_vertex (location 0, gles_link_program)
 * and, in 3.00, gl_FragColor as the output a_fragcolor; any other source as it is */
static int idch(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static char *swap_word(char *s, const char *from, const char *to)
{
    size_t nf = strlen(from), nt = strlen(to), n = 0;
    for (const char *p = s; (p = strstr(p, from)); p += nf) n++;
    char *o = (char *)malloc(strlen(s) + n * (nt > nf ? nt - nf : 0) + 1), *q = o;
    if (!o) return s;
    for (const char *p = s;;) {
        const char *h = strstr(p, from);
        if (!h) { strcpy(q, p); break; }
        int word = (h == s || !idch(h[-1])) && !idch(h[nf]);
        memcpy(q, p, (size_t)(h - p)); q += h - p;
        if (word) { memcpy(q, to, nt); q += nt; } else { memcpy(q, h, nf); q += nf; }
        p = h + nf;
    }
    free(s);
    return o;
}
static void APIENTRY gles_shader_source(GLuint sh, GLsizei n, const GLchar *const *src, const GLint *len)
{
    size_t total = 0;
    for (GLsizei i = 0; i < n; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
    char *s = (char *)malloc(total + 1), *q = s;
    if (!s) { glShaderSource(sh, n, src, len); return; }
    for (GLsizei i = 0; i < n; i++) { size_t k = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]); memcpy(q, src[i], k); q += k; }
    *q = 0;
    const char *p = s + strspn(s, " \t\r\n"), *eol = strchr(p, '\n');
    int v = 0;
    if (strncmp(p, "#version", 8) || sscanf(p + 8, "%d", &v) != 1 || (v != 120 && v != 130) || (v == 130 && !es3()) || !eol) {
        const GLchar *one = s; glShaderSource(sh, 1, &one, NULL); free(s); return;
    }
    GLint type = 0; glGetShaderiv(sh, GL_SHADER_TYPE, &type);
    char head[256];
    snprintf(head, sizeof head, "%s\nprecision highp float;\nprecision highp sampler2D;\n%s", v == 130 ? "#version 300 es" : "#version 100",
             type == GL_VERTEX_SHADER ? (v == 130 ? "in vec4 a_vertex;\n" : "attribute vec4 a_vertex;\n") : v == 130 ? "out vec4 a_fragcolor;\n" : "");
    char *body = (char *)malloc(strlen(head) + strlen(eol + 1) + 1);
    if (!body) { free(s); return; }
    strcpy(body, head); strcat(body, eol + 1); free(s);
    body = swap_word(body, "gl_Vertex", "a_vertex");
    if (v == 130) body = swap_word(body, "gl_FragColor", "a_fragcolor");
    const GLchar *one = body; glShaderSource(sh, 1, &one, NULL); free(body);
}
static void APIENTRY gles_link_program(GLuint p) { glBindAttribLocation(p, A_VERT, "a_vertex"); glLinkProgram(p); }

/* glPushAttrib / glPopAttrib: the state above and the GL state the renderer and the passes change; each group comes back
 * with its bit (the enables with GL_ENABLE_BIT) */
#define ADEPTH 8
typedef struct {
    unsigned char s[sizeof S]; GLbitfield mask; GLuint user;
    GLboolean dmask, cmask[4]; GLint dfunc, cull, front, tex[3]; GLfloat clear[4], poff[2];
} Attr;
static Attr g_attr[ADEPTH]; static int g_asp;
void gles_push_attrib(GLbitfield mask)
{
    if (g_asp == ADEPTH) { printf("gles: glPushAttrib stack full\n"); return; }
    Attr *a = &g_attr[g_asp++];
    memcpy(a->s, &S, sizeof S); a->mask = mask; a->user = g_user;
    if (mask & GL_DEPTH_BUFFER_BIT) { glGetBooleanv(GL_DEPTH_WRITEMASK, &a->dmask); glGetIntegerv(GL_DEPTH_FUNC, &a->dfunc); }
    if (mask & GL_COLOR_BUFFER_BIT) { glGetBooleanv(GL_COLOR_WRITEMASK, a->cmask); glGetFloatv(GL_COLOR_CLEAR_VALUE, a->clear); }
    if (mask & GL_POLYGON_BIT) {
        glGetIntegerv(GL_CULL_FACE_MODE, &a->cull); glGetIntegerv(GL_FRONT_FACE, &a->front);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &a->poff[0]); glGetFloatv(GL_POLYGON_OFFSET_UNITS, &a->poff[1]);
    }
    if (mask & GL_TEXTURE_BIT) {
        for (int u = 0; u < 3; u++) { glActiveTexture(GL_TEXTURE0 + u); glGetIntegerv(GL_TEXTURE_BINDING_2D, &a->tex[u]); }
        glActiveTexture(S.active);
    }
}
void gles_pop_attrib(void)
{
    if (!g_asp) return;
    Attr *a = &g_attr[--g_asp];
    GLbitfield m = a->mask;
    __typeof__(S) o; memcpy(&o, a->s, sizeof S);
    if (m & GL_ENABLE_BIT) {                               /* the caps back (one first touched meanwhile: its default) */
        for (int i = 0; i < S.ncaps; i++) {
            GLenum cap = S.caps[i].cap; GLboolean was = cap == GL_DITHER;
            for (int k = 0; k < o.ncaps; k++) if (o.caps[k].cap == cap) was = o.caps[k].on;
            if (S.caps[i].on != was && !ff_cap(cap)) { if (was) glEnable(cap); else glDisable(cap); }
        }
    } else { memcpy(o.tex, S.tex, sizeof o.tex); o.atest = S.atest; memcpy(o.caps, S.caps, sizeof o.caps); o.ncaps = S.ncaps; }
    /* the groups not asked for keep what they are now */
    if (!(m & GL_CURRENT_BIT)) { memcpy(o.col, S.col, sizeof o.col); memcpy(o.tc, S.tc, sizeof o.tc); memcpy(o.nrm, S.nrm, sizeof o.nrm); }
    if (!(m & GL_COLOR_BUFFER_BIT)) { o.afunc = S.afunc; o.aref = S.aref; memcpy(o.blend, S.blend, sizeof o.blend); }
    if (!(m & GL_LIGHTING_BIT)) { memcpy(o.light, S.light, sizeof o.light); memcpy(o.model_amb, S.model_amb, sizeof o.model_amb); o.cmat = S.cmat; }
    if (!(m & GL_FOG_BIT)) { o.fog_mode = S.fog_mode; memcpy(o.fog_col, S.fog_col, sizeof o.fog_col); o.fog_start = S.fog_start; o.fog_end = S.fog_end; o.fog_density = S.fog_density; }
    if (!(m & GL_TEXTURE_BIT)) { memcpy(o.env, S.env, sizeof o.env); memcpy(o.scale, S.scale, sizeof o.scale); memcpy(o.gen, S.gen, sizeof o.gen); o.active = S.active; }
    if (!(m & GL_VIEWPORT_BIT)) { memcpy(o.vp, S.vp, sizeof o.vp); o.have_vp = S.have_vp; }
    S = o;
    if ((m & GL_VIEWPORT_BIT) && S.have_vp) glViewport(S.vp[0], S.vp[1], S.vp[2], S.vp[3]);
    if (m & GL_COLOR_BUFFER_BIT) {
        glBlendFunc((GLenum)S.blend[0], (GLenum)S.blend[1]);
        glColorMask(a->cmask[0], a->cmask[1], a->cmask[2], a->cmask[3]); glClearColor(a->clear[0], a->clear[1], a->clear[2], a->clear[3]);
    }
    if (m & GL_DEPTH_BUFFER_BIT) { glDepthMask(a->dmask); glDepthFunc((GLenum)a->dfunc); }
    if (m & GL_POLYGON_BIT) { glCullFace((GLenum)a->cull); glFrontFace((GLenum)a->front); glPolygonOffset(a->poff[0], a->poff[1]); }
    if (m & GL_TEXTURE_BIT) for (int u = 0; u < 3; u++) { glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, (GLuint)a->tex[u]); }
    glActiveTexture(S.active);
    gles_use_program(a->user);
}

#ifndef GL_ES_VERSION_3_0                                  /* the two ES 3.0 calls the passes need (GLESv3 / Mesa's GLESv2) */
GL_APICALL void GL_APIENTRY glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
GL_APICALL void GL_APIENTRY glRenderbufferStorageMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
#endif
/* the entry points by name: the shim's own for what it emulates, ES 2.0's, and ES 3.0's on an ES 3 context; a desktop
 * name with EXT / ARB / OES at its end is the core one (ES 2.0 has the framebuffer objects in its core) */
void (*gles_proc(const char *name))(void)
{
    char base[64]; size_t n = strlen(name);
    if (n >= sizeof base) return NULL;
    memcpy(base, name, n + 1);
    if (n > 3 && (!strcmp(base + n - 3, "EXT") || !strcmp(base + n - 3, "ARB") || !strcmp(base + n - 3, "OES"))) base[n - 3] = 0;
    if (!strcmp(base, "glActiveTexture")) return (void (*)(void))gles_active_texture;
    if (!strcmp(base, "glMultiTexCoord2f")) return (void (*)(void))gles_multitexcoord2f;
    if (!strcmp(base, "glClientActiveTexture")) return (void (*)(void))gles_client_active_texture;
    if (!strcmp(base, "glUseProgram")) return (void (*)(void))gles_use_program;
    if (!strcmp(base, "glShaderSource")) return (void (*)(void))gles_shader_source;
    if (!strcmp(base, "glLinkProgram")) return (void (*)(void))gles_link_program;
    if (!strcmp(base, "glBindBuffer")) return (void (*)(void))gles_bind_buffer;
    if (!strcmp(base, "glBufferData")) return (void (*)(void))gles_buffer_data;
    if (!strcmp(base, "glDeleteBuffers")) return (void (*)(void))gles_delete_buffers;
    #define P(f) { #f, (void (*)(void))f }
    static const struct { const char *name; void (*f)(void); } k_es2[] = {
        P(glCreateShader), P(glCompileShader), P(glGetShaderiv), P(glGetShaderInfoLog), P(glDeleteShader),
        P(glCreateProgram), P(glAttachShader), P(glBindAttribLocation), P(glGetProgramiv), P(glGetProgramInfoLog),
        P(glDeleteProgram), P(glGetUniformLocation), P(glGetAttribLocation), P(glUniform1i), P(glUniform1f), P(glUniform2f),
        P(glUniform3f), P(glUniform4f), P(glUniform1fv), P(glUniform2fv), P(glUniform3fv), P(glUniform4fv), P(glUniform1iv),
        P(glUniformMatrix3fv), P(glUniformMatrix4fv), P(glVertexAttribPointer), P(glEnableVertexAttribArray),
        P(glDisableVertexAttribArray), P(glVertexAttrib4f), P(glGenBuffers), P(glDeleteBuffers), P(glBindBuffer), P(glBufferData),
        P(glGenFramebuffers), P(glDeleteFramebuffers), P(glBindFramebuffer), P(glFramebufferTexture2D), P(glFramebufferRenderbuffer),
        P(glCheckFramebufferStatus), P(glGenRenderbuffers), P(glDeleteRenderbuffers), P(glBindRenderbuffer), P(glRenderbufferStorage),
        P(glGenerateMipmap), P(glBlendFuncSeparate), P(glBlendEquation) };
    for (size_t i = 0; i < sizeof k_es2 / sizeof *k_es2; i++) if (!strcmp(base, k_es2[i].name)) return k_es2[i].f;
#ifndef __vita__                                           /* (vitaGL, ES 2.0 only, has no glRenderbufferStorageMultisample) */
    static const struct { const char *name; void (*f)(void); } k_es3[] = { P(glBlitFramebuffer), P(glRenderbufferStorageMultisample) };
    if (es3()) for (size_t i = 0; i < sizeof k_es3 / sizeof *k_es3; i++) if (!strcmp(base, k_es3[i].name)) return k_es3[i].f;
#endif
    #undef P
    return NULL;
}
