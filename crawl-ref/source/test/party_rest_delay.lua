-- Resting while a teammate is busy with a multi-turn action (taking off
-- armour). The group rest runs by itself until everyone has healed, then
-- hands control back -- and its many automatic turns aren't each announced
-- ("--- Player N's turn ---" plus a full redraw), which made it crawl.

you.init("migl", "morningstar")
debug.reset_player_data()
debug.goto_place("D:1")
debug.generate_level()
debug.reset_rng(1)
dgn.dismiss_monsters()
dgn.fill_grd_area(5, 5, 40, 30, "floor")
debug.los_changed()
crawl.setopt("show_more = false")
crawl.setopt("travel_key_stop = false")

-- busy: who takes off armour; rester: who presses 5; hurt: whose HP drops.
local function scenario(name, busy, rester, hurt)
  party.reset()
  you.moveto(10, 10)
  local p2 = party.add_player(12, 10)
  party.switch_to(1)
  assert(p2 == 2)

  -- Get the turn order to `busy` first.
  if busy ~= 1 then
    crawl.sendkeys("s")
    debug.input_once()
  end
  assert(party.active() == busy, name .. ": not busy player's turn")
  if hurt then party.set_hp(hurt, 1) end

  local t0 = you.time()
  crawl.sendkeys("Tb")        -- take off the leather armour (several turns)
  debug.input_once()
  for _ = 1, 3 do
    if party.active() == rester then break end
    debug.input_once()
  end
  assert(party.active() == rester, name .. ": turn didn't pass to the rester: "
                                   .. crawl.messages(6))
  crawl.sendkeys("5")
  debug.input_once()
  -- Pressing 5 only starts the rest; its first turn makes them the leader.
  for _ = 1, 3 do
    if party.rest_leader() ~= 0 then break end
    debug.input_once()
  end
  assert(party.rest_leader() == rester, name .. ": group rest didn't start: "
                                        .. crawl.messages(6))

  -- Nobody should need to type anything until the rest is over. One ESC is
  -- queued for whoever is finally asked for a command (keys don't interrupt
  -- runs: travel_key_stop is off), so input_once doesn't block there.
  crawl.sendkeys(string.char(27))
  local steps = 0
  while party.rest_leader() ~= 0 and steps < 3000 do
    debug.input_once()
    steps = steps + 1
  end
  assert(steps < 3000, string.format(
    "%s: group rest never ended after %d steps (%d aut); messages: %s",
    name, steps, you.time() - t0, crawl.messages(10)))
  assert(party.hp(hurt) == party.hp(busy),   -- busy player is at full HP
         name .. ": rest ended before the hurt player healed")

  -- Automatic turns go by silently: only players actually asked for a
  -- command get "--- Player N's turn ---", not every handoff of the rest.
  -- (Before, each of the rest's ~140 handoffs was announced.)
  local _, announced = crawl.messages(30):gsub("'s turn %(HP", "")
  assert(announced <= 3, string.format(
    "%s: %d turn announcements in the last 30 messages of a %d-step rest",
    name, announced, steps))
  -- (Player 1 is left without armour; a further scenario would re-init.)
  party.reset()
end

scenario("p1 busy, p2 rests, p2 hurt", 1, 2, 2)
