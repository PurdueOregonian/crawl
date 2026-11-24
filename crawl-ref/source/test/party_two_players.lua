-- Milestone (a) acceptance test: two players, one level, swap-based turns.
--
-- API (module `party`, see party.cc):
--   party.size()            -> number of players (1 when not in a party)
--   party.add_player(x, y)  -> index of a new player cloned from the active
--                              one, standing at (x, y)
--   party.active()          -> index of the player whose turn it is
--   party.switch_to(i)      -> make player i the active `you`
--   party.spend(aut)        -> active player spends `aut` time. When every
--                              living player has spent >= 10 this round,
--                              monsters get one 10-aut round, then the next
--                              round starts. The active player keeps control
--                              until they have spent >= 10 this round, then
--                              it passes to the next player who hasn't.
--   party.reset()           -> dissolve the party (active player carries on)
--   party.hp(i)             -> hp, hp_max of player i (no swap needed)
--   party.pos(i)            -> x, y of player i

-- Real character, fresh level (see party_baseline.lua).
you.init("migl", "morningstar")
you.set_xl(10)
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()

you.moveto(10, 10)
assert(party.size() == 1, "fresh game should have a party of one")

-- 1. Adding a second player.
local p2 = party.add_player(20, 10)
assert(party.size() == 2, "add_player did not grow the party")
local x2, y2 = party.pos(p2)
assert(x2 == 20 and y2 == 10, "player 2 placed at wrong position")
local x1, y1 = party.pos(1)
assert(x1 == 10 and y1 == 10, "adding player 2 moved player 1")

-- 2. Round structure: monsters do not act until everyone has spent a turn.
-- The goblin starts nearer player 1. Player 2 acts last in each round, so
-- player 2 is the one sitting in `you` when monsters move; going after
-- player 1 only works if the monster phase really swaps targets in.
local mon = dgn.create_monster(5, 10, "generate_awake goblin")
assert(mon, "failed to place goblin")
-- Use abs_x/abs_y: mon.x/mon.y are relative to whichever player is in `you`.

-- A freshly placed monster may not have enough energy to act in its very
-- first round, so allow a few rounds -- but in every round it must stay put
-- until player 2 has had their turn.
party.switch_to(1)
local moved = false
for round = 1, 3 do
  local bx, by = mon.abs_x, mon.abs_y
  party.spend(10)
  assert(mon.abs_x == bx and mon.abs_y == by,
         "monster acted before player 2 had their turn (round " .. round .. ")")
  assert(party.active() == p2, "control did not pass to player 2")
  party.spend(10)
  assert(party.active() == 1, "control did not return to player 1")
  -- Any movement counts: an unaware monster wanders rather than approaches.
  if mon.abs_x ~= bx or mon.abs_y ~= by then
    moved = true
    break
  end
end
assert(moved, "monster never acted after completed rounds")

-- 3. Targeting: the goblin is nearer player 1, so it should approach them.
-- Give it time to notice someone (stealth rolls), close in and swing.
local function cheb(ax, ay, bx, by)
  return math.max(math.abs(ax - bx), math.abs(ay - by))
end
for i = 1, 40 do
  party.spend(10)
  party.spend(10)
  local hp1, max1 = party.hp(1)
  if hp1 < max1 then break end
end
x1, y1 = party.pos(1)
assert(cheb(mon.abs_x, mon.abs_y, x1, y1) <= 1,
       "goblin did not close in on the nearer player (player 1)")

-- 4. HP is per player: player 1 took the hits, player 2 did not.
local hp1, max1 = party.hp(1)
local hp2, max2 = party.hp(p2)
assert(hp1 < max1, "player 1 was never hit")
assert(hp2 == max2, "player 2 took damage they shouldn't have")

-- 5. Haste-style fractional turns: a player spending 5 aut keeps control
--    until they have spent a full 10 for the round.
dgn.dismiss_monsters()
party.switch_to(1)
party.spend(5)
assert(party.active() == 1, "player 1 lost control after only 5 aut")
party.spend(5)
assert(party.active() == p2, "player 1 kept control after a full turn")

dgn.dismiss_monsters()
party.reset()
assert(party.size() == 1, "party.reset did not dissolve the party")
you.set_xl(1)
