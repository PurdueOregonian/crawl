-- The Abyss mustn't separate the party: area shifts move teammates with the
-- player, new-area teleports bring them along, and morphing terrain never
-- leaves a teammate inside a wall.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("Abyss")
test.regenerate_level()
dgn.dismiss_monsters()
crawl.setopt("show_more = false")

local function cheb(x1, y1, x2, y2)
  return math.max(math.abs(x1 - x2), math.abs(y1 - y2))
end

local function together(what, max)
  local x1, y1 = party.pos(1)
  local x2, y2 = party.pos(2)
  assert(cheb(x1, y1, x2, y2) <= (max or 2), string.format(
    "%s: separated, (%d,%d) and (%d,%d)", what, x1, y1, x2, y2))
  assert(dgn.grid(x2, y2) ~= dgn.feature_number("rock_wall")
         and dgn.grid(x2, y2) ~= dgn.feature_number("stone_wall"),
         what .. ": player 2 is in a wall")
  return x1, y1, x2, y2
end

-- The harness doesn't put the player anywhere; start at the Abyss's centre.
dgn.fill_grd_area(38, 33, 42, 37, "floor")
you.moveto(40, 35)
debug.los_changed()
local p2 = party.add_player(41, 35)
party.follow()
together("start")

-- 1. A new-area teleport (as from the Abyss's random translocations).
for _ = 1, 3 do
  you.random_teleport()
  dgn.dismiss_monsters()
  together("abyss teleport")
end

-- 2. Walking far enough shifts the area around the player.
--    a) teammate right there: the shift carries them with the player.
dgn.fill_grd_area(64, 31, 74, 39, "floor")  -- so moves land where asked
party.set_pos(p2, 69, 35)
you.teleport_to(70, 35)                 -- far past the shift distance
local x1, y1, x2, y2 = together("after shift")
assert(x1 < 70, "the abyss didn't shift (player 1 at " .. x1 .. ")")
local dx, dy = x1 - 70, y1 - 35
assert(x2 == 69 + dx and y2 == 35 + dy, string.format(
  "player 2 at (%d,%d) didn't move with the area (expected (%d,%d))",
  x2, y2, 69 + dx, 35 + dy))
--    b) teammate left behind far away: brought along.
party.set_pos(p2, 20, 35)
you.teleport_to(66, 35)
together("shift with a distant teammate")

-- 3. Morphing terrain that walls a teammate in moves them out.
x1, y1 = party.pos(1)
local wx, wy
for dx = -3, 3 do
  for dy = -3, 3 do
    if not wx and cheb(0, 0, dx, dy) == 2
       and dgn.grid(x1 + dx, y1 + dy) == dgn.feature_number("rock_wall") then
      wx, wy = x1 + dx, y1 + dy
    end
  end
end
if not wx then
  wx, wy = x1 + 2, y1
  dgn.terrain_changed(wx, wy, "rock_wall", false)
end
party.set_pos(p2, wx, wy)
debug.pass_turn(10)
together("after morph")
party.reset()
