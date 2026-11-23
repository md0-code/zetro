/* The presentation shaders.
 *
 * One source for OpenGL 3.3 core and OpenGL ES 3.0: each shader is its
 * body below after one of the version headers (ZR_GLSL_330, ZR_GLSL_300ES),
 * which the presenter picks by the context it got.
 *
 * Ordinary front-end code: nothing here is reverse engineered and nothing
 * here may disturb what is. The core still produces the same picture; these
 * shaders only decide how it is put on the screen.
 *
 * `ZR_CRT_FRAGMENT` is the CRT emulation shader, written for libretro and
 * taken unchanged apart from the uniform names. Its chain is
 * overscan, curvature, a 3x3 gaussian, the optional monochrome quantisation,
 * rounded corners, vignette, scanlines and the aperture stripe, then gamma,
 * contrast, saturation and brightness. The parameter defaults below are the
 * `#pragma parameter` defaults of the original. What is dropped is only
 * libretro plumbing with no meaning here: the MVP matrix (the quad is already
 * in clip space) and the version and GL_ES compatibility ladders.
 *
 * Four things are added, each neutral at its default: MONOCHROME_TYPE 4,
 * a paper-white phosphor; CRT_MASK, which at 0 turns the scanlines and the
 * aperture stripe off; PIXEL_GRID, the darkening of each source pixel's
 * last output row and column when it covers two or more (an LCD's grid);
 * and SHARP, the sharp-bilinear tap (ZR_SHARP_TAP) for the picture's one
 * texture read (the blur's reads keep their own). */
#ifndef ZR_SHADERS_H
#define ZR_SHADERS_H

/* The version headers: desktop GLSL 3.30, or GLSL ES 3.00 with the default
 * precisions a fragment shader there must state. */
#define ZR_GLSL_330 "#version 330 core\n"
#define ZR_GLSL_300ES "#version 300 es\nprecision highp float;\nprecision mediump int;\n"

/* A quad already in clip space, with its texture coordinates passed through. */
#define ZR_PRESENT_VERTEX \
    "layout(location = 0) in vec2 aPosition;\n" \
    "layout(location = 1) in vec2 aTexCoord;\n" \
    "out vec2 vTexCoord;\n" \
    "void main() {\n" \
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n" \
    "    vTexCoord = aTexCoord;\n" \
    "}\n"

/* Sharp bilinear (libretro's sharp-bilinear-simple), read through a linear
 * texture: each source pixel is drawn flat over the whole multiple of it that
 * fits, and blended only across the leftover fraction of an output pixel at
 * its edges. At a whole scale it equals nearest; below 1x it is linear.
 * Needs uTextureSize and uOutputSize (the picture's rectangle). */
#define ZR_SHARP_TAP \
    "uniform float SHARP;\n" \
    "vec2 sharpTap(vec2 uv)\n" \
    "{\n" \
    "    if (SHARP <= 0.0) return uv;\n" \
    "    vec2 texel = uv * uTextureSize;\n" \
    "    vec2 scale = max(floor(uOutputSize / uTextureSize), vec2(1.0));\n" \
    "    vec2 center = fract(texel) - 0.5;\n" \
    "    vec2 range = 0.5 - 0.5 / scale;\n" \
    "    vec2 f = (center - clamp(center, -range, range)) * scale + 0.5;\n" \
    "    return (floor(texel) + f) / uTextureSize;\n" \
    "}\n"

/* The pass-through: the picture as it is, at whatever filtering the texture
 * carries, or sharp bilinear. */
#define ZR_NONE_FRAGMENT \
    "uniform sampler2D uSource;\n" \
    "uniform vec2 uOutputSize;\n" \
    "uniform vec2 uTextureSize;\n" \
    ZR_SHARP_TAP \
    "in vec2 vTexCoord;\n" \
    "out vec4 FragColor;\n" \
    "void main() {\n" \
    "    FragColor = texture(uSource, sharpTap(vTexCoord));\n" \
    "}\n"

#define ZR_CRT_FRAGMENT \
    "uniform sampler2D uSource;\n" \
    "uniform vec2 uOutputSize;\n" \
    "uniform vec2 uTextureSize;\n" \
    "uniform vec2 uInputSize;\n" \
    "uniform float OVERSCAN_X;\n" \
    "uniform float OVERSCAN_Y;\n" \
    "uniform float CURVATURE_X;\n" \
    "uniform float CURVATURE_Y;\n" \
    "uniform float BLUR_STRENGTH;\n" \
    "uniform float MONOCHROME_DISPLAY;\n" \
    "uniform float MONOCHROME_TYPE;\n" \
    "uniform float MONOCHROME_COLORS;\n" \
    "uniform float CORNER_SIZE;\n" \
    "uniform float SCANLINES_STRENGTH;\n" \
    "uniform float LIGHT_STRENGTH;\n" \
    "uniform float GAMMA;\n" \
    "uniform float CONTRAST;\n" \
    "uniform float SATURATION;\n" \
    "uniform float BRIGHTNESS;\n" \
    "uniform float CRT_MASK;\n" \
    "uniform float PIXEL_GRID;\n" \
    ZR_SHARP_TAP \
    "in vec2 vTexCoord;\n" \
    "out vec4 FragColor;\n" \
    "vec3 postEffects(in vec3 rgb)\n" \
    "{\n" \
    "    rgb = pow(rgb, vec3(GAMMA));\n" \
    "    rgb = mix(vec3(.5), mix(vec3(dot(vec3(.2125, .7154, .0721), rgb * BRIGHTNESS)), rgb * BRIGHTNESS, SATURATION), CONTRAST);\n" \
    "    return rgb;\n" \
    "}\n" \
    "vec3 gaussian(in vec2 uv)\n" \
    "{\n" \
    "    float b = BLUR_STRENGTH / (uOutputSize.x / uOutputSize.y);\n" \
    "    vec3 col = texture(uSource, vec2(uv.x - b / uOutputSize.x, uv.y - b / uOutputSize.y)).rgb * 0.077847;\n" \
    "    col += texture(uSource, vec2(uv.x - b / uOutputSize.x, uv.y)).rgb * 0.123317;\n" \
    "    col += texture(uSource, vec2(uv.x - b / uOutputSize.x, uv.y + b / uOutputSize.y)).rgb * 0.077847;\n" \
    "    col += texture(uSource, vec2(uv.x, uv.y - b / uOutputSize.y)).rgb * 0.123317;\n" \
    "    col += texture(uSource, vec2(uv.x, uv.y)).rgb * 0.195346;\n" \
    "    col += texture(uSource, vec2(uv.x, uv.y + b / uOutputSize.y)).rgb * 0.123317;\n" \
    "    col += texture(uSource, vec2(uv.x + b / uOutputSize.x, uv.y - b / uOutputSize.y)).rgb * 0.077847;\n" \
    "    col += texture(uSource, vec2(uv.x + b / uOutputSize.x, uv.y)).rgb * 0.123317;\n" \
    "    col += texture(uSource, vec2(uv.x + b / uOutputSize.x, uv.y + b / uOutputSize.y)).rgb * 0.077847;\n" \
    "    return col;\n" \
    "}\n" \
    "float corners(vec2 coord)\n" \
    "{\n" \
    "    coord *= uTextureSize.xy / uInputSize.xy;\n" \
    "    coord = (coord - vec2(0.5)) * 1.0 + vec2(0.5);\n" \
    "    coord = min(coord, vec2(1.0) - coord) * vec2(1.0, uInputSize.y / uInputSize.x);\n" \
    "    vec2 cdist = vec2(CORNER_SIZE);\n" \
    "    coord = (cdist - min(coord, cdist));\n" \
    "    float dist = sqrt(dot(coord, coord));\n" \
    "    return clamp((cdist.x - dist) * 1000.0, 0.0, 1.0);\n" \
    "}\n" \
    "void main()\n" \
    "{\n" \
    "    vec2 st = vTexCoord * (uTextureSize.xy / uInputSize.xy);\n" \
    "    st = st * 2.0 - 1.0;\n" \
    "    st *= vec2(OVERSCAN_X, OVERSCAN_Y);\n" \
    "    st = st * 0.5 + 0.5;\n" \
    "    vec2 uv = st;\n" \
    "    if (CURVATURE_X > 0.0 && CURVATURE_Y > 0.0)\n" \
    "    {\n" \
    "        vec2 crtDistortion = vec2(CURVATURE_X, CURVATURE_Y) * 15.0;\n" \
    "        vec2 curvedCoords = st * 2.0 - 1.0;\n" \
    "        float curvedCoordsDistance = sqrt(curvedCoords.x * curvedCoords.x + curvedCoords.y * curvedCoords.y);\n" \
    "        curvedCoords = curvedCoords / curvedCoordsDistance;\n" \
    "        curvedCoords = curvedCoords * (1.0 - pow(vec2(1.0 - (curvedCoordsDistance / 1.4142135623730950488016887242097)), (1.0 / (1.0 + crtDistortion * 0.2))));\n" \
    "        curvedCoords = curvedCoords / (1.0 - pow(vec2(0.29289321881345247559915563789515), (1.0 / (vec2(1.0) + crtDistortion * 0.2))));\n" \
    "        uv = curvedCoords * 0.5 + 0.5;\n" \
    "    }\n" \
    "    float d = length((uv - .5) * .5 * (uv - .5) * .5);\n" \
    "    uv *= uInputSize.xy / uTextureSize.xy;\n" \
    "    vec3 color = texture(uSource, sharpTap(uv)).rgb;\n" \
    "    if (BLUR_STRENGTH > 0.0)\n" \
    "        color = gaussian(uv);\n" \
    "    if (MONOCHROME_DISPLAY == 1.0)\n" \
    "    {\n" \
    "        vec3 ink;\n" \
    "        if (MONOCHROME_TYPE == 1.0) { ink = vec3(0.5, 0.5, 0.5); }\n" \
    "        else if (MONOCHROME_TYPE == 2.0) { ink = vec3(0., 0.4, 0.); }\n" \
    "        else if (MONOCHROME_TYPE == 4.0) { ink = vec3(0.54, 0.56, 0.6); }\n" \
    "        else { ink = vec3(0.5, 0.3, 0.0); }\n" \
    "        color = vec3(floor(MONOCHROME_COLORS * length(color)) / MONOCHROME_COLORS * ink);\n" \
    "    }\n" \
    "    if (CORNER_SIZE > 0.0)\n" \
    "        color *= corners(uv);\n" \
    "    color *= 1.0 - min(1.0, d * LIGHT_STRENGTH);\n" \
    "    float showScanlines = CRT_MASK;\n" \
    "    if (uOutputSize.y < 360.0)\n" \
    "        showScanlines = 0.0;\n" \
    "    float s = 1. - smoothstep(320., 1440., uOutputSize.y) + 1.;\n" \
    "    float j = cos(uv.y * uOutputSize.y * s) * SCANLINES_STRENGTH;\n" \
    "    color = abs(showScanlines - 1.) * color + showScanlines * (color - color * j);\n" \
    "    color *= 1. - (.01 + ceil(mod((st.x + .5) * uOutputSize.x, 3.0)) * (.995 - 1.01)) * showScanlines;\n" \
    "    if (PIXEL_GRID > 0.0)\n" \
    "    {\n" \
    "        vec2 cell = uOutputSize / uInputSize;\n" \
    "        vec2 last = step((vec2(1.0) - fract(uv * uTextureSize)) * cell, vec2(1.0));\n" \
    "        if (cell.x >= 2.0 && cell.y >= 2.0)\n" \
    "            color *= 1.0 - PIXEL_GRID * max(last.x, last.y);\n" \
    "    }\n" \
    "    color = postEffects(color);\n" \
    "    FragColor = vec4(color, 1.0);\n" \
    "}\n"

/* The `#pragma parameter` defaults of the original shader. */
#define ZR_CRT_OVERSCAN_X 1.02f
#define ZR_CRT_OVERSCAN_Y 1.02f
#define ZR_CRT_CURVATURE_X 0.021f
#define ZR_CRT_CURVATURE_Y 0.021f
#define ZR_CRT_BLUR_STRENGTH 0.8f
#define ZR_CRT_MONOCHROME_DISPLAY 0.0f
#define ZR_CRT_MONOCHROME_TYPE 1.0f
#define ZR_CRT_MONOCHROME_COLORS 256.0f
#define ZR_CRT_CORNER_SIZE 0.01f
#define ZR_CRT_SCANLINES_STRENGTH 0.05f
#define ZR_CRT_LIGHT_STRENGTH 8.0f
#define ZR_CRT_GAMMA 0.8f
#define ZR_CRT_CONTRAST 1.0f
#define ZR_CRT_SATURATION 1.0f
#define ZR_CRT_BRIGHTNESS 1.0f
#define ZR_CRT_MASK 1.0f
/* The LCD look's grid: a fifth darker, faint as on a PowerBook panel. */
#define ZR_LCD_PIXEL_GRID 0.2f

#endif /* ZR_SHADERS_H */
