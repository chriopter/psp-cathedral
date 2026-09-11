/* Maths, noise, the arena and texture plumbing. */
#include "cathedral.h"
#include <string.h>

u32 pack(float r, float g, float b, float a)
{
	int R = (int)(clampf(r, 0, 1) * 255 + 0.5f), G = (int)(clampf(g, 0, 1) * 255 + 0.5f);
	int B = (int)(clampf(b, 0, 1) * 255 + 0.5f), A = (int)(clampf(a, 0, 1) * 255 + 0.5f);
	return ((u32)A << 24) | ((u32)B << 16) | ((u32)G << 8) | (u32)R;
}

static u32 rng = 0x2545F491u;
float frand(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return (rng & 0xffffff) * (1.0f / 16777216.0f);
}

u32 ihash(int x, int y, int s)
{
	u32 h = (u32)x * 0x8da6b343u ^ (u32)y * 0xd8163841u ^ (u32)s * 0xcb1ab31fu;
	h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
	return h;
}
float hf(int x, int y, int s) { return (ihash(x, y, s) & 0xffffff) * (1.0f / 16777216.0f); }

float vnoise(float x, float y, int per, int seed)
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

float fbm(float x, float y, int per, int oct, int seed)
{
	float s = 0, a = 0.5f, n = 0;
	for (int i = 0; i < oct; i++) {
		s += a * vnoise(x, y, per, seed + i); n += a;
		x *= 2; y *= 2; if (per) per *= 2; a *= 0.5f;
	}
	return s / n;
}

static inline void vor_pt(int gx, int gy, float *fx, float *fy)
{
	*fx = gx + 0.12f + 0.76f * hf(gx, gy, 11);
	*fy = gy + 0.12f + 0.76f * hf(gx, gy, 12);
}
Vor voronoi(float x, float y, float cell)
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
			if (!i && !j) continue;
			int gx = bi + i, gy = bj + j;
			float fx, fy; vor_pt(gx, gy, &fx, &fy);
			float mx = (fx + bx) * 0.5f - px, my = (fy + by) * 0.5f - py;
			float nx = fx - bx, ny = fy - by, nl = sqrtf(nx * nx + ny * ny);
			float d = (mx * nx + my * ny) / nl;
			if (d < edge) edge = d;
		}
	Vor v = {edge * cell, ihash(bi, bj, 13)};
	return v;
}

float sdArch(float x, float y, float a, float y0, float ys)
{
	if (y <= ys) {
		float dx = fabsf(x) - a, dy = y0 - y;
		return dx > dy ? dx : dy;
	}
	float d1 = sqrtf((x + a) * (x + a) + (y - ys) * (y - ys)) - 2 * a;
	float d2 = sqrtf((x - a) * (x - a) + (y - ys) * (y - ys)) - 2 * a;
	return d1 > d2 ? d1 : d2;
}

float sdRound(float x, float y, float a, float y0, float ys)
{
	if (y <= ys) {
		float dx = fabsf(x) - a, dy = y0 - y;
		return dx > dy ? dx : dy;
	}
	return sqrtf(x * x + (y - ys) * (y - ys)) - a;
}

float sdBox(float x, float y, float bx, float by)
{
	float dx = fabsf(x) - bx, dy = fabsf(y) - by;
	float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
	float in = dx > dy ? dx : dy;
	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0);
}

/* One arena. Everything a church owns is allocated after its mark, and the
 * mark is wound back when another church is built -- nothing is freed
 * piecemeal, which is the whole point. */
static unsigned char __attribute__((aligned(64))) pool[5 * 1024 * 1024];
static u32 poolUsed;
void *palloc(u32 n)
{
	n = (n + 63) & ~63u;
	if (poolUsed + n > sizeof(pool)) return 0;
	void *p = pool + poolUsed;
	poolUsed += n;
	return p;
}
u32 pool_mark(void) { return poolUsed; }
void pool_release(u32 mark) { poolUsed = mark; }

Tex new_tex(int w, int h, int lv)
{
	Tex t; t.w = w; t.h = h; t.lv = lv;
	for (int i = 0; i < lv; i++) t.px[i] = palloc((w >> i) * (h >> i) * 4);
	return t;
}

void mip_build(Tex *t, int alphaCut)
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

void bind(const Tex *t)
{
	sceGuTexMode(GU_PSM_8888, t->lv - 1, 0, 0);
	for (int i = 0; i < t->lv; i++) sceGuTexImage(i, t->w >> i, t->h >> i, t->w >> i, t->px[i]);
	sceGuTexFilter(t->lv > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR, GU_LINEAR);
	sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
}

Mesh mesh_new(int nv, int ni)
{
	Mesh m; m.v = palloc(nv * sizeof(VL)); m.ix = palloc(ni * 2); m.nv = m.ni = 0;
	return m;
}
void vput(Mesh *m, float u, float v, V3 n, V3 p)
{
	VL *q = &m->v[m->nv++];
	q->u = u; q->v = v; q->nx = n.x; q->ny = n.y; q->nz = n.z; q->x = p.x; q->y = p.y; q->z = p.z;
}
void grid_idx(Mesh *m, int base, int cols, int rows)
{
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++) {
			int a = base + r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
			m->ix[m->ni++] = a; m->ix[m->ni++] = d; m->ix[m->ni++] = b;
			m->ix[m->ni++] = b; m->ix[m->ni++] = d; m->ix[m->ni++] = e;
		}
}
void quad_idx(Mesh *m, int base)
{
	m->ix[m->ni++] = base; m->ix[m->ni++] = base + 1; m->ix[m->ni++] = base + 2;
	m->ix[m->ni++] = base; m->ix[m->ni++] = base + 2; m->ix[m->ni++] = base + 3;
}
