/*
 * Lux Aeterna -- ten churches, and the sun coming through their glass.
 *
 * L and R walk from one to the next. Each is built when you arrive: the window
 * is fired pane by pane, the room is raised around it, and a camera takes it in
 * shot by shot. Bach plays underneath.
 *
 * What the GE is asked to do each frame:
 *   - hardware lighting: four lights (window glow, floor bounce, candles with
 *     specular, cool fill), attenuation, material colours
 *   - vertex fog for the depth of the nave
 *   - a planar reflection in the polished floor (mirrored model matrix)
 *   - texture projection: the glass cast onto floor, walls and piers by the
 *     texture matrix (GU_TEXTURE_MATRIX + GU_POSITION), lit by N.L
 *   - colour doubling (GU_FRAGMENT_2X) so glass and light can over-expose
 *   - volumetric light shafts from 56 additive slices of the window
 *   - dust motes as 3D point sprites, coloured by the shaft they drift in
 *   - candle flames and halos as additive sprites
 *   - a hardware-tessellated Bezier vault
 *   - mip-mapped textures, alpha test, scissor, clip planes
 *   - bloom: render-to-texture, a bright pass by reverse-subtract blending,
 *     three downsampled levels with Kawase blur, additive composite
 *   - a multiplicative vignette, and a font with alpha blending for the HUD
 * Matrices go through libpspgum_vfpu, i.e. the VFPU.
 */
#include "cathedral.h"
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspaudio.h>
#include <pspmp3.h>
#include <psputility.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

PSP_MODULE_INFO("LuxAeterna", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

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

/* --------------------------------------------------------------- state */
typedef struct {
	float t, dt;
	float tau;
	V3 d;
	C3 sun;
	float I;
	float L;
	V3 eye, ctr, camR, camU;
	float flick;
	float glassZ;
} Frame;
static Frame F;
static int optBloom = 1, optShafts = 1, optDust = 1, optPause = 0, optAuto = 1, optHelp = 0;
static float camTh = 0.3f, camDist = 11.5f, camH = 1.9f, camTY = 4.4f;
static float hudFade = 1.0f, cardFade = 0.0f;
static int chIdx = 1;
static u32 poolBase;
static float tourT;
static int tourShot;

static Tex tDot, tFlame, tVig, tFont;

#define NDUST 520
typedef struct { V3 p, v; float life, max, ph, sz; } Dust;
static Dust dust[NDUST];

static inline V3 glass_point(float u, float v)
{
	float s = CH->winW * 0.5f;
	return v3((u * 2 - 1) * s, CH->winY + (1 - v) * unitsY * s, F.glassZ);
}
static inline void to_window(V3 p, float *u, float *v)
{
	float s = CH->winW * 0.5f;
	float k = (p.z - F.glassZ) / F.d.z;
	float hx = p.x - k * F.d.x, hy = p.y - k * F.d.y;
	*u = hx / (2 * s) + 0.5f;
	*v = 1 - (hy - CH->winY) / (unitsY * s);
}

static void dust_spawn(Dust *q)
{
	q->p = v3(0, -99, 0);
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
	float az = CH->sunAz + 0.22f - 0.44f * F.tau;
	float el = CH->sunEl + 0.16f * sinf(PI * F.tau);
	F.d = v3(-cosf(el) * sinf(az), -sinf(el), cosf(el) * cosf(az));
	float warm = 1 - smooth(0.5f, 0.85f, el);
	F.sun = cmix(c3(1.0f, 0.97f, 0.90f), c3(1.0f, 0.74f, 0.46f), warm);
	float cl = 0.5f + 0.5f * sinf(F.t * 0.21f) * sinf(F.t * 0.13f + 1.3f);
	float dip = smooth(0.75f, 1.0f, 0.5f + 0.5f * sinf(F.t * 0.071f + 2.0f));
	F.I = (0.80f + 0.20f * cl) * (1 - 0.35f * dip) * (0.85f + 0.15f * smooth(0.5f, 0.85f, el));
	F.L = (CH->winY + CH->winH) / -F.d.y;
	F.flick = 0.82f + 0.10f * sinf(F.t * 13.0f) * sinf(F.t * 7.3f + 0.5f) + 0.06f * sinf(F.t * 29.0f + 1.0f) + 0.04f * sinf(F.t * 3.1f);
}

/* Where the sun lands on the floor: the middle of the window, followed down. */
static V3 light_patch(void)
{
	float cy = CH->winY + CH->winH * 0.5f;
	float t = cy / -F.d.y;
	return v3(F.d.x * t, 0, F.glassZ + F.d.z * t);
}

/* ---------------------------------------------------------- camera tour */
typedef struct { V3 e0, c0, e1, c1; float dur; } Move;
static Move tour[4];
static void plan_tour(void)
{
	float h = CH->navHalf, len = CH->navLen;
	float wy = CH->winY + CH->winH * 0.45f;
	V3 patch = light_patch();
	Move m0 = {v3(h * 0.45f, 1.75f, len * 0.72f), v3(0, wy, 0),
	           v3(h * 0.28f, 2.10f, len * 0.42f), v3(0, wy * 0.96f, 0), 15.0f};
	Move m1 = {v3(-h * 0.80f, 1.35f, len * 0.30f), v3(patch.x * 0.4f, CH->winY + CH->winH * 0.25f, 1.0f),
	           v3(-h * 0.45f, 2.30f, len * 0.52f), v3(patch.x * 0.6f, CH->winY + CH->winH * 0.60f, 0.5f), 14.0f};
	Move m2 = {vadd(patch, v3(1.8f, 1.30f, 3.4f)), vadd(patch, v3(0, 0.10f, 0)),
	           vadd(patch, v3(-0.6f, 1.05f, 2.2f)), vadd(patch, v3(0.2f, 0.15f, -0.4f)), 12.0f};
	Move m3 = {v3(h * 0.12f, 1.65f, len * 0.46f), v3(0, CH->apexH * 0.80f, len * 0.12f),
	           v3(-h * 0.16f, 2.40f, len * 0.30f), v3(0, CH->apexH * 0.62f, 0.0f), 13.0f};
	tour[0] = m0; tour[1] = m1; tour[2] = m2; tour[3] = m3;
}

static void update_camera(void)
{
	if (optAuto) {
		tourT += F.dt;
		if (tourT > tour[tourShot].dur) { tourT = 0; tourShot = (tourShot + 1) & 3; }
		Move *m = &tour[tourShot];
		float k = smooth(0, 1, tourT / m->dur);
		float drift = 0.06f * sinf(F.t * 0.31f);
		F.eye = v3(mixf(m->e0.x, m->e1.x, k) + drift, mixf(m->e0.y, m->e1.y, k) + 0.04f * sinf(F.t * 0.23f), mixf(m->e0.z, m->e1.z, k));
		F.ctr = v3(mixf(m->c0.x, m->c1.x, k), mixf(m->c0.y, m->c1.y, k), mixf(m->c0.z, m->c1.z, k));
	} else {
		F.ctr = v3(0, camTY, 0.5f);
		F.eye = v3(camDist * sinf(camTh), camH, CH->navLen * 0.05f + camDist * cosf(camTh));
	}
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
	float wy = CH->winY + CH->winH * 0.5f;
	ScePspFVector3 p0 = {0, wy * my, 1.1f};
	sceGuLight(0, GU_POINTLIGHT, GU_DIFFUSE, &p0);
	sceGuLightColor(0, GU_DIFFUSE, lcol(c3(avgGlass.r * 0.9f + 0.1f, avgGlass.g * 0.9f + 0.1f, avgGlass.b * 0.9f + 0.1f), 1.3f * I));
	sceGuLightAtt(0, 0.35f, 0.0f, 0.16f);
	V3 patch = light_patch();
	ScePspFVector3 p1 = {patch.x, 0.6f * my, patch.z};
	sceGuLight(1, GU_POINTLIGHT, GU_DIFFUSE, &p1);
	sceGuLightColor(1, GU_DIFFUSE, lcol(c3((avgGlass.r + F.sun.r) * 0.5f, (avgGlass.g + F.sun.g) * 0.5f, (avgGlass.b + F.sun.b) * 0.5f), 0.55f * I));
	sceGuLightAtt(1, 0.6f, 0.0f, 0.10f);
	ScePspFVector3 p2 = {CH->navHalf * 0.52f + 0.6f, 1.25f * my, CH->navLen * 0.26f + 0.3f};
	sceGuLight(2, GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, &p2);
	sceGuLightColor(2, GU_DIFFUSE, lcol(c3(1.0f, 0.55f, 0.22f), CH->candles ? F.flick : 0.0f));
	sceGuLightColor(2, GU_SPECULAR, lcol(c3(1.0f, 0.7f, 0.4f), CH->candles ? F.flick : 0.0f));
	sceGuLightAtt(2, 0.25f, 0.35f, 0.32f);
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
static inline void draw_mesh(const Mesh *m)
{
	if (m->ni) sceGumDrawArray(GU_TRIANGLES, VL_FMT | GU_INDEX_16BIT, m->ni, m->ix, m->v);
}

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
	if (CH->candles) {
		material(0xff2a3a52, 0xff303030);
		draw_mesh(&mRack);
		sceGuDisable(GU_TEXTURE_2D);
		material(0xffc8e4f2, 0xff606060);
		draw_mesh(&mCandles);
		sceGuEnable(GU_TEXTURE_2D);
	}
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
	v[0] = (VT){0, 0, col, a.x, a.y, F.glassZ};
	v[1] = (VT){1, 0, col, b.x, a.y, F.glassZ};
	v[2] = (VT){0, 1, col, a.x, b.y, F.glassZ};
	v[3] = (VT){1, 1, col, b.x, b.y, F.glassZ};
	sceGumDrawArray(GU_TRIANGLE_STRIP, VT_FMT, 4, 0, v);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_FRAGMENT_2X);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static void draw_projection(void)
{
	float s = CH->winW * 0.5f, hy = unitsY * s;
	float kx = F.d.x / F.d.z, ky = F.d.y / F.d.z;
	ScePspFMatrix4 m;
	memset(&m, 0, sizeof(m));
	m.x.x = 1 / (2 * s);
	m.y.y = -1 / hy;
	m.z.x = -kx / (2 * s);
	m.z.y = ky / hy;
	m.w.x = F.glassZ * kx / (2 * s) + 0.5f;
	m.w.y = 1 + CH->winY / hy - F.glassZ * ky / hy;
	m.w.z = 1;
	m.w.w = 1;
	sceGuSetMatrix(GU_TEXTURE, &m);
	sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
	sceGuTexProjMapMode(GU_POSITION);

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
		if (!pass) sceGuBlendFunc(GU_ADD, 0, GU_FIX, 0, 0xffffff);
		else sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x6c6c6c, 0xffffff);
		draw_mesh(&mFloor);
		draw_mesh(&mPillars);
		draw_mesh(&mSideL);
		draw_mesh(&mSideR);
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
				float sc = w * dens;
				pg[gy][gx] = p;
				cg[gy][gx] = pack(F.sun.r * sc, F.sun.g * sc, F.sun.b * sc, 1);
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
		V3 a = vadd(vsub(q->p, vmul(F.camR, q->sz)), vmul(F.camU, q->sz));
		V3 b = vsub(vadd(q->p, vmul(F.camR, q->sz)), vmul(F.camU, q->sz));
		v[n++] = (VT){0, 0, col, a.x, a.y, a.z};
		v[n++] = (VT){1, 1, col, b.x, b.y, b.z};
	}
	if (n) sceGumDrawArray(GU_SPRITES, VT_FMT, n, 0, v);
}

static void draw_flames(int mirror)
{
	if (!nFlames) return;
	float my = mirror ? -1.0f : 1.0f;
	bind(&tDot);
	VT *v = sceGuGetMemory(nFlames * 2 * sizeof(VT));
	for (int c = 0; c < nFlames; c++) {
		float fl = F.flick * (0.9f + 0.1f * sinf(F.t * 17 + c * 2.1f));
		V3 p = flamePos[c]; p.y = (p.y + 0.03f) * my;
		float s = 0.16f;
		float k = mirror ? 0.5f : 1.0f;
		u32 col = pack(0.55f * fl * k, 0.26f * fl * k, 0.08f * fl * k, 1);
		V3 a = vadd(vsub(p, vmul(F.camR, s)), vmul(F.camU, s));
		V3 b = vsub(vadd(p, vmul(F.camR, s)), vmul(F.camU, s));
		v[c * 2] = (VT){0, 0, col, a.x, a.y, a.z};
		v[c * 2 + 1] = (VT){1, 1, col, b.x, b.y, b.z};
	}
	sceGumDrawArray(GU_SPRITES, VT_FMT, nFlames * 2, 0, v);
	bind(&tFlame);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	v = sceGuGetMemory(nFlames * 2 * sizeof(VT));
	for (int c = 0; c < nFlames; c++) {
		float fl = 0.85f + 0.15f * sinf(F.t * 21 + c * 1.7f) * sinf(F.t * 9 + c);
		float h = 0.075f * fl, w = 0.018f;
		V3 p = flamePos[c];
		p.x += 0.006f * sinf(F.t * 5 + c * 3.3f);
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
	sceGumDrawArray(GU_SPRITES, VT_FMT, nFlames * 2, 0, v);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static void set_camera(void)
{
	sceGumMatrixMode(GU_PROJECTION);
	sceGumLoadIdentity();
	sceGumPerspective(54.0f, 480.0f / 272.0f, 0.4f, 160.0f);
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

	sceGuEnable(GU_FOG);
	set_lights(0);
	bind(&tFloor);
	material(0xffffffff, 0xff9a9a9a);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	draw_mesh(&mFloor);
	sceGuDisable(GU_BLEND);

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

static void bind_vram(u32 off, int w, int h, int stride)
{
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, w, h, stride, vram(off));
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexFlush();
}

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

	clear_target(RT_A, 256, 144, 256);
	set_target(RT_A, 240, 136, 256);
	bind_vram((u32)drawBuf, 512, 512, BUF_W);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	blit(0, 0, 480, 272, 0, 0, 240, 136, 0xffffffff);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	rect(0, 0, 240, 136, 0xff585858);
	sceGuDisable(GU_BLEND);

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
	sceGuBlendFunc(GU_ADD, GU_FIX, 0, 0, 0);
	blit(0.5f, 0.5f, 63.5f, 63.5f, 0, 0, SCR_W, SCR_H, 0xffffffff);
	sceGuDisable(GU_BLEND);
}

/* ----------------------------------------------------------------- HUD */
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
		text(240 - 11 * 8, 20, "LUX AETERNA", acol(0xffe8f4ff, a), 2);
		text(240 - 30 * 4, 42, "zehn Kirchen, ein Sonnenstrahl", acol(0xffb8c8d8, a * 0.9f), 1);
	}
	if (cardFade > 0.01f) {
		float a = smooth(0, 0.35f, cardFade);
		char num[24];
		sprintf(num, "%d / %d", chIdx + 1, NCHURCH);
		text(20, 206, CH->name, acol(0xfff0f4ff, a), 2);
		text(20, 226, CH->place, acol(0xffa8bcd0, a * 0.9f), 1);
		text(20, 238, CH->work, acol(0xff90b0c8, a * 0.85f), 1);
		text(444 - strlen(num) * 8, 206, num, acol(0xff88a0b8, a * 0.8f), 1);
		text(20, 254, "L / R : Kirche wechseln", acol(0xff70889c, a * 0.7f), 1);
	}
	if (optHelp) {
		sceGuDisable(GU_TEXTURE_2D);
		VT *v = sceGuGetMemory(2 * sizeof(VT));
		v[0] = (VT){0, 0, 0xa0000000, 12, 12, 0};
		v[1] = (VT){0, 0, 0xa0000000, 468, 150, 0};
		sceGuDrawArray(GU_SPRITES, V2_FMT, 2, 0, v);
		sceGuEnable(GU_TEXTURE_2D);
		u32 w = 0xffe0e8f0, g = 0xff7fd0ff, on = 0xff80ff90, off = 0xff6060ff;
		text(20, 18, "L / R       Kirche wechseln", w, 1);
		text(20, 28, "Analog      Kamera selbst fuehren", w, 1);
		text(20, 38, "Pfeile      naeher/weiter, Tageszeit", w, 1);
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

/* ------------------------------------------------------------- sprites */
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
			float fy = (y + 0.5f) / 64.0f;
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
			float dx = (x - 31.5f) / 32, dy = (y - 31.5f) / 32, r = sqrtf(dx * dx + dy * dy * 0.85f);
			float v = 1 - 0.62f * smooth(0.42f, 1.25f, r);
			tVig.px[0][y * 64 + x] = pack(v, v * 0.985f, v * 0.955f, 1);
		}
}

/* ---------------------------------------------------------------- audio
 * Bach, "Ich ruf zu dir, Herr Jesu Christ" BWV 639, played by Yeonju Sarah
 * Kim on the Heilig-Geist organ of Salzburg cathedral, CC0 -- see CREDITS.md.
 * The Media Engine decodes it; a thread of our own feeds the channel, one step
 * under the interface, the way the devbook says. */
extern const unsigned char music_mp3[];
extern const unsigned char music_mp3_end[];
static SceUID musicThread = -1;
static volatile int musicRun = 1;

static int music_main(SceSize args, void *argp)
{
	static unsigned char __attribute__((aligned(64))) mp3Buf[16 * 1024];
	static short __attribute__((aligned(64))) pcmBuf[1152 * 2 * 2];
	const unsigned char *blob = music_mp3;
	int len = (int)(music_mp3_end - music_mp3);

	if (sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC) < 0) return 0;
	if (sceUtilityLoadModule(PSP_MODULE_AV_MP3) < 0) return 0;
	if (sceMp3InitResource() < 0) return 0;

	SceMp3InitArg arg;
	memset(&arg, 0, sizeof(arg));
	arg.mp3StreamStart = 0;
	arg.mp3StreamEnd = len;
	arg.mp3Buf = mp3Buf;
	arg.mp3BufSize = sizeof(mp3Buf);
	arg.pcmBuf = (SceUChar8 *)pcmBuf;
	arg.pcmBufSize = sizeof(pcmBuf);
	int h = sceMp3ReserveMp3Handle(&arg);
	if (h < 0) return 0;

	for (int i = 0; i < 3; i++) {
		SceUChar8 *dst; SceInt32 need, pos;
		if (sceMp3GetInfoToAddStreamData(h, &dst, &need, &pos) < 0) break;
		if (pos + need > len) need = len - pos;
		if (need <= 0) break;
		memcpy(dst, blob + pos, need);
		sceMp3NotifyAddStreamData(h, need);
	}
	if (sceMp3Init(h) < 0) { sceMp3ReleaseMp3Handle(h); return 0; }

	int chans = sceMp3GetMp3ChannelNum(h);
#ifdef CAPTURE_AUDIO
	{
		char line[200];
		int n = sprintf(line, "len %d init ok rate %d chans %d\n", len, (int)sceMp3GetSamplingRate(h), chans);
		SceUID fd = sceIoOpen("host0:/audio.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
		if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
	}
#endif
	int fmt = chans == 1 ? PSP_AUDIO_FORMAT_MONO : PSP_AUDIO_FORMAT_STEREO;
	int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152, fmt);
	if (ch < 0) { sceMp3ReleaseMp3Handle(h); return 0; }

	const int vol = 0x3800;
	while (musicRun) {
		SceShort16 *buf = 0;
		int bytes = sceMp3Decode(h, &buf);
		if (bytes <= 0) {
			if (sceMp3CheckStreamDataNeeded(h) > 0) {
				SceUChar8 *dst; SceInt32 need, pos;
				if (sceMp3GetInfoToAddStreamData(h, &dst, &need, &pos) >= 0) {
					if (pos + need > len) need = len - pos;
					if (need > 0) {
						memcpy(dst, blob + pos, need);
						sceMp3NotifyAddStreamData(h, need);
						continue;
					}
				}
			}
			sceMp3ResetPlayPosition(h);      /* round again */
			sceKernelDelayThread(2000);
			continue;
		}
#ifdef CAPTURE_AUDIO
		{
			static int frames = 0;
			if (++frames == 20) {
				char line[200];
				int n = sprintf(line, "decoded %d frames, last %d bytes, sum %d\n", frames, bytes, (int)buf[0] + (int)buf[1] + (int)buf[100]);
				SceUID fd = sceIoOpen("host0:/audio.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
				if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
			}
		}
#endif
		sceAudioOutputPannedBlocking(ch, vol, vol, buf);
	}
	sceAudioChRelease(ch);
	sceMp3ReleaseMp3Handle(h);
	sceMp3TermResource();
	return 0;
}

static void music_start(void)
{
	musicThread = sceKernelCreateThread("music", music_main, 0x21, 0x8000, 0, 0);
	if (musicThread >= 0) sceKernelStartThread(musicThread, 0, 0);
}
static void music_stop(void)
{
	musicRun = 0;
	if (musicThread >= 0) sceKernelWaitThreadEnd(musicThread, 0);
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
		sceGuDisable(GU_DEPTH_TEST);
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

static void enter_church(int idx, int quiet)
{
	chIdx = ((idx % NCHURCH) + NCHURCH) % NCHURCH;
	if (!quiet) loading("Das Glas wird gebrannt ...");
	pool_release(poolBase);
	glass_select(chIdx);
	F.glassZ = -0.55f;
	F.tau = 0.35f;
	make_glass();
	build_church();
	sceKernelDcacheWritebackAll();
	update_sun();
	plan_tour();
	tourT = 0;
	tourShot = 0;
	camDist = CH->navLen * 0.45f;
	camH = 1.8f;
	camTY = CH->winY + CH->winH * 0.45f;
	update_camera();
	for (int i = 0; i < NDUST; i++) { dust_spawn(&dust[i]); dust[i].life = frand() * dust[i].max; }
	cardFade = 1.0f;
}

#if defined(CAPTURE) || defined(CAPTURE_VIDEO)
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
#endif

int main(void)
{
	setup_callbacks();
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	make_font();
	make_sprites();
	sceKernelDcacheWritebackAll();
	gu_init();
	poolBase = pool_mark();

	F.t = 0; F.dt = 1 / 60.0f;

#ifdef CAPTURE_AUDIO
	music_start();
	sceKernelDelayThread(4000000);
	musicRun = 0;
	sceKernelDelayThread(200000);
	sceGuTerm();
	sceKernelExitGame();
	return 0;
#endif
#ifdef CAPTURE
	/* one frame per church, for the shots in the README */
	for (int s = 0; s < NCHURCH && running; s++) {
		F.t = 24.0f;
		optAuto = 1;
		enter_church(s, 1);
		hudFade = 0;
		cardFade = 1.0f;
		tourShot = s & 3;
		tourT = tour[tourShot].dur * 0.55f;
		F.dt = 1 / 60.0f;
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
#elif defined(CAPTURE_VIDEO)
	/* ten seconds for the catalog clip: three churches, one shot each */
	{
		int order[3] = {1, 0, 8};
		int shots[3] = {0, 1, 3};
		int n = 0;
		for (int s = 0; s < 3 && running; s++) {
			enter_church(order[s], 1);
			hudFade = 0;
			for (int i = 0; i < 100 && running; i++, n++) {
				F.dt = 1 / 30.0f;
				F.t = 12.0f + n * F.dt;
				tourShot = shots[s];
				tourT = tour[tourShot].dur * (0.25f + 0.5f * i / 100.0f);
				cardFade = clampf(1.5f - i / 45.0f, 0, 1);
				update_sun();
				update_camera();
				update_dust();
				frame();
				char path[64];
				sprintf(path, "host0:/vid/f%03d.bmp", n);
				save_bmp(path);
				swap();
			}
		}
	}
	sceGuTerm();
	sceKernelExitGame();
	return 0;
#else
	enter_church(1, 1);
	music_start();

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
		if (pressed & PSP_CTRL_LTRIGGER) { enter_church(chIdx - 1, 0); last = sceKernelGetSystemTimeLow(); }
		if (pressed & PSP_CTRL_RTRIGGER) { enter_church(chIdx + 1, 0); last = sceKernelGetSystemTimeLow(); }
		if (pressed & PSP_CTRL_TRIANGLE) optBloom ^= 1;
		if (pressed & PSP_CTRL_CIRCLE) optShafts ^= 1;
		if (pressed & PSP_CTRL_SQUARE) optDust ^= 1;
		if (pressed & PSP_CTRL_CROSS) optPause ^= 1;
		if (pressed & PSP_CTRL_START) { optAuto ^= 1; if (optAuto) tourT = 0; }
		if (pressed & PSP_CTRL_SELECT) optHelp ^= 1;
		float ax = (pad.Lx - 128) / 128.0f, ay = (pad.Ly - 128) / 128.0f;
		if (fabsf(ax) < 0.2f) ax = 0;
		if (fabsf(ay) < 0.2f) ay = 0;
		if (ax != 0 || ay != 0 || (pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN))) optAuto = 0;
		if (!optAuto) {
			camTh = clampf(camTh + ax * 0.9f * F.dt, -1.2f, 1.2f);
			camH = clampf(camH - ay * 2.5f * F.dt, 0.6f, CH->apexH * 0.7f);
			if (pad.Buttons & PSP_CTRL_UP) camDist = clampf(camDist - 4 * F.dt, 2.0f, CH->navLen);
			if (pad.Buttons & PSP_CTRL_DOWN) camDist = clampf(camDist + 4 * F.dt, 2.0f, CH->navLen);
		}
		if (pad.Buttons & PSP_CTRL_LEFT) F.tau = clampf(F.tau - 0.15f * F.dt, 0, 1);
		if (pad.Buttons & PSP_CTRL_RIGHT) F.tau = clampf(F.tau + 0.15f * F.dt, 0, 1);
		if (!optPause && !(pad.Buttons & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			static float dir = 1;
			F.tau += dir * F.dt / 150.0f;
			if (F.tau > 1) { F.tau = 1; dir = -1; }
			if (F.tau < 0) { F.tau = 0; dir = 1; }
		}
		hudFade = F.t < 5 ? 1 : clampf(1 - (F.t - 5) / 2.0f, 0, 1);
		cardFade = clampf(cardFade - F.dt / 7.0f, 0, 1);

		update_sun();
		update_camera();
		update_dust();
		frame();
		swap();
	}
	music_stop();
	sceGuTerm();
	sceKernelExitGame();
	return 0;
#endif
}
