/**
 * @file
 * @brief Multiplayer party prototype (hotseat; LAN later).
 *
 * The game is written around a single global `player you`. Rather than
 * rewrite ~15k references, a party keeps N player structs and swaps the one
 * that should be acting into `you`:
 *
 *  - the *active* player is whoever's turn it is (takes input);
 *  - the *focused* player is whoever is currently in `you`. Usually the same
 *    as active, but during the monster phase each monster gets the player it
 *    is targeting swapped in before it acts.
 *
 * Turn structure: each player spends time on their own actions. Once every
 * player has spent at least PARTY_ROUND_AUT in the current round, monsters
 * get one PARTY_ROUND_AUT round. Time beyond that carries over, so a slow
 * action eats into the next round and a hasted player gets several actions.
 *
 * With a party of one, none of this is active and the game is unchanged.
 */

#pragma once

#include <string>

#include "coord-def.h"

class monster;
class player;

const int PARTY_ROUND_AUT = 10;
const int PARTY_MAX_SIZE = 4;

// Grow the party to `size` players (clones of the current player), placed
// near them. Used at game start when the party_size option is > 1.
void party_start(int size);

// Called by the main loop wherever a solo game would call world_reacts()
// after the player acts: does the acting player's per-action upkeep and
// passes the turn on (running the monster phase when the round is over).
// Uses you.time_taken.
void party_action_done();

// "--- Player 2's turn ---", plus a full redraw for the new player.
void party_announce_turn();

string party_member_name(int idx);

// Index of a player other than the focused one standing at `pos`, or -1.
int party_member_at(const coord_def &pos);

// After `you` arrives on a new level, bring every other player along and
// place them nearby. Covers stairs, shafts, portals and banishment alike.
void party_follow_to_current_level();

// The focused player is stepping onto `pos`. If a party member is there,
// move them to the focused player's square and return true.
bool party_swap_places_with_member_at(const coord_def &pos);

// True when there is more than one player.
bool party_active();
int party_size();

// 0-based indices.
int party_active_index();
int party_focused_index();

// Add a new player, cloned from the active one, standing at `pos`.
// Returns its index.
int party_add_player(const coord_def &pos);

// Dissolve the party; the player currently in `you` carries on alone.
void party_reset();

// Make player `idx` the active player (and swap them into `you`).
void party_switch_to(int idx);

// The active player spends `aut` time. Handles their per-action upkeep,
// hands control to the next player when their round is used up, and runs
// the monster phase when everyone's is. With a party of one this is just a
// normal turn.
void party_spend(int aut);

// Read-only access to any player without swapping (the focused one is `you`).
const player &party_member(int idx);

// Is there a player other than the focused one standing at `pos`?
bool party_unfocused_player_at(const coord_def &pos);

// Monster phase hooks (called from handle_monsters).
void party_focus_for_monster(const monster &mons);
void party_restore_focus();

// True while the party's monster phase is running world_reacts(); per-player
// upkeep is skipped there because it already ran per action.
bool party_in_monster_phase();
