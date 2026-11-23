/* The presenter: layout, display modes, the OpenGL path and the CRT shader.
 *
 * The window is opened for OpenGL 3.3 core whenever it can be, else for
 * OpenGL ES 3.0: the picture is then drawn through a
 * pass-through shader or the CRT shader, one GLSL source for both with its
 * version header, and the filter is a texture parameter, so every option
 * switches while ZetRo runs. Only the GL entry points that needs are
 * resolved, through SDL, so no loader library is pulled in. The texture
 * holds the frame's bytes as RGBA, which on a little-endian host are the
 * ARGB pixels' B, G, R, A: the texture's swizzle (core in both APIs) swaps
 * red and blue back. When no GL context can be had, the SDL renderer takes
 * over with everything but the CRT shader.
 */

#include "video.h"
#include "png.h"
#include "shaders.h"

#include <SDL_opengl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif

/* --- the OpenGL entry points ---------------------------------------------- */

typedef void (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void (APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef GLint (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void (APIENTRY *PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClear)(GLbitfield);
typedef void (APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glPixelStorei)(GLenum, GLint);

static struct {
    PFN_glGenVertexArrays GenVertexArrays;
    PFN_glBindVertexArray BindVertexArray;
    PFN_glDeleteVertexArrays DeleteVertexArrays;
    PFN_glGenBuffers GenBuffers;
    PFN_glBindBuffer BindBuffer;
    PFN_glBufferData BufferData;
    PFN_glDeleteBuffers DeleteBuffers;
    PFN_glVertexAttribPointer VertexAttribPointer;
    PFN_glEnableVertexAttribArray EnableVertexAttribArray;
    PFN_glCreateShader CreateShader;
    PFN_glShaderSource ShaderSource;
    PFN_glCompileShader CompileShader;
    PFN_glGetShaderiv GetShaderiv;
    PFN_glGetShaderInfoLog GetShaderInfoLog;
    PFN_glDeleteShader DeleteShader;
    PFN_glCreateProgram CreateProgram;
    PFN_glAttachShader AttachShader;
    PFN_glLinkProgram LinkProgram;
    PFN_glGetProgramiv GetProgramiv;
    PFN_glGetProgramInfoLog GetProgramInfoLog;
    PFN_glUseProgram UseProgram;
    PFN_glDeleteProgram DeleteProgram;
    PFN_glGetUniformLocation GetUniformLocation;
    PFN_glUniform1i Uniform1i;
    PFN_glUniform1f Uniform1f;
    PFN_glUniform2f Uniform2f;
    PFN_glActiveTexture ActiveTexture;
    PFN_glGenTextures GenTextures;
    PFN_glDeleteTextures DeleteTextures;
    PFN_glBindTexture BindTexture;
    PFN_glTexParameteri TexParameteri;
    PFN_glTexImage2D TexImage2D;
    PFN_glTexSubImage2D TexSubImage2D;
    PFN_glViewport Viewport;
    PFN_glClearColor ClearColor;
    PFN_glClear Clear;
    PFN_glDrawArrays DrawArrays;
    PFN_glReadPixels ReadPixels;
    PFN_glPixelStorei PixelStorei;
} gl;

#define ZR_GL_LOAD(name)                                                  \
    do {                                                                  \
        gl.name = (PFN_gl##name)SDL_GL_GetProcAddress("gl" #name);        \
        if (!gl.name) return false;                                       \
    } while (0)

static bool load_gl(void) {
    ZR_GL_LOAD(GenVertexArrays);
    ZR_GL_LOAD(BindVertexArray);
    ZR_GL_LOAD(DeleteVertexArrays);
    ZR_GL_LOAD(GenBuffers);
    ZR_GL_LOAD(BindBuffer);
    ZR_GL_LOAD(BufferData);
    ZR_GL_LOAD(DeleteBuffers);
    ZR_GL_LOAD(VertexAttribPointer);
    ZR_GL_LOAD(EnableVertexAttribArray);
    ZR_GL_LOAD(CreateShader);
    ZR_GL_LOAD(ShaderSource);
    ZR_GL_LOAD(CompileShader);
    ZR_GL_LOAD(GetShaderiv);
    ZR_GL_LOAD(GetShaderInfoLog);
    ZR_GL_LOAD(DeleteShader);
    ZR_GL_LOAD(CreateProgram);
    ZR_GL_LOAD(AttachShader);
    ZR_GL_LOAD(LinkProgram);
    ZR_GL_LOAD(GetProgramiv);
    ZR_GL_LOAD(GetProgramInfoLog);
    ZR_GL_LOAD(UseProgram);
    ZR_GL_LOAD(DeleteProgram);
    ZR_GL_LOAD(GetUniformLocation);
    ZR_GL_LOAD(Uniform1i);
    ZR_GL_LOAD(Uniform1f);
    ZR_GL_LOAD(Uniform2f);
    ZR_GL_LOAD(ActiveTexture);
    ZR_GL_LOAD(GenTextures);
    ZR_GL_LOAD(DeleteTextures);
    ZR_GL_LOAD(BindTexture);
    ZR_GL_LOAD(TexParameteri);
    ZR_GL_LOAD(TexImage2D);
    ZR_GL_LOAD(TexSubImage2D);
    ZR_GL_LOAD(Viewport);
    ZR_GL_LOAD(ClearColor);
    ZR_GL_LOAD(Clear);
    ZR_GL_LOAD(DrawArrays);
    ZR_GL_LOAD(ReadPixels);
    ZR_GL_LOAD(PixelStorei);
    return true;
}

/* --- the presenter ---------------------------------------------------------- */

enum { BACKEND_GL33, BACKEND_GLES30, BACKEND_RENDERER };

struct zr_video {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *prescaled;     /* sharp without OpenGL: the picture at a whole scale */
    int pre_w, pre_h;
    int tex_w, tex_h;           /* what the texture currently holds */
    int tex_filter;             /* the scale mode the texture was given, -1 none */
    int backend;

    int screen_w, screen_h, aspect_w, aspect_h;   /* aspect_w 0: square pixels */
    int shape_w, shape_h;       /* the screen's shape on its monitor */
    int aspect_mode;            /* zr_aspect */
    int scale;
    bool vsync;
    bool fullscreen;
    int win_x, win_y, win_w, win_h;   /* the window's place before full screen */
    int filter;
    bool crt;                   /* the shader draws the picture */
    int look, phosphor;         /* what it draws: ZR_VIDEO_LOOK_*, ZR_VIDEO_PHOSPHOR_* */

    struct {
        SDL_GLContext context;
        GLuint plain, crt, vao, vbo, texture;
        bool ready;
    } gl;

    char shot_path[ZR_PATH_MAX];
    bool shot_pending, shot_raw;
    char shot_done[ZR_PATH_MAX];
    bool shot_result;
};

/* The picture's unit: the screen's width and the height it had on a
 * monitor of its shape (640x200 shown at 4:3 is 640x480 per step). */
static void unit(const zr_video *v, int *uw, int *uh) {
    *uw = v->screen_w;
    *uh = v->aspect_w > 0 ? v->screen_w * v->aspect_h / v->aspect_w : v->screen_h;
    if (*uh <= 0) *uh = v->screen_h;
}

/* Where the picture falls in an output of the given size. A whole scale N
 * gives N units, or the largest whole number that fits (at least 1); scale 0
 * takes the largest rectangle of the unit's shape that fits. */
static SDL_Rect layout(const zr_video *v, int out_w, int out_h) {
    int uw, uh;
    unit(v, &uw, &uh);
    SDL_Rect r;
    if (v->scale > 0) {
        int scale = v->scale, fits = out_w / uw < out_h / uh ? out_w / uw : out_h / uh;
        if (scale > fits) scale = fits > 1 ? fits : 1;
        r.w = uw * scale;
        r.h = uh * scale;
    } else if ((long long)out_w * uh <= (long long)out_h * uw) {
        r.w = out_w;
        r.h = (int)((long long)out_w * uh / uw);
    } else {
        r.h = out_h;
        r.w = (int)((long long)out_h * uw / uh);
    }
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

/* Nearest or linear for a picture `w` wide. A picture larger than the
 * screen is always smoothed, since it is mostly scaled down, where hard
 * sampling only aliases. Sharp reads a linear texture at shifted positions. */
static bool smooth(const zr_video *v, int w) {
    return w > v->screen_w || v->filter == ZR_FILTER_LINEAR || v->filter == ZR_FILTER_SHARP;
}
/* Sharp bilinear applies to the screen's picture in every look but the CRT,
 * whose preset does its own smoothing. */
static bool sharp(const zr_video *v, int w) {
    return v->filter == ZR_FILTER_SHARP && w <= v->screen_w && !(v->crt && v->look == ZR_VIDEO_LOOK_CRT);
}

static void save_pixels(zr_video *v, int w, int h, const void *pixels, int pitch, bool flip) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    v->shot_pending = false;
    v->shot_result = true;
    v->shot_done[0] = '\0';
    if (zr_png_name(v->shot_path)) {
        SDL_FreeSurface(s);
        if (zr_png_write(v->shot_path, pixels, w, h, (size_t)pitch, flip)) snprintf(v->shot_done, sizeof v->shot_done, "%s", v->shot_path);
        else SDL_Log("could not save %s", v->shot_path);
        return;
    }
    if (!s) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)pixels + (size_t)(flip ? h - 1 - y : y) * (size_t)pitch;
        memcpy((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch, src, (size_t)w * 4);
    }
    if (SDL_SaveBMP(s, v->shot_path) == 0) {
        snprintf(v->shot_done, sizeof v->shot_done, "%s", v->shot_path);
    } else {
        SDL_Log("could not save %s: %s", v->shot_path, SDL_GetError());
    }
    SDL_FreeSurface(s);
}

static GLuint compile(GLenum kind, const char *header, const char *body, char *log, int log_size) {
    GLint ok = 0;
    GLuint shader = gl.CreateShader(kind);
    if (!shader) {
        return 0;
    }
    const GLchar *source[2] = {header, body};
    gl.ShaderSource(shader, 2, source, NULL);
    gl.CompileShader(shader);
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (log && log_size > 0) {
            gl.GetShaderInfoLog(shader, log_size, NULL, log);
        }
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *header, const char *vertex, const char *fragment, char *log, int log_size) {
    GLint ok = 0;
    GLuint vs = compile(GL_VERTEX_SHADER, header, vertex, log, log_size);
    if (!vs) {
        return 0;
    }
    GLuint fs = compile(GL_FRAGMENT_SHADER, header, fragment, log, log_size);
    if (!fs) {
        gl.DeleteShader(vs);
        return 0;
    }
    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!ok) {
        if (log && log_size > 0) {
            gl.GetProgramInfoLog(program, log_size, NULL, log);
        }
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

/* A screen-filling quad: position then texture coordinate, with the picture's
 * first row at the top. */
static const GLfloat QUAD[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

static void gl_release(zr_video *v) {
    if (v->gl.ready) {
        if (v->gl.vbo) gl.DeleteBuffers(1, &v->gl.vbo);
        if (v->gl.vao) gl.DeleteVertexArrays(1, &v->gl.vao);
        if (v->gl.texture) gl.DeleteTextures(1, &v->gl.texture);
        if (v->gl.plain) gl.DeleteProgram(v->gl.plain);
        if (v->gl.crt) gl.DeleteProgram(v->gl.crt);
    }
    if (v->gl.context) SDL_GL_DeleteContext(v->gl.context);
    memset(&v->gl, 0, sizeof v->gl);
}

static bool gl_init(zr_video *v, bool es) {
    char log[512];
    const char *header = es ? ZR_GLSL_300ES : ZR_GLSL_330;
    const char *name = es ? "OpenGL ES 3.0" : "OpenGL 3.3 core";
    log[0] = '\0';
    v->gl.context = SDL_GL_CreateContext(v->window);
    if (!v->gl.context) {
        SDL_Log("no %s context (%s)", name, SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(v->window, v->gl.context);
    SDL_GL_SetSwapInterval(v->vsync ? 1 : 0);
    if (!load_gl()) {
        SDL_Log("this %s driver lacks entry points the presenter needs", name);
        return false;
    }
    v->gl.plain = link_program(header, ZR_PRESENT_VERTEX, ZR_NONE_FRAGMENT, log, (int)sizeof log);
    if (!v->gl.plain) {
        SDL_Log("the plain shader did not build on %s (%s)", name, log);
        return false;
    }
    v->gl.crt = link_program(header, ZR_PRESENT_VERTEX, ZR_CRT_FRAGMENT, log, (int)sizeof log);
    if (!v->gl.crt) SDL_Log("the CRT shader did not build on %s (%s); it is unavailable", name, log);
    gl.GenVertexArrays(1, &v->gl.vao);
    gl.BindVertexArray(v->gl.vao);
    gl.GenBuffers(1, &v->gl.vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, v->gl.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof QUAD, QUAD, GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat), (const void *)0);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat),
                           (const void *)(2 * sizeof(GLfloat)));
    gl.EnableVertexAttribArray(1);
    gl.GenTextures(1, &v->gl.texture);
    gl.BindTexture(GL_TEXTURE_2D, v->gl.texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* The frame's B, G, R, A bytes read as R, G, B, A: red and blue swapped back. */
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    v->gl.ready = true;
    v->backend = es ? BACKEND_GLES30 : BACKEND_GL33;
    return true;
}

/* The shader's parameters per look: the CRT is the original preset; plain
 * (used only for a phosphor colour) and the LCD turn every CRT effect off. */
static void gl_uniforms(const zr_video *v, GLuint program, const SDL_Rect *r, int w, int h) {
    const bool crt = v->look == ZR_VIDEO_LOOK_CRT;
    /* The shader's MONOCHROME_TYPE per phosphor: gray 1, green 2, amber 3,
     * white 4. */
    static const float types[] = {ZR_CRT_MONOCHROME_TYPE, 4.0f, 1.0f, 2.0f, 3.0f};
    const int phosphor = v->phosphor >= 0 && v->phosphor < 5 ? v->phosphor : 0;
    const struct {
        const char *name;
        float value;
    } p[] = {
        {"OVERSCAN_X", crt ? ZR_CRT_OVERSCAN_X : 1.0f}, {"OVERSCAN_Y", crt ? ZR_CRT_OVERSCAN_Y : 1.0f},
        {"CURVATURE_X", crt ? ZR_CRT_CURVATURE_X : 0.0f}, {"CURVATURE_Y", crt ? ZR_CRT_CURVATURE_Y : 0.0f},
        {"BLUR_STRENGTH", crt ? ZR_CRT_BLUR_STRENGTH : 0.0f},
        {"MONOCHROME_DISPLAY", phosphor ? 1.0f : ZR_CRT_MONOCHROME_DISPLAY},
        {"MONOCHROME_TYPE", types[phosphor]},
        {"MONOCHROME_COLORS", ZR_CRT_MONOCHROME_COLORS},
        {"CORNER_SIZE", crt ? ZR_CRT_CORNER_SIZE : 0.0f},
        {"SCANLINES_STRENGTH", ZR_CRT_SCANLINES_STRENGTH},
        {"LIGHT_STRENGTH", crt ? ZR_CRT_LIGHT_STRENGTH : 0.0f},
        {"GAMMA", crt ? ZR_CRT_GAMMA : 1.0f}, {"CONTRAST", ZR_CRT_CONTRAST},
        {"SATURATION", ZR_CRT_SATURATION}, {"BRIGHTNESS", ZR_CRT_BRIGHTNESS},
        {"CRT_MASK", crt ? ZR_CRT_MASK : 0.0f},
        {"PIXEL_GRID", v->look == ZR_VIDEO_LOOK_LCD ? ZR_LCD_PIXEL_GRID : 0.0f},
        {"SHARP", sharp(v, w) ? 1.0f : 0.0f},
    };
    GLint at = gl.GetUniformLocation(program, "uSource");
    if (at >= 0) gl.Uniform1i(at, 0);
    if ((at = gl.GetUniformLocation(program, "uOutputSize")) >= 0) gl.Uniform2f(at, (GLfloat)r->w, (GLfloat)r->h);
    if ((at = gl.GetUniformLocation(program, "uTextureSize")) >= 0) gl.Uniform2f(at, (GLfloat)w, (GLfloat)h);
    if ((at = gl.GetUniformLocation(program, "uInputSize")) >= 0) gl.Uniform2f(at, (GLfloat)w, (GLfloat)h);
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
        if ((at = gl.GetUniformLocation(program, p[i].name)) >= 0) gl.Uniform1f(at, p[i].value);
    }
}

static void gl_present(zr_video *v, const uint32_t *frame, int w, int h) {
    int out_w, out_h;
    SDL_GL_GetDrawableSize(v->window, &out_w, &out_h);
    const SDL_Rect r = layout(v, out_w, out_h);
    const bool use_crt = v->crt && v->gl.crt;

    gl.Viewport(0, 0, out_w, out_h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(r.x, out_h - r.y - r.h, r.w, r.h);
    const GLuint program = use_crt ? v->gl.crt : v->gl.plain;
    gl.UseProgram(program);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, v->gl.texture);
    if (w != v->tex_w || h != v->tex_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        v->tex_w = w;
        v->tex_h = h;
    }
    /* The CRT shader samples smoothly whatever the filter says: that is the
     * one setting of its own preset. The other looks keep the filter. */
    const int f = (use_crt && v->look == ZR_VIDEO_LOOK_CRT) || smooth(v, w) ? GL_LINEAR : GL_NEAREST;
    if (f != v->tex_filter) {
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        v->tex_filter = f;
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame);
    gl_uniforms(v, program, &r, w, h);
    gl.BindVertexArray(v->gl.vao);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (v->shot_pending && !v->shot_raw) {
        /* RGBA bytes (the one format both APIs read back) to ARGB pixels. */
        uint8_t *rgba = malloc((size_t)out_w * (size_t)out_h * 4u);
        uint32_t *px = malloc((size_t)out_w * (size_t)out_h * sizeof *px);
        if (rgba && px) {
            gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.ReadPixels(0, 0, out_w, out_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            for (size_t i = 0; i < (size_t)out_w * (size_t)out_h; i++) {
                const uint8_t *p = rgba + 4 * i;
                px[i] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
            save_pixels(v, out_w, out_h, px, out_w * 4, true);
        }
        free(rgba);
        free(px);
    }
    SDL_GL_SwapWindow(v->window);
}

static bool renderer_init(zr_video *v) {
    Uint32 flags = SDL_RENDERER_ACCELERATED;
    if (v->vsync) flags |= SDL_RENDERER_PRESENTVSYNC;
    v->renderer = SDL_CreateRenderer(v->window, -1, flags);
    if (!v->renderer) v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_SOFTWARE);
    v->backend = BACKEND_RENDERER;
    return v->renderer != NULL;
}

static void renderer_present(zr_video *v, const uint32_t *frame, int w, int h) {
    int out_w, out_h;
    SDL_GetRendererOutputSize(v->renderer, &out_w, &out_h);
    const SDL_Rect r = layout(v, out_w, out_h);
    if (!v->texture || w != v->tex_w || h != v->tex_h) {
        if (v->texture) SDL_DestroyTexture(v->texture);
        v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!v->texture) return;
        v->tex_w = w;
        v->tex_h = h;
        v->tex_filter = -1;
    }
    /* Sharp: the picture drawn nearest at the largest whole scale that fits,
     * then that scaled linearly over the rest (none needed at a whole scale;
     * below 1x, plain linear). */
    const int whole = r.w / w < r.h / h ? r.w / w : r.h / h;
    bool prescale = sharp(v, w) && whole >= 1 && (r.w != whole * w || r.h != whole * h) &&
                    SDL_RenderTargetSupported(v->renderer);
    if (prescale && (!v->prescaled || v->pre_w != whole * w || v->pre_h != whole * h)) {
        if (v->prescaled) SDL_DestroyTexture(v->prescaled);
        v->prescaled = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, whole * w,
                                         whole * h);
        v->pre_w = whole * w;
        v->pre_h = whole * h;
        if (v->prescaled) SDL_SetTextureScaleMode(v->prescaled, SDL_ScaleModeLinear);
        else prescale = false;
    }
    const int f = sharp(v, w) ? (whole < 1 || (!prescale && (r.w != whole * w || r.h != whole * h))) : smooth(v, w);
    if (f != v->tex_filter) {
        SDL_SetTextureScaleMode(v->texture, f ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        v->tex_filter = f;
    }
    SDL_UpdateTexture(v->texture, NULL, frame, w * (int)sizeof *frame);
    if (prescale) {
        SDL_SetRenderTarget(v->renderer, v->prescaled);
        SDL_RenderCopy(v->renderer, v->texture, NULL, NULL);
        SDL_SetRenderTarget(v->renderer, NULL);
    }
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    SDL_RenderCopy(v->renderer, prescale ? v->prescaled : v->texture, NULL, &r);
    if (v->shot_pending && !v->shot_raw) {
        uint32_t *px = malloc((size_t)out_w * (size_t)out_h * sizeof *px);
        if (px && SDL_RenderReadPixels(v->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, px, out_w * 4) == 0) {
            save_pixels(v, out_w, out_h, px, out_w * 4, false);
        }
        free(px);
    }
    SDL_RenderPresent(v->renderer);
}

/* A window with a GL context of the kind asked; false (and nothing left
 * open) when the driver offers none. */
static bool open_gl(zr_video *v, const char *title, int w, int h, Uint32 flags, bool es) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 0 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    v->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags | SDL_WINDOW_OPENGL);
    if (v->window && gl_init(v, es)) return true;
    gl_release(v);
    if (v->window) SDL_DestroyWindow(v->window);
    v->window = NULL;
    return false;
}

zr_video *zr_video_open(const char *title, const zr_settings *s, int screen_w, int screen_h, int aspect_w,
                        int aspect_h, bool hidden, char *err, size_t errlen) {
    zr_video *v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v->screen_w = screen_w;
    v->screen_h = screen_h;
    v->shape_w = aspect_w;
    v->shape_h = aspect_h;
    v->aspect_mode = s->aspect;
    v->aspect_w = s->aspect == ZR_ASPECT_SQUARE ? 0 : aspect_w;
    v->aspect_h = aspect_h;
    v->scale = s->integer_scale;
    v->vsync = s->vsync;
    v->filter = s->filter;
    v->crt = s->crt;
    v->look = s->crt ? ZR_VIDEO_LOOK_CRT : ZR_VIDEO_LOOK_PLAIN;
    v->tex_filter = -1;

    int uw, uh;
    unit(v, &uw, &uh);
    const int scale = s->window_scale > 0 ? s->window_scale : 2;
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (hidden) flags |= SDL_WINDOW_HIDDEN;
    const bool fullscreen = !hidden && s->fullscreen;   /* set once the window is made (zr_video_set_fullscreen) */

    /* OpenGL 3.3 core, then OpenGL ES 3.0, then the SDL renderer. */
    const char *start = SDL_getenv("ZETRO_VIDEO");
    const int first = start && !strcmp(start, "renderer") ? BACKEND_RENDERER : start && !strcmp(start, "gles") ? BACKEND_GLES30 : BACKEND_GL33;
    if ((first <= BACKEND_GL33 && open_gl(v, title, uw * scale, uh * scale, flags, false)) ||
        (first <= BACKEND_GLES30 && open_gl(v, title, uw * scale, uh * scale, flags, true))) {
        if (fullscreen) zr_video_set_fullscreen(v, true);
        return v;
    }
    /* No OpenGL: the SDL renderer, everything but the CRT shader. */
    v->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, uw * scale, uh * scale, flags);
    if (!v->window) {
        snprintf(err, errlen, "could not create a window: %s", SDL_GetError());
        free(v);
        return NULL;
    }
    if (!renderer_init(v)) {
        snprintf(err, errlen, "could not create a renderer: %s", SDL_GetError());
        SDL_DestroyWindow(v->window);
        free(v);
        return NULL;
    }
    if (fullscreen) zr_video_set_fullscreen(v, true);
    return v;
}

void zr_video_close(zr_video *v) {
    if (!v) return;
    gl_release(v);
    if (v->prescaled) SDL_DestroyTexture(v->prescaled);
    if (v->texture) SDL_DestroyTexture(v->texture);
    if (v->renderer) SDL_DestroyRenderer(v->renderer);
    if (v->window) SDL_DestroyWindow(v->window);
    free(v);
}

const char *zr_video_backend(const zr_video *v) {
    return v->backend == BACKEND_GL33 ? "OpenGL 3.3 core" : v->backend == BACKEND_GLES30 ? "OpenGL ES 3.0" : "SDL renderer";
}

void zr_video_present(zr_video *v, const uint32_t *frame, int w, int h) {
    if (v->shot_pending && v->shot_raw) save_pixels(v, w, h, frame, w * 4, false);
    if (v->gl.ready) gl_present(v, frame, w, h);
    else renderer_present(v, frame, w, h);
}

void zr_video_set_geometry(zr_video *v, int w, int h, int aspect_w, int aspect_h) {
    if (w <= 0 || h <= 0) return;
    if (aspect_w <= 0 || aspect_h <= 0) { aspect_w = w; aspect_h = h; }
    if (w == v->screen_w && h == v->screen_h && aspect_w == v->shape_w && aspect_h == v->shape_h) return;
    v->screen_w = w;
    v->screen_h = h;
    v->shape_w = aspect_w;
    v->shape_h = aspect_h;
    v->aspect_w = v->aspect_mode == ZR_ASPECT_SQUARE ? 0 : aspect_w;
    v->aspect_h = aspect_h;
    v->tex_filter = -1;
}

/* Full screen is a borderless window over the display it is on (windowed
 * full screen: other windows, notifications and Alt+Tab work as on the
 * desktop, no mode change). It is one pixel taller than the display: a
 * window that covers a display exactly is taken over by the graphics
 * driver as exclusive full screen when it presents through OpenGL. */
void zr_video_set_icon(zr_video *v, const uint32_t *argb, int size) {
    SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom((void *)(uintptr_t)argb, size, size, 32, size * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!icon) return;
    SDL_SetWindowIcon(v->window, icon);
    SDL_FreeSurface(icon);
}

void zr_video_set_fullscreen(zr_video *v, bool on) {
    if (on == v->fullscreen) return;
    if (on) {
        SDL_GetWindowPosition(v->window, &v->win_x, &v->win_y);
        SDL_GetWindowSize(v->window, &v->win_w, &v->win_h);
        int d = SDL_GetWindowDisplayIndex(v->window);
        SDL_Rect b;
        if (d < 0 || SDL_GetDisplayBounds(d, &b) != 0) return;
        SDL_SetWindowResizable(v->window, SDL_FALSE);
        SDL_SetWindowBordered(v->window, SDL_FALSE);
        SDL_SetWindowPosition(v->window, b.x, b.y);
        SDL_SetWindowSize(v->window, b.w, b.h + 1);
        SDL_RaiseWindow(v->window);
    } else {
        SDL_SetWindowBordered(v->window, SDL_TRUE);
        SDL_SetWindowResizable(v->window, SDL_TRUE);
        SDL_SetWindowSize(v->window, v->win_w, v->win_h);
        SDL_SetWindowPosition(v->window, v->win_x, v->win_y);
    }
    v->fullscreen = on;
}

bool zr_video_fullscreen(const zr_video *v) { return v->fullscreen; }

void zr_video_set_filter(zr_video *v, int filter) {
    v->filter = filter;
    v->tex_filter = -1;
}

void zr_video_set_integer_scale(zr_video *v, int scale) { v->scale = scale; v->tex_filter = -1; }

void zr_video_set_aspect(zr_video *v, int aspect) {
    v->aspect_mode = aspect;
    v->aspect_w = aspect == ZR_ASPECT_SQUARE ? 0 : v->shape_w;
    v->aspect_h = v->shape_h;
    v->tex_filter = -1;
}

void zr_video_set_vsync(zr_video *v, bool on) {
    v->vsync = on;
    if (v->gl.ready) SDL_GL_SetSwapInterval(on ? 1 : 0);
    else if (v->renderer) SDL_RenderSetVSync(v->renderer, on ? 1 : 0);
}

bool zr_video_set_look(zr_video *v, int look, int phosphor) {
    const bool shader = look != ZR_VIDEO_LOOK_PLAIN || phosphor != ZR_VIDEO_PHOSPHOR_COLOR;
    if (shader && !zr_video_crt_available(v)) return false;
    v->crt = shader;
    v->look = look;
    v->phosphor = phosphor;
    v->tex_filter = -1;
    return true;
}

bool zr_video_crt_available(const zr_video *v) { return v->gl.ready && v->gl.crt; }

bool zr_video_to_screen(const zr_video *v, int wx, int wy, int *gx, int *gy) {
    /* Mouse events come in window points; the layout is computed in the
     * same units so a high-DPI drawable does not skew the mapping. */
    int ww, wh;
    SDL_GetWindowSize(v->window, &ww, &wh);
    const SDL_Rect r = layout(v, ww, wh);
    if (r.w <= 0 || r.h <= 0) return false;
    int x = (int)((long long)(wx - r.x) * v->screen_w / r.w);
    int y = (int)((long long)(wy - r.y) * v->screen_h / r.h);
    const bool inside = wx >= r.x && wy >= r.y && x < v->screen_w && y < v->screen_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= v->screen_w) x = v->screen_w - 1;
    if (y >= v->screen_h) y = v->screen_h - 1;
    *gx = x;
    *gy = y;
    return inside;
}

void zr_video_request_shot(zr_video *v, const char *path, bool raw) {
    snprintf(v->shot_path, sizeof v->shot_path, "%s", path);
    v->shot_pending = true;
    v->shot_raw = raw;
}

bool zr_video_take_shot_result(zr_video *v, char *path, size_t len) {
    if (!v->shot_result) return false;
    snprintf(path, len, "%s", v->shot_done);
    v->shot_result = false;
    return true;
}
