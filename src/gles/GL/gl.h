/* GL/gl.h for OpenGL ES 2.0 / 3.0 (port extra, WoodyRE's src/gles shim; `GLES=1 ./build.sh` puts src/gles first on
 * the include path, so every #include <GL/gl.h> of the viewer lands here). The renderer draws with the fixed-function
 * pipeline of desktop OpenGL 1.1; src/gles/gles2.c emulates the part of it the viewer uses with one shader pair:
 * - the projection, modelview and texture matrix stacks (load, multiply, translate, scale, ortho, push / pop, glGetFloatv)
 * - vertex colours and the current colour, two texture units with GL_MODULATE / GL_REPLACE / GL_ADD / GL_DECAL and
 *   GL_COMBINE (modulate, GL_RGB_SCALE 1 or 2), the alpha test
 * - lighting: up to four directional lights (GL_POSITION under the modelview of the call, GL_DIFFUSE, GL_AMBIENT), the
 *   light model's ambient, GL_COLOR_MATERIAL (ambient and / or diffuse), GL_NORMALIZE, per vertex as GL does
 * - fog (GL_LINEAR, GL_EXP, GL_EXP2 at the eye-plane distance), sphere-map texture coordinates (GL_TEXTURE_GEN_S / T)
 * - client arrays (vertex, normal, colour, texcoords of both units) as vertex attributes, glBegin / glEnd as client
 *   arrays (GL_QUADS as triangles, GL_POLYGON as a fan), glPushClientAttrib / glPopClientAttrib
 * - glPushAttrib / glPopAttrib of the state above and of the GL state the renderer and the shader passes change
 * - buffer objects under the client arrays (glBindBuffer / glBufferData / glDeleteBuffers through plat_gl_proc: an
 *   array takes the buffer bound at its pointer call, as in GL; on a bare ES 2.0 the shim keeps a copy of each)
 * - glReadPixels into GL_RGB, GL_CLAMP (= CLAMP_TO_EDGE), glDrawElements with GL_UNSIGNED_INT on a bare ES 2.0
 * Not there: point lights, spot lights, specular, glMaterial, the other texgen modes, glPolygonMode (a no-op), GL_FLAT.
 * Shader passes with an own program (glUseProgram of it: glBegin then feeds its attribute 0 the positions) pass
 * straight through; plat_gl_proc hands out the ES 2.0 / 3.0 entry points they ask for (gles_proc), and the shim's own
 * program comes back at the next draw after glUseProgram(0). Their desktop GLSL is taken as it is written for GL:
 * glShaderSource turns #version 120 into ES 1.00 and #version 130 into ES 3.00 (highp, gl_Vertex = attribute 0,
 * gl_FragColor an own output), and GL_SHADING_LANGUAGE_VERSION answers 1.30 on ES 3 (1.10 on ES 2: no passes). */
#ifndef RAMPAGE_GLES_GL_H
#define RAMPAGE_GLES_GL_H
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define RAMPAGE_GLES 1                         /* the viewer draws through this shim */

typedef double GLdouble;
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
/* the desktop names the viewer uses */
#define GL_QUADS                 0x0007
#define GL_QUAD_STRIP            0x0008
#define GL_POLYGON               0x0009
#define GL_CLAMP                 0x2900
#define GL_LINE                  0x1B01
#define GL_FILL                  0x1B02
#define GL_SMOOTH                0x1D01
#define GL_FLAT                  0x1D00
#define GL_MODELVIEW             0x1700
#define GL_PROJECTION            0x1701
#define GL_TEXTURE               0x1702
#define GL_MODELVIEW_MATRIX      0x0BA6
#define GL_PROJECTION_MATRIX     0x0BA7
#define GL_TEXTURE_MATRIX        0x0BA8
#define GL_CURRENT_COLOR         0x0B00
#define GL_VERTEX_ARRAY          0x8074
#define GL_NORMAL_ARRAY          0x8075
#define GL_COLOR_ARRAY           0x8076
#define GL_TEXTURE_COORD_ARRAY   0x8078
#define GL_ALPHA_TEST            0x0BC0
#define GL_LIGHTING              0x0B50
#define GL_LIGHT0                0x4000
#define GL_LIGHT1                0x4001
#define GL_LIGHT2                0x4002
#define GL_LIGHT3                0x4003
#define GL_AMBIENT               0x1200
#define GL_DIFFUSE               0x1201
#define GL_SPECULAR              0x1202
#define GL_POSITION              0x1203
#define GL_AMBIENT_AND_DIFFUSE   0x1602
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE  0x0B52
#define GL_LIGHT_MODEL_AMBIENT   0x0B53
#define GL_COLOR_MATERIAL        0x0B57
#define GL_NORMALIZE             0x0BA1
#define GL_RESCALE_NORMAL        0x803A
#define GL_FOG                   0x0B60
#define GL_FOG_DENSITY           0x0B62
#define GL_FOG_START             0x0B63
#define GL_FOG_END               0x0B64
#define GL_FOG_MODE              0x0B65
#define GL_FOG_COLOR             0x0B66
#define GL_EXP                   0x0800
#define GL_EXP2                  0x0801
#define GL_S                     0x2000
#define GL_T                     0x2001
#define GL_TEXTURE_GEN_MODE      0x2500
#define GL_SPHERE_MAP            0x2402
#define GL_TEXTURE_GEN_S         0x0C60
#define GL_TEXTURE_GEN_T         0x0C61
#define GL_LINE_SMOOTH           0x0B20
#define GL_POINT_SMOOTH          0x0B10
#define GL_POLYGON_SMOOTH        0x0B41
#define GL_MULTISAMPLE           0x809D
#define GL_TEXTURE_ENV           0x2300
#define GL_TEXTURE_ENV_MODE      0x2200
#define GL_MODULATE              0x2100
#define GL_DECAL                 0x2101
#define GL_ADD                   0x0104
#define GL_COMBINE               0x8570
#define GL_COMBINE_RGB           0x8571
#define GL_COMBINE_ALPHA         0x8572
#define GL_RGB_SCALE             0x8573
#define GL_PRIMARY_COLOR         0x8577
#define GL_PREVIOUS              0x8578
#define GL_SRC0_RGB              0x8580
#define GL_SRC1_RGB              0x8581
#define GL_SRC0_ALPHA            0x8588
#define GL_SRC1_ALPHA            0x8589
#define GL_BLEND_SRC             0x0BE1
#define GL_BLEND_DST             0x0BE0
#define GL_RGBA8                 0x8058
#define GL_DEPTH24_STENCIL8      0x88F0
#define GL_DEPTH_STENCIL         0x84F9           /* (ES 3.0's names) */
#define GL_UNSIGNED_INT_24_8     0x84FA
#define GL_TEXTURE_MAX_LEVEL     0x813D
#define GL_CURRENT_BIT           0x00000001
#define GL_POLYGON_BIT           0x00000008
#define GL_LIGHTING_BIT          0x00000040
#define GL_FOG_BIT               0x00000080
#define GL_VIEWPORT_BIT          0x00000800
#define GL_TRANSFORM_BIT         0x00001000
#define GL_ENABLE_BIT            0x00002000
#define GL_TEXTURE_BIT           0x00040000
#define GL_ALL_ATTRIB_BITS       0xFFFFFFFF
#define GL_CLIENT_VERTEX_ARRAY_BIT 0x00000002
#define GL_CLIENT_ALL_ATTRIB_BITS  0xFFFFFFFF

void gles_begin(GLenum mode);
void gles_end(void);
void gles_vertex3f(GLfloat x, GLfloat y, GLfloat z);
void gles_texcoord2f(GLfloat s, GLfloat t);
void gles_multitexcoord2f(GLenum unit, GLfloat s, GLfloat t);
void gles_color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gles_normal3f(GLfloat x, GLfloat y, GLfloat z);
void gles_tex_parameteri(GLenum target, GLenum pname, GLint param);
void gles_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px);
void gles_draw_arrays(GLenum mode, GLint first, GLsizei count);
void gles_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *idx);
void gles_enable_client_state(GLenum array);
void gles_disable_client_state(GLenum array);
void gles_client_active_texture(GLenum unit);
void gles_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_normal_pointer(GLenum type, GLsizei stride, const void *p);
void gles_color_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_tex_coord_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_push_client_attrib(GLbitfield mask);
void gles_pop_client_attrib(void);
void gles_enable(GLenum cap);
void gles_disable(GLenum cap);
GLboolean gles_is_enabled(GLenum cap);
void gles_active_texture(GLenum unit);
void gles_viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void gles_blend_func(GLenum src, GLenum dst);
void gles_get_integerv(GLenum pname, GLint *v);
void gles_get_floatv(GLenum pname, GLfloat *v);
const GLubyte *gles_get_string(GLenum name);
void gles_matrix_mode(GLenum mode);
void gles_load_identity(void);
void gles_load_matrixf(const GLfloat *m);
void gles_mult_matrixf(const GLfloat *m);
void gles_translatef(GLfloat x, GLfloat y, GLfloat z);
void gles_scalef(GLfloat x, GLfloat y, GLfloat z);
void gles_ortho(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void gles_push_matrix(void);
void gles_pop_matrix(void);
void gles_tex_envi(GLenum target, GLenum pname, GLint v);
void gles_tex_envf(GLenum target, GLenum pname, GLfloat v);
void gles_get_tex_enviv(GLenum target, GLenum pname, GLint *v);
void gles_tex_geni(GLenum coord, GLenum pname, GLint param);
void gles_alpha_func(GLenum func, GLfloat ref);
void gles_lightfv(GLenum light, GLenum pname, const GLfloat *v);
void gles_light_modelfv(GLenum pname, const GLfloat *v);
void gles_light_modeli(GLenum pname, GLint v);
void gles_color_material(GLenum face, GLenum mode);
void gles_fogf(GLenum pname, GLfloat v);
void gles_fogi(GLenum pname, GLint v);
void gles_fogfv(GLenum pname, const GLfloat *v);
void gles_use_program(GLuint p);
void gles_push_attrib(GLbitfield mask);
void gles_pop_attrib(void);
void (*gles_proc(const char *name))(void);   /* plat_gl_proc: the entry points the viewer asks for by name */

#ifndef RAMPAGE_GLES_IMPL
#define glBegin                   gles_begin
#define glEnd                     gles_end
#define glVertex2f(x, y)          gles_vertex3f((x), (y), 0.0f)
#define glVertex3f                gles_vertex3f
#define glVertex3fv(v)            gles_vertex3f((v)[0], (v)[1], (v)[2])
#define glTexCoord2f              gles_texcoord2f
#define glTexCoord2fv(v)          gles_texcoord2f((v)[0], (v)[1])
#define glNormal3f                gles_normal3f
#define glColor3f(r, g, b)        gles_color4f((r), (g), (b), 1.0f)
#define glColor4f                 gles_color4f
#define glColor4fv(v)             gles_color4f((v)[0], (v)[1], (v)[2], (v)[3])
#define glOrtho(l, r, b, t, n, f) gles_ortho((GLfloat)(l), (GLfloat)(r), (GLfloat)(b), (GLfloat)(t), (GLfloat)(n), (GLfloat)(f))
#define glPolygonMode(face, mode) ((void)(face), (void)(mode))
#define glShadeModel(mode)        ((void)(mode))          /* ES is always smooth */
#define glTexParameteri           gles_tex_parameteri
#define glReadPixels              gles_read_pixels
#define glDrawArrays              gles_draw_arrays
#define glDrawElements            gles_draw_elements
#define glEnableClientState       gles_enable_client_state
#define glDisableClientState      gles_disable_client_state
#define glClientActiveTexture     gles_client_active_texture
#define glVertexPointer           gles_vertex_pointer
#define glNormalPointer           gles_normal_pointer
#define glColorPointer            gles_color_pointer
#define glTexCoordPointer         gles_tex_coord_pointer
#define glPushClientAttrib        gles_push_client_attrib
#define glPopClientAttrib         gles_pop_client_attrib
#define glEnable                  gles_enable
#define glDisable                 gles_disable
#define glIsEnabled               gles_is_enabled
#define glActiveTexture           gles_active_texture
#define glViewport                gles_viewport
#define glBlendFunc               gles_blend_func
#define glGetIntegerv             gles_get_integerv
#define glGetFloatv               gles_get_floatv
#define glGetString               gles_get_string
#define glMatrixMode              gles_matrix_mode
#define glLoadIdentity            gles_load_identity
#define glLoadMatrixf             gles_load_matrixf
#define glMultMatrixf             gles_mult_matrixf
#define glTranslatef              gles_translatef
#define glScalef                  gles_scalef
#define glPushMatrix              gles_push_matrix
#define glPopMatrix               gles_pop_matrix
#define glTexEnvi                 gles_tex_envi
#define glTexEnvf                 gles_tex_envf
#define glGetTexEnviv             gles_get_tex_enviv
#define glTexGeni                 gles_tex_geni
#define glAlphaFunc               gles_alpha_func
#define glLightfv                 gles_lightfv
#define glLightModelfv            gles_light_modelfv
#define glLightModeli             gles_light_modeli
#define glColorMaterial           gles_color_material
#define glFogf                    gles_fogf
#define glFogi                    gles_fogi
#define glFogfv                   gles_fogfv
#define glUseProgram              gles_use_program
#define glPushAttrib              gles_push_attrib
#define glPopAttrib               gles_pop_attrib
#endif
#endif
