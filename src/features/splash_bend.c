// splash_bend.c - rocket splash that reaches round a step or, weakened, a little way round a corner.
//
// CanDamage traces from the explosion to the target's centre and to four points 15 units to its
// sides, all at the centre's height. A rocket that hits the riser of a stair explodes 0.05 units off
// the riser and below the edge of the step, so that edge hides anyone standing on the step, however
// close. The same goes for a strip on the floor, and for any explosion tucked against a corner.
//
// With qlx_splashBend on, a player CanDamage refused is looked at again by Splash_Bend
// (splash_math.c, which has the rules): any part of the hitbox in plain view, or seen from straight
// above the explosion within qlx_splashBendStep units, takes full damage; behind a corner, within
// qlx_splashBendDistance of the explosion, the player is in a zone qlx_splashBendAngle degrees wide
// and takes qlx_splashBendDamage of the damage, less the deeper into the zone, with the falloff of
// qlx_splashBendRadius. Every path is traced against the world, so nothing passes through a wall.
// What stock lets through is not touched.
//
// A slab is refused before the zone: tracing from the player back to the explosion stops on the
// obstacle's face towards the player, and if that face looks away from the surface the rocket hit,
// the blast would have to come out on the far side of the obstacle, through a hole in a platform or
// around the end of a wall. On a stair the two faces are at a right angle. The step rule comes
// first, because the back of a strip on the floor looks just like a slab.
//
// G_Damage works the damage of a rocket splash out again from g_splashdamage_rl and the distance,
// and uses what it is given only for the knockback. So for a player in the zone the argument scales
// the knockback, and the cvar is scaled for the length of the call.
//
// All the cvars are read on every explosion, so they can be tuned in a running game. With
// qlx_splashBendDebug 1 every player the rules let through gets a line printed to everyone.
//
// Rockets only. CanDamage is not told what exploded, so the G_RadiusDamage hook notes it.
// G_RadiusDamage is the common path; the variant for surfaces a rocket splashes through
// (g_dmgThroughSurface*) is left alone. Game thread only, like everything that reaches here.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "engine/patterns.h"
#include "engine/quake_common.h"
#include "hook/simple_hook.h"
#include "server/maps_parser.h"
#include "splash_bend.h"
#include "splash_math.h"

#define SPLASH_CONTENTS_SOLID 1
// CanDamage's test points, to the sides of the target's centre.
#define SPLASH_TARGET_SPREAD 15.0f
// Hit surface against the obstacle's face towards the player: 0 is a right angle, -1 a flat slab.
#define SPLASH_OPPOSED_FACES -0.5f
// The mod G_Damage recomputes the damage for.
#define SPLASH_MOD_ROCKET_SPLASH 7
// What a rocket's splashRadius is when the entity carries none.
#define SPLASH_DEFAULT_RADIUS 120.0f

typedef qboolean(__cdecl *CanDamage_ptr)(gentity_t *targ, const vec_t *origin);
typedef qboolean(__cdecl *G_RadiusDamage_ptr)(const vec_t *origin, gentity_t *inflictor, gentity_t *ignore,
                                              const vec_t *dir, const trace_t *impact, int skip_sight);
typedef void(__cdecl *Trace_ptr)(trace_t *results, const vec_t *start, const vec_t *mins, const vec_t *maxs,
                                 const vec_t *end, int pass_entity, int mask);

static CanDamage_ptr CanDamage;
static G_RadiusDamage_ptr G_RadiusDamage;
static pint *trace_table; // qagame's pointer to the engine's import table; filled in by the engine

static cvar_t *qlx_splashBend;
static cvar_t *qlx_splashBendDistance;
static cvar_t *qlx_splashBendDamage;
static cvar_t *qlx_splashBendKnockback;
static cvar_t *qlx_splashBendRadius;
static cvar_t *qlx_splashBendAngle;
static cvar_t *qlx_splashBendStep;
static cvar_t *qlx_splashBendDebug;

static int in_rocket;
static const vec_t *rocket_normal; // of the surface hit; NULL if the game gave none
static float rocket_radius;

// The player the rules let through in this explosion, and how. G_RadiusDamage damages each player
// right after asking CanDamage about them, so one at a time is enough.
static gentity_t *noted_target;
static splash_result_t noted;

static int *splash_damage_rl; // .integer of qagame's g_splashdamage_rl
static int splash_damage_saved;
static int splash_damage_scaled; // between SplashBend_ScaleDamage and SplashBend_EndDamage
static gentity_t *damage_target; // the same span: whose health and armour to report
static int damage_health, damage_armor;

static struct {
    unsigned explosions; // rockets through G_RadiusDamage
    unsigned refused;    // players CanDamage refused during those
    unsigned kinds[SPLASH_ZONE + 1];
    unsigned traces;
    splash_result_t last; // the last player let through
    int last_health, last_armor;
} splash_stats;

static void splash_cvars(void) {
    if (Cvar_Get && !qlx_splashBend) {
        qlx_splashBend          = Cvar_Get("qlx_splashBend", "0", CVAR_ARCHIVE);
        qlx_splashBendDistance  = Cvar_Get("qlx_splashBendDistance", "32", CVAR_ARCHIVE);
        qlx_splashBendDamage    = Cvar_Get("qlx_splashBendDamage", "0.6", CVAR_ARCHIVE);
        qlx_splashBendKnockback = Cvar_Get("qlx_splashBendKnockback", "0.6", CVAR_ARCHIVE);
        qlx_splashBendRadius    = Cvar_Get("qlx_splashBendRadius", "96", CVAR_ARCHIVE);
        qlx_splashBendAngle     = Cvar_Get("qlx_splashBendAngle", "60", CVAR_ARCHIVE);
        qlx_splashBendStep      = Cvar_Get("qlx_splashBendStep", "20", CVAR_ARCHIVE);
        qlx_splashBendDebug     = Cvar_Get("qlx_splashBendDebug", "0", 0);
    }
}

static splash_params_t splash_params(void) {
    splash_params_t p = {
        qlx_splashBendDistance->value, qlx_splashBendDamage->value, qlx_splashBendKnockback->value,
        rocket_radius,                 qlx_splashBendRadius->value, qlx_splashBendAngle->value,
        qlx_splashBendStep->value,
    };
    Splash_ClampParams(&p);
    return p;
}

static void splash_trace(trace_t *tr, const vec_t *a, const vec_t *b) {
    static const vec3_t zero = {0, 0, 0};
    Trace_ptr trace          = *(Trace_ptr *)(*trace_table + RELOFFSET_GI_TRACE);
    trace(tr, a, zero, zero, b, ENTITYNUM_NONE, SPLASH_CONTENTS_SOLID);
}

static int world_visible(void *ctx, const splash_vec3 a, const splash_vec3 b) {
    trace_t tr;
    (void)ctx;
    splash_trace(&tr, a, b);
    return !tr.startsolid && tr.fraction >= 1.0f;
}

static float world_reach(void *ctx, const splash_vec3 a, const splash_vec3 b) {
    trace_t tr;
    (void)ctx;
    splash_trace(&tr, a, b);
    return tr.startsolid ? -1.0f : tr.fraction;
}

typedef struct {
    gentity_t *targ;
    const vec_t *origin;
} slab_ctx_t;

static int world_behind_slab(void *ctx) {
    const slab_ctx_t *c            = ctx;
    gentity_t *targ                = c->targ;
    static const float sides[5][2] = {
        {0, 0},
        {SPLASH_TARGET_SPREAD, SPLASH_TARGET_SPREAD},
        {SPLASH_TARGET_SPREAD, -SPLASH_TARGET_SPREAD},
        {-SPLASH_TARGET_SPREAD, SPLASH_TARGET_SPREAD},
        {-SPLASH_TARGET_SPREAD, -SPLASH_TARGET_SPREAD},
    };

    if (!rocket_normal) {
        return 0;
    }
    for (int i = 0; i < 5; i++) {
        vec3_t point;
        trace_t tr;

        point[0] = (targ->r.absmin[0] + targ->r.absmax[0]) * 0.5f + sides[i][0];
        point[1] = (targ->r.absmin[1] + targ->r.absmax[1]) * 0.5f + sides[i][1];
        point[2] = (targ->r.absmin[2] + targ->r.absmax[2]) * 0.5f;
        splash_trace(&tr, point, c->origin);

        // A test point can sit in a wall next to the player.
        if (tr.startsolid || tr.fraction >= 1.0f) {
            continue;
        }
        float facing = tr.plane.normal[0] * rocket_normal[0] + tr.plane.normal[1] * rocket_normal[1] +
                       tr.plane.normal[2] * rocket_normal[2];
        if (facing < SPLASH_OPPOSED_FACES) {
            return 1;
        }
    }
    return 0;
}

static qboolean __cdecl My_CanDamage(gentity_t *targ, const vec_t *origin) {
    if (CanDamage(targ, origin)) {
        return qtrue;
    }
    // Doors and other shootable brushes stay as they were.
    if (!in_rocket || !targ->client || !qlx_splashBend || !qlx_splashBend->integer || !trace_table) {
        return qfalse;
    }

    splash_stats.refused++;
    splash_params_t p    = splash_params();
    slab_ctx_t slab      = {targ, origin};
    splash_world_t world = {world_visible, world_reach, world_behind_slab, &slab};
    splash_result_t r    = Splash_Bend(origin, targ->r.absmin, targ->r.absmax, &p, &world);

    splash_stats.kinds[r.kind]++;
    splash_stats.traces += r.traces;
    if (r.kind != SPLASH_HITBOX && r.kind != SPLASH_STEP && r.kind != SPLASH_ZONE) {
        return qfalse;
    }
    noted_target = targ;
    noted        = r;
    return qtrue;
}

static qboolean __cdecl My_G_RadiusDamage(const vec_t *origin, gentity_t *inflictor, gentity_t *ignore,
                                          const vec_t *dir, const trace_t *impact, int skip_sight) {
    in_rocket     = inflictor && inflictor->s.weapon == WP_ROCKET_LAUNCHER;
    rocket_normal = impact ? impact->plane.normal : NULL;
    rocket_radius = in_rocket && inflictor->splashRadius > 0 ? (float)inflictor->splashRadius : SPLASH_DEFAULT_RADIUS;
    noted_target  = NULL;
    if (in_rocket) {
        splash_stats.explosions++;
    }

    qboolean hit = G_RadiusDamage(origin, inflictor, ignore, dir, impact, skip_sight);

    in_rocket    = 0;
    noted_target = NULL;
    return hit;
}

static int armor_of(gentity_t *ent) {
    return ent->client ? ent->client->ps.stats[STAT_ARMOR] : 0;
}

int SplashBend_ScaleDamage(gentity_t *targ, int mod, int *damage) {
    int given = *damage;

    if (!targ || targ != noted_target || given <= 0) {
        return given;
    }
    noted_target  = NULL;
    damage_target = targ;
    damage_health = targ->health;
    damage_armor  = armor_of(targ);
    splash_stats.last = noted;

    if (noted.rate >= 1.0f && noted.knock >= 1.0f) {
        return given;
    }
    int dealt = (int)(given * noted.rate + 0.5f);
    if (mod == SPLASH_MOD_ROCKET_SPLASH && splash_damage_rl) {
        splash_damage_saved  = *splash_damage_rl;
        splash_damage_scaled = 1;
        *splash_damage_rl    = (int)(splash_damage_saved * noted.rate + 0.5f);
        *damage              = (int)(given * noted.knock + 0.5f);
    } else {
        *damage = dealt;
    }
    return dealt;
}

void SplashBend_EndDamage(void) {
    if (splash_damage_scaled) {
        splash_damage_scaled = 0;
        *splash_damage_rl    = splash_damage_saved;
    }
    if (!damage_target) {
        return;
    }
    gentity_t *targ = damage_target;
    damage_target   = NULL;

    splash_stats.last_health = damage_health - targ->health;
    splash_stats.last_armor  = damage_armor - armor_of(targ);
    if (!qlx_splashBendDebug || !qlx_splashBendDebug->integer || !SV_SendServerCommand) {
        return;
    }

    const splash_result_t *r = &splash_stats.last;
    char name[40], line[200];
    snprintf(name, sizeof(name), "%s", targ->client ? targ->client->pers.netname : "?");
    for (char *c = name; *c; c++) {
        if (*c == '"' || *c == '\n') {
            *c = '\'';
        }
    }
    if (r->kind == SPLASH_ZONE) {
        snprintf(line, sizeof(line), "splash: %s^7 zone %.0f/%.0f deg, hop %.0f, x%.2f: -%d hp -%d armor\n", name,
                 r->theta, r->alpha, r->hop, r->rate, splash_stats.last_health, splash_stats.last_armor);
    } else if (r->kind == SPLASH_STEP) {
        snprintf(line, sizeof(line), "splash: %s^7 step, hop %.0f: -%d hp -%d armor\n", name, r->hop,
                 splash_stats.last_health, splash_stats.last_armor);
    } else {
        snprintf(line, sizeof(line), "splash: %s^7 hitbox: -%d hp -%d armor\n", name, splash_stats.last_health,
                 splash_stats.last_armor);
    }
    SV_SendServerCommand(NULL, "print \"%s\"", line);
}

void SplashBend_Resolve(void) {
    CanDamage            = NULL;
    G_RadiusDamage       = NULL;
    trace_table          = NULL;
    in_rocket            = 0;
    rocket_normal        = NULL;
    noted_target         = NULL;
    damage_target        = NULL;
    splash_damage_rl     = NULL;
    splash_damage_scaled = 0;
    splash_cvars();

    pint can_damage = (pint)PatternSearch((void *)((pint)qagame + 0xB000), 0xB0000, PTRN_CANDAMAGE, MASK_CANDAMAGE);
    pint radius     = (pint)PatternSearch((void *)((pint)qagame + 0xB000), 0xB0000, PTRN_G_RADIUSDAMAGE, MASK_G_RADIUSDAMAGE);
    if (!can_damage || !radius) {
        DebugPrint("WARNING: Unable to find CanDamage (%p) or G_RadiusDamage (%p). qlx_splashBend will have no "
                   "effect.\n",
                   (void *)can_damage, (void *)radius);
        return;
    }

    // CanDamage's first load of the import table, a GOT slot holding the address of qagame's own
    // pointer to it. Followed only this far here: the engine fills that pointer in dllEntry.
    pint relp = can_damage + RELOFFSET_RELP_GI_CANDAMAGE;
    pint slot = *(int32_t *)relp + relp + 4;
    if (!InVm(slot) || !InVm(*(pint *)slot)) {
        DebugPrint("WARNING: The trace import resolved outside qagame. qlx_splashBend will have no effect.\n");
        return;
    }

    // G_Damage's read of g_splashdamage_rl, in the branch for a rocket splash: mov rax, [rip + rel32].
    // Without it the knockback and any other mod still scale.
    pint g_damage = (pint)PatternSearch((void *)((pint)qagame + 0xB000), 0xB0000, PTRN_G_DAMAGE, MASK_G_DAMAGE);
    if (g_damage && !memcmp((void *)(g_damage + RELOFFSET_LOAD_SPLASHDAMAGE_RL), "\x48\x8b\x05", 3)) {
        pint relp_cvar = g_damage + RELOFFSET_LOAD_SPLASHDAMAGE_RL + 3;
        pint cvar_slot = *(int32_t *)relp_cvar + relp_cvar + 4;
        if (InVm(cvar_slot) && InVm(*(pint *)cvar_slot)) {
            splash_damage_rl = (int *)(*(pint *)cvar_slot + VMCVAR_INTEGER);
        }
    }
    if (!splash_damage_rl) {
        DebugPrint("WARNING: Unable to find g_splashdamage_rl in G_Damage. qlx_splashBendDamage will not reduce a "
                   "rocket splash.\n");
    }

    trace_table    = (pint *)*(pint *)slot;
    CanDamage      = (CanDamage_ptr)can_damage;
    G_RadiusDamage = (G_RadiusDamage_ptr)radius;
    DebugPrint("CanDamage: %p, G_RadiusDamage: %p\n", (void *)CanDamage, (void *)G_RadiusDamage);
}

int SplashBend_Hook(void) {
    if (!CanDamage || !G_RadiusDamage) {
        return 0;
    }

    int res = Hook((void *)CanDamage, My_CanDamage, (void *)&CanDamage);
    if (res) {
        DebugPrint("WARNING: Failed to hook CanDamage: %d. qlx_splashBend will have no effect.\n", res);
        return 0;
    }
    // CanDamage alone is harmless: without the note from here it never bends anything.
    res = Hook((void *)G_RadiusDamage, My_G_RadiusDamage, (void *)&G_RadiusDamage);
    if (res) {
        DebugPrint("WARNING: Failed to hook G_RadiusDamage: %d. qlx_splashBend will have no effect.\n", res);
        return 1;
    }
    return 2;
}

void SplashBend_Command(void) {
    splash_cvars();

    if (Cmd_Argc() > 1) {
        const char *arg = Cmd_Argv(1);
        if (!strcmp(arg, "reset")) {
            memset(&splash_stats, 0, sizeof(splash_stats));
        } else if (!strcmp(arg, "0") || !strcmp(arg, "1")) {
            Cvar_Set2("qlx_splashBend", arg, qtrue);
        } else {
            ENGINE_PRINTF("Usage: %s [0|1|reset]\n", Cmd_Argv(0));
            return;
        }
    }
    if (!qlx_splashBend) {
        return;
    }

    splash_params_t p = splash_params();
    ENGINE_PRINTF("qlx_splashBend %d, distance %.0f, damage %.2f, knockback %.2f, radius %.0f, angle %.0f, step %.0f, "
                  "debug %d, %s%s\n",
                  qlx_splashBend->integer, p.distance, p.damage, p.knockback, p.bend_radius, p.angle, p.step,
                  qlx_splashBendDebug->integer, trace_table ? "hooks found" : "HOOKS NOT FOUND, stock splash",
                  splash_damage_rl ? "" : ", DAMAGE NOT SCALED");
    ENGINE_PRINTF("rockets %u, players refused by stock %u: hitbox %u, step %u, zone %u, slab %u, blocked %u; "
                  "traces %u\n",
                  splash_stats.explosions, splash_stats.refused, splash_stats.kinds[SPLASH_HITBOX],
                  splash_stats.kinds[SPLASH_STEP], splash_stats.kinds[SPLASH_ZONE], splash_stats.kinds[SPLASH_SLAB],
                  splash_stats.kinds[SPLASH_NONE], splash_stats.traces);
    const splash_result_t *r = &splash_stats.last;
    ENGINE_PRINTF("last: %s, theta %.0f of %.0f, hop %.0f, damage x%.2f, knockback x%.2f, %d traces: -%d hp -%d "
                  "armor\n",
                  Splash_KindName(r->kind), r->theta, r->alpha, r->hop, r->rate, r->knock, r->traces,
                  splash_stats.last_health, splash_stats.last_armor);
}
