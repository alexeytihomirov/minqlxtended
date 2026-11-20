// Standalone harness for the multi-POV recording (demo_multipov.c) and its disk writer (demos.c).
//
// Build (from the repository root):
//     gcc -std=gnu11 -O2 -Wall -Wextra -pthread -Isrc -o test_multipov
//         src/features/test_multipov.c src/features/demo_multipov.c src/features/demos.c
//         src/features/demo_name.c src/democut/dc_parser.c src/democut/dc_msg_read.c
//         src/democut/dc_msg_write.c src/democut/dc_fields.c src/democut/dc_huffman.c
//     (one command line)
//
// Usage:
//     ./test_multipov <home_dir>
//
// Feeds a synthetic two-player world (fake server_t, sharedEntity_t, gclient_t and playerState
// arrays, in the engine's own layouts from quake_common.h) through MultiPov_Frame for twelve
// server frames, with the real demos.c ring, writer thread, .part and rename behind it. The file
// lands in <home_dir>/mp/ and is read back with democut's dc_parser, checking per frame: the
// follow playerState, which entities are and are not there, the player data channel, the server
// commands and the snapshot header. It also writes <home_dir>/mp/expected.jsonl, one line per
// frame, for a second reader (ql-tournament's JS parser and readPlayerChannel) to check against.
// Prints the file's path and exits non-zero on a failure.
//
// Not part of the Makefile on purpose, like src/democut's harnesses: it has its own main() and
// must not link into the .so. The stream side is stubbed out (no relay here).

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "democut/dc_parser.h"
#include "features/demo_multipov.h"
#include "features/demos.h"
#include "features/profile.h"
#include "features/stream.h"
#include "engine/quake_common.h"

// ---------------------------------------------------------------------------
// the engine, faked
// ---------------------------------------------------------------------------

#define NCLIENTS 4 // sv_maxclients, so the recorder is slot 4

static serverStatic_t svs_store;
static server_t sv_store;
static client_t clients_store[NCLIENTS];
static gentity_t gents[MAX_GENTITIES];
static gclient_t gclients[NCLIENTS];
static level_locals_t level_store;

serverStatic_t *svs = &svs_store;
server_t *sv        = &sv_store;
gentity_t *g_entities;
level_locals_t *level;
cvar_t *sv_maxclients;
atomic_int vm_rehooking;
int sv_spawning;
int prof_enabled;
MSG_WriteBits_ptr MSG_WriteBits;

uint64_t Profile_Now(void) { return 0; }
void Profile_Record(prof_id_t id, uint64_t ns) { (void)id; (void)ns; }

void DebugPrint(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [log] ");
    vprintf(fmt, ap);
    va_end(ap);
}

// Cvars: a small table, created on first Cvar_Get like the engine's.
#define NCVARS 32
static cvar_t cvars[NCVARS];
static char cvar_names[NCVARS][64];
static char cvar_values[NCVARS][256];
static int ncvars;

static cvar_t *cvar_find(const char *name) {
    for (int i = 0; i < ncvars; i++) {
        if (!strcmp(cvar_names[i], name)) {
            return &cvars[i];
        }
    }
    return NULL;
}

static cvar_t *cvar_set(const char *name, const char *value) {
    cvar_t *c = cvar_find(name);
    if (!c) {
        c = &cvars[ncvars];
        snprintf(cvar_names[ncvars], sizeof(cvar_names[0]), "%s", name);
        c->name = cvar_names[ncvars];
        ncvars++;
    }
    int i = (int)(c - cvars);
    snprintf(cvar_values[i], sizeof(cvar_values[0]), "%s", value);
    c->string  = cvar_values[i];
    c->integer = atoi(value);
    c->value   = (float)atof(value);
    return c;
}

static cvar_t *fake_cvar_get(const char *name, const char *value, int flags) {
    (void)flags;
    cvar_t *c = cvar_find(name);
    return c ? c : cvar_set(name, value);
}

static cvar_t *fake_cvar_findvar(const char *name) { return cvar_find(name); }

Cvar_Get_ptr Cvar_Get         = fake_cvar_get;
Cvar_FindVar_ptr Cvar_FindVar = fake_cvar_findvar;

// The stream is not under test here: no relay, so it never wants anything.
int Stream_Interested(int slot) { (void)slot; return 0; }
int Stream_Want(int slot, int is_gamestate, client_t *client) { (void)slot; (void)is_gamestate; (void)client; return 0; }
void Stream_Block(int slot, int seq, int is_gamestate, client_t *client, const unsigned char *data, uint32_t len) {
    (void)slot; (void)seq; (void)is_gamestate; (void)client; (void)data; (void)len;
}
qboolean Stream_IsStreaming(int slot) { (void)slot; return qfalse; }
int Stream_MultiPovWanted(void) { return 0; }
int Stream_MultiPovWant(int is_gamestate, int seq, int command_seq) { (void)is_gamestate; (void)seq; (void)command_seq; return 0; }
int Stream_MultiPovWantsFull(void) { return 0; }
void Stream_MultiPovBlock(int seq, int is_gamestate, int is_full, const unsigned char *data, uint32_t len) {
    (void)seq; (void)is_gamestate; (void)is_full; (void)data; (void)len;
}

// ---------------------------------------------------------------------------
// the world, and what each frame should come out as
// ---------------------------------------------------------------------------

#define NFRAMES 12
#define MAX_EXPECT_CMDS 16

typedef struct {
    int time;
    int follow;    // clientNum the playerState should carry
    int spectator; // 1: free spectator (nobody playing)
    int full;      // deltaNum 0
    int present[MAX_GENTITIES];
    int npresent;
    int absent[64];
    int nabsent;
    int entity_count; // -1: not checked
    int chan_clients[NCLIENTS];
    int nchan;
    playerState_t chan_ps[NCLIENTS]; // the playerState each channel entry must equal
    playerState_t ps_of[NCLIENTS];   // every client's playerState this frame
} expect_t;

static expect_t expect[NFRAMES];

typedef struct {
    int seq;
    char text[256];
} expect_cmd_t;
static expect_cmd_t expect_cmds[MAX_EXPECT_CMDS];
static int nexpect_cmds;

static char configstring_store[8][256];

static void link_entity(int n, int etype, float x, float y, float z) {
    gentity_t *e = &gents[n];
    memset(e, 0, sizeof(*e));
    e->s.number       = n;
    e->s.eType        = etype;
    e->s.pos.trBase[0] = x;
    e->s.pos.trBase[1] = y;
    e->s.pos.trBase[2] = z;
    e->r.currentOrigin[0] = x;
    e->r.currentOrigin[1] = y;
    e->r.currentOrigin[2] = z;
    e->r.linked       = qtrue;
    e->inuse          = qtrue;
}

static void unlink_entity(int n) {
    gents[n].r.linked = qfalse;
}

static void set_in_game(int c, int in_game) {
    gclients[c].pers.connected   = CON_CONNECTED;
    gclients[c].sess.sessionTeam = in_game ? TEAM_FREE : TEAM_SPECTATOR;
}

// The playerState of client c at frame f: odd ammo, a negative stat, a powerup timer past 2^16,
// all moving from frame to frame so deltas carry them.
static void fill_ps(int c, int f) {
    playerState_t *ps = &gclients[c].ps;
    ps->commandTime    = 1000 + 25 * f - 3;
    ps->pm_type        = 0;
    ps->pm_flags       = 0x40 | (c ? 0x2 : 0);
    ps->origin[0]      = 100.0f * (float)(c + 1) + (float)f;
    ps->origin[1]      = -50.5f;
    ps->origin[2]      = 24.0f;
    ps->velocity[0]    = 320.25f;
    ps->viewangles[1]  = 90.0f + (float)f;
    ps->viewheight     = 22;
    ps->gravity        = 800;
    ps->speed          = 320;
    ps->clientNum      = c;
    ps->weapon         = 7 - c;
    ps->weaponstate    = (f & 1) ? 3 : 0;
    ps->eFlags         = 0x400 + c;
    ps->legsAnim       = 11;
    ps->torsoAnim      = 13;
    ps->groundEntityNum = 1022;
    ps->events[0]      = 10 + f;
    ps->eventSequence  = 100 + f;
    for (int i = 0; i < 16; i++) {
        ps->ammo[i]       = (i * 7 + f + c) | 1; // odd
        ps->persistant[i] = i * 3 + c - 4;       // runs through negatives
        ps->powerups[i]   = 0;
    }
    ps->stats[0]    = -5 - f;        // health below zero
    ps->stats[1]    = 0x3c;          // weapons mask
    ps->stats[2]    = 1234;
    ps->stats[3]    = 100 + c;
    ps->stats[9]    = -77;
    ps->stats[15]   = 32767;
    ps->powerups[1] = 123457 + f;    // a timer: absolute level time, past 2^16
    ps->powerups[15] = 70001 + c;
    ps->powerups[8] = -2;
    ps->jumpTime    = 5000 + f;
    ps->fov         = 100;
    ps->location    = 3;
    ps->forwardmove = -127;
}

static void player_entity(int c, int f) {
    link_entity(c, ET_PLAYER, gclients[c].ps.origin[0], gclients[c].ps.origin[1], gclients[c].ps.origin[2]);
    gents[c].s.clientNum = c;
    gents[c].s.weapon    = gclients[c].ps.weapon;
    gents[c].s.pos.gravity = 800.0f; // float in the engine, its bits on the wire
    gents[c].s.legsAnim  = 11 + f;
    gents[c].client      = &gclients[c];
}

static void expect_present(expect_t *e, int n) { e->present[e->npresent++] = n; }
static void expect_absent(expect_t *e, int n) { e->absent[e->nabsent++] = n; }

static void expect_channel(expect_t *e, int c) {
    e->chan_clients[e->nchan] = c;
    e->chan_ps[e->nchan]      = gclients[c].ps;
    e->nchan++;
}

// Cap test: entities placed in frame 3 only.
#define CAP_FIRST 300
#define CAP_COUNT 260

static void setup_static_world(void) {
    g_entities = gents;
    level      = &level_store;

    sv_maxclients = cvar_set("sv_maxclients", "4");
    cvar_set("sv_demoRecord", "1");
    cvar_set("sv_demoMultiPov", "1");
    cvar_set("sv_demoDir", "mp");
    cvar_set("sv_demoNameFormat", "%date_slot%slot_%name");
    cvar_set("sv_demoCleanupParts", "0");

    svs->clients           = clients_store;
    svs->snapFlagServerBit = 4;
    svs->time              = 1000;

    sv->state         = SS_GAME;
    sv->serverId      = 777;
    sv->checksumFeed  = 12345;
    sv->gentities     = (sharedEntity_t *)gents;
    sv->gentitySize   = (int)sizeof(gentity_t);
    sv->gameClients   = &gclients[0].ps;
    sv->gameClientSize = (int)sizeof(gclient_t);
    sv->num_entities  = MAX_GENTITIES - 1;

    level->clients      = gclients;
    level->maxclients   = NCLIENTS;
    level->num_entities = MAX_GENTITIES - 1;

    snprintf(configstring_store[0], sizeof(configstring_store[0]), "%s",
             "\\mapname\\testmap\\g_gametype\\1\\sv_hostname\\harness");
    snprintf(configstring_store[1], sizeof(configstring_store[1]), "%s", "n\\Alpha\\t\\0");
    snprintf(configstring_store[2], sizeof(configstring_store[2]), "%s", "n\\Bravo\\t\\0");
    snprintf(configstring_store[3], sizeof(configstring_store[3]), "%s", "\\sv_serverid\\777");
    for (int i = 0; i < MAX_CONFIGSTRINGS; i++) {
        sv->configstrings[i] = NULL;
    }
    sv->configstrings[CS_SERVERINFO]   = configstring_store[0];
    sv->configstrings[CS_SYSTEMINFO]   = configstring_store[3];
    sv->configstrings[CS_PLAYERS + 0]  = configstring_store[1];
    sv->configstrings[CS_PLAYERS + 1]  = configstring_store[2];

    // Two baselines: the item's and the mover's.
    sv->svEntities[70].baseline.number      = 70;
    sv->svEntities[70].baseline.eType       = ET_ITEM;
    sv->svEntities[70].baseline.modelindex  = 5;
    sv->svEntities[70].baseline.pos.trBase[0] = 640.0f;
    sv->svEntities[71].baseline.number      = 71;
    sv->svEntities[71].baseline.eType       = ET_MOVER;

    for (int c = 0; c < 2; c++) {
        clients_store[c].state = CS_ACTIVE;
        clients_store[c].netchan.outgoingSequence = 50;
        clients_store[c].frames[49 & PACKET_MASK].areabytes = 2;
        snprintf(clients_store[c].name, sizeof(clients_store[c].name), "%s", c ? "Bravo" : "Alpha");
    }

    // Free spectator anchor.
    gents[200].inuse        = qtrue;
    gents[200].classname    = "info_player_intermission";
    gents[200].s.origin[0]  = 100.0f;
    gents[200].s.origin[1]  = 200.0f;
    gents[200].s.origin[2]  = 300.0f;
    gents[200].s.angles[1]  = 90.0f;
}

// Builds frame f's world and its expectations.
static void setup_frame(int f) {
    expect_t *e = &expect[f];
    memset(e, 0, sizeof(*e));
    e->entity_count = -1;
    e->time         = 1000 + 25 * f;
    svs->time       = e->time;

    // Who plays: 0 and 1 except 0 sits out frame 5, both sit out frames 8, 1 comes back at 9,
    // 0 at 10.
    int in0 = !(f == 5 || f == 8 || f == 9);
    int in1 = !(f == 8);
    set_in_game(0, in0);
    set_in_game(1, in1);
    fill_ps(0, f);
    fill_ps(1, f);

    for (int n = 0; n < MAX_GENTITIES; n++) {
        if (n != 200) {
            unlink_entity(n);
        }
    }
    if (in0) {
        player_entity(0, f);
    }
    if (in1) {
        player_entity(1, f);
    }

    // Follow: 0 while he plays; from frame 5 player 1 and it stays with him when 0 comes back
    // (frame 6, 7); nobody at 8; 1 again at 9.
    int follow = -1;
    if (f <= 4) {
        follow = 0;
    } else if (f != 8) {
        follow = 1;
    }
    e->follow    = follow >= 0 ? follow : NCLIENTS;
    e->spectator = follow < 0;
    e->full      = (f == 0);

    for (int c = 0; c < 2; c++) {
        int in = c ? in1 : in0;
        if (!in) {
            expect_absent(e, c);
        } else if (c == follow) {
            expect_absent(e, c); // the followed player's own entity is never sent
        } else {
            expect_present(e, c);
        }
    }

    // The map: an item with a baseline, a mover, an item hidden with SVF_NOCLIENT.
    link_entity(70, ET_ITEM, 640.0f, 0.0f, 0.0f);
    gents[70].s.modelindex = 5;
    gents[70].s.time       = f; // moves the delta along
    link_entity(71, ET_MOVER, 0.0f, 0.0f, 64.0f);
    link_entity(72, ET_ITEM, 10.0f, 10.0f, 10.0f);
    gents[72].r.svFlags = SVF_NOCLIENT;
    expect_present(e, 70);
    expect_present(e, 71);
    expect_absent(e, 72);

    // Entities aimed at single clients: 80 only for 0, 81 only for 1.
    link_entity(80, ET_GENERAL, 0, 0, 0);
    gents[80].r.svFlags     = SVF_SINGLECLIENT;
    gents[80].r.singleClient = 0;
    link_entity(81, ET_GENERAL, 0, 0, 0);
    gents[81].r.svFlags     = SVF_SINGLECLIENT;
    gents[81].r.singleClient = 1;
    if (follow == 0) {
        expect_present(e, 80);
        expect_absent(e, 81);
    } else if (follow == 1) {
        expect_absent(e, 80);
        expect_present(e, 81);
    } else {
        expect_absent(e, 80);
        expect_absent(e, 81);
    }

    // Player events: an ET_EVENTS entity per player, SVF_NOTSINGLECLIENT on its owner as
    // G_AddEvent/G_TempEntity do for player events. The followed player's own is left out.
    for (int c = 0; c < 2; c++) {
        int n = 90 + c;
        link_entity(n, ET_EVENTS + 61, 1.0f, 2.0f, 3.0f);
        gents[n].s.otherEntityNum = c;
        gents[n].s.eventParm      = 7 + f;
        gents[n].r.svFlags        = SVF_NOTSINGLECLIENT;
        gents[n].r.singleClient   = c;
        if (c == follow) {
            expect_absent(e, n);
        } else {
            expect_present(e, n);
        }
    }

    // A missile, never dropped.
    link_entity(95, ET_MISSILE, 5000.0f, 5000.0f, 0.0f);
    expect_present(e, 95);

    // Frame 3: 260 items more, so the snapshot is over 256 and the farthest items go.
    if (f == 3) {
        for (int i = 0; i < CAP_COUNT; i++) {
            link_entity(CAP_FIRST + i, ET_ITEM, 1000.0f + 10.0f * (float)i, 0.0f, 0.0f);
        }
        e->entity_count = 256;
    }
    // Frame 4: a real entity on client 0's channel page 0 number; his channel is left out.
    if (f == 4) {
        link_entity(1021, ET_GENERAL, 0, 0, 0);
        expect_present(e, 1021);
    }

    // Channel: every in-game client, both pages.
    for (int c = 0; c < 2; c++) {
        int in = c ? in1 : in0;
        if (in && !(f == 4 && c == 0)) {
            expect_channel(e, c);
            expect_present(e, 1021 - 2 * c);
            expect_present(e, 1020 - 2 * c);
        } else if (!(f == 4 && c == 0)) {
            expect_absent(e, 1021 - 2 * c);
            expect_absent(e, 1020 - 2 * c);
        }
    }
    if (f == 4) {
        expect_absent(e, 1020); // page 1 of client 0 goes with page 0
    }
    for (int c = 0; c < NCLIENTS; c++) {
        e->ps_of[c] = gclients[c].ps;
    }
}

// ---------------------------------------------------------------------------
// reading it back
// ---------------------------------------------------------------------------

static int failures;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            failures++;                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

typedef struct {
    int snapshots;
    int gamestates;
    int commands;
    int frame;
} read_state_t;

static float chan_float(const dc_entityState_t *es, int i) {
    const float *groups[8] = {es->pos.trBase, es->pos.trDelta, es->apos.trBase, es->apos.trDelta,
                              es->origin,     es->origin2,     es->angles,      es->angles2};
    return groups[i / 3][i % 3];
}

static int chan_int(const dc_entityState_t *es, int i) {
    const int v[9] = {es->pos.trTime, es->pos.trDuration, es->pos.gravity, es->apos.trTime, es->apos.trDuration,
                      es->apos.gravity, es->time, es->time2, es->jumpTime};
    return v[i];
}

static void check_channel(int frame, const dc_entityState_t *p0, const dc_entityState_t *p1, const playerState_t *ps) {
    int c = ps->clientNum;
    for (int i = 0; i < 16; i++) {
        CHECK(chan_float(p0, i) == (float)ps->ammo[i], "frame %d client %d ammo[%d] %g != %d", frame, c, i,
              (double)chan_float(p0, i), ps->ammo[i]);
        CHECK(chan_float(p1, 8 + i) == (float)ps->persistant[i], "frame %d client %d persistant[%d]", frame, c, i);
    }
    for (int i = 0; i < 8; i++) {
        CHECK(chan_float(p0, 16 + i) == (float)ps->stats[i], "frame %d client %d stats[%d] %g != %d", frame, c, i,
              (double)chan_float(p0, 16 + i), ps->stats[i]);
        CHECK(chan_float(p1, i) == (float)ps->stats[8 + i], "frame %d client %d stats[%d]", frame, c, 8 + i);
    }
    for (int i = 0; i < 9; i++) {
        CHECK(chan_int(p0, i) == ps->powerups[i], "frame %d client %d powerups[%d] %d != %d", frame, c, i,
              chan_int(p0, i), ps->powerups[i]);
    }
    for (int i = 0; i < 7; i++) {
        CHECK(chan_int(p1, i) == ps->powerups[9 + i], "frame %d client %d powerups[%d]", frame, c, 9 + i);
    }
    CHECK(chan_int(p1, 7) == ps->pm_flags, "frame %d client %d pm_flags", frame, c);
    CHECK(chan_int(p1, 8) == ps->eFlags, "frame %d client %d eFlags", frame, c);
    CHECK(p0->weapon == ps->weapon && p0->legsAnim == ps->weaponstate && p1->weapon == ps->pm_type,
          "frame %d client %d weapon/weaponstate/pm_type", frame, c);
    for (int k = 0; k < 2; k++) {
        const dc_entityState_t *p = k ? p1 : p0;
        CHECK(p->eType == ET_INVISIBLE && p->modelindex2 == 0xFB && p->frame == 1 && p->clientNum == c &&
                  p->generic1 == k && p->event == 0 && p->eFlags == 0 && p->solid == 0 && p->modelindex == 0,
              "frame %d client %d page %d markers", frame, c, k);
    }
}

static void on_gamestate(dc_parser_t *p, void *user) {
    read_state_t *rs = (read_state_t *)user;
    rs->gamestates++;
    CHECK(p->inClientNum == NCLIENTS, "gamestate clientNum %d, want the recorder slot %d", p->inClientNum, NCLIENTS);
    CHECK(p->inChecksumFeed == 12345, "checksumFeed %d", p->inChecksumFeed);
    const char *row = p->configStrings[CS_PLAYERS + NCLIENTS];
    CHECK(row && !strncmp(row, "n\\multipov\\t\\3\\", 15), "recorder row \"%s\"", row ? row : "(none)");
    CHECK(p->configStrings[CS_PLAYERS] && !strcmp(p->configStrings[CS_PLAYERS], "n\\Alpha\\t\\0"), "player 0 row");
    CHECK(p->configStrings[CS_SERVERINFO] != NULL, "serverinfo");
    CHECK(p->baselines[70].number == 70 && p->baselines[70].eType == ET_ITEM && p->baselines[70].modelindex == 5 &&
              p->baselines[70].pos.trBase[0] == 640.0f,
          "baseline 70");
    CHECK(p->baselines[71].number == 71 && p->baselines[71].eType == ET_MOVER, "baseline 71");
}

static void on_command(dc_parser_t *p, int seq, const char *text, int cs_index, const char *cs_value, void *user) {
    (void)p;
    (void)cs_index;
    (void)cs_value;
    read_state_t *rs = (read_state_t *)user;
    int i            = rs->commands++;
    if (i >= nexpect_cmds) {
        CHECK(0, "unexpected command %d \"%s\"", seq, text);
        return;
    }
    CHECK(seq == expect_cmds[i].seq && !strcmp(text, expect_cmds[i].text), "command %d: got %d \"%s\", want %d \"%s\"",
          i, seq, text, expect_cmds[i].seq, expect_cmds[i].text);
}

static void on_snapshot(dc_parser_t *p, const dc_snapshot_t *snap, void *user) {
    read_state_t *rs = (read_state_t *)user;
    int f            = rs->snapshots++;
    if (f >= NFRAMES) {
        CHECK(0, "more snapshots than frames");
        return;
    }
    const expect_t *e = &expect[f];
    CHECK(snap->serverTime == e->time, "frame %d serverTime %d, want %d", f, snap->serverTime, e->time);
    CHECK((snap->deltaWireNum == 0) == (e->full != 0) && (e->full || snap->deltaWireNum == 1),
          "frame %d deltaNum %d", f, snap->deltaWireNum);
    CHECK(snap->snapFlags == 4, "frame %d snapFlags %d", f, snap->snapFlags);
    CHECK(snap->areamaskLen == 2 && snap->areamask[0] == 0 && snap->areamask[1] == 0, "frame %d areamask len %d", f,
          snap->areamaskLen);
    CHECK(snap->ps.clientNum == e->follow, "frame %d ps.clientNum %d, want %d", f, snap->ps.clientNum, e->follow);
    CHECK(snap->ps.pm_type == 2, "frame %d pm_type %d", f, snap->ps.pm_type);
    if (e->spectator) {
        CHECK(!(snap->ps.pm_flags & 4096) && snap->ps.persistant[3] == 3 && snap->ps.viewheight == 26 &&
                  snap->ps.origin[0] == 100.0f && snap->ps.origin[2] == 300.0f && snap->ps.viewangles[1] == 90.0f,
              "frame %d free spectator playerState", f);
    } else {
        const playerState_t *src = &e->ps_of[e->follow];
        CHECK(snap->ps.pm_flags == (src->pm_flags | 4096), "frame %d pm_flags %#x", f, snap->ps.pm_flags);
        CHECK(snap->ps.origin[0] == src->origin[0] && snap->ps.origin[1] == src->origin[1] &&
                  snap->ps.velocity[0] == src->velocity[0] && snap->ps.viewangles[1] == src->viewangles[1],
              "frame %d follow origin/velocity/view", f);
        CHECK(snap->ps.weapon == src->weapon && snap->ps.ammo[3] == src->ammo[3] &&
                  snap->ps.stats[0] == src->stats[0] && snap->ps.powerups[1] == src->powerups[1] &&
                  snap->ps.persistant[0] == src->persistant[0] &&
                  // 8 unsigned bits on the wire, as the engine sends the char
                  snap->ps.forwardmove == (src->forwardmove & 0xff) &&
                  snap->ps.jumpTime == src->jumpTime && snap->ps.events[0] == src->events[0],
              "frame %d follow ps fields", f);
    }

    // Entities.
    static uint8_t seen[MAX_GENTITIES];
    const dc_entityState_t *byNum[MAX_GENTITIES];
    memset(seen, 0, sizeof(seen));
    memset(byNum, 0, sizeof(byNum));
    int last = -1;
    for (int i = 0; i < snap->numEntities; i++) {
        const dc_entityState_t *es = &p->parseEntities[(snap->parseEntitiesNum + i) & (DC_MAX_PARSE_ENTITIES - 1)];
        CHECK(es->number > last, "frame %d entities out of order", f);
        last             = es->number;
        seen[es->number] = 1;
        byNum[es->number] = es;
    }
    for (int i = 0; i < e->npresent; i++) {
        CHECK(seen[e->present[i]], "frame %d entity %d missing", f, e->present[i]);
    }
    for (int i = 0; i < e->nabsent; i++) {
        CHECK(!seen[e->absent[i]], "frame %d entity %d should not be there", f, e->absent[i]);
    }
    if (e->entity_count >= 0) {
        CHECK(snap->numEntities == e->entity_count, "frame %d %d entities, want %d", f, snap->numEntities,
              e->entity_count);
        // The nearest of the cap items stay, the farthest go.
        int kept = 0;
        for (int i = 0; i < CAP_COUNT; i++) {
            kept += seen[CAP_FIRST + i];
        }
        for (int i = 0; i < kept; i++) {
            CHECK(seen[CAP_FIRST + i], "frame %d near item %d dropped", f, CAP_FIRST + i);
        }
        printf("  frame %d: %d entities, %d of %d cap items kept\n", f, snap->numEntities, kept, CAP_COUNT);
    }
    if (byNum[0] && f == 6) {
        CHECK(byNum[0]->pos.gravity != 0 && byNum[0]->clientNum == 0, "frame %d player 0 entity fields", f);
    }
    for (int k = 0; k < e->nchan; k++) {
        int c = e->chan_clients[k];
        const dc_entityState_t *p0 = byNum[1021 - 2 * c];
        const dc_entityState_t *p1 = byNum[1020 - 2 * c];
        if (p0 && p1) {
            check_channel(f, p0, p1, &e->chan_ps[k]);
        }
    }
    // reliableAcknowledge = the last command sequence written so far.
    CHECK(p->inReliableSequenceAcknowledge == p->inServerCommandSequence || p->inServerCommandSequence < 0,
          "frame %d reliableAcknowledge %d, last command %d", f, p->inReliableSequenceAcknowledge,
          p->inServerCommandSequence);
}

static void write_expected_jsonl(const char *path) {
    FILE *fh = fopen(path, "w");
    if (!fh) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    for (int f = 0; f < NFRAMES; f++) {
        const expect_t *e = &expect[f];
        fprintf(fh, "{\"time\":%d,\"follow\":%d,\"spectator\":%d,\"full\":%d,\"present\":[", e->time, e->follow,
                e->spectator, e->full);
        for (int i = 0; i < e->npresent; i++) {
            fprintf(fh, "%s%d", i ? "," : "", e->present[i]);
        }
        fprintf(fh, "],\"absent\":[");
        for (int i = 0; i < e->nabsent; i++) {
            fprintf(fh, "%s%d", i ? "," : "", e->absent[i]);
        }
        fprintf(fh, "],\"entity_count\":%d,\"channel\":{", e->entity_count);
        for (int k = 0; k < e->nchan; k++) {
            const playerState_t *ps = &e->chan_ps[k];
            fprintf(fh, "%s\"%d\":{\"ammo\":[", k ? "," : "", e->chan_clients[k]);
            for (int i = 0; i < 16; i++) fprintf(fh, "%s%d", i ? "," : "", ps->ammo[i]);
            fprintf(fh, "],\"stats\":[");
            for (int i = 0; i < 16; i++) fprintf(fh, "%s%d", i ? "," : "", ps->stats[i]);
            fprintf(fh, "],\"persistant\":[");
            for (int i = 0; i < 16; i++) fprintf(fh, "%s%d", i ? "," : "", ps->persistant[i]);
            fprintf(fh, "],\"powerups\":[");
            for (int i = 0; i < 16; i++) fprintf(fh, "%s%d", i ? "," : "", ps->powerups[i]);
            fprintf(fh, "],\"weapon\":%d,\"weaponstate\":%d,\"pm_type\":%d,\"pm_flags\":%d,\"eFlags\":%d}", ps->weapon,
                    ps->weaponstate, ps->pm_type, ps->pm_flags, ps->eFlags);
        }
        fprintf(fh, "}}\n");
    }
    fclose(fh);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <home_dir>\n", argv[0]);
        return 2;
    }
    cvar_set("fs_homepath", argv[1]);
    setup_static_world();
    Demo_Init();

    char path[600] = "";
    for (int f = 0; f < NFRAMES; f++) {
        setup_frame(f);

        // Server commands since the last frame: a broadcast, one to the followed player, one to
        // the other player. Only the first two belong in the recording. Before frame 0 nothing
        // is recording yet, so nothing is kept.
        int follow = expect[f].spectator ? -1 : expect[f].follow;
        if (f == 1 || f == 6) {
            char text[256];
            snprintf(text, sizeof(text), "print \"broadcast %d\n\"", f);
            MultiPov_ServerCommand(-1, text);
            snprintf(expect_cmds[nexpect_cmds].text, sizeof(expect_cmds[0].text), "%s", text);
            nexpect_cmds++;
            snprintf(text, sizeof(text), "cs %d \"n\\\\Alpha\\\\t\\\\0\\\\f%d\"", CS_PLAYERS, f);
            // The builder takes the followed player of the frame before as the source.
            int prev_follow = expect[f - 1].spectator ? -1 : expect[f - 1].follow;
            MultiPov_ServerCommand(prev_follow, text);
            snprintf(expect_cmds[nexpect_cmds].text, sizeof(expect_cmds[0].text), "%s", text);
            nexpect_cmds++;
            MultiPov_ServerCommand(prev_follow == 0 ? 1 : 0, "print \"not for the camera\n\"");
        }
        (void)follow;

        MultiPov_Frame();
        if (f == 2) {
            MultiPov_Frame(); // a catch-up G_RunFrame at the same server time: no second frame
        }
        if (f == 0) {
            const char *p = Demo_GetPath(DEMO_MULTIPOV_SLOT);
            CHECK(p != NULL, "the multi-POV slot is not recording after frame 0");
            if (p) {
                snprintf(path, sizeof(path), "%s", p);
            }
        }
    }
    for (int i = 0; i < nexpect_cmds; i++) {
        expect_cmds[i].seq = i + 1; // the gamestate carries 0
    }

    Demo_DrainFinalise();
    demo_finished_t done;
    int finished = 0;
    for (int tries = 0; tries < 200 && !finished; tries++) {
        while (Demo_PollFinished(&done)) {
            if (done.slot == DEMO_MULTIPOV_SLOT && !done.snapshot) {
                finished = 1;
                CHECK(!done.failed && !done.discarded, "writer reported failed=%d discarded=%d", done.failed,
                      done.discarded);
                CHECK(!strcmp(done.path, path), "finished %s, opened %s", done.path, path);
            }
        }
        if (!finished) {
            usleep(10000);
        }
    }
    CHECK(finished, "no completion for the multi-POV slot");

    // Read it back.
    FILE *fh = fopen(path, "rb");
    CHECK(fh != NULL, "cannot open %s", path);
    if (!fh) {
        return 1;
    }
    dc_parser_t *p = dc_parser_create();
    read_state_t rs;
    memset(&rs, 0, sizeof(rs));
    p->cb.user         = &rs;
    p->cb.on_gamestate = on_gamestate;
    p->cb.on_command   = on_command;
    p->cb.on_snapshot  = on_snapshot;
    static dc_byte buf[DC_MAX_MSGLEN];
    int blocks = 0, last_seq = 0, eof = 0;
    for (;;) {
        int32_t hdr[2];
        if (fread(hdr, sizeof(hdr), 1, fh) != 1) {
            break;
        }
        if (hdr[0] == -1 && hdr[1] == -1) {
            eof = 1;
            break;
        }
        CHECK(hdr[1] > 0 && hdr[1] <= DC_MAX_MSGLEN, "block length %d", hdr[1]);
        if (hdr[1] <= 0 || hdr[1] > DC_MAX_MSGLEN || fread(buf, (size_t)hdr[1], 1, fh) != 1) {
            break;
        }
        CHECK(blocks == 0 || hdr[0] == last_seq + 1, "block seq %d after %d", hdr[0], last_seq);
        last_seq = hdr[0];
        blocks++;
        int rc = dc_parser_parse_message(p, buf, hdr[1], hdr[0]);
        CHECK(rc == 1, "parse of block %d (seq %d) returned %d: %s", blocks, hdr[0], rc, p->err);
        if (rc != 1) {
            break;
        }
    }
    fclose(fh);
    dc_parser_destroy(p);

    CHECK(eof, "no end marker");
    CHECK(rs.gamestates == 1, "%d gamestates", rs.gamestates);
    CHECK(rs.snapshots == NFRAMES, "%d snapshots, want %d", rs.snapshots, NFRAMES);
    CHECK(rs.commands == nexpect_cmds, "%d commands, want %d", rs.commands, nexpect_cmds);
    CHECK(blocks == NFRAMES + 1, "%d blocks, want %d", blocks, NFRAMES + 1);

    char jsonl[700];
    snprintf(jsonl, sizeof(jsonl), "%s/mp/expected.jsonl", argv[1]);
    write_expected_jsonl(jsonl);

    printf("file: %s\nexpected: %s\n%d blocks, %d snapshots, %d commands, %d failure(s)\n", path, jsonl, blocks,
           rs.snapshots, rs.commands, failures);
    return failures ? 1 : 0;
}
