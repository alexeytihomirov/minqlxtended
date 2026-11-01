// splash_bend.c - rocket splash that bends around a nearby corner.
//
// CanDamage traces from the explosion to the target's centre and to four points 15 units to its
// sides. A rocket that hits the riser of a stair explodes 0.05 units off the riser and below the
// edge of the step, so that edge hides anyone standing on the step, however close. The same goes
// for any explosion tucked against a corner.
//
// With qlx_splashBend on, a player CanDamage refused gets a second chance: rays go out from the
// explosion, up to qlx_splashBendDistance units, and CanDamage is asked again from where each one
// ends. So the blast may turn one corner, and it never passes through anything, since both legs of
// the path are traced against the world. What stock lets through is not touched.
//
// A slab is refused: tracing from the player back to the explosion stops on the obstacle's face
// towards the player, and if that face looks away from the surface the rocket hit, the blast would
// have to come out on the far side of the obstacle, through a hole in a platform or around the end
// of a wall. On a stair the two faces are at a right angle.
//
// A bent blast is weaker, by qlx_splashBendDamage and qlx_splashBendKnockback. G_Damage works the
// damage of a rocket splash out again from g_splashdamage_rl and the distance, and uses what it is
// given only for the knockback. So the argument scales the knockback, and the cvar is scaled for
// the length of the call.
//
// Rockets only. CanDamage is not told what exploded, so the G_RadiusDamage hook notes it.
// G_RadiusDamage is the common path; the variant for surfaces a rocket splashes through
// (g_dmgThroughSurface*) is left alone. Game thread only, like everything that reaches here.

#include <string.h>

#include "common.h"
#include "engine/patterns.h"
#include "engine/quake_common.h"
#include "hook/simple_hook.h"
#include "server/maps_parser.h"
#include "splash_bend.h"

#define SPLASH_CONTENTS_SOLID 1
#define SPLASH_DISTANCE_MIN 8
#define SPLASH_DISTANCE_MAX 64
// A ray stopped this short by the surface next to the explosion shows nothing new.
#define SPLASH_RAY_MIN 4.0f
// Kept off whatever stopped the ray, so the next trace does not start inside it.
#define SPLASH_RAY_BACKOFF 1.0f
// CanDamage's test points, to the sides of the target's centre.
#define SPLASH_TARGET_SPREAD 15.0f
// Hit surface against the obstacle's face towards the player: 0 is a right angle, -1 a flat slab.
#define SPLASH_OPPOSED_FACES -0.5f
// The mod G_Damage recomputes the damage for.
#define SPLASH_MOD_ROCKET_SPLASH 7

#define D1 1.0f
#define D2 0.70710678f
#define D3 0.57735027f

// Up first: a stair riser is the case this exists for, and the first ray that sees the player
// ends the search.
static const vec3_t splash_rays[] = {
    {0, 0, D1},
    {D2, 0, D2}, {-D2, 0, D2}, {0, D2, D2}, {0, -D2, D2},
    {D3, D3, D3}, {D3, -D3, D3}, {-D3, D3, D3}, {-D3, -D3, D3},
    {D1, 0, 0}, {-D1, 0, 0}, {0, D1, 0}, {0, -D1, 0},
    {D2, D2, 0}, {D2, -D2, 0}, {-D2, D2, 0}, {-D2, -D2, 0},
    {D2, 0, -D2}, {-D2, 0, -D2}, {0, D2, -D2}, {0, -D2, -D2},
    {D3, D3, -D3}, {D3, -D3, -D3}, {-D3, D3, -D3}, {-D3, -D3, -D3},
    {0, 0, -D1},
};

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

static int in_rocket;
static const vec_t *rocket_normal; // of the surface hit; NULL if the game gave none
static gentity_t *bent_target;     // CanDamage needed the bend for this one, in this explosion

static int *splash_damage_rl; // .integer of qagame's g_splashdamage_rl
static int splash_damage_saved;
static int splash_damage_scaled; // between SplashBend_ScaleDamage and SplashBend_EndDamage

static struct {
    unsigned explosions; // rockets through G_RadiusDamage
    unsigned refused;    // players CanDamage refused during those
    unsigned opposed;    // ...of them, behind a slab
    unsigned bent;       // ...and how many of them the bend let through
    unsigned rays;
    unsigned scaled;     // hits G_Damage dealt at the bent rate
    int last_stock;      // the last of them: what the distance alone gives
    int last_dealt;
    int last_knockback;  // and what G_Damage was given to work the knockback from
} splash_stats;

static void splash_cvars(void) {
    if (Cvar_Get && !qlx_splashBend) {
        qlx_splashBend          = Cvar_Get("qlx_splashBend", "0", CVAR_ARCHIVE);
        qlx_splashBendDistance  = Cvar_Get("qlx_splashBendDistance", "24", CVAR_ARCHIVE);
        qlx_splashBendDamage    = Cvar_Get("qlx_splashBendDamage", "0.5", CVAR_ARCHIVE);
        qlx_splashBendKnockback = Cvar_Get("qlx_splashBendKnockback", "0.5", CVAR_ARCHIVE);
    }
}

static float splash_distance(void) {
    if (qlx_splashBendDistance->value < SPLASH_DISTANCE_MIN) {
        return SPLASH_DISTANCE_MIN;
    }
    return qlx_splashBendDistance->value > SPLASH_DISTANCE_MAX ? SPLASH_DISTANCE_MAX : qlx_splashBendDistance->value;
}

static float splash_fraction(const cvar_t *cvar) {
    if (cvar->value < 0.0f) {
        return 0.0f;
    }
    return cvar->value > 1.0f ? 1.0f : cvar->value;
}

static qboolean splash_behind_slab(gentity_t *targ, const vec_t *origin) {
    static const vec3_t zero       = {0, 0, 0};
    static const float sides[5][2] = {
        {0, 0},
        {SPLASH_TARGET_SPREAD, SPLASH_TARGET_SPREAD},
        {SPLASH_TARGET_SPREAD, -SPLASH_TARGET_SPREAD},
        {-SPLASH_TARGET_SPREAD, SPLASH_TARGET_SPREAD},
        {-SPLASH_TARGET_SPREAD, -SPLASH_TARGET_SPREAD},
    };
    Trace_ptr trace = *(Trace_ptr *)(*trace_table + RELOFFSET_GI_TRACE);

    if (!rocket_normal) {
        return qfalse;
    }
    for (int i = 0; i < 5; i++) {
        vec3_t point;
        trace_t tr;

        point[0] = (targ->r.absmin[0] + targ->r.absmax[0]) * 0.5f + sides[i][0];
        point[1] = (targ->r.absmin[1] + targ->r.absmax[1]) * 0.5f + sides[i][1];
        point[2] = (targ->r.absmin[2] + targ->r.absmax[2]) * 0.5f;
        trace(&tr, point, zero, zero, origin, ENTITYNUM_NONE, SPLASH_CONTENTS_SOLID);

        // A test point can sit in a wall next to the player.
        if (tr.startsolid || tr.fraction >= 1.0f) {
            continue;
        }
        float facing = tr.plane.normal[0] * rocket_normal[0] + tr.plane.normal[1] * rocket_normal[1] +
                       tr.plane.normal[2] * rocket_normal[2];
        if (facing < SPLASH_OPPOSED_FACES) {
            return qtrue;
        }
    }
    return qfalse;
}

static qboolean splash_bends_to(gentity_t *targ, const vec_t *origin) {
    static const vec3_t zero = {0, 0, 0};
    Trace_ptr trace          = *(Trace_ptr *)(*trace_table + RELOFFSET_GI_TRACE);
    float distance           = splash_distance();

    for (size_t i = 0; i < sizeof(splash_rays) / sizeof(splash_rays[0]); i++) {
        vec3_t end, hop;
        trace_t tr;

        for (int k = 0; k < 3; k++) {
            end[k] = origin[k] + splash_rays[i][k] * distance;
        }
        trace(&tr, origin, zero, zero, end, ENTITYNUM_NONE, SPLASH_CONTENTS_SOLID);
        splash_stats.rays++;

        // An explosion that is already in the wall has no corner to turn.
        if (tr.startsolid) {
            return qfalse;
        }

        float reach = tr.fraction * distance;
        if (reach < SPLASH_RAY_MIN) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            hop[k] = origin[k] + splash_rays[i][k] * (reach - SPLASH_RAY_BACKOFF);
        }
        if (CanDamage(targ, hop)) {
            return qtrue;
        }
    }
    return qfalse;
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
    if (splash_behind_slab(targ, origin)) {
        splash_stats.opposed++;
        return qfalse;
    }
    if (splash_bends_to(targ, origin)) {
        splash_stats.bent++;
        bent_target = targ;
        return qtrue;
    }
    return qfalse;
}

static qboolean __cdecl My_G_RadiusDamage(const vec_t *origin, gentity_t *inflictor, gentity_t *ignore,
                                          const vec_t *dir, const trace_t *impact, int skip_sight) {
    in_rocket     = inflictor && inflictor->s.weapon == WP_ROCKET_LAUNCHER;
    rocket_normal = impact ? impact->plane.normal : NULL;
    bent_target   = NULL;
    if (in_rocket) {
        splash_stats.explosions++;
    }

    qboolean hit = G_RadiusDamage(origin, inflictor, ignore, dir, impact, skip_sight);

    in_rocket   = 0;
    bent_target = NULL;
    return hit;
}

int SplashBend_ScaleDamage(gentity_t *targ, int mod, int *damage) {
    int given = *damage;

    if (!targ || targ != bent_target || given <= 0) {
        return given;
    }
    bent_target = NULL;

    float rate = splash_fraction(qlx_splashBendDamage);
    int dealt  = (int)(given * rate + 0.5f);

    if (mod == SPLASH_MOD_ROCKET_SPLASH && splash_damage_rl) {
        splash_damage_saved  = *splash_damage_rl;
        splash_damage_scaled = 1;
        *splash_damage_rl    = (int)(splash_damage_saved * rate + 0.5f);
        *damage              = (int)(given * splash_fraction(qlx_splashBendKnockback) + 0.5f);
    } else {
        *damage = dealt;
    }

    splash_stats.scaled++;
    splash_stats.last_stock     = given;
    splash_stats.last_dealt     = dealt;
    splash_stats.last_knockback = *damage;
    return dealt;
}

void SplashBend_EndDamage(void) {
    if (splash_damage_scaled) {
        splash_damage_scaled = 0;
        *splash_damage_rl    = splash_damage_saved;
    }
}

void SplashBend_Resolve(void) {
    CanDamage      = NULL;
    G_RadiusDamage = NULL;
    trace_table    = NULL;
    in_rocket      = 0;
    rocket_normal  = NULL;
    bent_target    = NULL;
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

    ENGINE_PRINTF("qlx_splashBend %d, distance %d, damage %.3f, knockback %.3f, %s%s\n",
                  qlx_splashBend ? qlx_splashBend->integer : 0,
                  qlx_splashBendDistance ? (int)splash_distance() : 0,
                  qlx_splashBendDamage ? splash_fraction(qlx_splashBendDamage) : 0.0f,
                  qlx_splashBendKnockback ? splash_fraction(qlx_splashBendKnockback) : 0.0f,
                  trace_table ? "hooks found" : "HOOKS NOT FOUND, stock splash",
                  splash_damage_rl ? "" : ", DAMAGE NOT SCALED");
    ENGINE_PRINTF("rockets %u, players refused by stock %u, behind a slab %u, let through by the bend %u, rays %u\n",
                  splash_stats.explosions, splash_stats.refused, splash_stats.opposed, splash_stats.bent,
                  splash_stats.rays);
    ENGINE_PRINTF("bent hits %u, last: stock %d, dealt %d, knockback from %d\n", splash_stats.scaled,
                  splash_stats.last_stock, splash_stats.last_dealt, splash_stats.last_knockback);
}
