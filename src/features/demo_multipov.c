// demo_multipov.c - one multi-POV .dm_91 per server, built from the server's own state.
//
// What the file has to look like is fixed by ql-tournament's offline merge (tools/demo-merge,
// tasks ql-tournament-0208/0209/0210), which builds the same file from N POV captures and is
// proven to play in the official client, wolfcam and the site player. The design, with the
// reasons for each rule, is docs/superpowers/specs/2026-11-20-multipov-server-recording-design.md
// in the monorepo. In short, per server frame:
//
//   message = long reliableAcknowledge (the last command sequence written)
//             { svc_serverCommand, long seq, string text }   commands since the last frame
//             svc_snapshot, long serverTime, byte deltaNum, byte snapFlags, areamask,
//             playerState delta, packet entities, svc_EOF
//
// and a gamestate at the start of every file (and every stream open). The engine's entityState_t
// and playerState_t are laid out differently from democut's wire structs, so everything is
// converted field by field before it reaches dc_msg_write_delta_*.
//
// Game thread only, all of it. The only engine state read is what SV_BuildClientSnapshot itself
// reads: sv.gentities, sv.gameClients, sv.configstrings, sv.svEntities[].baseline, plus the game
// module's session team to tell players from spectators.

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "demo_multipov.h"
#include "demos.h"
#include "stream.h"
#include "democut/dc_msg.h"
#include "engine/quake_common.h"

extern serverStatic_t *svs; // defined in dllmain.c
extern server_t *sv;        // defined in dllmain.c; NULL if its pattern did not resolve

#define MP_SLOT DEMO_MULTIPOV_SLOT

// The client keeps at most this many entities of one snapshot (MAX_ENTITIES_IN_SNAPSHOT), and a
// server never sends more (SV_AddEntToSnapshot stops at MAX_SNAPSHOT_ENTITIES).
#define MP_MAX_SNAPSHOT_ENTITIES 256

// Player data channel v1, as frontend/public/player/lib/qldemo/player-channel.js of
// ql-tournament reads it: player c (0..31) gets entities 1021 - 2c - page.
#define MP_CHANNEL_TOP         1021
#define MP_CHANNEL_MAX_CLIENTS 32
#define MP_CHANNEL_MAGIC       0xFB
#define MP_CHANNEL_VERSION     1

// What the offline merge writes for its spectator, and what a real follow spectator carries.
#define MP_SPECTATOR_VIEWHEIGHT 26
#define MP_PERS_TEAM            3 // persistant[] index the merge and real spectator demos use
#define MP_TEAM_SPECTATOR       3

// The recorder's own CS_PLAYERS row: the offline merge's, character for character.
static const char mp_player_row[] =
    "n\\multipov\\t\\3\\model\\sarge\\hmodel\\sarge\\c1\\1\\c2\\1\\hc\\100\\w\\0\\l\\0\\tt\\0\\tl\\0";

// One message, gamestate or frame. The demo reader and the site player refuse a block over 32 KB
// (MAX_MSGLEN 16384*2), so that is the ceiling, not the writer's MAX_NETCHAN_MSGLEN + 64.
static dc_byte mp_msg_buf[DC_MAX_MSGLEN];

// Server commands between two frames. The engine refuses a command longer than 1022 characters
// (SV_SendServerCommand), so nothing longer is kept either.
#define MP_CMD_RING        512
#define MP_CMD_MAX_LEN     1022
// Per message: the client keeps 64 server commands (MAX_RELIABLE_COMMANDS) and cgame fails on a
// command cycled out of that ring, and a message has to stay under 32 KB with a full snapshot in
// it. Anything past either bound waits for the next frame.
#define MP_CMDS_PER_MSG    48
#define MP_CMD_BYTES_PER_MSG 12288
static char mp_cmd_text[MP_CMD_RING][MP_CMD_MAX_LEN + 1];
static unsigned mp_cmd_head, mp_cmd_tail;

static int32_t mp_seq = 1;  // block sequence the next message gets
static int32_t mp_cmd_seq;  // last server command sequence written
static int mp_recording;    // a sink was open at the end of the last frame
static int mp_cmd_source = -1; // the client whose own commands are recorded, -1 for none

static int mp_last_time;
static int mp_have_time;
static int mp_serverid;
static int mp_serverid_valid;
static int mp_followed = -1;
static int mp_areabytes;

// The free spectator's place while nobody plays: the first info_player_intermission.
static int mp_anchor_looked;
static int mp_anchor_found;
static vec3_t mp_anchor_origin;
static vec3_t mp_anchor_angles;

// The last frame written, what the next one deltas against.
static int mp_prev_valid;
static dc_playerState_t mp_prev_ps;
static dc_entityState_t mp_prev_ents[MAX_GENTITIES];
static int mp_prev_count;

// Baselines as the reader has them: what the last gamestate sent, zero for the rest.
static dc_entityState_t mp_baselines[MAX_GENTITIES];

// This frame.
static dc_entityState_t mp_cur_ents[MAX_GENTITIES];
static int mp_cur_count;

typedef struct {
    dc_entityState_t es;
    int drop_class; // 0 never dropped, 1 ET_ITEM, 2 any other non-player entity
    float dist2;
    int dropped;
} mp_cand_t;
static mp_cand_t mp_cand[MAX_GENTITIES];
static dc_entityState_t mp_chan[2 * MP_CHANNEL_MAX_CLIENTS];
static uint8_t mp_used[MAX_GENTITIES];

// Once-per-match log lines (reset at every gamestate and every serverId change).
static int mp_logged_channel;
static int mp_logged_cap;
static int mp_logged_size;
static int mp_logged_cmds;

// ---------------------------------------------------------------------------
// engine -> democut wire structs
// ---------------------------------------------------------------------------

static void mp_conv_trajectory(dc_trajectory_t *o, const trajectory_t *t) {
    o->trType     = (int)t->trType;
    o->trTime     = t->trTime;
    o->trDuration = t->trDuration;
    memcpy(o->trBase, t->trBase, sizeof(o->trBase));
    memcpy(o->trDelta, t->trDelta, sizeof(o->trDelta));
    // The engine keeps gravity as a float; protocol 91 sends those 32 bits as an integer field.
    memcpy(&o->gravity, &t->gravity, sizeof(o->gravity));
}

static void mp_conv_entity(dc_entityState_t *o, const entityState_t *s, int number) {
    memset(o, 0, sizeof(*o));
    o->number = number;
    o->eType  = s->eType;
    o->eFlags = s->eFlags;
    mp_conv_trajectory(&o->pos, &s->pos);
    mp_conv_trajectory(&o->apos, &s->apos);
    o->time  = s->time;
    o->time2 = s->time2;
    memcpy(o->origin, s->origin, sizeof(o->origin));
    memcpy(o->origin2, s->origin2, sizeof(o->origin2));
    memcpy(o->angles, s->angles, sizeof(o->angles));
    memcpy(o->angles2, s->angles2, sizeof(o->angles2));
    o->otherEntityNum  = s->otherEntityNum;
    o->otherEntityNum2 = s->otherEntityNum2;
    o->groundEntityNum = s->groundEntityNum;
    o->constantLight   = s->constantLight;
    o->loopSound       = s->loopSound;
    o->modelindex      = s->modelindex;
    o->modelindex2     = s->modelindex2;
    o->clientNum       = s->clientNum;
    o->frame           = s->frame;
    o->solid           = s->solid;
    o->event           = s->event;
    o->eventParm       = s->eventParm;
    o->powerups        = s->powerups;
    o->weapon          = s->weapon;
    o->legsAnim        = s->legsAnim;
    o->torsoAnim       = s->torsoAnim;
    o->generic1        = s->generic1;
    o->jumpTime        = s->jumpTime;
    o->doubleJumped    = s->doubleJumped;
    o->health          = s->health;
    o->armor           = s->armor;
    o->location        = s->location;
}

static void mp_conv_ps(dc_playerState_t *o, const playerState_t *p) {
    memset(o, 0, sizeof(*o));
    o->commandTime = p->commandTime;
    o->pm_type     = p->pm_type;
    o->bobCycle    = p->bobCycle;
    o->pm_flags    = p->pm_flags;
    o->pm_time     = p->pm_time;
    memcpy(o->origin, p->origin, sizeof(o->origin));
    memcpy(o->velocity, p->velocity, sizeof(o->velocity));
    o->weaponTime = p->weaponTime;
    o->gravity    = p->gravity;
    o->speed      = p->speed;
    for (int i = 0; i < 3; i++) {
        o->delta_angles[i] = p->delta_angles[i];
    }
    o->groundEntityNum = p->groundEntityNum;
    o->legsTimer       = p->legsTimer;
    o->legsAnim        = p->legsAnim;
    o->torsoTimer      = p->torsoTimer;
    o->torsoAnim       = p->torsoAnim;
    o->movementDir     = p->movementDir;
    memcpy(o->grapplePoint, p->grapplePoint, sizeof(o->grapplePoint));
    o->eFlags        = p->eFlags;
    o->eventSequence = p->eventSequence;
    for (int i = 0; i < DC_MAX_PS_EVENTS; i++) {
        o->events[i]     = p->events[i];
        o->eventParms[i] = p->eventParms[i];
    }
    o->externalEvent     = p->externalEvent;
    o->externalEventParm = p->externalEventParm;
    o->clientNum         = p->clientNum;
    o->weapon            = p->weapon;
    o->weaponstate       = p->weaponstate;
    memcpy(o->viewangles, p->viewangles, sizeof(o->viewangles));
    o->viewheight  = p->viewheight;
    o->damageEvent = p->damageEvent;
    o->damageYaw   = p->damageYaw;
    o->damagePitch = p->damagePitch;
    o->damageCount = p->damageCount;
    for (int i = 0; i < DC_MAX_STATS; i++) {
        o->stats[i] = p->stats[i];
    }
    for (int i = 0; i < DC_MAX_PERSISTANT; i++) {
        o->persistant[i] = p->persistant[i];
    }
    for (int i = 0; i < DC_MAX_POWERUPS; i++) {
        o->powerups[i] = p->powerups[i];
    }
    for (int i = 0; i < DC_MAX_WEAPONS; i++) {
        o->ammo[i] = p->ammo[i];
    }
    o->generic1        = p->generic1;
    o->loopSound       = p->loopSound;
    o->jumppad_ent     = p->jumppad_ent;
    o->doubleJumped    = p->doubleJumped;
    o->jumpTime        = p->jumpTime;
    o->crouchTime      = p->crouchTime;
    o->crouchSlideTime = p->crouchSlideTime;
    o->location        = p->location;
    o->fov             = p->fov;
    o->forwardmove     = p->forwardmove;
    o->rightmove       = p->rightmove;
    o->upmove          = p->upmove;
    o->weaponPrimary   = p->weaponPrimary;
}

// ---------------------------------------------------------------------------
// world access
// ---------------------------------------------------------------------------

static int mp_maxclients(void) {
    int n = sv_maxclients ? sv_maxclients->integer : 0;
    if (n > MAX_CLIENTS) {
        n = MAX_CLIENTS;
    }
    if (level && level->maxclients < n) {
        n = level->maxclients;
    }
    return n < 0 ? 0 : n;
}

static const playerState_t *mp_ps(int c) {
    return (const playerState_t *)((const byte *)sv->gameClients + (size_t)c * (size_t)sv->gameClientSize);
}

static const sharedEntity_t *mp_gent(int n) {
    return (const sharedEntity_t *)((const byte *)sv->gentities + (size_t)n * (size_t)sv->gentitySize);
}

// Connected and in the game, i.e. not a spectator: the game module's own session team, the same
// test demo_match.c makes for a match POV.
static int mp_in_game(int c) {
    if (c < 0 || c >= mp_maxclients() || !level || !level->clients) {
        return 0;
    }
    if (!svs->clients || svs->clients[c].state < CS_CONNECTED) {
        return 0;
    }
    const gclient_t *cl = &level->clients[c];
    return cl->pers.connected == CON_CONNECTED && cl->sess.sessionTeam != TEAM_SPECTATOR;
}

// The lowest slot in the game, kept until that player leaves the game.
static void mp_update_followed(void) {
    if (mp_followed >= 0 && mp_in_game(mp_followed)) {
        return;
    }
    mp_followed = -1;
    int n       = mp_maxclients();
    for (int c = 0; c < n; c++) {
        if (mp_in_game(c)) {
            mp_followed = c;
            return;
        }
    }
}

// Whose own commands go in the file: the followed player's, and while nobody plays the lowest
// active client's, so configstring changes (sent per client) still reach the recording.
static void mp_update_cmd_source(void) {
    if (mp_followed >= 0) {
        mp_cmd_source = mp_followed;
        return;
    }
    mp_cmd_source = -1;
    int n         = mp_maxclients();
    for (int c = 0; c < n; c++) {
        if (svs->clients[c].state == CS_ACTIVE) {
            mp_cmd_source = c;
            return;
        }
    }
}

// The map's area byte count, as SV_BuildClientSnapshot last wrote it for any active client: a
// real client's last built frame is behind outgoingSequence, a bot's (never transmitted) at it.
static void mp_update_areabytes(void) {
    int n = mp_maxclients();
    for (int c = 0; c < n; c++) {
        const client_t *cl = &svs->clients[c];
        if (cl->state != CS_ACTIVE) {
            continue;
        }
        int bot = cl->gentity && (cl->gentity->r.svFlags & SVF_BOT);
        int idx = bot ? cl->netchan.outgoingSequence : cl->netchan.outgoingSequence - 1;
        int ab  = cl->frames[idx & PACKET_MASK].areabytes;
        if (ab >= 1 && ab <= DC_MAX_MAP_AREA_BYTES) {
            mp_areabytes = ab;
            return;
        }
    }
}

static void mp_find_anchor(void) {
    if (mp_anchor_looked || !g_entities || !level) {
        return;
    }
    mp_anchor_looked = 1;
    mp_anchor_found  = 0;
    int n            = level->num_entities;
    if (n > MAX_GENTITIES) {
        n = MAX_GENTITIES;
    }
    for (int i = 0; i < n; i++) {
        const gentity_t *e = &g_entities[i];
        if (e->inuse && e->classname && !strcmp(e->classname, "info_player_intermission")) {
            memcpy(mp_anchor_origin, e->s.origin, sizeof(mp_anchor_origin));
            memcpy(mp_anchor_angles, e->s.angles, sizeof(mp_anchor_angles));
            mp_anchor_found = 1;
            return;
        }
    }
}

static void mp_reset_logs(void) {
    mp_logged_channel = 0;
    mp_logged_cap     = 0;
    mp_logged_size    = 0;
    mp_logged_cmds    = 0;
}

// ---------------------------------------------------------------------------
// frame contents
// ---------------------------------------------------------------------------

// The followed player's playerState as a follow spectator gets it (PMF_FOLLOW, PM_SPECTATOR,
// measured on real QL spectator demos), or the free spectator while nobody plays.
static void mp_build_ps(dc_playerState_t *ps, int recorder, int server_time) {
    if (mp_followed >= 0) {
        mp_conv_ps(ps, mp_ps(mp_followed));
        ps->pm_flags |= PMF_FOLLOW;
        ps->pm_type = PM_SPECTATOR;
        return;
    }
    memset(ps, 0, sizeof(*ps));
    ps->clientNum                 = recorder;
    ps->pm_type                   = PM_SPECTATOR;
    ps->commandTime               = server_time;
    ps->viewheight                = MP_SPECTATOR_VIEWHEIGHT;
    ps->persistant[MP_PERS_TEAM] = MP_TEAM_SPECTATOR;
    mp_find_anchor();
    if (mp_anchor_found) {
        memcpy(ps->origin, mp_anchor_origin, sizeof(ps->origin));
        memcpy(ps->viewangles, mp_anchor_angles, sizeof(ps->viewangles));
    }
}

// Every entity a client could be sent, without the PVS: SV_AddEntitiesVisibleFromPoint's
// per-entity tests as they come out for a follow spectator of the followed player.
static int mp_collect_entities(const float *eye) {
    int count = 0;
    int n     = sv->num_entities;
    if (n > MAX_GENTITIES) {
        n = MAX_GENTITIES;
    }
    memset(mp_used, 0, sizeof(mp_used));
    for (int e = 0; e < n; e++) {
        const sharedEntity_t *ent = mp_gent(e);
        if (!ent->r.linked) {
            continue;
        }
        if (ent->s.eType < 0 || ent->s.eType > 0xFF) { // eType is 8 bits on the wire
            continue;
        }
        int flags = ent->r.svFlags;
        if (flags & SVF_NOCLIENT) {
            continue;
        }
        if ((flags & SVF_SINGLECLIENT) && (mp_followed < 0 || ent->r.singleClient != mp_followed)) {
            continue;
        }
        if ((flags & SVF_NOTSINGLECLIENT) && mp_followed >= 0 && ent->r.singleClient == mp_followed) {
            continue;
        }
        if ((flags & SVF_CLIENTMASK) &&
            (mp_followed < 0 || mp_followed >= 32 || !((unsigned)ent->r.singleClient & (1u << mp_followed)))) {
            continue;
        }
        if (e == mp_followed) {
            continue; // a follow spectator never gets the followed player's own entity
        }

        mp_cand_t *c = &mp_cand[count++];
        mp_conv_entity(&c->es, &ent->s, e);
        int type = ent->s.eType;
        if (type == ET_PLAYER || type == ET_MISSILE || type >= ET_EVENTS) {
            c->drop_class = 0;
        } else if (type == ET_ITEM) {
            c->drop_class = 1;
        } else {
            c->drop_class = 2;
        }
        float d[3];
        for (int k = 0; k < 3; k++) {
            d[k] = ent->r.currentOrigin[k] - eye[k];
        }
        c->dist2   = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        c->dropped = 0;
        mp_used[e] = 1;
    }
    return count;
}

static void mp_channel_float(dc_entityState_t *es, int index, int value) {
    float v = (float)value;
    switch (index / 3) {
    case 0: es->pos.trBase[index % 3] = v; break;
    case 1: es->pos.trDelta[index % 3] = v; break;
    case 2: es->apos.trBase[index % 3] = v; break;
    case 3: es->apos.trDelta[index % 3] = v; break;
    case 4: es->origin[index % 3] = v; break;
    case 5: es->origin2[index % 3] = v; break;
    case 6: es->angles[index % 3] = v; break;
    default: es->angles2[index % 3] = v; break;
    }
}

static void mp_channel_int(dc_entityState_t *es, int index, int value) {
    switch (index) {
    case 0: es->pos.trTime = value; break;
    case 1: es->pos.trDuration = value; break;
    case 2: es->pos.gravity = value; break;
    case 3: es->apos.trTime = value; break;
    case 4: es->apos.trDuration = value; break;
    case 5: es->apos.gravity = value; break;
    case 6: es->time = value; break;
    case 7: es->time2 = value; break;
    default: es->jumpTime = value; break;
    }
}

// The two channel pages of player c, in the container order of player-channel.js
// playerChannelEntities(): 24 floats, 9 int32s, weapon and legsAnim per page.
static void mp_channel_pages(dc_entityState_t out[2], const playerState_t *p, int c) {
    int floats[2][24];
    int ints[2][9];
    for (int i = 0; i < 16; i++) {
        floats[0][i] = p->ammo[i];
    }
    for (int i = 0; i < 8; i++) {
        floats[0][16 + i] = p->stats[i];
        floats[1][i]      = p->stats[8 + i];
    }
    for (int i = 0; i < 16; i++) {
        floats[1][8 + i] = p->persistant[i];
    }
    for (int i = 0; i < 9; i++) {
        ints[0][i] = p->powerups[i];
    }
    for (int i = 0; i < 7; i++) {
        ints[1][i] = p->powerups[9 + i];
    }
    ints[1][7] = p->pm_flags;
    ints[1][8] = p->eFlags;

    for (int page = 0; page < 2; page++) {
        dc_entityState_t *es = &out[page];
        memset(es, 0, sizeof(*es));
        es->number      = MP_CHANNEL_TOP - 2 * c - page;
        es->eType       = ET_INVISIBLE;
        es->modelindex2 = MP_CHANNEL_MAGIC;
        es->frame       = MP_CHANNEL_VERSION;
        es->clientNum   = c;
        es->generic1    = page;
        for (int i = 0; i < 24; i++) {
            mp_channel_float(es, i, floats[page][i]);
        }
        for (int i = 0; i < 9; i++) {
            mp_channel_int(es, i, ints[page][i]);
        }
    }
    out[0].weapon   = p->weapon;
    out[0].legsAnim = p->weaponstate;
    out[1].weapon   = p->pm_type;
}

// Channel entities for every in-game player, ascending by entity number.
static int mp_collect_channel(void) {
    int count = 0;
    int n     = mp_maxclients();
    if (n > MP_CHANNEL_MAX_CLIENTS) {
        n = MP_CHANNEL_MAX_CLIENTS;
    }
    // Highest client first gives the lowest entity numbers first; page 1 sits below page 0.
    for (int c = n - 1; c >= 0; c--) {
        if (!mp_in_game(c)) {
            continue;
        }
        int num0 = MP_CHANNEL_TOP - 2 * c;
        if (mp_used[num0] || mp_used[num0 - 1]) {
            if (!mp_logged_channel) {
                mp_logged_channel = 1;
                DebugPrint("demo: multipov: channel entity %d/%d of client %d is a real entity; "
                           "his channel is left out while it is\n",
                           num0, num0 - 1, c);
            }
            continue;
        }
        dc_entityState_t pages[2];
        mp_channel_pages(pages, mp_ps(c), c);
        mp_chan[count++] = pages[1];
        mp_chan[count++] = pages[0];
    }
    return count;
}

// Keeps the snapshot within MP_MAX_SNAPSHOT_ENTITIES: items farthest from the camera go first,
// then any other non-player entity. Players, missiles, events and the channel always stay.
static void mp_apply_cap(int real, int chan) {
    int over = real + chan - MP_MAX_SNAPSHOT_ENTITIES;
    if (over <= 0) {
        return;
    }
    int want = over;
    for (int cls = 1; cls <= 2 && over > 0; cls++) {
        while (over > 0) {
            int best = -1;
            for (int i = 0; i < real; i++) {
                if (!mp_cand[i].dropped && mp_cand[i].drop_class == cls &&
                    (best < 0 || mp_cand[i].dist2 > mp_cand[best].dist2)) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            mp_cand[best].dropped = 1;
            over--;
        }
    }
    // Nothing droppable is left: the server's own cap stops adding, so stop at the limit too.
    for (int i = real - 1; i >= 0 && over > 0; i--) {
        if (!mp_cand[i].dropped) {
            mp_cand[i].dropped = 1;
            over--;
        }
    }
    if (!mp_logged_cap) {
        mp_logged_cap = 1;
        DebugPrint("demo: multipov: %d entities in one frame, %d over the client's %d; dropped %d\n",
                   real + chan, want, MP_MAX_SNAPSHOT_ENTITIES, want);
    }
}

static void mp_merge_entities(int real, int chan) {
    int i = 0, j = 0;
    mp_cur_count = 0;
    while (i < real || j < chan) {
        while (i < real && mp_cand[i].dropped) {
            i++;
        }
        if (i >= real && j >= chan) {
            break;
        }
        if (j >= chan || (i < real && mp_cand[i].es.number < mp_chan[j].number)) {
            mp_cur_ents[mp_cur_count++] = mp_cand[i++].es;
        } else {
            mp_cur_ents[mp_cur_count++] = mp_chan[j++];
        }
    }
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

static int mp_finish(dc_msg_t *m) {
    if (m->overflowed || m->cursize <= 0 || m->cursize > DC_MAX_MSGLEN) {
        return 0;
    }
    return m->cursize;
}

// dc_parser.c's dc_write_gamestate, from the server's tables instead of a parsed demo.
static int mp_write_gamestate(int recorder) {
    static const dc_entityState_t null_state;
    dc_msg_t m;
    dc_msg_init(&m, mp_msg_buf, (int)sizeof(mp_msg_buf));

    dc_msg_write_long(&m, mp_cmd_seq);
    dc_msg_write_byte(&m, DC_SVC_GAMESTATE);
    dc_msg_write_long(&m, mp_cmd_seq);

    for (int i = 0; i < MAX_CONFIGSTRINGS; i++) {
        const char *cs = (i == CS_PLAYERS + recorder) ? mp_player_row : sv->configstrings[i];
        if (!cs || !cs[0]) {
            continue;
        }
        dc_msg_write_byte(&m, DC_SVC_CONFIGSTRING);
        dc_msg_write_short(&m, i);
        dc_msg_write_big_string(&m, cs);
    }

    // SV_SendClientGameState sends a baseline only when its number is set, so entity 0's never;
    // the reader deltas everything else against zero.
    for (int n = 0; n < MAX_GENTITIES; n++) {
        const entityState_t *base = &sv->svEntities[n].baseline;
        if (!base->number) {
            memset(&mp_baselines[n], 0, sizeof(mp_baselines[n]));
            continue;
        }
        mp_conv_entity(&mp_baselines[n], base, n);
        dc_msg_write_byte(&m, DC_SVC_BASELINE);
        dc_msg_write_delta_entity(&m, &null_state, &mp_baselines[n], 1);
    }

    dc_msg_write_byte(&m, DC_SVC_EOF);
    dc_msg_write_long(&m, recorder);
    dc_msg_write_long(&m, sv->checksumFeed);
    dc_msg_write_byte(&m, DC_SVC_EOF);
    return mp_finish(&m);
}

// dc_parser.c's dc_emit_packet_entities over the builder's own arrays.
static void mp_write_packet_entities(dc_msg_t *m, int full) {
    const dc_entityState_t *old = full ? NULL : mp_prev_ents;
    const int old_count         = full ? 0 : mp_prev_count;
    int oi = 0, ni = 0;

    while (ni < mp_cur_count || oi < old_count) {
        int newnum = ni < mp_cur_count ? mp_cur_ents[ni].number : 9999;
        int oldnum = oi < old_count ? old[oi].number : 9999;
        if (newnum == oldnum) {
            dc_msg_write_delta_entity(m, &old[oi], &mp_cur_ents[ni], 0);
            oi++;
            ni++;
        } else if (newnum < oldnum) {
            dc_msg_write_delta_entity(m, &mp_baselines[newnum], &mp_cur_ents[ni], 1);
            ni++;
        } else {
            dc_msg_write_delta_entity(m, &old[oi], NULL, 1);
            oi++;
        }
    }
    dc_msg_write_bits(m, MAX_GENTITIES - 1, GENTITYNUM_BITS);
}

// One frame. Takes up to the per-message bound of queued commands; *taken says how many.
static int mp_write_frame(const dc_playerState_t *ps, int full, int server_time, unsigned *taken) {
    // A full snapshot writes clientNum even when it is 0. The site player seeds a blank
    // playerState with the gamestate's clientNum (the recorder slot), so a follow of player 0
    // would otherwise read as the recorder (the offline merge does the same, ql-tournament-0210).
    static dc_playerState_t keyframe_base;
    keyframe_base.clientNum = -1;

    unsigned n = 0, bytes = 0;
    while (mp_cmd_tail + n != mp_cmd_head && n < MP_CMDS_PER_MSG) {
        size_t len = strlen(mp_cmd_text[(mp_cmd_tail + n) % MP_CMD_RING]);
        if (n > 0 && bytes + len > MP_CMD_BYTES_PER_MSG) {
            break;
        }
        bytes += (unsigned)len;
        n++;
    }
    *taken = n;

    dc_msg_t m;
    dc_msg_init(&m, mp_msg_buf, (int)sizeof(mp_msg_buf));
    dc_msg_write_long(&m, mp_cmd_seq + (int32_t)n);
    for (unsigned i = 0; i < n; i++) {
        dc_msg_write_byte(&m, DC_SVC_SERVERCOMMAND);
        dc_msg_write_long(&m, mp_cmd_seq + 1 + (int32_t)i);
        dc_msg_write_string(&m, mp_cmd_text[(mp_cmd_tail + i) % MP_CMD_RING]);
    }

    dc_msg_write_byte(&m, DC_SVC_SNAPSHOT);
    dc_msg_write_long(&m, server_time);
    dc_msg_write_byte(&m, full ? 0 : 1);
    dc_msg_write_byte(&m, svs->snapFlagServerBit);
    // Every area visible: the engine sends its areabits inverted, so zero bits are open areas.
    dc_msg_write_byte(&m, mp_areabytes);
    for (int i = 0; i < mp_areabytes; i++) {
        dc_msg_write_byte(&m, 0);
    }
    dc_msg_write_delta_playerstate(&m, full ? &keyframe_base : &mp_prev_ps, ps);
    mp_write_packet_entities(&m, full);
    dc_msg_write_byte(&m, DC_SVC_EOF);
    return mp_finish(&m);
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

int32_t MultiPov_NextSeq(void) {
    return mp_seq;
}

void MultiPov_ServerCommand(int slot, const char *text) {
    if (!mp_recording || !text) {
        return;
    }
    if (slot >= 0 && slot != mp_cmd_source) {
        return;
    }
    size_t len = strlen(text);
    if (len > MP_CMD_MAX_LEN) {
        return; // the engine drops it as well
    }
    if (mp_cmd_head - mp_cmd_tail >= MP_CMD_RING) {
        if (!mp_logged_cmds) {
            mp_logged_cmds = 1;
            DebugPrint("demo: multipov: more than %d server commands in one frame; the rest are not recorded\n",
                       MP_CMD_RING);
        }
        return;
    }
    memcpy(mp_cmd_text[mp_cmd_head % MP_CMD_RING], text, len + 1);
    mp_cmd_head++;
}

void MultiPov_Frame(void) {
    if (!svs || !svs->clients || !sv || !g_entities || !level || !level->clients || !sv_maxclients) {
        return;
    }
    if (atomic_load_explicit(&vm_rehooking, memory_order_acquire) || sv_spawning || sv->state != SS_GAME) {
        return;
    }
    if (!sv->gentities || !sv->gameClients || sv->gentitySize <= 0 || sv->gameClientSize <= 0) {
        return;
    }

    int disk_wanted   = Demo_MultiPovWanted();
    int stream_wanted = Stream_MultiPovWanted();
    if (!disk_wanted && !stream_wanted) {
        mp_recording  = 0;
        mp_prev_valid = 0;
        mp_cmd_tail   = mp_cmd_head;
        return;
    }

    // A new serverId is a map change or a map_restart. A map change has already closed both
    // sinks (Demo_CloseAll / Stream_CloseAll from SV_SpawnServer), so the gamestate follows from
    // that; a map_restart sends a client no gamestate and neither does this.
    if (!mp_serverid_valid || sv->serverId != mp_serverid) {
        mp_serverid       = sv->serverId;
        mp_serverid_valid = 1;
        mp_have_time      = 0;
        mp_anchor_looked  = 0;
        mp_reset_logs();
    }

    // One frame per server time: a catch-up run of G_RunFrame must not repeat a frame.
    if (mp_have_time && svs->time == mp_last_time) {
        return;
    }
    mp_last_time = svs->time;
    mp_have_time = 1;

    const int recorder = sv_maxclients->integer;
    mp_update_followed();
    mp_update_cmd_source();
    mp_update_areabytes();
    if (!mp_areabytes) {
        mp_areabytes = 1; // no client has had a snapshot yet; replaced by the real count when one has
    }

#ifdef NOPY
    {
        static int logged;
        if (!logged) {
            logged = 1;
            DebugPrint("demo: multipov: nopy build, the recording carries no server commands\n");
        }
    }
#endif

    int disk_open   = Demo_IsRecording(MP_SLOT);
    int stream_open = Stream_IsStreaming(MP_SLOT);
    int need_disk   = disk_wanted && !disk_open;
    int need_stream = stream_wanted && !stream_open;
    if (need_disk || need_stream) {
        int len = mp_write_gamestate(recorder);
        if (len <= 0) {
            if (!mp_logged_size) {
                mp_logged_size = 1;
                DebugPrint("demo: multipov: the gamestate does not fit %d bytes; not recording\n", DC_MAX_MSGLEN);
            }
        } else {
            int32_t seq = mp_seq++;
            if (need_disk && Demo_MultiPovWant(1)) {
                Demo_MultiPovBlock(seq, mp_msg_buf, (uint32_t)len);
            }
            if (need_stream && Stream_MultiPovWant(1, seq, mp_cmd_seq)) {
                Stream_MultiPovBlock(seq, 1, 0, mp_msg_buf, (uint32_t)len);
            }
            // The configstrings are in the gamestate; what was queued before it is in there too.
            mp_cmd_tail   = mp_cmd_head;
            mp_prev_valid = 0;
            mp_reset_logs();
        }
        disk_open   = Demo_IsRecording(MP_SLOT);
        stream_open = Stream_IsStreaming(MP_SLOT);
    }
    if (!disk_open && !stream_open) {
        mp_recording  = 0;
        mp_prev_valid = 0;
        return;
    }
    mp_recording = 1;

    dc_playerState_t ps;
    mp_build_ps(&ps, recorder, svs->time);

    int real = mp_collect_entities(ps.origin);
    int chan = mp_collect_channel();
    mp_apply_cap(real, chan);
    mp_merge_entities(real, chan);

    int full = !mp_prev_valid || (stream_open && Stream_MultiPovWantsFull());
    unsigned taken = 0;
    int len        = mp_write_frame(&ps, full, svs->time, &taken);
    int32_t seq    = mp_seq++;

    mp_cmd_tail += taken; // written or lost with the frame; never resent with a later one
    if (len <= 0) {
        if (!mp_logged_size) {
            mp_logged_size = 1;
            DebugPrint("demo: multipov: a frame does not fit %d bytes; skipped, the next is a full snapshot\n",
                       DC_MAX_MSGLEN);
        }
        mp_prev_valid = 0;
        return;
    }
    mp_cmd_seq += (int32_t)taken;

    // A sink that does not take the frame either closed (the disk reopens at a gamestate) or noted
    // the drop (the stream asks for a full snapshot), so the next delta is never against it.
    if (disk_open && Demo_MultiPovWant(0)) {
        Demo_MultiPovBlock(seq, mp_msg_buf, (uint32_t)len);
    }
    if (stream_open && Stream_MultiPovWant(0, seq, mp_cmd_seq)) {
        Stream_MultiPovBlock(seq, 0, full, mp_msg_buf, (uint32_t)len);
    }

    mp_prev_ps    = ps;
    mp_prev_count = mp_cur_count;
    memcpy(mp_prev_ents, mp_cur_ents, (size_t)mp_cur_count * sizeof(mp_cur_ents[0]));
    mp_prev_valid = 1;
}
