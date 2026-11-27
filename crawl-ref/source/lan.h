/**
 * @file
 * @brief LAN play for the multiplayer party: one computer hosts the game,
 *        the other players join with a thin client.
 *
 * The host (start menu "Host LAN Game", or `crawl -lan-host [port]`) runs
 * the one and only game, with a party of `party_size` players (at least
 * 2). Player 1 plays at the host; each player after that is a "seat"
 * played from another computer ("Join LAN Game", or
 * `crawl -lan-join <host>[:port]`). A client is just a window: it shows
 * the frames the host sends it and sends back its keyboard and mouse input.
 *
 * Every seat sees its own player. Whoever's turn it is gets the live screen
 * (menus, prompts and all) and their input is used; everyone else's window
 * is refreshed between actions with their own player's view (the host swaps
 * that player into `you`, draws the screen, and sends or shows the frame).
 *
 * Local tiles only: frames are read back from the GL window.
 */

#pragma once

#include <cstdint>
#include <string>

using std::string;

const int LAN_DEFAULT_PORT = 31337;

// Command line.
void lan_request_host(int port);
void lan_request_join(const string &address);   // host[:port]
bool lan_host_requested();
bool lan_join_requested();
// Client test helpers: keys to type on our turns, and where to save the
// frames we're shown (as BMP).
void lan_set_client_script(const string &keys);
void lan_set_client_dump(const string &path);

// Start menu "Host LAN Game": host on `port` and start listening right
// away, so other computers can connect while player 1 is being made.
// False (with the reason in `err`) if that's impossible.
bool lan_host_start(int port, string &err);
// This computer's addresses, for telling the other players ("" if unknown).
string lan_host_addresses();
// Before a hosted game starts: show who's joined, taking each computer that
// connects as the next player, until the host starts the game. The party is
// then player 1, everyone who joined, and any extra players the host added
// at this computer (see lan_lobby_party_size()). False if the host gave up
// hosting.
bool lan_host_lobby();
// The party size the lobby settled on, or 0 if no lobby was held.
int lan_lobby_party_size();
// Continuing a saved party game: wait for the computers of the players who
// had their own (see party.h) to join again, carrying on once they all have
// or when the host says so; any still missing are then played at the host.
// A joiner with a copy of this game gets the player it says they were;
// anyone else gets the next player nobody has. The host may also give up
// (stop), or delete the game instead (then the caller deletes it and calls
// lan_host_end_game(), which still has everyone who joined).
struct party_save_info;
enum class lan_resume { play, stop, delete_game };
lan_resume lan_host_resume_lobby(const party_save_info &info);
// Stop hosting: drop everyone and stop listening.
void lan_host_stop();
// Saving and quitting: party member `idx`'s copy of the save (see party.h),
// with them as its main player; lan_host_end_game() sends it to them.
void lan_host_add_copy(int idx, const string &filename, const string &data);
// The hosted game is over (saved, lost, quit...): tell every remote player
// why, which sends them back to their start menu, and stop hosting. Each
// gets their copy (if one was added), or with `delete_copies` (it's over for
// good) deletes their copies of game `game_id`.
void lan_host_end_game(const string &why, const string &game_id,
                       bool delete_copies = false);
// The name the player of party member `idx` gave at their own computer, or
// "" (not remote, or none given).
string lan_player_name(int idx);

// Start menu "Join LAN Game": ask for the host's address, then join.
// `name`: the name the player typed in the start menu (their character's
// name, if the game is new), or "".
void lan_join_menu(const string &name);

// Be a window onto the game hosted at `address` (host[:port]) until the
// host goes away, the player cancels, or the window is closed. Runs in the
// game window, so it needs tiles.initialise() first. `name` as above.
void lan_run_client(const string &address, const string &name);
// The address given with -lan-join.
const string &lan_join_address();

// Host: wait for the computer that plays party member `idx` (index from 0;
// player 1 is local, so idx >= 1) to connect. False if the host gave up
// waiting (that player is then played at the host, as in hotseat).
bool lan_host_wait_for_player(int idx);

// Character choice, all at once: after the lobby, each remote player picks
// their character at their own computer while player 1 does at the host.
// Begin: ask every remote seat to choose.
void lan_host_begin_choosing();
// Player 1 is done; wait (showing who's still choosing) until every remote
// player who was asked has answered, or left, or the host stops waiting.
void lan_host_wait_for_choices();
// Remote party member `idx`'s answer. False if there's none (they left, or
// the host stopped waiting); otherwise `chosen` is false if they backed
// out, else their species, job and weapon.
bool lan_host_choice(int idx, bool &chosen, int &species, int &job,
                     int &weapon);

// Is any remote seat connected?
bool lan_active();

// Seat that plays party member `idx`: 0 for the host's own screen, k > 0
// for the k-th remote computer.
int lan_seat_of_player(int idx);

// The seat that gets frames and input right now.
int lan_output_seat();

// While alive, frames and input go to `seat` (e.g. a joining player's
// character choice, or drawing an idle player's view).
class lan_seat_scope
{
public:
    explicit lan_seat_scope(int seat);
    ~lan_seat_scope();
private:
    int prev;
};

// Draw each idle seat's own view and send/show it. `force`: even if one
// was just drawn (otherwise at most a few times a second). Call only
// between actions: it swaps other players into `you`.
void lan_refresh_idle_views(bool force);

#ifdef USE_TILE_LOCAL
struct SDL_Window;
union SDL_Event;

// The SDL window wrapper's hooks.
void lan_set_window(SDL_Window *window);
// A frame has been drawn into the back buffer. True if it was for a remote
// seat (and has been sent); then it mustn't be shown locally.
bool lan_present();
// Replacement for SDL_WaitEventTimeout(): input comes from the output seat.
bool lan_wait_event(SDL_Event &event, int timeout);
// Is the output seat remote? (Then local input is ignored.)
bool lan_routing_remote();
// Does the output seat have a queued event of this SDL type?
bool lan_peek_remote(uint32_t sdl_type);
// If the output seat is remote: its mouse position (window coordinates),
// SDL button mask and SDL modifier keys, as of its last input. The UI must
// use these, not this computer's mouse and keyboard.
bool lan_remote_input(int &x, int &y, uint32_t &buttons, uint16_t &mod);
#endif
