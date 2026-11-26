-- Weapons and armour nobody has on are in the shared pack too, including a
-- separately chosen character's spare starting gear.

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

local function names()
  local t = {}
  for _, it in ipairs(items.inventory()) do
    t[#t + 1] = it.name() .. (it.equipped and " (on)" or "")
  end
  return table.concat(t, ", ")
end

local function has(pat, worn)
  for _, it in ipairs(items.inventory()) do
    if it.name():find(pat) and (worn == nil or it.equipped == worn) then
      return true
    end
  end
  return false
end

you.moveto(10, 10)
local p2 = party.add_character(12, 10, "HuFi", "hand axe")
party.switch_to(1)

-- Player 1 picks up a weapon and a body armour.
dgn.create_item(10, 10, "dagger")
turn(",")
party.switch_to(1)
dgn.create_item(10, 10, "robe")
turn(",")
assert(has("dagger") and has("robe"),
       "player 1 didn't pick both up: " .. names() .. " / " .. crawl.messages(4))
assert(party.active() == p2 or party.active() == 1)
party.switch_to(p2)
assert(has("dagger", false), "player 2 can't see the dagger: " .. names())
assert(has("robe", false), "player 2 can't see the robe: " .. names())
assert(has("hand axe", true), "player 2 lost their own axe: " .. names())
party.switch_to(1)
assert(has("dagger", false), "the dagger didn't come back: " .. names())
assert(not has("hand axe"), "player 1 sees player 2's wielded axe: " .. names())
party.reset()
