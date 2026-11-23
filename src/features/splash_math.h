// splash_math.h - where a rocket splash that a wall stopped may still reach a player, and how hard.
// No engine dependencies: the world comes in through callbacks, so a standalone harness can run it
// (test_splash_math.c). splash_bend.c wires it to the game.

#ifndef SPLASH_MATH_H
#define SPLASH_MATH_H

typedef float splash_vec3[3];

typedef struct {
    float distance;    // how far from the explosion the blast may go to turn a corner (qlx_splashBendDistance)
    float damage;      // share of the damage that gets through the zone (qlx_splashBendDamage)
    float knockback;   // share of the knockback, same (qlx_splashBendKnockback)
    float radius;      // the rocket's own splash radius, what the game's falloff uses
    float bend_radius; // the falloff radius for damage through the zone (qlx_splashBendRadius)
    float angle;       // widest zone, in degrees, for an explosion right at the corner (qlx_splashBendAngle)
    float step;        // highest rise that still counts as a step, 0 = no step rule (qlx_splashBendStep)
} splash_params_t;

typedef enum {
    SPLASH_NONE = 0, // stays blocked
    SPLASH_HITBOX,   // some part of the hitbox is in plain view: full damage
    SPLASH_STEP,     // seen from straight above the explosion, within a step's height: full damage
    SPLASH_SLAB,     // refused: the player is behind a slab
    SPLASH_ZONE,     // in the zone behind the corner: scaled
} splash_kind_t;

typedef struct {
    splash_kind_t kind;
    float rate;      // damage, as a share of what the game works out from the distance
    float knock;     // knockback, the same way
    float hop;       // how far the blast went before it turned (step, zone)
    float theta;     // zone: degrees the path turns at the hop
    float alpha;     // zone: how wide the zone was for that hop
    unsigned traces; // world traces spent
} splash_result_t;

typedef struct {
    // 1 when nothing solid lies between a and b.
    int (*visible)(void *ctx, const splash_vec3 a, const splash_vec3 b);
    // How far, 0..1, a ray from a towards b gets before it hits something; < 0 if a is inside a solid.
    float (*reach)(void *ctx, const splash_vec3 a, const splash_vec3 b);
    // 1 when the player is behind a slab (see splash_bend.c). May be NULL.
    int (*behind_slab)(void *ctx);
    void *ctx;
} splash_world_t;

// Called only for a player the stock check refused. mins/maxs are the player's absolute bounds.
splash_result_t Splash_Bend(const splash_vec3 origin, const splash_vec3 mins, const splash_vec3 maxs,
                            const splash_params_t *p, const splash_world_t *w);

// Clamps every field to its allowed range; the cvars can hold anything.
void Splash_ClampParams(splash_params_t *p);

const char *Splash_KindName(splash_kind_t kind);

#endif /* SPLASH_MATH_H */
