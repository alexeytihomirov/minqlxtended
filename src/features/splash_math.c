// splash_math.c - see splash_math.h. Game thread only: the pair table below is static.
//
// For a player the stock check refused, in this order:
//   hitbox  any of 15 points of the hitbox (centre, corners, face centres) seen from the explosion:
//           full damage. Stock looks at the centre's height only, so feet and head never count.
//   step    rising straight up from the explosion, at most `step` units, the player is seen above:
//           full damage. A stair or a strip on the floor does not give cover.
//   slab    the player is behind a slab: nothing (the caller's check).
//   zone    rays go out from the explosion, up to `distance` units, and from every point on them
//           (the hop) the hitbox is looked for again. The path turns at the hop by theta. The zone
//           behind the corner is alpha wide, alpha = angle * (1 - hop / distance), so it narrows
//           as the explosion moves away from the corner. Damage = damage * (1 - theta / alpha),
//           with the falloff of bend_radius in place of the rocket's own.

#include <math.h>
#include <stdlib.h>

#include "splash_math.h"

#define D1 1.0f
#define D2 0.70710678f
#define D3 0.57735027f

#define SPLASH_HOP_STEP 2.0f
// Kept off whatever stopped a ray, so a hop never sits on a surface.
#define SPLASH_RAY_BACKOFF 1.0f
// Hitbox points are kept this far inside the box: a player pressed to a wall has a face in it.
#define SPLASH_INSET 1.0f
#define SPLASH_POINTS 15
#define SPLASH_RAYS 26
#define SPLASH_MAX_HOPS 32 // distance 64 / SPLASH_HOP_STEP
// Zone candidates traced at most, best first. Past this the player stays blocked.
#define SPLASH_MAX_ZONE_TRACES 192

static const splash_vec3 splash_rays[SPLASH_RAYS] = {
    {0, 0, D1},
    {D2, 0, D2}, {-D2, 0, D2}, {0, D2, D2}, {0, -D2, D2},
    {D3, D3, D3}, {D3, -D3, D3}, {-D3, D3, D3}, {-D3, -D3, D3},
    {D1, 0, 0}, {-D1, 0, 0}, {0, D1, 0}, {0, -D1, 0},
    {D2, D2, 0}, {D2, -D2, 0}, {-D2, D2, 0}, {-D2, -D2, 0},
    {D2, 0, -D2}, {-D2, 0, -D2}, {0, D2, -D2}, {0, -D2, -D2},
    {D3, D3, -D3}, {D3, -D3, -D3}, {-D3, D3, -D3}, {-D3, -D3, -D3},
    {0, 0, -D1},
};

typedef struct {
    float m;
    float s;
    float theta;
    unsigned char ray;
    unsigned char point;
} splash_pair_t;

static splash_pair_t splash_pairs[SPLASH_RAYS * SPLASH_MAX_HOPS * SPLASH_POINTS];

static float clampf(float v, float lo, float hi) {
    if (!(v >= lo)) { // NaN lands here too
        return lo;
    }
    return v > hi ? hi : v;
}

void Splash_ClampParams(splash_params_t *p) {
    p->distance    = clampf(p->distance, 8.0f, SPLASH_MAX_HOPS * SPLASH_HOP_STEP);
    p->damage      = clampf(p->damage, 0.0f, 1.0f);
    p->knockback   = clampf(p->knockback, 0.0f, 1.0f);
    p->radius      = p->radius > 0.0f ? p->radius : 120.0f;
    p->bend_radius = clampf(p->bend_radius, 8.0f, 512.0f);
    p->angle       = clampf(p->angle, 0.0f, 90.0f);
    p->step        = clampf(p->step, 0.0f, 64.0f);
}

const char *Splash_KindName(splash_kind_t kind) {
    switch (kind) {
    case SPLASH_HITBOX: return "hitbox";
    case SPLASH_STEP: return "step";
    case SPLASH_SLAB: return "slab";
    case SPLASH_ZONE: return "zone";
    default: return "blocked";
    }
}

static void hitbox_points(const splash_vec3 mins, const splash_vec3 maxs, splash_vec3 out[SPLASH_POINTS]) {
    splash_vec3 lo, hi, c;
    int n = 0;

    for (int k = 0; k < 3; k++) {
        lo[k] = mins[k] + SPLASH_INSET;
        hi[k] = maxs[k] - SPLASH_INSET;
        if (lo[k] > hi[k]) {
            lo[k] = hi[k] = (mins[k] + maxs[k]) * 0.5f;
        }
        c[k] = (lo[k] + hi[k]) * 0.5f;
    }
    out[n][0] = c[0], out[n][1] = c[1], out[n][2] = c[2], n++;
    for (int i = 0; i < 8; i++, n++) {
        out[n][0] = (i & 1) ? hi[0] : lo[0];
        out[n][1] = (i & 2) ? hi[1] : lo[1];
        out[n][2] = (i & 4) ? hi[2] : lo[2];
    }
    for (int k = 0; k < 3; k++) {
        for (int side = 0; side < 2; side++, n++) {
            out[n][0] = c[0], out[n][1] = c[1], out[n][2] = c[2];
            out[n][k] = side ? hi[k] : lo[k];
        }
    }
}

static float distance_to_box(const splash_vec3 o, const splash_vec3 mins, const splash_vec3 maxs) {
    float sum = 0.0f;
    for (int k = 0; k < 3; k++) {
        float d = 0.0f;
        if (o[k] < mins[k]) {
            d = mins[k] - o[k];
        } else if (o[k] > maxs[k]) {
            d = o[k] - maxs[k];
        }
        sum += d * d;
    }
    return sqrtf(sum);
}

// Degrees between a unit vector and b.
static float turn(const splash_vec3 unit, const splash_vec3 b) {
    float len = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (len < 1e-4f) {
        return 0.0f;
    }
    float c = (unit[0] * b[0] + unit[1] * b[1] + unit[2] * b[2]) / len;
    return acosf(clampf(c, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
}

static void along(const splash_vec3 o, const splash_vec3 dir, float s, splash_vec3 out) {
    out[0] = o[0] + dir[0] * s;
    out[1] = o[1] + dir[1] * s;
    out[2] = o[2] + dir[2] * s;
}

static int by_m_desc(const void *a, const void *b) {
    float ma = ((const splash_pair_t *)a)->m, mb = ((const splash_pair_t *)b)->m;
    return (ma < mb) - (ma > mb);
}

splash_result_t Splash_Bend(const splash_vec3 origin, const splash_vec3 mins, const splash_vec3 maxs,
                            const splash_params_t *p, const splash_world_t *w) {
    splash_result_t r = {SPLASH_NONE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0};
    splash_vec3 pts[SPLASH_POINTS];

    hitbox_points(mins, maxs, pts);

    // Hitbox.
    for (int i = 0; i < SPLASH_POINTS; i++) {
        r.traces++;
        if (w->visible(w->ctx, origin, pts[i])) {
            r.kind = SPLASH_HITBOX, r.rate = 1.0f, r.knock = 1.0f;
            return r;
        }
    }

    // Step.
    if (p->step >= SPLASH_HOP_STEP) {
        static const splash_vec3 up = {0, 0, 1};
        splash_vec3 top;
        along(origin, up, p->step, top);
        float frac = w->reach(w->ctx, origin, top);
        r.traces++;
        float top_s = frac < 0.0f ? 0.0f : frac * p->step - SPLASH_RAY_BACKOFF;
        for (float s = SPLASH_HOP_STEP; s <= top_s; s += SPLASH_HOP_STEP) {
            splash_vec3 hop;
            along(origin, up, s, hop);
            for (int i = 0; i < SPLASH_POINTS; i++) {
                if (pts[i][2] <= hop[2]) {
                    continue;
                }
                r.traces++;
                if (w->visible(w->ctx, hop, pts[i])) {
                    r.kind = SPLASH_STEP, r.rate = 1.0f, r.knock = 1.0f, r.hop = s;
                    return r;
                }
            }
        }
    }

    if (w->behind_slab && w->behind_slab(w->ctx)) {
        r.kind = SPLASH_SLAB;
        return r;
    }

    // Zone.
    float d       = distance_to_box(origin, mins, maxs);
    float own     = 1.0f - d / p->radius;
    float bent    = 1.0f - d / p->bend_radius;
    if (own <= 0.0f || bent <= 0.0f || p->angle <= 0.0f || p->damage <= 0.0f) {
        return r;
    }
    float ratio = bent / own;

    int n = 0;
    for (int ray = 0; ray < SPLASH_RAYS; ray++) {
        splash_vec3 end;
        along(origin, splash_rays[ray], p->distance, end);
        float frac = w->reach(w->ctx, origin, end);
        r.traces++;
        if (frac < 0.0f) {
            return r; // an explosion inside a wall has no corner to turn
        }
        float last = frac * p->distance - SPLASH_RAY_BACKOFF;
        for (float s = SPLASH_HOP_STEP; s <= last; s += SPLASH_HOP_STEP) {
            float alpha = p->angle * (1.0f - s / p->distance);
            splash_vec3 hop;
            along(origin, splash_rays[ray], s, hop);
            for (int i = 0; i < SPLASH_POINTS; i++) {
                splash_vec3 out = {pts[i][0] - hop[0], pts[i][1] - hop[1], pts[i][2] - hop[2]};
                float theta = turn(splash_rays[ray], out);
                float m     = 1.0f - theta / alpha;
                if (m <= 0.0f) {
                    continue;
                }
                splash_pairs[n++] = (splash_pair_t){m, s, theta, (unsigned char)ray, (unsigned char)i};
            }
        }
    }
    qsort(splash_pairs, (size_t)n, sizeof(splash_pairs[0]), by_m_desc);

    for (int k = 0; k < n && k < SPLASH_MAX_ZONE_TRACES; k++) {
        const splash_pair_t *c = &splash_pairs[k];
        splash_vec3 hop;
        along(origin, splash_rays[c->ray], c->s, hop);
        r.traces++;
        if (w->visible(w->ctx, hop, pts[c->point])) {
            r.kind  = SPLASH_ZONE;
            r.rate  = clampf(p->damage * c->m * ratio, 0.0f, 1.0f);
            r.knock = clampf(p->knockback * c->m * ratio, 0.0f, 1.0f);
            r.hop   = c->s;
            r.theta = c->theta;
            r.alpha = p->angle * (1.0f - c->s / p->distance);
            return r;
        }
    }
    return r;
}
