-- Party placement on arrival (game start / stairs / shafts all use the same
-- step that party.follow() runs). Teammates go to the nearest free square on
-- foot. If the arrival spot is a closet whose only way out is a closed door,
-- they're placed beyond it and the door is opened, so nobody is shut away.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
dgn.dismiss_monsters()

local function feat(x, y) return dgn.feature_name(dgn.grid(x, y)) end
local function cheb(ax, ay, bx, by)
  return math.max(math.abs(ax - bx), math.abs(ay - by))
end

-- 1. Closet: player 1 at (10,10), rock all round except a closed door at
--    (11,10) leading to a corridor (12..16, 10).
dgn.fill_grd_area(5, 5, 20, 15, "rock_wall")
dgn.grid(10, 10, "floor")
dgn.grid(11, 10, "closed_door")
dgn.fill_grd_area(12, 10, 16, 10, "floor")
debug.los_changed()
you.moveto(10, 10)

local p2 = party.add_player(30, 30)   -- somewhere else; follow() moves them
party.follow()

local x2, y2 = party.pos(p2)
assert(feat(x2, y2) == "floor",
       string.format("player 2 placed on %s at (%d,%d)", feat(x2, y2), x2, y2))
assert(not (x2 == 10 and y2 == 10), "player 2 placed on player 1")
assert(cheb(x2, y2, 10, 10) <= 2,
       string.format("player 2 placed too far away: (%d,%d)", x2, y2))
assert(feat(11, 10) == "open_door",
       "closet door wasn't opened (it's " .. feat(11, 10) .. ")")
party.reset()

-- 2. Open room next to a closed door: no door needs opening, so none is.
dgn.fill_grd_area(5, 5, 20, 15, "rock_wall")
dgn.fill_grd_area(8, 8, 12, 12, "floor")
dgn.grid(13, 10, "closed_door")
dgn.fill_grd_area(14, 10, 16, 10, "floor")
debug.los_changed()
you.moveto(10, 10)

p2 = party.add_player(30, 30)
party.follow()
x2, y2 = party.pos(p2)
assert(cheb(x2, y2, 10, 10) == 1,
       string.format("player 2 not placed next to player 1: (%d,%d)", x2, y2))
assert(feat(13, 10) == "closed_door",
       "a door was opened although the room had space")
party.reset()
