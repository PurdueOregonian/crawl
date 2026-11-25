# Co-op Crawl (multiplayer prototype)

This fork adds **hotseat co-op** to Dungeon Crawl Stone Soup: 2–4 players
explore the dungeon together, taking turns at one keyboard. LAN play is the
longer-term goal.

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
  appears when control passes. Once every player has spent a full turn,
  the monsters act.
- **Fast and slow actions.** A hasted player (or one with a quick weapon)
  keeps control until they've used up a whole turn. A slow action carries
  over into the next round.
- **Monsters** go after the nearest player they can see.
- **Moving into another player** swaps places with them, like with allies.
  Travel routes around teammates and stops with "Player 2 is in the way."
  rather than swapping. The player leading an autoexplore may swap past a
  follower (e.g. to get back out of a dead-end corridor).
- **Group autoexplore.** When a player presses `o`, the whole party explores
  together: that player picks the way, the others follow automatically (no
  keys pressed), and the leader waits for anyone more than 2 squares
  behind, so the group stays together. It stops
  for everyone when the leader finds something (an item, a monster, nothing
  left to explore), a follower spots danger or can't keep up, or someone
  plays by hand.
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
  is shut away.
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
5. Shared inventory
6. Per-player gods
7. More shops, items and monsters to match party size
8. LAN play through WebTiles

## For developers

The design lives in `crawl-ref/source/party.h`: N player structs, with the
acting one swapped into the global `you`. Tests (Windows, Debug Console
build):

```
pwsh crawl-ref/source/util/run-tests.ps1 party_baseline,party_two_players,party_hotseat
pwsh crawl-ref/source/util/party-smoke.ps1     # real game: stairs, group explore
```
