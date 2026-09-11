/* The glass: ten windows, each drawn after a real one but re-invented here in
 * code. Nothing is traced from a photograph -- the tracery, the fields and the
 * palette are described as distance functions and noise, so every pane is this
 * program's own work.
 *
 * Three textures come out of one pass: the window itself (256x512, mip-mapped,
 * alpha 0 outside the opening), a light map for what the sun casts on the floor
 * (128x256) and a blurrier one for the shafts in the air (64x128). */
#include "cathedral.h"
#include <string.h>
#include <stdio.h>
#include <pspiofilemgr.h>

Tex tGlass, tLight, tShaft;
C3 avgGlass;
float unitsY = 4.0f;
C3 BLUE, DBLUE, RUBY, GOLD, AMBER, GREEN, PURPLE, WHITE, PINK, SKY;

#define LH 0.0055f
#define GSCALE 0.80f

static const Church *W;      /* the church being fired */
static float A0v, Y00v, YSv, RARCv, CCY, RCIRC;
static float cellSize = 0.085f;
static int noLead;

/* ---------------------------------------------------------- the opening */
static float sdArchR(float x, float y, float a, float y0, float ys, float R)
{
	if (y <= ys) {
		float dx = fabsf(x) - a, dy = y0 - y;
		return dx > dy ? dx : dy;
	}
	float c = R - a;
	float d1 = sqrtf((x + c) * (x + c) + (y - ys) * (y - ys)) - R;
	float d2 = sqrtf((x - c) * (x - c) + (y - ys) * (y - ys)) - R;
	return d1 > d2 ? d1 : d2;
}

float win_sd(float X, float Y)
{
	switch (W->outline) {
	case OUTL_CIRCLE: return sdCirc(X, Y - CCY, RCIRC);
	case OUTL_ROUND: return sdRound(X, Y, A0v, Y00v, YSv);
	default: return sdArchR(X, Y, A0v, Y00v, YSv, RARCv);
	}
}

float win_outline(float th, float off)
{
	float dx = cosf(th), dy = sinf(th), lo = 0, hi = 14;
	for (int i = 0; i < 28; i++) {
		float m = (lo + hi) * 0.5f;
		if (win_sd(dx * m, CCY + dy * m) < off) lo = m; else hi = m;
	}
	return (lo + hi) * 0.5f;
}

void glass_select(int idx)
{
	W = CH = &CHURCH[idx];
	BLUE = CH->pal[0]; DBLUE = CH->pal[1]; RUBY = CH->pal[2]; GOLD = CH->pal[3];
	AMBER = CH->pal[4]; GREEN = CH->pal[5]; PURPLE = CH->pal[6]; WHITE = CH->pal[7];
	PINK = cmix(RUBY, WHITE, 0.55f);
	SKY = cmix(BLUE, WHITE, 0.35f);
	unitsY = CH->winH / CH->winW * 2.0f;
	A0v = 0.985f;
	Y00v = 0.015f;
	CCY = unitsY * 0.45f;
	RCIRC = (unitsY * 0.5f < 1.0f ? unitsY * 0.5f : 1.0f) - 0.02f;
	float apex = unitsY - 0.015f;
	if (CH->outline == OUTL_ROUND) {
		YSv = apex - A0v;
	} else if (CH->outline == OUTL_CIRCLE) {
		CCY = unitsY * 0.5f;
		YSv = 0;
	} else {
		float rise = CH->rise > 0 ? CH->rise * A0v : A0v * 1.7320508f;
		RARCv = (A0v * A0v + rise * rise) / (2 * rise);
		YSv = apex - rise;
	}
	cellSize = 0.085f;
	noLead = 0;
	if (CH->win == WIN_JEWEL) cellSize = 0.055f;
	if (CH->win == WIN_GRISAILLE) cellSize = 0.07f;
	if (CH->win == WIN_ALABASTER) noLead = 1;
	if (CH->win == WIN_NOUVEAU || CH->win == WIN_VORTEX) cellSize = 0.13f;
}

/* ------------------------------------------------------------- fragments */
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

/* a standing figure, the way a glazier draws one: halo, head, robe */
static int figure(float x, float y, float scale, C3 robe, C3 *c)
{
	x /= scale; y /= scale;
	float halo = sdCirc(x, y - 0.62f, 0.20f);
	float head = sdCirc(x, y - 0.62f, 0.115f);
	float body = fmaxf(sdBox(x, y - 0.02f, 0.26f - 0.10f * y, 0.48f), -sdCirc(x, y - 0.62f, 0.125f));
	float arms = sdBox(x, y - 0.28f, 0.36f, 0.055f);
	float d = fminf(body, arms);
	if (fabsf(head) < LH * 1.4f) return LEAD;
	if (head < 0) { *c = PINK; return GLASS; }
	if (fabsf(halo) < LH * 1.4f) return LEAD;
	if (halo < 0) { *c = GOLD; return GLASS; }
	if (fabsf(d) < LH * 1.4f) return LEAD;
	if (d < 0) {
		float fold = sinf(x * 22.0f + y * 3.0f);
		*c = fabsf(fold) < 0.12f ? cmul(robe, 0.7f) : robe;
		if (y < -0.34f && fabsf(x) < 0.2f) *c = cmix(robe, WHITE, 0.4f);
		return GLASS;
	}
	return 0; /* nothing here: the caller paints the ground */
}

static int pearl_border(float e, float y, float width, C3 a, C3 b, C3 *c)
{
	if (e < LH || e > width - LH) return LEAD;
	float s = y * 9.0f, fs = s - floorf(s);
	float dp = sqrtf((e - width * 0.5f) * (e - width * 0.5f) + (fs - 0.5f) * (fs - 0.5f) / 81.0f);
	if (dp < width * 0.22f) { *c = WHITE; return GLASS; }
	if (dp < width * 0.22f + 2 * LH) return LEAD;
	if ((fs < 0.5f ? fs : 1 - fs) / 9.0f < LH) return LEAD;
	*c = (((int)floorf(s)) & 1) ? a : b;
	return GLASS;
}

static int diaper(float x, float y, float pitch, C3 *c)
{
	float u = (x + y) / pitch, v = (x - y) / pitch;
	float fu = u - floorf(u), fv = v - floorf(v);
	float du = fu < 0.5f ? fu : 1 - fu, dv = fv < 0.5f ? fv : 1 - fv;
	float g = sqrtf(du * du + dv * dv);
	if (g < 0.24f) { *c = (((int)floorf(u) + (int)floorf(v)) & 2) ? GOLD : RUBY; return GLASS; }
	if (g < 0.30f) return LEAD;
	if (du < 0.06f || dv < 0.06f) return LEAD;
	*c = (((int)floorf(u) + (int)floorf(v)) & 1) ? BLUE : DBLUE;
	return GLASS;
}

/* ------------------------------------------------- 1: lancets and a rose */
#define MR 0.26f
#define RING 0.034f
static const float MEDY[3] = {0.52f, 1.12f, 1.70f};
static const int SYM_L[3] = {0, 1, 2}, SYM_R[3] = {3, 4, 5};

static int lancet_field(float x, float y, float e, int side, C3 *c)
{
	if (e < 0.075f) return pearl_border(e, y, 0.075f, RUBY, DBLUE, c);
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
	return diaper(x, y, 0.13f, c);
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

static int rose_field(float x, float y, float R, C3 *c)
{
	float k = R / 0.62f;
	float r = sqrtf(x * x + y * y) / k, a = atan2f(y, x);
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
	int kk = (int)floorf((a + PI) / sw);
	float ac = -PI + (kk + 0.5f) * sw, dl = a - ac;
	if ((sw * 0.5f - fabsf(dl)) * r < 0.014f) return STONE;
	float lx = 0.43f * cosf(ac), ly = 0.43f * sinf(ac);
	float dlobe = sdCirc(x / k - lx, y / k - ly, 0.085f);
	if (dlobe < 0) {
		if (dlobe > -0.016f) return STONE;
		float dc = sdCirc(x / k - lx, y / k - ly, 0.028f);
		if (fabsf(dc) < LH) return LEAD;
		*c = dc < 0 ? WHITE : ((kk & 1) ? GREEN : GOLD);
		return GLASS;
	}
	if (r > 0.43f) {
		if (fabsf(dl) * r < LH) return LEAD;
		*c = (kk & 1) ? PURPLE : SKY;
		return GLASS;
	}
	float dp = sqrtf((r - 0.265f) * (r - 0.265f) + (dl * r) * (dl * r));
	if (dp < 0.02f) { *c = WHITE; return GLASS; }
	if (dp < 0.02f + 2 * LH) return LEAD;
	if (fabsf(dl) * r < LH) return LEAD;
	*c = (kk & 1) ? BLUE : RUBY;
	return GLASS;
}

static int win_lancets_rose(float X, float Y, C3 *c)
{
	const float LCX = 0.48f, AL = 0.43f, YSL = 1.555f, RRr = 0.62f, YC = 3.089f;
	float dl = sdArchR(X + LCX, Y, AL, 0.09f, YSL, 2 * AL);
	if (dl < 0) return dl > -0.05f ? STONE : lancet_field(X + LCX, Y, -dl - 0.05f, 0, c);
	float dr = sdArchR(X - LCX, Y, AL, 0.09f, YSL, 2 * AL);
	if (dr < 0) return dr > -0.05f ? STONE : lancet_field(X - LCX, Y, -dr - 0.05f, 1, c);
	float dro = sdCirc(X, Y - YC, RRr);
	if (dro < 0) return dro > -0.05f ? STONE : rose_field(X, Y - YC, RRr - 0.05f, c);
	int q = quatrefoil(X, Y, 0, 2.14f, 0.11f, c);
	if (!q) q = quatrefoil(X, Y, -0.79f, 2.45f, 0.07f, c);
	if (!q) q = quatrefoil(X, Y, 0.79f, 2.45f, 0.07f, c);
	return q ? q : STONE;
}

/* --------------------------------------- 2: three tall lights of medallions */
static int win_tall_lights(float X, float Y, C3 *c)
{
	const float half = 0.285f;
	for (int k = -1; k <= 1; k++) {
		float cx = k * 0.63f;
		float apex = unitsY - 0.22f, ys = apex - half * 1.7320508f;
		float d = sdArchR(X - cx, Y, half, 0.12f, ys, 2 * half);
		if (d >= 0) continue;
		if (d > -0.035f) return STONE;
		float e = -d - 0.035f;
		float x = X - cx;
		if (e < 0.045f) return pearl_border(e, Y, 0.045f, RUBY, GOLD, c);
		/* a column of quatrefoil medallions on a mosaic ground */
		float pitch = 0.62f;
		float row = floorf((Y - 0.2f) / pitch);
		float dy = Y - (0.2f + row * pitch + pitch * 0.5f);
		int q = quatrefoil(x, dy, 0, 0, 0.245f, c);
		if (q == STONE) return LEAD;                 /* inside glass it is lead */
		if (q == GLASS || q == LEAD) {
			if (q == GLASS) {
				float s = sdCirc(x, dy, 0.045f);
				if (fabsf(s) < LH) return LEAD;
				if (s < 0) { *c = WHITE; return GLASS; }
				int sym = ((int)row + (k + 1)) % 6;
				C3 ground = ((int)row & 1) ? RUBY : cmix(DBLUE, BLUE, 0.4f);
				*c = ground;
				return symbol(sym, x * 1.9f, dy * 1.9f, c);
			}
			return q;
		}
		/* ground: small pieces, a band between the medallions */
		if (fabsf(dy) > pitch * 0.5f - 0.028f) { *c = ((int)row & 1) ? GOLD : GREEN; return GLASS; }
		float u = (x + dy) / 0.075f, v = (x - dy) / 0.075f;
		float fu = u - floorf(u), fv = v - floorf(v);
		if ((fu < 0.5f ? fu : 1 - fu) < 0.1f || (fv < 0.5f ? fv : 1 - fv) < 0.1f) return LEAD;
		*c = (((int)floorf(u) + (int)floorf(v)) & 1) ? BLUE : DBLUE;
		if (((int)floorf(u) * 7 + (int)floorf(v) * 3) % 11 == 0) *c = WHITE;
		return GLASS;
	}
	/* the spandrels between the lights: stone with small openings */
	int q = quatrefoil(X, Y, 0, unitsY - 0.55f, 0.13f, c);
	if (!q) q = quatrefoil(X, Y, -0.63f, unitsY - 0.75f, 0.09f, c);
	if (!q) q = quatrefoil(X, Y, 0.63f, unitsY - 0.75f, 0.09f, c);
	return q ? q : STONE;
}

/* --------------------------------------------- 3: grisaille, five sisters */
static int win_grisaille(float X, float Y, C3 *c)
{
	const float half = 0.165f;
	for (int k = -2; k <= 2; k++) {
		float cx = k * 0.39f;
		float apex = unitsY - 0.1f, ys = apex - half * 1.7320508f;
		float d = sdArchR(X - cx, Y, half, 0.06f, ys, 2 * half);
		if (d >= 0) continue;
		if (d > -0.028f) return STONE;
		float e = -d - 0.028f, x = X - cx;
		if (e < 0.03f) { if (e < LH) return LEAD; *c = cmix(WHITE, GREEN, 0.35f); return GLASS; }
		/* bands of colour crossing a silver-green cross-hatch */
		float bandPitch = 0.62f;
		float by = Y / bandPitch;
		float fb = by - floorf(by);
		if (fb < 0.075f) {
			if (fb < LH * 3) return LEAD;
			*c = (((int)floorf(by)) & 1) ? RUBY : BLUE;
			return GLASS;
		}
		if (fb < 0.135f && fb > 0.09f) { *c = cmix(GOLD, WHITE, 0.4f); return GLASS; }
		/* strapwork: a lozenge grid with a leaf in each */
		float u = (x + Y * 0.9f) / 0.115f, v = (x - Y * 0.9f) / 0.115f;
		float fu = u - floorf(u) - 0.5f, fv = v - floorf(v) - 0.5f;
		float du = fabsf(fu), dv = fabsf(fv);
		if (du > 0.42f || dv > 0.42f) return LEAD;
		float leaf = sqrtf(fu * fu * 1.8f + fv * fv) - (0.16f + 0.14f * sinf((u + v) * 1.7f));
		C3 pale = cmix(WHITE, GREEN, 0.22f + 0.12f * hf((int)floorf(u), (int)floorf(v), 71));
		if (leaf < 0) {
			*c = cmix(pale, GREEN, 0.55f);
			if (leaf > -0.03f) *c = cmul(pale, 0.8f);
		} else {
			*c = pale;
		}
		return GLASS;
	}
	return STONE;
}

/* ------------------------------------------ 4: perpendicular, a grid of lights */
static int win_perp_grid(float X, float Y, C3 *c)
{
	const int COLS = 4, ROWS = 5;
	float x0 = -0.90f, x1 = 0.90f, y0 = 0.08f, y1 = unitsY - 0.62f;
	if (X < x0 || X > x1 || Y < y0) {
		/* the head above the transom: cusped tracery lights */
		if (Y > y1) {
			float hx = X / 0.95f, hy = (Y - y1) / (unitsY - 0.02f - y1);
			if (hy > 1) return STONE;
			float cellw = 0.34f;
			float u = hx / cellw;
			float fu = u - floorf(u) - 0.5f;
			if (fabsf(fu) > 0.40f || hy > 0.92f - fabsf(hx) * 0.55f) return STONE;
			if (hy < 0.06f) return STONE;
			*c = (((int)floorf(u)) & 1) ? RUBY : BLUE;
			if (hy > 0.55f) *c = cmix(*c, GOLD, 0.7f);
			return GLASS;
		}
		return STONE;
	}
	if (Y > y1) {
		float hx = X / 0.95f, hy = (Y - y1) / (unitsY - 0.02f - y1);
		if (hy > 1) return STONE;
		float cellw = 0.34f;
		float u = hx / cellw;
		float fu = u - floorf(u) - 0.5f;
		if (fabsf(fu) > 0.40f || hy > 0.92f - fabsf(hx) * 0.55f || hy < 0.06f) return STONE;
		*c = (((int)floorf(u)) & 1) ? RUBY : BLUE;
		if (hy > 0.55f) *c = cmix(*c, GOLD, 0.7f);
		return GLASS;
	}
	float cw = (x1 - x0) / COLS, ch = (y1 - y0) / ROWS;
	float u = (X - x0) / cw, v = (Y - y0) / ch;
	int cu = (int)u, cv = (int)v;
	float fu = u - cu - 0.5f, fv = v - cv - 0.5f;
	if (fabsf(fu) > 0.44f || fabsf(fv) > 0.45f) return STONE;    /* mullions and transoms */
	float px = fu * cw, py = fv * ch;
	/* every panel: a canopy over a figure on a silver ground */
	if (fv > 0.28f) {
		float cy = (fv - 0.28f) / 0.17f;
		if (fabsf(fu) < 0.36f - cy * 0.22f) { *c = cmix(GOLD, WHITE, 0.35f); return GLASS; }
		*c = WHITE;
		return GLASS;
	}
	C3 robe = ((cu + cv) & 1) ? RUBY : BLUE;
	int f = figure(px, py + 0.10f, 0.62f, robe, c);
	if (f) return f;
	*c = cmix(WHITE, GOLD, 0.22f);
	if (fabsf(px) > 0.10f) {
		float s = sinf(px * 30.0f) * cosf(py * 24.0f);
		if (s > 0.6f) *c = cmix(*c, GREEN, 0.5f);
	}
	return GLASS;
}

/* ----------------------------------------------- 5: a Romanesque figure */
static int win_romanesque(float X, float Y, C3 *c)
{
	float d = win_sd(X, Y);
	float e = -d;
	if (e < 0.07f) return STONE;
	e -= 0.07f;
	/* wide border of palmettes */
	if (e < 0.16f) {
		if (e < LH || e > 0.16f - LH) return LEAD;
		float s = Y * 6.0f, fs = s - floorf(s);
		if ((fs < 0.5f ? fs : 1 - fs) / 6.0f < LH) return LEAD;
		float t = (e - 0.08f) / 0.08f;
		float pal = fabsf(fs - 0.5f) * 2;
		*c = (((int)floorf(s)) & 1) ? cmix(GREEN, WHITE, pal * 0.6f) : cmix(GOLD, RUBY, pal * 0.5f);
		if (fabsf(t) > 0.7f) *c = WHITE;
		return GLASS;
	}
	float cy = unitsY * 0.46f;
	int f = figure(X, Y - cy, 1.15f, BLUE, c);
	if (f) return f;
	/* ground: big lozenges, the way a twelfth-century glazier laid them */
	float u = (X * 1.4f + (Y - cy)) / 0.42f, v = (X * 1.4f - (Y - cy)) / 0.42f;
	float fu = u - floorf(u), fv = v - floorf(v);
	float du = fu < 0.5f ? fu : 1 - fu, dv = fv < 0.5f ? fv : 1 - fv;
	if (du < 0.05f || dv < 0.05f) return LEAD;
	int idx = ((int)floorf(u) + (int)floorf(v) * 3) % 4;
	*c = idx == 0 ? RUBY : (idx == 1 ? DBLUE : (idx == 2 ? GREEN : cmix(GOLD, WHITE, 0.3f)));
	return GLASS;
}

/* ------------------------------------------------ 6: a Renaissance oculus */
static int win_oculus(float X, float Y, C3 *c)
{
	float x = X, y = Y - CCY;
	float r = sqrtf(x * x + y * y) / RCIRC, a = atan2f(y, x);
	if (r > 0.995f) return STONE;
	if (r > 0.90f) { /* laurel ring */
		float fa = (a + PI) * (28 / (2 * PI));
		float f = fa - floorf(fa);
		if (f < 0.12f) return LEAD;
		*c = (((int)fa) & 1) ? GREEN : cmix(GREEN, GOLD, 0.45f);
		return GLASS;
	}
	if (r > 0.86f) return LEAD;
	if (r < 0.17f) {
		if (r > 0.15f) return LEAD;
		float p = powf(fabsf(cosf(4 * a)), 4);
		*c = r < 0.10f + 0.05f * p ? GOLD : cmix(GOLD, AMBER, 0.6f);
		return GLASS;
	}
	int seg = (int)floorf((a + PI) / (2 * PI / 8));
	float ac = -PI + (seg + 0.5f) * (2 * PI / 8), dl = a - ac;
	if ((PI / 8 - fabsf(dl)) * r < 0.022f) return LEAD;
	C3 ground = (seg & 1) ? DBLUE : RUBY;
	if (r < 0.62f) {
		/* a small figure in each segment, upright towards the rim */
		float fx = dl * r * 2.6f, fy = (r - 0.42f) * 2.2f;
		int f = figure(fx, -fy, 0.55f, (seg & 1) ? GOLD : WHITE, c);
		if (f) return f;
	} else {
		float band = (r - 0.62f) / 0.24f;
		if (band > 0.35f && band < 0.65f) { *c = cmix(GOLD, WHITE, 0.4f); return GLASS; }
	}
	*c = ground;
	return GLASS;
}

/* ------------------------------- 7: alabaster, a dove in a glory of rays */
static int win_alabaster(float X, float Y, C3 *c)
{
	float x = X, y = Y - CCY;
	float r = sqrtf(x * x + y * y) / RCIRC, a = atan2f(y, x);
	if (r > 0.995f) return STONE;
	float glow = 0.95f - 0.45f * r * r;
	float rays = 0.5f + 0.5f * cosf(a * 24.0f);
	C3 base = cmix(AMBER, GOLD, clampf(glow - 0.25f, 0, 1));
	base = cmul(base, 0.80f + 0.28f * glow + 0.12f * rays * (1 - r));
	/* the dove: body, head, wings, tail */
	float bx = x / RCIRC, by = y / RCIRC;
	float body = sdCirc(bx * 1.7f, by * 2.6f + 0.06f, 0.26f);
	float head = sdCirc(bx * 1.5f + 0.22f, by * 1.5f - 0.30f, 0.13f);
	float wingL = sdCirc((bx + 0.22f) * 1.5f, (by - 0.18f) * 3.0f, 0.30f);
	float wingR = sdCirc((bx - 0.26f) * 1.6f, (by - 0.16f) * 3.2f, 0.28f);
	float tail = sdBox(bx + 0.34f, by + 0.06f, 0.20f, 0.05f);
	float d = fminf(fminf(body, head), fminf(fminf(wingL, wingR), tail));
	if (d < 0) {
		float t = clampf(-d * 5.0f, 0, 1);
		*c = cmix(cmix(WHITE, base, 0.35f), WHITE, t);
		return GLASS;
	}
	*c = base;
	return GLASS;
}

/* --------------------------------------------- 8: art nouveau, flowing bands */
static int win_nouveau(float X, float Y, C3 *c)
{
	float d = win_sd(X, Y);
	if (-d < 0.06f) return STONE;
	float x = X, y = Y;
	float warp = 0.22f * sinf(y * 1.6f + 0.8f) + 0.12f * sinf(y * 3.1f);
	float cx = x - warp;
	float cyc = unitsY * 0.56f;
	float r = sqrtf(cx * cx * 1.25f + (y - cyc) * (y - cyc));
	float a = atan2f(y - cyc, cx);
	/* a halo of bands around a central figure, whiplash edges */
	float band = r * 2.1f + 0.42f * sinf(a * 3.0f + r * 2.5f) + 0.28f * sinf(y * 2.2f);
	float fb = band - floorf(band);
	if (fb < 0.085f) return LEAD;
	int bi = (int)floorf(band);
	C3 ramp[6] = {AMBER, cmix(GOLD, GREEN, 0.45f), GREEN, cmix(GREEN, BLUE, 0.55f), BLUE, DBLUE};
	int idx = bi < 0 ? 0 : (bi > 5 ? 5 : bi);
	C3 col = ramp[idx];
	float shade = 0.85f + 0.3f * fb;
	if (r < 0.45f) {
		int f = figure(cx, (y - cyc) * 0.95f, 0.52f, cmix(GOLD, AMBER, 0.35f), c);
		if (f) return f;
		col = cmix(GOLD, AMBER, 0.30f);
	}
	if (r > 0.52f && r < 0.63f) { *c = cmix(WHITE, GOLD, 0.35f); return GLASS; }
	/* flower heads scattered over the outer bands */
	if (r > 0.9f) {
		float fx = cx * 3.0f, fy = (y - cyc) * 3.0f;
		int gx = (int)floorf(fx), gy = (int)floorf(fy);
		float ox = fx - gx - 0.5f + (hf(gx, gy, 5) - 0.5f) * 0.5f;
		float oy = fy - gy - 0.5f + (hf(gx, gy, 6) - 0.5f) * 0.5f;
		float rr = sqrtf(ox * ox + oy * oy), aa = atan2f(oy, ox);
		float petal = rr - (0.10f + 0.16f * fabsf(cosf(2.5f * aa)));
		if (petal < 0 && hf(gx, gy, 7) > 0.45f) {
			if (petal > -0.03f) return LEAD;
			*c = rr < 0.06f ? GOLD : cmix(PINK, WHITE, 0.4f);
			return GLASS;
		}
	}
	*c = cmul(col, shade);
	return GLASS;
}

/* ------------------------------------- 9: a vortex, "let there be light" */
static int win_vortex(float X, float Y, C3 *c)
{
	float d = win_sd(X, Y);
	if (-d < 0.06f) return STONE;
	float cyc = unitsY * 0.62f;
	float x = X, y = Y - cyc;
	float r = sqrtf(x * x + y * y * 0.55f);
	float a = atan2f(y, x) + r * 2.4f;                     /* the spiral */
	float n = fbm(x * 2.2f + 11, y * 1.1f, 0, 3, 9);
	float band = a * (3.0f / (2 * PI)) + r * 0.8f + n * 0.5f;
	float fb = band - floorf(band);
	if (fb < 0.05f) return LEAD;
	float heat = clampf(1.25f - r * 0.85f + 0.35f * sinf(a * 1.5f), 0, 1);
	C3 cold = cmix(PURPLE, DBLUE, 0.35f + 0.4f * fb);
	C3 hot = cmix(GOLD, AMBER, fb);
	C3 col = cmix(cold, hot, smooth(0.35f, 0.85f, heat + 0.25f * fb));
	if (r < 0.34f) {
		float core = smooth(0.34f, 0.06f, r);
		col = cmix(col, cmix(GOLD, WHITE, 0.45f), core * 0.7f);
	}
	/* sparks thrown out of the middle */
	float sx = x * 5.0f, sy = y * 5.0f;
	int gx = (int)floorf(sx), gy = (int)floorf(sy);
	if (hf(gx, gy, 21) > 0.93f && r > 0.4f && r < 1.6f) {
		float ox = sx - gx - 0.5f, oy = sy - gy - 0.5f;
		if (sqrtf(ox * ox + oy * oy) < 0.22f) { *c = GOLD; return GLASS; }
	}
	*c = col;
	return GLASS;
}

/* ----------------------------------- 10: dense jewels around one figure */
static int win_jewel(float X, float Y, C3 *c)
{
	float d = win_sd(X, Y);
	if (-d < 0.055f) return STONE;
	float e = -d - 0.055f;
	if (e < 0.07f) {
		/* a border of tiny jewels */
		float s = Y * 14.0f, fs = s - floorf(s);
		float dp = sqrtf((e - 0.035f) * (e - 0.035f) + (fs - 0.5f) * (fs - 0.5f) / 196.0f);
		if (dp < 0.021f) { *c = (((int)floorf(s)) % 3 == 0) ? RUBY : ((((int)floorf(s)) % 3 == 1) ? GREEN : GOLD); return GLASS; }
		if (dp < 0.028f) return LEAD;
		*c = DBLUE;
		return GLASS;
	}
	float cyc = unitsY * 0.52f;
	int f = figure(X, (Y - cyc) * 0.85f, 1.05f, PURPLE, c);
	if (f == GLASS) {
		/* Clarke's robes are never flat: sprinkle them with jewels */
		float jx = X * 26.0f, jy = Y * 26.0f;
		int gx = (int)floorf(jx), gy = (int)floorf(jy);
		float ox = jx - gx - 0.5f, oy = jy - gy - 0.5f;
		if (sqrtf(ox * ox + oy * oy) < 0.3f) {
			float h = hf(gx, gy, 33);
			if (h > 0.72f) *c = h > 0.9f ? GOLD : (h > 0.81f ? RUBY : GREEN);
		}
		return GLASS;
	}
	if (f) return f;
	/* ground: deep blue mosaic with scattered gems and a few stars */
	float gxf = X * 15.0f, gyf = Y * 15.0f;
	int gx = (int)floorf(gxf), gy = (int)floorf(gyf);
	float ox = gxf - gx - 0.5f, oy = gyf - gy - 0.5f;
	float rr = sqrtf(ox * ox + oy * oy);
	float h = hf(gx, gy, 44);
	if (rr < 0.34f) {
		if (rr > 0.30f) return LEAD;
		*c = h > 0.93f ? GOLD : (h > 0.86f ? RUBY : (h > 0.78f ? GREEN : (h > 0.4f ? BLUE : DBLUE)));
		return GLASS;
	}
	*c = cmul(DBLUE, 0.75f + 0.25f * h);
	return GLASS;
}

/* ------------------------------------------------------------ the pass */
static int glass_class(float X, float Y, C3 *c)
{
	float dO = win_sd(X, Y);
	if (dO > 0) return OUT;
	switch (W->win) {
	case WIN_LANCETS_ROSE:
		if (dO > -0.075f) return STONE;
		return win_lancets_rose(X, Y, c);
	case WIN_TALL_LIGHTS:
		if (dO > -0.05f) return STONE;
		return win_tall_lights(X, Y, c);
	case WIN_GRISAILLE:
		if (dO > -0.05f) return STONE;
		return win_grisaille(X, Y, c);
	case WIN_PERP_GRID:
		if (dO > -0.05f) return STONE;
		return win_perp_grid(X, Y, c);
	case WIN_ROMANESQUE: return win_romanesque(X, Y, c);
	case WIN_OCULUS: return win_oculus(X, Y, c);
	case WIN_ALABASTER: return win_alabaster(X, Y, c);
	case WIN_NOUVEAU: return win_nouveau(X, Y, c);
	case WIN_VORTEX: return win_vortex(X, Y, c);
	default: return win_jewel(X, Y, c);
	}
}

/* cut the glass into pieces, tint each, add streaks, bevel and bubbles */
static int glass_finish(float X, float Y, C3 *c)
{
	if (noLead) {
		float streak = 0.93f + 0.12f * vnoise(X * 6.0f, Y * 9.0f, 0, 3);
		*c = cmul(*c, streak);
		return GLASS;
	}
	Vor v = voronoi(X, Y, cellSize);
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

void make_glass(void)
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
				float Y = unitsY * (1.0f - (j + 0.25f + 0.5f * (s >> 1)) / 512.0f);
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
	/* Exposure. A pale grisaille wall of glass and a narrow jewelled lancet
	 * differ by a factor of ten in what they let through; left alone, half the
	 * churches are a white hole and the other half a dim slit. So the glass is
	 * levelled to a mean brightness, and what it casts into the room is levelled
	 * again by the area of the opening. */
	float lum = ng ? (float)((sr + sg + sb) / (3.0 * ng)) : 0.35f;
	float kGlass = clampf(0.35f / (lum > 1e-4f ? lum : 1e-4f), 0.45f, 1.7f);
	float area = CH->winW * CH->winH;
	float kArea = clampf(powf(26.0f / area, 0.30f), 0.42f, 1.12f);
	for (int k = 0; k < 256 * 512; k++) {
		u32 p = tGlass.px[0][k];
		float r = (p & 255) * (1 / 255.0f) * kGlass;
		float g = ((p >> 8) & 255) * (1 / 255.0f) * kGlass;
		float b = ((p >> 16) & 255) * (1 / 255.0f) * kGlass;
		tGlass.px[0][k] = pack(r, g, b, (p >> 24) & 255 ? 1.0f : 0.0f);
	}
	mip_build(&tGlass, 1);
	avgGlass = ng ? c3(sr / ng, sg / ng, sb / ng) : c3(0.4f, 0.45f, 0.7f);

	/* What the room gets is levelled on its own: the mean over the whole
	 * opening, so that a window of mostly stone tracery is not asked to light
	 * a nave like a wall of clear grisaille. */
	double sl = 0;
	for (int k = 0; k < 128 * 256; k++) sl += (lightAcc[k * 3] + lightAcc[k * 3 + 1] + lightAcc[k * 3 + 2]) / 3.0;
	float meanL = (float)(sl / (128 * 256));
	float kLight = clampf(0.105f / (meanL > 1e-4f ? meanL : 1e-4f), 0.12f, 2.2f) * kArea;
	for (int k = 0; k < 128 * 256 * 3; k++) lightAcc[k] *= kLight;
#ifdef CAPTURE
	{
		char line[160];
		int n = sprintf(line, "%-24s lum %.3f kGlass %.2f meanL %.4f kArea %.2f kLight %.2f\n", CH->name, lum, kGlass, meanL, kArea, kLight);
		SceUID fd = sceIoOpen("host0:/expo.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
		if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
	}
#endif
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

C3 sample_light(float u, float v)
{
	if (u <= 0 || u >= 1 || v <= 0 || v >= 1) return c3(0, 0, 0);
	int x = (int)(u * 64), y = (int)(v * 128);
	const float *p = &shaftF[(y * 64 + x) * 3];
	return c3(p[0], p[1], p[2]);
}
