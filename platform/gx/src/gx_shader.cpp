// Shader generator: turns the XF (texgen, lighting) and BP (TEV, indirect,
// alpha compare, fog) register state into a GLSL 3.30 program.  Programs are
// cached by the bytes of the state that shapes the code; everything numeric
// (matrices, colours, references) is a uniform.
#include "gx_internal.h"
#include "gx_glthread.h"
#include "gl_funcs.h"
#include "gx_glcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace gx {

extern uint32_t g_statShaderCompiles;

struct ShaderKey {
    uint32_t numTexGens, numChans, dualTex, clip;
    uint32_t texgen[8], post[8];
    uint32_t chan[4];
    uint32_t numStages, numInd;
    uint32_t colorEnv[16], alphaEnv[16], order[16], ksel[16], ind[16];
    uint32_t swap[4];
    uint32_t iref, alphaFunc, fogType, earlyZ, depthOnly;
};
// The program keys' layout (ShaderKey and ProgramKey): bump it when a field
// is added, removed or means something else, and remake gx_shader_keys.inc
// (tools/gx/shader_keys.py), whose keys are kept in this layout.
static const uint32_t kShaderKeyLayout = 1;

// The qualifier is available in GL 4.2 and as an extension on GL 3.3.
// Older contexts (including macOS) use the ordered depth pass in flushBatch.
static int s_earlyFragmentTests = -1;
bool shaderHasEarlyFragmentTests() {
    if (s_earlyFragmentTests >= 0) return s_earlyFragmentTests != 0;
    s_earlyFragmentTests = 0;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (ext && !strcmp(ext, "GL_ARB_shader_image_load_store")) s_earlyFragmentTests = 1;
    }
    // Also permits exercising the GL 3.3 fallback on newer drivers.
    if (getenv("SMS_GX_FORCE_EARLY_Z_FALLBACK")) s_earlyFragmentTests = 0;
    return s_earlyFragmentTests != 0;
}

// GXSetZCompLoc(GX_TRUE) differs from a late depth test only for a draw that
// writes depth and whose alpha test can reject: the rejected pixels still
// write depth. Early fragment tests are kept to those draws because they also
// count occlusion-query samples before the alpha test's discard, and the
// pollution counters read pixel metrics with ReInitializeGX's early Z left on.
bool earlyZWritesRejected() {
    uint32_t alphaFunc = (g.bp[BP_ALPHACOMPARE] >> 16) & 0xFF;
    return (g.bp[BP_PE_CONTROL] & (1 << 6)) && (g.bp[BP_ZMODE] & 0x11) == 0x11 && alphaFunc != 0x3F &&
           alphaFunc != 0x7F;
}

// ShaderKey.order's bit for the stage whose goop map a goop layer's tint map
// goes with (buildKey); the order itself is 10 bits.
static const uint32_t kOrderGoop = 0x80000000u;
static int s_goopMap = -1;  // the map of the last key built, -1 none
int goopMapNow() { return s_goopMap; }

static void buildKey(ShaderKey& k) {
    memset(&k, 0, sizeof(k));
    k.numTexGens = g.xfReg[XFR_NUMTEXGENS] & 15;
    if (k.numTexGens > 8) k.numTexGens = 8;
    k.numChans = g.xfReg[XFR_NUMCHANS] & 3;
    k.dualTex = g.xfReg[XFR_DUALTEX] & 1;
    k.clip = g.xfReg[0x05] == 0;
    for (uint32_t i = 0; i < k.numTexGens; i++) {
        k.texgen[i] = g.xfReg[XFR_TEXGEN + i];
        if (k.dualTex) k.post[i] = g.xfReg[XFR_POSTTEX + i] & 0x13F;
    }
    for (uint32_t i = 0; i < 2; i++) {
        k.chan[i] = g.xfReg[XFR_COLOR0CTRL + i] & 0x7FFF;
        k.chan[2 + i] = g.xfReg[XFR_ALPHA0CTRL + i] & 0x7FFF;
    }
    uint32_t gen = g.bp[BP_GENMODE];
    k.numStages = ((gen >> 10) & 15) + 1;
    k.numInd = (gen >> 16) & 7;
    if (k.numInd > 4) k.numInd = 4;
    for (uint32_t s = 0; s < k.numStages; s++) {
        k.colorEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFF;
        k.alphaEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFF;
        k.order[s] = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
        k.ksel[s] = (s & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
        k.ind[s] = k.numInd ? (g.bp[BP_IND_CMD + s] & 0x1FFFFF) : 0;
    }
    for (int t = 0; t < 4; t++)
        k.swap[t] = (g.bp[BP_TEV_KSEL + 2 * t] & 15) | (g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4;
    k.iref = k.numInd ? g.bp[BP_RAS1_IREF] & 0xFFFFFF : 0;
    // A goop layer drawn in colours of its own (GXPC_SetGoopTint): the first
    // plain (not indirect) stage that samples its goop map is marked, and the
    // program tints its result. Old keys never have the bit, so the layout
    // (and gx_shader_keys.inc) stays as it was.
    s_goopMap = -1;
    for (uint32_t s = 0; s < k.numStages; s++) {
        const uint32_t ord = k.order[s];
        if (!((ord >> 6) & 1) || k.ind[s] || !goopTinted(g.texImage[ord & 7])) continue;
        k.order[s] |= kOrderGoop;
        s_goopMap = int(ord & 7);
        break;
    }
    k.alphaFunc = (g.bp[BP_ALPHACOMPARE] >> 16) & 0xFF;
    k.earlyZ = earlyZWritesRejected() && shaderHasEarlyFragmentTests();
    k.fogType = (g.bp[BP_FOG3] >> 21) & 7;
}

// ------------------------------------------------------------------ vertex shader
static const char* kVsHeader = R"(#version 330 core
layout(std140) uniform XFBlock { uvec4 xf[184]; };
uniform vec4 u_proj[2];
uniform vec4 u_vp[2];
uniform vec2 u_efb;
uniform vec4 u_chan[4];
uniform vec4 u_uclip;
uniform vec4 u_wet[2];
uniform vec4 u_shadow[12];
// Moving Vegetation (gx_render.cpp, swayView): [0] up, on; [1] wind, time; [2] the plant's lowest
// and highest y (model space), amount, flutter; [3..8] pushers (view, radius); [10] their count,
// push strength
uniform vec4 u_sway[11];
// 3D Ladders (gx_fifo.cpp): [0] the way back (model space) and how far, [1].x how dark
uniform vec4 u_shell[2];
in vec3 a_pos; in vec3 a_nrm; in vec3 a_bin; in vec3 a_tan;
in vec4 a_clr0; in vec4 a_clr1;
in vec2 a_tex0; in vec2 a_tex1; in vec2 a_tex2; in vec2 a_tex3;
in vec2 a_tex4; in vec2 a_tex5; in vec2 a_tex6; in vec2 a_tex7;
in uvec3 a_mtx;
out vec4 v_c0; out vec4 v_c1;
out vec3 v_wn; out vec3 v_wp; out vec3 v_wl; out vec3 v_wlc; out vec3 v_mp;
out vec3 v_tc0; out vec3 v_tc1; out vec3 v_tc2; out vec3 v_tc3;
out vec3 v_tc4; out vec3 v_tc5; out vec3 v_tc6; out vec3 v_tc7;
float X(int i) { return uintBitsToFloat(xf[i >> 2][i & 3]); }
vec4 R4(int w) { return vec4(X(w), X(w + 1), X(w + 2), X(w + 3)); }
vec3 R3(int w) { return vec3(X(w), X(w + 1), X(w + 2)); }
vec4 C(int w) { uint u = xf[w >> 2][w & 3];
  return vec4(float((u >> 24) & 255u), float((u >> 16) & 255u), float((u >> 8) & 255u), float(u & 255u)) / 255.0; }
// light block: word 608 + 16 * light
vec4 lightColor(int l) { return C(608 + l * 16 + 3); }
vec3 lightA(int l) { return R3(608 + l * 16 + 4); }
vec3 lightK(int l) { return R3(608 + l * 16 + 7); }
vec3 lightPos(int l) { return R3(608 + l * 16 + 10); }
vec3 lightDir(int l) { return R3(608 + l * 16 + 13); }
// A plant's point moved (vp: in the view; mp, mn: its position and normal in its model). How much:
// by how far up the plant it is (the model's own height, when the plant is a model of its own),
// squared, so that it stays rooted; a stage's plants (part of the level's own mesh, no height of
// their own) all alike, but not where the surface lies flat facing up (the ground the plants'
// material also covers). The wind leans it its way in gusts that roll across the
// stage, its leaves flutter, and Mario and FLUDD's water push it aside (and a little down).
vec3 sway(vec3 vp, vec3 mp, vec3 mn) {
  vec3 up = u_sway[0].xyz, wind = u_sway[1].xyz;
  float t = u_sway[1].w;
  float h = u_sway[2].y - u_sway[2].x;
  bool own = h > 1.0 && h < 1500.0;
  float w = own ? clamp((mp.y - u_sway[2].x) / h, 0.0, 1.0) : 1.0;
  w *= w;
  if (!own && dot(mn, mn) > 0.01) w *= 1.0 - smoothstep(0.7, 0.92, abs(normalize(mn).y));
  float amp = u_sway[2].z * (own ? clamp(h * 0.04, 3.0, 18.0) : 10.0);
  float ph = dot(mp.xz, vec2(0.0021, 0.0017));
  float gust = 0.55 + 0.3 * sin(t * 1.3 + ph) + 0.15 * sin(t * 2.9 + ph * 2.3);
  float fl = sin(t * 7.0 + dot(mp, vec3(0.13, 0.11, 0.17))) * u_sway[2].w;
  vec3 side = cross(up, wind);
  vp += (wind * gust + side * (fl * 0.5) + up * (fl * 0.3)) * (amp * w);
  int n = int(u_sway[10].x);
  for (int i = 0; i < 6; i++) {
    if (i >= n) break;
    vec4 q = u_sway[3 + i];
    vec3 d = vp - q.xyz;
    float dv = dot(d, up);
    vec3 dh = d - up * dv;
    float l = length(dh);
    if (l < q.w && l > 0.01 && abs(dv) < q.w * 1.5) {
      float k = 1.0 - l / q.w;
      vp += (dh / l * 0.6 - up * 0.25) * (k * k * q.w * u_sway[10].y * max(w, 0.15));
    }
  }
  return vp;
}
)";

// Attenuation divides by k0 + k1*d + k2*d^2.  A light loaded with all-zero
// coefficients (J3D leaves unused lights like that) must contribute nothing
// rather than 0/0 = NaN, which would turn the whole channel black; a zero
// denominator with a positive numerator saturates, as the hardware reciprocal does.
static void emitLight(std::string& s, uint32_t ctrl, const char* comp, const char* sumName, int light) {
    char buf[1024];
    std::string sumS = std::string(sumName) + comp;
    const char* sum = sumS.c_str();
    uint32_t diff = (ctrl >> 7) & 3;
    bool attn = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
    const char* dfn = diff == 0 ? "1.0" : diff == 1 ? "dot(N, L)" : "max(dot(N, L), 0.0)";
    if (!attn) {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = normalize(lightPos(%d) - vpos); %s += (%s) * lightColor(%d)%s; }\n", light, sum, dfn,
                 light, comp);
    } else if (spot) {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = lightPos(%d) - vpos; float d2 = dot(L, L); float d = sqrt(d2); L /= d;\n"
                 "    float cs = max(dot(L, lightDir(%d)), 0.0); vec3 a = lightA(%d); vec3 k = lightK(%d);\n"
                 "    float at = max(a.x + a.y * cs + a.z * cs * cs, 0.0) / max(k.x + k.y * d + k.z * d2, 1e-30);\n"
                 "    %s += at * (%s) * lightColor(%d)%s; }\n",
                 light, light, light, light, sum, dfn, light, comp);
    } else {
        snprintf(buf, sizeof buf,
                 "  { vec3 L = normalize(lightPos(%d)); float nl = dot(N, L);\n"
                 "    float t = nl > 0.0 ? max(dot(N, lightDir(%d)), 0.0) : 0.0; vec3 a = lightA(%d); vec3 k = lightK(%d);\n"
                 "    float at = max(a.x + a.y * t + a.z * t * t, 0.0) / max(k.x + k.y * t + k.z * t * t, 1e-30);\n"
                 "    %s += at * (%s) * lightColor(%d)%s; }\n",
                 light, light, light, light, sum, dfn, light, comp);
    }
    s += buf;
}

static std::string genVS(const ShaderKey& k) {
    std::string s = kVsHeader;
    char buf[1024];
    s += "void main() {\n"
         "  int pm = int(a_mtx.x & 255u);\n"
         "  vec4 p = vec4(a_pos, 1.0);\n";
    // 3D Ladders: a layer of a ladder's card, moved back toward the wall behind it
    // (along the corner's own normal when it has one, away from the camera: the layers go behind the
    // card from either side; else the way given, the card's)
    if (k.depthOnly == 5)
        s += "  { int pm0 = int(a_mtx.x & 255u) * 3 + 256;\n"
             "    vec3 nv = vec3(dot(R3(pm0), a_nrm), dot(R3(pm0 + 3), a_nrm), dot(R3(pm0 + 6), a_nrm));\n"
             "    vec3 dir = dot(a_nrm, a_nrm) > 0.01 ? normalize(a_nrm) * (nv.z > 0.0 ? -1.0 : 1.0) : u_shell[0].xyz;\n"
             "    p.xyz += dir * u_shell[0].w; }\n";
    s += "  vec3 vpos = vec3(dot(R4(pm * 4), p), dot(R4(pm * 4 + 4), p), dot(R4(pm * 4 + 8), p));\n"
         "  if (u_sway[0].w > 0.5) vpos = sway(vpos, a_pos, a_nrm);\n"
         "  int nm = 256 + pm * 3;\n"
         "  vec3 N = vec3(dot(R3(nm), a_nrm), dot(R3(nm + 3), a_nrm), dot(R3(nm + 6), a_nrm));\n"
         "  N = dot(N, N) > 0.0 ? normalize(N) : vec3(0.0, 0.0, 1.0);\n"
         // Wet Mario (u_wet[0].x, 0 dry): where its lights come from (by their brightness), and
         // their colour, for the fragment shader's highlight
         "  vec3 wdir = vec3(0.0), wcol = vec3(0.0);\n"
         "  vec4 clip;\n"
         "  if (u_proj[1].z == 0.0) {\n"
         "    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y * vpos.z, u_proj[0].z * vpos.y + u_proj[0].w * vpos.z,\n"
         "                u_proj[1].x * vpos.z + u_proj[1].y, -vpos.z);\n"
         "  } else {\n"
         "    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y, u_proj[0].z * vpos.y + u_proj[0].w,\n"
         "                u_proj[1].x * vpos.z + u_proj[1].y, 1.0);\n"
         "  }\n"
         // viewport in EFB pixels, then to GL clip space over the whole EFB (row 0 = EFB top)
         "  gl_Position.x = ((u_vp[1].x - 342.0) * clip.w + u_vp[0].x * clip.x) * (2.0 / u_efb.x) - clip.w;\n"
         "  gl_Position.y = ((u_vp[1].y - 342.0) * clip.w + u_vp[0].y * clip.y) * (2.0 / u_efb.y) - clip.w;\n"
         "  float zw = u_vp[1].z * clip.w + u_vp[0].z * clip.z;\n"
         "  gl_Position.z = zw * (2.0 / 16777215.0) - clip.w;\n"
         "  gl_Position.w = clip.w;\n";
    if (k.clip) s += "  gl_ClipDistance[0] = clip.z + clip.w;\n  gl_ClipDistance[1] = -clip.z;\n";
    else s += "  gl_ClipDistance[0] = 1.0;\n  gl_ClipDistance[1] = 1.0;\n";
    // the port's own clip plane (State::userClip): (0, 0, 0, 1) while off
    s += "  gl_ClipDistance[2] = dot(vec4(vpos, 1.0), u_uclip);\n";

    // lighting: channels 0/1, colour and alpha controlled separately
    const char* vtx[2] = {"a_clr0", "a_clr1"};
    for (int c = 0; c < 2; c++) {
        uint32_t cc = k.chan[c], ac = k.chan[2 + c];
        snprintf(buf, sizeof buf, "  vec4 col%d;\n", c);
        s += buf;
        if (uint32_t(c) >= k.numChans) {
            snprintf(buf, sizeof buf, "  col%d = vec4(0.0);\n", c);
            s += buf;
            continue;
        }
        for (int part = 0; part < 2; part++) {
            uint32_t ctrl = part ? ac : cc;
            const char* comp = part ? ".a" : ".rgb";
            snprintf(buf, sizeof buf, "  { vec4 mat = %s; ", (ctrl & 1) ? vtx[c] : (c ? "u_chan[3]" : "u_chan[2]"));
            s += buf;
            if (ctrl & 2) {
                snprintf(buf, sizeof buf, "vec4 amb = %s; vec4 lsum = amb;\n", (ctrl & 64) ? vtx[c] : (c ? "u_chan[1]" : "u_chan[0]"));
                s += buf;
                uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
                for (int l = 0; l < 8; l++)
                    if (mask & (1u << l)) {
                        emitLight(s, ctrl, comp, "lsum", l);
                        // Wet Mario, Wet Characters (depthOnly 4): this light, for the highlight
                        if (c == 0 && part == 0 && (k.depthOnly == 3 || k.depthOnly == 4)) {
                            snprintf(buf, sizeof buf,
                                     "    if (%s) { vec3 lc = lightColor(%d).rgb;\n"
                                     "      wdir += normalize(lightPos(%d) - vpos) * dot(lc, vec3(0.3333));\n"
                                     "      wcol += lc; }\n",
                                     k.depthOnly == 3 ? "u_wet[0].x > 0.0" : "true", l, l);
                            s += buf;
                        }
                    }
                snprintf(buf, sizeof buf, "    col%d%s = (mat * clamp(lsum, 0.0, 1.0))%s; }\n", c, comp, comp);
            } else {
                snprintf(buf, sizeof buf, "col%d%s = mat%s; }\n", c, comp, comp);
            }
            s += buf;
        }
    }
    // Wet Mario: what the fragment shader needs (its normal and position in view space, its lights)
    s += "  v_wn = N; v_wp = vpos; v_wl = wdir; v_wlc = min(wcol, vec3(1.0)); v_mp = a_pos;\n";
    s += "  v_c0 = col0;\n  v_c1 = col1;\n";

    // texgen
    for (uint32_t i = 0; i < 8; i++) {
        if (i >= k.numTexGens) {
            snprintf(buf, sizeof buf, "  v_tc%u = vec3(a_tex%u, 1.0);\n", i, i);
            s += buf;
            continue;
        }
        uint32_t tg = k.texgen[i];
        uint32_t proj = (tg >> 1) & 1, form = (tg >> 2) & 1, type = (tg >> 4) & 7, row = (tg >> 7) & 31;
        uint32_t embSrc = (tg >> 12) & 7;
        std::string src;
        if (row == 0) src = "vec4(a_pos, 1.0)";
        else if (row == 1) src = "vec4(a_nrm, 1.0)";
        else if (row == 2) src = "a_clr0";
        else if (row == 3) src = "vec4(a_bin, 1.0)";
        else if (row == 4) src = "vec4(a_tan, 1.0)";
        else {
            snprintf(buf, sizeof buf, "vec4(a_tex%u, 1.0, 1.0)", (row - 5) & 7);
            src = buf;
        }
        if (form == 0 && row >= 5) {
            // AB11: third component forced to 1
        } else if (form == 0) {
            src = "vec4((" + src + ").xy, 1.0, 1.0)";
        }
        // texture matrix index for this coordinate
        const char* mi[8] = {"(a_mtx.x >> 8) & 255u", "(a_mtx.x >> 16) & 255u", "a_mtx.x >> 24", "a_mtx.y & 255u",
                              "(a_mtx.y >> 8) & 255u", "(a_mtx.y >> 16) & 255u", "a_mtx.y >> 24", "a_mtx.z & 255u"};
        if (type == 0) {
            snprintf(buf, sizeof buf,
                     "  { vec4 sv = %s; int tm = int(%s) * 4; vec3 t = vec3(dot(R4(tm), sv), dot(R4(tm + 4), sv), %s);\n",
                     src.c_str(), mi[i], proj ? "dot(R4(tm + 8), sv)" : "1.0");
            s += buf;
        } else if (type == 1) {
            // emboss: offset of the source coordinate (approximated as the source itself)
            snprintf(buf, sizeof buf, "  { vec3 t = v_tc%u;\n", embSrc < i ? embSrc : 0);
            s += buf;
        } else {
            snprintf(buf, sizeof buf, "  { vec3 t = vec3(%s.rg, 1.0);\n", type == 3 ? "col1" : "col0");
            s += buf;
        }
        if (k.dualTex && type == 0) {
            uint32_t pt = k.post[i] & 63;
            bool norm = (k.post[i] >> 8) & 1;
            snprintf(buf, sizeof buf,
                     "    %s int pw = 352 + %u * 4; vec4 pv = vec4(t, 1.0);\n"
                     "    t = vec3(dot(R4(pw), pv), dot(R4(pw + 4), pv), dot(R4(pw + 8), pv));\n",
                     norm ? "t = normalize(t);" : "", pt);
            s += buf;
        }
        snprintf(buf, sizeof buf, "    v_tc%u = t; }\n", i);
        s += buf;
    }
    // Shadows: a caster's draw into the sun's shadow map (depthOnly 2): from the sun, not the
    // camera (u_shadow rows 4-7: view space to the map), none of the camera's clipping
    // (rows 6-8), into its cascade's half of the texture (u_shadow[11].x: 0 the left, 1 the right),
    // clipped to that half
    if (k.depthOnly == 2)
        s += "  { vec4 sp = vec4(vpos, 1.0);\n"
             "    vec3 c = vec3(dot(u_shadow[6], sp), dot(u_shadow[7], sp), dot(u_shadow[8], sp));\n"
             "    gl_Position = vec4(c.x * 0.5 + (u_shadow[11].x > 0.5 ? 0.5 : -0.5), c.y, c.z, 1.0);\n"
             "    gl_ClipDistance[0] = 1.0 - c.x; gl_ClipDistance[1] = 1.0 + c.x; gl_ClipDistance[2] = 1.0; }\n";
    s += "}\n";
    return s;
}

// ------------------------------------------------------------------ fragment shader
static const char* kFsHeader = R"(#version 330 core
in vec4 v_c0; in vec4 v_c1;
// Sandy Mario (gx_api.cpp GXPC_SetSand): [0] the plane at his feet (up, in the view); [1] his feet's
// sand's height, and how much of his feet's, front's and back's is left; [2] the way he faces, his
// back's height
uniform vec4 u_sand[3];
in vec3 v_wn; in vec3 v_wp; in vec3 v_wl; in vec3 v_wlc; in vec3 v_mp;
uniform vec4 u_wet[2];
// Wet Characters (gx_api.cpp GXPC_SetWetCharacters): [0] count, darkness, shine; [1] the view's up;
// then per wet one (up to 8), its sphere (centre, radius) and (wetness, waterline d)
uniform vec4 u_wetc[18];
uniform vec4 u_shell[2];
uniform vec4 u_shadow[12];
uniform sampler2DShadow u_shadowMap;
uniform vec4 u_plight[17];
// Shadows: how lit a point is in a cascade's map (c: its clip space; tile 0 near, 1 far; the two
// side by side in the texture), 3 by 3 taps (each comparing 4 texels), kept to the cascade's half.
// Soft Shadow Edges (u_shadow[11].w: how wide the soft edge is, in units): 16 taps over a disc
// that wide (the same in both maps: a map's texel is u_shadow[9].y / 1.5 units, halved), turned
// for each pixel so that they blend into one smooth edge, not 16 copies. Each tap compares the
// depth the surface itself has there (g: how its depth runs across the map), not the centre's:
// else a wide disc on ground that slopes away from the sun finds the ground itself in front, and
// the ground shades itself round every shadow's edge.
float shadowLit(vec3 c, float tile, vec2 g) {
  vec2 uv = c.xy * 0.5 + 0.5;
  float zr = c.z * 0.5 + 0.5;
  float t = u_shadow[9].w;
  float lit = 0.0;
  if (u_shadow[11].w > 0.0) {
    float unitsUV = 1.5 * t / (tile < 0.5 ? u_shadow[9].y : u_shadow[9].z);  // a unit, in the map
    float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec2 cs = vec2(cos(ang), sin(ang));
    float width = u_shadow[11].w;
    float r = max(width * unitsUV, t);
    for (int i = 0; i < 16; i++) {
      float th = float(i) * 2.3999632;
      vec2 o = vec2(cos(th), sin(th)) * sqrt((float(i) + 0.5) / 16.0) * r;
      o = vec2(o.x * cs.x - o.y * cs.y, o.x * cs.y + o.y * cs.x);
      vec2 q = clamp(uv + o, vec2(t), vec2(1.0 - t));
      lit += textureLod(u_shadowMap, vec3((q.x + tile) * 0.5, q.y, zr + dot(g, q - uv)), 0.0);
    }
    return lit / 16.0;
  }
  for (int sy = -1; sy <= 1; sy++)
    for (int sx = -1; sx <= 1; sx++) {
      vec2 q = clamp(uv + vec2(sx, sy) * t, vec2(t), vec2(1.0 - t));
      lit += textureLod(u_shadowMap, vec3((q.x + tile) * 0.5, q.y, zr), 0.0);
    }
  return lit / 9.0;
}
// How a face's depth in a cascade's map (rows r0 to r2) runs across the map: per unit of the map's
// x and y (0 to 1), in its depth (0 to 1); none for faces turned nearly edge-on to the sun
vec2 shadowSlope(vec3 n, vec4 r0, vec4 r1, vec4 r2) {
  vec3 t1 = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
  vec3 t2 = cross(n, t1);
  vec3 a = vec3(dot(r0.xyz, t1), dot(r1.xyz, t1), dot(r2.xyz, t1));
  vec3 b = vec3(dot(r0.xyz, t2), dot(r1.xyz, t2), dot(r2.xyz, t2));
  float det = a.x * b.y - a.y * b.x;
  if (abs(det) < 1e-12) return vec2(0.0);
  vec2 g = vec2(a.z * b.y - b.z * a.y, a.x * b.z - b.x * a.z) / det;
  float l = length(g);
  return l > 4.0 ? g * (4.0 / l) : g;
}
in vec3 v_tc0; in vec3 v_tc1; in vec3 v_tc2; in vec3 v_tc3;
in vec3 v_tc4; in vec3 v_tc5; in vec3 v_tc6; in vec3 v_tc7;
uniform sampler2D u_tex[8];
uniform ivec4 u_tevreg[4];
uniform ivec4 u_konst[4];
uniform vec2 u_texscale[8];
uniform vec2 u_texsize[8];
uniform ivec2 u_alpharef;
uniform vec4 u_fog;
uniform vec4 u_fogcolor;
uniform vec4 u_indmtx[6];
out vec4 o_color;
ivec4 texi(vec4 c) { return ivec4(round(clamp(c, 0.0, 1.0) * 255.0)); }
vec2 proj(vec3 t) { return t.xy / (t.z == 0.0 ? 1.0 : t.z); }
ivec3 lerp3(ivec3 a, ivec3 b, ivec3 c) { ivec3 c2 = c + (c >> 7); return (a * (256 - c2) + b * c2 + 128) >> 8; }
int lerp1(int a, int b, int c) { int c2 = c + (c >> 7); return (a * (256 - c2) + b * c2 + 128) >> 8; }
)";

static const char* kColorArg[16] = {"cprev.rgb", "cprev.aaa", "creg0.rgb", "creg0.aaa", "creg1.rgb", "creg1.aaa",
                                    "creg2.rgb", "creg2.aaa", "tex.rgb",   "tex.aaa",   "ras.rgb",   "ras.aaa",
                                    "ivec3(255)", "ivec3(128)", "konst.rgb", "ivec3(0)"};
static const char* kAlphaArg[8] = {"cprev.a", "creg0.a", "creg1.a", "creg2.a", "tex.a", "ras.a", "konst.a", "0"};
static const char* kRegName[4] = {"cprev", "creg0", "creg1", "creg2"};

static std::string konstExpr(uint32_t sel, bool alpha) {
    static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    char buf[64];
    if (sel < 8) {
        snprintf(buf, sizeof buf, alpha ? "%d" : "ivec3(%d)", k8[sel]);
        return buf;
    }
    if (!alpha && sel >= 12 && sel < 16) {
        snprintf(buf, sizeof buf, "u_konst[%u].rgb", sel - 12);
        return buf;
    }
    if (sel >= 16) {
        const char comp = "rgba"[(sel - 16) >> 2];
        snprintf(buf, sizeof buf, alpha ? "u_konst[%u].%c" : "ivec3(u_konst[%u].%c)", sel & 3, comp);
        return buf;
    }
    return alpha ? "255" : "ivec3(255)";
}

static std::string swizzle(uint32_t swap) {
    std::string s;
    const char* c = "rgba";
    s += c[swap & 3];
    s += c[(swap >> 2) & 3];
    s += c[(swap >> 4) & 3];
    s += c[(swap >> 6) & 3];
    return s;
}

static std::string genFS(const ShaderKey& k) {
    std::string s = kFsHeader;
    if (k.earlyZ) {
        s.insert(s.find('\n') + 1, "#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n");
    }
    if (k.depthOnly == 1) return s + "void main() { o_color = vec4(0.0); }\n";
    char buf[2048];
    int goopStage = -1;  // its result tinted by a goop layer's tint map (GXPC_SetGoopTint)
    for (uint32_t st = 0; st < k.numStages && goopStage < 0; st++)
        if (k.order[st] & kOrderGoop) goopStage = int(st);
    if (goopStage >= 0) s.insert(s.find("out vec4 o_color;"), "uniform sampler2D u_goop;\nuniform float u_goopref;\n");
    s += "void main() {\n"
         "  ivec4 cprev = u_tevreg[0], creg0 = u_tevreg[1], creg1 = u_tevreg[2], creg2 = u_tevreg[3];\n"
         "  ivec4 ras0 = texi(v_c0), ras1 = texi(v_c1);\n"
         "  ivec4 tex = ivec4(255), ras = ivec4(0), konst = ivec4(0);\n"
         "  int bumpA = 0; vec2 prevTc = vec2(0.0);\n";
    // indirect texture lookups
    for (uint32_t i = 0; i < k.numInd; i++) {
        uint32_t map = (k.iref >> (6 * i)) & 7, coord = (k.iref >> (6 * i + 3)) & 7;
        uint32_t ss = (g.bp[i < 2 ? BP_RAS1_SS0 : BP_RAS1_SS1] >> ((i & 1) * 8)) & 0xFF;
        snprintf(buf, sizeof buf,
                 "  ivec3 ind%u = texi(texture(u_tex[%u], proj(v_tc%u) / vec2(%d.0, %d.0))).abg;\n", i, map, coord,
                 1 << (ss & 15), 1 << ((ss >> 4) & 15));
        s += buf;
    }
    uint32_t lastDest = 0;
    bool lastAlphaDest = 0;
    uint32_t lastA = 0;
    for (uint32_t st = 0; st < k.numStages; st++) {
        uint32_t ord = k.order[st];
        uint32_t map = ord & 7, coord = (ord >> 3) & 7, en = (ord >> 6) & 1, chan = (ord >> 7) & 7;
        uint32_t ce = k.colorEnv[st], ae = k.alphaEnv[st];
        uint32_t rasSwap = k.swap[ae & 3], texSwap = k.swap[(ae >> 2) & 3];
        snprintf(buf, sizeof buf, "  // stage %u\n  {\n", st);
        s += buf;
        uint32_t ind = k.ind[st];
        if (en) {
            if (ind) {
                uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3,
                         mid = (ind >> 9) & 15, sw = (ind >> 13) & 7, tw = (ind >> 16) & 7, fb = (ind >> 20) & 1;
                static const int fmtMask[4] = {255, 31, 15, 7};
                snprintf(buf, sizeof buf, "    ivec3 iv = ind%u & ivec3(%d);\n", bt < k.numInd ? bt : 0, fmtMask[fmt]);
                s += buf;
                if (bs) {
                    snprintf(buf, sizeof buf, "    bumpA = (ind%u.%c) & 0xF8;\n", bt < k.numInd ? bt : 0, "xxyz"[bs]);
                    s += buf;
                }
                int bv = fmt == 0 ? -128 : 1;
                snprintf(buf, sizeof buf, "    iv += ivec3(%d, %d, %d);\n", (bias & 1) ? bv : 0, (bias & 2) ? bv : 0, (bias & 4) ? bv : 0);
                s += buf;
                if (mid >= 1 && mid <= 3) {
                    // The hardware's integer offset: s.10 matrix entries, the product >> 3 in 1/128 texels,
                    // shifted by the matrix's exponent and kept to the coordinate's 24 bits. Shifted 24 or
                    // more, nothing is left (Sunshine's goop bubbles, ms_raku_indawa, use an exponent of 29).
                    uint32_t m = mid - 1;
                    snprintf(buf, sizeof buf,
                             "    ivec3 m0 = ivec3(u_indmtx[%u].xyz * 1024.0), m1 = ivec3(u_indmtx[%u].xyz * 1024.0);\n"
                             "    ivec2 it = ivec2(m0.x * iv.x + m0.y * iv.y + m0.z * iv.z, m1.x * iv.x + m1.y * iv.y + m1.z * iv.z) >> 3;\n"
                             "    int sc = int(u_indmtx[%u].w);\n"
                             "    it = sc >= 32 ? ivec2(0) : sc >= 0 ? it << sc : it >> -sc;\n"
                             "    vec2 off = vec2((it << 8) >> 8) / 128.0;\n",
                             m * 2, m * 2 + 1, m * 2);
                } else {
                    snprintf(buf, sizeof buf, "    vec2 off = vec2(0.0);\n");
                }
                s += buf;
                snprintf(buf, sizeof buf, "    vec2 tcx = proj(v_tc%u) * u_texscale[%u] * u_texsize[%u];\n", coord, coord, map);
                s += buf;
                static const int wrapSize[8] = {0, 256, 128, 64, 32, 16, 0, 0};
                if (sw == 6) s += "    tcx.x = 0.0;\n";
                else if (sw) { snprintf(buf, sizeof buf, "    tcx.x = mod(tcx.x, %d.0);\n", wrapSize[sw]); s += buf; }
                if (tw == 6) s += "    tcx.y = 0.0;\n";
                else if (tw) { snprintf(buf, sizeof buf, "    tcx.y = mod(tcx.y, %d.0);\n", wrapSize[tw]); s += buf; }
                snprintf(buf, sizeof buf, "    tcx += off%s; prevTc = tcx;\n", fb ? " + prevTc" : "");
                s += buf;
                snprintf(buf, sizeof buf, "    tex = texi(texture(u_tex[%u], tcx / u_texsize[%u])).%s;\n", map, map, swizzle(texSwap).c_str());
            } else {
                snprintf(buf, sizeof buf, "    tex = texi(texture(u_tex[%u], proj(v_tc%u) * u_texscale[%u])).%s;\n", map, coord,
                         coord, swizzle(texSwap).c_str());
            }
            s += buf;
        }
        if (chan == 0) snprintf(buf, sizeof buf, "    ras = ras0.%s;\n", swizzle(rasSwap).c_str());
        else if (chan == 1) snprintf(buf, sizeof buf, "    ras = ras1.%s;\n", swizzle(rasSwap).c_str());
        else if (chan == 5) snprintf(buf, sizeof buf, "    ras = ivec4(bumpA);\n");
        else if (chan == 6) snprintf(buf, sizeof buf, "    ras = ivec4(bumpA | (bumpA >> 5));\n");
        else snprintf(buf, sizeof buf, "    ras = ivec4(0);\n");
        s += buf;
        uint32_t kc = k.ksel[st] & 31, ka = (k.ksel[st] >> 5) & 31;
        snprintf(buf, sizeof buf, "    konst = ivec4(%s, %s);\n", konstExpr(kc, false).c_str(), konstExpr(ka, true).c_str());
        s += buf;

        // colour
        {
            uint32_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
            uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clampv = (ce >> 19) & 1, scale = (ce >> 20) & 3,
                     dest = (ce >> 22) & 3;
            snprintf(buf, sizeof buf,
                     "    ivec3 A = (%s) & 255, B = (%s) & 255, Cc = (%s) & 255, D = %s;\n", kColorArg[a], kColorArg[b],
                     kColorArg[c], kColorArg[d]);
            s += buf;
            if (bias != 3) {
                snprintf(buf, sizeof buf, "    ivec3 r = D %c lerp3(A, B, Cc)%s;\n", op ? '-' : '+',
                         bias == 1 ? " + 128" : bias == 2 ? " - 128" : "");
                s += buf;
                if (scale == 1) s += "    r *= 2;\n";
                else if (scale == 2) s += "    r *= 4;\n";
                else if (scale == 3) s += "    r = r >> 1;\n";
            } else {
                uint32_t mode = (scale << 1) | op;
                const char* cond;
                switch (mode) {
                case 0: cond = "bvec3(A.r > B.r)"; break;
                case 1: cond = "bvec3(A.r == B.r)"; break;
                case 2: cond = "bvec3((A.g * 256 + A.r) > (B.g * 256 + B.r))"; break;
                case 3: cond = "bvec3((A.g * 256 + A.r) == (B.g * 256 + B.r))"; break;
                case 4: cond = "bvec3((A.b * 65536 + A.g * 256 + A.r) > (B.b * 65536 + B.g * 256 + B.r))"; break;
                case 5: cond = "bvec3((A.b * 65536 + A.g * 256 + A.r) == (B.b * 65536 + B.g * 256 + B.r))"; break;
                case 6: cond = "greaterThan(A, B)"; break;
                default: cond = "equal(A, B)"; break;
                }
                snprintf(buf, sizeof buf, "    ivec3 r = D + ivec3(%s) * Cc;\n", cond);
                s += buf;
            }
            snprintf(buf, sizeof buf, "    %s.rgb = %s;\n", kRegName[dest],
                     clampv ? "clamp(r, 0, 255)" : "clamp(r, -1024, 1023)");
            s += buf;
            lastDest = dest;
        }
        // alpha
        {
            uint32_t d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
            uint32_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clampv = (ae >> 19) & 1, scale = (ae >> 20) & 3,
                     dest = (ae >> 22) & 3;
            snprintf(buf, sizeof buf, "    int Aa = (%s) & 255, Ba = (%s) & 255, Ca = (%s) & 255, Da = %s;\n",
                     kAlphaArg[a], kAlphaArg[b], kAlphaArg[c], kAlphaArg[d]);
            s += buf;
            if (bias != 3) {
                snprintf(buf, sizeof buf, "    int ra = Da %c lerp1(Aa, Ba, Ca)%s;\n", op ? '-' : '+',
                         bias == 1 ? " + 128" : bias == 2 ? " - 128" : "");
                s += buf;
                if (scale == 1) s += "    ra *= 2;\n";
                else if (scale == 2) s += "    ra *= 4;\n";
                else if (scale == 3) s += "    ra = ra >> 1;\n";
            } else {
                uint32_t mode = (scale << 1) | op;
                const char* cond;
                switch (mode) {
                case 0: cond = "A.r > B.r"; break;
                case 1: cond = "A.r == B.r"; break;
                case 2: cond = "(A.g * 256 + A.r) > (B.g * 256 + B.r)"; break;
                case 3: cond = "(A.g * 256 + A.r) == (B.g * 256 + B.r)"; break;
                case 4: cond = "(A.b * 65536 + A.g * 256 + A.r) > (B.b * 65536 + B.g * 256 + B.r)"; break;
                case 5: cond = "(A.b * 65536 + A.g * 256 + A.r) == (B.b * 65536 + B.g * 256 + B.r)"; break;
                case 6: cond = "Aa > Ba"; break;
                default: cond = "Aa == Ba"; break;
                }
                snprintf(buf, sizeof buf, "    int ra = Da + ((%s) ? Ca : 0);\n", cond);
                s += buf;
            }
            snprintf(buf, sizeof buf, "    %s.a = %s;\n", kRegName[dest], clampv ? "clamp(ra, 0, 255)" : "clamp(ra, -1024, 1023)");
            s += buf;
            lastA = dest;
            lastAlphaDest = true;
        }
        s += "  }\n";
    }
    (void)lastAlphaDest;
    // The last stage's output keeps its low 8 bits, as on the console (and in
    // Dolphin): an unclamped stage can wrap. Bianco Hills' river surface
    // computes its alpha as (1 + a) * 2 and relies on the wrap to get 2a back;
    // clamped, it is 255 and the additive layer washes the water out.
    snprintf(buf, sizeof buf, "  ivec4 outc = ivec4(%s.rgb, %s.a) & 255;\n", kRegName[lastDest], kRegName[lastA]);
    s += buf;
    if (goopStage >= 0) {
        // The goop in its layer's colours: the tint map is sampled where the
        // goop map is, and the goop's lightness (over its typical lightness)
        // scales the colour, so its shading, edges and gloss stay; the map's
        // alpha says how far towards that colour.
        const uint32_t coord = (k.order[goopStage] >> 3) & 7;
        snprintf(buf, sizeof buf,
                 "  {\n"
                 "    vec4 gt = texture(u_goop, proj(v_tc%u) * u_texscale[%u]);\n"
                 "    vec3 gc = vec3(outc.rgb) / 255.0;\n"
                 "    float gl = dot(gc, vec3(0.299, 0.587, 0.114)) * u_goopref;\n"
                 "    outc.rgb = ivec3(round(mix(gc, clamp(gt.rgb * gl, 0.0, 1.0), gt.a) * 255.0));\n"
                 "  }\n",
                 coord, coord);
        s += buf;
    }

    // alpha compare
    {
        uint32_t c0 = k.alphaFunc & 7, c1 = (k.alphaFunc >> 3) & 7, op = (k.alphaFunc >> 6) & 3;
        auto cmp = [](uint32_t f, const char* ref) -> std::string {
            static const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
            if (f == 0) return "false";
            if (f == 7) return "true";
            return std::string("(outc.a ") + ops[f] + " " + ref + ")";
        };
        std::string a = cmp(c0, "u_alpharef.x"), b = cmp(c1, "u_alpharef.y");
        std::string e;
        switch (op) {
        case 0: e = a + " && " + b; break;
        case 1: e = a + " || " + b; break;
        case 2: e = a + " != " + b; break;
        default: e = a + " == " + b; break;
        }
        if (!(c0 == 7 && c1 == 7 && op <= 1)) s += "  if (!(" + e + ")) discard;\n";
    }
    s += "  vec4 color = vec4(outc) / 255.0;\n";
    // Shadows: a blended draw's see-through parts (water's surface) cast no shadow into the sun's
    // map; a caster needs nothing more (its colour is not kept), so its program stays small
    if (k.depthOnly == 2) return s + "  if (u_shadow[11].y > 0.5 && color.a < 0.9) discard;\n  o_color = color;\n}\n";
    // Mario's own draw only (depthOnly 3: GXPC_ShadowTag): Wet Mario, Sandy Mario.
    // Every other program goes without them: smaller, quicker to compile (fewer stutters)
    std::string mario = ""
         // Wet Mario (u_wet[0].x, 0 dry; below the waterline u_wet[1], softly over 12 units; how dark
         // u_wet[0].y, how shiny u_wet[0].z, 1 each as it was): soaked
         // cloth and skin are darker (55%) and their colours deeper, as wet things are (less light is
         // scattered back white), with a thin film of water on them: a small, tight highlight from
         // its lights and from the sky above, and a little more at grazing angles (Fresnel)
         "  if (u_wet[0].x > 0.0) {\n"
         "    float w = u_wet[0].x * clamp(0.5 - (dot(u_wet[1].xyz, v_wp) + u_wet[1].w) / 12.0, 0.0, 1.0);\n"
         "    if (w > 0.0) {\n"
         "      vec3 N = normalize(v_wn), V = normalize(-v_wp);\n"
         "      float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
         "      float d = w * u_wet[0].y;\n"
         "      vec3 c = max(mix(vec3(lum), color.rgb, 1.0 + 0.3 * d), vec3(0.0)) * (1.0 - min(0.55 * d, 0.9));\n"
         "      vec3 up = dot(u_wet[1].xyz, u_wet[1].xyz) > 1e-8 ? normalize(u_wet[1].xyz) : vec3(0.0, 1.0, 0.0);\n"
         "      vec3 L = dot(v_wl, v_wl) > 1e-8 ? normalize(v_wl) : up;\n"
         "      float sl = pow(max(dot(N, normalize(L + V)), 0.0), 60.0);\n"
         "      float ss = pow(max(dot(N, normalize(up + V)), 0.0), 40.0);\n"
         "      float fr = pow(1.0 - max(dot(N, V), 0.0), 5.0);\n"
         "      vec3 lc = dot(v_wlc, v_wlc) > 0.0 ? v_wlc : vec3(1.0);\n"
         "      color.rgb = clamp(c + w * u_wet[0].z * (0.3 * sl * lc + 0.08 * ss + 0.06 * fr), 0.0, 1.0);\n"
         "    }\n"
         "  }\n"
         // Sandy Mario (u_sand[1].yzw, 0 none): where his body touched sand, a patchy, grainy coat of
         // it (by a hash of the point in his model, so that it stays on him): his feet up to their
         // height, his front (facing his way) to his chest, his back (facing away) to its height;
         // sand's colour, lit as what it covers, with darker grains
         "  if (u_sand[1].y + u_sand[1].z + u_sand[1].w > 0.0) {\n"
         "    float h = dot(u_sand[0].xyz, v_wp) + u_sand[0].w;\n"
         "    float fwd = dot(normalize(v_wn), u_sand[2].xyz);\n"
         "    float a = u_sand[1].y * (1.0 - smoothstep(u_sand[1].x * 0.6, u_sand[1].x, h));\n"
         "    a = max(a, u_sand[1].z * smoothstep(0.05, 0.45, fwd) * (1.0 - smoothstep(110.0, 145.0, h)));\n"
         "    a = max(a, u_sand[1].w * smoothstep(0.05, 0.45, -fwd) * (1.0 - smoothstep(u_sand[2].w * 0.75, u_sand[2].w, h)));\n"
         "    if (a > 0.0) {\n"
         "      vec3 q = v_mp * 0.18;\n"
         "      vec3 c0 = floor(q), f = fract(q);\n"
         "      f = f * f * (3.0 - 2.0 * f);\n"
         "      float n = mix(mix(mix(fract(sin(dot(c0, vec3(12.9898, 78.233, 37.719))) * 43758.55),\n"
         "                            fract(sin(dot(c0 + vec3(1, 0, 0), vec3(12.9898, 78.233, 37.719))) * 43758.55), f.x),\n"
         "                        mix(fract(sin(dot(c0 + vec3(0, 1, 0), vec3(12.9898, 78.233, 37.719))) * 43758.55),\n"
         "                            fract(sin(dot(c0 + vec3(1, 1, 0), vec3(12.9898, 78.233, 37.719))) * 43758.55), f.x), f.y),\n"
         "                    mix(mix(fract(sin(dot(c0 + vec3(0, 0, 1), vec3(12.9898, 78.233, 37.719))) * 43758.55),\n"
         "                            fract(sin(dot(c0 + vec3(1, 0, 1), vec3(12.9898, 78.233, 37.719))) * 43758.55), f.x),\n"
         "                        mix(fract(sin(dot(c0 + vec3(0, 1, 1), vec3(12.9898, 78.233, 37.719))) * 43758.55),\n"
         "                            fract(sin(dot(c0 + vec3(1, 1, 1), vec3(12.9898, 78.233, 37.719))) * 43758.55), f.x), f.y), f.z);\n"
         "      float grain = fract(sin(dot(floor(v_mp * 1.6), vec3(41.3, 289.1, 97.7))) * 15731.7);\n"
         "      float cover = a * smoothstep(0.35, 0.75, n + a * 0.25);\n"
         "      float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
         "      vec3 sandy = vec3(0.88, 0.77, 0.56) * clamp(lum * 0.7 + 0.5, 0.45, 1.0) * (grain > 0.82 ? 0.72 : 1.0);\n"
         "      color.rgb = mix(color.rgb, sandy, clamp(cover * 0.9, 0.0, 0.9));\n"
         "    }\n"
         "  }\n"
         "";
    if (k.depthOnly == 3) s += mario;
    if (k.depthOnly == 5) s += "  color.rgb *= u_shell[1].x;\n";  // 3D Ladders: a layer behind, darker
    // Wet Characters (depthOnly 4: a registered character's draw while some are wet): wet inside
    // the sphere of a wet one (fading over its outer fifth), below its waterline, as Wet Mario is
    if (k.depthOnly == 4)
        s += "  {\n"
             "    float w = 0.0;\n"
             "    vec3 up = u_wetc[1].xyz;\n"
             "    for (int i = 0; i < 8; i++) {\n"
             "      if (float(i) >= u_wetc[0].x) break;\n"
             "      vec4 a = u_wetc[2 + 2 * i], b = u_wetc[3 + 2 * i];\n"
             "      float inside = 1.0 - smoothstep(a.w * 0.8, a.w, length(v_wp - a.xyz));\n"
             "      float below = clamp(0.5 - (dot(up, v_wp) + b.y) / 12.0, 0.0, 1.0);\n"
             "      w = max(w, b.x * inside * below);\n"
             "    }\n"
             "    if (w > 0.0) {\n"
             "      vec3 N = normalize(v_wn), V = normalize(-v_wp);\n"
             "      float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
             "      float d = w * u_wetc[0].y;\n"
             "      vec3 c = max(mix(vec3(lum), color.rgb, 1.0 + 0.3 * d), vec3(0.0)) * (1.0 - min(0.55 * d, 0.9));\n"
             "      vec3 L = dot(v_wl, v_wl) > 1e-8 ? normalize(v_wl) : up;\n"
             "      float sl = pow(max(dot(N, normalize(L + V)), 0.0), 60.0);\n"
             "      float ss = pow(max(dot(N, normalize(up + V)), 0.0), 40.0);\n"
             "      float fr = pow(1.0 - max(dot(N, V), 0.0), 5.0);\n"
             "      vec3 lc = dot(v_wlc, v_wlc) > 0.0 ? v_wlc : vec3(1.0);\n"
             "      color.rgb = clamp(c + w * u_wetc[0].z * (0.3 * sl * lc + 0.08 * ss + 0.06 * fr), 0.0, 1.0);\n"
             "    }\n"
             "  }\n";
    s += ""
         // Shadows (u_shadow[9].x, 0 off; u_shadow[10].w, 1 for the main camera's view): where the
         // sun's light is blocked, the surface darker. The face's own normal (from the position's
         // slopes, whatever the model has), turned to the camera; only faces toward the sun; the
         // position pushed out along it a little (no shadow of a surface on itself). The near map
         // where it reaches, the far one past it, blended between; fading out at the far one's edge.
         // Mario's own draw (u_shadow[11].z) looked up from that far toward the sun: none of himself
         "  if (u_shadow[9].x > 0.0 && u_shadow[10].w > 0.0 && !(u_shadow[11].y > 0.5 && color.a < 0.9)) {\n"
         "    vec3 fn = cross(dFdx(v_wp), dFdy(v_wp));\n"
         "    fn = dot(fn, fn) > 1e-12 ? normalize(fn) : vec3(0.0, 0.0, 1.0);\n"
         "    if (dot(fn, v_wp) > 0.0) fn = -fn;\n"
         "    float nl = dot(fn, u_shadow[10].xyz);\n"
         "    if (nl > 0.0) {\n"
         "      float lean = 1.0 + 2.0 * (1.0 - nl);\n"
         "      vec3 sp0 = v_wp + u_shadow[10].xyz * u_shadow[11].z;\n"
         "      float sb = min(u_shadow[11].w, 5.0) * 0.15;\n"  // Soft Shadow Edges: pushed out a little more
         "      vec4 p0 = vec4(sp0 + fn * (u_shadow[9].y + sb) * lean, 1.0);\n"
         "      vec4 p1 = vec4(sp0 + fn * (u_shadow[9].z + sb) * lean, 1.0);\n"
         "      vec3 a = vec3(dot(u_shadow[0], p0), dot(u_shadow[1], p0), dot(u_shadow[2], p0));\n"
         "      vec3 b = vec3(dot(u_shadow[3], p1), dot(u_shadow[4], p1), dot(u_shadow[5], p1));\n"
         "      float ea = max(abs(a.x), abs(a.y)), eb = max(abs(b.x), abs(b.y));\n"
         "      float wn = a.z < 1.0 ? 1.0 - smoothstep(0.8, 0.95, ea) : 0.0;\n"
         "      float dark = 0.0;\n"
         "      bool soft = u_shadow[11].w > 0.0;\n"
         "      if (wn > 0.0) dark = wn * (1.0 - shadowLit(a, 0.0, soft ? shadowSlope(fn, u_shadow[0], u_shadow[1], u_shadow[2]) : vec2(0.0)));\n"
         "      if (wn < 1.0 && eb < 1.0 && b.z < 1.0)\n"
         "        dark += (1.0 - wn) * (1.0 - shadowLit(b, 1.0, soft ? shadowSlope(fn, u_shadow[3], u_shadow[4], u_shadow[5]) : vec2(0.0))) * (1.0 - smoothstep(0.75, 1.0, eb));\n"
         // a little blue, as shade lit by the sky is
         "      color.rgb *= 1.0 - u_shadow[9].x * dark * smoothstep(0.0, 0.2, nl) * vec3(1.0, 0.94, 0.8);\n"
         "    }\n"
         "  }\n"
         // Fire Lights (u_plight[16].x lights, while their view is drawn): each a warm light that
         // fades to nothing at its radius, brighter on faces turned to it; what it lights is
         // brighter (its own colour more so), with a little of the light's own
         "  if (u_plight[16].x > 0.5) {\n"
         "    vec3 pn = cross(dFdx(v_wp), dFdy(v_wp));\n"
         "    pn = dot(pn, pn) > 1e-12 ? normalize(pn) : vec3(0.0, 0.0, 1.0);\n"
         "    if (dot(pn, v_wp) > 0.0) pn = -pn;\n"
         "    vec3 lsum = vec3(0.0);\n"
         "    int pc = int(u_plight[16].x);\n"
         "    for (int i = 0; i < 8; i++) {\n"
         "      if (i >= pc) break;\n"
         "      vec4 lp = u_plight[i * 2], lc = u_plight[i * 2 + 1];\n"
         "      vec3 L = lp.xyz - v_wp;\n"
         "      float d = length(L);\n"
         "      float a = lp.w > 1.0 ? clamp(1.0 - d / lp.w, 0.0, 1.0) : 0.0;\n"
         "      if (!(a > 0.0)) continue;\n"
         "      lsum += lc.rgb * (lc.a * a * a * (max(dot(pn, L / max(d, 1.0)), 0.0) * 0.7 + 0.3));\n"
         "    }\n"
         "    if (!(lsum.r + lsum.g + lsum.b >= 0.0) || lsum.r + lsum.g + lsum.b > 30.0) lsum = vec3(0.0);\n"
         "    color.rgb = clamp(color.rgb + color.rgb * lsum * 1.5 + lsum * 0.05, 0.0, 1.0);\n"
         "  }\n";
    if (k.fogType) {
        // u_fog: x = A, y = B, z = C, w = 1 for orthographic. The depth is the
        // pixel's own (zw / w, screen-linear, as GX's rasterizer gives fog): a
        // per-vertex z / w interpolated perspective-correctly is wrong across
        // a large polygon and wild where one runs behind the camera, which
        // fogged bands along Noki Bay's undersea floors fully.
        s += "  float ze = u_fog.w != 0.0 ? u_fog.x * gl_FragCoord.z : u_fog.x / (u_fog.y - gl_FragCoord.z);\n"
             "  float f = clamp(ze - u_fog.z, 0.0, 1.0);\n";
        switch (k.fogType) {
        case 4: s += "  f = 1.0 - exp2(-8.0 * f);\n"; break;
        case 5: s += "  f = 1.0 - exp2(-8.0 * f * f);\n"; break;
        case 6: s += "  f = exp2(-8.0 * (1.0 - f));\n"; break;
        case 7: s += "  f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"; break;
        default: break;
        }
        s += "  color.rgb = mix(color.rgb, u_fogcolor.rgb, f);\n";
    }
    s += "  o_color = color;\n}\n";
    return s;
}

// ------------------------------------------------------------------ compile / cache
static GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        logmsg("shader compile failed:\n%s\n--- source ---\n%s", log, src.c_str());
    }
    return sh;
}

// The program cache key: the shader key plus the indirect scale factors,
// which are baked into the code. Compared as bytes (buildKey zero-fills).
struct ProgramKey {
    ShaderKey k;
    uint32_t indScale[2];
    bool operator==(const ProgramKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct ProgramKeyHash {
    size_t operator()(const ProgramKey& k) const { return size_t(hashBytes(&k, sizeof(k))); }
};
static std::unordered_map<ProgramKey, ShaderProgram, ProgramKeyHash> s_programs;
static ProgramKey s_lastKey;
static const ShaderProgram* s_lastProgram = nullptr;

static void bindAttributes(GLuint p) {
    static const char* attrs[] = {"a_pos", "a_nrm", "a_bin", "a_tan", "a_clr0", "a_clr1", "a_tex0", "a_tex1", "a_tex2",
                                  "a_tex3", "a_tex4", "a_tex5", "a_tex6", "a_tex7", "a_mtx"};
    for (GLuint i = 0; i < 15; i++) glBindAttribLocation(p, i, attrs[i]);
    glBindFragDataLocation(p, 0, "o_color");
}

// A linked program's fixed bindings and uniform locations (on the GL thread).
// `aside`: while the game's thread runs on, which expects the bound program
// to be the one it bound (g_glc), so it is bound back afterwards.
static void setupProgram(GLuint p, ShaderProgram& sp, bool aside = false) {
    sp.prog = p;
    sp.uc.init = false;
    GLint bound = 0;
    if (aside) glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    glUseProgram(p);
    if (!aside) g_glc.prog = p;
    GLuint blk = glGetUniformBlockIndex(p, "XFBlock");
    if (blk != GL_INVALID_INDEX) glUniformBlockBinding(p, blk, 0);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    sp.uShadow = glGetUniformLocation(p, "u_shadow");
    sp.uPlight = glGetUniformLocation(p, "u_plight");
    sp.uSway = glGetUniformLocation(p, "u_sway");
    sp.uSand = glGetUniformLocation(p, "u_sand");
    GLint shadowMap = glGetUniformLocation(p, "u_shadowMap");
    if (shadowMap >= 0) glUniform1i(shadowMap, kShadowUnit);
    GLint loc = glGetUniformLocation(p, "u_tex");
    if (loc >= 0) glUniform1iv(loc, 8, units);
    sp.uTevReg = glGetUniformLocation(p, "u_tevreg");
    sp.uKonst = glGetUniformLocation(p, "u_konst");
    sp.uTexScale = glGetUniformLocation(p, "u_texscale");
    sp.uAlphaRef = glGetUniformLocation(p, "u_alpharef");
    sp.uFog = glGetUniformLocation(p, "u_fog");
    sp.uFogColor = glGetUniformLocation(p, "u_fogcolor");
    sp.uIndMtx = glGetUniformLocation(p, "u_indmtx");
    sp.uTexSize = glGetUniformLocation(p, "u_texsize");
    sp.uEfb = glGetUniformLocation(p, "u_efb");
    sp.uProj = glGetUniformLocation(p, "u_proj");
    sp.uViewport = glGetUniformLocation(p, "u_vp");
    sp.uAmbMat = glGetUniformLocation(p, "u_chan");
    sp.uLineWidth = glGetUniformLocation(p, "u_linewidth");
    sp.uUserClip = glGetUniformLocation(p, "u_uclip");
    sp.uWet = glGetUniformLocation(p, "u_wet");
    sp.uWetc = glGetUniformLocation(p, "u_wetc");
    sp.uShell = glGetUniformLocation(p, "u_shell");
    sp.uGoopRef = glGetUniformLocation(p, "u_goopref");
    loc = glGetUniformLocation(p, "u_goop");
    if (loc >= 0) glUniform1i(loc, kGoopUnit);
    sp.uDstAlpha = -1;
    if (aside) glUseProgram(GLuint(bound));
}

static uint64_t sourceHash(const std::string& vs, const std::string& fs) {
    return hashBytes(vs.data(), vs.size(), 0x5348u) ^ hashBytes(fs.data(), fs.size(), 0x4653u) * 0x9E3779B97F4A7C15ull;
}

// ------------------------------------------------------------------ program cache
// Linked programs are kept on disk (glGetProgramBinary) and loaded at the
// first draw of the next session, so a material seen before costs no compile
// (a compile is a hitch of a few to tens of milliseconds, and a stage's
// arrival compiles dozens). The file is per word size, in
// $XDG_CACHE_HOME/sms-port (~/.cache/sms-port; ~/Library/Caches/sms-port on
// macOS; %LOCALAPPDATA%\sms-port on Windows); it starts over when the GL
// driver's vendor, renderer or version changes, and an entry is used only
// while its key still generates the same GLSL. SMS_GX_SHADER_CACHE=path puts
// it elsewhere; =0 turns it off.
static const char kCacheMagic[8] = {'S', 'M', 'S', 'G', 'X', 'P', 'B', '1'};
static std::string s_cachePath;
static FILE* s_cacheOut = nullptr;  // appended to as programs are made
static int s_cacheState = -1;       // -1 not set up, 0 off, 1 on

static void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') {
            std::string d = path.substr(0, i);
#ifdef _WIN32
            _mkdir(d.c_str());
#else
            mkdir(d.c_str(), 0755);
#endif
        }
}

std::string userCacheDir() {
    std::string base;
#ifdef _WIN32
    if (const char* x = getenv("LOCALAPPDATA")) base = std::string(x) + "/sms-port";
#elif defined(__APPLE__)
    if (const char* h = getenv("HOME")) base = std::string(h) + "/Library/Caches/sms-port";
#else
    if (const char* x = getenv("XDG_CACHE_HOME")) base = std::string(x) + "/sms-port";
    else if (const char* h = getenv("HOME")) base = std::string(h) + "/.cache/sms-port";
#endif
    if (!base.empty()) makeDirs(base);
    return base;
}

static uint64_t driverId() {
    std::string id;
    for (GLenum e : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
        const char* v = reinterpret_cast<const char*>(glGetString(e));
        id += v ? v : "?";
        id += '\n';
    }
    return hashBytes(id.data(), id.size(), sizeof(ProgramKey));
}

static bool programCacheOn() { return s_cacheState == 1; }

// Whether a key read back from the cache file is one buildKey can make. A torn
// record (a write cut short, or two games appending at once) reads as garbage,
// and genFS loops numInd times: 0xFFFFFFFF of them ran out of memory.
static bool programKeyValid(const ProgramKey& key) {
    const ShaderKey& k = key.k;
    return k.numTexGens <= 8 && k.numChans <= 3 && k.numStages >= 1 && k.numStages <= 16 && k.numInd <= 4;
}

// Loads the cached programs (on the GL thread, at the first draw). Some drivers build a program's binary
// up again as slowly as its source, one at a time: AMD's took about 0.15 s each, and the 1150 cached took
// three minutes at every start (handing them over all at once did not help), while it compiles them from
// their source side by side at about 0.015 s each. So the first few are timed: when they are that slow,
// the rest are not loaded but compiled from source by the warm-up instead, in the background as stages
// load (s_cacheKeysToWarm), and not written to the file again (s_cacheFileKeys).
static std::vector<ProgramKey> s_cacheKeysToWarm;
static std::unordered_set<ProgramKey, ProgramKeyHash> s_cacheFileKeys;
static bool s_slowBinaries = false;
static void programCacheLoad() {
    s_cacheState = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!glGetProgramBinary || !glProgramBinary || !glProgramParameteri) return;
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    if (formats <= 0) return;
    const char* e = getenv("SMS_GX_SHADER_CACHE");
    if (e && !strcmp(e, "0")) return;
    if (e && *e) {
        s_cachePath = e;
    } else {
        std::string base = userCacheDir();
        if (base.empty()) return;
        // the Vulkan backend's programs (glvk, SPIR-V) in a file of their own,
        // so switching backends does not start either cache over
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const bool vk = vendor && !strcmp(vendor, "glvk");
        s_cachePath = base + (vk ? "/gx-programs-vk-" : "/gx-programs-") + (sizeof(void*) == 8 ? "64.bin" : "32.bin");
    }
    const uint64_t driver = driverId();
    // the driver's compile threads, before the binaries go to it (warmSetup asks again for its compiles)
    if (glMaxShaderCompilerThreadsKHR) {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; i++) {
            const char* x = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
            if (x && (!strcmp(x, "GL_KHR_parallel_shader_compile") || !strcmp(x, "GL_ARB_parallel_shader_compile"))) {
                glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
                break;
            }
        }
    }
    unsigned loaded = 0, stale = 0, toWarm = 0;
    double binarySecs = 0;  // the binaries' own time, to tell a slow driver
    const unsigned kTimed = 12;           // timed before deciding
    const double kSlow   = 0.04;          // seconds each: slower than compiling from source in parallel
    bool keep = false;  // the file is this driver's: append to it
    bool corrupt = false;
    if (FILE* in = fopen(s_cachePath.c_str(), "rb")) {
        char magic[8];
        uint64_t fileDriver = 0;
        uint32_t keySize = 0;
        if (fread(magic, 8, 1, in) == 1 && !memcmp(magic, kCacheMagic, 8) && fread(&fileDriver, 8, 1, in) == 1 &&
            fread(&keySize, 4, 1, in) == 1 && fileDriver == driver && keySize == sizeof(ProgramKey)) {
            keep = true;
            std::vector<uint8_t> bin;
            for (;;) {
                uint64_t srcHash;
                ProgramKey key;
                uint32_t format, len;
                int c = fgetc(in);
                if (c == EOF) break;  // the end, between records
                ungetc(c, in);
                // A record cut short, or one that is not a record: those after
                // it (and any appended to it) are out of step, so start over.
                if (fread(&srcHash, 8, 1, in) != 1 || fread(&key, sizeof key, 1, in) != 1 ||
                    fread(&format, 4, 1, in) != 1 || fread(&len, 4, 1, in) != 1 || len > (64u << 20) ||
                    !programKeyValid(key)) {
                    corrupt = true;
                    break;
                }
                bin.resize(len);
                if (len && fread(bin.data(), len, 1, in) != 1) {
                    corrupt = true;
                    break;
                }
                if (s_slowBinaries) {  // compiled from source by the warm-up instead (its source is today's then)
                    if (s_cacheFileKeys.insert(key).second) {
                        s_cacheKeysToWarm.push_back(key);
                        toWarm++;
                    }
                    continue;
                }
                if (s_programs.count(key) || sourceHash(genVS(key.k), genFS(key.k)) != srcHash) {
                    stale++;
                    continue;
                }
                if (!s_cacheFileKeys.insert(key).second) {  // the same key twice in the file: the first kept
                    stale++;
                    continue;
                }
                const auto b0 = std::chrono::steady_clock::now();
                GLuint p = glCreateProgram();
                bindAttributes(p);
                glProgramBinary(p, GLenum(format), bin.data(), GLsizei(len));
                GLint ok = 0;
                glGetProgramiv(p, GL_LINK_STATUS, &ok);
                binarySecs += std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
                if (!ok) {
                    glDeleteProgram(p);
                    stale++;
                    continue;
                }
                ShaderProgram& sp = s_programs[key];
                sp.id = 0;
                setupProgram(p, sp);
                loaded++;
                if (loaded == kTimed && binarySecs / kTimed > kSlow) {
                    s_slowBinaries = true;
                    logmsg("shader cache: this driver takes %.0f ms to load a program's binary: the rest are "
                           "compiled from source in the background instead", binarySecs / kTimed * 1000.0);
                }
            }
        }
        fclose(in);
    }
    if (stale > 64 && stale > loaded) keep = false;  // mostly unusable (repeated or rejected): start over
    if (corrupt) keep = false;
    s_cacheOut = fopen(s_cachePath.c_str(), keep ? "ab" : "wb");
    if (!s_cacheOut) return;
    // Unbuffered, so that each record (programCacheStore) is one append that
    // another game running at the same time cannot land in the middle of.
    setvbuf(s_cacheOut, nullptr, _IONBF, 0);
    if (!keep) {
        const uint32_t keySize = sizeof(ProgramKey);
        uint8_t head[20];
        memcpy(head, kCacheMagic, 8);
        memcpy(head + 8, &driver, 8);
        memcpy(head + 16, &keySize, 4);
        fwrite(head, sizeof head, 1, s_cacheOut);
    }
    s_cacheState = 1;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (keep)
        logmsg("shader cache: %u programs loaded in %.1f s, %u to compile from source, %u skipped (%s)", loaded, secs,
               toWarm, stale, s_cachePath.c_str());
    else if (corrupt) logmsg("shader cache: damaged after %u programs, made anew (%s)", loaded, s_cachePath.c_str());
    else logmsg("shader cache: new (%s)", s_cachePath.c_str());
}

static void programCacheStore(const ProgramKey& key, uint64_t srcHash, GLuint p) {
    if (!s_cacheOut) return;
    if (s_slowBinaries && !s_cacheFileKeys.insert(key).second) return;  // in the file already (its key is what counts)
    GLint len = 0;
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return;
    // The record in one buffer: srcHash, key, format, length, binary.
    const size_t head = 8 + sizeof key + 4 + 4;
    std::vector<uint8_t> rec(head + static_cast<size_t>(len));
    GLenum format = 0;
    GLsizei got = 0;
    glGetProgramBinary(p, len, &got, &format, rec.data() + head);
    if (got <= 0) return;
    const uint32_t fmt = format, n = uint32_t(got);
    memcpy(rec.data(), &srcHash, 8);
    memcpy(rec.data() + 8, &key, sizeof key);
    memcpy(rec.data() + 8 + sizeof key, &fmt, 4);
    memcpy(rec.data() + 8 + sizeof key + 4, &n, 4);
    fwrite(rec.data(), head + n, 1, s_cacheOut);
}

// ------------------------------------------------------------------ warm-up
// A program is otherwise compiled when a draw first needs it, and a stage's
// arrival needs dozens: the stutter of its first seconds. The program cache
// makes that once per computer; the warm-up makes it before gameplay, from
// the program keys the game's stages are known to draw with
// (gx_shader_keys.inc, made by tools/gx/shader_keys.py from SMS_GX_SHADER_KEYS
// recordings and kept in the key layout kShaderKeyLayout). While a stage loads
// (shaderPrepareStage), its own programs are compiled, then the other known
// ones for up to a second (a quarter without GL_KHR/ARB_parallel_shader_compile,
// which compiles them together), so a computer's first loads make the lot.
// Nothing is compiled between frames: with the driver compiling, the GPU
// fences frames wait for (EFB copies) are held up for whole compiles.
// Programs the cache already holds cost nothing. SMS_GX_SHADER_WARMUP=0 turns
// it off, and the settings menu does either from the next stage's load.
#include "gx_shader_keys.inc"

static const int kStageBoot = 0xFE, kStageAny = 0xFF;
static int s_stage = kStageBoot;  // the stage loaded last (shaderPrepareStage)
static int s_warmState = -1;      // -1 not set up, 0 unusable here, 1 its tables are loaded
static bool s_warmWanted = true;  // SMS_GX_SHADER_WARMUP, then the settings menu
static bool s_parallel = false;   // the driver compiles in the background
static std::vector<ProgramKey> s_warmKeys;                    // each once
static std::unordered_map<int, std::vector<size_t>> s_stageKeys;  // a stage's, in s_warmKeys
static std::unordered_set<ProgramKey, ProgramKeyHash> s_warmIssued;
static size_t s_warmNext = 0;        // the next of the others to compile
static uint32_t s_warmMade = 0;

// On the GL thread, while the game's thread waits for it: the compiles in
// flight, and the programs finished for the game's thread to take (warmDrain).
struct WarmJob {
    ProgramKey key;
    std::string vs, fs;
    GLuint prog = 0, v = 0, f = 0;
};
struct Warmed {
    ProgramKey key;
    ShaderProgram sp;
    bool ok;
};
static std::vector<WarmJob*> s_inFlight;
static std::vector<Warmed> s_warmed;

static GLuint startCompile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    return sh;
}

static void warmIssue(WarmJob* j) {  // GL thread; with parallel compiles, returns before they end
    j->v = startCompile(GL_VERTEX_SHADER, j->vs);
    j->f = startCompile(GL_FRAGMENT_SHADER, j->fs);
    j->prog = glCreateProgram();
    glAttachShader(j->prog, j->v);
    glAttachShader(j->prog, j->f);
    bindAttributes(j->prog);
    glLinkProgram(j->prog);
    s_inFlight.push_back(j);
}

static void warmFinish(WarmJob* j) {  // GL thread
    GLint ok = 0;
    glGetProgramiv(j->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(j->prog, sizeof log, nullptr, log);
        logmsg("shader warm-up: program link failed: %s", log);
        glDeleteProgram(j->prog);
    }
    glDeleteShader(j->v);
    glDeleteShader(j->f);
    Warmed w{j->key, ShaderProgram(), ok != 0};
    if (ok) {
        setupProgram(j->prog, w.sp, true);
        w.sp.id = 0;
        programCacheStore(j->key, sourceHash(j->vs, j->fs), j->prog);
    }
    s_warmed.push_back(w);
    delete j;
}

// GL thread: waits for the compiles in flight and takes them.
static void warmPoll() {
    for (WarmJob* j : s_inFlight) warmFinish(j);
    s_inFlight.clear();
}

// Game thread: queues a compile of `key` for the GL thread.
static void warmPost(const ProgramKey& key) {
    if (!s_warmIssued.insert(key).second) return;
    WarmJob* j = new WarmJob{key, genVS(key.k), genFS(key.k)};
    struct Issue {
        WarmJob* j;
        void operator()() const { warmIssue(j); }
    };
    glt::post(Issue{j});
}

// Game thread: takes the finished programs.
static void warmDrain() {
    std::vector<Warmed> got;
    got.swap(s_warmed);
    for (Warmed& w : got) {
        if (!w.ok) continue;
        if (s_programs.count(w.key)) {  // a draw needed it first
            glDeleteProgram(w.sp.prog);
            continue;
        }
        s_programs.emplace(w.key, w.sp);
        s_warmMade++;
    }
}

static void warmSetup() {
    s_warmState = 0;
    const char* e = getenv("SMS_GX_SHADER_WARMUP");
    s_warmWanted = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));  // off still loads the tables, for the menu
    if (kWarmLayout != kShaderKeyLayout || kWarmKeySize != sizeof(ProgramKey) || kWarmFormat != 2) {
        logmsg("shader warm-up: off (gx_shader_keys.inc is for key layout %u of %u bytes, not %u of %zu)",
               unsigned(kWarmLayout), unsigned(kWarmKeySize), unsigned(kShaderKeyLayout), sizeof(ProgramKey));
        return;
    }
    // The programs (each its count of nonzero words, then each as its index
    // and its value, LEB128), then each stage's (its number, its count, their
    // indices; 16 bits each but the number); tools/gx/shader_keys.py.
    const bool early = shaderHasEarlyFragmentTests();
    const uint8_t* p = kWarmKeys;
    const uint8_t* const end = kWarmKeys + sizeof kWarmKeys;
    auto u16 = [&] { uint32_t v = p + 2 <= end ? uint32_t(p[0]) | uint32_t(p[1]) << 8 : 0; p += 2; return v; };
    const uint32_t count = u16();
    for (uint32_t k = 0; k < count && p < end; k++) {
        uint32_t words[sizeof(ProgramKey) / 4] = {};
        for (int n = *p++; n > 0 && p < end; n--) {
            const uint32_t i = *p++;
            uint32_t v = 0;
            for (int shift = 0; p < end; shift += 7) {
                v |= uint32_t(*p & 0x7F) << shift;
                if (!(*p++ & 0x80)) break;
            }
            if (i < sizeof(ProgramKey) / 4) words[i] = v;
        }
        ProgramKey key;
        memcpy(&key, words, sizeof key);
        if (!early) key.k.earlyZ = 0;  // as buildKey makes it on this driver
        s_warmKeys.push_back(key);
    }
    while (p + 3 <= end) {
        const int stage = *p++;
        std::vector<size_t>& mine = s_stageKeys[stage];
        for (uint32_t n = u16(); n > 0 && p + 2 <= end; n--) {
            const uint32_t i = u16();
            if (i < s_warmKeys.size()) mine.push_back(i);
        }
    }
    glt::sync([] {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; i++) {
            const char* x = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
            if (x && (!strcmp(x, "GL_KHR_parallel_shader_compile") || !strcmp(x, "GL_ARB_parallel_shader_compile")))
                s_parallel = true;
        }
        if (s_parallel && glMaxShaderCompilerThreadsKHR) glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
    });
    if (getenv("SMS_GX_SHADER_WARMUP_SERIAL")) s_parallel = false;  // as on drivers without it (tests)
    // a slow driver's cached programs (programCacheLoad): compiled from source with the others
    if (!s_cacheKeysToWarm.empty()) {
        std::unordered_set<ProgramKey, ProgramKeyHash> have(s_warmKeys.begin(), s_warmKeys.end());
        for (const ProgramKey& k : s_cacheKeysToWarm)
            if (have.insert(k).second) s_warmKeys.push_back(k);
        std::vector<ProgramKey>().swap(s_cacheKeysToWarm);
    }
    size_t cached = 0;
    for (const ProgramKey& k : s_warmKeys) cached += s_programs.count(k);
    s_warmState = 1;
    if (s_warmWanted)
        logmsg("shader warm-up: %zu known programs, %zu cached%s", s_warmKeys.size(), cached,
               s_parallel ? ", compiled in parallel" : "");
}

// SMS_GX_SHADER_KEYS=file appends each program key a stage draws with, once
// per stage and session, as a line "<stage> <word>=<hex value> ..." of its
// nonzero words (tools/gx/shader_keys.py turns them into gx_shader_keys.inc).
static void recordKey(const ProgramKey& key) {
    static FILE* out = nullptr;
    static int on = -1;
    static std::unordered_set<uint64_t> seen;
    if (on < 0) {
        const char* e = getenv("SMS_GX_SHADER_KEYS");
        out = e && *e ? fopen(e, "a") : nullptr;
        on = out != nullptr;
    }
    if (!on || !seen.insert(hashBytes(&key, sizeof key, uint64_t(s_stage) + 1)).second) return;
    uint32_t words[sizeof(ProgramKey) / 4];
    memcpy(words, &key, sizeof key);
    fprintf(out, "%d", s_stage);
    for (size_t i = 0; i < sizeof(ProgramKey) / 4; i++)
        if (words[i]) fprintf(out, " %zu=%x", i, words[i]);
    fputc('\n', out);
    fflush(out);
}

static void shaderSetup() {
    glt::sync([] { programCacheLoad(); });
    warmSetup();
}

// While a stage loads (before its first frame): its known programs, compiled
// together (in parallel where the driver can), then others while time allows.
void shaderPrepareStage(int stage) {
    if (s_cacheState < 0) shaderSetup();
    s_stage = stage & 0xFF;
    s_lastProgram = nullptr;  // so the stage's first key is recorded
    if (s_warmState != 1 || !s_warmWanted) return;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    auto finish = [] {
        glt::sync([] { warmPoll(); });
        warmDrain();
    };
    const uint32_t before = s_warmMade;
    auto it = s_stageKeys.find(s_stage);
    if (it != s_stageKeys.end())
        for (size_t i : it->second)
            if (!s_programs.count(s_warmKeys[i])) warmPost(s_warmKeys[i]);
    finish();
    const uint32_t own = s_warmMade - before;
    const double budget = s_parallel ? 1000 : 250;
    const int batch = s_parallel ? 32 : 1;
    while (s_warmNext < s_warmKeys.size() && elapsed() < budget) {
        for (int n = 0; n < batch && s_warmNext < s_warmKeys.size();) {
            const ProgramKey& k = s_warmKeys[s_warmNext++];
            if (s_programs.count(k) || s_warmIssued.count(k)) continue;
            warmPost(k);
            n++;
        }
        finish();
    }
    if (s_warmMade != before)
        logmsg("shader warm-up: compiled %u programs for stage %d and %u others in %.1f ms%s", own, s_stage,
               s_warmMade - before - own, elapsed(), s_warmNext < s_warmKeys.size() ? "" : " (all known programs made)");
}

bool shaderWarmupAvailable() { return s_warmState == 1; }
bool shaderWarmupWanted() { return s_warmWanted; }
void shaderSetWarmup(bool on) { s_warmWanted = on; }

// ------------------------------------------------------------------ lines
// GX draws a line as a quad GXSetLineWidth sixths of an EFB pixel wide,
// widened along the minor axis. GL's own lines are one pixel of the scaled
// EFB, and wider ones are missing from forward-compatible core contexts
// (macOS), so 1-pixel lines at a 4x EFB leave three rows in four uncovered:
// the iris wipes' black field (one line per row) turned into a translucent
// grey stripe pattern. Line batches draw through the same program with a
// geometry shader that makes the quads; only a few 2D draws (the wipes) use
// lines, so these are kept apart from s_programs, the program cache and the
// warm-up's keys, and compiled when first drawn.
static const char* const kLineVaryings[][2] = {
    {"vec4", "c0"},  {"vec4", "c1"},  {"vec3", "tc0"}, {"vec3", "tc1"}, {"vec3", "tc2"},
    {"vec3", "tc3"}, {"vec3", "tc4"}, {"vec3", "tc5"}, {"vec3", "tc6"}, {"vec3", "tc7"},
};

// The vertex shader's outputs renamed (v_ to vl_), as the geometry shader's inputs.
static std::string genLineVS(const ShaderKey& k) {
    std::string vs = genVS(k), defs;
    for (const auto& v : kLineVaryings) defs += std::string("#define v_") + v[1] + " vl_" + v[1] + "\n";
    return vs.insert(vs.find('\n') + 1, defs);
}

static std::string genLineGS() {
    std::string s = "#version 330 core\n"
                    "layout(lines) in;\n"
                    "layout(triangle_strip, max_vertices = 4) out;\n"
                    "uniform vec2 u_efb;\n"
                    "uniform float u_linewidth;  // EFB pixels\n";
    for (const auto& v : kLineVaryings)
        s += std::string("in ") + v[0] + " vl_" + v[1] + "[]; out " + v[0] + " v_" + v[1] + ";\n";
    s += "void main() {\n"
         "  vec4 a = gl_in[0].gl_Position, b = gl_in[1].gl_Position;\n"
         "  vec2 d = (b.xy / max(abs(b.w), 1e-6) - a.xy / max(abs(a.w), 1e-6)) * u_efb;\n"
         "  vec2 off = abs(d.y) > abs(d.x) ? vec2(u_linewidth / u_efb.x, 0.0) : vec2(0.0, u_linewidth / u_efb.y);\n";
    // each end, then each side of it; constant indices, as gl_ClipDistance is unsized
    for (int i = 0; i < 4; i++) {
        char buf[256];
        const int e = i >> 1;
        snprintf(buf, sizeof buf,
                 "  gl_Position = gl_in[%d].gl_Position + vec4(%soff * gl_in[%d].gl_Position.w, 0.0, 0.0);\n"
                 "  gl_ClipDistance[0] = gl_in[%d].gl_ClipDistance[0];\n"
                 "  gl_ClipDistance[1] = gl_in[%d].gl_ClipDistance[1];\n"
                 "  gl_ClipDistance[2] = gl_in[%d].gl_ClipDistance[2];\n",
                 e, i & 1 ? "" : "-", e, e, e, e);
        s += buf;
        for (const auto& v : kLineVaryings) s += std::string("  v_") + v[1] + " = vl_" + v[1] + "[" + char('0' + e) + "];\n";
        s += "  EmitVertex();\n";
    }
    return s + "  EndPrimitive();\n}\n";
}

static std::unordered_map<ProgramKey, ShaderProgram, ProgramKeyHash> s_linePrograms;

static const ShaderProgram* lineProgram(const ProgramKey& key) {
    auto it = s_linePrograms.find(key);
    if (it != s_linePrograms.end()) return &it->second;
    g_statShaderCompiles++;
    std::string vs = genLineVS(key.k), gs = genLineGS(), fs = genFS(key.k);
    ShaderProgram sp;
    glt::sync([&] {
        GLuint v = compile(GL_VERTEX_SHADER, vs), gm = compile(GL_GEOMETRY_SHADER, gs),
               f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, gm);
        glAttachShader(p, f);
        bindAttributes(p);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(p, sizeof log, nullptr, log);
            logmsg("line program link failed: %s", log);
        }
        glDeleteShader(v);
        glDeleteShader(gm);
        glDeleteShader(f);
        sp.id = int(g_statShaderCompiles);
        setupProgram(p, sp);
    });
    return &(s_linePrograms[key] = sp);
}

const ShaderProgram* shaderForCurrentState(int depthOnly, bool lines) {
    if (s_cacheState < 0) shaderSetup();
    ProgramKey key;
    ShaderKey& k = key.k;
    buildKey(k);
    k.depthOnly = uint32_t(depthOnly);
    if (depthOnly == 0 && g.shadowTag) k.depthOnly = 3;  // Mario's own draw: his wet, sand
    // a character's draw (not a mirror's copy) while some are wet: Wet Characters
    else if (depthOnly == 0 && g.charDraw && g.wetChars[0] > 0.0f && !g.userClipOn) k.depthOnly = 4;
    else if (depthOnly == 0 && g.shellOn) k.depthOnly = 5;  // 3D Ladders: a layer of a ladder's card
    if (depthOnly == 2) k.earlyZ = 0;  // a shadow caster: its alpha test discards before depth
    if (depthOnly != 0 || lines)  // nothing to tint: their programs are as ever
        for (uint32_t st = 0; st < 16; st++) k.order[st] &= ~kOrderGoop;
    key.indScale[0] = k.numInd ? g.bp[BP_RAS1_SS0] : 0;
    key.indScale[1] = k.numInd ? g.bp[BP_RAS1_SS0 + 1] : 0;
    if (lines) return lineProgram(key);
    // consecutive batches mostly share a program
    if (s_lastProgram && key == s_lastKey) return s_lastProgram;
    recordKey(key);
    auto it = s_programs.find(key);
    if (it != s_programs.end()) {
        s_lastKey = key;
        return s_lastProgram = &it->second;
    }

    g_statShaderCompiles++;
    std::string vs = genVS(k), fs = genFS(k);
    ShaderProgram sp;
    // made on the GL thread in one go: each query below would otherwise
    // wait for it on its own (gx_glthread.h)
    glt::sync([&] {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        bindAttributes(p);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(p, sizeof log, nullptr, log);
            logmsg("program link failed: %s", log);
        }
        glDeleteShader(v);
        glDeleteShader(f);
        sp.id = int(g_statShaderCompiles);
        if (const char* dir = getenv("SMS_GX_DUMP_SHADERS")) {
            char path[1024];
            snprintf(path, sizeof path, "%s/prog%d.vs", dir, sp.id);
            if (FILE* f = fopen(path, "w")) { fputs(vs.c_str(), f); fclose(f); }
            snprintf(path, sizeof path, "%s/prog%d.fs", dir, sp.id);
            if (FILE* f = fopen(path, "w")) { fputs(fs.c_str(), f); fclose(f); }
        }
        setupProgram(p, sp);
        if (ok) programCacheStore(key, sourceHash(vs, fs), p);
    });
    s_lastKey = key;
    return s_lastProgram = &(s_programs[key] = sp);
}

// ------------------------------------------------------------------ 3D Grass
// The program for the blades of grass (GXPC_Grass*, gx_render.cpp): each blade an instance
// (a_blade: its root and height; a_blade2: its facing, its phase, its shade and keep), its
// triangles from gl_VertexID; swaying, pressed and coloured here, placed as the GX vertex shader
// places the game's vertices (the same projection and viewport uniforms), fogged as its fragments.
static const char* kGrassVs = R"(#version 330 core
uniform vec4 u_proj[2];
uniform vec4 u_vp[2];
uniform vec2 u_efb;
uniform vec4 u_gview[3];   // world to view, rows
uniform vec4 u_g[6];       // see GXPC_GrassBegin
uniform vec4 u_gp[64];     // what presses: x, y, z, radius
uniform vec4 u_gs[16];     // their strengths, four a vec4
uniform vec4 u_gtile;      // x: presses reach this tile; y: segments (1 or 2); z: 1, 2 the AO maps (w across)
uniform vec4 u_gtop;       // the AO map from above: its corner (x, z), 1 / its span
uniform vec4 u_uclip;      // the port's clip plane (a puddle's surface while its reflection is drawn)
uniform sampler2D u_gcolor; // the ground drawn so far: its colour (with mipmaps)...
uniform sampler2D u_gdepth; // ...and its depth
uniform sampler2D u_gflat;  // the flattened grass round Mario (r: how flat; g, b: which way)...
uniform vec4 u_gflatArea;   // ...its corner (x, z, the world's), whether there is one, 1 / its span
in vec4 a_blade;
in vec4 a_blade2;
out vec3 v_col;
const ivec2 kV1[3] = ivec2[3](ivec2(-1, 0), ivec2(1, 0), ivec2(0, 2));
const ivec2 kV2[9] = ivec2[9](ivec2(-1, 0), ivec2(1, 0), ivec2(-1, 1), ivec2(1, 0), ivec2(1, 1), ivec2(-1, 1),
                             ivec2(-1, 1), ivec2(1, 1), ivec2(0, 2));
void hide() {
  gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  gl_ClipDistance[0] = -1.0; gl_ClipDistance[1] = -1.0; gl_ClipDistance[2] = -1.0;
  v_col = vec3(0.0);
}
// clip space, as the GX vertex shader places a point (world to view, the projection, the viewport);
// s_gx: its two clip distances, as that shader gives them
vec2 s_gx;
vec3 s_vpos;
vec4 place(vec3 p) {
  vec4 wp = vec4(p, 1.0);
  vec3 vpos = vec3(dot(u_gview[0], wp), dot(u_gview[1], wp), dot(u_gview[2], wp));
  s_vpos = vpos;
  vec4 clip;
  if (u_proj[1].z == 0.0) {
    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y * vpos.z, u_proj[0].z * vpos.y + u_proj[0].w * vpos.z,
                u_proj[1].x * vpos.z + u_proj[1].y, -vpos.z);
  } else {
    clip = vec4(u_proj[0].x * vpos.x + u_proj[0].y, u_proj[0].z * vpos.y + u_proj[0].w,
                u_proj[1].x * vpos.z + u_proj[1].y, 1.0);
  }
  vec4 o;
  o.x = ((u_vp[1].x - 342.0) * clip.w + u_vp[0].x * clip.x) * (2.0 / u_efb.x) - clip.w;
  o.y = ((u_vp[1].y - 342.0) * clip.w + u_vp[0].y * clip.y) * (2.0 / u_efb.y) - clip.w;
  float zw = u_vp[1].z * clip.w + u_vp[0].z * clip.z;
  o.z = zw * (2.0 / 16777215.0) - clip.w;
  o.w = clip.w;
  s_gx = vec2(clip.z + clip.w, -clip.z);
  return o;
}
// What the camera drew, in the copy (its viewport, a pixel and a half in): lower left, upper right.
vec4 drawnRect() {
  vec2 mid = (u_vp[1].xy - 342.0) / u_efb.xy, hs = abs(u_vp[0].xy) / u_efb.xy;
  vec2 in1 = 1.5 / u_efb.xy;
  vec2 lo = max(mid - hs + in1, vec2(0.0)), hi = min(mid + hs - in1, vec2(1.0));
  return vec4(lo, hi);
}
// The ground's colour at a point on it, if it shows there (nothing drawn in front of it): w 1.
vec4 groundAt(vec3 p, vec3 eye) {
  vec4 c = place(p + vec3(0.0, 1.5, 0.0));
  if (c.w <= 0.0) return vec4(0.0);
  vec3 n = c.xyz / c.w;
  vec2 uv = n.xy * 0.5 + 0.5;
  // within what the camera drew (its viewport): the copy's rows and columns past it are not the
  // picture (a blade standing just below the bottom edge, its tip showing, would take their colour)
  vec4 r = drawnRect();
  if (any(lessThan(uv, r.xy)) || any(greaterThan(uv, r.zw))) return vec4(0.0);
  // in front of the ground by more than 20 units: something else is there
  vec3 toEye = eye - p;
  vec4 f = place(p + toEye * (20.0 / max(length(toEye), 1.0)));
  float front = f.z / f.w * 0.5 + 0.5;
  if (textureLod(u_gdepth, uv, 0.0).r < front) return vec4(0.0);
  return vec4(textureLod(u_gcolor, uv, 0.0).rgb, 1.0);
}
// A point just past the edge of the picture (a blade near the camera, below the bottom edge): the
// colour at the edge nearest it, if that is grass's green (else none): w 0.5.
vec4 groundNear(vec3 p) {
  vec4 c = place(p + vec3(0.0, 1.5, 0.0));
  if (c.w <= 0.0) return vec4(0.0);
  vec2 uv = c.xy / c.w * 0.5 + 0.5;
  vec4 r = drawnRect();
  vec2 e = clamp(uv, r.xy, r.zw);
  if (length(uv - e) > 0.25) return vec4(0.0);
  vec3 col = textureLod(u_gcolor, e, 0.0).rgb;
  if (col.g / max(max(col.r, col.b), 0.02) < 1.1) return vec4(0.0);
  return vec4(col, 0.5);
}
void main() {
  vec3 bp = a_blade.xyz;
  float h = a_blade.w * u_g[0].w;  // Cut / Wild Grass Height
  float d = length(bp.xz - u_g[3].xz);
  float reach = u_g[2].w;
  float shadeB = a_blade2.w - floor(a_blade2.w / 256.0) * 256.0;
  if (d > reach) { hide(); return; }
  float outv = d / reach;
  if (outv > 0.85) h *= (1.0 - outv) / 0.15;
  // the ground's colour under it (or beside it, where something hides it)
  vec3 eye = -(u_gview[0].xyz * u_gview[0].w + u_gview[1].xyz * u_gview[1].w + u_gview[2].xyz * u_gview[2].w);
  vec4 ground = groundAt(bp, eye);
  if (ground.w == 0.0) ground = groundAt(bp + vec3(28.0, 0.0, 0.0), eye);
  if (ground.w == 0.0) ground = groundAt(bp + vec3(-20.0, 0.0, 20.0), eye);
  if (ground.w == 0.0) ground = groundAt(bp + vec3(-20.0, 0.0, -20.0), eye);
  if (ground.w == 0.0) ground = groundNear(bp);  // past the picture's edge: the edge's green
  if (ground.w > 0.75) {
    // grows only where the ground is green (bare earth and paths stay bare)
    float green = ground.g / max(max(ground.r, ground.b), 0.02);
    h *= smoothstep(0.9, 1.1, green);
  } else if (ground.w == 0.0) {
    // hidden all round: the kind's own colour, as bright as the picture is
    vec3 avg = textureLod(u_gcolor, vec2(0.5), 20.0).rgb;
    float lum = dot(avg, vec3(0.3, 0.59, 0.11));
    ground = vec4((u_g[1].rgb + u_g[2].rgb) * (0.5 / 255.0) * clamp(lum / 0.42, 0.35, 1.15), 1.0);
  }
  if (h < 0.5) { hide(); return; }
  // AO Near Grass's map (u_gtile.z 1): a flat disc round the root, u_gtile.w across, at the
  // ground; v_col: where in the disc, and the ground's view z
  if (u_gtile.z > 0.5) {
    const vec2 kQ[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 q = kQ[gl_VertexID % 6];
    // 2: from above (All But Characters): where round the blades, and the ground's height there
    // (the highest wins)
    if (u_gtile.z > 1.5) {
      vec2 m = (bp.xz + q * u_gtile.w - u_gtop.xy) * u_gtop.z;
      gl_Position = vec4(m * 2.0 - 1.0, clamp(-bp.y / 60000.0, -1.0, 1.0), 1.0);
      gl_ClipDistance[0] = 1.0;
      gl_ClipDistance[1] = 1.0;
      gl_ClipDistance[2] = 1.0;
      v_col = vec3(q, bp.y);
      return;
    }
    gl_Position = place(bp + vec3(q.x * u_gtile.w, 1.0, q.y * u_gtile.w));
    gl_ClipDistance[0] = s_gx.x;
    gl_ClipDistance[1] = s_gx.y;
    gl_ClipDistance[2] = dot(vec4(s_vpos, 1.0), u_uclip);
    v_col = vec3(q, s_vpos.z);
    return;
  }
  // the breeze
  float t = u_g[0].x;
  vec2 wind = u_g[4].xy;
  float ph = a_blade2.z * 6.2831853;
  float gust = 0.6 + 0.4 * sin(t * 0.7 + dot(bp.xz, wind) * 0.0035);
  float sway = (sin(t * 2.1 + ph + (bp.x + bp.z) * 0.01) * 0.6 + 0.4) * gust * u_g[0].y * h;
  vec2 tip = wind * sway;
  float down = 0.0;
  // pressed: away from what presses, and down
  if (u_gtile.x > 0.5) {
    int n = int(u_g[5].y);
    for (int i = 0; i < n; i++) {
      vec4 q = u_gp[i];
      vec2 pv = bp.xz - q.xz;
      float pd = length(pv);
      if (pd >= q.w || abs(q.y - bp.y) > 60.0) continue;
      float fr = pd / q.w;
      float amt = (1.0 - fr * fr) * u_gs[i >> 2][i & 3];  // flat across most of it
      tip += (pd > 0.01 ? pv / pd : vec2(0.0)) * amt * h * 0.9;
      down += amt * h * 0.55;
    }
  }
  // the flattened grass (FLUDD's spray, Mario's steps: port_grass.cpp's map)
  if (u_gflatArea.z > 0.5) {
    vec2 fuv = (bp.xz - u_gflatArea.xy) * u_gflatArea.w;
    if (all(greaterThan(fuv, vec2(0.0))) && all(lessThan(fuv, vec2(1.0)))) {
      vec4 f = textureLod(u_gflat, fuv, 0.0);
      if (f.r > 0.004) {
        vec2 dir = f.gb * 2.0 - 1.0;
        float dl = length(dir);
        dir = dl > 0.05 ? dir / dl : vec2(cos(ph), sin(ph));
        tip += dir * (f.r * h * 0.9);
        down += f.r * h * 0.55;
      }
    }
  }
  down = min(down, h * 0.8);
  // the vertex: which side, how high
  ivec2 vtx = u_gtile.y > 1.5 ? kV2[gl_VertexID % 9] : kV1[gl_VertexID % 3];
  vec2 across = a_blade2.xy * (u_g[0].z * 0.5);
  float topY = h - down;
  vec3 p;
  float level;
  if (vtx.y == 0) {
    p = vec3(bp.x + across.x * float(vtx.x), bp.y, bp.z + across.y * float(vtx.x));
    level = 0.0;
  } else if (vtx.y == 1) {
    p = vec3(bp.x + tip.x * 0.3 + across.x * 0.65 * float(vtx.x), bp.y + topY * 0.55,
             bp.z + tip.y * 0.3 + across.y * 0.65 * float(vtx.x));
    level = 0.5;
  } else {
    p = vec3(bp.x + tip.x, bp.y + topY, bp.z + tip.y);
    level = 1.0;
  }
  // colour: darker at the root; each blade its own; the mower's stripes; some dry wild blades
  float shade = 0.86 + 0.28 * (shadeB / 255.0);
  if (u_g[4].z > 0.5) shade *= (int(floor(dot(bp.xz, vec2(0.6, 0.8)) / 110.0)) & 1) != 0 ? 1.07 : 0.94;
  bool dry = u_g[4].w > 0.5 && mod(shadeB, 8.0) < 0.5;
  if (dry) shade *= 1.12;
  // the ground's colour: darker at the root (its own shade), lighter toward the tip (light through
  // the blade), a little greener
  vec3 baseC = ground.rgb * 0.78;
  vec3 tipC  = ground.rgb * 1.22 + vec3(-0.01, 0.025, -0.01);
  if (dry) tipC *= vec3(1.3, 1.05, 0.8);
  v_col = clamp(mix(baseC, tipC, level) * shade, 0.0, 1.0);
  // placed as the GX vertex shader places the game's
  gl_Position = place(p);
  gl_ClipDistance[0] = s_gx.x;
  gl_ClipDistance[1] = s_gx.y;
  gl_ClipDistance[2] = dot(vec4(s_vpos, 1.0), u_uclip);
}
)";
static const char* kGrassFs = R"(#version 330 core
in vec3 v_col;
uniform vec4 u_fog;
uniform vec4 u_fogcolor;
uniform vec4 u_g[6];
uniform vec4 u_gtile;
out vec4 o_color;
void main() {
  // AO Near Grass's map: the ground's view z, within the disc
  if (u_gtile.z > 0.5) {
    if (dot(v_col.xy, v_col.xy) > 1.0) discard;
    o_color = vec4(v_col.z, 0.0, 0.0, 1.0);
    return;
  }
  vec4 color = vec4(v_col, 1.0);
  int fogType = int(u_g[5].x);
  if (fogType != 0) {
    float ze = u_fog.w != 0.0 ? u_fog.x * gl_FragCoord.z : u_fog.x / (u_fog.y - gl_FragCoord.z);
    float f = clamp(ze - u_fog.z, 0.0, 1.0);
    if (fogType == 4) f = 1.0 - exp2(-8.0 * f);
    else if (fogType == 5) f = 1.0 - exp2(-8.0 * f * f);
    else if (fogType == 6) f = exp2(-8.0 * (1.0 - f));
    else if (fogType == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));
    color.rgb = mix(color.rgb, u_fogcolor.rgb, f);
  }
  o_color = color;
}
)";

static ShaderProgram s_grassProgram;
static bool s_grassMade = false;
static GrassUniforms s_grassUniforms;

const ShaderProgram* grassProgram(GrassUniforms* u) {
    if (!s_grassMade) {
        s_grassMade = true;
        glt::sync([&] {
            GLuint v = compile(GL_VERTEX_SHADER, kGrassVs), f = compile(GL_FRAGMENT_SHADER, kGrassFs);
            GLuint p = glCreateProgram();
            glAttachShader(p, v);
            glAttachShader(p, f);
            glBindAttribLocation(p, 0, "a_blade");
            glBindAttribLocation(p, 1, "a_blade2");
            glBindFragDataLocation(p, 0, "o_color");
            glLinkProgram(p);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[4096];
                glGetProgramInfoLog(p, sizeof log, nullptr, log);
                logmsg("grass program link failed: %s", log);
            }
            glDeleteShader(v);
            glDeleteShader(f);
            setupProgram(p, s_grassProgram);
            s_grassUniforms.view = glGetUniformLocation(p, "u_gview");
            s_grassUniforms.params = glGetUniformLocation(p, "u_g");
            s_grassUniforms.press = glGetUniformLocation(p, "u_gp");
            s_grassUniforms.strength = glGetUniformLocation(p, "u_gs");
            s_grassUniforms.tile = glGetUniformLocation(p, "u_gtile");
            s_grassUniforms.flatArea = glGetUniformLocation(p, "u_gflatArea");
            s_grassUniforms.top = glGetUniformLocation(p, "u_gtop");
            GLint gf = glGetUniformLocation(p, "u_gflat");
            if (gf >= 0) glUniform1i(gf, kGrassFlatUnit);
            GLint gc = glGetUniformLocation(p, "u_gcolor"), gd = glGetUniformLocation(p, "u_gdepth");
            if (gc >= 0) glUniform1i(gc, kGrassColorUnit);
            if (gd >= 0) glUniform1i(gd, kGrassDepthUnit);
        });
    }
    *u = s_grassUniforms;
    return &s_grassProgram;
}

void shaderShutdown() {
    glt::sync([] { warmPoll(); });
    warmDrain();
    s_warmState = -1;
    s_warmKeys.clear();
    s_stageKeys.clear();
    s_warmIssued.clear();
    s_warmNext = 0;
    s_parallel = false;
    if (s_cacheOut) fclose(s_cacheOut);
    s_cacheOut = nullptr;
    s_cacheState = -1;
    for (auto& kv : s_programs) glDeleteProgram(kv.second.prog);
    s_programs.clear();
    for (auto& kv : s_linePrograms) glDeleteProgram(kv.second.prog);
    s_linePrograms.clear();
    s_lastProgram = nullptr;
    s_earlyFragmentTests = -1;
    if (s_grassMade && s_grassProgram.prog) glDeleteProgram(s_grassProgram.prog);
    s_grassMade = false;
}

}  // namespace gx
