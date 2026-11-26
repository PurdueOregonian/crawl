-- Teleport traps and the "malevolent force" teleport move the whole party
-- together, and a teammate's Ashenzari warns everyone of malevolent traps.
-- (D:15 is the bottom of the Dungeon, so the malevolence can't shaft anyone:
-- real level changes can't run under -test.)

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:15")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
debug.los_changed()
debug.disable("confirmations")
crawl.setopt("show_more = false")

local function turn(key)
  crawl.sendkeys(key)
  debug.input_once()
end

local function cheb(x1, y1, x2, y2)
  return math.max(math.abs(x1 - x2), math.abs(y1 - y2))
end

local floor = dgn.feature_number("floor")
local sx, sy
for y = 2, 68 do
  for x = 2, 76 do
    if not sx and dgn.grid(x, y) == floor and dgn.grid(x + 1, y) == floor
       and dgn.grid(x + 2, y) == floor and dgn.grid(x + 3, y) == floor then
      sx, sy = x, y
    end
  end
end
assert(sx, "no open floor on the level")

-- A hostile monster far away, so a malevolent teleport has a target.
local gx, gy
for y = 2, 68 do
  for x = 2, 78 do
    if not gx and dgn.grid(x, y) == floor and cheb(x, y, sx, sy) > 20 then
      gx, gy = x, y
    end
  end
end
assert(gx, "nowhere to put a goblin")
assert(dgn.create_monster(gx, gy, "goblin"), "couldn't place the goblin")

-- Player 2 on the left, player 1 next to them.
you.moveto(sx + 1, sy)
local p2 = party.add_player(sx, sy)
party.switch_to(1)

-- 1. Player 1 steps onto a permanent teleport trap: both land together.
dgn.grid(sx + 2, sy, "trap_teleport_permanent")
debug.los_changed()
turn("l")
local x1, y1 = party.pos(1)
local x2, y2 = party.pos(p2)
assert(cheb(x1, y1, sx + 2, sy) > 2, string.format(
  "player 1 wasn't teleported by the trap: at (%d,%d); %s", x1, y1,
  crawl.messages(6)))
assert(cheb(x1, y1, x2, y2) <= 2, string.format(
  "teleport trap left them apart: (%d,%d) and (%d,%d)", x1, y1, x2, y2))

-- 2. The malevolent force teleports player 1: player 2 comes too.
party.switch_to(1)
local moved = false
for _ = 1, 200 do
  local ox, oy = party.pos(1)
  party.malevolence()
  x1, y1 = party.pos(1)
  if x1 ~= ox or y1 ~= oy then
    moved = true
    break
  end
end
assert(moved, "the malevolent force never teleported player 1")
x2, y2 = party.pos(p2)
assert(cheb(x1, y1, x2, y2) <= 2, string.format(
  "malevolent teleport left them apart: (%d,%d) and (%d,%d)",
  x1, y1, x2, y2))

-- 3. Player 2 worships Ashenzari (at the piety for the warning): nothing
-- malevolent happens to player 1 either, and Ashenzari says so.
party.set_god(p2, "Ashenzari", 200)
party.switch_to(1)
assert(you.god() == "No God", "player 1 has a god: " .. you.god())
local warned = false
for _ = 1, 200 do
  local ox, oy = party.pos(1)
  party.malevolence()
  x1, y1 = party.pos(1)
  assert(x1 == ox and y1 == oy, "player 1 was teleported despite "
                                .. "Ashenzari: " .. crawl.messages(4))
  if crawl.messages(3):find("Ashenzari") then warned = true end
end
assert(warned, "Ashenzari never warned the party")
party.reset()
