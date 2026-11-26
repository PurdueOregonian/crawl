-- Teleportation is one status for the whole party: a teleport started by
-- anyone counts down for everyone, cancelling it cancels it for all, and
-- when it goes off the whole party lands together.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
debug.los_changed()
crawl.setopt("show_more = false")

local function turn(key)
  crawl.sendkeys(key)
  debug.input_once()
end

local function letter(pat)
  for _, it in ipairs(items.inventory()) do
    if it.name():find(pat) then
      local i = it.slot
      return i < 26 and string.char(97 + i) or string.char(65 + i - 26)
    end
  end
end

-- Somewhere open in the generated level for the two of them.
local sx, sy
for y = 2, 68 do
  for x = 2, 78 do
    if not sx and dgn.grid(x, y) == dgn.feature_number("floor")
       and dgn.grid(x + 1, y) == dgn.feature_number("floor")
       and dgn.grid(x + 2, y) == dgn.feature_number("floor") then
      sx, sy = x, y
    end
  end
end
assert(sx, "no open floor on the level")
you.moveto(sx, sy)
local p2 = party.add_player(sx + 2, sy)
party.switch_to(1)

local function cheb(x1, y1, x2, y2)
  return math.max(math.abs(x1 - x2), math.abs(y1 - y2))
end

-- 1. Player 1 reads teleportation: both start counting down.
dgn.create_item(sx, sy, "scroll of teleportation q:3")
turn(",")
party.switch_to(1)
turn("r" .. letter("scroll"))
assert(party.teleporting(1) > 0, "player 1 isn't teleporting: "
                                 .. crawl.messages(4))
-- (Player 1's has already ticked down by the turn spent reading.)
assert(party.teleporting(p2) >= party.teleporting(1), string.format(
  "player 2's countdown is %d, player 1's %d",
  party.teleporting(p2), party.teleporting(1)))

-- 2. It goes off: both land together, nobody is still teleporting.
local ox, oy = party.pos(1)
for _ = 1, 40 do
  if party.teleporting(1) == 0 and party.teleporting(p2) == 0 then break end
  turn("s")
end
local x1, y1 = party.pos(1)
local x2, y2 = party.pos(p2)
assert(party.teleporting(1) == 0 and party.teleporting(p2) == 0,
       "still teleporting after 40 turns")
assert(not (x1 == ox and y1 == oy), "player 1 never moved")
assert(cheb(x1, y1, x2, y2) <= 2, string.format(
  "landed apart: (%d,%d) and (%d,%d)", x1, y1, x2, y2))
assert(crawl.messages(10):find("pulled through space with you"),
       "no message that the teammate came along: " .. crawl.messages(10))

-- 3. Player 2 starts one, then player 2 reads another: cancelled for both.
-- (Now identified, reading asks "no enemies in sight, really?": Y.)
party.switch_to(p2)
turn("r" .. letter("scroll") .. "Y")
assert(party.teleporting(1) > 0, "player 2's teleport didn't reach player 1")
party.switch_to(p2)
turn("r" .. letter("scroll") .. "Y")
assert(party.teleporting(p2) == 0, "player 2's teleport wasn't cancelled")
assert(party.teleporting(1) == 0, "player 1's teleport wasn't cancelled")

-- 4. An instant teleport (like a teleport trap) takes everyone too.
party.switch_to(p2)
ox, oy = party.pos(p2)
you.random_teleport()
x1, y1 = party.pos(1)
x2, y2 = party.pos(p2)
assert(not (x2 == ox and y2 == oy), "player 2 never moved")
assert(cheb(x1, y1, x2, y2) <= 2, string.format(
  "instant teleport left them apart: (%d,%d) and (%d,%d)", x1, y1, x2, y2))

-- A teammate who can't teleport is left out and stays where they are, and
-- doesn't stop the others going.
local function teleport_leaves_behind(who, why)
  party.switch_to(1)
  local hx, hy = party.pos(who)
  ox, oy = party.pos(1)
  dgn.create_item(ox, oy, "scroll of teleportation")
  turn(",")
  party.switch_to(1)
  turn("r" .. letter("scroll") .. "Y")
  assert(party.teleporting(1) > 0, "player 1 isn't teleporting ("
                                   .. why .. "): " .. crawl.messages(4))
  assert(party.teleporting(who) == 0, "a teammate who can't teleport ("
                                      .. why .. ") got a countdown")
  for _ = 1, 40 do
    if party.teleporting(1) == 0 then break end
    turn("s")
  end
  x1, y1 = party.pos(1)
  x2, y2 = party.pos(who)
  assert(not (x1 == ox and y1 == oy), "player 1 never moved (" .. why .. ")")
  assert(x2 == hx and y2 == hy, string.format(
    "teammate (%s) moved from (%d,%d) to (%d,%d)", why, hx, hy, x2, y2))

  -- An instant teleport leaves them too.
  party.switch_to(1)
  ox, oy = party.pos(1)
  you.random_teleport()
  x2, y2 = party.pos(who)
  assert(x2 == hx and y2 == hy, "instant teleport moved the teammate ("
                                .. why .. ")")
end

-- 5. Dimension anchor.
party.anchor(p2, 5000)
teleport_leaves_behind(p2, "anchored")
party.anchor(p2, 0)

-- 6. A formicid's stasis.
party.reset()
you.moveto(sx, sy)
local fo = party.add_character(sx + 2, sy, "FoFi", "hand axe")
assert(party.species(fo) == "Formicid", party.species(fo))
teleport_leaves_behind(fo, "formicid")
party.reset()
