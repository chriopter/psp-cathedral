/* The ten rooms. Each is a handful of numbers -- how wide the nave, how high
 * it springs, which piers carry it, which vault, which floor, which wall --
 * and the builders below turn those into textures and meshes. The churches are
 * real; the geometry is a sketch of one, cut to what a PSP draws at 60 Hz. */
#include "cathedral.h"
#include <string.h>

const Church *CH;
Mesh mWall, mReveal, mFloor, mSideL, mSideR, mPillars, mRack, mCandles;
VL __attribute__((aligned(16))) vaultCP[7 * 4];
V3 flamePos[16];
int nFlames;
Tex tWall, tFloor, tVault;

#define C(r, g, b) {r, g, b}

const Church CHURCH[NCHURCH] = {
{ /* 1 */
	"Sainte-Chapelle", "Paris, France", "upper chapel, glazed 1248",
	WIN_TALL_LIGHTS, OUTL_POINTED, 0, 4.6f, 13.5f, 3.0f,
	VAULT_STARS, PIER_SLENDER, FLOOR_MOSAIC, WALL_PAINTED,
	5.4f, 9.6f, 15.6f, 33.0f,
	C(0.40f, 0.30f, 0.34f), C(0.10f, 0.13f, 0.42f),
	{C(0.09f,0.24f,0.88f), C(0.04f,0.09f,0.52f), C(0.88f,0.07f,0.12f), C(1.00f,0.76f,0.16f),
	 C(1.00f,0.48f,0.06f), C(0.10f,0.60f,0.30f), C(0.48f,0.13f,0.64f), C(0.95f,0.93f,0.82f)},
	0.42f, 0.62f, 1 },
{ /* 2 */
	"Chartres Cathedral", "Chartres, France", "the north rose, c. 1235",
	WIN_LANCETS_ROSE, OUTL_POINTED, 0, 3.6f, 7.2f, 2.0f,
	VAULT_RIB, PIER_CLUSTER, FLOOR_LABYRINTH, WALL_ASHLAR,
	7.6f, 11.8f, 16.8f, 26.0f,
	C(0.56f, 0.51f, 0.42f), C(0.30f, 0.27f, 0.24f),
	{C(0.10f,0.27f,0.85f), C(0.05f,0.12f,0.58f), C(0.90f,0.07f,0.10f), C(1.00f,0.74f,0.14f),
	 C(1.00f,0.48f,0.06f), C(0.10f,0.66f,0.30f), C(0.52f,0.14f,0.66f), C(0.95f,0.93f,0.80f)},
	0.45f, 0.60f, 1 },
{ /* 3 */
	"York Minster", "York, England", "the Five Sisters, c. 1250",
	WIN_GRISAILLE, OUTL_POINTED, 0, 7.4f, 15.0f, 2.4f,
	VAULT_RIB, PIER_SLENDER, FLOOR_SLAB, WALL_ASHLAR,
	7.0f, 12.0f, 16.5f, 26.0f,
	C(0.60f, 0.58f, 0.52f), C(0.34f, 0.33f, 0.30f),
	{C(0.46f,0.62f,0.86f), C(0.30f,0.45f,0.70f), C(0.80f,0.16f,0.20f), C(0.92f,0.86f,0.52f),
	 C(0.86f,0.72f,0.38f), C(0.56f,0.74f,0.52f), C(0.52f,0.52f,0.72f), C(0.93f,0.96f,0.88f)},
	0.30f, 0.70f, 0 },
{ /* 4 */
	"King's College Chapel", "Cambridge, England", "the east window, c. 1515",
	WIN_PERP_GRID, OUTL_POINTED, 0.62f, 5.6f, 11.5f, 2.6f,
	VAULT_FAN, PIER_NONE, FLOOR_CHECKER, WALL_ASHLAR,
	6.0f, 9.2f, 13.6f, 30.0f,
	C(0.62f, 0.58f, 0.50f), C(0.58f, 0.55f, 0.48f),
	{C(0.12f,0.30f,0.80f), C(0.06f,0.15f,0.55f), C(0.86f,0.10f,0.14f), C(0.98f,0.80f,0.26f),
	 C(0.92f,0.60f,0.18f), C(0.16f,0.58f,0.34f), C(0.50f,0.20f,0.62f), C(0.94f,0.94f,0.86f)},
	0.36f, 0.66f, 0 },
{ /* 5 */
	"Le Mans Cathedral", "Le Mans, France", "the Ascension, c. 1120",
	WIN_ROMANESQUE, OUTL_ROUND, 0, 2.6f, 5.4f, 4.2f,
	VAULT_RIB, PIER_ROUND, FLOOR_SLAB, WALL_ASHLAR,
	5.6f, 8.2f, 11.6f, 22.0f,
	C(0.58f, 0.50f, 0.40f), C(0.32f, 0.28f, 0.23f),
	{C(0.16f,0.34f,0.82f), C(0.08f,0.18f,0.55f), C(0.86f,0.12f,0.12f), C(0.96f,0.82f,0.26f),
	 C(0.94f,0.58f,0.12f), C(0.24f,0.66f,0.32f), C(0.46f,0.20f,0.58f), C(0.94f,0.94f,0.84f)},
	0.50f, 0.55f, 1 },
{ /* 6 */
	"Santa Maria del Fiore", "Florence, Italy", "oculus after Donatello, 1434",
	WIN_OCULUS, OUTL_CIRCLE, 0, 4.4f, 4.4f, 9.2f,
	VAULT_RIB, PIER_CLUSTER, FLOOR_STAR, WALL_MARBLE,
	8.6f, 13.5f, 19.0f, 28.0f,
	C(0.66f, 0.62f, 0.56f), C(0.40f, 0.36f, 0.32f),
	{C(0.14f,0.32f,0.78f), C(0.07f,0.16f,0.50f), C(0.84f,0.12f,0.16f), C(1.00f,0.78f,0.20f),
	 C(0.94f,0.56f,0.14f), C(0.14f,0.58f,0.32f), C(0.48f,0.18f,0.58f), C(0.96f,0.94f,0.84f)},
	0.28f, 0.74f, 1 },
{ /* 7 */
	"St Peter's Basilica", "Rome, Vatican", "the Holy Spirit window, 1666",
	WIN_ALABASTER, OUTL_CIRCLE, 0, 3.0f, 3.0f, 9.0f,
	VAULT_COFFER, PIER_PILASTER, FLOOR_STAR, WALL_MARBLE,
	10.0f, 13.0f, 18.5f, 30.0f,
	C(0.68f, 0.63f, 0.54f), C(0.58f, 0.54f, 0.47f),
	{C(0.70f,0.60f,0.35f), C(0.45f,0.35f,0.18f), C(0.92f,0.55f,0.18f), C(1.00f,0.82f,0.35f),
	 C(1.00f,0.62f,0.16f), C(0.70f,0.62f,0.30f), C(0.75f,0.55f,0.30f), C(1.00f,0.96f,0.86f)},
	0.20f, 0.78f, 1 },
{ /* 8 */
	"St Vitus Cathedral", "Prague, Czechia", "art nouveau glass, 1931",
	WIN_NOUVEAU, OUTL_POINTED, 0, 3.4f, 8.2f, 2.6f,
	VAULT_NET, PIER_CLUSTER, FLOOR_CHECKER, WALL_ASHLAR,
	6.2f, 11.5f, 16.4f, 26.0f,
	C(0.52f, 0.50f, 0.46f), C(0.34f, 0.32f, 0.30f),
	{C(0.16f,0.44f,0.86f), C(0.08f,0.22f,0.60f), C(0.88f,0.22f,0.26f), C(1.00f,0.78f,0.22f),
	 C(0.98f,0.52f,0.12f), C(0.30f,0.70f,0.42f), C(0.58f,0.28f,0.70f), C(0.97f,0.95f,0.86f)},
	0.34f, 0.64f, 1 },
{ /* 9 */
	"Franciscan Church", "Krakow, Poland", "Let there be!, 1904",
	WIN_VORTEX, OUTL_POINTED, 0, 3.4f, 7.6f, 3.0f,
	VAULT_RIB, PIER_CLUSTER, FLOOR_CHECKER, WALL_PAINTED,
	5.4f, 9.8f, 14.0f, 22.0f,
	C(0.42f, 0.30f, 0.28f), C(0.26f, 0.20f, 0.24f),
	{C(0.20f,0.30f,0.86f), C(0.10f,0.12f,0.50f), C(0.92f,0.18f,0.16f), C(1.00f,0.82f,0.20f),
	 C(1.00f,0.50f,0.06f), C(0.26f,0.62f,0.36f), C(0.46f,0.16f,0.72f), C(0.98f,0.94f,0.80f)},
	0.40f, 0.58f, 1 },
{ /* 10 */
	"Honan Chapel", "Cork, Ireland", "windows by Harry Clarke, 1916",
	WIN_JEWEL, OUTL_POINTED, 0, 1.7f, 4.8f, 2.8f,
	VAULT_WOOD, PIER_NONE, FLOOR_MOSAIC, WALL_ASHLAR,
	4.3f, 5.6f, 8.4f, 22.0f,
	C(0.58f, 0.54f, 0.46f), C(0.34f, 0.22f, 0.14f),
	{C(0.10f,0.22f,0.86f), C(0.04f,0.08f,0.48f), C(0.86f,0.08f,0.22f), C(1.00f,0.76f,0.18f),
	 C(0.96f,0.44f,0.10f), C(0.10f,0.62f,0.40f), C(0.42f,0.12f,0.74f), C(0.96f,0.94f,0.86f)},
	0.44f, 0.56f, 1 },
};

/* ------------------------------------------------------------- textures */
static void make_wall_tex(void)
{
	tWall = new_tex(128, 128, 4);
	C3 base = CH->stone;
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			float n = fbm(x / 16.0f, y / 16.0f, 8, 4, 22);
			C3 c;
			switch (CH->wall) {
			case WALL_BRICK: {
				int row = y / 16, bx = (x + (row & 1) * 16) & 127;
				int lx = bx % 32, ly = y % 16;
				float edge = fminf(fminf(lx, 31 - lx), fminf(ly, 15 - ly));
				float h = hf(bx / 32, row, 21);
				c = cmul(base, (0.86f + 0.26f * h) * (0.8f + 0.4f * n));
				if (edge < 1.0f) c = cmul(c3(0.32f, 0.30f, 0.28f), 0.85f + 0.3f * n);
				break;
			}
			case WALL_MARBLE: {
				float s = sinf(2 * PI * (x / 128.0f * 2 + y / 128.0f) + n * 7.0f);
				float vein = powf(1 - fabsf(s), 10);
				c = cmul(base, 0.90f + 0.16f * n);
				c = cmix(c, cmul(base, 0.45f), vein * 0.55f);
				int panel = (x / 32 + y / 64) & 1;
				if (panel) c = cmul(c, 0.92f);
				if (x % 32 < 1 || y % 64 < 1) c = cmul(c, 0.6f);
				break;
			}
			case WALL_PAINTED: {
				int row = y / 32, bx = (x + (row & 1) * 32) & 127;
				int lx = bx % 64, ly = y % 32;
				float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 31 - ly));
				c = cmul(base, (0.9f + 0.16f * hf(bx / 64, row, 21)) * (0.78f + 0.4f * n));
				if (edge < 0.9f) c = cmul(c, 0.55f);
				/* a painted band of quatrefoils, as the Sainte-Chapelle dado */
				float qx = (x % 32) - 15.5f, qy = (y % 32) - 15.5f;
				float q = fminf(fminf(sdCirc(qx - 6, qy, 7), sdCirc(qx + 6, qy, 7)),
				                fminf(sdCirc(qx, qy - 6, 7), sdCirc(qx, qy + 6, 7)));
				if (q < 0 && ((y / 32) & 1)) {
					C3 paint = ((x / 32 + y / 32) & 1) ? c3(0.42f, 0.12f, 0.14f) : c3(0.12f, 0.16f, 0.45f);
					c = cmix(c, cmul(paint, 0.8f + 0.4f * n), q < -1.5f ? 0.85f : 0.4f);
				}
				break;
			}
			default: {
				int row = y / 32, bx = (x + (row & 1) * 32) & 127, col = bx / 64;
				int lx = bx % 64, ly = y % 32;
				float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 31 - ly));
				float h = hf(col, row, 21);
				c = cmul(base, (0.90f + 0.14f * h) * (0.78f + 0.40f * n));
				if (edge < 0.8f) c = cmul(cmul(base, 0.45f), 0.85f + 0.3f * n);
				else if (edge < 3.5f) c = cmul(c, 0.90f + 0.10f * (edge - 0.8f) / 2.7f);
			}
			}
			tWall.px[0][y * 128 + x] = packc(c, 1);
		}
	mip_build(&tWall, 0);
}

static void make_floor_tex(void)
{
	tFloor = new_tex(128, 128, 4);
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			float n = fbm(x / 32.0f, y / 32.0f, 4, 5, 31);
			C3 c; float a = 0.80f;
			float fx = x - 63.5f, fy = y - 63.5f;
			switch (CH->floor) {
			case FLOOR_LABYRINTH: {
				/* concentric paths, the way Chartres has them underfoot */
				float r = sqrtf(fx * fx + fy * fy) / 8.0f;
				float band = r - floorf(r);
				C3 pale = cmul(c3(0.74f, 0.70f, 0.62f), 0.9f + 0.18f * n);
				C3 dark = cmul(c3(0.22f, 0.21f, 0.20f), 0.9f + 0.2f * n);
				c = band < 0.42f ? pale : dark;
				float ang = atan2f(fy, fx);
				if (fabsf(ang) < 0.06f && r > 1) c = pale;   /* the turns */
				if (band > 0.40f && band < 0.46f) c = cmul(dark, 1.4f);
				a = 0.84f;
				break;
			}
			case FLOOR_STAR: {
				/* marble inlay: an eight-pointed star in every tile */
				float r = sqrtf(fx * fx + fy * fy) / 52.0f, ang = atan2f(fy, fx);
				float star = r - (0.42f + 0.44f * powf(fabsf(cosf(4 * ang)), 4));
				C3 cream = cmul(c3(0.78f, 0.74f, 0.64f), 0.92f + 0.14f * n);
				C3 green = cmul(c3(0.16f, 0.26f, 0.22f), 0.9f + 0.2f * n);
				C3 red = cmul(c3(0.42f, 0.16f, 0.16f), 0.9f + 0.2f * n);
				c = star < 0 ? (r < 0.30f ? red : green) : cream;
				if (fabsf(star) < 0.03f) c = cmul(cream, 1.1f);
				a = 0.88f;
				break;
			}
			case FLOOR_MOSAIC: {
				/* small tesserae with a running border */
				int gx = x / 6, gy = y / 6;
				float h = hf(gx, gy, 61);
				int border = (x < 8 || x > 119 || y < 8 || y > 119);
				C3 t = border ? cmix(c3(0.55f, 0.42f, 0.22f), c3(0.75f, 0.64f, 0.4f), h)
				              : cmix(c3(0.28f, 0.30f, 0.34f), c3(0.62f, 0.60f, 0.55f), h * h);
				if (h > 0.93f) t = c3(0.5f, 0.18f, 0.16f);
				if (x % 6 == 0 || y % 6 == 0) t = cmul(t, 0.6f);
				c = cmul(t, 0.92f + 0.16f * n);
				a = 0.78f;
				break;
			}
			case FLOOR_SLAB: {
				int gx = x / 64, gy = y / 43;
				float h = hf(gx, gy, 71);
				int lx = x % 64, ly = y % 43;
				float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 42 - ly));
				c = cmul(c3(0.52f, 0.50f, 0.46f), (0.86f + 0.24f * h) * (0.85f + 0.3f * n));
				if (edge < 1.0f) c = cmul(c, 0.5f);
				a = 0.70f;
				break;
			}
			default: {
				int tile = ((x / 64) + (y / 64)) & 1;
				int lx = x % 64, ly = y % 64;
				float edge = fminf(fminf(lx, 63 - lx), fminf(ly, 63 - ly));
				float s = sinf(2 * PI * (x / 128.0f + y / 128.0f) + n * 9.0f);
				float vein = powf(1 - fabsf(s), 14);
				if (!tile) { c = cmul(c3(0.80f, 0.76f, 0.68f), 0.88f + 0.18f * n); c = cmix(c, c3(0.45f, 0.42f, 0.40f), vein * 0.5f); a = 0.86f; }
				else { c = cmul(c3(0.16f, 0.17f, 0.19f), 0.85f + 0.3f * n); c = cmix(c, c3(0.55f, 0.55f, 0.52f), vein * 0.4f); a = 0.74f; }
				if (edge < 1.0f) c = c3(0.07f, 0.07f, 0.07f);
			}
			}
			tFloor.px[0][y * 128 + x] = packc(c, a);
		}
	mip_build(&tFloor, 0);
}

static void make_vault_tex(void)
{
	tVault = new_tex(128, 128, 4);
	C3 base = CH->vaultCol;
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++) {
			float n = fbm(x / 16.0f, y / 16.0f, 8, 4, 41);
			C3 c = cmul(base, 0.78f + 0.42f * n);
			float fx = x - 63.5f, fy = y - 63.5f;
			switch (CH->vault) {
			case VAULT_STARS: {
				int cx = x / 32, cy = y / 32;
				float sx = cx * 32 + 9 + 14 * hf(cx, cy, 42), sy = cy * 32 + 9 + 14 * hf(cx, cy, 43);
				float dx = x - sx, dy = y - sy, r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
				float rs = 4.2f * (0.42f + 0.58f * powf(fabsf(cosf(2.5f * a)), 3));
				if (r < rs) c = cmix(c3(0.95f, 0.74f, 0.30f), c, smooth(rs - 1.0f, rs, r));
				break;
			}
			case VAULT_FAN: {
				/* ribs fanning out of the springer in the corner */
				float r = sqrtf((float)(x * x + y * y));
				float a = atan2f((float)y, (float)x);
				float spokes = fabsf(sinf(a * 7.0f)) * r;
				float rings = fabsf(sinf(r * 0.14f)) * 30.0f;
				float rib = fminf(spokes, rings);
				c = cmul(base, 0.9f + 0.2f * n);
				if (rib < 2.2f) c = cmul(c, 0.55f);
				else if (rib < 4.0f) c = cmul(c, 0.8f);
				break;
			}
			case VAULT_NET: {
				float u = (fx + fy) / 26.0f, v = (fx - fy) / 26.0f;
				float du = fabsf(u - floorf(u) - 0.5f), dv = fabsf(v - floorf(v) - 0.5f);
				float rib = fminf(du, dv);
				if (rib > 0.42f) c = cmul(c, 0.5f);
				else if (rib > 0.36f) c = cmul(c, 0.75f);
				break;
			}
			case VAULT_WOOD: {
				int plank = y / 11;
				float h = hf(plank, 0, 51);
				c = cmul(base, (0.8f + 0.35f * h) * (0.82f + 0.32f * fbm(x / 4.0f, y / 30.0f, 32, 3, 52)));
				if (y % 11 < 1) c = cmul(c, 0.45f);
				if ((x % 64) < 2) c = cmul(c, 0.6f);
				break;
			}
			case VAULT_COFFER: {
				int gx = x / 32, gy = y / 32;
				float lx = (x % 32) - 15.5f, ly = (y % 32) - 15.5f;
				float d = sdBox(lx, ly, 11.5f, 11.5f);
				c = cmul(base, 0.92f + 0.16f * n);
				if (d < 0) {
					c = cmul(c, 0.55f - 0.25f * smooth(-11.0f, 0.0f, d));
					float r = sqrtf(lx * lx + ly * ly);
					if (r < 4.5f) c = cmix(c, c3(0.85f, 0.7f, 0.3f), 0.7f);   /* gilt rosette */
				} else if (d < 2.5f) {
					c = cmul(c, 1.15f);
				}
				(void)gx; (void)gy;
				break;
			}
			default: { /* ribs */
				float u = fx / 64.0f, v = fy / 64.0f;
				float rib = fminf(fabsf(fabsf(u) - fabsf(v)), fminf(fabsf(u), fabsf(v)) * 2.2f);
				if (rib < 0.035f) c = cmul(c, 0.5f);
				else if (rib < 0.07f) c = cmul(c, 0.78f);
			}
			}
			tVault.px[0][y * 128 + x] = packc(c, 1);
		}
	mip_build(&tVault, 0);
}

/* ------------------------------------------------------------- geometry */
#define NA 96
static inline V3 win2world(float X, float Y, float z)
{
	float s = CH->winW * 0.5f;
	return v3(X * s, CH->winY + Y * s, z);
}

static void build_wall_and_reveal(void)
{
	float s = CH->winW * 0.5f;
	float xmin = -CH->navHalf / s, xmax = CH->navHalf / s;
	float ymin = -CH->winY / s, ymax = (CH->apexH + 0.4f - CH->winY) / s;
	float ccy = unitsY * (CH->outline == OUTL_CIRCLE ? 0.5f : 0.45f);
	float th[NA + 5]; int n = 0;
	for (int i = 0; i < NA; i++) th[n++] = -PI * 0.5f + i * 2 * PI / NA;
	float cx[4] = {xmax, xmin, xmin, xmax}, cy[4] = {ymax, ymax, ymin, ymin};
	for (int k = 0; k < 4; k++) {
		float a = atan2f(cy[k] - ccy, cx[k]);
		if (a < -PI * 0.5f) a += 2 * PI;
		th[n++] = a;
	}
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && th[j] < th[j - 1]; j--) { float t = th[j]; th[j] = th[j - 1]; th[j - 1] = t; }
	th[n] = th[0] + 2 * PI;

	static const float RS[5] = {0, 0.1f, 0.25f, 0.5f, 1.0f};
	float gz = -0.55f;
	mWall = mesh_new((n + 1) * 5, n * 4 * 6);
	mReveal = mesh_new((n + 1) * 3, n * 2 * 6);
	for (int i = 0; i <= n; i++) {
		float dx = cosf(th[i]), dy = sinf(th[i]);
		float tq = fminf(dx > 0 ? xmax / dx : (dx < 0 ? xmin / dx : 1e9f),
		                 dy > 0 ? (ymax - ccy) / dy : (dy < 0 ? (ymin - ccy) / dy : 1e9f));
		float to = win_outline(th[i], 0.22f), ti = win_outline(th[i], -0.045f);
		for (int k = 0; k < 5; k++) {
			float t = mixf(to, tq, RS[k]);
			V3 p = win2world(dx * t, ccy + dy * t, 0);
			vput(&mWall, p.x * 0.5f, p.y * 0.5f, v3(0, 0, 1), p);
		}
		for (int k = 0; k < 3; k++) {
			float f = k * 0.5f;
			float t = mixf(ti, to, f);
			V3 p = win2world(dx * t, ccy + dy * t, mixf(gz + 0.02f, 0, f));
			vput(&mReveal, 0, f * 0.35f, v3(0, 0, 1), p);
		}
	}
	float arc = 0;
	for (int i = 0; i <= n; i++) {
		if (i) {
			float ex = mReveal.v[i * 3 + 2].x - mReveal.v[(i - 1) * 3 + 2].x;
			float ey = mReveal.v[i * 3 + 2].y - mReveal.v[(i - 1) * 3 + 2].y;
			arc += sqrtf(ex * ex + ey * ey);
		}
		for (int k = 0; k < 3; k++) {
			VL *q = &mReveal.v[i * 3 + k];
			int ip = i > 0 ? i - 1 : n - 1, in = i < n ? i + 1 : 1;
			VL *a = &mReveal.v[ip * 3 + k], *b = &mReveal.v[in * 3 + k];
			VL *c0 = &mReveal.v[i * 3 + 0], *c2 = &mReveal.v[i * 3 + 2];
			V3 tt = v3(b->x - a->x, b->y - a->y, b->z - a->z);
			V3 td = v3(c2->x - c0->x, c2->y - c0->y, c2->z - c0->z);
			V3 nn = vnorm(vcross(tt, td));
			V3 toAxis = v3(-q->x, (CH->winY + ccy * s) - q->y, 0);
			if (vdot(nn, toAxis) < 0) nn = vmul(nn, -1);
			q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			q->u = arc * 0.5f;
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
	int nx = (int)(CH->navHalf * 2 / 0.5f + 0.5f), nz = (int)(CH->navLen / 0.5f + 0.5f);
	if (nx > 48) nx = 48;
	if (nz > 64) nz = 64;
	float dx = CH->navHalf * 2 / nx, dz = CH->navLen / nz;
	mFloor = mesh_new((nx + 1) * (nz + 1), nx * nz * 6);
	for (int iz = 0; iz <= nz; iz++)
		for (int ix = 0; ix <= nx; ix++) {
			float x = -CH->navHalf + ix * dx, z = iz * dz;
			vput(&mFloor, x * 0.5f, z * 0.5f, v3(0, 1, 0), v3(x, 0, z));
		}
	grid_idx(&mFloor, 0, nx, nz);

	int wy = (int)(CH->springH + 0.5f), wz = (int)(CH->navLen + 0.5f);
	if (wz > 40) wz = 40;
	float sz = CH->navLen / wz;
	for (int s = 0; s < 2; s++) {
		Mesh *m = s ? &mSideR : &mSideL;
		float x = s ? CH->navHalf : -CH->navHalf;
		*m = mesh_new((wz + 1) * (wy + 1), wz * wy * 6);
		for (int iy = 0; iy <= wy; iy++)
			for (int iz = 0; iz <= wz; iz++)
				vput(m, iz * sz * 0.5f, iy * 0.5f, v3(s ? -1 : 1, 0, 0), v3(x, (float)iy * CH->springH / wy, iz * sz));
		grid_idx(m, 0, wz, wy);
	}
}

#define PSEG 28
static void build_piers(void)
{
	if (CH->pier == PIER_NONE) {
		mPillars = mesh_new(4, 6);
		return;
	}
	float prof[10][2];
	int NR = 10;
	float r0, flute = 0.07f;
	switch (CH->pier) {
	case PIER_ROUND: r0 = 0.95f; flute = 0.0f; break;
	case PIER_SLENDER: r0 = 0.34f; flute = 0.14f; break;
	case PIER_PILASTER: r0 = 0.75f; flute = 0.0f; break;
	default: r0 = 0.55f; flute = 0.09f;
	}
	float h = CH->springH;
	float base[10][2] = {{0, 1.32f}, {0.30f, 1.32f}, {0.38f, 1.14f}, {0.55f, 1.02f}, {0.62f, 1.0f},
	                     {0.93f, 1.0f}, {0.945f, 1.10f}, {0.965f, 1.26f}, {0.985f, 1.40f}, {1.0f, 1.40f}};
	for (int i = 0; i < NR; i++) {
		prof[i][0] = base[i][0] * h;
		prof[i][1] = base[i][1] * r0;
	}
	int cols = CH->pier == PIER_PILASTER ? 5 : 3;
	float inset = CH->pier == PIER_PILASTER ? 0.2f : 0.95f;
	int np = cols * 2;
	mPillars = mesh_new(np * (PSEG + 1) * NR, np * PSEG * (NR - 1) * 6);
	for (int p = 0; p < np; p++) {
		float px = (p < cols ? -1 : 1) * (CH->navHalf - inset);
		float pz = CH->navLen * (0.16f + 0.68f * ((p % cols) / (float)(cols - 1)));
		int b = mPillars.nv;
		for (int r = 0; r < NR; r++)
			for (int i = 0; i <= PSEG; i++) {
				float th = i * 2 * PI / PSEG;
				float rr = prof[r][1] * (1 - flute * 0.5f + flute * 0.5f * cosf(8 * th));
				if (CH->pier == PIER_PILASTER) rr = prof[r][1] * (0.45f + 0.55f * fabsf(cosf(th)));
				V3 pos = v3(px + rr * cosf(th), prof[r][0], pz + rr * sinf(th));
				vput(&mPillars, th / (2 * PI) * 3, prof[r][0] * 0.5f, v3(0, 0, 0), pos);
			}
		for (int r = 0; r < NR; r++)
			for (int i = 0; i <= PSEG; i++) {
				VL *q = &mPillars.v[b + r * (PSEG + 1) + i];
				int il = i > 0 ? i - 1 : PSEG - 1, ir = i < PSEG ? i + 1 : 1;
				int rd = r > 0 ? r - 1 : 0, ru = r < NR - 1 ? r + 1 : NR - 1;
				VL *a = &mPillars.v[b + r * (PSEG + 1) + il], *bb = &mPillars.v[b + r * (PSEG + 1) + ir];
				VL *c = &mPillars.v[b + rd * (PSEG + 1) + i], *d = &mPillars.v[b + ru * (PSEG + 1) + i];
				V3 tth = v3(bb->x - a->x, bb->y - a->y, bb->z - a->z);
				V3 ty = v3(d->x - c->x, d->y - c->y, d->z - c->z);
				V3 nn = vnorm(vcross(ty, tth));
				V3 rad = v3(q->x - px, 0, q->z - pz);
				if (vdot(nn, rad) < 0) nn = vmul(nn, -1);
				q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			}
		grid_idx(&mPillars, b, PSEG, NR - 1);
	}
}

static void build_candles(void)
{
	nFlames = 0;
	mRack = mesh_new(20, 30);
	mCandles = mesh_new(12 * 30, 12 * 48);
	if (!CH->candles) return;
	float x0 = CH->navHalf * 0.52f, x1 = x0 + 1.2f, y1 = 0.85f;
	float z0 = CH->navLen * 0.26f, z1 = z0 + 0.6f;
	int b;
	b = mRack.nv;
	vput(&mRack, x0, z0, v3(0, 1, 0), v3(x0, y1, z0)); vput(&mRack, x1, z0, v3(0, 1, 0), v3(x1, y1, z0));
	vput(&mRack, x1, z1, v3(0, 1, 0), v3(x1, y1, z1)); vput(&mRack, x0, z1, v3(0, 1, 0), v3(x0, y1, z1)); quad_idx(&mRack, b);
	b = mRack.nv;
	vput(&mRack, x0, 0, v3(0, 0, 1), v3(x0, 0, z1)); vput(&mRack, x1, 0, v3(0, 0, 1), v3(x1, 0, z1));
	vput(&mRack, x1, y1, v3(0, 0, 1), v3(x1, y1, z1)); vput(&mRack, x0, y1, v3(0, 0, 1), v3(x0, y1, z1)); quad_idx(&mRack, b);
	b = mRack.nv;
	vput(&mRack, x0, 0, v3(0, 0, -1), v3(x0, 0, z0)); vput(&mRack, x1, 0, v3(0, 0, -1), v3(x1, 0, z0));
	vput(&mRack, x1, y1, v3(0, 0, -1), v3(x1, y1, z0)); vput(&mRack, x0, y1, v3(0, 0, -1), v3(x0, y1, z0)); quad_idx(&mRack, b);
	b = mRack.nv;
	vput(&mRack, z0, 0, v3(-1, 0, 0), v3(x0, 0, z0)); vput(&mRack, z1, 0, v3(-1, 0, 0), v3(x0, 0, z1));
	vput(&mRack, z1, y1, v3(-1, 0, 0), v3(x0, y1, z1)); vput(&mRack, z0, y1, v3(-1, 0, 0), v3(x0, y1, z0)); quad_idx(&mRack, b);
	b = mRack.nv;
	vput(&mRack, z0, 0, v3(1, 0, 0), v3(x1, 0, z0)); vput(&mRack, z1, 0, v3(1, 0, 0), v3(x1, 0, z1));
	vput(&mRack, z1, y1, v3(1, 0, 0), v3(x1, y1, z1)); vput(&mRack, z0, y1, v3(1, 0, 0), v3(x1, y1, z0)); quad_idx(&mRack, b);

	for (int c = 0; c < 12; c++) {
		float cx = x0 + 0.12f + (c % 6) * 0.19f + (hf(c, 1, 51) - 0.5f) * 0.05f;
		float cz = (c < 6 ? z0 + 0.18f : z0 + 0.44f) + (hf(c, 2, 51) - 0.5f) * 0.04f;
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
		flamePos[nFlames++] = v3(cx, y1 + h + 0.012f, cz);
	}
}

static void build_vault(void)
{
	float h = CH->navHalf, s = CH->springH, a = CH->apexH;
	int round = (CH->vault == VAULT_WOOD || CH->vault == VAULT_COFFER);
	float PXY[7][2];
	float rise = a - s;
	PXY[0][0] = -h; PXY[0][1] = s;
	PXY[1][0] = -h; PXY[1][1] = s + rise * 0.55f;
	PXY[2][0] = round ? -h * 0.55f : -h * 0.54f; PXY[2][1] = round ? a : a - rise * 0.12f;
	PXY[3][0] = 0; PXY[3][1] = a;
	for (int i = 0; i < 3; i++) { PXY[6 - i][0] = -PXY[i][0]; PXY[6 - i][1] = PXY[i][1]; }
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 7; i++) {
			VL *q = &vaultCP[j * 7 + i];
			int ip = i > 0 ? i - 1 : 0, in = i < 6 ? i + 1 : 6;
			float tx = PXY[in][0] - PXY[ip][0], ty = PXY[in][1] - PXY[ip][1];
			V3 nn = vnorm(v3(ty, -tx, 0));
			if (vdot(nn, v3(-PXY[i][0], s - PXY[i][1], 0)) < 0) nn = vmul(nn, -1);
			q->u = i * 0.8f; q->v = j * CH->navLen / 3.0f * 0.25f;
			q->nx = nn.x; q->ny = nn.y; q->nz = nn.z;
			q->x = PXY[i][0]; q->y = PXY[i][1]; q->z = j * CH->navLen / 3.0f;
		}
}

void build_church(void)
{
	make_wall_tex();
	make_floor_tex();
	make_vault_tex();
	build_wall_and_reveal();
	build_floor_and_sides();
	build_piers();
	build_candles();
	build_vault();
}
