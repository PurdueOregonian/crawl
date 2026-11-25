-- Party members with their own characters. Player 2 is created as a
-- different species/background (as when picked from the new-game menus) and
-- must be a genuinely separate character, while sharing the game: same
-- level, same clock, and player 1 left exactly as they were.

you.init("migl", "morningstar")
you.set_xl(5)
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()
you.moveto(10, 10)

local hp1_before, max1_before = you.hp()
local t0 = you.time()

-- Player 2: a Djinni Fire Elementalist.
local p2 = party.add_character(11, 10, "DjFE", "unarmed")
assert(party.size() == 2, "add_character did not grow the party")
assert(party.active() == 1, "adding a character changed whose turn it is")

-- Player 1 untouched.
assert(you.species() == "Minotaur", "player 1 is now a " .. you.species())
assert(you.class() == "Gladiator", "player 1 is now a " .. you.class())
local hp1, max1 = you.hp()
assert(hp1 == hp1_before and max1 == max1_before,
       string.format("player 1's hp changed: %d/%d -> %d/%d",
                     hp1_before, max1_before, hp1, max1))
assert(you.xl() == 5, "player 1's level changed")

-- Player 2 is their own character...
assert(party.species(p2) == "Djinni", "player 2 is a " .. party.species(p2))
party.switch_to(p2)
assert(you.species() == "Djinni", "player 2 (in you) is a " .. you.species())
assert(you.class() == "Fire Elementalist", "player 2 is a " .. you.class())
assert(you.xl() == 1, "player 2 should start at level 1, is " .. you.xl())
local hp2, max2 = you.hp()
assert(hp2 == max2 and max2 > 0, string.format("player 2 hp %d/%d", hp2, max2))
assert(max2 ~= max1, "player 2 has player 1's max hp")

-- ...but in the same game: same level, place and time.
assert(party.where(p2) == "D:1", "player 2 is on " .. party.where(p2))
local x2, y2 = party.pos(p2)
assert(x2 == 11 and y2 == 10, string.format("player 2 at (%d,%d)", x2, y2))
assert(you.time() == t0, "player 2's clock differs from the game's")
party.switch_to(1)

-- They can play: a full round with both.
party.spend(10)
assert(party.active() == p2, "turn did not pass to player 2")
party.spend(10)
assert(party.active() == 1, "turn did not return to player 1")
assert(you.time() == t0 + 10, "round did not advance time")
assert(party.species(1) == "Minotaur" and party.species(p2) == "Djinni",
       "species got mixed up across turns")

party.reset()
you.set_xl(1)
