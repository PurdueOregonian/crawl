-- Baseline for the multiplayer (party) work: pins down the single-player
-- turn/targeting behaviour that the party code must preserve.
--
--  * debug.pass_turn(aut) advances game time by exactly `aut`.
--  * A hostile, awake monster with the player in view closes the distance
--    and attacks within a reasonable number of normal-speed turns.
--
-- When the party code lands, this must still pass unchanged with a party
-- of one.
--
-- Notes for test authors:
--  * The test harness does not create a real character (hp is 0); use
--    you.init() as below.
--  * mon.x / mon.y are RELATIVE to the player, not absolute map coords.

-- Real character, fresh level (same pattern as fsim.lua).
you.init("migl", "morningstar")
you.set_xl(10)          -- enough HP that a goblin can't kill us mid-test
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()

-- Open arena so terrain can't interfere with movement or LOS.
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()

local px, py = 10, 10
you.moveto(px, py)

local mon = dgn.create_monster(px + 6, py, "generate_awake goblin")
assert(mon, "failed to place goblin")

local function dist_to_player(m)
  return math.max(math.abs(m.x), math.abs(m.y))  -- relative coords
end
assert(dist_to_player(mon) == 6, "goblin placed at wrong distance: "
                                 .. dist_to_player(mon))

-- 1. Time accounting.
local t0 = you.time()
debug.pass_turn(10)
assert(you.time() == t0 + 10,
       "pass_turn(10) advanced time by " .. (you.time() - t0))
debug.pass_turn(15)
assert(you.time() == t0 + 25,
       "pass_turn(15) advanced time by " .. (you.time() - t0 - 10))

-- 2. The monster approaches and attacks. Player stays put (no action, just
--    time passing), so the goblin should reach melee range and swing.
local hp0 = you.hp()
assert(hp0 > 0, "test character has no HP")
local reached = false
for i = 1, 20 do
  debug.pass_turn(10)
  if dist_to_player(mon) <= 1 then
    reached = true
  end
  -- Stop at the first hit.
  if reached and you.hp() < hp0 then
    break
  end
end

assert(reached, "goblin never reached the player (distance "
                .. dist_to_player(mon) .. ")")
assert(you.hp() < hp0, "goblin reached the player but never hit (hp "
                       .. you.hp() .. "/" .. hp0 .. ")")

-- Clean up for any test that runs after us.
dgn.dismiss_monsters()
you.set_xl(1)
