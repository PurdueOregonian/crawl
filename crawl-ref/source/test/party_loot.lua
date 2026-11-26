-- New levels get a level's worth of random loot per player: the number of
-- random floor items (and so randarts) scales with the party size, including
-- the configured party_size before the party itself exists (D:1 is made then).

you.init("migl", "morningstar")
debug.reset_player_data()
crawl.setopt("show_more = false")

local function floor_items()
  local n = 0
  for x = 1, 78 do
    for y = 1, 68 do
      local stack = dgn.items_at(x, y)
      if stack then
        n = n + #stack
      end
    end
  end
  return n
end

local LEVELS = 12
local function total_items()
  debug.reset_rng(7)
  local total = 0
  for i = 1, LEVELS do
    debug.goto_place("D:4")
    debug.generate_level()
    total = total + floor_items()
  end
  return total
end

party.reset()
crawl.setopt("party_size = 1")
assert(party.loot_scale() == 1, "solo loot scale " .. party.loot_scale())
local solo = total_items()

-- Configured but not yet started (as when D:1 is generated).
crawl.setopt("party_size = 3")
assert(party.loot_scale() == 3, "configured loot scale " .. party.loot_scale())
local trio = total_items()
assert(trio > solo * 1.8,
       string.format("3 players: %d items over %d levels vs %d solo",
                     trio, LEVELS, solo))

-- Shops: each level's shops (from any vault) are topped up to one level's
-- worth per player, so with 3 players every level has a multiple of 3.
crawl.setopt("party_size = 3")
debug.reset_rng(11)
local with_shops = 0
for i = 1, 15 do
  debug.goto_place("Elf:1")
  debug.generate_level()
  local shops = 0
  for x = 1, 78 do
    for y = 1, 68 do
      if dgn.feature_name(dgn.grid(x, y)) == "enter_shop" then
        shops = shops + 1
      end
    end
  end
  assert(shops % 3 == 0,
         "3 players, Elf:1 level " .. i .. " has " .. shops .. " shops")
  if shops > 0 then
    with_shops = with_shops + 1
  end
end
assert(with_shops > 0, "no shops in 15 Elf:1 levels")

-- A party that exists counts too, whatever the option says.
crawl.setopt("party_size = 1")
debug.goto_place("D:1")
debug.generate_level()
you.moveto(10, 10)
party.add_player(12, 10)
assert(party.loot_scale() == 2, "2-player loot scale " .. party.loot_scale())
party.reset()
