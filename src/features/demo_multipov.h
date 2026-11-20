// demo_multipov.h - one multi-POV recording per server (sv_demoMultiPov).

#ifndef DEMO_MULTIPOV_H
#define DEMO_MULTIPOV_H

#include <stdint.h>

/*
 * One multi-POV recording per server (sv_demoMultiPov 1, with sv_demoRecord for the file and
 * sv_demoStream for the live stream). Instead of tapping every client's outgoing messages, the
 * server builds one .dm_91 of its own from its world state, once per server frame:
 *
 *   - the gamestate's clientNum is a recorder slot no player has (sv_maxclients), with its own
 *     CS_PLAYERS row as a spectator;
 *   - every entity a client could be sent, with no PVS test;
 *   - the playerState of the followed player (the lowest slot in the game) as a follow spectator
 *     sees it, or a free spectator while nobody plays;
 *   - every in-game player's playerState in the player data channel v1 (two ET_INVISIBLE
 *     entities per player, the layout of ql-tournament's player-channel.js);
 *   - the server commands that go to everyone and to the followed player.
 *
 * The file is what ql-tournament's offline merge (tools/demo-merge) writes from N POV captures.
 * It is written through demos.c's virtual slot DEMO_MULTIPOV_SLOT and streamed through
 * stream.c's STREAM_MULTIPOV_SLOT. Game thread only, all of it.
 */

// Once per server frame, after G_RunFrame (and the game event poll), in both builds.
void MultiPov_Frame(void);

// A server command as it goes out. slot is the client it is for, -1 for every client. Called
// after the server_command dispatch, from My_SV_SendServerCommand and from the paced path in
// reliable.c, so in Python builds only.
void MultiPov_ServerCommand(int slot, const char *text);

// The block sequence number the recording's next message will carry: the multi-POV slot's
// counterpart of a client's netchan.outgoingSequence, for demo_match.c's arm_seq.
int32_t MultiPov_NextSeq(void);

#endif /* DEMO_MULTIPOV_H */
