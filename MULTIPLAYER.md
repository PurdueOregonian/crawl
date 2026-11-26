# Co-op Crawl (multiplayer prototype)

This fork adds **hotseat co-op** to Dungeon Crawl Stone Soup: 2–4 players
explore the dungeon together, taking turns at one keyboard or each from
their own computer over a LAN (Tiles).

> Prototype status: early. Expect rough edges (see *Known limitations*).

## How to play

Add this to your init file (`init.txt` / `.crawlrc`):

```
party_size = 2        # 1-4; 1 = normal single-player Crawl
```

Then start a new game as usual. After you've made your character, each
other player picks their own species and background from the same menus
("Welcome, Player 2..."). Add `party_choose_characters = false` to skip
that and have them join as copies of you (Esc in their menus does the same
for that player).

- **Taking turns.** Players act one at a time. `--- Player 2's turn ---`
  appears when a player is asked for a command. (Turns that play themselves
  -- resting together, following an explore, finishing a multi-turn action
  like putting on armour -- go by without it.) Once every player has spent
  a full turn, the monsters act.
- **Fast and slow actions.** A hasted player (or one with a quick weapon)
  keeps control until they've used up a whole turn. A slow action carries
  over into the next round.
- **Monsters** go after the nearest player they can see.
- **Moving into another player** swaps places with them, like with allies.
  The player leading an autoexplore or a travel may swap past a follower
  (e.g. to get back out of a dead-end corridor).
- **Group autoexplore.** When a player presses `o`, the whole party explores
  together: that player picks the way, the others follow automatically (no
  keys pressed), and the leader waits for anyone more than 2 squares
  behind, so the group stays together. It stops
  for everyone when the leader finds something (an item, a monster, nothing
  left to explore), a follower spots danger or can't keep up, or someone
  plays by hand.
- **Group travel.** Travel (`G`, `Ctrl+G` to another floor, or travelling
  from the map) works the same way: everyone follows the player who set off,
  and at the stairs they wait until the whole party is beside them, so the
  party arrives on the new floor together and carries on.
- **Group rest.** Resting works the same way: one player rests, everyone
  rests, and it keeps going until *every* player has full HP and MP.
- **Safety is party-wide.** Nobody can start resting or exploring while any
  player can see an enemy, and if anyone spots one, it stops for everyone
  ("Player 2 can see a goblin; the party stops resting.").
- **Playing by hand stops everyone else's autopilot.** If you act manually,
  any teammate who is resting or travelling stops and gets the keyboard on
  their turn.
- **Stairs, shafts, portals and banishment** take the whole party along.
  To take stairs (or jump down a shaft), everyone must be next to you;
  while you climb, the others wait automatically. On arrival, teammates are
  placed beside you; if you arrive in a closet, its door is opened so nobody
  is shut away. This includes teammates who couldn't fall on their own
  (a formicid, mountain boots): when you fall down a shaft, they follow you
  down. If *they* step on a shaft trap and resist it, nobody falls.
- **Teleports are shared.** When one player starts a teleport, everyone
  counts down with them and lands beside them. A teammate who can't teleport
  (a formicid's stasis, a -Tele item, a dimension anchor, tree form) is left
  out: they stay where they are and the rest of the party still goes.
  Anchoring one player mid-countdown stops only that player's teleport. In
  the Abyss, a teleport always takes everyone, since it makes a new area.
  Teleport traps and the "malevolent force" teleport work the same way.
- **Ashenzari protects the party.** If any living teammate on the level has
  Ashenzari's trap warning (the same piety as in the base game), the
  malevolent force's hidden shafts, alarms and teleports are avoided for
  whoever it strikes, and Ashenzari says so.
- **Shared pack.** The party has one inventory. Potions, scrolls, wands and
  any gear nobody has on belong to everyone: whoever's turn it is can use
  them, and what one player picks up is there for the next. Each player's
  worn armour and wielded weapon stay their own. Items keep their letters
  where they can; if two players' things want the same letter, one moves.
  Identifying something identifies it for the whole party. (Gold isn't
  shared yet.)
- **Skill manuals teach everyone.** When one player picks up a manual, every
  player gets its bonus training. Spellbooks and parchments add their
  spells to everyone's library too (except djinn, who have none).
- **Falling and revival.** A player who dies while a teammate is still
  standing falls instead of ending the game: their body (`%`, or a faded
  doll in Tiles) stays where they fell and travels with the party. Stand on
  it and do nothing (wait or rest) for 50 turns to revive them, at full HP
  and MP. Doing anything else, or stepping off, starts the count over. The
  game ends only when the last player standing dies.
- **Talismans and forms** only affect the player who uses them.
- On the map, the active player is drawn as the normal `@`. Everyone else
  is a coloured `@` (player 1 white, player 2 cyan, player 3 yellow,
  player 4 magenta). The view and status panel follow whoever's turn it is.

## LAN play (Tiles)

Each player can play from their own computer, and sees their own character's
view. One computer hosts the game; the others join it.

1. **Host:** type a name and choose **Host LAN Game** on the start menu. The
   lobby shows this computer's address and who has joined. The party is
   everyone who joins, up to 4 players; `party_size` in the options is only
   for hotseat play and is ignored here. Enter starts the game, `+`/`-`
   add or remove an extra player who plays at the host's keyboard, and
   Escape stops hosting.
2. **Each other player:** choose **Join LAN Game** and enter the host's
   address (`127.0.0.1` for another copy of Crawl on the same computer).
3. The host makes player 1, then each joined player picks their own species
   and background on their own computer (keyboard or mouse). Only then is
   the dungeon made and everyone starts together.

From a command line: `crawl -lan-host [port]` hosts (port 31337 by default)
and `crawl -lan-join <address>[:port]` joins.

Windows may ask whether to let Crawl through the firewall on the host: allow
it for private networks. All computers need the same version of Crawl.

- Whoever's turn it is sees the game live, menus and prompts included, and
  only their keyboard and mouse do anything. Everyone else's window shows
  their own player's view, refreshed as the game goes on. Under the stats,
  a green **YOUR TURN** or a grey "Waiting: Player N's turn" says which.
- Only the host's computer runs the game (and saves it). If a player's
  computer disconnects, the host plays them from then on.
- The joining copy only shows pictures of the host's screen, scaled to fit
  its window. It doesn't need the save files or the host's options. When the
  host leaves, it goes back to the start menu.

## Known limitations

- **No saving yet.** Only the active player is saved. On reload, the
  others rejoin as fresh copies of that player.
- **Spells and area effects** (fireballs, clouds, beams) only affect the
  player whose turn it is, not other party members in range.
- **Some per-turn god effects** only apply to the player who is acting.

## Roadmap

1. ~~Two-player core (turn rounds, monster targeting)~~
2. ~~Hotseat play (input, turn handoff, drawing, level changes)~~
3. ~~Group autoexplore and rest~~
4. ~~Separate characters; revive a fallen player (50 turns on their body)~~
5. ~~Shared inventory~~ (gold still per player)
6. Per-player gods
7. More shops, items and monsters to match party size
8. ~~LAN play~~ (Tiles: host and thin clients; see *LAN play*)

## For developers

The design lives in `crawl-ref/source/party.h`: N player structs, with the
acting one swapped into the global `you`. Tests (Windows, Debug Console
build):

```
pwsh crawl-ref/source/util/run-tests.ps1 party_baseline,party_two_players,party_hotseat
pwsh crawl-ref/source/util/party-smoke.ps1     # real game: stairs, travel, explore...
pwsh crawl-ref/source/util/lan-smoke.ps1       # two Tiles copies over the network
```

Both take `-Exe <path>` to run a console build from somewhere else (all
configurations write the same `source/crawl.exe`, so a Tiles build there, or
a running game that locks it, would otherwise get in the way):

```
MSBuild crawl-ref/source/MSVC/crawl.vcxproj "-p:Configuration=Debug Console" -p:Platform=x64 -p:BuildProjectReferences=false -p:SolutionDir=<MSVC dir>\ -p:OutDir=<somewhere>\
pwsh crawl-ref/source/util/run-tests.ps1 party_shared_pack -Exe <somewhere>\crawl.exe
```
