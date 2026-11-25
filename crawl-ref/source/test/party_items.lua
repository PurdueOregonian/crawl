-- Items that benefit the whole party. A skill manual picked up by one player
-- teaches everyone (each gets the manual's bonus training points).

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
local p2 = party.add_player(11, 10)
party.switch_to(1)

assert(party.manual_points(1, "Fighting") == 0, "player 1 starts with manual points")
assert(party.manual_points(p2, "Fighting") == 0, "player 2 starts with manual points")

-- Player 1 picks up a manual of Fighting through the real input loop.
dgn.create_item(10, 10, "manual of fighting")
crawl.sendkeys(",")
debug.input_once()

local m1 = party.manual_points(1, "Fighting")
local m2 = party.manual_points(p2, "Fighting")
assert(m1 > 0, "player 1 didn't get the manual (messages: "
               .. crawl.messages(4) .. ")")
assert(m2 == m1, string.format("player 2 got %d manual points, player 1 %d",
                               m2, m1))
assert(crawl.messages(4):find("Player 2 studies it too"),
       "no message that player 2 studies it too: " .. crawl.messages(4))

party.reset()
dgn.dismiss_monsters()
