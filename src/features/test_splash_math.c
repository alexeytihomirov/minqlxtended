// Standalone harness for Splash_Bend: a world of axis-aligned boxes and the scenes the rules were
// tuned on (step, strip on the floor, wall corner, ledge from above, platform from below).
//
// Build (from the repository root):
//     gcc -std=gnu11 -O2 -Wall -Wextra -Isrc -o test_splash_math src/features/test_splash_math.c src/features/splash_math.c -lm
//
// Usage:
//     ./test_splash_math        runs every case, prints the damage tables, exits non-zero on a failure
//
// Not part of the Makefile on purpose, like test_demo_name.c: it has its own main().

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "features/splash_math.h"

#define BIG 4096.0f
#define SPLASH 84.0f

typedef struct {
    float lo[3], hi[3];
} box_t;

typedef struct {
    box_t solids[4];
    int count;
} world_t;

static int failures;

static int inside(const box_t *b, const splash_vec3 p) {
    const float e = 1e-3f;
    for (int k = 0; k < 3; k++) {
        if (!(p[k] > b->lo[k] + e && p[k] < b->hi[k] - e)) {
            return 0;
        }
    }
    return 1;
}

// Fraction of a->b before the first solid; -1 when a is inside one.
static float cast(const world_t *w, const splash_vec3 a, const splash_vec3 b) {
    float best = 1.0f;
    for (int i = 0; i < w->count; i++) {
        const box_t *s = &w->solids[i];
        if (inside(s, a)) {
            return -1.0f;
        }
        float t_in = -INFINITY, t_out = INFINITY;
        int miss = 0;
        for (int k = 0; k < 3 && !miss; k++) {
            float d = b[k] - a[k];
            if (fabsf(d) < 1e-9f) {
                if (a[k] <= s->lo[k] || a[k] >= s->hi[k]) {
                    miss = 1;
                }
                continue;
            }
            float t1 = (s->lo[k] - a[k]) / d, t2 = (s->hi[k] - a[k]) / d;
            if (t1 > t2) {
                float t = t1;
                t1 = t2, t2 = t;
            }
            if (t1 > t_in) t_in = t1;
            if (t2 < t_out) t_out = t2;
        }
        if (!miss && t_in < t_out - 1e-6f && t_in >= 0.0f && t_in < best) {
            best = t_in;
        }
    }
    return best;
}

static int w_visible(void *ctx, const splash_vec3 a, const splash_vec3 b) {
    return cast(ctx, a, b) >= 1.0f;
}

static float w_reach(void *ctx, const splash_vec3 a, const splash_vec3 b) {
    return cast(ctx, a, b);
}

typedef struct {
    world_t world;
    splash_vec3 origin, mins, maxs;
} scene_t;

static void add(world_t *w, float x0, float y0, float z0, float x1, float y1, float z1) {
    box_t *b = &w->solids[w->count++];
    b->lo[0] = x0, b->lo[1] = y0, b->lo[2] = z0;
    b->hi[0] = x1, b->hi[1] = y1, b->hi[2] = z1;
}

static void player(scene_t *s, float x0, float y0, float z0, float x1, float y1, float z1) {
    s->mins[0] = x0, s->mins[1] = y0, s->mins[2] = z0;
    s->maxs[0] = x1, s->maxs[1] = y1, s->maxs[2] = z1;
}

static void at(scene_t *s, float x, float y, float z) {
    s->origin[0] = x, s->origin[1] = y, s->origin[2] = z;
}

// e: explosion to the corner, p: player to the corner, as on the comparison page.
static scene_t scene(const char *name, float e, float p) {
    scene_t s;
    memset(&s, 0, sizeof(s));
    if (!strcmp(name, "step")) { // 16 high, player on it, rocket into the riser or the floor before it
        add(&s.world, -BIG, -BIG, -64, BIG, BIG, 0);
        add(&s.world, 0, -BIG, 0, BIG, BIG, 16);
        player(&s, p, -15, 16, p + 30, 15, 72);
        if (e < 16) at(&s, -1, 0, 16 - e);
        else at(&s, -1 - (e - 16), 0, 1);
    } else if (!strcmp(name, "strip")) { // 12 high, 8 wide, player on the floor behind it
        add(&s.world, -BIG, -BIG, -64, BIG, BIG, 0);
        add(&s.world, 0, -BIG, 0, 8, BIG, 12);
        player(&s, 8 + p, -15, 0, 38 + p, 15, 56);
        if (e < 12) at(&s, -1, 0, 12 - e);
        else at(&s, -1 - (e - 12), 0, 1);
    } else if (!strcmp(name, "wall")) { // a corner seen from above, player pressed to the far face
        add(&s.world, 0, -BIG, -BIG, BIG, 0, BIG);
        player(&s, p, 1, -24, p + 30, 31, 32);
        at(&s, -1, -e, 4);
    } else if (!strcmp(name, "ledge")) { // 64 high, rocket on top near the edge, player below it
        add(&s.world, 0, -BIG, -64, BIG, BIG, 0);
        add(&s.world, -BIG, -BIG, -128, BIG, BIG, -64);
        player(&s, -p - 30, -15, -64, -p, 15, -8);
        at(&s, e, 0, 1);
    } else { // platform: 64 high, player on top, rocket into its face below the edge
        add(&s.world, 0, -BIG, -64, BIG, BIG, 0);
        add(&s.world, -BIG, -BIG, -128, BIG, BIG, -64);
        player(&s, p, -15, 0, p + 30, 15, 56);
        at(&s, -1, 0, -(e < 63 ? e : 63));
    }
    return s;
}

static splash_params_t defaults(void) {
    splash_params_t p = {32, 0.6f, 0.6f, 120, 96, 60, 20};
    return p;
}

static float own_falloff(const scene_t *s, float radius) {
    float sum = 0;
    for (int k = 0; k < 3; k++) {
        float d = 0;
        if (s->origin[k] < s->mins[k]) d = s->mins[k] - s->origin[k];
        else if (s->origin[k] > s->maxs[k]) d = s->origin[k] - s->maxs[k];
        sum += d * d;
    }
    float f = 1.0f - sqrtf(sum) / radius;
    return f > 0 ? f : 0;
}

static splash_result_t run(const char *name, float e, float p, const splash_params_t *params) {
    scene_t s = scene(name, e, p);
    splash_world_t w = {w_visible, w_reach, NULL, &s.world};
    return Splash_Bend(s.origin, s.mins, s.maxs, params, &w);
}

static int damage(const char *name, float e, float p, const splash_params_t *params) {
    scene_t s = scene(name, e, p);
    splash_world_t w = {w_visible, w_reach, NULL, &s.world};
    splash_result_t r = Splash_Bend(s.origin, s.mins, s.maxs, params, &w);
    return (int)(SPLASH * own_falloff(&s, params->radius) * r.rate + 0.5f);
}

static void expect_kind(const char *name, float e, float p, const splash_params_t *params, splash_kind_t want) {
    splash_result_t r = run(name, e, p, params);
    if (r.kind != want) {
        printf("FAIL %s e=%g p=%g: %s, want %s\n", name, e, p, Splash_KindName(r.kind), Splash_KindName(want));
        failures++;
    }
}

static void expect(int ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void table(const char *name, const splash_params_t *params) {
    static const float es[] = {2, 4, 8, 16, 24, 32};
    static const float ps[] = {0, 8, 16, 24, 32, 48, 64, 96};
    printf("\n%s (damage of 84; z = zone, s = step, h = hitbox)\n  e\\p", name);
    for (size_t j = 0; j < sizeof(ps) / sizeof(ps[0]); j++) printf("%5g", ps[j]);
    printf("\n");
    for (size_t i = 0; i < sizeof(es) / sizeof(es[0]); i++) {
        printf("  %3g", es[i]);
        for (size_t j = 0; j < sizeof(ps) / sizeof(ps[0]); j++) {
            splash_result_t r = run(name, es[i], ps[j], params);
            const char *k = r.kind == SPLASH_ZONE ? "z" : r.kind == SPLASH_STEP ? "s" : r.kind == SPLASH_HITBOX ? "h" : " ";
            printf("%4d%s", damage(name, es[i], ps[j], params), k);
        }
        printf("\n");
    }
}

int main(void) {
    splash_params_t p = defaults();

    // A stair or a strip does not give cover.
    expect_kind("step", 8, 16, &p, SPLASH_STEP);
    expect_kind("step", 8, 64, &p, SPLASH_STEP);
    expect_kind("strip", 4, 16, &p, SPLASH_STEP);
    expect(run("strip", 20, 16, &p).rate == 1.0f, "strip: rocket on the floor before it, full damage");
    // A rocket right under the platform's edge reaches the player on it; deep below it does not.
    expect_kind("platform", 8, 32, &p, SPLASH_STEP);
    expect(run("platform", 40, 32, &p).rate < 0.1f, "platform: 40 below the edge stays (nearly) blocked");

    // Wall corner: the zone, fading as the player goes deeper and as the explosion moves off the corner.
    expect_kind("wall", 8, 16, &p, SPLASH_ZONE);
    expect_kind("wall", 8, 64, &p, SPLASH_NONE);
    {
        float a = run("wall", 8, 8, &p).rate, b = run("wall", 8, 16, &p).rate, c = run("wall", 8, 24, &p).rate;
        expect(a > b && b >= c, "wall: deeper behind the corner, less damage");
        expect(a <= 0.6f, "wall: never more than qlx_splashBendDamage");
        float near = run("wall", 4, 16, &p).rate, far = run("wall", 24, 16, &p).rate;
        expect(near > far, "wall: explosion nearer the corner, more damage");
    }
    // The ledge from above (the operator's drawing): some damage below the edge, none far away.
    expect_kind("ledge", 8, 16, &p, SPLASH_ZONE);
    expect_kind("ledge", 32, 96, &p, SPLASH_NONE);

    // Knobs.
    {
        splash_params_t q = p;
        q.step = 0;
        expect(run("step", 8, 16, &q).kind != SPLASH_STEP, "qlx_splashBendStep 0 turns the step rule off");
        q = p;
        q.damage = 0;
        expect(run("wall", 8, 16, &q).kind == SPLASH_NONE, "qlx_splashBendDamage 0 turns the zone off");
        q = p;
        q.angle = 30;
        expect(run("wall", 8, 16, &q).rate < run("wall", 8, 16, &p).rate, "a narrower angle, less damage");
        q = p;
        q.knockback = 0.3f;
        splash_result_t r = run("wall", 8, 16, &q);
        expect(fabsf(r.knock - r.rate / 2) < 1e-4f, "knockback scales the same way as damage");
    }
    {
        splash_params_t q = {NAN, 7, -1, 0, 9999, 400, 100};
        Splash_ClampParams(&q);
        expect(q.distance == 8 && q.damage == 1 && q.knockback == 0 && q.radius == 120 && q.bend_radius == 512 &&
                   q.angle == 90 && q.step == 64,
               "Splash_ClampParams");
    }
    {
        splash_result_t r = run("wall", 8, 16, &p);
        expect(r.traces < 400, "trace budget");
        printf("wall e=8 p=16: %s rate %.2f theta %.0f alpha %.0f hop %.0f, %u traces\n", Splash_KindName(r.kind),
               r.rate, r.theta, r.alpha, r.hop, r.traces);
    }

    table("step", &p);
    table("strip", &p);
    table("wall", &p);
    table("ledge", &p);
    table("platform", &p);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
