-- The party shares one pack: potions, scrolls and any gear nobody has on go
-- with whoever has control. What each player wears or wields stays theirs.

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

-- Items in the pack of the player in `you` named like `pat`; `worn` filters
-- on whether they're equipped (nil: either).
local function count(pat, worn)
  local n = 0
  for _, it in ipairs(items.inventory()) do
    if it.name():find(pat) and (worn == nil or it.equipped == worn) then
      n = n + it.quantity
    end
  end
  return n
end

local function letters_unique(what)
  local seen = {}
  for _, it in ipairs(items.inventory()) do
    local key = (it.class(true) == "potion" and "q" or
                 it.class(true) == "scroll" and "r" or "gear") .. it.slot
    assert(not seen[key], what .. ": two items share a slot/letter: "
                          .. it.name() .. " and " .. tostring(seen[key]))
    seen[key] = it.name()
  end
end

-- 1. Potions picked up before the party forms aren't duplicated for the copy.
you.moveto(10, 10)
dgn.create_item(10, 10, "potion of curing q:2")
turn(",")
assert(count("potion") == 2, "player 1 didn't pick up 2 potions: "
                             .. crawl.messages(4))
local p2 = party.add_player(12, 10)
party.switch_to(p2)
assert(count("potion") == 2, string.format(
  "player 2 should hold the party's 2 potions, has %d", count("potion")))
assert(count("leather armour", true) == 1, "player 2 lost their own armour")
party.switch_to(1)
assert(count("potion") == 2, string.format(
  "player 1 should hold the party's 2 potions again, has %d", count("potion")))
assert(count("leather armour", true) == 1, "player 1 lost their own armour")
assert(count("leather armour") == 1,
       "player 1 can see player 2's armour in the pack")

-- 2. Something one player picks up is there for the next player.
turn("s")                                 -- player 1 waits
assert(party.active() == p2, "turn didn't pass to player 2")
local x, y = party.pos(p2)
dgn.create_item(x, y, "scroll of teleportation")
turn(",")
assert(party.active() == 1, "turn didn't come back to player 1")
assert(count("scroll") == 1, "player 1 can't see player 2's scroll: "
                               .. crawl.messages(4))

-- 3. Gear one player takes off goes into the pack, even when the other
-- player has their own gear at that letter (both wear a hat as 'c').
turn("s")
assert(party.active() == p2)
turn("Tc")                                -- player 2 takes off their hat
for _ = 1, 40 do
  if party.active() == 1 then
    if count("hat") == 2 then break end
    turn("s")
  elseif count("hat", true) == 0 then
    turn("s")             -- player 2 is done and is asked for a command
  else
    debug.input_once()    -- player 2 is still taking it off
  end
end
assert(party.active() == 1, "never got back to player 1: "
                            .. crawl.messages(8))
assert(count("hat", true) == 1, "player 1 lost their hat")
assert(count("hat", false) == 1, "player 2's hat isn't in the pack: "
                                 .. crawl.messages(6))
letters_unique("after the hat moved")

-- 4. Identification is shared: player 2 drinks one of the potions, and
-- player 1 then knows what the other one is.
local function letter(pat)
  for _, it in ipairs(items.inventory()) do
    if it.name():find(pat) then
      local i = it.slot
      return i < 26 and string.char(97 + i) or string.char(65 + i - 26)
    end
  end
end
turn("s")
assert(party.active() == p2)
assert(count("curing") == 0, "potions already identified")
party.set_hp(p2, 5)   -- or drinking curing asks "really?" at full health
turn("q" .. letter("potion"))
assert(party.active() == 1, "turn didn't pass after quaffing: "
                            .. crawl.messages(4))
assert(count("curing") == 1, "player 1 doesn't know the potion player 2 "
                             .. "drank: " .. crawl.messages(4))
party.reset()
