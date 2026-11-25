-- Party members are like permanent allies: nothing ever ends up inside one.
-- Anything put onto a teammate's square (the player teleporting or blinking
-- there, a monster arriving) pushes the teammate to the nearest free square.

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
local p2 = party.add_player(20, 20)
party.switch_to(1)

local function cheb(ax, ay, bx, by)
  return math.max(math.abs(ax - bx), math.abs(ay - by))
end

-- 1. Player 1 lands on player 2 (as a teleport/blink would).
you.moveto(20, 20)
local x1, y1 = you.pos()
local x2, y2 = party.pos(p2)
assert(x1 == 20 and y1 == 20, "player 1 didn't arrive")
assert(not (x2 == 20 and y2 == 20), "player 1 ended up inside player 2")
assert(cheb(x2, y2, 20, 20) == 1,
       string.format("player 2 pushed to (%d,%d), not next door", x2, y2))

-- 2. A monster placed on player 2's square pushes them aside too.
party.set_pos(p2, 15, 15)
local mon = dgn.create_monster(15, 15, "goblin att:friendly")
assert(mon, "couldn't place the monster")
x2, y2 = party.pos(p2)
assert(not (x2 == 15 and y2 == 15), "a monster ended up inside player 2")
assert(cheb(x2, y2, 15, 15) == 1,
       string.format("player 2 pushed to (%d,%d), not next door", x2, y2))
assert(mon.abs_x == 15 and mon.abs_y == 15, "the monster went elsewhere")

party.reset()
dgn.dismiss_monsters()
