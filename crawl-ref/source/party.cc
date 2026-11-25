/**
 * @file
 * @brief Multiplayer party prototype. See party.h for the model.
 */

#include "AppHdr.h"

#include "party.h"

#include <climits>
#include <memory>
#include <vector>

#include "abyss.h"
#include "act-iter.h"
#include "cluautil.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "env.h"
#include "fineff.h"
#include "initfile.h"
#include "l-libs.h"
#include "level-id.h"
#include "los.h"
#include "losglobal.h"
#include "mapmark.h"
#include "message.h"
#include "monster.h"
#include "newgame.h"
#include "newgame-def.h"
#include "ng-setup.h"
#include "species.h"
#include "jobs.h"
#include "item-prop.h"
#include "movement.h"
#include "nearby-danger.h"
#include "unwind.h"
#include "mon-util.h"
#include "output.h"
#include "player.h"
#include "player-reacts.h"
#include "religion.h"
#include "shopping.h"
#include "skills.h"
#include "spl-util.h"
#include "options.h"
#include "state.h"
#include "stringutil.h"
#include "terrain.h"
#include "transform.h"
#include "travel.h"
#include "viewgeom.h"
#ifdef USE_TILE
#include "tiledoll.h"
#include "tilemcache.h"
#include "tilepick-p.h"
#endif

// Defined in main.cc; not exposed in a header.
void world_reacts();

static bool _find_spot_near(const coord_def &centre, const player &who,
                            coord_def &spot);

namespace
{
    // slots[i] holds player i, EXCEPT slots[focused], which is a stale
    // placeholder while that player lives in the global `you`. Empty means
    // no party (a party of one).
    vector<unique_ptr<player>> slots;
    // Time each player has spent in the current round.
    vector<int> spent;
    // Why this player's auto-activity (explore, travel, rest) should stop
    // when they next get control; empty if it shouldn't.
    vector<string> stop_auto;
    // A message to show this player when they next get the keyboard.
    vector<string> notice;
    // Revival: how long (aut) someone has spent doing nothing on each fallen
    // player's body. A fallen player has pending_revival set (as a Felid
    // between lives) and never gets a turn.
    vector<int> revive_progress;
#ifdef USE_TILE
    // Tiles draws the player in `you` from the global `player_doll` (body,
    // hair... chosen for the character) plus their current equipment. Each
    // party member has their own base doll, swapped in and out of
    // player_doll with them, and a full doll (base + equipment) recorded
    // whenever they're swapped out, for drawing them when it isn't their turn.
    vector<dolls_data> base_dolls;
    vector<dolls_data> dolls;

    // The full doll of the player in `you`.
    dolls_data current_full_doll()
    {
        dolls_data d = player_doll;
        fill_doll_equipment(d);
        return d;
    }
#endif
    // Set when the current action was "doing nothing" (wait/rest, or an
    // automatic wait during a group activity); counts towards revival.
    bool idle_action = false;
    // Group activity (explore or rest): index of the player leading it, or
    // -1. Everyone else follows along automatically on their turns.
    int group_leader = -1;
    enum class group_activity { none, explore, rest };
    group_activity group_kind = group_activity::none;
    // While a follower is pathing to the leader, the leader's square must
    // not count as blocked (party members otherwise block travel).
    coord_def follow_target;
    // Consecutive follower turns spent blocked; the leader waits for them,
    // so without a limit a boxed-in follower would stall the party forever.
    int follow_blocked_turns = 0;
    const int FOLLOW_MAX_BLOCKED_TURNS = 5;
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

#ifdef USE_TILE
        base_dolls[focused] = player_doll;
        dolls[focused] = current_full_doll();
#endif
        package *save = you.save;
        you.save = nullptr;
        swap(you, *slots[focused]);   // park the focused player
        swap(you, *slots[idx]);       // bring in the new one
        you.save = save;
        focused = idx;
#ifdef USE_TILE
        player_doll = base_dolls[idx];
#endif
        // The view geometry (viewport centre, LOS bounds) tracks the player
        // in `you`; drawing and targeting index arrays by it.
        crawl_view.set_player_at(you.pos());
        // The shopping list caches a pointer into you.props, which the swap
        // just replaced; left alone it dangles (crash on learning a spell).
        // This is the only such cache: other code holding pointers into `you`
        // does so within a single action, and swaps only happen between them.
        shopping_list.refresh();
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
                if (spent[j] < PARTY_ROUND_AUT
                    && !party_member(j).pending_revival)
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
        stop_auto.emplace_back();
        notice.emplace_back();
        revive_progress.push_back(0);
#ifdef USE_TILE
        base_dolls.push_back(player_doll);
        dolls.push_back(current_full_doll());
#endif
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
    stop_auto.emplace_back();
    notice.emplace_back();
    revive_progress.push_back(0);
#ifdef USE_TILE
    // A copy of `you` looks like them (until rebuilt as a new character).
    base_dolls.push_back(player_doll);
    dolls.push_back(current_full_doll());
#endif
    return slots.size() - 1;
}

void party_reset()
{
    // Not an assert: a game that ended mid monster phase (someone died)
    // never got to clear the flag.
    in_monster_phase = false;
    slots.clear();
    spent.clear();
    stop_auto.clear();
    notice.clear();
    revive_progress.clear();
#ifdef USE_TILE
    base_dolls.clear();
    dolls.clear();
#endif
    idle_action = false;
    group_leader = -1;
    group_kind = group_activity::none;
    follow_target.reset();
    follow_blocked_turns = 0;
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

    you.time_taken = aut;
    you.turn_is_over = true;
    party_action_done();
}

// Fallen player (other than the one in `you`) whose body is at `pos`, or -1.
#ifdef USE_TILE
tileidx_t party_doll_tile(int idx)
{
    if (idx < 0 || idx >= (int)dolls.size())
        return 0;
    // Fallen players are drawn see-through, like ghosts.
    return mcache.register_doll(dolls[idx], party_member(idx).pending_revival);
}
#endif

int party_corpse_at(const coord_def &pos)
{
    for (int i = 0; i < (int)slots.size(); ++i)
        if (i != focused && slots[i]->pending_revival && slots[i]->pos() == pos)
            return i;
    return -1;
}

void party_note_idle_action()
{
    idle_action = true;
}

bool party_member_falls()
{
    if (!party_active() || you.pending_revival)
        return you.pending_revival;   // already fallen: nothing more to do
    bool anyone_left = false;
    for (int i = 0; i < (int)slots.size(); ++i)
        if (i != focused && slots[i]->hp > 0 && !slots[i]->pending_revival)
            anyone_left = true;
    if (!anyone_left)
        return false;                 // last one standing: the game ends

    const string name = party_member_name(focused);
    you.pending_revival = true;
    you.hp = 0;
    you.deaths++;
    stop_delay(true);
    revive_progress[focused] = 0;
    if (group_leader == focused)
    {
        group_leader = -1;
        group_kind = group_activity::none;
    }
    {
        const interrupt_block quiet;
        mprf(MSGCH_DANGER, "%s has fallen! Another player can revive them by "
             "resting on their body for %d turns.", name.c_str(),
             PARTY_REVIVE_AUT / BASELINE_DELAY);
    }
    return true;
}

// Bring fallen player `f` back, next to their body, as a Felid comes back
// from a lost life (full hp/mp, bad effects gone).
static void _revive_member(int f)
{
    const int back_to = focused;
    const coord_def body = party_member(f).pos();
    swap_in(f);
    revive();
    coord_def spot;
    you.position.reset();             // don't count as blocking our own spot
    if (!_find_spot_near(body, you, spot))
        spot = body;                  // shouldn't happen; stand on the body
    you.position = spot;
    swap_in(back_to);
    revive_progress[f] = 0;
    spent[f] = PARTY_ROUND_AUT;       // they act from the next round
    mprf(MSGCH_PROMPT, "%s is revived!", party_member_name(f).c_str());
}

// The acting player just finished an action. If they're on a fallen
// player's body and did nothing, that counts towards reviving them;
// anything else (or leaving) starts it over.
static void _tend_fallen(bool idle)
{
    for (int f = 0; f < (int)slots.size(); ++f)
    {
        if (f == focused || !slots[f]->pending_revival)
            continue;
        if (slots[f]->pos() != you.pos())
        {
            // Nobody on the body any more (e.g. you just stepped off it):
            // the revival has to start over.
            if (revive_progress[f] > 0 && party_member_at(slots[f]->pos()) < 0)
            {
                mprf("You leave %s's body; you'll have to start over.",
                     party_member_name(f).c_str());
                revive_progress[f] = 0;
            }
            continue;
        }
        if (!idle)
        {
            if (revive_progress[f] > 0)
            {
                mprf("You stop tending to %s.", party_member_name(f).c_str());
            }
            revive_progress[f] = 0;
            continue;
        }
        if (revive_progress[f] == 0)
        {
            const interrupt_block quiet;
            mprf("You tend to %s's body...", party_member_name(f).c_str());
        }
        revive_progress[f] += you.time_taken;
        if (revive_progress[f] >= PARTY_REVIVE_AUT)
            _revive_member(f);
    }
}

bool party_reviving()
{
    for (int f = 0; f < (int)slots.size(); ++f)
        if (f != focused && slots[f]->pending_revival
            && slots[f]->pos() == you.pos())
        {
            return true;
        }
    return false;
}

int party_revive_progress(int idx)
{
    return revive_progress[idx];
}

static bool _doing(const player &p, group_activity kind)
{
    switch (kind)
    {
    case group_activity::explore: return p.running.is_explore();
    case group_activity::rest:    return p.running.is_rest();
    default:                      return false;
    }
}

static const char *_verb(group_activity kind)
{
    return kind == group_activity::rest ? "resting" : "exploring";
}

string party_danger_seen()
{
    // Solo play keeps the game's own rules (i_feel_safe etc.) untouched.
    if (!party_active())
        return "";
    for (int i = 0; i < party_size(); ++i)
    {
        const player &p = party_member(i);
        if (p.hp <= 0)
            continue;
        for (monster_near_iterator mi(p.pos(), LOS_NO_TRANS); mi; ++mi)
        {
            // visible_to() asserts its looker is the player in `you`, so for
            // the others just check invisibility (their see-invisible
            // isn't modelled yet).
            const bool seen = i == focused ? mi->visible_to(&you)
                                           : !mi->invisible();
            if (mi->wont_attack() || mi->is_firewood()
                || mons_is_always_safe(*mi) || !seen)
            {
                continue;
            }
            return make_stringf("%s can see %s", party_member_name(i).c_str(),
                                mi->name(DESC_A).c_str());
        }
    }
    return "";
}

bool party_others_need_rest()
{
    // Resting on a fallen teammate's body: keep going until they're back.
    if (party_reviving())
        return true;
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == focused || slots[i]->hp <= 0)
            continue;
        if (!slots[i]->is_sufficiently_rested())
            return true;
    }
    return false;
}

bool party_check_auto_activity()
{
    if (!party_active() || !you.running)
        return false;
    ASSERT(focused == active);

    const bool exploring = you.running.is_explore();
    const bool resting = you.running.is_rest();
    if (!exploring && !resting)
        return false;

    // Nobody explores or rests while any of the party can see an enemy.
    const string danger = party_danger_seen();
    if (!danger.empty())
    {
        mprf(MSGCH_WARN, "%s; you stop %s.", danger.c_str(),
             exploring ? "exploring" : "resting");
        stop_running();
        return true;
    }

    // A group rest keeps the leader resting while others recover (see
    // delay.cc); once nobody needs it any more, it's over.
    if (resting && group_kind == group_activity::rest && group_leader == active
        // runmode counts down from RMODE_REST_DURATION (rest until healed);
        // a plain wait starts at RMODE_WAIT_DURATION and isn't ours to stop.
        && you.running.runmode > RMODE_WAIT_DURATION
        && you.is_sufficiently_rested() && !party_others_need_rest())
    {
        mpr("The whole party is rested.");
        stop_running();
        return true;
    }
    return false;
}

void party_action_done(bool manual)
{
    ASSERT(party_active());
    ASSERT(!in_monster_phase);
    ASSERT(focused == active);
    activate_pending_markers();

    // The acting player's share of what world_reacts() normally does after
    // an action. The world-level parts (monsters, clouds, level timers) run
    // once per round in monster_phase() instead.
    fire_final_effects();
    if (!crawl_state.game_is_arena())
    {
        // As in world_reacts(): some callers (e.g. delays that only set
        // time_taken) arrive here without it set, and god code asserts it.
        you.turn_is_over = true;
        religion_turn_end();
        crawl_state.clear_god_acting();
    }
    check_banished();
    player_reacts();
    if (you.num_turns != -1 && you.num_turns < INT_MAX)
        you.num_turns++;

    spent[active] += you.time_taken;
    _tend_fallen(idle_action);
    idle_action = false;

    // Exploring or resting with nobody leading: you lead, everyone else
    // follows along. (A stopped group activity can't restart this way: the
    // leader's is cancelled as soon as control passes to them; see stop_auto
    // below.)
    if (group_leader < 0 && (you.running.is_explore() || you.running.is_rest()))
    {
        group_kind = you.running.is_explore() ? group_activity::explore
                                               : group_activity::rest;
        // Messages interrupt resting: don't let this one stop the rest it
        // announces.
        const interrupt_block quiet;
        if (group_kind == group_activity::explore)
        {
            mprf(MSGCH_PROMPT, "%s leads the party exploring.",
                 party_member_name(active).c_str());
        }
        else
        {
            mprf(MSGCH_PROMPT, "The party rests with %s.",
                 party_member_name(active).c_str());
        }
        group_leader = active;
    }
    if (manual)
    {
        group_leader = -1;
        group_kind = group_activity::none;
        const string why = party_member_name(active) + " is playing by hand";
        for (int i = 0; i < (int)slots.size(); ++i)
            if (i != active)
                stop_auto[i] = why;
    }

    const int before = active;
    advance_turn();
    if (active != before)
        party_announce_turn();

    // The player now in control may have been auto-playing (e.g. exploring)
    // while someone else played by hand. Hand them back the controls.
    if (!stop_auto[active].empty())
    {
        const string why = stop_auto[active];
        stop_auto[active].clear();
        if (you.running)
        {
            mprf(MSGCH_WARN, "You stop %s: %s.",
                 you.running.is_explore() ? "exploring"
                 : you.running.is_rest()  ? "resting"
                                          : "travelling",
                 why.c_str());
            stop_running();
        }
    }
}

string party_member_name(int idx)
{
    return make_stringf("Player %d", idx + 1);
}

void party_announce_turn()
{
    if (!party_active())
        return;
    {
        // Messages interrupt resting; this one mustn't stop a group rest's
        // leader when control comes back to them.
        const interrupt_block quiet;
        mprf(MSGCH_PROMPT, "--- %s's turn (HP %d/%d) ---",
             party_member_name(active).c_str(), you.hp, you.hp_max);
    }
    if (!crawl_state.test)
    {
        // Status panel, map centre and LOS all belong to the new player.
        los_changed();
        redraw_screen();
    }
}

void party_share_manual(skill_type sk, int points)
{
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == focused || slots[i]->hp <= 0)
            continue;
        player &p = *slots[i];
        // XXX: is_useless_skill() judges the player in `you`. Party members
        // are currently all copies of one character, so it's the same answer
        // for everyone; revisit once players can differ (species etc).
        if (is_useless_skill(sk) || p.skills[sk] >= MAX_SKILL_LEVEL)
            continue;
        p.skill_manual_points[sk] += points;
        mprf("%s studies it too.", party_member_name(i).c_str());
    }
}

void party_share_book(const vector<spell_type> &spells)
{
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == focused || slots[i]->hp <= 0)
            continue;
        player &p = *slots[i];
        // Djinn and other innate casters have no library to add to.
        // Read the array directly: player::has_mutation() and friends look at
        // the global `you`, not at the player they're called on.
        if (p.mutation[MUT_INNATE_CASTER] > 0)
            continue;

        vector<string> learned;
        for (spell_type st : spells)
        {
            if (st == SPELL_NO_SPELL || p.spell_library[st])
                continue;
            p.spell_library.set(st, true);
            if (Options.auto_hide_spells)
                p.hidden_spells.set(st, true);
            learned.push_back(spell_title(st));
        }
        // (Unlike library_add_spells() for the reader, this skips the
        // shopping-list update: that list belongs to the player in `you`.)
        if (!learned.empty())
        {
            mprf("%s adds %s to their library too.",
                 party_member_name(i).c_str(),
                 comma_separated_line(learned.begin(), learned.end()).c_str());
        }
    }
}

bool party_knows_spell(int idx, spell_type spell)
{
    return party_member(idx).spell_library[spell];
}

// Skill-manual points left for player `idx` in skill `sk` (tests).
int party_manual_points(int idx, skill_type sk)
{
    return party_member(idx).skill_manual_points[sk];
}

string party_member_not_adjacent()
{
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == focused || slots[i]->hp <= 0)
            continue;
        if (grid_distance(slots[i]->pos(), you.pos()) > 1)
            return party_member_name(i);
    }
    return "";
}

// Is `p` partway through taking stairs (the climb takes a turn)?
static bool _taking_stairs(const player &p)
{
    for (const auto &d : p.delay_queue)
    {
        if (dynamic_cast<const DescendingStairsDelay*>(d.get())
            || dynamic_cast<const AscendingStairsDelay*>(d.get()))
        {
            return true;
        }
    }
    return false;
}

int party_member_at(const coord_def &pos)
{
    for (int i = 0; i < (int)slots.size(); ++i)
        if (i != focused && slots[i]->pos() == pos && slots[i]->hp > 0)
            return i;
    return -1;
}

bool party_unfocused_player_at(const coord_def &pos)
{
    return party_member_at(pos) >= 0;
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
        if (p.hp <= 0 || p.pending_revival)
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
    if (!party_active())
        return;
    // Never leave a fallen player in `you` for the rest of world_reacts().
    int idx = active;
    if (party_member(idx).pending_revival)
    {
        for (int i = 0; i < (int)slots.size(); ++i)
            if (!party_member(i).pending_revival)
            {
                idx = i;
                break;
            }
    }
    swap_in(idx);
}

bool party_in_monster_phase()
{
    return in_monster_phase;
}

// A free spot near `centre` that `who` can stand on: no monster, no player.
// A plain closed door a teammate may be placed beyond (and which then gets
// opened). Runed, sealed and locked doors stay shut.
static bool _openable_door(const coord_def &c)
{
    const dungeon_feature_type f = env.grid(c);
    return f == DNGN_CLOSED_DOOR || f == DNGN_CLOSED_CLEAR_DOOR;
}

// Breadth-first search by walking distance from `centre` for the nearest
// free square `who` can stand on. If `through_doors`, plain closed doors can
// be walked through; any on the path to the chosen square are opened, so the
// party doesn't start shut apart by a door (e.g. arriving in a closet).
static bool _search_spot(const coord_def &centre, const player &who,
                         bool through_doors, coord_def &spot)
{
    const int max_steps = 10;
    static FixedArray<coord_def, GXM, GYM> parent;
    static FixedArray<int, GXM, GYM> dist;
    parent.init(coord_def(-1, -1));
    dist.init(-1);

    vector<coord_def> queue = { centre };
    dist(centre) = 0;
    for (size_t head = 0; head < queue.size(); ++head)
    {
        const coord_def c = queue[head];
        if (dist(c) >= max_steps)
            continue;
        for (adjacent_iterator ai(c); ai; ++ai)
        {
            const coord_def n = *ai;
            if (!in_bounds(n) || dist(n) >= 0)
                continue;
            const bool door = through_doors && _openable_door(n);
            if (!door && !who.is_habitable(n))
                continue;
            dist(n) = dist(c) + 1;
            parent(n) = c;
            queue.push_back(n);

            if (door || actor_at(n) || party_member_at(n) >= 0)
                continue;   // can pass, but not stand here

            // Found it. Open any doors between here and the centre.
            bool opened = false;
            for (coord_def p = parent(n); p != centre; p = parent(p))
            {
                if (!_openable_door(p))
                    continue;
                set<coord_def> door_tiles;
                find_connected_identical(p, door_tiles);
                for (const coord_def &d : door_tiles)
                    dgn_open_door(d);
                opened = true;
            }
            if (opened)
                los_changed();
            spot = n;
            return true;
        }
    }
    return false;
}

// Where to put a teammate arriving with the player at `centre`: the nearest
// free square on foot, preferring ones that don't need a door opened.
static bool _find_spot_near(const coord_def &centre, const player &who,
                            coord_def &spot)
{
    return _search_spot(centre, who, false, spot)
           || _search_spot(centre, who, true, spot);
}

int party_add_character(const coord_def &pos, const newgame_def &ng)
{
    ASSERT(!in_monster_phase);
    const int prev = active;
    // Start from a copy (world state, level, position), then rebuild the
    // character part of it as `ng` while it's swapped into `you`.
    const int idx = party_add_player(pos);
    party_switch_to(idx);
    setup_party_member(ng);
#ifdef USE_TILE
    // Their own body, the default look for their species (player_doll is
    // still the copied player's); swapping out records it as theirs.
    for (int i = 0; i < TILEP_PART_MAX; i++)
        player_doll.parts[i] = TILEP_SHOW_EQUIP;
    tilep_race_default(you.species, you.experience_level, &player_doll);
#endif
    party_switch_to(prev);
    return idx;
}

void party_start(int size, bool choose)
{
    ASSERT(!in_monster_phase);
    size = min(size, PARTY_MAX_SIZE);
    while (party_size() < size)
    {
        const string name = party_member_name(party_size());
        coord_def spot;
        if (!_find_spot_near(you.pos(), you, spot))
        {
            mprf(MSGCH_ERROR, "No room for %s to join!", name.c_str());
            break;
        }
        newgame_def ng;
        if (choose && choose_party_member(ng, name))
            party_add_character(spot, ng);
        else
        {
            if (choose)
                mprf("%s joins as a copy of you.", name.c_str());
            party_add_player(spot);
        }
    }
    if (party_active())
    {
        mprf(MSGCH_PROMPT, "A party of %d sets out. Players act in turn; "
             "monsters move once everyone has.", party_size());
        party_announce_turn();
    }
}

void party_follow_to_current_level()
{
    if (!party_active())
        return;
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == focused)
            continue;
        player &p = *slots[i];
        // A player's level is stored on the player; bring them along.
        p.where_are_you = you.where_are_you;
        p.depth = you.depth;
        p.on_current_level = true;
        p.position.reset();     // so they don't block their own placement
        coord_def spot;
        if (_find_spot_near(you.pos(), p, spot))
            p.position = spot;
        else
        {
            // Fall back to anywhere passable; better than being in a wall.
            for (rectangle_iterator ri(1); ri; ++ri)
                if (!actor_at(*ri) && party_member_at(*ri) < 0
                    && p.is_habitable(*ri))
                {
                    p.position = *ri;
                    break;
                }
        }
    }
}

bool party_swap_places_with_member_at(const coord_def &pos)
{
    const int idx = party_member_at(pos);
    if (idx < 0)
        return false;
    player &other = *slots[idx];
    if (!other.is_habitable(you.pos()))
    {
        mprf("%s can't stand where you are.",
             party_member_name(idx).c_str());
        return false;
    }
    other.position = you.pos();
    mprf("You swap places with %s.", party_member_name(idx).c_str());
    return true;
}

bool party_blocks_travel(const coord_def &c)
{
    // The explorer paths straight through teammates and swaps past them
    // (see move_player_action); otherwise a follower standing in a corridor
    // hides everything behind them and explore wrongly says it's done. Only
    // one player explores at a time (the rest follow), so this can't loop.
    if (you.running.is_explore())
        return false;
    return party_member_at(c) >= 0 && c != follow_target;
}

int party_explore_leader()
{
    return group_kind == group_activity::explore ? group_leader : -1;
}

int party_rest_leader()
{
    return group_kind == group_activity::rest ? group_leader : -1;
}

bool party_leader_waits_for_group()
{
    if (!party_active() || group_kind != group_activity::explore
        || group_leader != active || !you.running.is_explore())
    {
        return false;
    }
    ASSERT(focused == active);
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i == active || slots[i]->hp <= 0)
            continue;
        if (grid_distance(slots[i]->pos(), you.pos()) > PARTY_EXPLORE_MAX_GAP)
            return true;
    }
    return false;
}

// End group explore: `msg` tells the player whose turn it is; the leader (if
// still exploring) is stopped on their next turn, told `leader_why`.
static bool _end_group_explore(const string &msg, const string &leader_why)
{
    mprf(MSGCH_WARN, "%s", msg.c_str());
    if (group_leader >= 0 && group_leader != active)
        stop_auto[group_leader] = leader_why;
    group_leader = -1;
    group_kind = group_activity::none;
    return false;
}

bool party_group_explore_turn()
{
    if (!party_active())
        return false;
    ASSERT(focused == active);

    if (!notice[active].empty())
    {
        mprf(MSGCH_WARN, "%s", notice[active].c_str());
        notice[active].clear();
    }

    // Someone is climbing the stairs and the whole party is about to go
    // with them: nothing to do but wait.
    for (int i = 0; i < (int)slots.size(); ++i)
    {
        if (i != active && slots[i]->hp > 0 && _taking_stairs(*slots[i]))
        {
            party_note_idle_action();
            you.turn_is_over = true;
            return true;
        }
    }
    if (group_leader < 0)
        return false;

    const group_activity kind = group_kind;
    const char *verb = _verb(kind);

    // The main loop only asks us when the player is about to be prompted, so
    // if that's the leader, their activity has stopped (item, monster, done,
    // rested): the group activity is over.
    if (active == group_leader)
    {
        for (int i = 0; i < (int)slots.size(); ++i)
        {
            if (i != active)
            {
                notice[i] = make_stringf("%s has stopped %s.",
                                         party_member_name(active).c_str(),
                                         verb);
            }
        }
        group_leader = -1;
        group_kind = group_activity::none;
        return false;
    }

    const player &leader = party_member(group_leader);
    const string lname = party_member_name(group_leader);

    // The leader's activity stopped on its own. Everyone gets control back.
    if (!_doing(leader, kind))
    {
        group_leader = -1;
        group_kind = group_activity::none;
        mprf(MSGCH_WARN, "%s has stopped %s.", lname.c_str(), verb);
        return false;
    }

    // Anyone in the party seeing an enemy stops everyone.
    const string danger = party_danger_seen();
    if (!danger.empty())
    {
        return _end_group_explore(
            make_stringf("%s; the party stops %s.", danger.c_str(), verb),
            danger);
    }

    // Resting together: just wait (and regenerate) on your turns.
    if (kind == group_activity::rest)
    {
        party_note_idle_action();
        you.turn_is_over = true;
        return true;
    }

    // Already beside the leader: wait for them to move on.
    if (grid_distance(you.pos(), leader.pos()) <= 1)
    {
        party_note_idle_action();
        you.turn_is_over = true;
        return true;
    }

    // One step along the travel path towards the leader.
    unwind_var<coord_def> target(follow_target, leader.pos());
    travel_pathfind tp;
    tp.set_src_dst(you.pos(), leader.pos());
    const coord_def step = tp.pathfind(RMODE_TRAVEL);
    if (step.origin() || grid_distance(step, you.pos()) != 1)
    {
        return _end_group_explore(make_stringf(
            "You can't find a way to %s; the party stops exploring.",
            lname.c_str()), party_member_name(active) + " can't reach you");
    }

    // Someone's in the way (another follower, or a monster stepped in):
    // wait rather than swap or attack -- but not forever.
    if (actor_at(step) || party_member_at(step) >= 0)
    {
        if (++follow_blocked_turns > FOLLOW_MAX_BLOCKED_TURNS)
        {
            follow_blocked_turns = 0;
            return _end_group_explore(make_stringf(
                "Something is in your way; you can't follow %s.",
                lname.c_str()), party_member_name(active) + " is stuck");
        }
        you.turn_is_over = true;
        return true;
    }
    follow_blocked_turns = 0;

    move_player_action(step - you.pos());
    if (!you.turn_is_over)
    {
        return _end_group_explore(make_stringf(
            "You can't keep up with %s; the party stops exploring.",
            lname.c_str()), party_member_name(active) + " can't keep up");
    }
    return true;
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

// party.add_character(x, y, combo, weapon): e.g. ("DjFE", "unarmed").
LUAFN(party_lua_add_character)
{
    const coord_def pos(luaL_safe_checkint(ls, 1), luaL_safe_checkint(ls, 2));
    if (!in_bounds(pos))
        luaL_argerror(ls, 1, "position out of bounds");
    const string combo = luaL_checkstring(ls, 3);
    newgame_def ng;
    ng.type = GAME_TYPE_NORMAL;
    ng.species = species::from_abbrev(combo.substr(0, 2).c_str());
    ng.job = get_job_by_abbrev(combo.substr(2, 2).c_str());
    ng.weapon = str_to_weapon(luaL_optstring(ls, 4, "unarmed"));
    ng.name = party_member_name(party_size());
    if (ng.species == SP_UNKNOWN || ng.job == JOB_UNKNOWN)
        luaL_argerror(ls, 3, "unknown species/background abbreviation");
    PLUARET(integer, party_add_character(pos, ng) + 1);
}

// party.species(i): e.g. "Djinni".
LUAFN(party_lua_species)
{
    PLUARET(string, species::name(party_member(_lua_index(ls, 1)).species).c_str());
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

LUAFN(party_lua_where)
{
    const player &p = party_member(_lua_index(ls, 1));
    PLUARET(string, level_id(p.where_are_you, p.depth).describe().c_str());
}

// What load_level() does for the party on arrival; lets tests check the
// placement without real stairs (which need a save file).
LUAFN(party_lua_follow)
{
    UNUSED(ls);
    party_follow_to_current_level();
    return 0;
}

// 1-based index of the player leading a group rest, or 0 if none.
LUAFN(party_lua_rest_leader)
{
    PLUARET(integer, party_rest_leader() + 1);
}

// "Player 2 can see a goblin", or "" if no player sees an enemy.
LUAFN(party_lua_danger)
{
    PLUARET(string, party_danger_seen().c_str());
}

// 1-based index of the player leading group explore, or 0 if none.
LUAFN(party_lua_explore_leader)
{
    PLUARET(integer, party_explore_leader() + 1);
}

// party.knows_spell(i, spell_name): is it in player i's library?
LUAFN(party_lua_knows_spell)
{
    const int idx = _lua_index(ls, 1);
    const spell_type sp = spell_by_name(luaL_checkstring(ls, 2), true);
    if (sp == SPELL_NO_SPELL)
        luaL_argerror(ls, 2, "unknown spell");
    PLUARET(boolean, party_knows_spell(idx, sp));
}

// party.manual_points(i, skill_name): manual points player i has left.
LUAFN(party_lua_manual_points)
{
    const int idx = _lua_index(ls, 1);
    const skill_type sk = str_to_skill(luaL_checkstring(ls, 2));
    if (sk == SK_NONE)
        luaL_argerror(ls, 2, "unknown skill");
    PLUARET(integer, party_manual_points(idx, sk));
}

// party.form(i): player i's current form ("" if none), read from their own
// struct (not `you`), so it shows whether a transformation leaked across.
LUAFN(party_lua_form)
{
    const player &p = party_member(_lua_index(ls, 1));
    PLUARET(string, p.form == transformation::none ? ""
                    : transform_name(p.form));
}

// party.fallen(i): is player i down, waiting to be revived?
LUAFN(party_lua_fallen)
{
    PLUARET(boolean, party_member(_lua_index(ls, 1)).pending_revival);
}

// party.revive_progress(i): turns spent tending player i's body so far.
LUAFN(party_lua_revive_progress)
{
    PLUARET(integer, party_revive_progress(_lua_index(ls, 1)) / BASELINE_DELAY);
}

LUAFN(party_lua_hp)
{
    const player &p = party_member(_lua_index(ls, 1));
    lua_pushinteger(ls, p.hp);
    lua_pushinteger(ls, p.hp_max);
    return 2;
}

// party.set_hp(i, hp): for tests, e.g. to make the party need a rest.
LUAFN(party_lua_set_hp)
{
    player &p = const_cast<player &>(party_member(_lua_index(ls, 1)));
    p.hp = max(1, min((int)luaL_checkinteger(ls, 2), p.hp_max));
    p.redraw_hit_points = true;
    return 0;
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
{ "add_character", party_lua_add_character },
{ "species", party_lua_species },
{ "fallen", party_lua_fallen },
{ "form", party_lua_form },
{ "revive_progress", party_lua_revive_progress },
{ "spend", party_lua_spend },
{ "reset", party_lua_reset },
{ "hp", party_lua_hp },
{ "set_hp", party_lua_set_hp },
{ "pos", party_lua_pos },
{ "where", party_lua_where },
{ "manual_points", party_lua_manual_points },
{ "knows_spell", party_lua_knows_spell },
{ "follow", party_lua_follow },
{ "explore_leader", party_lua_explore_leader },
{ "rest_leader", party_lua_rest_leader },
{ "danger", party_lua_danger },
{ nullptr, nullptr }
};
