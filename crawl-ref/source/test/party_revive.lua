-- Dying in a party: the player falls (leaving a body) instead of the game
-- ending, as long as someone else is standing. Another player revives them
-- by doing nothing on the body for 50 turns (waiting or resting there).
-- Everything here goes through the real input loop.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()
crawl.setopt("show_more = false")

you.moveto(10, 10)
local p2 = party.add_player(12, 10)
party.switch_to(1)

local function turn(key)
  crawl.sendkeys(key)
  debug.input_once()
end

-- 1. Player 2 dies; with player 1 still standing they fall instead.
party.switch_to(p2)
you.die()
party.switch_to(1)
assert(party.fallen(p2), "player 2 didn't fall")
assert(not party.fallen(1), "player 1 fell too")
assert(party.size() == 2, "the fallen player left the party")

-- 2. The fallen player gets no turns: player 1 keeps the keyboard and each
--    of their turns is a full round.
local t0 = you.time()
turn("s")
assert(party.active() == 1, "turn passed to the fallen player")
assert(you.time() == t0 + 10, "round didn't complete without the fallen player")

-- 3. Waiting away from the body does nothing.
assert(party.revive_progress(p2) == 0, "progress from waiting elsewhere")

-- 4. Stand on the body (12,10) and wait: progress builds; stepping off
--    resets it.
turn("l")                       -- (11,10)
turn("l")                       -- (12,10): on the body
local x1, y1 = party.pos(1)
assert(x1 == 12 and y1 == 10, string.format("player 1 at (%d,%d)", x1, y1))
for i = 1, 10 do turn("s") end
assert(party.revive_progress(p2) == 10,
       "10 waits gave " .. party.revive_progress(p2) .. " turns of progress")
turn("h")                       -- step off
assert(party.revive_progress(p2) == 0, "stepping off didn't reset progress")
turn("l")                       -- back on

-- 5. 50 turns of doing nothing on the body revives them.
for i = 1, 49 do turn("s") end
assert(party.fallen(p2), "revived after only 49 turns")
turn("s")
assert(not party.fallen(p2), "not revived after 50 turns ("
       .. party.revive_progress(p2) .. " progress; messages: "
       .. crawl.messages(3) .. ")")
local hp2, max2 = party.hp(p2)
assert(hp2 == max2 and hp2 > 0, string.format("revived with %d/%d hp", hp2, max2))
local x2, y2 = party.pos(p2)
assert(math.max(math.abs(x2 - 12), math.abs(y2 - 10)) <= 1
       and not (x2 == 12 and y2 == 10),
       string.format("revived player 2 at (%d,%d), not beside the body", x2, y2))

-- 6. They're back in the turn order.
turn("s")
assert(party.active() == p2, "revived player didn't get a turn")
turn("s")
assert(party.active() == 1, "turn didn't come back to player 1")

-- 7. The last one standing dying is a real death (the test harness cheats
--    death for a solo player, so just check nobody is marked fallen then).
party.switch_to(p2)
you.die()
party.switch_to(1)
assert(party.fallen(p2), "player 2 didn't fall the second time")
you.die()                       -- player 1: last one standing
assert(not party.fallen(1), "the last player standing was made to 'fall'")

party.reset()
dgn.dismiss_monsters()
