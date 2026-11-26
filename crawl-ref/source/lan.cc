/**
 * @file
 * @brief LAN play: the host streams each remote seat its own view and takes
 *        that seat's input on its player's turns. See lan.h.
 *
 * Wire format (TCP, little-endian): each message is a 1-byte type, a 4-byte
 * payload length, then the payload.
 *   client -> host  'H' hello: the client's version string
 *                   'E' an input event (struct net_event)
 *   host -> client  'W' welcome: u32 seat, u32 player number (1-based)
 *                   'F' frame: u32 w, u32 h, u8 keyframe, zlib data of
 *                       w*h RGB bytes, top row first; a non-keyframe is
 *                       XORed with the previous frame
 *                   'S' status: u8 1 if it's this seat's turn
 *                   'R' refused: a message
 */

#include "AppHdr.h"

#include "lan.h"

#include <cerrno>
#include <cstring>
#include <deque>
#include <vector>

#include "cio.h"
#include "libutil.h"
#include "message.h"
#include "options.h"
#include "output.h"
#include "party.h"
#include "state.h"
#include "ui.h"
#include "stringutil.h"
#include "version.h"
#include "viewgeom.h"
#ifdef USE_TILE_LOCAL
# include "tilesdl.h"
# include "windowmanager.h"
# include <zlib.h>
# ifdef TARGET_COMPILER_VC
#  include <SDL.h>
# else
#  include <SDL2/SDL.h>
# endif
# include <SDL_opengl.h>
#endif

#ifdef TARGET_OS_WINDOWS
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
static const sock_t BAD_SOCK = INVALID_SOCKET;
static void _close_sock(sock_t s) { closesocket(s); }
#else
# include <arpa/inet.h>
# include <fcntl.h>
# include <netdb.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>
typedef int sock_t;
static const sock_t BAD_SOCK = -1;
static void _close_sock(sock_t s) { close(s); }
#endif

using std::deque;
using std::vector;

namespace
{
    int host_port = -1;           // -lan-host
    string join_address;          // -lan-join
    string client_script;
    string client_dump;

    bool _net_init()
    {
#ifdef TARGET_OS_WINDOWS
        static bool done = false;
        if (!done)
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                return false;
            done = true;
        }
#endif
        return true;
    }

    void _no_delay(sock_t s)
    {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char *>(&one), sizeof(one));
    }

    // Wait up to `ms` for `s` to be readable.
    bool _readable(sock_t s, int ms)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return select((int)s + 1, &fds, nullptr, nullptr, &tv) > 0;
    }

    void _put32(vector<uint8_t> &b, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b.push_back((v >> (8 * i)) & 0xff);
    }

    uint32_t _get32(const uint8_t *p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    void _set_blocking(sock_t s, bool blocking)
    {
#ifdef TARGET_OS_WINDOWS
        u_long nb = blocking ? 0 : 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
    }

    // Did the last socket call fail only because it would have blocked?
    bool _would_block()
    {
#ifdef TARGET_OS_WINDOWS
        return WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    // One TCP connection, with message framing. The socket is non-blocking:
    // what can't be sent yet waits in `out` and goes out on later flush()es.
    // (Blocking sends deadlock: each side stuck sending, neither reading.)
    struct connection
    {
        sock_t s = BAD_SOCK;
        vector<uint8_t> in;
        vector<uint8_t> out;
        size_t out_pos = 0;             // how much of `out` has been sent

        bool ok() const { return s != BAD_SOCK; }

        // Set up a newly connected socket.
        void adopt(sock_t sock)
        {
            s = sock;
            _no_delay(s);
            _set_blocking(s, false);
        }

        void close()
        {
            if (ok())
                _close_sock(s);
            s = BAD_SOCK;
            in.clear();
            out.clear();
            out_pos = 0;
        }

        // Is there anything still waiting to be sent?
        bool backed_up() const { return out_pos < out.size(); }

        // Send as much of `out` as the socket takes now.
        bool flush()
        {
            while (ok() && backed_up())
            {
                const int sent = ::send(s,
                    reinterpret_cast<const char *>(&out[out_pos]),
                    (int)min<size_t>(out.size() - out_pos, 1 << 20), 0);
                if (sent < 0 && _would_block())
                    break;
                if (sent <= 0)
                {
                    close();
                    return false;
                }
                out_pos += sent;
            }
            if (!backed_up())
            {
                out.clear();
                out_pos = 0;
            }
            return ok();
        }

        bool send_msg(uint8_t type, const vector<uint8_t> &payload)
        {
            if (!ok())
                return false;
            out.push_back(type);
            _put32(out, payload.size());
            out.insert(out.end(), payload.begin(), payload.end());
            return flush();
        }

        // Read whatever has arrived (waiting at most `ms` for the first
        // bytes). False if the connection is gone.
        bool fill(int ms)
        {
            while (ok() && _readable(s, ms))
            {
                uint8_t buf[65536];
                const int got = ::recv(s, reinterpret_cast<char *>(buf),
                                       sizeof(buf), 0);
                if (got < 0 && _would_block())
                    break;
                if (got <= 0)
                {
                    close();
                    return false;
                }
                in.insert(in.end(), buf, buf + got);
                ms = 0;
            }
            return ok();
        }

        // Take the next complete message, if there is one.
        bool next(uint8_t &type, vector<uint8_t> &payload)
        {
            if (in.size() < 5)
                return false;
            const uint32_t len = _get32(&in[1]);
            if (in.size() < 5 + (size_t)len)
                return false;
            type = in[0];
            payload.assign(in.begin() + 5, in.begin() + 5 + len);
            in.erase(in.begin(), in.begin() + 5 + len);
            return true;
        }
    };

    // An input event on the wire. Mouse positions are in frame pixels.
#pragma pack(push, 1)
    struct net_event
    {
        uint32_t type;
        int32_t sym;
        int32_t scancode;
        uint16_t mod;
        uint8_t repeat;
        uint8_t pressed;
        int32_t x, y;
        uint32_t buttons;       // motion: button state; button: which
        uint8_t clicks;
        int32_t wheel_x, wheel_y;
        char text[32];
    };
#pragma pack(pop)
}

void lan_request_host(int port)
{
    host_port = port > 0 ? port : LAN_DEFAULT_PORT;
}

void lan_request_join(const string &address)
{
    join_address = address;
}

bool lan_host_requested()
{
    return host_port > 0;
}

bool lan_join_requested()
{
    return !join_address.empty();
}

const string &lan_join_address()
{
    return join_address;
}

void lan_set_client_script(const string &keys)
{
    client_script = keys;
}

void lan_set_client_dump(const string &path)
{
    client_dump = path;
}


#ifndef USE_TILE_LOCAL

// LAN play needs local tiles: frames are read back from the GL window.
void lan_run_client(const string &/*address*/)
{
    fprintf(stderr, "LAN play needs the Tiles build of Crawl.\n");
}

void lan_join_menu()
{
    mprf(MSGCH_ERROR, "LAN play needs the Tiles build of Crawl.");
}

bool lan_host_start(int /*port*/, string &err)
{
    err = "LAN play needs the Tiles build of Crawl.";
    return false;
}

string lan_host_addresses()
{
    return "";
}

bool lan_host_wait_for_player(int /*idx*/)
{
    mprf(MSGCH_ERROR, "LAN play needs the Tiles build of Crawl.");
    host_port = -1;
    return false;
}

bool lan_host_lobby(int /*players*/) { return true; }
void lan_host_stop() { host_port = -1; }
bool lan_active() { return false; }
int lan_seat_of_player(int /*idx*/) { return 0; }
int lan_output_seat() { return 0; }
lan_seat_scope::lan_seat_scope(int seat) : prev(seat) {}
lan_seat_scope::~lan_seat_scope() {}
void lan_refresh_idle_views(bool /*force*/) {}

#else // USE_TILE_LOCAL

namespace
{
    // ---------------------------------------------------------------- host

    struct seat
    {
        connection conn;
        int player = -1;                // party index this seat plays
        deque<net_event> events;        // its input, while it's its turn
        vector<uint8_t> last_frame;     // what the client is showing
        int last_w = 0, last_h = 0;
        int told_turn = -1;             // last 'S' sent
        // The newest frame, drawn while the last one was still on its way:
        // sent once that's gone, so a slow link skips frames, not input.
        vector<uint8_t> pending;
        int pending_w = 0, pending_h = 0;
        // Its mouse and modifier keys as of the last input event taken from
        // it: the UI asks for "the" mouse position and button state, which
        // on its turn must be this player's, not this computer's.
        int mouse_x = 0, mouse_y = 0;
        uint32_t mouse_buttons = 0;
        uint16_t mod = 0;
    };

    sock_t listener = BAD_SOCK;
    // seats[0] is the host's own screen and is never connected.
    vector<seat> seats(1);
    int seat_override = -1;
    SDL_Window *window = nullptr;
    unsigned int last_idle_refresh = 0;

    bool _listen()
    {
        if (listener != BAD_SOCK)
            return true;
        if (!_net_init())
            return false;
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == BAD_SOCK)
            return false;
        int one = 1;
#ifdef TARGET_OS_WINDOWS
        // Windows' SO_REUSEADDR would let a second host share the port (and
        // steal its players); make the port ours alone.
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&one), sizeof(one));
#else
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&one), sizeof(one));
#endif
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(host_port);
        if (::bind(listener, reinterpret_cast<sockaddr *>(&addr),
                   sizeof(addr)) != 0
            || ::listen(listener, 4) != 0)
        {
            _close_sock(listener);
            listener = BAD_SOCK;
            return false;
        }
        return true;
    }

    // This computer's IPv4 addresses, to tell the other players.
    vector<string> _local_address_list()
    {
        vector<string> found;
        char name[256];
        if (gethostname(name, sizeof(name)) != 0)
            return found;
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        addrinfo *res = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &res) != 0)
            return found;
        for (addrinfo *a = res; a; a = a->ai_next)
        {
            const auto *sin = reinterpret_cast<sockaddr_in *>(a->ai_addr);
            const auto *b = reinterpret_cast<const uint8_t *>(&sin->sin_addr);
            found.push_back(make_stringf("%d.%d.%d.%d", b[0], b[1], b[2],
                                         b[3]));
        }
        freeaddrinfo(res);
        return found;
    }

    string _local_addresses()
    {
        const vector<string> found = _local_address_list();
        return comma_separated_line(found.begin(), found.end(), " or ", ", ");
    }

    void _drop_seat(int k, const char *why)
    {
        seat &s = seats[k];
        if (!s.conn.ok())
            return;
        s.conn.close();
        s.events.clear();
        mprf(MSGCH_WARN, "%s's computer %s; they're played from here now.",
             party_member_name(s.player).c_str(), why);
    }

    int _live_seat()
    {
        if (seat_override >= 0)
            return seat_override;
        return party_active() ? lan_seat_of_player(party_active_index()) : 0;
    }

    void _send_frame(seat &s, int w, int h, vector<uint8_t> &px);

    // Read the remote seats' input: the output seat's is queued, anyone
    // else's (typing while it isn't their turn) is dropped. Also sends
    // whatever was waiting to go out.
    void _poll()
    {
        const int live = _live_seat();
        for (int k = 1; k < (int)seats.size(); ++k)
        {
            seat &s = seats[k];
            if (!s.conn.ok())
                continue;
            if (!s.conn.fill(0) || !s.conn.flush())
            {
                _drop_seat(k, "disconnected");
                continue;
            }
            if (!s.conn.backed_up() && !s.pending.empty())
            {
                vector<uint8_t> px;
                px.swap(s.pending);
                _send_frame(s, s.pending_w, s.pending_h, px);
            }
            uint8_t type;
            vector<uint8_t> payload;
            while (s.conn.next(type, payload))
            {
                if (type == 'E' && payload.size() == sizeof(net_event)
                    && k == live)
                {
                    net_event e;
                    memcpy(&e, payload.data(), sizeof(e));
                    // Only where the mouse ended up matters; handling every
                    // step of a long movement takes the host ages.
                    if (e.type == SDL_MOUSEMOTION && !s.events.empty()
                        && s.events.back().type == SDL_MOUSEMOTION)
                    {
                        s.events.back() = e;
                    }
                    else
                        s.events.push_back(e);
                }
            }
            const int turn = k == live;
            if (turn != s.told_turn)
            {
                s.told_turn = turn;
                s.conn.send_msg('S', { (uint8_t)turn });
            }
        }
    }

    // Frame pixels -> this window's coordinates (they differ with hidpi).
    void _scale_to_window(int &x, int &y)
    {
        int ww, wh, dw, dh;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        if (dw > 0 && dh > 0)
        {
            x = x * ww / dw;
            y = y * wh / dh;
        }
    }

    SDL_Event _to_sdl(const net_event &e)
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = e.type;
        int x = e.x, y = e.y;
        switch (e.type)
        {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            ev.key.state = e.pressed ? SDL_PRESSED : SDL_RELEASED;
            ev.key.repeat = e.repeat;
            ev.key.keysym.sym = e.sym;
            ev.key.keysym.scancode = (SDL_Scancode)e.scancode;
            ev.key.keysym.mod = e.mod;
            break;
        case SDL_TEXTINPUT:
            static_assert(sizeof(e.text) <= sizeof(ev.text.text), "text");
            memcpy(ev.text.text, e.text, sizeof(e.text));
            ev.text.text[sizeof(e.text) - 1] = 0;
            break;
        case SDL_MOUSEMOTION:
            _scale_to_window(x, y);
            ev.motion.x = x;
            ev.motion.y = y;
            ev.motion.state = e.buttons;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            _scale_to_window(x, y);
            ev.button.x = x;
            ev.button.y = y;
            ev.button.button = e.buttons;
            ev.button.clicks = e.clicks;
            ev.button.state = e.pressed ? SDL_PRESSED : SDL_RELEASED;
            break;
        case SDL_MOUSEWHEEL:
            ev.wheel.x = e.wheel_x;
            ev.wheel.y = e.wheel_y;
            break;
        }
        return ev;
    }

    bool _is_input(const SDL_Event &ev)
    {
        switch (ev.type)
        {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
        case SDL_TEXTEDITING: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
            return true;
        default:
            return false;
        }
    }

    void _send_frame(seat &s, int w, int h, vector<uint8_t> &px)
    {
        // Still sending the last one: keep only the newest (see `pending`).
        if (s.conn.backed_up())
        {
            s.pending.swap(px);
            s.pending_w = w;
            s.pending_h = h;
            return;
        }
        s.pending.clear();
        const bool key = w != s.last_w || h != s.last_h;
        vector<uint8_t> delta(px.size());
        if (key)
            delta = px;
        else
            for (size_t i = 0; i < px.size(); ++i)
                delta[i] = px[i] ^ s.last_frame[i];

        uLongf zlen = compressBound(delta.size());
        vector<uint8_t> payload(9 + zlen);
        if (compress2(&payload[9], &zlen, delta.data(), delta.size(), 1)
            != Z_OK)
        {
            return;
        }
        payload.resize(9 + zlen);
        for (int i = 0; i < 4; ++i)
        {
            payload[i] = (w >> (8 * i)) & 0xff;
            payload[4 + i] = (h >> (8 * i)) & 0xff;
        }
        payload[8] = key;
        if (s.conn.send_msg('F', payload))
        {
            s.last_frame.swap(px);
            s.last_w = w;
            s.last_h = h;
        }
    }
}

void lan_set_window(SDL_Window *w)
{
    window = w;
}

namespace
{
    // Take a computer waiting to connect, if one turns up within `ms`, as
    // the seat for party member `idx`. True if it joined.
    bool _accept_seat(int idx, int ms)
    {
        if (!_readable(listener, ms))
            return false;
        seat s;
        const sock_t sock = accept(listener, nullptr, nullptr);
        if (sock == BAD_SOCK)
            return false;
        s.conn.adopt(sock);
        // Hello: the client must be the same version as us.
        uint8_t type = 0;
        vector<uint8_t> payload;
        for (int tries = 0; tries < 50 && !s.conn.next(type, payload);
             ++tries)
        {
            s.conn.fill(100);
        }
        const string theirs(payload.begin(), payload.end());
        if (type != 'H' || theirs != Version::Long)
        {
            const string why = make_stringf(
                "Version mismatch: the host runs %s, you run %s.",
                Version::Long, theirs.c_str());
            s.conn.send_msg('R', vector<uint8_t>(why.begin(), why.end()));
            s.conn.close();
            mprf(MSGCH_WARN, "A computer tried to join with a different "
                 "version of Crawl (%s).", theirs.c_str());
            return false;
        }
        s.player = idx;
        vector<uint8_t> welcome;
        _put32(welcome, seats.size());
        _put32(welcome, idx + 1);
        s.conn.send_msg('W', welcome);
        seats.push_back(std::move(s));
        return true;
    }

    // How to join, a short line at a time (the lobby is a narrow popup).
    string _join_instructions()
    {
        const string port = host_port == LAN_DEFAULT_PORT
                            ? "" : make_stringf(":%d", host_port);
        string how = "On the other computers, choose Join LAN Game\n"
                     "and enter this computer's address";
        const vector<string> where = _local_address_list();
        if (where.empty())
            how += port.empty() ? "." : " with " + port + ".";
        else
        {
            how += where.size() > 1 ? ", one of:\n" : ":\n";
            for (const string &a : where)
                how += "    " + a + port + "\n";
        }
        return how + "(Another copy of Crawl on this computer: 127.0.0.1"
               + port + ")";
    }
}

bool lan_host_lobby(int players)
{
    if (!lan_host_requested())
        return true;
    if (!_listen())
    {
        mprf(MSGCH_ERROR, "Can't host on port %d (is it already in use?).",
             host_port);
        host_port = -1;
        return true;
    }

    bool cancel = false, start_now = false;
    auto text = make_shared<ui::Text>();
    text->set_margin_for_sdl(10);
    auto popup = make_shared<ui::Popup>(text);
    popup->on_keydown_event([&](const ui::KeyEvent& ev) {
        const int key = ev.key();
        if (key == CK_ENTER || key == '\r' || key == '\n')
            start_now = true;
        else if (ui::key_exits_popup(key, false))
            cancel = true;
        return true;
    });
    ui::push_layout(popup);

    const string how = _join_instructions();
    while (!cancel && !start_now && !crawl_state.seen_hups)
    {
        // Forget anyone who left again.
        for (int k = (int)seats.size() - 1; k >= 1; --k)
            if (!seats[k].conn.fill(0) || !seats[k].conn.flush())
                seats.erase(seats.begin() + k);

        int missing = 0;
        string list = "<white>Player 1</white>: you\n";
        int first_free = -1;
        for (int idx = 1; idx < players; ++idx)
        {
            const bool in = lan_seat_of_player(idx) > 0;
            if (!in)
            {
                ++missing;
                if (first_free < 0)
                    first_free = idx;
            }
            list += make_stringf("<white>Player %d</white>: %s\n", idx + 1,
                                 in ? "<lightgreen>joined</lightgreen>"
                                    : "<yellow>waiting...</yellow>");
        }
        if (!missing)
            break;

        text->set_text(formatted_string::parse_string(
            "<cyan>Hosting a LAN game for " + to_string(players)
            + " players.</cyan>\n\n" + how + "\n\n" + list
            + "\nThe game starts once everyone has joined.\n\n"
              "<lightgrey>[Enter] start now (missing players play here)\n"
              "[Esc] stop hosting</lightgrey>"));
        ui::pump_events(50);
        _accept_seat(first_free, 50);
    }
    ui::pop_layout();

    if (cancel || crawl_state.seen_hups)
    {
        lan_host_stop();
        return false;
    }
    return true;
}

void lan_host_stop()
{
    for (int k = 1; k < (int)seats.size(); ++k)
        seats[k].conn.close();
    seats.resize(1);
    if (listener != BAD_SOCK)
        _close_sock(listener);
    listener = BAD_SOCK;
    host_port = -1;
}

bool lan_host_wait_for_player(int idx)
{
    ASSERT(idx >= 1);
    if (!lan_host_requested())
        return false;
    // Already here (from the lobby)?
    if (lan_seat_of_player(idx) > 0)
        return true;
    const string name = party_member_name(idx);
    if (!_listen())
    {
        mprf(MSGCH_ERROR, "Can't host on port %d (is it already in use?); "
             "%s plays here instead.", host_port, name.c_str());
        host_port = -1;
        return false;
    }

    mprf(MSGCH_PROMPT, "Waiting for %s to join.", name.c_str());
    mprf(MSGCH_PROMPT, "%s", _join_instructions().c_str());
    mprf(MSGCH_PROMPT, "(Press Escape to play %s here instead.)",
         name.c_str());

    while (true)
    {
        redraw_screen();
        update_screen();
        tiles.redraw();
        if (_accept_seat(idx, 100))
        {
            mprf(MSGCH_PROMPT, "%s has joined.", name.c_str());
            return true;
        }
        if (kbhit() && getch_ck() == ESCAPE)
        {
            mprf("%s will play here.", name.c_str());
            return false;
        }
    }
}

bool lan_host_start(int port, string &err)
{
    lan_request_host(port);
    if (_listen())
        return true;
    err = make_stringf("Can't host on port %d. Is another copy of Crawl "
                       "already hosting?", host_port);
    host_port = -1;
    return false;
}

string lan_host_addresses()
{
    return _local_addresses();
}

bool lan_active()
{
    for (int k = 1; k < (int)seats.size(); ++k)
        if (seats[k].conn.ok())
            return true;
    return false;
}

int lan_seat_of_player(int idx)
{
    for (int k = 1; k < (int)seats.size(); ++k)
        if (seats[k].player == idx && seats[k].conn.ok())
            return k;
    return 0;
}

int lan_output_seat()
{
    return _live_seat();
}

lan_seat_scope::lan_seat_scope(int seat) : prev(seat_override)
{
    seat_override = seat;
}

lan_seat_scope::~lan_seat_scope()
{
    seat_override = prev;
}

bool lan_routing_remote()
{
    return _live_seat() > 0;
}

bool lan_remote_input(int &x, int &y, uint32_t &buttons, uint16_t &mod)
{
    const int k = _live_seat();
    if (k <= 0 || !seats[k].conn.ok())
        return false;
    x = seats[k].mouse_x;
    y = seats[k].mouse_y;
    buttons = seats[k].mouse_buttons;
    mod = seats[k].mod;
    return true;
}

bool lan_present()
{
    const int k = _live_seat();
    if (k <= 0 || !window)
        return false;
    seat &s = seats[k];
    if (!s.conn.ok())
        return false;

    int w, h;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0 || h <= 0)
        return true;
    vector<uint8_t> raw((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
    // GL's rows go bottom-up.
    vector<uint8_t> px(raw.size());
    const size_t row = (size_t)w * 3;
    for (int y = 0; y < h; ++y)
        memcpy(&px[y * row], &raw[(h - 1 - y) * row], row);
    _send_frame(s, w, h, px);
    if (!s.conn.ok())
        _drop_seat(k, "disconnected");
    _poll();
    return true;
}

bool lan_peek_remote(uint32_t sdl_type)
{
    _poll();
    const int k = _live_seat();
    if (k <= 0)
        return false;
    for (const net_event &e : seats[k].events)
        if (e.type == sdl_type)
            return true;
    return false;
}

bool lan_wait_event(SDL_Event &event, int timeout)
{
    if (!lan_active())
        return SDL_WaitEventTimeout(&event, timeout);

    const unsigned int start = SDL_GetTicks();
    while (true)
    {
        _poll();
        const int k = _live_seat();
        if (k > 0 && !seats[k].events.empty())
        {
            seat &st = seats[k];
            event = _to_sdl(st.events.front());
            st.events.pop_front();
            switch (event.type)
            {
            case SDL_MOUSEMOTION:
                st.mouse_x = event.motion.x;
                st.mouse_y = event.motion.y;
                st.mouse_buttons = event.motion.state;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                st.mouse_x = event.button.x;
                st.mouse_y = event.button.y;
                if (event.type == SDL_MOUSEBUTTONDOWN)
                    st.mouse_buttons |= SDL_BUTTON(event.button.button);
                else
                    st.mouse_buttons &= ~SDL_BUTTON(event.button.button);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                st.mod = event.key.keysym.mod;
                break;
            }
            return true;
        }
        if (SDL_WaitEventTimeout(&event, 10))
        {
            // Someone else's turn: this keyboard and mouse do nothing (the
            // window itself still works: focus, resizing, closing).
            if (k > 0 && _is_input(event))
                continue;
            return true;
        }
        if (timeout != INT_MAX && (int)(SDL_GetTicks() - start) >= timeout)
            return false;
    }
}

void lan_refresh_idle_views(bool force)
{
    if (!lan_active() || !party_active() || seat_override >= 0
        || !crawl_state.need_save)
    {
        return;
    }
    const unsigned int now = SDL_GetTicks();
    if (!force && now - last_idle_refresh < 250)
        return;
    last_idle_refresh = now;

    const int live = _live_seat();
    bool drew = false;
    for (int k = 0; k < (int)seats.size(); ++k)
    {
        if (k == live || k > 0 && !seats[k].conn.ok())
            continue;
        // The host's own screen shows player 1 (and any other players
        // played here, when it's their turn).
        const int idx = k == 0 ? 0 : seats[k].player;
        party_view_as(idx, [k] {
            lan_seat_scope scope(k);
            redraw_screen();
            tiles.redraw();
        });
        drew = true;
    }
    if (drew)
    {
        // Back to the live player's view (the regions hold the last one
        // drawn).
        redraw_screen();
        update_screen();
    }
}

// ------------------------------------------------------------------ client
//
// The client runs in the game's own window (made by tiles.initialise()):
// while it connects it shows a status popup through the normal UI, then it
// takes the window over, drawing the host's frames with GL and sending the
// window's input to the host.

namespace
{
    string last_join_address = "127.0.0.1";

    void _split_address(const string &address, string &host, int &port)
    {
        host = address;
        port = LAN_DEFAULT_PORT;
        const size_t colon = address.rfind(':');
        if (colon != string::npos)
        {
            host = address.substr(0, colon);
            port = atoi(address.c_str() + colon + 1);
        }
        trim_string(host);
    }

    // Try to connect, waiting at most `ms` (so the window stays alive when
    // the address doesn't answer at all).
    bool _connect(connection &c, const string &address, int ms, string &err)
    {
        string host;
        int port;
        _split_address(address, host, port);
        if (host.empty() || port <= 0)
        {
            err = "bad address " + address;
            return false;
        }
        if (!_net_init())
        {
            err = "can't start networking";
            return false;
        }
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res)
            != 0 || !res)
        {
            err = "unknown host " + host;
            return false;
        }
        const sock_t sock = socket(res->ai_family, res->ai_socktype,
                                   res->ai_protocol);
        bool ok = sock != BAD_SOCK;
        if (ok)
        {
            c.adopt(sock);
            if (::connect(c.s, res->ai_addr, (int)res->ai_addrlen) != 0)
            {
                fd_set wfds, efds;
                FD_ZERO(&wfds);
                FD_ZERO(&efds);
                FD_SET(c.s, &wfds);
                FD_SET(c.s, &efds);
                timeval tv;
                tv.tv_sec = ms / 1000;
                tv.tv_usec = (ms % 1000) * 1000;
                ok = select((int)c.s + 1, nullptr, &wfds, &efds, &tv) > 0
                     && FD_ISSET(c.s, &wfds);
                if (ok)
                {
                    int so_err = 0;
                    socklen_t len = sizeof(so_err);
                    getsockopt(c.s, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char *>(&so_err), &len);
                    ok = so_err == 0;
                }
            }
        }
        freeaddrinfo(res);
        if (!ok)
        {
            c.close();
            err = make_stringf("nobody is hosting at %s:%d", host.c_str(),
                               port);
            return false;
        }
        return true;
    }

    // The host's picture, drawn into our window.
    struct client_view
    {
        SDL_Window *win = nullptr;
        vector<uint8_t> frame;          // RGB, top row first
        int w = 0, h = 0;
        SDL_Rect dst = {0, 0, 0, 0};    // where it's drawn, drawable pixels

        void draw()
        {
            int dw, dh;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            if (dw <= 0 || dh <= 0)
                return;

            // Leave the tiles code's GL state as we found it.
            glPushAttrib(GL_ALL_ATTRIB_BITS);
            glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();

            glViewport(0, 0, dw, dh);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!frame.empty())
            {
                // Fit, keeping the aspect ratio.
                const double scale = min((double)dw / w, (double)dh / h);
                dst.w = (int)(w * scale);
                dst.h = (int)(h * scale);
                dst.x = (dw - dst.w) / 2;
                dst.y = (dh - dst.h) / 2;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                // Start at the centre (always a valid raster position),
                // then move to the picture's top left corner (GL's y goes
                // up) and draw the rows downwards.
                glRasterPos2f(0, 0);
                glBitmap(0, 0, 0, 0, (GLfloat)(dst.x - dw / 2.0),
                         (GLfloat)(dh / 2.0 - dst.y), nullptr);
                glPixelZoom((GLfloat)scale, (GLfloat)-scale);
                glDrawPixels(w, h, GL_RGB, GL_UNSIGNED_BYTE, frame.data());
            }

            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glPopClientAttrib();
            glPopAttrib();
            SDL_GL_SwapWindow(win);
        }

        bool apply(const vector<uint8_t> &p)
        {
            if (p.size() < 9)
                return false;
            const int fw = _get32(&p[0]), fh = _get32(&p[4]);
            const bool key = p[8];
            vector<uint8_t> delta((size_t)fw * fh * 3);
            uLongf len = delta.size();
            if (uncompress(delta.data(), &len, &p[9], p.size() - 9) != Z_OK
                || len != delta.size())
            {
                return false;
            }
            if (key || fw != w || fh != h)
            {
                frame.swap(delta);
                w = fw;
                h = fh;
            }
            else
                for (size_t i = 0; i < frame.size(); ++i)
                    frame[i] ^= delta[i];
            return true;
        }

        // Window coordinates -> frame pixels.
        void to_frame(int &x, int &y) const
        {
            int ww, wh, dw, dh;
            SDL_GetWindowSize(win, &ww, &wh);
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            if (ww > 0 && wh > 0)
            {
                x = x * dw / ww;
                y = y * dh / wh;
            }
            if (dst.w > 0 && dst.h > 0)
            {
                x = (x - dst.x) * w / dst.w;
                y = (y - dst.y) * h / dst.h;
            }
        }

        void save(const string &path) const
        {
            if (frame.empty())
                return;
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                const_cast<uint8_t *>(frame.data()), w, h, 24, w * 3,
                SDL_PIXELFORMAT_RGB24);
            if (surf)
            {
                SDL_SaveBMP(surf, path.c_str());
                SDL_FreeSurface(surf);
            }
        }
    };

    bool _send_event(connection &c, const net_event &e)
    {
        vector<uint8_t> p(sizeof(e));
        memcpy(p.data(), &e, sizeof(e));
        return c.send_msg('E', p);
    }

    // The next scripted keypress, as the events a real keyboard would
    // produce: printable characters as text input, \e (escape), \r (enter)
    // and \t (tab) as key presses; \m<x>,<y>; is a left click at those
    // frame pixels (the pointer moves there first).
    vector<net_event> _script_key(string &script)
    {
        vector<net_event> out;
        if (script.empty())
            return out;
        net_event e = {};
        if (starts_with(script, "\\m"))
        {
            const size_t end = script.find(';');
            int x = 0, y = 0;
            sscanf(script.c_str() + 2, "%d,%d", &x, &y);
            script.erase(0, end == string::npos ? script.size() : end + 1);
            e.x = x;
            e.y = y;
            e.type = SDL_MOUSEMOTION;
            out.push_back(e);
            e.type = SDL_MOUSEBUTTONDOWN;
            e.buttons = SDL_BUTTON_LEFT;
            e.clicks = 1;
            e.pressed = 1;
            out.push_back(e);
            e.type = SDL_MOUSEBUTTONUP;
            e.pressed = 0;
            out.push_back(e);
            return out;
        }
        if (script[0] == '\\' && script.size() > 1)
        {
            const char c = script[1];
            script.erase(0, 2);
            e.sym = c == 'e' ? SDLK_ESCAPE : c == 'r' ? SDLK_RETURN
                  : c == 't' ? SDLK_TAB : c;
            e.scancode = SDL_GetScancodeFromKey(e.sym);
            e.type = SDL_KEYDOWN;
            e.pressed = 1;
            out.push_back(e);
            e.type = SDL_KEYUP;
            e.pressed = 0;
            out.push_back(e);
            return out;
        }
        e.type = SDL_TEXTINPUT;
        e.text[0] = script[0];
        script.erase(0, 1);
        out.push_back(e);
        return out;
    }

    // A message in a popup; any key closes it.
    void _client_message(const string &text)
    {
        if (crawl_state.seen_hups)
            return;
        bool done = false;
        auto body = make_shared<ui::Text>(formatted_string(
            text + "\n\n[press any key]", WHITE));
        body->set_margin_for_sdl(10);
        auto popup = make_shared<ui::Popup>(body);
        popup->on_keydown_event([&done](const ui::KeyEvent&) {
            return done = true;
        });
        ui::run_layout(std::move(popup), done);
    }

    // Connect, be given a seat, and wait for the game to need us (the
    // first frame, which goes in `first_frame`), showing how it's going.
    // Returns the seat's player number (1-based), or 0 if cancelled or
    // refused (`why` says why, if it's worth saying).
    int _client_connect(connection &c, const string &address, string &why,
                        vector<uint8_t> &first_frame, bool &my_turn)
    {
        bool cancel = false;
        auto status = make_shared<ui::Text>();
        status->set_margin_for_sdl(10);
        auto popup = make_shared<ui::Popup>(status);
        popup->on_keydown_event([&cancel](const ui::KeyEvent& ev) {
            if (ui::key_exits_popup(ev.key(), false))
                cancel = true;
            return true;
        });
        const auto show = [&status](const string &s) {
            status->set_text(formatted_string::parse_string(
                s + "\n\n<lightgrey>[Esc] cancel</lightgrey>"));
        };
        show("<white>Looking for a game at " + address + "...</white>\n"
             "(On the host computer, choose Host LAN Game.)");
        ui::push_layout(popup);

        int player_no = 0;
        bool connected = false, got_frame = false;
        unsigned int next_try = 0;
        string err;
        while (!cancel && !crawl_state.seen_hups && !got_frame)
        {
            ui::pump_events(50);
            if (!connected)
            {
                if (SDL_GetTicks() < next_try)
                    continue;
                if (!_connect(c, address, 300, err))
                {
                    next_try = SDL_GetTicks() + 500;
                    // A typo won't fix itself.
                    if (starts_with(err, "bad address")
                        || starts_with(err, "unknown host"))
                    {
                        why = "Can't join: " + err + ".";
                        break;
                    }
                    continue;
                }
                connected = true;
                const string version = Version::Long;
                c.send_msg('H', vector<uint8_t>(version.begin(),
                                                version.end()));
                show("<white>Connected to " + address + ".</white>\n"
                     "Waiting for the host to let you in (they may still "
                     "be making their character)...");
            }
            if (!c.fill(0) || !c.flush())
            {
                why = "The host closed the connection.";
                break;
            }
            uint8_t type;
            vector<uint8_t> p;
            while (!got_frame && c.next(type, p))
            {
                if (type == 'R')
                {
                    why = string(p.begin(), p.end());
                    c.close();
                    break;
                }
                else if (type == 'W' && p.size() >= 8)
                {
                    player_no = _get32(&p[4]);
                    show(make_stringf("<white>You're Player %d.</white>\n"
                         "Waiting for the game to start: the host may be "
                         "waiting for other\nplayers, or making their own "
                         "character. You'll make yours after that.",
                         player_no));
                }
                else if (type == 'S' && !p.empty())
                    my_turn = p[0];
                else if (type == 'F' && player_no)
                {
                    first_frame.swap(p);
                    got_frame = true;
                }
            }
            if (!c.ok())
                break;
        }
        ui::pop_layout();
        if (!got_frame)
        {
            c.close();
            return 0;
        }
        return player_no;
    }
}

void lan_run_client(const string &address)
{
    if (!window)
        return;
    last_join_address = address;

    connection c;
    string why;
    vector<uint8_t> first_frame;
    bool my_turn = false;
    const int player_no = _client_connect(c, address, why, first_frame,
                                          my_turn);
    if (!player_no)
    {
        if (!why.empty())
            _client_message(why);
        return;
    }

    char *title = SDL_strdup(SDL_GetWindowTitle(window));
    int start_w, start_h;
    SDL_GetWindowSize(window, &start_w, &start_h);

    client_view v;
    v.win = window;
    v.apply(first_frame);
    bool quit = false;
    string script = client_script;
    unsigned int last_frame_at = SDL_GetTicks();
    unsigned int last_saved = 0;
    bool dirty_save = false;

    const auto set_title = [&] {
        SDL_SetWindowTitle(window, make_stringf("Crawl LAN - Player %d%s",
            player_no, my_turn ? " (your turn)" : "").c_str());
    };
    set_title();
    SDL_StartTextInput();
    v.draw();

    // Mouse movement comes in floods: send where it's got to now and then
    // (and before any click), not every step.
    net_event motion = {};
    bool moved = false;
    unsigned int motion_sent_at = 0;

    // Frames that arrive faster than we can draw are only drawn once.
    while (c.ok() && !quit)
    {
        if (!c.flush() || !c.fill(5))
            break;
        uint8_t type;
        vector<uint8_t> p;
        bool redraw = false;
        while (c.next(type, p))
        {
            if (type == 'R')
            {
                why = string(p.begin(), p.end());
                c.close();
            }
            else if (type == 'F')
            {
                if (v.apply(p))
                {
                    redraw = true;
                    last_frame_at = SDL_GetTicks();
                    dirty_save = true;
                }
            }
            else if (type == 'S' && !p.empty())
            {
                my_turn = p[0];
                set_title();
            }
        }
        if (redraw)
            v.draw();
        if (!c.ok())
            break;

        // Test hooks: save what we're shown once it settles; type the
        // script on our turns, a key at a time once the screen is idle.
        const unsigned int now = SDL_GetTicks();
        if (!client_dump.empty() && dirty_save && now - last_frame_at > 300
            && now - last_saved > 300)
        {
            v.save(client_dump);
            dirty_save = false;
            last_saved = now;
        }
        if (my_turn && !script.empty() && now - last_frame_at > 400)
        {
            for (const net_event &e : _script_key(script))
                _send_event(c, e);
            last_frame_at = now;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            net_event e = {};
            e.type = ev.type;
            switch (ev.type)
            {
            case SDL_QUIT:
                quit = true;
                continue;
            case SDL_WINDOWEVENT:
                v.draw();
                continue;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                e.sym = ev.key.keysym.sym;
                e.scancode = ev.key.keysym.scancode;
                e.mod = ev.key.keysym.mod;
                e.repeat = ev.key.repeat;
                e.pressed = ev.key.state == SDL_PRESSED;
                break;
            case SDL_TEXTINPUT:
                strncpy(e.text, ev.text.text, sizeof(e.text) - 1);
                break;
            case SDL_MOUSEMOTION:
                e.x = ev.motion.x;
                e.y = ev.motion.y;
                v.to_frame(e.x, e.y);
                e.buttons = ev.motion.state;
                motion = e;
                moved = true;
                continue;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (moved)
                {
                    // Keep the order: the pointer gets there, then clicks.
                    if (c.ok() && my_turn)
                        _send_event(c, motion);
                    moved = false;
                }
                e.x = ev.button.x;
                e.y = ev.button.y;
                v.to_frame(e.x, e.y);
                e.buttons = ev.button.button;
                e.clicks = ev.button.clicks;
                e.pressed = ev.button.state == SDL_PRESSED;
                break;
            case SDL_MOUSEWHEEL:
                e.wheel_x = ev.wheel.x;
                e.wheel_y = ev.wheel.y;
                break;
            default:
                continue;
            }
            if (c.ok() && my_turn)
                _send_event(c, e);
        }
        if (moved && SDL_GetTicks() - motion_sent_at >= 30)
        {
            if (c.ok() && my_turn)
                _send_event(c, motion);
            moved = false;
            motion_sent_at = SDL_GetTicks();
        }
    }
    const bool host_left = !c.ok();
    c.close();

    if (!client_dump.empty())
        v.save(client_dump);

    // Give the window back to the game.
    SDL_SetWindowTitle(window, title);
    SDL_free(title);
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    if (w != start_w || h != start_h)
        tiles.resize_event(w, h);
    if (quit)
        crawl_state.seen_hups++;
    else if (host_left)
        _client_message(why.empty() ? "The host has left the game." : why);
}

namespace
{
    // The join prompt's text box swallows Enter and Esc, so catch them as
    // they reach it (a key proc is a plain function, hence the globals).
    bool join_done, join_cancel;

    keyfun_action _keyfun_join_input(int &ch)
    {
        if (ch == CK_ENTER || ch == '\r' || ch == '\n')
        {
            join_done = true;
            return KEYFUN_IGNORE;
        }
        if (ui::key_exits_popup(ch, false))
        {
            join_done = join_cancel = true;
            return KEYFUN_IGNORE;
        }
        return KEYFUN_PROCESS;
    }
}

void lan_join_menu()
{
    join_done = join_cancel = false;
    bool &done = join_done, &cancel = join_cancel;
    auto box = make_shared<ui::Box>(ui::Widget::VERT);
    box->set_margin_for_sdl(10);
    box->add_child(make_shared<ui::Text>(formatted_string(
        "Join a LAN game\n", CYAN)));
    box->add_child(make_shared<ui::Text>(
        "Enter the address of the computer hosting the game (it shows it\n"
        "when you choose Host LAN Game there). 127.0.0.1 means this\n"
        "computer. Add :port if the host uses a different port.\n"));
    auto row = make_shared<ui::Box>(ui::Box::HORZ);
    row->set_cross_alignment(ui::Widget::CENTER);
    row->add_child(make_shared<ui::Text>("Address: "));
    auto input = make_shared<ui::TextEntry>();
    input->set_text(last_join_address);
    input->set_keyproc(_keyfun_join_input);
    row->add_child(input);
    box->add_child(row);
    box->add_child(make_shared<ui::Text>(formatted_string(
        "\n[Enter] join   [Esc] cancel", LIGHTGREY)));
    auto popup = make_shared<ui::Popup>(box);
    popup->on_keydown_event([&](const ui::KeyEvent& ev) {
        const auto key = ev.key();
        if (key == CK_ENTER || key == '\r' || key == '\n')
            return done = true;
        if (ui::key_exits_popup(key, false))
            return done = cancel = true;
        return false;
    });
    ui::run_layout(std::move(popup), done, input);
    if (cancel || crawl_state.seen_hups)
        return;
    string address = input->get_text();
    trim_string(address);
    if (address.empty())
        return;
    lan_run_client(address);
}

#endif // USE_TILE_LOCAL
