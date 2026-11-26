-- Experience is shared: a kill by one player gives every living teammate
-- the same experience, and they level up on their own next turn.

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
local p2 = party.add_character(14, 10, "HuFi", "hand axe")
local p3 = party.add_player(16, 10)
party.switch_to(1)

local _, xp1 = party.xp(1)
local _, xp2 = party.xp(p2)
assert(xp1 == 0 and xp2 == 0, "someone starts with experience")

-- Player 1 kills a monster with the real attack command.
local m = dgn.create_monster(11, 10, "orc generate_awake")
assert(m, "couldn't place the orc")
m.set_hp(1)
for _ = 1, 20 do
  party.switch_to(1)
  turn("l")
  _, xp1 = party.xp(1)
  if xp1 > 0 then break end
end
_, xp2 = party.xp(p2)
local _, xp3 = party.xp(p3)
assert(xp1 > 0, "player 1 got no experience: " .. crawl.messages(4))
assert(xp2 == xp1, string.format("player 2 got %d experience, player 1 %d",
                                 xp2, xp1))
assert(xp3 == xp1, string.format("player 3 got %d experience, player 1 %d",
                                 xp3, xp1))

-- Enough to level: player 2 levels up on their own turn, not before.
party.switch_to(1)
local xl2_before = party.xp(p2)
assert(xl2_before == 1 and xp2 < 25, "player 2 already levelled")
-- To 25 total: XL 2 (needs 10), short of XL 3 (30) and its stat prompt.
you.gain_exp(25 - xp2)
assert(party.xp(p2) == xl2_before, "player 2 levelled while parked")
party.switch_to(p2)
turn("s")
assert(party.xp(p2) > xl2_before, "player 2 never levelled up: "
                                  .. crawl.messages(6))

-- A fallen player gains nothing.
party.reset()
you.moveto(10, 10)
p2 = party.add_player(12, 10)
party.switch_to(p2)
you.die()
party.switch_to(1)
assert(party.fallen(p2), "player 2 didn't fall")
_, xp2 = party.xp(p2)
you.gain_exp(50)
local _, after2 = party.xp(p2)
assert(after2 == xp2, "a fallen player gained experience")
party.reset()
