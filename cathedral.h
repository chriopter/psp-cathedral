/* Lux Aeterna -- shared types and the church table. */
#ifndef CATHEDRAL_H
#define CATHEDRAL_H

#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <math.h>

#define PI 3.14159265f

/* ------------------------------------------------------------------ math */
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
u32 pack(float r, float g, float b, float a);
static inline u32 packc(C3 c, float a) { return pack(c.r, c.g, c.b, a); }

float frand(void);
u32 ihash(int x, int y, int s);
float hf(int x, int y, int s);
float vnoise(float x, float y, int per, int seed);
float fbm(float x, float y, int per, int oct, int seed);
typedef struct { float edge; u32 id; } Vor;
Vor voronoi(float x, float y, float cell);

/* signed distances, in window units */
float sdArch(float x, float y, float a, float y0, float ys);   /* pointed */
float sdRound(float x, float y, float a, float y0, float ys);  /* semicircular */
float sdBox(float x, float y, float bx, float by);
static inline float sdCirc(float x, float y, float r) { return sqrtf(x * x + y * y) - r; }

/* ----------------------------------------------------------------- pool */
void *palloc(u32 n);
u32 pool_mark(void);
void pool_release(u32 mark);

/* ------------------------------------------------------------- textures */
typedef struct { int w, h, lv; u32 *px[5]; } Tex;
Tex new_tex(int w, int h, int lv);
void mip_build(Tex *t, int alphaCut);
void bind(const Tex *t);

/* ------------------------------------------------------------- geometry */
typedef struct { float u, v; float nx, ny, nz; float x, y, z; } VL;
typedef struct { float u, v; u32 c; float x, y, z; } VT;
#define VL_FMT (GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define VT_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define V2_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

typedef struct { VL *v; unsigned short *ix; int nv, ni; } Mesh;
Mesh mesh_new(int nv, int ni);
void vput(Mesh *m, float u, float v, V3 n, V3 p);
void grid_idx(Mesh *m, int base, int cols, int rows);
void quad_idx(Mesh *m, int base);

/* --------------------------------------------------------- the churches */
enum { WIN_LANCETS_ROSE, WIN_TALL_LIGHTS, WIN_GRISAILLE, WIN_PERP_GRID,
       WIN_ROMANESQUE, WIN_OCULUS, WIN_ALABASTER, WIN_NOUVEAU, WIN_VORTEX, WIN_JEWEL };
enum { OUTL_POINTED, OUTL_ROUND, OUTL_CIRCLE };
enum { VAULT_RIB, VAULT_FAN, VAULT_NET, VAULT_WOOD, VAULT_COFFER, VAULT_STARS };
enum { PIER_CLUSTER, PIER_ROUND, PIER_SLENDER, PIER_PILASTER, PIER_NONE };
enum { FLOOR_CHECKER, FLOOR_LABYRINTH, FLOOR_STAR, FLOOR_MOSAIC, FLOOR_SLAB };
enum { WALL_ASHLAR, WALL_PAINTED, WALL_BRICK, WALL_MARBLE };

typedef struct {
	const char *name;    /* church */
	const char *place;   /* town, country */
	const char *work;    /* the window, its date */
	int win, outline;
	float rise;               /* arch rise as a fraction of half-width, 0 = equilateral */
	float winW, winH, winY;   /* the glass, in metres */
	int vault, pier, floor, wall;
	float navHalf;            /* side walls at +-navHalf */
	float springH, apexH;     /* vault springs and peaks */
	float navLen;
	C3 stone, vaultCol;
	C3 pal[8];                /* blue dblue ruby gold amber green purple white */
	float sunAz, sunEl;       /* the sun this room is at its best in */
	int candles;
} Church;

#define NCHURCH 10
extern const Church CHURCH[NCHURCH];
extern const Church *CH;

/* glass palette for the church being drawn, set by glass_select() */
extern C3 BLUE, DBLUE, RUBY, GOLD, AMBER, GREEN, PURPLE, WHITE, PINK, SKY;
enum { OUT = 0, STONE, LEAD, GLASS };

/* window units: x in [-1,1], y in [0,unitsY], isotropic with the world */
extern float unitsY;
float win_outline(float th, float off);          /* ray from the window centre */
float win_sd(float X, float Y);                  /* the opening, negative inside */
void glass_select(int idx);                      /* palette + shape for a church */
void make_glass(void);                           /* renders the three textures */
C3 sample_light(float u, float v);
extern Tex tGlass, tLight, tShaft;
extern C3 avgGlass;

/* ---------------------------------------------------------- the interior */
extern Mesh mWall, mReveal, mFloor, mSideL, mSideR, mPillars, mRack, mCandles;
extern VL vaultCP[7 * 4];
extern V3 flamePos[16];
extern int nFlames;
extern Tex tWall, tFloor, tVault;
void build_church(void);   /* textures and meshes for CH */

#endif
