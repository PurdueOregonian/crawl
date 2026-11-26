-- Runes are shared: one picked up by anyone counts for the whole party.

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
assert(you.num_runes() == 0, "player 1 starts with runes")

-- Player 1 picks up a rune.
dgn.create_item(10, 10, "decaying rune of Zot")
turn(",")
party.switch_to(1)
assert(you.have_rune("decaying"), "player 1 didn't get the rune: "
       .. crawl.messages(4))

party.switch_to(p2)
assert(you.have_rune("decaying"), "player 2 doesn't share player 1's rune")
assert(you.num_runes() == 1, "player 2 has " .. you.num_runes() .. " runes")

-- Player 2 picks up another; both runes are everyone's.
you.moveto(12, 10)
dgn.create_item(12, 10, "serpentine rune of Zot")
turn(",")
party.switch_to(p2)
assert(you.have_rune("serpentine"), "player 2 didn't get the rune: "
       .. crawl.messages(4))
party.switch_to(1)
assert(you.num_runes() == 2, "player 1 has " .. you.num_runes() .. " runes")
assert(you.have_rune("serpentine"), "player 1 doesn't share player 2's rune")
party.switch_to(p2)
assert(you.num_runes() == 2, "player 2 has " .. you.num_runes() .. " runes")
party.reset()
