-- Gems are shared: one picked up by anyone is the whole party's, and their
-- clock runs on game time (once per round), not once per member's action.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()
crawl.setopt("show_more = false")

local function turn(key)
  crawl.sendkeys(key)
  debug.input_once()
end

you.moveto(10, 10)
local p2 = party.add_player(12, 10)
party.switch_to(1)

-- Player 1 picks up the Dungeon's gem.
dgn.create_item(10, 10, "smoky gem")
turn(",")
party.switch_to(1)
local found = party.gem(1, "smoky")
assert(found, "player 1 didn't get the gem: " .. crawl.messages(4))

party.switch_to(p2)
found = party.gem(p2, "smoky")
assert(found, "player 2 doesn't share player 1's gem")

-- Player 2 finishes the pickup's round.
turn("s")
assert(party.active() == 1, "after the pickup round: player "
       .. party.active() .. " active, time " .. you.time())

-- A full round (both players wait once) is 10 aut of game time.
local _, _, t0 = party.gem(1, "smoky")
local time0 = you.time()
turn("s")
assert(party.active() == p2, "player 2 should act next, not " .. party.active())
turn("s")
assert(party.active() == 1, "a new round should start with player 1, not "
       .. party.active() .. "; time " .. you.time() - time0)
local _, _, t1 = party.gem(1, "smoky")
assert(t1 - t0 == 10, "a round ran the gem clock " .. (t1 - t0) .. " aut")
party.switch_to(p2)
local _, shattered, t2 = party.gem(p2, "smoky")
assert(t2 == t1, "player 2's gem clock " .. t2 .. " ~= player 1's " .. t1)
assert(not shattered)
party.reset()
