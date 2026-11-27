/**
 * @file
 * @brief LAN play: the host streams each remote seat its own view and takes
 *        that seat's input on its player's turns. See lan.h.
 *
 * Wire format (TCP, little-endian): each message is a 1-byte type, a 4-byte
 * payload length, then the payload.
 *   client -> host  'H' hello: the client's version string
 *                   'J' its copies of party games (sent after 'H'): u32
 *                       count, then each: string game id, u32 owner (from
 *                       0), u32 save counter; then string its player's
 *                       name ("": none). (Strings: u32 length, bytes.)
 *                   'E' an input event (struct net_event)
 *   host -> client  'W' welcome: u32 seat, u32 player number (1-based)
 *                   'V' the game was saved, here's your copy: string file
 *                       name, string game id, u32 owner, then the save
 *                   'D' the game is over: delete your copies of string
 *                       game id
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
#include <map>
#include <vector>

#include "cio.h"
#include "artefact.h"
#include "colour.h"
#include "database.h"
#include "describe.h"
#include "describe-spells.h"
#include "invent.h"
#include "item-name.h"
#include "menu.h"
#include "player.h"
#include "libutil.h"
#include "end.h"
#include "files.h"
#include "message.h"
#include "newgame.h"
#include "newgame-def.h"
#include "ng-input.h"
#include "options.h"
#include "output.h"
#include "party.h"
#include "state.h"
#include "syscalls.h"
#include "ui.h"
#include "stringutil.h"
#include "unwind.h"
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
    // A client: the name its player gave (their character's name).
    string client_name;
    // The host, saving and quitting: each remote player's copy of the save,
    // by party index: its file name and contents.
    std::map<int, std::pair<string, string>> pending_copies;
    // A client is making its character in its own window (the host asked).
    bool client_choosing = false;

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

    void _put_string(vector<uint8_t> &b, const string &s)
    {
        _put32(b, s.size());
        b.insert(b.end(), s.begin(), s.end());
    }

    // Reading a payload a field at a time; `ok` goes false if it runs out.
    struct unpacker
    {
        const vector<uint8_t> &p;
        size_t at = 0;
        bool ok = true;

        explicit unpacker(const vector<uint8_t> &payload) : p(payload) {}

        uint32_t u32()
        {
            if (!ok || p.size() - at < 4)
            {
                ok = false;
                return 0;
            }
            at += 4;
            return _get32(&p[at - 4]);
        }

        string str()
        {
            const uint32_t n = u32();
            if (!ok || p.size() - at < n)
            {
                ok = false;
                return "";
            }
            at += n;
            return string(p.begin() + (at - n), p.begin() + at);
        }

        string rest()
        {
            const size_t from = at;
            at = p.size();
            return ok ? string(p.begin() + from, p.end()) : "";
        }
    };

    // For scripted tests (like crawl.stderr(); see util/lan-copy-smoke.ps1):
    // Tiles on Windows has no stderr to read.
    void _test_log(const string &text)
    {
        if (const char *path = getenv("CRAWL_STDERR_FILE"))
            if (FILE *f = fopen_u(path, "a"))
            {
                fprintf(f, "%s\n", text.c_str());
                fclose(f);
            }
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

        // Done sending (everything flushed): say so, and wait up to `ms` for
        // the other end to hang up, throwing away whatever it still sends,
        // before closing. Closing with unread input would reset the
        // connection, which can lose what we sent last.
        void hang_up(int ms)
        {
            if (!ok())
                return;
#ifdef TARGET_OS_WINDOWS
            shutdown(s, SD_SEND);
#else
            shutdown(s, SHUT_WR);
#endif
            for (int waited = 0; waited < ms; waited += 10)
            {
                if (!_readable(s, 10))
                    continue;
                char buf[4096];
                const int got = ::recv(s, buf, sizeof(buf), 0);
                if (got == 0 || got < 0 && !_would_block())
                    break;
            }
            close();
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
                    // Gone, but what it sent before going (e.g. why) can
                    // still be read with next().
                    _close_sock(s);
                    s = BAD_SOCK;
                    out.clear();
                    out_pos = 0;
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
void lan_run_client(const string &/*address*/, const string &/*name*/)
{
    fprintf(stderr, "LAN play needs the Tiles build of Crawl.\n");
}

void lan_join_menu(const string &/*name*/)
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

bool lan_host_lobby() { return true; }
lan_resume lan_host_resume_lobby(const party_save_info &)
{
    return lan_resume::play;
}
int lan_lobby_party_size() { return 0; }
void lan_host_stop() { host_port = -1; }
void lan_host_end_game(const string &, const string &, bool)
{
    host_port = -1;
}
void lan_host_add_copy(int, const string &, const string &) {}
string lan_player_name(int) { return ""; }
bool lan_active() { return false; }
int lan_seat_of_player(int /*idx*/) { return 0; }
int lan_output_seat() { return 0; }
lan_seat_scope::lan_seat_scope(int seat) : prev(seat) {}
lan_seat_scope::~lan_seat_scope() {}
void lan_refresh_idle_views(bool /*force*/) {}
void lan_host_begin_choosing() {}
void lan_host_wait_for_choices() {}
bool lan_host_choice(int, bool &, int &, int &, int &) { return false; }

#else // USE_TILE_LOCAL

namespace
{
    // ------------------------------------------------- off-turn inventory
    //
    // While it isn't their turn, a player can look at their inventory as it
    // stood after the last move ('i'): a real inventory menu, describing an
    // item when it's chosen, but nothing can be used from it. The host
    // makes a snapshot of each waiting player's inventory (only it has the
    // game) and sends it to their computer; the menu is drawn over the
    // window without stopping anything, so the game goes on meanwhile.

    struct inv_item
    {
        string name;            // "a - a +0 flail (weapon)"
        int colour = -1;        // menu colour (-1: default)
        vector<tile_def> tiles;
        string title;           // description popup's title
        string desc;            // its text, with colour tags
    };

    struct inv_snapshot
    {
        string title;
        vector<inv_item> items;
    };

    void _put_str(vector<uint8_t> &b, const string &s)
    {
        _put_string(b, s);
    }

    vector<uint8_t> _pack(const inv_snapshot &s)
    {
        vector<uint8_t> b;
        _put_str(b, s.title);
        _put32(b, s.items.size());
        for (const inv_item &it : s.items)
        {
            _put_str(b, it.name);
            _put32(b, (uint32_t)it.colour);
            _put32(b, it.tiles.size());
            for (const tile_def &t : it.tiles)
            {
                _put32(b, t.tile);
                _put32(b, (uint32_t)t.ymax);
            }
            _put_str(b, it.title);
            _put_str(b, it.desc);
        }
        return b;
    }

    bool _unpack(const vector<uint8_t> &b, inv_snapshot &s)
    {
        unpacker u{b};
        s.title = u.str();
        s.items.clear();
        const uint32_t n = u.u32();
        for (uint32_t i = 0; u.ok && i < n && i < 1000; ++i)
        {
            inv_item it;
            it.name = u.str();
            it.colour = (int)u.u32();
            const uint32_t nt = u.u32();
            for (uint32_t j = 0; u.ok && j < nt && j < 16; ++j)
            {
                const tileidx_t tile = u.u32();
                const int ymax = (int)u.u32();
                it.tiles.emplace_back(tile, ymax);
            }
            it.title = u.str();
            it.desc = u.str();
            s.items.push_back(std::move(it));
        }
        return u.ok;
    }

    // The swapped-in player's item names, in inventory order: when these
    // haven't changed, neither has much else worth resending.
    string _inventory_key()
    {
        string key = make_stringf("%d|", you.gold);
        for (int obj = 0; obj < NUM_OBJECT_CLASSES; obj++)
            for (const auto &item : you.inv)
                if (item.defined() && item.base_type == inv_order[obj])
                    key += item.name(DESC_INVENTORY_EQUIP) + "|";
        return key;
    }

    // The swapped-in player's inventory, with each item's description.
    inv_snapshot _inventory_snapshot(const string &who)
    {
        inv_snapshot s;
        s.title = make_stringf("%s's inventory: %s, %d gold", who.c_str(),
                               slot_description().c_str(), you.gold);
        for (int obj = 0; obj < NUM_OBJECT_CLASSES; obj++)
        {
            const int cls = inv_order[obj];
            bool header = false;
            for (const auto &item : you.inv)
            {
                if (!item.defined() || item.base_type != cls)
                    continue;
                if (!header)
                {
                    // A category heading (no letter, no tiles).
                    inv_item h;
                    h.name = item_class_name(cls);
                    s.items.push_back(h);
                    header = true;
                }
                inv_item it;
                it.name = item.name(DESC_INVENTORY_EQUIP);
                it.colour = menu_colour(it.name, item_prefix(item, false),
                                        "pickup", false);
                get_tiles_for_item(item, it.tiles, true);

                // As describe_item_popup() has it.
                it.title = it.name + ".";
                string desc = get_item_description(item);
                const string quote = is_unrandom_artefact(item)
                                     && item.is_identified()
                    ? getQuoteString(get_artefact_name(item))
                    : getQuoteString(item.name(DESC_DBNAME, true, false,
                                               false));
                if (!quote.empty())
                    desc += "\n_________________\n\n<darkgrey>" + quote
                            + "</darkgrey>";
                formatted_string fs = formatted_string::parse_string(desc);
                formatted_string spells_desc;
                describe_spellset(item_spellset(item), &item, spells_desc,
                                  nullptr);
                fs += spells_desc;
                it.desc = fs.trim().to_colour_string(LIGHTGREY);
                s.items.push_back(std::move(it));
            }
        }
        return s;
    }

    class offturn_menu : public Menu
    {
    public:
        offturn_menu()
            : Menu(MF_SINGLESELECT | MF_ALLOW_FORMATTING | MF_INIT_HOVER
                   | MF_SHOW_EMPTY | MF_ARROWS_SELECT)
        {
        }

        bool process_key(int key) override
        {
            // 'i' closes it again, like Escape.
            if (key == 'i' || key == 'I')
                return false;
            return Menu::process_key(key);
        }

        int hovered() const { return last_hovered; }
    };

    class offturn_inventory
    {
    public:
        bool has_snapshot() const { return have; }
        bool is_open() const { return menu != nullptr; }

        void set(const inv_snapshot &s)
        {
            snap = s;
            have = true;
            if (!menu)
                return;
            const int hover = menu->hovered();
            _fill();
            menu->update_menu(true);
            if (hover >= 0)
                menu->set_hovered(min(hover, (int)menu->item_count() - 1));
            if (desc && (described < 0
                         || described >= (int)snap.items.size()
                         || snap.items[described].title != desc_title))
            {
                desc = nullptr;     // that item has gone (or changed)
            }
        }

        void open()
        {
            if (!have)
                return;
            menu = make_unique<offturn_menu>();
            menu->on_single_selection = [this](const MenuEntry &me) {
                _describe(*static_cast<const int *>(me.data));
                return true;
            };
            _fill();
            popup = menu->overlay_popup();
        }

        void close()
        {
            desc = nullptr;
            popup = nullptr;
            menu = nullptr;
            indices.clear();
        }

        // Draw it over whatever the window shows.
        void draw()
        {
            if (!is_open())
                return;
            _with_layouts([] { ui::force_render(); });
        }

        // Handle one input event (then draw, unless that closed it).
        void handle(const SDL_Event &event)
        {
            if (!is_open())
                return;
            // Back in the queue, for the UI's own event handling (which
            // also turns key presses into keys) to take out again.
            SDL_Event copy = event;
            SDL_PushEvent(&copy);
            _with_layouts([] { ui::pump_events(0); });
            if (desc_done)
                desc = nullptr;
            if (menu->overlay_done() || crawl_state.seen_hups)
                close();
            else
                draw();
        }

    private:
        inv_snapshot snap;
        bool have = false;
        unique_ptr<offturn_menu> menu;
        shared_ptr<ui::Popup> popup;
        shared_ptr<ui::Popup> desc;
        bool desc_done = false;
        int described = -1;
        string desc_title;
        // Entry data: each item's index in snap.items.
        vector<unique_ptr<int>> indices;

        void _fill()
        {
            menu->clear();
            indices.clear();
            menu->set_title(new MenuEntry(snap.title + "  <lightgrey>(not "
                "your turn: look only)</lightgrey>", MEL_TITLE));
            for (int i = 0; i < (int)snap.items.size(); ++i)
            {
                const inv_item &it = snap.items[i];
                const bool heading = it.title.empty();
                const int hotkey = !heading && it.name.size() > 4
                                   && it.name.compare(1, 3, " - ") == 0
                    ? it.name[0] : 0;
                // The menu puts the letter in front itself.
                auto me = make_unique<MenuEntry>(
                    replace_all(hotkey ? it.name.substr(4) : it.name, "<",
                                "<<"),
                    heading ? MEL_SUBTITLE : MEL_ITEM, 1, hotkey);
                if (!heading)
                {
                    if (it.colour != -1)
                        me->colour = it.colour;
                    for (const tile_def &t : it.tiles)
                        me->add_tile(t);
                    indices.push_back(make_unique<int>(i));
                    me->data = indices.back().get();
                }
                menu->add_entry(std::move(me));
            }
        }

        void _describe(int i)
        {
            if (i < 0 || i >= (int)snap.items.size())
                return;
            const inv_item &it = snap.items[i];
            described = i;
            desc_title = it.title;
            desc_done = false;

            auto vbox = make_shared<ui::Box>(ui::Widget::VERT);
            auto title_hbox = make_shared<ui::Box>(ui::Widget::HORZ);
            if (!it.tiles.empty())
            {
                auto stack = make_shared<ui::Stack>();
                for (const tile_def &t : it.tiles)
                {
                    auto icon = make_shared<ui::Image>();
                    icon->set_tile(t);
                    stack->add_child(std::move(icon));
                }
                title_hbox->add_child(std::move(stack));
            }
            auto title = make_shared<ui::Text>(it.title);
            title->set_margin_for_sdl(0, 0, 0, 10);
            title_hbox->add_child(std::move(title));
            title_hbox->set_cross_alignment(ui::Widget::CENTER);
            title_hbox->set_margin_for_sdl(0, 0, 20, 0);
            vbox->add_child(std::move(title_hbox));

            auto scroller = make_shared<ui::Scroller>();
            auto text = make_shared<ui::Text>(
                formatted_string::parse_string(it.desc));
            text->set_wrap_text(true);
            scroller->set_child(text);
            vbox->add_child(scroller);
            auto footer = make_shared<ui::Text>(formatted_string(
                "It isn't your turn: you can only look.", CYAN));
            footer->set_margin_for_sdl(20, 0, 0, 0);
            vbox->add_child(std::move(footer));
            vbox->max_size().width = tiles.get_crt_font()->char_width() * 80;

            desc = make_shared<ui::Popup>(std::move(vbox));
            desc->on_keydown_event([this, scroller](const ui::KeyEvent& ev) {
                if (ui::key_exits_popup(ev.key(), true) || ev.key() == 'i')
                    return desc_done = true;
                return scroller->on_event(ev);
            });
        }

        // Run `fn` with the menu (and description) as the only layouts
        // drawn, over a blank window.
        void _with_layouts(const function<void()> &fn)
        {
            unwind_bool regions(ui::should_render_current_regions, false);
            ui::cutoff_point cutoff;
            ui::push_layout(popup, KMC_MENU);
            const bool had_desc = desc != nullptr;
            if (had_desc)
                ui::push_layout(desc);
            fn();
            if (had_desc)
                ui::pop_layout();
            ui::pop_layout();
        }
    };

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
        // Character choice (see lan_host_begin_choosing()): asked and not
        // yet answered; answered (with the 'P' payload kept in `choice`).
        bool choosing = false;
        bool has_choice = false;
        vector<uint8_t> choice;
        // What their last inventory snapshot ('I') was made from (see
        // _inventory_key()).
        string inv_key;
        // Their copies of party games ('J'), for continuing a saved game:
        // and who the copy of this one says they were (-1 if none), and how
        // new that copy is.
        bool claim_known = false;
        vector<party_copy> copies;
        int claim_player = -1;
        int claim_generation = 0;
        unsigned int joined_at = 0;
        // The name they gave on their start menu ("" if none).
        string name;

        void take_copies(const vector<uint8_t> &p)
        {
            claim_known = true;
            copies.clear();
            unpacker u(p);
            const uint32_t n = u.u32();
            for (uint32_t c = 0; c < n && c < 1000 && u.ok; ++c)
            {
                party_copy pc;
                pc.game_id = u.str();
                pc.owner = u.u32();
                pc.generation = u.u32();
                if (u.ok)
                    copies.push_back(pc);
            }
            const string their_name = u.str();
            if (u.ok && is_good_name(their_name, false))
                name = their_name;
        }

        // Before the game: take their 'J' (sent right after 'H') if it has
        // come. True once it has (or it's too late for it to).
        bool read_copies(unsigned int now)
        {
            uint8_t type;
            vector<uint8_t> p;
            while (!claim_known && conn.next(type, p))
                if (type == 'J')
                    take_copies(p);
            // (Every version that can join sends it; just in case.)
            if (!claim_known && now - joined_at > 10000)
                claim_known = true;
            return claim_known;
        }
    };

    sock_t listener = BAD_SOCK;
    // Party size the lobby settled on (0: no lobby was held).
    int lobby_party = 0;
    // seats[0] is the host's own screen and is never connected.
    vector<seat> seats(1);
    int seat_override = -1;
    SDL_Window *window = nullptr;
    unsigned int last_idle_refresh = 0;
    // Player 1 is still making their character (remote seats were asked to
    // choose at the same time).
    bool host_choosing = false;
    // Player 1's inventory, to look at during a remote player's turn, and
    // the host's last idle view (to put back after looking).
    offturn_inventory host_inv;
    string host_inv_key;
    vector<uint8_t> host_frame;
    int host_frame_w = 0, host_frame_h = 0;
    // Keep each frame the host's screen is drawn (and, if capture_only, don't
    // show it).
    bool capture_host_frame = false;
    bool capture_only = false;
    // Handling the host's inventory menu: input still comes from the remote
    // player whose turn it is.
    bool host_menu_busy = false;

    // Who's still choosing a character, e.g. "Player 1 and Player 3" (""
    // if nobody).
    string _still_choosing()
    {
        vector<string> who;
        if (host_choosing)
            who.push_back(party_member_name(0));
        for (int k = 1; k < (int)seats.size(); ++k)
            if (seats[k].conn.ok() && seats[k].choosing)
                who.push_back(party_member_name(seats[k].player));
        return comma_separated_line(who.begin(), who.end());
    }

    // Tell the players who have chosen who they're waiting for.
    void _send_waiting()
    {
        const string who = _still_choosing();
        const string text = who.empty()
            ? "Everyone has chosen: the game is starting..."
            : "Waiting for " + who + " to choose a character...";
        for (int k = 1; k < (int)seats.size(); ++k)
            if (seats[k].conn.ok() && !seats[k].choosing)
                seats[k].conn.send_msg('Q', vector<uint8_t>(text.begin(),
                                                            text.end()));
    }

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
        if (s.choosing)
        {
            s.choosing = false;
            _send_waiting();
        }
    }

    // The seat whose turn it is.
    int _turn_seat()
    {
        return party_active() ? lan_seat_of_player(party_active_index()) : 0;
    }

    int _live_seat()
    {
        if (seat_override >= 0)
            return seat_override;
        return _turn_seat();
    }

    // Read the back buffer: RGB, top row first.
    void _read_back(int &w, int &h, vector<uint8_t> &px)
    {
        SDL_GL_GetDrawableSize(window, &w, &h);
        if (w <= 0 || h <= 0)
        {
            px.clear();
            return;
        }
        vector<uint8_t> raw((size_t)w * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
        // GL's rows go bottom-up.
        px.resize(raw.size());
        const size_t row = (size_t)w * 3;
        for (int y = 0; y < h; ++y)
            memcpy(&px[y * row], &raw[(h - 1 - y) * row], row);
    }

    void _send_frame(seat &s, int w, int h, vector<uint8_t> &px);

    // Read the remote seats' input: the output seat's is queued, anyone
    // else's (typing while it isn't their turn) is dropped. Also sends
    // whatever was waiting to go out.
    void _poll()
    {
        // (While the host works their inventory menu, the input that counts
        // is still that of the player whose turn it is.)
        const int live = host_menu_busy ? _turn_seat() : _live_seat();
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
                if (type == 'J')
                    s.take_copies(payload);
                else if (type == 'P' && s.choosing)
                {
                    s.choosing = false;
                    s.has_choice = true;
                    s.choice = payload;
                    _send_waiting();
                }
                else if (type == 'E' && payload.size() == sizeof(net_event)
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
    // the seat for party member `idx` (-1: not yet decided, so not welcomed
    // yet either). True if it joined.
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
        s.joined_at = SDL_GetTicks();
        if (idx >= 0)
        {
            vector<uint8_t> welcome;
            _put32(welcome, seats.size());
            _put32(welcome, idx + 1);
            s.conn.send_msg('W', welcome);
        }
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

int lan_lobby_party_size()
{
    return lobby_party;
}

void lan_host_begin_choosing()
{
    host_choosing = true;
    for (int k = 1; k < (int)seats.size(); ++k)
    {
        seat &s = seats[k];
        s.has_choice = false;
        s.choice.clear();
        s.choosing = s.conn.send_msg('C', {});
    }
}

void lan_host_wait_for_choices()
{
    host_choosing = false;
    _send_waiting();
    _poll();
    if (_still_choosing().empty())
        return;

    bool stop = false;
    auto text = make_shared<ui::Text>();
    text->set_margin_for_sdl(10);
    auto popup = make_shared<ui::Popup>(text);
    popup->on_keydown_event([&stop](const ui::KeyEvent& ev) {
        if (ui::key_exits_popup(ev.key(), false))
            stop = true;
        return true;
    });
    ui::push_layout(popup);
    string who;
    while (!stop && !crawl_state.seen_hups)
    {
        _poll();
        who = _still_choosing();
        if (who.empty())
            break;
        text->set_text(formatted_string::parse_string(
            "<white>Waiting for " + who + " to choose a character...</white>"
            "\n\n<lightgrey>[Esc] stop waiting (anyone still choosing joins"
            " as a copy of you)</lightgrey>"));
        ui::pump_events(50);
    }
    ui::pop_layout();
    // Whoever's still choosing gets no say now (their answer is ignored).
    for (int k = 1; k < (int)seats.size(); ++k)
        seats[k].choosing = false;
}

bool lan_host_choice(int idx, bool &chosen, int &species, int &job,
                     int &weapon)
{
    const int k = lan_seat_of_player(idx);
    if (k <= 0 || !seats[k].has_choice)
        return false;
    const vector<uint8_t> &p = seats[k].choice;
    if (p.size() < 13)
        return false;
    chosen = p[0];
    species = (int32_t)_get32(&p[1]);
    job = (int32_t)_get32(&p[5]);
    weapon = (int32_t)_get32(&p[9]);
    return true;
}

bool lan_host_lobby()
{
    lobby_party = 0;
    if (!lan_host_requested())
        return true;
    if (!_listen())
    {
        mprf(MSGCH_ERROR, "Can't host on port %d (is it already in use?).",
             host_port);
        host_port = -1;
        return true;
    }

    // Players at this computer besides player 1 (hotseat), after the
    // remote ones.
    int locals = 0;
    bool cancel = false, start_now = false;
    auto text = make_shared<ui::Text>();
    text->set_margin_for_sdl(10);
    auto popup = make_shared<ui::Popup>(text);
    popup->on_keydown_event([&](const ui::KeyEvent& ev) {
        const int key = ev.key();
        if (key == CK_ENTER || key == '\r' || key == '\n')
            start_now = true;
        else if (key == '+' || key == '=')
            ++locals;
        else if (key == '-' || key == '_')
            locals = max(locals - 1, 0);
        else if (ui::key_exits_popup(key, false))
            cancel = true;
        return true;
    });
    ui::push_layout(popup);

    const string how = _join_instructions();
    // Scripted tests: start by ourselves once the party is this big.
    const char *autostart_env = getenv("CRAWL_LAN_AUTOSTART");
    const int autostart = autostart_env ? atoi(autostart_env) : 0;
    int remotes = 0;
    while (!cancel && !start_now && !crawl_state.seen_hups)
    {
        // Forget anyone who left again, and number the rest 2, 3, ... in
        // the order they joined (telling them if that changed).
        for (int k = (int)seats.size() - 1; k >= 1; --k)
            if (!seats[k].conn.fill(0) || !seats[k].conn.flush())
                seats.erase(seats.begin() + k);
        remotes = (int)seats.size() - 1;
        for (int k = 1; k <= remotes; ++k)
        {
            if (seats[k].player == k)
                continue;
            seats[k].player = k;
            vector<uint8_t> welcome;
            _put32(welcome, k);
            _put32(welcome, k + 1);
            seats[k].conn.send_msg('W', welcome);
        }
        locals = min(locals, PARTY_MAX_SIZE - 1 - remotes);
        const int players = 1 + remotes + locals;
        // Their names (and copies of saved games) come right after they
        // join; the game needs the names.
        bool all_in = true;
        for (int k = 1; k <= remotes; ++k)
            all_in = seats[k].read_copies(SDL_GetTicks()) && all_in;
        if (autostart > 0 && players >= autostart && all_in)
            break;

        string list = "<white>Player 1</white>: you\n";
        for (int idx = 1; idx < players; ++idx)
        {
            const string who = idx <= remotes && !seats[idx].name.empty()
                               ? " (" + seats[idx].name + ")" : "";
            list += make_stringf("<white>Player %d</white>%s: %s\n", idx + 1,
                                 who.c_str(),
                                 idx <= remotes
                                 ? "<lightgreen>joined</lightgreen>"
                                 : "<lightcyan>plays at this computer"
                                   "</lightcyan>");
        }
        const bool full = players >= PARTY_MAX_SIZE;
        if (!full)
            list += "<darkgrey>(waiting for more players...)</darkgrey>\n";

        text->set_text(formatted_string::parse_string(
            "<cyan>Hosting a LAN game.</cyan> The party is everyone who "
            "joins (up to " + to_string(PARTY_MAX_SIZE) + ").\n\n"
            + how + "\n\n" + list + "\n"
              "<lightgrey>[Enter] start the game with "
            + (players == 1 ? string("just you")
                            : to_string(players) + " players") + "\n"
              "[+]/[-] add/remove a player at this computer\n"
              "[Esc] stop hosting</lightgrey>"));
        ui::pump_events(50);
        if (!full)
            _accept_seat(remotes + 1, 50);
    }
    ui::pop_layout();

    if (cancel || crawl_state.seen_hups)
    {
        lan_host_stop();
        return false;
    }
    // Someone may have joined since the last look: keep the numbers tidy.
    remotes = (int)seats.size() - 1;
    lobby_party = min(1 + remotes + locals, PARTY_MAX_SIZE);
    // Anyone whose name hasn't come yet: it's on its way (a moment at most).
    for (int k = 1; k < (int)seats.size(); ++k)
        while (seats[k].conn.ok() && !seats[k].read_copies(SDL_GetTicks())
               && !crawl_state.seen_hups)
        {
            seats[k].conn.fill(20);
        }
    return true;
}

lan_resume lan_host_resume_lobby(const party_save_info &info)
{
    lobby_party = 0;
    // The players who had their own computers, in order: each joiner gets
    // the one their copy of the game says they were, or else the first
    // one nobody has.
    vector<int> wanted;
    for (int i = 0; i < info.size; ++i)
        if (info.remote[i])
            wanted.push_back(i);
    if (wanted.empty() || !lan_host_requested())
        return lan_resume::play;
    if (!_listen())
    {
        mprf(MSGCH_ERROR, "Can't host on port %d (is it already in use?).",
             host_port);
        host_port = -1;
        return lan_resume::play;
    }

    bool cancel = false, start_now = false, delete_game = false;
    // 'D' once asks to make sure; 'D' again deletes.
    bool delete_asked = false;
    auto text = make_shared<ui::Text>();
    text->set_margin_for_sdl(10);
    auto popup = make_shared<ui::Popup>(text);
    popup->on_keydown_event([&](const ui::KeyEvent& ev) {
        const int key = ev.key();
        if (key == 'D' || key == 'd')
        {
            if (delete_asked)
                delete_game = true;
            delete_asked = true;
            return true;
        }
        delete_asked = false;
        if (key == CK_ENTER || key == '\r' || key == '\n')
            start_now = true;
        else if (ui::key_exits_popup(key, false))
            cancel = true;
        return true;
    });
    ui::push_layout(popup);

    const auto refuse = [](int k, const string &why) {
        seats[k].conn.send_msg('R', vector<uint8_t>(why.begin(), why.end()));
        seats[k].conn.flush();
        seats[k].conn.hang_up(500);
        seats.erase(seats.begin() + k);
    };

    const string how = _join_instructions();
    while (!cancel && !start_now && !delete_game && !crawl_state.seen_hups)
    {
        const unsigned int now = SDL_GetTicks();
        for (int k = (int)seats.size() - 1; k >= 1; --k)
        {
            seat &s = seats[k];
            if (!s.conn.fill(0) || !s.conn.flush())
            {
                seats.erase(seats.begin() + k);
                continue;
            }
            // Their copies (also taken by _poll(), which the UI's event
            // loop runs).
            s.read_copies(now);
            // Who their copy of this game says they were (the newest, if
            // they somehow have several).
            s.claim_player = -1;
            s.claim_generation = 0;
            for (const party_copy &pc : s.copies)
                if (!info.game_id.empty() && pc.game_id == info.game_id
                    && pc.generation >= s.claim_generation)
                {
                    s.claim_player = pc.owner;
                    s.claim_generation = pc.generation;
                }
        }

        // Who plays whom: everyone whose copy says they were one of the
        // players we're waiting for gets that player; the others get the
        // rest in the order they joined.
        vector<int> give(seats.size(), -1);
        vector<bool> taken(info.size, false);
        for (int k = 1; k < (int)seats.size(); ++k)
        {
            const int c = seats[k].claim_player;
            if (seats[k].claim_known && c >= 0 && c < info.size
                && info.remote[c] && !taken[c])
            {
                give[k] = c;
                taken[c] = true;
            }
        }
        for (int k = 1; k < (int)seats.size(); ++k)
        {
            if (!seats[k].claim_known || give[k] >= 0)
                continue;
            for (int idx : wanted)
                if (!taken[idx])
                {
                    give[k] = idx;
                    taken[idx] = true;
                    break;
                }
        }
        // More joined than there are players to give them: turn them away.
        for (int k = (int)seats.size() - 1; k >= 1; --k)
            if (seats[k].claim_known && give[k] < 0)
            {
                refuse(k, "This game has no more players to join as.");
                give.erase(give.begin() + k);
            }
        int back = 0;
        for (int k = 1; k < (int)seats.size(); ++k)
        {
            if (give[k] < 0)
                continue;
            ++back;
            if (seats[k].player == give[k])
                continue;
            seats[k].player = give[k];
            _test_log(make_stringf("LAN host: computer %d is Player %d (its "
                                   "copy says %d)", k, give[k] + 1,
                                   seats[k].claim_player + 1));
            vector<uint8_t> welcome;
            _put32(welcome, k);
            _put32(welcome, give[k] + 1);
            _put32(welcome, 1);     // continuing a saved game
            seats[k].conn.send_msg('W', welcome);
        }
        // Everyone's back: carry on.
        if (back == (int)wanted.size() && back == (int)seats.size() - 1)
            break;

        string list;
        for (int i = 0; i < info.size; ++i)
        {
            const bool remote = info.remote[i];
            const int k = remote ? lan_seat_of_player(i) : 0;
            string state = !remote ? "<lightcyan>this computer</lightcyan>"
                         : k > 0   ? "<lightgreen>back</lightgreen>"
                                   : "<yellow>waiting...</yellow>";
            if (k > 0 && seats[k].claim_player == i
                && seats[k].claim_generation > info.generation)
            {
                state += " <lightred>(has a newer save of this game!)"
                         "</lightred>";
            }
            list += make_stringf("<white>Player %d</white> (%s): %s\n", i + 1,
                info.describe[i].c_str(), state.c_str());
        }
        text->set_text(formatted_string::parse_string(
            "<cyan>Continuing a saved LAN game.</cyan> Waiting for the "
            "players who were on other computers to join again.\n\n"
            + how + "\n\n" + list + "\n"
              "The game carries on once everyone is back.\n\n"
              "<lightgrey>[Enter] carry on now (missing players play here)\n"
              "[Esc] stop hosting\n"
            + (delete_asked
               ? "</lightgrey><lightred>[D] again: delete this game for "
                 "good, here and the copies of everyone who has joined"
                 "</lightred>"
               : "[D] delete this game</lightgrey>")));
        ui::pump_events(50);
        if ((int)seats.size() - 1 < (int)wanted.size())
            _accept_seat(-1, 50);
    }
    ui::pop_layout();

    if (delete_game)
        return lan_resume::delete_game;
    if (cancel || crawl_state.seen_hups)
    {
        lan_host_stop();
        return lan_resume::stop;
    }
    // Anyone who joined but has no player yet: no room for them.
    for (int k = (int)seats.size() - 1; k >= 1; --k)
        if (seats[k].player < 0)
            refuse(k, "This game has no more players to join as.");
    lobby_party = info.size;
    return lan_resume::play;
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

void lan_host_add_copy(int idx, const string &filename, const string &data)
{
    pending_copies[idx] = make_pair(filename, data);
}

void lan_host_end_game(const string &why, const string &game_id,
                       bool delete_copies)
{
    bool any = false;
    for (int k = 1; k < (int)seats.size(); ++k)
    {
        seat &s = seats[k];
        if (!s.conn.ok())
            continue;
        any = true;
        const auto copy_for = pending_copies.find(s.player);
        if (copy_for != pending_copies.end() && !game_id.empty())
        {
            const string &data = copy_for->second.second;
            vector<uint8_t> copy;
            _put_string(copy, copy_for->second.first);
            _put_string(copy, game_id);
            _put32(copy, s.player);
            copy.insert(copy.end(), data.begin(), data.end());
            s.conn.send_msg('V', copy);
        }
        if (delete_copies && !game_id.empty())
        {
            vector<uint8_t> del;
            _put_string(del, game_id);
            s.conn.send_msg('D', del);
        }
        s.conn.send_msg('R', vector<uint8_t>(why.begin(), why.end()));
    }
    // A save is a few megabytes at most: wait (a while) for it all to go
    // before hanging up.
    const unsigned int start = SDL_GetTicks();
    while (any && SDL_GetTicks() - start < 30000)
    {
        any = false;
        for (int k = 1; k < (int)seats.size(); ++k)
            if (seats[k].conn.flush() && seats[k].conn.backed_up())
                any = true;
        if (any)
            SDL_Delay(5);
    }
    for (int k = 1; k < (int)seats.size(); ++k)
        seats[k].conn.hang_up(3000);
    pending_copies.clear();
    lan_host_stop();
}

string lan_player_name(int idx)
{
    const int k = lan_seat_of_player(idx);
    if (k <= 0)
        return "";
    // (It comes right after they join.)
    seat &s = seats[k];
    while (s.conn.ok() && !s.read_copies(SDL_GetTicks())
           && !crawl_state.seen_hups)
    {
        s.conn.fill(20);
    }
    return s.name;
}

bool lan_host_wait_for_player(int idx)
{
    ASSERT(idx >= 1);
    if (!lan_host_requested())
        return false;
    // Already here (from the lobby)?
    if (lan_seat_of_player(idx) > 0)
        return true;
    // The lobby settled who plays where: anyone without a computer then
    // plays here.
    if (lobby_party > 0)
        return false;
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
    if (k <= 0 && capture_host_frame && window)
    {
        _read_back(host_frame_w, host_frame_h, host_frame);
        // Not shown: the host's inventory menu stays up.
        if (capture_only)
            return true;
    }
    if (k <= 0 || !window)
        return false;
    seat &s = seats[k];
    if (!s.conn.ok())
        return false;

    int w, h;
    vector<uint8_t> px;
    _read_back(w, h, px);
    if (px.empty())
        return true;
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

namespace
{
    bool _client_scripted_event(SDL_Event &event, int timeout);
    void _draw_host_frame();

    // Work the host's inventory menu (drawn on the host's screen) without
    // losing the remote player's input.
    void _host_menu(const function<void()> &fn)
    {
        if (!window)
            return;
        unwind_bool busy(host_menu_busy, true);
        lan_seat_scope scope(0);
        fn();
    }

    // The host's keyboard and mouse during a remote player's turn: 'i'
    // opens player 1's inventory menu, which then takes them.
    void _host_idle_input(const SDL_Event &event)
    {
        if (!party_active() || !crawl_state.need_save)
            return;
        if (host_inv.is_open())
        {
            _host_menu([&event] { host_inv.handle(event); });
            if (!host_inv.is_open())
                _draw_host_frame();
        }
        else if (event.type == SDL_TEXTINPUT
                 && strcmp(event.text.text, "i") == 0
                 && host_inv.has_snapshot())
        {
            _host_menu([] {
                host_inv.open();
                host_inv.draw();
            });
        }
    }
}

bool lan_wait_event(SDL_Event &event, int timeout)
{
    if (!lan_active())
    {
        // A client making its character in its own window (test script).
        if (client_choosing && !client_script.empty())
            return _client_scripted_event(event, timeout);
        return SDL_WaitEventTimeout(&event, timeout);
    }

    const unsigned int start = SDL_GetTicks();
    while (true)
    {
        _poll();
        const int k = _live_seat();
        // The host's turn: the game's own screen replaces that menu.
        if (host_inv.is_open() && _turn_seat() <= 0)
            host_inv.close();
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
            // window itself still works: focus, resizing, closing), except
            // that 'i' shows player 1's inventory.
            if (k > 0 && _is_input(event) && !host_menu_busy)
            {
                _host_idle_input(event);
                continue;
            }
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
        // The host's own screen shows its own player (player 1, or whoever
        // loaded their copy of the game), and any other players played here
        // when it's their turn.
        const int idx = k == 0 ? party_home_player() : seats[k].player;
        party_view_as(idx, [k, idx, force] {
            lan_seat_scope scope(k);
            const bool menu_up = k == 0 && host_inv.is_open();
            redraw_screen();
            {
                unwind_bool capture(capture_host_frame, k == 0);
                // The host's inventory menu stays up over it.
                unwind_bool hidden(capture_only, menu_up);
                tiles.redraw();
            }
            // What they can look at with 'i' until it's their turn
            // (descriptions and all, so only remade when it has changed or
            // a turn has begun).
            const string key = _inventory_key();
            string &last = k == 0 ? host_inv_key : seats[k].inv_key;
            if (force || key != last || k == 0 && !host_inv.has_snapshot())
            {
                last = key;
                const inv_snapshot snap
                    = _inventory_snapshot(party_member_name(idx));
                if (k > 0)
                    seats[k].conn.send_msg('I', _pack(snap));
                else
                    host_inv.set(snap);
            }
            if (menu_up)
                _host_menu([] { host_inv.draw(); });
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

    void _draw_host_frame()
    {
        if (host_frame.empty() || !window)
            return;
        client_view v;
        v.win = window;
        v.frame = host_frame;
        v.w = host_frame_w;
        v.h = host_frame_h;
        v.draw();
    }

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

    // While a client makes its character itself: the test script's keys,
    // typed a key at a time once things have been quiet for a moment.
    bool _client_scripted_event(SDL_Event &event, int timeout)
    {
        static deque<net_event> queued;
        static unsigned int typed_at = 0;
        const unsigned int start = SDL_GetTicks();
        while (true)
        {
            if (!queued.empty())
            {
                event = _to_sdl(queued.front());
                queued.pop_front();
                return true;
            }
            const unsigned int now = SDL_GetTicks();
            if (!client_script.empty() && now - typed_at > 400)
            {
                for (const net_event &e : _script_key(client_script))
                    queued.push_back(e);
                typed_at = now;
                continue;
            }
            if (SDL_WaitEventTimeout(&event, 10))
                return true;
            if (timeout != INT_MAX && (int)(SDL_GetTicks() - start) >= timeout)
                return false;
        }
    }

    void _client_log(const string &text)
    {
        _test_log("LAN client: " + text);
    }

    // The host sent our copy of the game ('V') or said it's over ('D'):
    // keep or delete it, and say what happened.
    string _client_copy_message(uint8_t type, const vector<uint8_t> &p)
    {
        unpacker u(p);
        if (type == 'D')
        {
            const string id = u.str();
            return u.ok && party_delete_copy(id)
                   ? "Your copy of the game has been deleted." : "";
        }
        const string filename = u.str();
        const string id = u.str();
        const int owner = u.u32();
        const string data = u.rest();
        if (!u.ok || data.empty())
            return "The host's copy of the game didn't arrive whole.";
        string err;
        if (!party_store_copy(filename, id, owner, data, err))
            return err;
        return make_stringf("You have a copy of the game now (as Player %d): "
                            "load it from the\nstart menu to host it "
                            "yourself.", owner + 1);
    }

    // Our copies of party games, for the host: who we were in them.
    vector<uint8_t> _client_claims()
    {
        vector<uint8_t> j;
        const vector<party_copy> copies = party_copies();
        _put32(j, copies.size());
        for (const party_copy &pc : copies)
        {
            _put_string(j, pc.game_id);
            _put32(j, pc.owner);
            _put32(j, pc.generation);
        }
        _put_string(j, client_name);
        return j;
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
        string copy_note;
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
                // If the host is continuing a saved game, it gives us our
                // own player back.
                c.send_msg('J', _client_claims());
                show("<white>Connected to " + address + ".</white>\n"
                     "Waiting for the host to let you in (they may still "
                     "be making their character)...");
            }
            // (If the host has gone, still take its last messages: why.)
            const bool alive = c.fill(0) && c.flush();
            if (!alive)
                why = "The host closed the connection.";
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
                    // (A third field: continuing a saved game.)
                    if (p.size() >= 12 && _get32(&p[8]))
                    {
                        show(make_stringf("<white>You're Player %d.</white>\n"
                             "Continuing a saved game: waiting for the "
                             "others to join...", player_no));
                    }
                    else
                    {
                        show(make_stringf("<white>You're Player %d.</white>\n"
                             "Waiting for the host to start the game (they "
                             "may be waiting\nfor other players). Then "
                             "everyone makes their character.", player_no));
                    }
                }
                else if (type == 'D')
                    copy_note = _client_copy_message(type, p);
                else if (type == 'C' && player_no)
                {
                    // Make our character here, while the others make
                    // theirs, and tell the host.
                    newgame_def ng;
                    bool chosen = false;
                    try
                    {
                        unwind_bool choosing(client_choosing, true);
                        chosen = choose_party_member(ng,
                            client_name.empty()
                            ? make_stringf("Player %d", player_no)
                            : client_name);
                    }
                    catch (const game_ended_condition &)
                    {
                        // The window was closed.
                        cancel = true;
                        break;
                    }
                    if (crawl_state.seen_hups)
                        break;
                    vector<uint8_t> pick;
                    pick.push_back(chosen);
                    _put32(pick, chosen ? (uint32_t)ng.species : 0);
                    _put32(pick, chosen ? (uint32_t)ng.job : 0);
                    _put32(pick, chosen ? (uint32_t)ng.weapon : 0);
                    c.send_msg('P', pick);
                    show(chosen ? "<white>Character chosen.</white>\n"
                                  "Waiting for the others to finish..."
                                : "<white>You'll join as a copy of Player "
                                  "1.</white>\nWaiting for the others to "
                                  "finish...");
                }
                else if (type == 'Q' && player_no)
                    show("<white>" + string(p.begin(), p.end())
                         + "</white>");
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
        if (!copy_note.empty())
            why += (why.empty() ? "" : "\n\n") + copy_note;
        if (!got_frame)
        {
            c.close();
            return 0;
        }
        return player_no;
    }
}

void lan_run_client(const string &address, const string &name)
{
    if (!window)
        return;
    last_join_address = address;
    client_name = is_good_name(name, false) ? name : "";

    connection c;
    string why;
    vector<uint8_t> first_frame;
    bool my_turn = false;
    const int player_no = _client_connect(c, address, why, first_frame,
                                          my_turn);
    if (!player_no)
    {
        if (!why.empty())
        {
            _client_log(why);
            _client_message(why);
        }
        return;
    }
    _client_log(make_stringf("playing as Player %d", player_no));

    char *title = SDL_strdup(SDL_GetWindowTitle(window));
    int start_w, start_h;
    SDL_GetWindowSize(window, &start_w, &start_h);

    client_view v;
    v.win = window;
    v.apply(first_frame);
    bool quit = false;
    // What became of our copy of the game (see party.h), if anything.
    string copy_note;
    string script = client_script;
    unsigned int last_frame_at = SDL_GetTicks();
    unsigned int last_saved = 0;
    bool dirty_save = false;
    // Our inventory as of the host's last look (see
    // lan_refresh_idle_views()), to look at while it isn't our turn.
    offturn_inventory inv;

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
        // (If the host has gone, still take its last messages.)
        const bool alive = c.flush() && c.fill(5);
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
            else if (type == 'V' || type == 'D')
                copy_note = _client_copy_message(type, p);
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
                // Our turn: back to the game.
                if (my_turn && inv.is_open())
                {
                    inv.close();
                    redraw = true;
                }
            }
            else if (type == 'I')
            {
                inv_snapshot snap;
                if (_unpack(p, snap))
                {
                    inv.set(snap);
                    inv.draw();
                }
            }
        }
        // (The inventory menu stays up over the game.)
        if (redraw && !inv.is_open())
            v.draw();
        if (!alive || !c.ok())
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
            if (inv.is_open() && _is_input(ev))
            {
                // Our inventory menu takes the keyboard and mouse.
                inv.handle(ev);
                if (!inv.is_open())
                    v.draw();
                moved = false;
                continue;
            }
            switch (ev.type)
            {
            case SDL_QUIT:
                quit = true;
                continue;
            case SDL_WINDOWEVENT:
                v.draw();
                inv.draw();
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
                if (!my_turn && inv.has_snapshot()
                    && strcmp(ev.text.text, "i") == 0)
                {
                    // Look at our inventory here, while the host's frames
                    // (and news of our turn) keep coming.
                    inv.open();
                    inv.draw();
                    moved = false;
                    continue;
                }
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
    {
        if (why.empty())
            why = "The host has left the game.";
        _client_log(why);
        if (!copy_note.empty())
        {
            _client_log(copy_note);
            why += "\n\n" + copy_note;
        }
        _client_message(why);
    }
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

void lan_join_menu(const string &name)
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
    lan_run_client(address, name);
}

#endif // USE_TILE_LOCAL
