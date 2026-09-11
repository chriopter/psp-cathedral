/*
 * Lux Aeterna -- a Gothic stained-glass window for the PSP.
 *
 * Everything is made at start-up: the glass is "fired" procedurally (tracery,
 * lancets with medallions, a rose, lead cames, per-piece tint, streaks and
 * bubbles), the stone and marble are noise, and the church is built around it.
 *
 * What the GE is asked to do each frame:
 *   - hardware lighting: four lights (window glow, floor bounce, candles with
 *     specular, cool fill), attenuation, material colours
 *   - vertex fog for the depth of the nave
 *   - a planar reflection in the polished marble floor (mirrored model matrix)
 *   - texture projection: the glass is cast onto floor, walls and piers by
 *     the texture matrix (GU_TEXTURE_MATRIX + GU_POSITION), lit by N.L
 *   - colour doubling (GU_FRAGMENT_2X) so glass and light can over-expose
 *   - volumetric light shafts from 56 additive slices of the window
 *   - dust motes as 3D point sprites, coloured by the shaft they drift in
 *   - candle flames and halos as additive sprites
 *   - a hardware-tessellated Bezier vault, painted blue with gold stars
 *   - mip-mapped textures, alpha test, scissor, clip planes
 *   - bloom: render-to-texture, a bright pass by reverse-subtract blending,
 *     three downsampled levels with Kawase blur, additive composite
 *   - a multiplicative vignette, and a font with alpha blending for the HUD
 * Matrices go through libpspgum_vfpu, i.e. the VFPU.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

PSP_MODULE_INFO("LuxAeterna", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define PI 3.14159265f

/* ------------------------------------------------------------------ VRAM */
#define BUF_W 512
#define SCR_W 480
#define SCR_H 272
#define FB0 0x000000
#define FB1 0x088000
#define ZBUF 0x110000
#define RT_A 0x154000 /* 256x144 half res   */
#define RT_B 0x178000
#define RT_C 0x19C000 /* 128x72 quarter res */
#define RT_D 0x1A6000
#define RT_E 0x1B0000 /* 64x40 eighth res   */
#define RT_F 0x1B4000

static unsigned int __attribute__((aligned(64))) gulist[262144];
static void *drawBuf = (void *)FB0;
static inline void *vram(u32 off) { return (void *)((u32)sceGeEdramGetAddr() + off); }

/* ------------------------------------------------------------ callbacks */
static volatile int running = 1;
static int exit_cb(int a, int b, void *c) { running = 0; return 0; }
static int cb_thread(SceSize args, void *argp)
{
	int id = sceKernelCreateCallback("exit", exit_cb, NULL);
	sceKernelRegisterExitCallback(id);
	sceKernelSleepThreadCB();
	return 0;
}
static void setup_callbacks(void)
{
	int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
	if (th >= 0) sceKernelStartThread(th, 0, 0);
}

/* ----------------------------------------------------------------- math */
typedef struct { float x, y, z; } V3;
typedef struct { float r, g, b; } C3;
static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline V3 vnorm(V3 a) { float l = sqrtf(vdot(a, a)); return l > 1e-9f ? vmul(a, 1.0f / l) : a; }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static inline C3 c3(float r, float g, float b) { C3 c = {r, g, b}; return c; }
static inline C3 cmul(C3 a, float s) { return c3(a.r * s, a.g * s, a.b * s); }
static inline C3 cmix(C3 a, C3 b, float t) { return c3(mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)); }
static inline u32 pack(float r, float g, float b, float a)
{
	int R = (int)(clampf(r, 0, 1) * 255 + 0.5f), G = (int)(clampf(g, 0, 1) * 255 + 0.5f);
	int B = (int)(clampf(b, 0, 1) * 255 + 0.5f), A = (int)(clampf(a, 0, 1) * 255 + 0.5f);
	return ((u32)A << 24) | ((u32)B << 16) | ((u32)G << 8) | (u32)R;
}
static inline u32 packc(C3 c, float a) { return pack(c.r, c.g, c.b, a); }

static u32 rng = 0x2545F491u;
static inline float frand(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return (rng & 0xffffff) * (1.0f / 16777216.0f);
}

/* ---------------------------------------------------------------- noise */
static inline u32 ihash(int x, int y, int s)
{
	u32 h = (u32)x * 0x8da6b343u ^ (u32)y * 0xd8163841u ^ (u32)s * 0xcb1ab31fu;
	h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
	return h;
}
static inline float hf(int x, int y, int s) { return (ihash(x, y, s) & 0xffffff) * (1.0f / 16777216.0f); }
static float vnoise(float x, float y, int per, int seed)
{
	float fx = floorf(x), fy = floorf(y);
	int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1, y1 = y0 + 1;
	float tx = x - fx, ty = y - fy;
	tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
	if (per > 0) {
		x0 = ((x0 % per) + per) % per; x1 = ((x1 % per) + per) % per;
		y0 = ((y0 % per) + per) % per; y1 = ((y1 % per) + per) % per;
	}
	return mixf(mixf(hf(x0, y0, seed), hf(x1, y0, seed), tx), mixf(hf(x0, y1, seed), hf(x1, y1, seed), tx), ty);
}
static float fbm(float x, float y, int per, int oct, int seed)
{
	float s = 0, a = 0.5f, n = 0;
	for (int i = 0; i < oct; i++) {
		s += a * vnoise(x, y, per, seed + i); n += a;
		x *= 2; y *= 2; if (per) per *= 2; a *= 0.5f;
	}
	return s / n;
}

typedef struct { float edge; u32 id; } Vor;
static inline void vor_pt(int gx, int gy, float *fx, float *fy)
{
	*fx = gx + 0.12f + 0.76f * hf(gx, gy, 11);
	*fy = gy + 0.12f + 0.76f * hf(gx, gy, 12);
}
static Vor voronoi(float x, float y, float cell)
{
	float px = x / cell, py = y / cell;
	int cx = (int)floorf(px), cy = (int)floorf(py), bi = 0, bj = 0;
	float best = 1e9f, bx = 0, by = 0;
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++) {
			float fx, fy; vor_pt(cx + i, cy + j, &fx, &fy);
			float d = (fx - px) * (fx - px) + (fy - py) * (fy - py);
			if (d < best) { best = d; bx = fx; by = fy; bi = cx + i; bj = cy + j; }
		}
	float edge = 1e9f;
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++) {
			int gx = bi + i, gy = bj + j;
			if (!i && !j) continue;
			float fx, fy; vor_pt(gx, gy, &fx, &fy);
			float mx = (fx + bx) * 0.5f - px, my = (fy + by) * 0.5f - py;
			float nx = fx - bx, ny = fy - by, nl = sqrtf(nx * nx + ny * ny);
			float d = (mx * nx + my * ny) / nl;
			if (d < edge) edge = d;
		}
	Vor v = {edge * cell, ihash(bi, bj, 13)};
	return v;
}

/* ----------------------------------------------------------------- pool */
static unsigned char __attribute__((aligned(64))) pool[2 * 1024 * 1024 + 512 * 1024];
static u32 poolUsed;
static void *palloc(u32 n)
{
	n = (n + 63) & ~63u;
	void *p = pool + poolUsed;
	poolUsed += n;
	return p;
}

/* ------------------------------------------------------------- textures */
typedef struct { int w, h, lv; u32 *px[5]; } Tex;
static Tex tGlass, tLight, tShaft, tWall, tFloor, tVault, tDot, tFlame, tVig, tFont;

static Tex new_tex(int w, int h, int lv)
{
	Tex t; t.w = w; t.h = h; t.lv = lv;
	for (int i = 0; i < lv; i++) t.px[i] = palloc((w >> i) * (h >> i) * 4);
	return t;
}
static void mip_build(Tex *t, int alphaCut)
{
	for (int l = 1; l < t->lv; l++) {
		int w = t->w >> l, h = t->h >> l, sw = t->w >> (l - 1);
		u32 *s = t->px[l - 1], *d = t->px[l];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				u32 p[4] = {s[(2 * y) * sw + 2 * x], s[(2 * y) * sw + 2 * x + 1], s[(2 * y + 1) * sw + 2 * x], s[(2 * y + 1) * sw + 2 * x + 1]};
				u32 acc[4] = {0, 0, 0, 0};
				for (int k = 0; k < 4; k++)
					for (int c = 0; c < 4; c++) acc[c] += (p[k] >> (8 * c)) & 255;
				u32 a = acc[3] / 4;
				if (alphaCut) a = a >= 128 ? 255 : 0;
				d[y * w + x] = (acc[0] / 4) | ((acc[1] / 4) << 8) | ((acc[2] / 4) << 16) | (a << 24);
			}
	}
}
static void bind(const Tex *t)
{
	sceGuTexMode(GU_PSM_8888, t->lv - 1, 0, 0);
	for (int i = 0; i < t->lv; i++) sceGuTexImage(i, t->w >> i, t->h >> i, t->w >> i, t->px[i]);
	sceGuTexFilter(t->lv > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR, GU_LINEAR);
	sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
}
static void bind_vram(u32 off, int w, int h, int stride)
{
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, w, h, stride, vram(off));
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexFlush();
}

/* ------------------------------------------------ the window, in window units
 * x runs -1..1 across, y 0..4 up; one unit is S_WIN metres in the church.  */
#define SQ3 1.7320508f
#define A0 0.985f
#define Y00 0.015f
#define YS0 (3.985f - A0 * SQ3)
#define BAND_O 0.075f
#define LCX 0.48f
#define AL 0.43f
#define YL0 0.09f
#define YSL 1.555f
#define BAND_L 0.05f
#define YC 3.089f
#define RR 0.62f
#define BAND_R 0.05f
#define BORDER 0.075f
#define LH 0.0055f
#define MR 0.26f
#define RING 0.034f
#define GSCALE 0.80f

#define S_WIN 1.8f
#define WIN_Y 2.0f
#define GLASS_Z (-0.55f)

enum { OUT = 0, STONE, LEAD, GLASS };

static const C3 BLUE = {0.10f, 0.27f, 0.85f}, DBLUE = {0.05f, 0.12f, 0.58f}, RUBY = {0.90f, 0.07f, 0.10f};
static const C3 GOLD = {1.00f, 0.74f, 0.14f}, AMBER = {1.00f, 0.48f, 0.06f}, GREEN = {0.10f, 0.66f, 0.30f};
static const C3 PURPLE = {0.52f, 0.14f, 0.66f}, WHITE = {0.95f, 0.93f, 0.80f}, PINK = {0.98f, 0.55f, 0.58f};
static const C3 SKY = {0.35f, 0.62f, 0.98f};

static const float MEDY[3] = {0.52f, 1.12f, 1.70f};
static const int SYM_L[3] = {0, 1, 2}, SYM_R[3] = {3, 4, 5};

/* signed distance to an equilateral pointed arch of half-width a */
static float sdArch(float x, float y, float a, float y0, float ys)
{
	if (y <= ys) {
		float dx = fabsf(x) - a, dy = y0 - y;
		return dx > dy ? dx : dy;
	}
	float d1 = sqrtf((x + a) * (x + a) + (y - ys) * (y - ys)) - 2 * a;
	float d2 = sqrtf((x - a) * (x - a) + (y - ys) * (y - ys)) - 2 * a;
	return d1 > d2 ? d1 : d2;
}
static inline float sdBox(float x, float y, float bx, float by)
{
	float dx = fabsf(x) - bx, dy = fabsf(y) - by;
	float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
	float in = dx > dy ? dx : dy;
	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0);
}
static inline float sdCirc(float x, float y, float r) { return sqrtf(x * x + y * y) - r; }

/* medallion symbols; *c holds the background colour on entry */
static int symbol(int s, float x, float y, C3 *c)
{
	float r = sqrtf(x * x + y * y), a = atan2f(y, x), d;
	switch (s) {
	case 0: { /* rosette */
		if (r < 0.045f) { if (r > 0.045f - 2 * LH) return LEAD; *c = GOLD; return GLASS; }
		float p = fabsf(cosf(3 * a));
		d = r - (0.06f + 0.13f * p);
		if (fabsf(d) < LH) return LEAD;
		if (d < 0) { if (p < 0.07f) return LEAD; *c = cmix(PINK, WHITE, p * p); }
		return GLASS;
	}
	case 1: { /* cross */
		d = fminf(sdBox(x, y + 0.02f, 0.035f, 0.17f), sdBox(x, y - 0.05f, 0.12f, 0.035f));
		if (fabsf(d) < LH) return LEAD;
		if (d < 0) {
			float dc = sdBox(x, y - 0.05f, 0.022f, 0.022f);
			if (fabsf(dc) < LH) return LEAD;
			*c = dc < 0 ? WHITE : GOLD;
		}
		return GLASS;
	}
	case 2: { /* eight-pointed star */
		float p = powf(fabsf(cosf(4 * a)), 5);
		d = r - (0.07f + 0.13f * p);
		if (fabsf(d) < LH) return LEAD;
		if (d < 0) {
			if (fabsf(r - 0.04f) < LH) return LEAD;
			*c = r < 0.04f ? WHITE : GOLD;
		}
		return GLASS;
	}
	case 3: { /* chalice and host */
		float host = sdCirc(x, y - 0.155f, 0.045f);
		if (fabsf(host) < LH) return LEAD;
		if (host < 0) { *c = WHITE; return GLASS; }
		float cup = fmaxf(sdCirc(x, y - 0.06f, 0.10f), y - 0.06f);
		float rim = sdBox(x, y - 0.065f, 0.11f, 0.012f);
		float stem = sdBox(x, y + 0.09f, 0.018f, 0.07f);
		float knob = sdCirc(x, y + 0.07f, 0.03f);
		float base = sdBox(x, y + 0.165f, 0.085f, 0.018f);
		d = fminf(fminf(cup, rim), fminf(fminf(stem, knob), base));
		if (fabsf(d) < LH) return LEAD;
		if (d < 0) *c = GOLD;
		return GLASS;
	}
	case 4: { /* crown */
		float band = sdBox(x, y + 0.07f, 0.13f, 0.03f);
		float body = sdBox(x, y, 0.12f, 0.06f);
		float notch = fminf(sdCirc(x - 0.06f, y - 0.07f, 0.045f), sdCirc(x + 0.06f, y - 0.07f, 0.045f));
		body = fmaxf(body, -notch);
		float pts = fminf(sdCirc(x, y - 0.09f, 0.028f), fminf(sdCirc(x - 0.12f, y - 0.075f, 0.024f), sdCirc(x + 0.12f, y - 0.075f, 0.024f)));
		d = fminf(fminf(band, body), pts);
		if (fabsf(d) < LH) return LEAD;
		if (d < 0) {
			for (int g = -1; g <= 1; g++) {
				float dg = sdCirc(x - g * 0.07f, y + 0.07f, 0.017f);
				if (fabsf(dg) < LH) return LEAD;
				if (dg < 0) { *c = g ? GREEN : WHITE; return GLASS; }
			}
			*c = GOLD;
		}
		return GLASS;
	}
	default: { /* sun */
		if (r < 0.07f) { if (r > 0.07f - 2 * LH) return LEAD; *c = GOLD; return GLASS; }
		float fa = (a + PI) * (16 / (2 * PI));
		float k = floorf(fa), fr = fa - k - 0.5f;
		float len = ((int)k & 1) ? 0.15f : 0.2f;
		float w = 0.42f * (1 - (r - 0.07f) / (len - 0.07f));
		if (r < len && fabsf(fr) < w) {
			if (fabsf(fr) > w - 0.07f) return LEAD;
			*c = ((int)k & 1) ? AMBER : GOLD;
		}
		return GLASS;
	}
	}
}

static int lancet(float x, float y, float e, int side, C3 *c)
{
	if (e < BORDER) { /* border strip: red and blue with white pearls */
		if (e < LH || e > BORDER - LH) return LEAD;
		float s = y * 9.0f, fs = s - floorf(s);
		float dp = sqrtf((e - BORDER * 0.5f) * (e - BORDER * 0.5f) + (fs - 0.5f) * (fs - 0.5f) / 81.0f);
		if (dp < 0.016f) { *c = WHITE; return GLASS; }
		if (dp < 0.016f + 2 * LH) return LEAD;
		if ((fs < 0.5f ? fs : 1 - fs) / 9.0f < LH) return LEAD;
		*c = (((int)floorf(s)) & 1) ? RUBY : DBLUE;
		return GLASS;
	}
	for (int m = 0; m < 3; m++) {
		float dy = y - MEDY[m];
		float dm = sqrtf(x * x + dy * dy) - MR;
		if (dm >= 0) continue;
		if (dm > -2 * LH || (dm < -RING && dm > -RING - 2 * LH)) return LEAD;
		if (dm > -RING) {
			float fa = (atan2f(dy, x) + PI) * (18 / (2 * PI));
			float f = fa - floorf(fa);
			if (f < 0.07f || f > 0.93f) return LEAD;
			*c = (((int)fa) & 1) ? WHITE : GOLD;
			return GLASS;
		}
		*c = m == 1 ? RUBY : (m == 2 ? GREEN : BLUE);
		return symbol(side ? SYM_R[m] : SYM_L[m], x, dy, c);
	}
	/* diaper lattice with ruby gems at the nodes */
	float u = (x + y) * (1 / 0.13f), v = (x - y) * (1 / 0.13f);
	float fu = u - floorf(u), fv = v - floorf(v);
	float du = fu < 0.5f ? fu : 1 - fu, dv = fv < 0.5f ? fv : 1 - fv;
	float g = sqrtf(du * du + dv * dv);
	if (g < 0.24f) { *c = (((int)floorf(u) + (int)floorf(v)) & 2) ? GOLD : RUBY; return GLASS; }
	if (g < 0.30f) return LEAD;
	if (du < 0.06f || dv < 0.06f) return LEAD;
	*c = (((int)floorf(u) + (int)floorf(v)) & 1) ? BLUE : DBLUE;
	return GLASS;
}

static int rose(float x, float y, C3 *c)
{
	float r = sqrtf(x * x + y * y), a = atan2f(y, x);
	if (r < 0.10f) {
		if (r > 0.10f - 2 * LH) return LEAD;
		if (r < 0.03f) { *c = WHITE; return GLASS; }
		if (r < 0.03f + 2 * LH) return LEAD;
		if (fabsf(sinf(4 * a)) * r < LH) return LEAD;
		*c = (((int)floorf((a + PI) / (PI / 4))) & 1) ? GOLD : AMBER;
		return GLASS;
	}
	if (r < 0.17f) {
		float fa = (a + PI) * (12 / (2 * PI));
		float f = fa - floorf(fa) - 0.5f;
		float dp = sqrtf((r - 0.135f) * (r - 0.135f) + (f * 2 * PI / 12 * r) * (f * 2 * PI / 12 * r));
		if (dp < 0.017f) { *c = WHITE; return GLASS; }
		if (dp < 0.017f + 2 * LH) return LEAD;
		if (r > 0.17f - 2 * LH) return LEAD;
		*c = RUBY;
		return GLASS;
	}
	if (r < 0.19f) return STONE;
	float sw = 2 * PI / 12;
	int k = (int)floorf((a + PI) / sw);
	float ac = -PI + (k + 0.5f) * sw, dl = a - ac;
	if ((sw * 0.5f - fabsf(dl)) * r < 0.014f) return STONE;
	float lx = 0.43f * cosf(ac), ly = 0.43f * sinf(ac);
	float dlobe = sdCirc(x - lx, y - ly, 0.085f);
	if (dlobe < 0) {
		if (dlobe > -0.016f) return STONE;
		float dc = sdCirc(x - lx, y - ly, 0.028f);
		if (fabsf(dc) < LH) return LEAD;
		*c = dc < 0 ? WHITE : ((k & 1) ? GREEN : GOLD);
		return GLASS;
	}
	if (r > 0.43f) {
		if (fabsf(dl) * r < LH) return LEAD;
		*c = (k & 1) ? PURPLE : SKY;
		return GLASS;
	}
	float dp = sqrtf((r - 0.265f) * (r - 0.265f) + (dl * r) * (dl * r));
	if (dp < 0.02f) { *c = WHITE; return GLASS; }
	if (dp < 0.02f + 2 * LH) return LEAD;
	if (fabsf(dl) * r < LH) return LEAD;
	*c = (k & 1) ? BLUE : RUBY;
	return GLASS;
}

static int quatrefoil(float x, float y, float cx, float cy, float s, C3 *c)
{
	x -= cx; y -= cy;
	float h = s * 0.5f, rr = s * 0.55f;
	float d = fminf(fminf(sdCirc(x - h, y, rr), sdCirc(x + h, y, rr)), fminf(sdCirc(x, y - h, rr), sdCirc(x, y + h, rr)));
	if (d >= 0) return 0;
	if (d > -0.016f) return STONE;
	float dc = sdCirc(x, y, s * 0.32f);
	if (fabsf(dc) < LH) return LEAD;
	*c = dc < 0 ? GOLD : RUBY;
	return GLASS;
}

static int glass_class(float X, float Y, C3 *c)
{
	float dO = sdArch(X, Y, A0, Y00, YS0);
	if (dO > 0) return OUT;
	if (dO > -BAND_O) return STONE;
	float dl = sdArch(X + LCX, Y, AL, YL0, YSL);
	if (dl < 0) return dl > -BAND_L ? STONE : lancet(X + LCX, Y, -dl - BAND_L, 0, c);
	float dr = sdArch(X - LCX, Y, AL, YL0, YSL);
	if (dr < 0) return dr > -BAND_L ? STONE : lancet(X - LCX, Y, -dr - BAND_L, 1, c);
	float dro = sdCirc(X, Y - YC, RR);
	if (dro < 0) return dro > -BAND_R ? STONE : rose(X, Y - YC, c);
	int q = quatrefoil(X, Y, 0, 2.14f, 0.11f, c);
	if (!q) q = quatrefoil(X, Y, -0.79f, 2.45f, 0.07f, c);
	if (!q) q = quatrefoil(X, Y, 0.79f, 2.45f, 0.07f, c);
	return q ? q : STONE;
}

/* cut the glass into pieces, tint each, add streaks, bevel and bubbles */
static int glass_finish(float X, float Y, C3 *c)
{
	Vor v = voronoi(X, Y, 0.085f);
	if (v.edge < LH * 0.75f) return LEAD;
	float h1 = (v.id & 255) / 255.0f, h2 = ((v.id >> 8) & 255) / 255.0f, h3 = ((v.id >> 16) & 255) / 255.0f;
	float br = 0.80f + 0.34f * h1;
	float streak = 0.86f + 0.26f * vnoise(X * 5.0f + h2 * 7, Y * 48.0f, 0, 3);
	float bevel = 0.70f + 0.30f * smooth(0.0f, 0.032f, v.edge);
	float k = br * streak * bevel;
	c->r *= k * (0.92f + 0.16f * h2);
	c->g *= k;
	c->b *= k * (0.92f + 0.16f * h3);
	if (hf((int)(X * 420), (int)(Y * 420), 7) > 0.996f) *c = cmul(*c, 1.35f);
	return GLASS;
}

static float lightAcc[128 * 256 * 3];
static float shaftF[64 * 128 * 3];
static C3 avgGlass;

static void blur121(float *img, int w, int h, int passes)
{
	static float tmp[128 * 256 * 3];
	for (int p = 0; p < passes; p++) {
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int k = 0; k < 3; k++) {
					int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : w - 1;
					tmp[(y * w + x) * 3 + k] = 0.25f * img[(y * w + xl) * 3 + k] + 0.5f * img[(y * w + x) * 3 + k] + 0.25f * img[(y * w + xr) * 3 + k];
				}
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int k = 0; k < 3; k++) {
					int yu = y > 0 ? y - 1 : 0, yd = y < h - 1 ? y + 1 : h - 1;
					img[(y * w + x) * 3 + k] = 0.25f * tmp[(yu * w + x) * 3 + k] + 0.5f * tmp[(y * w + x) * 3 + k] + 0.25f * tmp[(yd * w + x) * 3 + k];
				}
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				img[(y * w + x) * 3] = img[(y * w + x) * 3 + 1] = img[(y * w + x) * 3 + 2] = 0;
}

static void make_glass(void)
{
	tGlass = new_tex(256, 512, 3);
	memset(lightAcc, 0, sizeof(lightAcc));
	double sr = 0, sg = 0, sb = 0; int ng = 0;
	for (int j = 0; j < 512; j++)
		for (int i = 0; i < 256; i++) {
			float ar = 0, ag = 0, ab = 0; int in = 0;
			float *la = &lightAcc[((j >> 1) * 128 + (i >> 1)) * 3];
			for (int s = 0; s < 4; s++) {
				float X = (i + 0.25f + 0.5f * (s & 1)) / 128.0f - 1.0f;
				float Y = 4.0f - (j + 0.25f + 0.5f * (s >> 1)) / 128.0f;
				C3 c = {0, 0, 0};
				int cls = glass_class(X, Y, &c);
				if (cls == OUT) continue;
				in++;
				if (cls == GLASS) cls = glass_finish(X, Y, &c);
				if (cls == GLASS) {
					ar += c.r * GSCALE; ag += c.g * GSCALE; ab += c.b * GSCALE;
					la[0] += c.r * (1 / 16.0f); la[1] += c.g * (1 / 16.0f); la[2] += c.b * (1 / 16.0f);
					sr += c.r; sg += c.g; sb += c.b; ng++;
				} else if (cls == STONE) {
					float n = 0.75f + 0.5f * vnoise(X * 70, Y * 70, 0, 5);
					ar += 0.050f * n; ag += 0.044f * n; ab += 0.038f * n;
				} else {
					ar += 0.012f; ag += 0.012f; ab += 0.013f;
				}
			}
			float inv = in ? 1.0f / in : 0;
			tGlass.px[0][j * 256 + i] = pack(ar * inv, ag * inv, ab * inv, in >= 2 ? 1.0f : 0.0f);
		}
	mip_build(&tGlass, 1);
	avgGlass = c3(sr / ng, sg / ng, sb / ng);

	blur121(lightAcc, 128, 256, 2);
	tLight = new_tex(128, 256, 1);
	for (int k = 0; k < 128 * 256; k++)
		tLight.px[0][k] = pack(lightAcc[k * 3], lightAcc[k * 3 + 1], lightAcc[k * 3 + 2], 1);

	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 64; x++)
			for (int k = 0; k < 3; k++)
				shaftF[(y * 64 + x) * 3 + k] = 0.25f * (lightAcc[((2 * y) * 128 + 2 * x) * 3 + k] + lightAcc[((2 * y) * 128 + 2 * x + 1) * 3 + k] +
				                                        lightAcc[((2 * y + 1) * 128 + 2 * x) * 3 + k] + lightAcc[((2 * y + 1) * 128 + 2 * x + 1) * 3 + k]);
	blur121(shaftF, 64, 128, 2);
	tShaft = new_tex(64, 128, 1);
	for (int k = 0; k < 64 * 128; k++)
		tShaft.px[0][k] = pack(shaftF[k * 3], shaftF[k * 3 + 1], shaftF[k * 3 + 2], 1);
}

static C3 sample_light(float u, float v)
{
	if (u <= 0 || u >= 1 || v <= 0 || v >= 1) return c3(0, 0, 0);
	int x = (int)(u * 64), y = (int)(v * 128);
	const float *p = &shaftF[(y * 64 + x) * 3];
	return c3(p[0], p[1], p[2]);
}

/* ------------------------------------------------------- other textures */
static void make_wall(void)
{
	tWall = new_tex(128, 128, 4);
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			int row = y / 32, bx = (x + (row & 1) * 32) & 127, col = bx / 64;
			int lx = bx % 64, ly = y % 32;
			float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 31 - ly));
			float h = hf(col, row, 21);
			float n = fbm(x / 16.0f, y / 16.0f, 8, 4, 22);
			C3 c = cmul(c3(0.52f, 0.48f, 0.42f), (0.90f + 0.14f * h) * (0.78f + 0.40f * n));
			if (edge < 0.8f) c = cmul(c3(0.26f, 0.24f, 0.21f), 0.85f + 0.3f * n);
			else if (edge < 3.5f) c = cmul(c, 0.90f + 0.10f * (edge - 0.8f) / 2.7f);
			tWall.px[0][y * 128 + x] = packc(c, 1);
		}
	mip_build(&tWall, 0);
}

static void make_floor(void)
{
	tFloor = new_tex(128, 128, 4);
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			int tile = ((x / 64) + (y / 64)) & 1;
			int lx = x % 64, ly = y % 64;
			float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 63 - ly));
			float n = fbm(x / 32.0f, y / 32.0f, 4, 5, 31 + tile * 7);
			float s = sinf(2 * PI * (x / 128.0f + y / 128.0f) + n * 9.0f);
			float vein = powf(1 - fabsf(s), 14);
			C3 c; float a;
			if (!tile) { c = cmul(c3(0.80f, 0.76f, 0.68f), 0.88f + 0.18f * n); c = cmix(c, c3(0.45f, 0.42f, 0.40f), vein * 0.5f); a = 0.86f; }
			else { c = cmul(c3(0.16f, 0.17f, 0.19f), 0.85f + 0.3f * n); c = cmix(c, c3(0.55f, 0.55f, 0.52f), vein * 0.4f); a = 0.74f; }
			if (edge < 1.0f) c = c3(0.07f, 0.07f, 0.07f);
			tFloor.px[0][y * 128 + x] = packc(c, a);
		}
	mip_build(&tFloor, 0);
}

static void make_vault(void)
{
	tVault = new_tex(128, 128, 4);
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			float n = fbm(x / 16.0f, y / 16.0f, 8, 4, 41);
			C3 c = cmul(c3(0.07f, 0.10f, 0.32f), 0.75f + 0.45f * n);
			int cx = x / 32, cy = y / 32;
			float sx = cx * 32 + 9 + 14 * hf(cx, cy, 42), sy = cy * 32 + 9 + 14 * hf(cx, cy, 43);
			float dx = x - sx, dy = y - sy, r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
			float rs = 4.2f * (0.42f + 0.58f * powf(fabsf(cosf(2.5f * a)), 3));
			if (r < rs) c = cmix(c3(0.95f, 0.74f, 0.30f), c, smooth(rs - 1.0f, rs, r));
			tVault.px[0][y * 128 + x] = packc(c, 1);
		}
	mip_build(&tVault, 0);
}

static void make_sprites(void)
{
	tDot = new_tex(32, 32, 1);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			float dx = (x - 15.5f) / 16, dy = (y - 15.5f) / 16, r2 = dx * dx + dy * dy;
			float g = expf(-r2 * 6.0f) * (1 - smooth(0.8f, 1.0f, sqrtf(r2)));
			tDot.px[0][y * 32 + x] = pack(g, g, g, g);
		}
	tFlame = new_tex(32, 64, 1);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 32; x++) {
			float fy = (y + 0.5f) / 64.0f; /* 0 tip .. 1 base */
			float fx = (x - 15.5f) / 16.0f;
			float w = 0.08f + 0.62f * powf(sinf(PI * powf(fy, 0.75f) * 0.92f), 1.2f);
			float q = fx / w;
			float I = expf(-q * q * 2.2f) * smooth(0.0f, 0.25f, fy) * (1 - smooth(0.85f, 1.0f, fy));
			float core = expf(-q * q * 6.0f) * smooth(0.45f, 0.8f, fy);
			C3 c = c3(powf(I, 0.6f), powf(I, 1.25f) * 0.82f, powf(I, 3.0f) * 0.55f);
			c = cmix(c, c3(0.85f, 0.9f, 1.0f), core * 0.5f);
			tFlame.px[0][y * 32 + x] = packc(c, I);
		}
	tVig = new_tex(64, 64, 1);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			float dx = (x - 31.5f) / 32, dy = (y - 31.5f) / 32, r = sqrtf(dx * dx * 1.0f + dy * dy * 0.85f);
			float v = 1 - 0.62f * smooth(0.42f, 1.25f, r);
			tVig.px[0][y * 64 + x] = pack(v, v * 0.985f, v * 0.955f, 1);
		}
}

extern unsigned char msx[];
static void make_font(void)
{
	tFont = new_tex(128, 128, 1);
	for (int ch = 0; ch < 256; ch++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				int on = msx[ch * 8 + y] & (0x80 >> x);
				tFont.px[0][((ch / 16) * 8 + y) * 128 + (ch % 16) * 8 + x] = on ? 0xffffffff : 0x00ffffff;
			}
}

/* ------------------------------------------------------------- geometry */
typedef struct { float u, v; float nx, ny, nz; float x, y, z; } VL;
typedef struct { float u, v; u32 c; float x, y, z; } VT;
#define VL_FMT (GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define VT_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define V2_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

typedef struct { VL *v; unsigned short *ix; int nv, ni; } Mesh;
static Mesh mWall, mReveal, mFloor, mSideL, mSideR, mPillars, mRack, mCandles;
static VL __attribute__((aligned(16))) vaultCP[7 * 4];

static Mesh mesh_new(int nv, int ni)
{
	Mesh m; m.v = palloc(nv * sizeof(VL)); m.ix = palloc(ni * 2); m.nv = m.ni = 0;
	return m;
}
static inline void vput(Mesh *m, float u, float v, V3 n, V3 p)
{
	VL *q = &m->v[m->nv++];
	q->u = u; q->v = v; q->nx = n.x; q->ny = n.y; q->nz = n.z; q->x = p.x; q->y = p.y; q->z = p.z;
}
static void grid_idx(Mesh *m, int base, int cols, int rows)
{
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++) {
			int a = base + r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
			m->ix[m->ni++] = a; m->ix[m->ni++] = d; m->ix[m->ni++] = b;
			m->ix[m->ni++] = b; m->ix[m->ni++] = d; m->ix[m->ni++] = e;
		}
}
static void quad_idx(Mesh *m, int base)
{
	m->ix[m->ni++] = base; m->ix[m->ni++] = base + 1; m->ix[m->ni++] = base + 2;
	m->ix[m->ni++] = base; m->ix[m->ni++] = base + 2; m->ix[m->ni++] = base + 3;
}

/* the window opening, cast as rays from a point inside it */
#define WCX 0.0f
#define WCY 1.9f
static float contour_t(float th, float off)
{
	float dx = cosf(th), dy = sinf(th), lo = 0, hi = 12;
	for (int i = 0; i < 28; i++) {
		float m = (lo + hi) * 0.5f;
		if (sdArch(WCX + dx * m, WCY + dy * m, A0, Y00, YS0) < off) lo = m; else hi = m;
	}
	return (lo + hi) * 0.5f;
}
static inline V3 win2world(float X, float Y, float z) { return v3(X * S_WIN, WIN_Y + Y * S_WIN, z); }

#define NA 96
static void build_wall_and_reveal(void)
{
	float xmin = -7.0f / S_WIN, xmax = 7.0f / S_WIN, ymin = -WIN_Y / S_WIN, ymax = (17.2f - WIN_Y) / S_WIN;
	float th[NA + 5]; int n = 0;
	for (int i = 0; i < NA; i++) th[n++] = -PI * 0.5f + i * 2 * PI / NA;
	float cx[4] = {xmax, xmin, xmin, xmax}, cy[4] = {ymax, ymax, ymin, ymin};
	for (int k = 0; k < 4; k++) {
		float a = atan2f(cy[k] - WCY, cx[k] - WCX);
		if (a < -PI * 0.5f) a += 2 * PI;
		th[n++] = a;
	}
	for (int i = 1; i < n; i++) /* insertion sort */
		for (int j = i; j > 0 && th[j] < th[j - 1]; j--) { float t = th[j]; th[j] = th[j - 1]; th[j - 1] = t; }
	th[n] = th[0] + 2 * PI; /* close the loop */

	static const float RS[5] = {0, 0.1f, 0.25f, 0.5f, 1.0f};
	mWall = mesh_new((n + 1) * 5, n * 4 * 6);
	mReveal = mesh_new((n + 1) * 3, n * 2 * 6);
	for (int i = 0; i <= n; i++) {
		float dx = cosf(th[i]), dy = sinf(th[i]);
		float tq = fminf(dx > 0 ? (xmax - WCX) / dx : (dx < 0 ? (xmin - WCX) / dx : 1e9f),
		                 dy > 0 ? (ymax - WCY) / dy : (dy < 0 ? (ymin - WCY) / dy : 1e9f));
		float to = contour_t(th[i], 0.22f), ti = contour_t(th[i], -0.045f);
		for (int k = 0; k < 5; k++) {
			float t = mixf(to, tq, RS[k]);
			V3 p = win2world(WCX + dx * t, WCY + dy * t, 0);
			vput(&mWall, p.x * 0.5f, p.y * 0.5f, v3(0, 0, 1), p);
		}
		for (int k = 0; k < 3; k++) {
			float f = k * 0.5f;
			float t = mixf(ti, to, f);
			V3 p = win2world(WCX + dx * t, WCY + dy * t, mixf(GLASS_Z + 0.02f, 0, f));
			vput(&mReveal, 0, f * 0.35f, v3(0, 0, 1), p);
		}
	}
	/* reveal: arc-length u, normals from the surface */
	float s = 0;
	for (int i = 0; i <= n; i++) {
		if (i) {
			float ex = mReveal.v[i * 3 + 2].x - mReveal.v[(i - 1) * 3 + 2].x;
			float ey = mReveal.v[i * 3 + 2].y - mReveal.v[(i - 1) * 3 + 2].y;
			s += sqrtf(ex * ex + ey * ey);
		}
		for (int k = 0; k < 3; k++) {
			VL *q = &mReveal.v[i * 3 + k];
			int ip = i > 0 ? i - 1 : n - 1, in = i < n ? i + 1 : 1;
			VL *a = &mReveal.v[ip * 3 + k], *b = &mReveal.v[in * 3 + k];
			VL *c0 = &mReveal.v[i * 3 + 0], *c2 = &mReveal.v[i * 3 + 2];
			V3 tt = v3(b->x - a->x, b->y - a->y, b->z - a->z);
			V3 td = v3(c2->x - c0->x, c2->y - c0->y, c2->z - c0->z);
			V3 nn = vnorm(vcross(tt, td));
			V3 toAxis = v3(WCX * S_WIN - q->x, (WIN_Y + WCY * S_WIN) - q->y, 0);
			if (vdot(nn, toAxis) < 0) nn = vmul(nn, -1);
			q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			q->u = s * 0.5f;
		}
	}
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < 4; k++) {
			int a = i * 5 + k, b = a + 1, c = (i + 1) * 5 + k, d = c + 1;
			mWall.ix[mWall.ni++] = a; mWall.ix[mWall.ni++] = c; mWall.ix[mWall.ni++] = b;
			mWall.ix[mWall.ni++] = b; mWall.ix[mWall.ni++] = c; mWall.ix[mWall.ni++] = d;
		}
		for (int k = 0; k < 2; k++) {
			int a = i * 3 + k, b = a + 1, c = (i + 1) * 3 + k, d = c + 1;
			mReveal.ix[mReveal.ni++] = a; mReveal.ix[mReveal.ni++] = c; mReveal.ix[mReveal.ni++] = b;
			mReveal.ix[mReveal.ni++] = b; mReveal.ix[mReveal.ni++] = c; mReveal.ix[mReveal.ni++] = d;
		}
	}
}

static void build_floor_and_sides(void)
{
	mFloor = mesh_new(29 * 49, 28 * 48 * 6);
	for (int iz = 0; iz <= 48; iz++)
		for (int ix = 0; ix <= 28; ix++) {
			float x = -7 + ix * 0.5f, z = iz * 0.5f;
			vput(&mFloor, x * 0.5f, z * 0.5f, v3(0, 1, 0), v3(x, 0, z));
		}
	grid_idx(&mFloor, 0, 28, 48);
	for (int s = 0; s < 2; s++) {
		Mesh *m = s ? &mSideR : &mSideL;
		float x = s ? 7.0f : -7.0f;
		*m = mesh_new(25 * 13, 24 * 12 * 6);
		for (int iy = 0; iy <= 12; iy++)
			for (int iz = 0; iz <= 24; iz++)
				vput(m, iz * 0.5f, iy * 0.5f, v3(s ? -1 : 1, 0, 0), v3(x, (float)iy, (float)iz));
		grid_idx(m, 0, 24, 12);
	}
}

#define PSEG 32
static void build_pillars(void)
{
	static const float prof[][2] = {{0, 0.72f}, {0.32f, 0.72f}, {0.38f, 0.62f}, {0.55f, 0.55f}, {0.62f, 0.5f},
	                                {11.2f, 0.5f}, {11.35f, 0.56f}, {11.6f, 0.66f}, {11.8f, 0.74f}, {12.0f, 0.74f}};
	const int NR = sizeof(prof) / sizeof(prof[0]);
	static const float PX[6] = {-5.6f, -5.6f, -5.6f, 5.6f, 5.6f, 5.6f}, PZ[6] = {3.5f, 9.5f, 15.5f, 3.5f, 9.5f, 15.5f};
	mPillars = mesh_new(6 * (PSEG + 1) * NR, 6 * PSEG * (NR - 1) * 6);
	for (int p = 0; p < 6; p++) {
		int base = mPillars.nv;
		for (int r = 0; r < NR; r++)
			for (int i = 0; i <= PSEG; i++) {
				float th = i * 2 * PI / PSEG;
				float rr = prof[r][1] * (0.93f + 0.07f * cosf(8 * th));
				V3 pos = v3(PX[p] + rr * cosf(th), prof[r][0], PZ[p] + rr * sinf(th));
				vput(&mPillars, th / (2 * PI) * 3, prof[r][0] * 0.5f, v3(0, 0, 0), pos);
			}
		for (int r = 0; r < NR; r++)
			for (int i = 0; i <= PSEG; i++) {
				VL *q = &mPillars.v[base + r * (PSEG + 1) + i];
				int il = i > 0 ? i - 1 : PSEG - 1, ir = i < PSEG ? i + 1 : 1;
				int rd = r > 0 ? r - 1 : 0, ru = r < NR - 1 ? r + 1 : NR - 1;
				VL *a = &mPillars.v[base + r * (PSEG + 1) + il], *b = &mPillars.v[base + r * (PSEG + 1) + ir];
				VL *c = &mPillars.v[base + rd * (PSEG + 1) + i], *d = &mPillars.v[base + ru * (PSEG + 1) + i];
				V3 tth = v3(b->x - a->x, b->y - a->y, b->z - a->z);
				V3 ty = v3(d->x - c->x, d->y - c->y, d->z - c->z);
				V3 nn = vnorm(vcross(ty, tth));
				V3 rad = v3(q->x - PX[p], 0, q->z - PZ[p]);
				if (vdot(nn, rad) < 0) nn = vmul(nn, -1);
				q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			}
		grid_idx(&mPillars, base, PSEG, NR - 1);
	}
}

#define NCAND 12
static V3 flamePos[NCAND];
static void build_rack(void)
{
	float x0 = 3.2f, x1 = 4.4f, y1 = 0.85f, z0 = 6.6f, z1 = 7.2f;
	mRack = mesh_new(20, 30);
	int b;
	b = mRack.nv; /* top */
	vput(&mRack, x0, z0, v3(0, 1, 0), v3(x0, y1, z0)); vput(&mRack, x1, z0, v3(0, 1, 0), v3(x1, y1, z0));
	vput(&mRack, x1, z1, v3(0, 1, 0), v3(x1, y1, z1)); vput(&mRack, x0, z1, v3(0, 1, 0), v3(x0, y1, z1)); quad_idx(&mRack, b);
	b = mRack.nv; /* front */
	vput(&mRack, x0, 0, v3(0, 0, 1), v3(x0, 0, z1)); vput(&mRack, x1, 0, v3(0, 0, 1), v3(x1, 0, z1));
	vput(&mRack, x1, y1, v3(0, 0, 1), v3(x1, y1, z1)); vput(&mRack, x0, y1, v3(0, 0, 1), v3(x0, y1, z1)); quad_idx(&mRack, b);
	b = mRack.nv; /* back */
	vput(&mRack, x0, 0, v3(0, 0, -1), v3(x0, 0, z0)); vput(&mRack, x1, 0, v3(0, 0, -1), v3(x1, 0, z0));
	vput(&mRack, x1, y1, v3(0, 0, -1), v3(x1, y1, z0)); vput(&mRack, x0, y1, v3(0, 0, -1), v3(x0, y1, z0)); quad_idx(&mRack, b);
	b = mRack.nv; /* left */
	vput(&mRack, z0, 0, v3(-1, 0, 0), v3(x0, 0, z0)); vput(&mRack, z1, 0, v3(-1, 0, 0), v3(x0, 0, z1));
	vput(&mRack, z1, y1, v3(-1, 0, 0), v3(x0, y1, z1)); vput(&mRack, z0, y1, v3(-1, 0, 0), v3(x0, y1, z0)); quad_idx(&mRack, b);
	b = mRack.nv; /* right */
	vput(&mRack, z0, 0, v3(1, 0, 0), v3(x1, 0, z0)); vput(&mRack, z1, 0, v3(1, 0, 0), v3(x1, 0, z1));
	vput(&mRack, z1, y1, v3(1, 0, 0), v3(x1, y1, z1)); vput(&mRack, z0, y1, v3(1, 0, 0), v3(x1, y1, z0)); quad_idx(&mRack, b);

	mCandles = mesh_new(NCAND * 30, NCAND * 48);
	for (int c = 0; c < NCAND; c++) {
		float cx = x0 + 0.12f + (c % 6) * 0.19f + (hf(c, 1, 51) - 0.5f) * 0.05f;
		float cz = (c < 6 ? 6.78f : 7.04f) + (hf(c, 2, 51) - 0.5f) * 0.04f;
		float h = 0.07f + 0.2f * hf(c, 3, 51), r = 0.026f + 0.01f * hf(c, 4, 51);
		for (int s = 0; s < 6; s++) {
			float a0 = s * PI / 3, a1 = (s + 1) * PI / 3, am = (a0 + a1) * 0.5f;
			V3 nn = v3(cosf(am), 0, sinf(am));
			b = mCandles.nv;
			vput(&mCandles, 0, 0, nn, v3(cx + r * cosf(a0), y1, cz + r * sinf(a0)));
			vput(&mCandles, 0, 0, nn, v3(cx + r * cosf(a1), y1, cz + r * sinf(a1)));
			vput(&mCandles, 0, 0, nn, v3(cx + r * cosf(a1), y1 + h, cz + r * sinf(a1)));
			vput(&mCandles, 0, 0, nn, v3(cx + r * cosf(a0), y1 + h, cz + r * sinf(a0)));
			quad_idx(&mCandles, b);
		}
		b = mCandles.nv;
		for (int s = 0; s < 6; s++) vput(&mCandles, 0, 0, v3(0, 1, 0), v3(cx + r * cosf(s * PI / 3), y1 + h, cz + r * sinf(s * PI / 3)));
		for (int s = 1; s < 5; s++) { mCandles.ix[mCandles.ni++] = b; mCandles.ix[mCandles.ni++] = b + s; mCandles.ix[mCandles.ni++] = b + s + 1; }
		flamePos[c] = v3(cx, y1 + h + 0.012f, cz);
	}
}

static void build_vault(void)
{
	static const float PXY[7][2] = {{-7, 12}, {-7, 14.4f}, {-3.8f, 16.2f}, {0, 17.0f}, {3.8f, 16.2f}, {7, 14.4f}, {7, 12}};
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 7; i++) {
			VL *q = &vaultCP[j * 7 + i];
			int ip = i > 0 ? i - 1 : 0, in = i < 6 ? i + 1 : 6;
			float tx = PXY[in][0] - PXY[ip][0], ty = PXY[in][1] - PXY[ip][1];
			V3 nn = vnorm(v3(ty, -tx, 0));
			if (vdot(nn, v3(-PXY[i][0], 13 - PXY[i][1], 0)) < 0) nn = vmul(nn, -1);
			q->u = i; q->v = j * 2.0f;
			q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			q->x = PXY[i][0]; q->y = PXY[i][1]; q->z = j * 8.0f;
		}
}

/* --------------------------------------------------------------- state */
typedef struct {
	float t, dt;
	float tau;     /* time of day 0..1 */
	V3 d;          /* sunlight direction, into the church */
	C3 sun;        /* sun colour */
	float I;       /* sunlight intensity after clouds */
	float L;       /* slice length to reach the floor from the top */
	V3 eye, ctr, camR, camU;
	float flick;
} Frame;
static Frame F;
static int optBloom = 1, optShafts = 1, optDust = 1, optPause = 0, optAuto = 1, optHelp = 0;
static float camTh = 0.3f, camR = 11.5f, camH = 1.9f, camTY = 4.4f;
static float hudFade = 1.0f;

#define NDUST 520
typedef struct { V3 p, v; float life, max, ph, sz; } Dust;
static Dust dust[NDUST];

static inline V3 glass_point(float u, float v) { return v3((u * 2 - 1) * S_WIN, WIN_Y + (1 - v) * 4 * S_WIN, GLASS_Z); }
static inline void to_window(V3 p, float *u, float *v)
{
	float s = (p.z - GLASS_Z) / F.d.z;
	float hx = p.x - s * F.d.x, hy = p.y - s * F.d.y;
	*u = hx / (2 * S_WIN) + 0.5f;
	*v = 1 - (hy - WIN_Y) / (4 * S_WIN);
}
static void dust_spawn(Dust *q)
{
	for (int tries = 0; tries < 16; tries++) {
		float u = frand(), v = frand();
		C3 l = sample_light(u, v);
		if (l.r + l.g + l.b < 0.25f) continue;
		float t = 0.4f + frand() * (F.L - 0.4f);
		V3 p = vadd(glass_point(u, v), vmul(F.d, t));
		if (p.y < 0.08f) continue;
		q->p = p;
		break;
	}
	q->v = v3((frand() - 0.5f) * 0.04f, (frand() - 0.3f) * 0.03f, (frand() - 0.5f) * 0.04f);
	q->max = 6 + frand() * 10;
	q->life = q->max;
	q->ph = frand() * 6.28f;
	q->sz = 0.010f + 0.016f * frand() * frand();
}

static void update_sun(void)
{
	float az = 0.55f - 0.5f * F.tau;
	float el = 0.55f + 0.33f * sinf(PI * F.tau);
	F.d = v3(-cosf(el) * sinf(az), -sinf(el), cosf(el) * cosf(az));
	float warm = 1 - smooth(0.5f, 0.85f, el);
	F.sun = cmix(c3(1.0f, 0.97f, 0.90f), c3(1.0f, 0.74f, 0.46f), warm);
	float cl = 0.5f + 0.5f * sinf(F.t * 0.21f) * sinf(F.t * 0.13f + 1.3f);
	float dip = smooth(0.75f, 1.0f, 0.5f + 0.5f * sinf(F.t * 0.071f + 2.0f));
	F.I = (0.80f + 0.20f * cl) * (1 - 0.35f * dip) * (0.85f + 0.15f * smooth(0.5f, 0.85f, el));
	F.L = (WIN_Y + 4 * S_WIN) / -F.d.y;
	F.flick = 0.82f + 0.10f * sinf(F.t * 13.0f) * sinf(F.t * 7.3f + 0.5f) + 0.06f * sinf(F.t * 29.0f + 1.0f) + 0.04f * sinf(F.t * 3.1f);
}

static void update_camera(void)
{
	if (optAuto) {
		float t = F.t;
		camTh = 0.28f + 0.26f * sinf(t * 0.045f);
		camR = 10.0f + 1.5f * sinf(t * 0.031f + 1.0f);
		camH = 1.9f + 0.5f * sinf(t * 0.037f);
		camTY = 4.4f + 0.5f * sinf(t * 0.05f + 0.7f);
	}
	F.ctr = v3(-0.3f, camTY, 1.5f);
	F.eye = v3(F.ctr.x + camR * sinf(camTh), camH, F.ctr.z + camR * cosf(camTh));
	V3 fwd = vnorm(vsub(F.ctr, F.eye));
	F.camR = vnorm(vcross(fwd, v3(0, 1, 0)));
	F.camU = vcross(F.camR, fwd);
}

static void update_dust(void)
{
	for (int i = 0; i < NDUST; i++) {
		Dust *q = &dust[i];
		q->life -= F.dt;
		if (q->life <= 0) { dust_spawn(q); continue; }
		q->v.x += (frand() - 0.5f) * 0.03f * F.dt;
		q->v.y += ((frand() - 0.5f) * 0.03f + 0.002f) * F.dt;
		q->v.z += (frand() - 0.5f) * 0.03f * F.dt;
		q->v = vmul(q->v, 1 - 0.2f * F.dt);
		q->p = vadd(q->p, vmul(q->v, F.dt));
	}
}

/* --------------------------------------------------------------- lights */
static inline u32 lcol(C3 c, float s) { return packc(cmul(c, s), 0); }

static void set_lights(int mirror)
{
	float my = mirror ? -1.0f : 1.0f;
	float I = F.I;
	/* 0: glow of the window on its reveal and the wall around it */
	ScePspFVector3 p0 = {0, 5.6f * my, 1.1f};
	sceGuLight(0, GU_POINTLIGHT, GU_DIFFUSE, &p0);
	sceGuLightColor(0, GU_DIFFUSE, lcol(c3(avgGlass.r * 0.9f + 0.1f, avgGlass.g * 0.9f + 0.1f, avgGlass.b * 0.9f + 0.1f), 1.3f * I));
	sceGuLightAtt(0, 0.35f, 0.0f, 0.16f);
	/* 1: light bounced off the sunlit floor */
	float tc = 5.6f / -F.d.y;
	ScePspFVector3 p1 = {F.d.x * tc, 0.6f * my, GLASS_Z + F.d.z * tc};
	sceGuLight(1, GU_POINTLIGHT, GU_DIFFUSE, &p1);
	sceGuLightColor(1, GU_DIFFUSE, lcol(c3((avgGlass.r + F.sun.r) * 0.5f, (avgGlass.g + F.sun.g) * 0.5f, (avgGlass.b + F.sun.b) * 0.5f), 0.55f * I));
	sceGuLightAtt(1, 0.6f, 0.0f, 0.10f);
	/* 2: the candles */
	ScePspFVector3 p2 = {3.8f, 1.25f * my, 6.9f};
	sceGuLight(2, GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, &p2);
	sceGuLightColor(2, GU_DIFFUSE, lcol(c3(1.0f, 0.55f, 0.22f), F.flick));
	sceGuLightColor(2, GU_SPECULAR, lcol(c3(1.0f, 0.7f, 0.4f), F.flick));
	sceGuLightAtt(2, 0.25f, 0.35f, 0.32f);
	/* 3: cool fill from the nave */
	ScePspFVector3 p3 = {0.25f, 0.8f * my, 0.55f};
	sceGuLight(3, GU_DIRECTIONAL, GU_DIFFUSE, &p3);
	sceGuLightColor(3, GU_DIFFUSE, lcol(c3(0.04f, 0.048f, 0.07f), 1));
	for (int i = 0; i < 4; i++) sceGuLightColor(i, GU_AMBIENT, 0);
	sceGuAmbient(0xff050404);
	sceGuSpecular(14.0f);
	sceGuEnable(GU_LIGHTING);
	for (int i = 0; i < 4; i++) sceGuEnable(GU_LIGHT0 + i);
}

static inline void material(u32 diffuse, u32 spec)
{
	sceGuMaterial(GU_AMBIENT | GU_DIFFUSE, diffuse);
	sceGuMaterial(GU_SPECULAR, spec);
}
static inline void draw_mesh(const Mesh *m) { sceGumDrawArray(GU_TRIANGLES, VL_FMT | GU_INDEX_16BIT, m->ni, m->ix, m->v); }

/* ---------------------------------------------------------------- draw */
static void draw_architecture(void)
{
	bind(&tWall);
	material(0xffffffff, 0);
	draw_mesh(&mWall);
	draw_mesh(&mReveal);
	draw_mesh(&mSideL);
	draw_mesh(&mSideR);
	material(0xffe8eef2, 0xff202020);
	draw_mesh(&mPillars);
	material(0xff2a3a52, 0xff303030); /* dark oak, through the stone texture */
	draw_mesh(&mRack);
	sceGuDisable(GU_TEXTURE_2D);
	material(0xffc8e4f2, 0xff606060);
	draw_mesh(&mCandles);
	sceGuEnable(GU_TEXTURE_2D);
}

static void draw_vault(void)
{
	bind(&tVault);
	material(0xffffffff, 0);
	sceGuPatchDivide(12, 8);
	sceGuPatchPrim(GU_TRIANGLE_STRIP);
	sceGumDrawBezier(GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF, 7, 4, 0, vaultCP);
}

static void draw_glass(void)
{
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_FOG);
	bind(&tGlass);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuEnable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
	sceGuEnable(GU_ALPHA_TEST);
	u32 col = pack(F.sun.r * F.I, F.sun.g * F.I, F.sun.b * F.I, 1);
	VT *v = sceGuGetMemory(4 * sizeof(VT));
	V3 a = glass_point(0, 0), b = glass_point(1, 1);
	v[0] = (VT){0, 0, col, a.x, a.y, GLASS_Z};
	v[1] = (VT){1, 0, col, b.x, a.y, GLASS_Z};
	v[2] = (VT){0, 1, col, a.x, b.y, GLASS_Z};
	v[3] = (VT){1, 1, col, b.x, b.y, GLASS_Z};
	sceGumDrawArray(GU_TRIANGLE_STRIP, VT_FMT, 4, 0, v);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

/* the glass cast along the sunlight onto whatever faces it */
static void draw_projection(void)
{
	float kx = F.d.x / F.d.z, ky = F.d.y / F.d.z;
	ScePspFMatrix4 m;
	memset(&m, 0, sizeof(m));
	m.x.x = 1 / (2 * S_WIN);
	m.y.y = -1 / (4 * S_WIN);
	m.z.x = -kx / (2 * S_WIN);
	m.z.y = ky / (4 * S_WIN);
	m.w.x = GLASS_Z * kx / (2 * S_WIN) + 0.5f;
	m.w.y = 1 + WIN_Y / (4 * S_WIN) - GLASS_Z * ky / (4 * S_WIN);
	m.w.z = 1;
	m.w.w = 1;
	sceGuSetMatrix(GU_TEXTURE, &m);
	sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
	sceGuTexProjMapMode(GU_POSITION);

	/* N.L from one white directional light, nothing else */
	ScePspFVector3 toSun = {-F.d.x, -F.d.y, -F.d.z};
	sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &toSun);
	sceGuLightColor(0, GU_DIFFUSE, 0xffffffff);
	sceGuLightAtt(0, 1, 0, 0);
	for (int i = 1; i < 4; i++) sceGuDisable(GU_LIGHT0 + i);
	sceGuAmbient(0);
	material(pack(F.sun.r * F.I, F.sun.g * F.I, F.sun.b * F.I, 1), 0);

	bind(&tLight);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuDisable(GU_FOG);
	sceGuDepthMask(GU_TRUE);
	sceGuEnable(GU_BLEND);
	sceGuEnable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	for (int pass = 0; pass < 2; pass++) {
		if (!pass) sceGuBlendFunc(GU_ADD, 0 /* dst colour */, GU_FIX, 0, 0xffffff);
		else sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x909090, 0xffffff);
		draw_mesh(&mFloor);
		draw_mesh(&mPillars);
		draw_mesh(&mSideL);
		draw_mesh(&mReveal);
	}
	sceGuDisable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuDisable(GU_BLEND);
	sceGuDepthMask(GU_FALSE);
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static void additive_begin(void)
{
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_FOG);
	sceGuDepthMask(GU_TRUE);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
}
static void additive_end(void)
{
	sceGuDisable(GU_BLEND);
	sceGuDepthMask(GU_FALSE);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
}

#define NSLICE 56
#define SGX 2
#define SGY 4
static void draw_shafts(void)
{
	bind(&tShaft);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	int nv = NSLICE * SGX * SGY * 6;
	VT *v = sceGuGetMemory(nv * sizeof(VT)), *o = v;
	float t0 = 0.2f, base = 0.095f * F.I;
	for (int k = 0; k < NSLICE; k++) {
		float t = t0 + (k + 0.5f) * (F.L - t0) / NSLICE;
		float w = base * (1 - 0.5f * t / F.L) * smooth(0.0f, 1.4f, t);
		V3 off = vmul(F.d, t);
		u32 cg[SGY + 1][SGX + 1];
		V3 pg[SGY + 1][SGX + 1];
		for (int gy = 0; gy <= SGY; gy++)
			for (int gx = 0; gx <= SGX; gx++) {
				V3 p = vadd(glass_point((float)gx / SGX, (float)gy / SGY), off);
				float dens = 0.72f + 0.28f * sinf(p.x * 1.1f + F.t * 0.35f + k * 0.13f) * sinf(p.z * 0.8f - F.t * 0.27f + p.y * 0.5f);
				float s = w * dens;
				pg[gy][gx] = p;
				cg[gy][gx] = pack(F.sun.r * s, F.sun.g * s, F.sun.b * s, 1);
			}
		for (int gy = 0; gy < SGY; gy++)
			for (int gx = 0; gx < SGX; gx++) {
				float u0 = (float)gx / SGX, u1 = (float)(gx + 1) / SGX, v0 = (float)gy / SGY, v1 = (float)(gy + 1) / SGY;
				VT a = {u0, v0, cg[gy][gx], pg[gy][gx].x, pg[gy][gx].y, pg[gy][gx].z};
				VT b = {u1, v0, cg[gy][gx + 1], pg[gy][gx + 1].x, pg[gy][gx + 1].y, pg[gy][gx + 1].z};
				VT c = {u0, v1, cg[gy + 1][gx], pg[gy + 1][gx].x, pg[gy + 1][gx].y, pg[gy + 1][gx].z};
				VT d = {u1, v1, cg[gy + 1][gx + 1], pg[gy + 1][gx + 1].x, pg[gy + 1][gx + 1].y, pg[gy + 1][gx + 1].z};
				*o++ = a; *o++ = b; *o++ = c;
				*o++ = b; *o++ = d; *o++ = c;
			}
	}
	sceGumDrawArray(GU_TRIANGLES, VT_FMT, nv, 0, v);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static void draw_dust(void)
{
	bind(&tDot);
	VT *v = sceGuGetMemory(NDUST * 2 * sizeof(VT));
	int n = 0;
	for (int i = 0; i < NDUST; i++) {
		Dust *q = &dust[i];
		float u, w;
		to_window(q->p, &u, &w);
		C3 l = sample_light(u, w);
		float fade = smooth(0, 1.5f, q->life) * smooth(0, 1.5f, q->max - q->life);
		float tw = 0.55f + 0.45f * sinf(q->ph + F.t * (1.5f + q->sz * 60));
		float s = 2.6f * F.I * fade * tw;
		if (q->p.y < 0.02f || (l.r + l.g + l.b) * s < 0.02f) continue;
		u32 col = pack((l.r * 0.7f + 0.3f * F.sun.r) * s, (l.g * 0.7f + 0.3f * F.sun.g) * s, (l.b * 0.7f + 0.3f * F.sun.b) * s, 1);
		/* top-left and bottom-right on screen */
		V3 a = vadd(vsub(q->p, vmul(F.camR, q->sz)), vmul(F.camU, q->sz));
		V3 b = vsub(vadd(q->p, vmul(F.camR, q->sz)), vmul(F.camU, q->sz));
		v[n++] = (VT){0, 0, col, a.x, a.y, a.z};
		v[n++] = (VT){1, 1, col, b.x, b.y, b.z};
	}
	if (n) sceGumDrawArray(GU_SPRITES, VT_FMT, n, 0, v);
}

static void draw_flames(int mirror)
{
	float my = mirror ? -1.0f : 1.0f;
	/* halos first */
	bind(&tDot);
	VT *v = sceGuGetMemory(NCAND * 2 * sizeof(VT));
	for (int c = 0; c < NCAND; c++) {
		float fl = F.flick * (0.9f + 0.1f * sinf(F.t * 17 + c * 2.1f));
		V3 p = flamePos[c]; p.y = (p.y + 0.03f) * my;
		float s = 0.16f;
		u32 col = pack(0.55f * fl * (mirror ? 0.5f : 1), 0.26f * fl * (mirror ? 0.5f : 1), 0.08f * fl * (mirror ? 0.5f : 1), 1);
		V3 a = vadd(vsub(p, vmul(F.camR, s)), vmul(F.camU, s));
		V3 b = vsub(vadd(p, vmul(F.camR, s)), vmul(F.camU, s));
		v[c * 2] = (VT){0, 0, col, a.x, a.y, a.z};
		v[c * 2 + 1] = (VT){1, 1, col, b.x, b.y, b.z};
	}
	sceGumDrawArray(GU_SPRITES, VT_FMT, NCAND * 2, 0, v);
	bind(&tFlame);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	v = sceGuGetMemory(NCAND * 2 * sizeof(VT));
	for (int c = 0; c < NCAND; c++) {
		float fl = 0.85f + 0.15f * sinf(F.t * 21 + c * 1.7f) * sinf(F.t * 9 + c);
		float h = 0.075f * fl, w = 0.018f;
		float sway = 0.006f * sinf(F.t * 5 + c * 3.3f);
		V3 p = flamePos[c]; p.x += sway;
		u32 col = mirror ? 0xff606060 : 0xffffffff;
		V3 a, b;
		if (!mirror) {
			a = vadd(vsub(p, vmul(F.camR, w)), vmul(F.camU, h));
			b = vsub(vadd(p, vmul(F.camR, w)), vmul(F.camU, h * 0.25f));
			v[c * 2] = (VT){0, 0, col, a.x, a.y, a.z};
			v[c * 2 + 1] = (VT){1, 1, col, b.x, b.y, b.z};
		} else {
			p.y = -p.y;
			a = vadd(vsub(p, vmul(F.camR, w)), vmul(F.camU, h * 0.25f));
			b = vsub(vadd(p, vmul(F.camR, w)), vmul(F.camU, h));
			v[c * 2] = (VT){0, 1, col, a.x, a.y, a.z};
			v[c * 2 + 1] = (VT){1, 0, col, b.x, b.y, b.z};
		}
	}
	sceGumDrawArray(GU_SPRITES, VT_FMT, NCAND * 2, 0, v);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static void set_camera(void)
{
	sceGumMatrixMode(GU_PROJECTION);
	sceGumLoadIdentity();
	sceGumPerspective(54.0f, 480.0f / 272.0f, 0.4f, 120.0f);
	sceGumMatrixMode(GU_VIEW);
	sceGumLoadIdentity();
	ScePspFVector3 e = {F.eye.x, F.eye.y, F.eye.z}, c = {F.ctr.x, F.ctr.y, F.ctr.z}, u = {0, 1, 0};
	sceGumLookAt(&e, &c, &u);
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadIdentity();
}

static void render_scene(void)
{
	set_camera();
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuDepthMask(GU_FALSE);
	sceGuDisable(GU_BLEND);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuTexScale(1, 1);
	sceGuTexOffset(0, 0);
	sceGuColorMaterial(0);
	sceGuFog(9.0f, 55.0f, 0x00100b08);

	/* the church upside down, under the marble */
	sceGuEnable(GU_FOG);
	set_lights(1);
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadIdentity();
	ScePspFVector3 flip = {1, -1, 1};
	sceGumScale(&flip);
	draw_architecture();
	draw_glass();
	additive_begin();
	draw_flames(1);
	additive_end();
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadIdentity();

	/* the polished floor over it */
	sceGuEnable(GU_FOG);
	set_lights(0);
	bind(&tFloor);
	material(0xffffffff, 0xff9a9a9a);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	draw_mesh(&mFloor);
	sceGuDisable(GU_BLEND);

	/* the church */
	draw_architecture();
	draw_vault();
	draw_glass();

	draw_projection();

	additive_begin();
	if (optShafts) draw_shafts();
	if (optDust) draw_dust();
	draw_flames(0);
	additive_end();
}

/* ---------------------------------------------------------- 2D and bloom */
static void set_target(u32 off, int w, int h, int stride)
{
	sceGuDrawBufferList(GU_PSM_8888, (void *)off, stride);
	sceGuOffset(2048 - w / 2, 2048 - h / 2);
	sceGuViewport(2048, 2048, w, h);
	sceGuScissor(0, 0, w, h);
}
static void set_main_target(void) { set_target((u32)drawBuf, SCR_W, SCR_H, BUF_W); }

static void blit(float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, u32 col)
{
	int strips = (int)ceilf((x1 - x0) / 32.0f);
	VT *v = sceGuGetMemory(strips * 2 * sizeof(VT));
	for (int s = 0; s < strips; s++) {
		float xa = x0 + s * 32.0f, xb = fminf(xa + 32.0f, x1);
		float ua = u0 + (xa - x0) / (x1 - x0) * (u1 - u0), ub = u0 + (xb - x0) / (x1 - x0) * (u1 - u0);
		v[s * 2] = (VT){ua, v0, col, xa, y0, 0};
		v[s * 2 + 1] = (VT){ub, v1, col, xb, y1, 0};
	}
	sceGuDrawArray(GU_SPRITES, V2_FMT, strips * 2, 0, v);
}
static void rect(float x0, float y0, float x1, float y1, u32 col)
{
	VT *v = sceGuGetMemory(2 * sizeof(VT));
	v[0] = (VT){0, 0, col, x0, y0, 0};
	v[1] = (VT){0, 0, col, x1, y1, 0};
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDrawArray(GU_SPRITES, V2_FMT, 2, 0, v);
	sceGuEnable(GU_TEXTURE_2D);
}

static void clear_target(u32 off, int pw, int ph, int stride)
{
	set_target(off, pw, ph, stride);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
}

/* four bilinear taps at the corners of a (2o+1) square: one Kawase pass */
static void kawase(u32 src, u32 dst, int w, int h, int stride, int ph, float o)
{
	clear_target(dst, stride, ph, stride);
	set_target(dst, w, h, stride);
	bind_vram(src, stride, stride >= 256 ? 256 : stride, stride);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	const float dx[4] = {-o, o, -o, o}, dy[4] = {-o, -o, o, o};
	for (int k = 0; k < 4; k++) blit(dx[k], dy[k], w + dx[k], h + dy[k], 0, 0, w, h, 0xff404040);
	sceGuDisable(GU_BLEND);
}

static void bloom(void)
{
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_LIGHTING);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);

	/* half res, then keep only what is brighter than the threshold */
	clear_target(RT_A, 256, 144, 256);
	set_target(RT_A, 240, 136, 256);
	bind_vram((u32)drawBuf, 512, 512, BUF_W);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	blit(0, 0, 480, 272, 0, 0, 240, 136, 0xffffffff);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	rect(0, 0, 240, 136, 0xff585858);
	sceGuDisable(GU_BLEND);

	/* quarter and eighth, each doubled on the way down */
	clear_target(RT_C, 128, 72, 128);
	set_target(RT_C, 120, 68, 128);
	bind_vram(RT_A, 256, 256, 256);
	sceGuEnable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	blit(0, 0, 240, 136, 0, 0, 120, 68, 0xffffffff);
	clear_target(RT_E, 64, 40, 64);
	set_target(RT_E, 60, 34, 64);
	bind_vram(RT_C, 128, 128, 128);
	blit(0, 0, 120, 68, 0, 0, 60, 34, 0xffffffff);
	sceGuDisable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);

	kawase(RT_A, RT_B, 240, 136, 256, 144, 1.0f);
	kawase(RT_C, RT_D, 120, 68, 128, 72, 1.5f);
	kawase(RT_D, RT_C, 120, 68, 128, 72, 2.5f);
	kawase(RT_E, RT_F, 60, 34, 64, 40, 1.5f);
	kawase(RT_F, RT_E, 60, 34, 64, 40, 2.5f);

	/* back onto the frame */
	set_main_target();
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x909090, 0xffffff);
	bind_vram(RT_B, 256, 256, 256);
	blit(0.5f, 0.5f, 240 - 0.5f, 136 - 0.5f, 0, 0, SCR_W, SCR_H, 0xffffffff);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xa0b0bc, 0xffffff);
	bind_vram(RT_C, 128, 128, 128);
	blit(0.5f, 0.5f, 120 - 0.5f, 68 - 0.5f, 0, 0, SCR_W, SCR_H, 0xffffffff);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xb0c8d8, 0xffffff);
	bind_vram(RT_E, 64, 64, 64);
	blit(0.5f, 0.5f, 60 - 0.5f, 34 - 0.5f, 0, 0, SCR_W, SCR_H, 0xffffffff);
	sceGuDisable(GU_BLEND);
}

static void vignette(void)
{
	sceGuDisable(GU_DEPTH_TEST);
	sceGuEnable(GU_TEXTURE_2D);
	bind(&tVig);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, 0 /* src colour */, 0, 0);
	blit(0.5f, 0.5f, 63.5f, 63.5f, 0, 0, SCR_W, SCR_H, 0xffffffff);
	sceGuDisable(GU_BLEND);
}

static void text(float x, float y, const char *s, u32 col, float sc)
{
	int n = strlen(s);
	if (!n) return;
	VT *v = sceGuGetMemory(n * 2 * sizeof(VT));
	for (int i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		float u = (ch % 16) * 8, w = (ch / 16) * 8;
		v[i * 2] = (VT){u, w, col, x + i * 8 * sc, y, 0};
		v[i * 2 + 1] = (VT){u + 8, w + 8, col, x + (i + 1) * 8 * sc, y + 8 * sc, 0};
	}
	sceGuDrawArray(GU_SPRITES, V2_FMT, n * 2, 0, v);
}
static inline u32 acol(u32 rgb, float a) { return (rgb & 0xffffff) | ((u32)(clampf(a, 0, 1) * 255) << 24); }

static void hud(void)
{
	sceGuDisable(GU_DEPTH_TEST);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	bind(&tFont);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	if (hudFade > 0.01f) {
		float a = hudFade;
		text(240 - 11 * 8, 22, "LUX AETERNA", acol(0xffe8f4ff, a), 2);
		text(240 - 20 * 4, 44, "ein Kirchenfenster", acol(0xffb8c8d8, a * 0.9f), 1);
		text(240 - 25 * 4, 250, "SELECT: Steuerung/Effekte", acol(0xff90a0b0, a * 0.8f), 1);
	}
	if (optHelp) {
		sceGuDisable(GU_TEXTURE_2D);
		VT *v = sceGuGetMemory(2 * sizeof(VT));
		v[0] = (VT){0, 0, 0xa0000000, 12, 12, 0};
		v[1] = (VT){0, 0, 0xa0000000, 468, 150, 0};
		sceGuDrawArray(GU_SPRITES, V2_FMT, 2, 0, v);
		sceGuEnable(GU_TEXTURE_2D);
		u32 w = 0xffe0e8f0, g = 0xff7fd0ff, on = 0xff80ff90, off = 0xff6060ff;
		text(20, 18, "Analog      Kamera schwenken / heben", w, 1);
		text(20, 28, "Pfeile      naeher, weiter, Blick hoch/tief", w, 1);
		text(20, 38, "L / R       Tageszeit (Sonnenstand)", w, 1);
		text(20, 48, "START       Kamerafahrt an/aus", w, 1);
		text(20, 62, "Dreieck     Bloom", optBloom ? on : off, 1);
		text(20, 72, "Kreis       Lichtstrahlen", optShafts ? on : off, 1);
		text(20, 82, "Quadrat     Staub im Licht", optDust ? on : off, 1);
		text(20, 92, "Kreuz       Sonne anhalten", optPause ? on : off, 1);
		text(20, 108, "GU: 4 HW-Lichter, Nebel, Spiegelung, Textur-", g, 1);
		text(20, 118, "projektion, 2x-Farbe, Bezier-Gewoelbe, Mipmaps,", g, 1);
		text(20, 128, "Render-to-Texture-Bloom, Sprites, VFPU-Matrizen", g, 1);
	}
	sceGuDisable(GU_BLEND);
}

/* ---------------------------------------------------------------- frame */
static void frame(void)
{
	sceGuStart(GU_DIRECT, gulist);
	set_main_target();
	sceGuClearColor(0xff0a0706);
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	render_scene();
	if (optBloom) bloom();
	set_main_target();
	vignette();
	hud();
	sceGuFinish();
	sceGuSync(0, 0);
}

static void gu_init(void)
{
	sceGuInit();
	sceGuStart(GU_DIRECT, gulist);
	sceGuDrawBuffer(GU_PSM_8888, (void *)FB0, BUF_W);
	sceGuDispBuffer(SCR_W, SCR_H, (void *)FB1, BUF_W);
	sceGuDepthBuffer((void *)ZBUF, BUF_W);
	sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
	sceGuViewport(2048, 2048, SCR_W, SCR_H);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuShadeModel(GU_SMOOTH);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
}

static void swap(void)
{
	sceDisplayWaitVblankStart();
	drawBuf = sceGuSwapBuffers();
}

static void loading(const char *msg)
{
	for (int i = 0; i < 2; i++) {
		sceGuStart(GU_DIRECT, gulist);
		set_main_target();
		sceGuClearColor(0xff0a0706);
		sceGuClear(GU_COLOR_BUFFER_BIT);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		bind(&tFont);
		sceGuTexFilter(GU_NEAREST, GU_NEAREST);
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		text(240 - strlen(msg) * 4, 132, msg, 0xffa0c0d8, 1);
		sceGuDisable(GU_BLEND);
		sceGuFinish();
		sceGuSync(0, 0);
		swap();
	}
}

#ifdef CAPTURE
static void save_bmp(const char *path)
{
	const u32 *px = (const u32 *)(0x44000000 | ((u32)sceGeEdramGetAddr() + (u32)drawBuf));
	unsigned char hdr[54] = {'B', 'M'};
	u32 size = 54 + 480 * 272 * 3;
	hdr[2] = size; hdr[3] = size >> 8; hdr[4] = size >> 16; hdr[5] = size >> 24;
	hdr[10] = 54; hdr[14] = 40;
	hdr[18] = 480 & 255; hdr[19] = 480 >> 8; hdr[22] = 272 & 255; hdr[23] = 272 >> 8;
	hdr[26] = 1; hdr[28] = 24;
	SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0) return;
	sceIoWrite(fd, hdr, 54);
	static unsigned char row[480 * 3];
	for (int y = 271; y >= 0; y--) {
		for (int x = 0; x < 480; x++) {
			u32 p = px[y * BUF_W + x];
			row[x * 3] = (p >> 16) & 255; row[x * 3 + 1] = (p >> 8) & 255; row[x * 3 + 2] = p & 255;
		}
		sceIoWrite(fd, row, sizeof(row));
	}
	sceIoClose(fd);
}
typedef struct { float t, tau, th, r, h, ty; int bloom, auto_; } Shot;
static const Shot shots[] = {
	{20.0f, 0.15f, 0.40f, 10.0f, 1.9f, 4.2f, 1, 0},
	{60.0f, 0.50f, 0.05f, 13.0f, 1.7f, 4.8f, 1, 0},
	{90.0f, 0.85f, 0.40f, 9.0f, 2.3f, 3.8f, 1, 0},
	{20.0f, 0.15f, 0.30f, 11.5f, 1.9f, 4.4f, 0, 0},
	{40.0f, 0.30f, -0.25f, 7.5f, 1.5f, 5.5f, 1, 0},
	{50.0f, 0.40f, 0.55f, 12.0f, 1.6f, 1.2f, 1, 0},
};
#endif

int main(void)
{
	setup_callbacks();
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	make_font();
	sceKernelDcacheWritebackAll();
	gu_init();
	loading("Das Glas wird gebrannt ...");

	make_glass();
	make_wall();
	make_floor();
	make_vault();
	make_sprites();
	build_wall_and_reveal();
	build_floor_and_sides();
	build_pillars();
	build_rack();
	build_vault();
	sceKernelDcacheWritebackAll();

	F.t = 0; F.tau = 0.15f; F.dt = 1 / 60.0f;
	update_sun();
	update_camera();
	for (int i = 0; i < NDUST; i++) { dust_spawn(&dust[i]); dust[i].life = frand() * dust[i].max; }

#ifdef CAPTURE
	for (unsigned s = 0; s < sizeof(shots) / sizeof(shots[0]) && running; s++) {
		F.t = shots[s].t; F.tau = shots[s].tau; optAuto = shots[s].auto_; optBloom = shots[s].bloom;
		camTh = shots[s].th; camR = shots[s].r; camH = shots[s].h; camTY = shots[s].ty;
		hudFade = s == 0 ? 1 : 0;
		optHelp = s == 3;
		update_sun();
		update_camera();
		for (int i = 0; i < NDUST; i++) { dust_spawn(&dust[i]); dust[i].life = 2 + frand() * (dust[i].max - 4); }
		update_dust();
		frame();
		char path[64];
		sprintf(path, "host0:/shot%d.bmp", s);
		save_bmp(path);
		swap();
	}
	sceGuTerm();
	sceKernelExitGame();
	return 0;
#endif

	u32 last = sceKernelGetSystemTimeLow();
	u32 prevButtons = 0;
	while (running) {
		u32 now = sceKernelGetSystemTimeLow();
		F.dt = clampf((now - last) / 1000000.0f, 0.001f, 0.1f);
		last = now;
		F.t += F.dt;

		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		u32 pressed = pad.Buttons & ~prevButtons;
		prevButtons = pad.Buttons;
		if (pressed & PSP_CTRL_TRIANGLE) optBloom ^= 1;
		if (pressed & PSP_CTRL_CIRCLE) optShafts ^= 1;
		if (pressed & PSP_CTRL_SQUARE) optDust ^= 1;
		if (pressed & PSP_CTRL_CROSS) optPause ^= 1;
		if (pressed & PSP_CTRL_START) optAuto ^= 1;
		if (pressed & PSP_CTRL_SELECT) optHelp ^= 1;
		float ax = (pad.Lx - 128) / 128.0f, ay = (pad.Ly - 128) / 128.0f;
		if (fabsf(ax) < 0.2f) ax = 0;
		if (fabsf(ay) < 0.2f) ay = 0;
		int manual = ax != 0 || ay != 0 || (pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT));
		if (manual) optAuto = 0;
		if (!optAuto) {
			camTh = clampf(camTh + ax * 0.9f * F.dt, -0.9f, 0.9f);
			camH = clampf(camH - ay * 2.5f * F.dt, 0.6f, 9.0f);
			if (pad.Buttons & PSP_CTRL_UP) camR = clampf(camR - 4 * F.dt, 3.0f, 17.0f);
			if (pad.Buttons & PSP_CTRL_DOWN) camR = clampf(camR + 4 * F.dt, 3.0f, 17.0f);
			if (pad.Buttons & PSP_CTRL_LEFT) camTY = clampf(camTY - 2 * F.dt, 0.5f, 12.0f);
			if (pad.Buttons & PSP_CTRL_RIGHT) camTY = clampf(camTY + 2 * F.dt, 0.5f, 12.0f);
		}
		if (pad.Buttons & PSP_CTRL_LTRIGGER) F.tau = clampf(F.tau - 0.12f * F.dt, 0, 1);
		if (pad.Buttons & PSP_CTRL_RTRIGGER) F.tau = clampf(F.tau + 0.12f * F.dt, 0, 1);
		if (!optPause && !(pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
			static float dir = 1;
			F.tau += dir * F.dt / 150.0f;
			if (F.tau > 1) { F.tau = 1; dir = -1; }
			if (F.tau < 0) { F.tau = 0; dir = 1; }
		}
		hudFade = F.t < 5 ? 1 : clampf(1 - (F.t - 5) / 2.0f, 0, 1);

		update_sun();
		update_camera();
		update_dust();
		frame();
		swap();
	}
	sceGuTerm();
	sceKernelExitGame();
	return 0;
}
