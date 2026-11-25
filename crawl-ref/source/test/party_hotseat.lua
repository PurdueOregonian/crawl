-- Hotseat play through the REAL input loop (keys -> _input() -> commands),
-- not party.spend(). Covers what a human at the keyboard does:
--
--  * a normal move ends your turn and hands control to the next player;
--  * walking into another party member swaps places with them;
--  * arriving on a new level brings the whole party along.

you.init("migl", "morningstar")
you.set_xl(10)
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()

-- No --more-- prompts: nobody is at the keyboard to dismiss them.
crawl.setopt("show_more = false")

you.moveto(10, 10)
local p2 = party.add_player(12, 10)
party.switch_to(1)

-- One real turn: queue a key, run one pass of the main loop.
local function turn(key)
  crawl.sendkeys(key)
  debug.input_once()
end

local function at(i, x, y, what)
  local px, py = party.pos(i)
  assert(px == x and py == y,
         string.format("%s: player %d at (%d,%d), expected (%d,%d)",
                       what, i, px, py, x, y))
end

-- 1. Player 1 moves right; the turn passes to player 2.
local t0 = you.time()
turn("l")
at(1, 11, 10, "after player 1's move")
assert(party.active() == p2, "turn did not pass to player 2 after a move")
assert(you.time() == t0, "monsters' round ran before player 2 acted")

-- 2. Player 2 walks left into player 1: they swap places. That completes
--    the round, so game time advances and it's player 1's turn again.
turn("h")
at(p2, 11, 10, "after swap")
at(1, 12, 10, "after swap")
assert(party.active() == 1, "turn did not return to player 1")
assert(you.time() == t0 + 10, "round did not advance game time by 10")

-- 2b. Stairs need the whole party adjacent. Player 2 is next to player 1;
--     add a player 3 far away and try to go down.
local sx, sy = you.pos()
dgn.grid(sx, sy, "stone_stairs_down_i")
party.add_player(30, 20)                 -- player 3, far away
local t1 = you.time()
turn(">")
assert(party.where(1) == "D:1", "took the stairs without the whole party")
assert(party.active() == 1, "a refused stair attempt passed the turn")
assert(you.time() == t1, "a refused stair attempt took time")
assert(crawl.messages(3):find("Player 3 must be next to you"),
       "no message saying who is missing: " .. crawl.messages(3))
party.reset()
p2 = party.add_player(11, 10)            -- back to two, where player 2 was
dgn.grid(sx, sy, "floor")

-- 3. Level change: player 1 arrives on D:2 and the party follows.
--    (Real stairs need a save file, which the test harness doesn't have, so
--    jump levels the way other tests do and run the same follow step that
--    load_level() runs on arrival. Real stairs are covered by playtesting.)
debug.goto_place("D:2")
debug.generate_level()
-- generate_level() leaves player 1 at their old coordinates, which on the new
-- layout may be solid rock (a real arrival puts you on the stairs). Give them
-- some floor to arrive on, as a staircase would.
do
  local ax, ay = you.pos()
  dgn.fill_grd_area(ax - 2, ay - 2, ax + 2, ay + 2, "floor")
  debug.los_changed()
end
party.follow()
assert(party.where(1) == "D:2", "player 1 is on " .. party.where(1))
assert(party.where(p2) == "D:2", "player 2 was left on " .. party.where(p2))
local x1, y1 = party.pos(1)
local x2, y2 = party.pos(p2)
assert(math.max(math.abs(x1 - x2), math.abs(y1 - y2)) <= 8,
       string.format("player 2 placed far from player 1: (%d,%d) vs (%d,%d)",
                     x1, y1, x2, y2))
assert(not (x1 == x2 and y1 == y2), "players placed on the same square")
assert(dgn.feature_name(dgn.grid(x2, y2)) ~= "rock_wall"
       and dgn.feature_name(dgn.grid(x2, y2)) ~= "stone_wall",
       "player 2 placed in a wall")

-- After the move, player 2 can still take their turn normally.
turn("s")                       -- player 1 waits
assert(party.active() == p2, "turn did not pass to player 2 on the new level")
turn("s")                       -- player 2 waits
assert(party.active() == 1, "turn did not return to player 1")

party.reset()
dgn.dismiss_monsters()
you.set_xl(1)
