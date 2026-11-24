/**
 * @file
 * @brief Multiplayer party prototype. See party.h for the model.
 */

#include "AppHdr.h"

#include "party.h"

#include <climits>
#include <memory>
#include <vector>

#include "cluautil.h"
#include "coord.h"
#include "env.h"
#include "l-libs.h"
#include "losglobal.h"
#include "mapmark.h"
#include "monster.h"
#include "player.h"
#include "player-reacts.h"

// Defined in main.cc; not exposed in a header.
void world_reacts();

namespace
{
    // slots[i] holds player i, EXCEPT slots[focused], which is a stale
    // placeholder while that player lives in the global `you`. Empty means
    // no party (a party of one).
    vector<unique_ptr<player>> slots;
    // Time each player has spent in the current round.
    vector<int> spent;
    int active = 0;
    int focused = 0;
    bool in_monster_phase = false;

    // Put player `idx` into `you`, parking whoever was there in their slot.
    // The save file handle belongs to the game, not to a player, so it stays
    // with the global `you` (and ~player asserts that copies don't own it).
    void swap_in(int idx)
    {
        ASSERT(idx >= 0 && idx < (int)slots.size());
        if (idx == focused)
            return;

        package *save = you.save;
        you.save = nullptr;
        swap(you, *slots[focused]);   // park the focused player
        swap(you, *slots[idx]);       // bring in the new one
        you.save = save;
        focused = idx;
    }

    void activate_pending_markers()
    {
        // A level made by debug.generate_level() was never "entered", so its
        // markers were never activated; world_reacts() asserts they are.
        if (env.markers.need_activate())
            env.markers.activate_all(false);
    }

    void monster_phase()
    {
        in_monster_phase = true;
        you.time_taken = PARTY_ROUND_AUT;
        you.turn_is_over = true;
        world_reacts();
        party_restore_focus();
        in_monster_phase = false;

        // Game time is shared: everyone has lived through the same round.
        for (int i = 0; i < (int)slots.size(); ++i)
            if (i != focused)
                slots[i]->elapsed_time = you.elapsed_time;
    }

    // Hand control to the next player with time left this round, running
    // monster phases as needed. The current active player keeps control if
    // they still have time (e.g. hasted).
    void advance_turn()
    {
        const int n = slots.size();
        while (true)
        {
            for (int k = 0; k < n; ++k)
            {
                const int j = (active + k) % n;
                if (spent[j] < PARTY_ROUND_AUT)
                {
                    active = j;
                    swap_in(j);
                    return;
                }
            }

            // Everyone has used their round.
            monster_phase();
            for (int &s : spent)
                s -= PARTY_ROUND_AUT;
            active = 0;
        }
    }
}

bool party_active()
{
    return slots.size() > 1;
}

int party_size()
{
    return slots.empty() ? 1 : slots.size();
}

int party_active_index()
{
    return active;
}

int party_focused_index()
{
    return focused;
}

const player &party_member(int idx)
{
    ASSERT(idx >= 0 && idx < party_size());
    if (slots.empty() || idx == focused)
        return you;
    return *slots[idx];
}

int party_add_player(const coord_def &pos)
{
    ASSERT(!in_monster_phase);
    if (slots.empty())
    {
        // Becoming a party: slot 0 is the existing player (who is in `you`,
        // so their slot is just a placeholder for now).
        slots.push_back(make_unique<player>(you));
        slots.back()->save = nullptr;
        spent.push_back(0);
        active = focused = 0;
    }

    auto p = make_unique<player>(you);
    p->save = nullptr;
    p->delay_queue.clear();
    p->flash_where = nullptr;
    // Direct assignment, not set_position(): this player isn't the one in
    // `you`, so don't fire "player moved" events or touch LOS.
    p->position = pos;

    slots.push_back(std::move(p));
    spent.push_back(0);
    return slots.size() - 1;
}

void party_reset()
{
    ASSERT(!in_monster_phase);
    slots.clear();
    spent.clear();
    active = focused = 0;
}

void party_switch_to(int idx)
{
    ASSERT(!in_monster_phase);
    ASSERT(idx >= 0 && idx < party_size());
    if (slots.empty())
        return;
    active = idx;
    swap_in(idx);
}

void party_spend(int aut)
{
    ASSERT(aut >= 0);
    ASSERT(!in_monster_phase);
    activate_pending_markers();

    if (!party_active())
    {
        // Party of one: an ordinary turn.
        you.time_taken = aut;
        you.turn_is_over = true;
        world_reacts();
        return;
    }

    ASSERT(focused == active);
    // Per-action upkeep for the acting player (durations, regen, clouds
    // under them...). world_reacts() skips this during the monster phase.
    you.time_taken = aut;
    player_reacts();
    spent[active] += aut;
    advance_turn();
}

bool party_unfocused_player_at(const coord_def &pos)
{
    for (int i = 0; i < (int)slots.size(); ++i)
        if (i != focused && slots[i]->pos() == pos && slots[i]->hp > 0)
            return true;
    return false;
}

void party_focus_for_monster(const monster &mons)
{
    if (!party_active())
        return;

    // Target the nearest living player the monster can see; failing that,
    // the nearest one at all. Ties go to the lower index.
    int best = -1;
    int best_score = INT_MAX;
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        const player &p = party_member(i);
        if (p.hp <= 0)
            continue;
        const bool visible = cell_see_cell(mons.pos(), p.pos(), LOS_DEFAULT);
        const int score = grid_distance(mons.pos(), p.pos())
                          + (visible ? 0 : 10000);
        if (score < best_score)
        {
            best = i;
            best_score = score;
        }
    }
    if (best >= 0)
        swap_in(best);
}

void party_restore_focus()
{
    if (party_active())
        swap_in(active);
}

bool party_in_monster_phase()
{
    return in_monster_phase;
}

// ---------------------------------------------------------------------------
// Lua bindings: the `party` table (dlua). Indices are 1-based in Lua.
// ---------------------------------------------------------------------------

static int _lua_index(lua_State *ls, int arg)
{
    const int idx = luaL_safe_checkint(ls, arg) - 1;
    if (idx < 0 || idx >= party_size())
        luaL_argerror(ls, arg, "no such player");
    return idx;
}

LUAFN(party_lua_size)
{
    PLUARET(integer, party_size());
}

LUAFN(party_lua_active)
{
    PLUARET(integer, party_active_index() + 1);
}

LUAFN(party_lua_add_player)
{
    const coord_def pos(luaL_safe_checkint(ls, 1), luaL_safe_checkint(ls, 2));
    if (!in_bounds(pos))
        luaL_argerror(ls, 1, "position out of bounds");
    PLUARET(integer, party_add_player(pos) + 1);
}

LUAFN(party_lua_switch_to)
{
    party_switch_to(_lua_index(ls, 1));
    return 0;
}

LUAFN(party_lua_spend)
{
    const int aut = luaL_safe_checkint(ls, 1);
    if (aut < 0)
        luaL_argerror(ls, 1, "time must be non-negative");
    party_spend(aut);
    return 0;
}

LUAFN(party_lua_reset)
{
    UNUSED(ls);
    party_reset();
    return 0;
}

LUAFN(party_lua_hp)
{
    const player &p = party_member(_lua_index(ls, 1));
    lua_pushinteger(ls, p.hp);
    lua_pushinteger(ls, p.hp_max);
    return 2;
}

LUAFN(party_lua_pos)
{
    const player &p = party_member(_lua_index(ls, 1));
    lua_pushinteger(ls, p.pos().x);
    lua_pushinteger(ls, p.pos().y);
    return 2;
}

const struct luaL_Reg party_dlib[] =
{
{ "size", party_lua_size },
{ "active", party_lua_active },
{ "add_player", party_lua_add_player },
{ "switch_to", party_lua_switch_to },
{ "spend", party_lua_spend },
{ "reset", party_lua_reset },
{ "hp", party_lua_hp },
{ "pos", party_lua_pos },
{ nullptr, nullptr }
};
