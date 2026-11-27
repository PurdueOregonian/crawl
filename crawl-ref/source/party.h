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

#include <functional>
#include <string>

#include "coord-def.h"
#include "god-type.h"
#include "skill-type.h"
#include "spell-type.h"

#include <vector>

enum class passive_t;
struct item_def;

class monster;
struct newgame_def;
class player;

const int PARTY_ROUND_AUT = 10;
const int PARTY_MAX_SIZE = 4;

// Doing nothing on a fallen player's body for this long (50 turns) revives
// them.
const int PARTY_REVIVE_AUT = 500;

// The player in `you` has just died. If anyone else in the party is still
// standing, they fall instead (pending_revival, like a Felid between lives;
// they get no turns and monsters ignore them) and this returns true. False
// means they were the last one, so the game really ends.
bool party_member_falls();

// The current action was "doing nothing" (wait, rest, or an automatic wait
// during a group activity): if done on a fallen player's body, it counts
// towards reviving them.
void party_note_idle_action();

#ifdef USE_TILE
#include "rltiles/tiledef_defines.h"
// Tile for drawing party member `idx` (not the one in `you`) as themselves:
// their own doll, see-through if fallen. 0 if unknown.
tileidx_t party_doll_tile(int idx);
#endif

// Fallen player (not the one in `you`) whose body is at `pos`, or -1.
int party_corpse_at(const coord_def &pos);

// Is the player in `you` standing on a fallen teammate's body?
bool party_reviving();

// How long (aut) someone has tended fallen player `idx`'s body so far.
int party_revive_progress(int idx);

// Grow the party to `size` players, placed near the current one. With
// `choose`, each new player picks their own character from the new-game
// menus (backing out gives a copy of the current player); otherwise they're
// all copies. Used at game start when the party_size option is > 1.
void party_start(int size, bool choose = false);

// How many players a new game's party will have: party_size, and at least 2
// when hosting a LAN game.
int party_planned_size();

// LAN play, a new game, before the world is made: each player at another
// computer picks their character there now (party_start() then uses it),
// so everyone is ready before the game starts. Players at this computer
// still pick theirs in party_start().
void party_prechoose_characters(int size);

// Add a player at `pos` playing the character described by `ng` (species,
// job, weapon). World state and level come from the current player.
int party_add_character(const coord_def &pos, const newgame_def &ng);

// Called by the main loop wherever a solo game would call world_reacts()
// after the player acts: does the acting player's per-action upkeep and
// passes the turn on (running the monster phase when the round is over).
// Uses you.time_taken. `manual` means the action was played by hand; any
// other player's autoexplore/travel/rest is then stopped when their turn
// comes, so nobody keeps auto-playing while someone else is at the keyboard.
// If the action was an explore step and nobody leads group explore, this
// player becomes the leader. (Pressing explore only queues it; the first step
// comes from the delay system, so this can't key off the keypress.)
void party_action_done(bool manual = false);

// Group autoexplore. When one player explores, the others follow them
// automatically on their turns until the leader stops (item, monster, done),
// a follower spots danger or can't keep up, or someone plays by hand.
//
// Called by the main loop before asking the active player for a command:
// if they're following, takes their step (or wait) and returns true with
// you.turn_is_over set. Returns false when they should get the keyboard.
bool party_group_explore_turn();

// Called on the leader's turn before their explore takes a step: true if a
// follower is too far behind, in which case the leader should just wait.
bool party_leader_waits_for_group();

// Followers further than this from the leader make the leader wait.
const int PARTY_EXPLORE_MAX_GAP = 2;

// Index of the player leading group explore / group rest, or -1.
int party_explore_leader();
int party_rest_leader();

// "Player 2 can see a goblin" for the first player who can see an enemy, or
// "" if nobody in the party can.
string party_danger_seen();

// Does any player other than the one in `you` still need to rest?
bool party_others_need_rest();

// Called on the active player's turn before their auto-activity (explore,
// rest) takes a step. Stops it if anyone in the party can see an enemy, or
// if a group rest has finished (everyone rested). Returns true if stopped.
bool party_check_auto_activity();

// Should travel/explore pathing treat `c` as blocked by a party member?
bool party_blocks_travel(const coord_def &c);

// May the player in `you`, while exploring or travelling, swap places with a
// teammate in their way? Yes for the group's leader (or a would-be one).
bool party_may_swap_while_travelling();

// Put an item into the party's shared pack (the inventory of the player in
// `you`): stacking it, keeping its letter if free, else on the floor.
void party_add_to_pack(const item_def &item);

// "--- Player 2's turn ---", plus a full redraw for the new player.
void party_announce_turn();
// Announce the active player unless they were the last one announced. Called
// when a player is about to be prompted for a command.
void party_announce_if_new();

string party_member_name(int idx);

// A skill manual picked up by the player in `you` teaches every other living
// party member who can still learn that skill, as if they'd picked it up too.
void party_share_manual(skill_type sk, int points);

// Experience gained by the player in `you` (a kill, an XP potion, ...):
// every other living party member gains the same amount.
void party_share_exp(unsigned int exp);

// A spellbook/parchment picked up by the player in `you`: every other living
// party member who has a spell library (not innate casters such as djinn)
// adds its spells to theirs.
void party_share_book(const vector<spell_type> &spells);

// Name of a living party member who isn't next to the player in `you`, or ""
// if everyone is. Taking stairs needs the whole party adjacent.
string party_member_not_adjacent();

// Index of a player other than the focused one standing at `pos`, or -1.
int party_member_at(const coord_def &pos);

// After `you` arrives on a new level, bring every other player along and
// place them nearby. Covers stairs, shafts, portals and banishment alike.
void party_follow_to_current_level();

// Something (the player in `you`, or a monster) is about to be put at `pos`.
// If another party member is standing there, move them to the nearest free
// square, so nothing ever ends up inside a teammate (blinks, teleports,
// summons...). Party members are like permanent allies in this respect.
void party_make_room_at(const coord_def &pos);

// Teleportation is shared by the party. party_share_teleport(): the player in
// `you` just started a delayed teleport; give everyone the same countdown.
// party_cancel_teleport(): it was cancelled; cancel it for everyone.
// party_teleport_along(): the player in `you` just teleported; clear the
// others' countdowns and bring them along to land beside them.
void party_share_teleport();
// Which god (if any) grants `passive` to the player in `you` or to a living
// teammate on this level, e.g. Ashenzari's warning of malevolent traps
// protects the whole party. GOD_NO_GOD if nobody's does.
god_type party_passive_god(passive_t passive);
void party_cancel_teleport();
void party_teleport_along();

// The Abyss moved the area within `radius` of `old_centre` (where the player
// in `you` was) by `delta`. Teammates in it move with it; anyone else is
// brought to the player's side.
void party_abyss_shifted(const coord_def &old_centre, const coord_def &delta,
                         int radius);
// The Abyss's terrain just changed: move teammates off walls or lava.
void party_abyss_morphed();

// Move party member `idx` (not the one in `you`) directly to `pos` (tests).
void party_member_set_pos(int idx, const coord_def &pos);

// The focused player is stepping onto `pos`. If a party member is there,
// move them to the focused player's square and return true.
bool party_swap_places_with_member_at(const coord_def &pos);

// True when there is more than one player.
bool party_active();
int party_size();
// How many players' worth of loot (random floor items, shops) a new level
// gets. Also counts the configured party_size, because the first level is
// made before the party is.
int party_loot_scale();

// 0-based indices.
int party_active_index();
int party_focused_index();

// Run `fn` with party member `idx` in `you` (their view, LOS, status), then
// put back whoever was there. Only between actions: nothing may be holding
// references into `you`.
void party_view_as(int idx, const function<void()> &fn);

// Add a new player, cloned from the active one, standing at `pos`.
// Returns its index.
int party_add_player(const coord_def &pos);

// Dissolve the party; the player currently in `you` carries on alone.
void party_reset();

// ---- Saving and loading the party ----
//
// A save holds the player in `you` as usual (the "you" chunk, with the
// shared pack and the world), a "party" chunk with the party's own state,
// and a TAG_PARTY_MEMBER chunk "partyN" for each other player N (their
// character and worn gear).

class reader;
class writer;

// LAN games: each player who had their own computer gets a copy of the save
// when the host saves and quits (see lan.h). A copy is the host's save plus a
// "pown" chunk saying whose copy it is (its *owner*). Whoever loads a copy
// plays their own player at that computer and hosts; everyone else who had a
// computer joins again, and gets their own player back if they have a copy
// too. Copies of one game share its game id; the save counter says which is
// newest.

// What a save says about its party, without loading the game.
struct party_save_info
{
    int size = 0;                 // 0: not a party game
    int main_player = 0;          // whose is the "you" chunk
    int owner = 0;                // the player at this computer
    string game_id;               // the same for every copy of a game
    int generation = 0;           // how many times it has been saved
    // Players who need to join from another computer when this copy is
    // loaded: everyone who had their own computer, except the owner.
    vector<bool> remote;
    vector<string> describe;      // "Minotaur Fighter, XL 3"
};

// Write the "party" chunk's contents, and call `write_member(idx)` for each
// other player with them in `you` (so TAG_PARTY_MEMBER writes them).
// Nothing to do with a party of one.
void party_save_state(writer &w);
void party_save_members(const function<void(int)> &write_member);
// Saving and quitting: put this computer's own player in `you` (if it's safe
// to swap now), so the save is listed under them; the turn order is
// unchanged.
void party_focus_lead_for_save();

// The player this computer plays when nobody else's turn shows: player 1,
// or the owner of the copy that was loaded.
int party_home_player();
// Saving and quitting a LAN game: call `write()` with player `idx` in `you`
// and as the home player, so what it saves is their copy (listed under
// them, and loaded with them as the main player); the save counter stays
// as it is. False (nothing done) if that can't be done now.
bool party_write_copy_as(int idx, const function<void()> &write);
// This game's id ("" before it was first saved) and save counter.
string party_game_id();
int party_save_generation();

// Read a "party" chunk. Fills `info`; with `keep`, also remembers the
// party's state for party_resume(). `owner`: whose copy this is (from its
// "pown" chunk), or -1 for the host's own save.
void party_read_state(reader &r, party_save_info &info, bool keep,
                      int owner = -1);
// Loading a save: call `read_member()` with a fresh player in `you` to read
// party member `idx`, and keep them for party_resume(). Before the "you"
// chunk is read, so that the main player's world state wins.
void party_load_member(int idx, const function<void()> &read_member);
// Forget any party read from a save (a new game, or a save without one).
void party_forget_saved();
// A party was just loaded with the game: rebuild it around the player in
// `you`. False if the save had no party.
bool party_resume();

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
