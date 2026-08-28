#include "mini_wallet_tui.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#endif

#ifdef MC_TUI_PDCURSES
  #include <curses.h>
#else
  #include <ncurses.h>
#endif

namespace mc::ui {
namespace {

enum { CP_TITLE = 1, CP_OK, CP_WARN, CP_DIM, CP_HDR };

void setup_colors() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_OK,    COLOR_GREEN, -1);
    init_pair(CP_WARN,  COLOR_YELLOW, -1);
    init_pair(CP_DIM,   COLOR_WHITE, -1);
    init_pair(CP_HDR,   COLOR_BLACK, COLOR_CYAN);
}

// ---- stdout/stderr discard sink --------------------------------------
//
// The mini-node keeps relaying while the wallet is on screen, and librats /
// the relay path write to std::cout+std::cerr from their own threads. We do
// not tail those logs in this TUI (a wallet screen has no log page), so the
// streams are simply swallowed for the lifetime of the session. This is also
// the belt-and-braces guarantee that nothing typed here — password, amount,
// recipient — can be echoed into a log by some other component.
class NullBuf : public std::streambuf {
protected:
    int_type overflow(int_type c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

NullBuf*        g_null      = nullptr;
std::streambuf* g_prev_cout = nullptr;
std::streambuf* g_prev_cerr = nullptr;

void silence_stdio() {
    if (g_null) return;
    g_null      = new NullBuf();
    g_prev_cout = std::cout.rdbuf(g_null);
    g_prev_cerr = std::cerr.rdbuf(g_null);
}

void restore_stdio() {
    if (g_prev_cout) std::cout.rdbuf(g_prev_cout);
    if (g_prev_cerr) std::cerr.rdbuf(g_prev_cerr);
    g_prev_cout = nullptr;
    g_prev_cerr = nullptr;
    delete g_null;
    g_null = nullptr;
}

// ---- input primitives ------------------------------------------------

void wipe(std::string& s) {
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}

// Visible single-line prompt drawn near the bottom. False on empty/ESC.
bool prompt_line(const char* title, std::string& out, int max_len = 128) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(COLOR_PAIR(CP_HDR));
    mvhline(y, 0, ' ', cols);
    mvprintw(y, 1, "%s", title);
    attroff(COLOR_PAIR(CP_HDR));
    move(y + 1, 0);
    clrtoeol();
    mvprintw(y + 1, 1, "> ");
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);
    std::vector<char> buf(static_cast<size_t>(max_len) + 1, 0);
    move(y + 1, 3);
    getnstr(buf.data(), max_len);
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    out.assign(buf.data());
    while (!out.empty() &&
           (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}

// Masked prompt. Characters are echoed as '*' and never leave this buffer.
bool prompt_secret(const char* title, std::string& out, int max_len = 128) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(COLOR_PAIR(CP_HDR));
    mvhline(y, 0, ' ', cols);
    mvprintw(y, 1, "%s", title);
    attroff(COLOR_PAIR(CP_HDR));
    move(y + 1, 0);
    clrtoeol();
    mvprintw(y + 1, 1, "> ");
    curs_set(1);
    noecho();
    nodelay(stdscr, FALSE);
    move(y + 1, 3);
    std::string buf;
    while (static_cast<int>(buf.size()) < max_len) {
        int ch = getch();
        if (ch == ERR) continue;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
        if (ch == 27) { wipe(buf); break; }
        if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
            if (!buf.empty()) {
                buf.pop_back();
                int cy, cx;
                getyx(stdscr, cy, cx);
                if (cx > 0) { mvaddch(cy, cx - 1, ' '); move(cy, cx - 1); }
            }
            continue;
        }
        if (ch >= 32 && ch < 127) { buf.push_back(static_cast<char>(ch)); addch('*'); }
    }
    curs_set(0);
    nodelay(stdscr, TRUE);
    out = buf;
    wipe(buf);
    return !out.empty();
}

int blocking_key() {
    nodelay(stdscr, FALSE);
    int c = getch();
    nodelay(stdscr, TRUE);
    return c;
}

// ---- shared screen state --------------------------------------------

enum class Screen { Locked, Wallet, Receive };

std::mutex  g_bal_mu;
std::string g_balance;          // last fetched balance, "" = unknown
std::string g_balance_note;     // "" or a short reason it's unavailable
std::string g_status;           // last action result
int         g_status_color = CP_DIM;

std::string balance_display() {
    std::lock_guard<std::mutex> lk(g_bal_mu);
    if (!g_balance.empty()) return g_balance + " BOP";
    return g_balance_note.empty() ? std::string("(fetching...)") : g_balance_note;
}

// ---- drawing ---------------------------------------------------------

void draw_header(const WalletTuiState& st, bool locked) {
    int cols = getmaxx(stdscr);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, " %s ", st.title.c_str());
    const char* chip = locked ? "[ LOCKED ]" : "[ UNLOCKED ]";
    int tx = cols - static_cast<int>(std::string(chip).size()) - 1;
    if (tx > static_cast<int>(st.title.size()) + 4) mvprintw(0, tx, "%s", chip);
    attroff(COLOR_PAIR(CP_HDR));
}

void draw_footer(const char* keys) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, "%s", keys);
    attroff(COLOR_PAIR(CP_HDR));
}

void draw_status(int row) {
    if (g_status.empty()) return;
    attron(COLOR_PAIR(g_status_color));
    mvprintw(row, 2, "%s", g_status.c_str());
    attroff(COLOR_PAIR(g_status_color));
}

void draw_locked(const WalletTuiState& st) {
    erase();
    draw_header(st, true);
    attron(COLOR_PAIR(CP_TITLE));
    mvprintw(3, 4, "Wallet locked.");
    attroff(COLOR_PAIR(CP_TITLE));
    mvprintw(5, 4, "Press U and enter the wallet password to unlock.");
    attron(A_DIM);
    mvprintw(7, 4, "The mini-node keeps relaying while the wallet is locked.");
    attroff(A_DIM);
    draw_status(9);
    draw_footer(" U Unlock   Q Quit ");
    refresh();
}

void draw_wallet(const WalletTuiState& st) {
    erase();
    draw_header(st, false);
    auto row = [](int r, const char* label, const std::string& val, int cp) {
        attron(COLOR_PAIR(CP_TITLE));
        mvprintw(r, 2, "%-10s", label);
        attroff(COLOR_PAIR(CP_TITLE));
        attron(COLOR_PAIR(cp));
        mvprintw(r, 14, "%s", val.c_str());
        attroff(COLOR_PAIR(cp));
    };
    row(3, "Address", st.address, CP_OK);
    row(5, "Balance", balance_display(), CP_DIM);
    attron(A_DIM);
    mvprintw(8, 2, "S  Send tokens");
    mvprintw(9, 2, "R  Receive (show address)");
    mvprintw(10, 2, "F  Refresh balance");
    mvprintw(11, 2, "L  Lock");
    attroff(A_DIM);
    draw_status(13);
    draw_footer(" S Send   R Receive   F Refresh   L Lock   Q Quit ");
    refresh();
}

void draw_receive(const WalletTuiState& st) {
    erase();
    draw_header(st, false);
    attron(COLOR_PAIR(CP_TITLE));
    mvprintw(3, 4, "Receive to this address:");
    attroff(COLOR_PAIR(CP_TITLE));
    attron(COLOR_PAIR(CP_OK) | A_BOLD);
    mvprintw(5, 4, "%s", st.address.c_str());
    attroff(COLOR_PAIR(CP_OK) | A_BOLD);
    attron(A_DIM);
    mvprintw(7, 4, "Standard 20-byte EVM-style address (EIP-55 checksummed).");
    mvprintw(8, 4, "Safe to publish - it reveals nothing about the seed.");
    attroff(A_DIM);
    draw_footer(" B Back   Q Quit ");
    refresh();
}

// ---- actions ---------------------------------------------------------

// Ask for the password and check it against the keystore. Returns true when
// it verifies (or when the caller supplied no verifier at all).
bool authorize(const WalletTuiState& st, const char* title) {
    if (!st.verify_password) return true;
    std::string pw;
    if (!prompt_secret(title, pw)) { wipe(pw); return false; }
    const bool ok = st.verify_password(pw);
    wipe(pw);
    return ok;
}

void action_send(const WalletTuiState& st) {
    if (!st.send) {
        g_status = "send: no full node connection available";
        g_status_color = CP_WARN;
        return;
    }
    std::string to;
    if (!prompt_line("Recipient address (0x...)", to)) {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    std::string amount;
    if (!prompt_line("Amount (e.g. 1.5)", amount, 32)) {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    // Confirm before we ask for the password, so a mistyped recipient is
    // caught while the operator is still looking at it.
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    move(rows - 6, 0); clrtoeol();
    attron(COLOR_PAIR(CP_WARN));
    mvprintw(rows - 6, 1, "Send %s to %s ?  [y/N]",
             amount.c_str(), to.c_str());
    attroff(COLOR_PAIR(CP_WARN));
    refresh();
    const int c = blocking_key();
    if (c != 'y' && c != 'Y') {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    if (!authorize(st, "Wallet password to authorize this transfer")) {
        g_status = "send: wrong password - transfer NOT sent";
        g_status_color = CP_WARN;
        return;
    }
    std::string msg;
    const bool ok = st.send(to, amount, msg);
    g_status = (ok ? "sent: " : "send failed: ") + msg;
    g_status_color = ok ? CP_OK : CP_WARN;
}

} // namespace

void run_wallet_tui(const WalletTuiState& st, std::atomic<bool>& running) {
#ifdef _WIN32
    HANDLE conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE conin  = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (conout != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, conout);
        SetStdHandle(STD_ERROR_HANDLE,  conout);
    }
    if (conin != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, conin);
#endif

    initscr();
    if (stdscr == nullptr) {
        // No terminal (e.g. --tui under a pipe). Stay alive as a plain relay.
        while (running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }

#ifdef _WIN32
    int nul_fd = _open("NUL", _O_WRONLY);
    if (nul_fd >= 0) {
        std::fflush(stdout);
        std::fflush(stderr);
        _dup2(nul_fd, _fileno(stdout));
        _dup2(nul_fd, _fileno(stderr));
        _close(nul_fd);
        if (conout != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, conout);
            SetStdHandle(STD_ERROR_HANDLE,  conout);
        }
    }
#endif

    silence_stdio();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    setup_colors();

    {
        std::lock_guard<std::mutex> lk(g_bal_mu);
        g_balance.clear();
        g_balance_note.clear();
    }
    g_status.clear();
    g_status_color = CP_DIM;

    // Balance refresher. st.balance() talks to a full node over librats and
    // can block for seconds, so it never runs on the draw loop.
    std::mutex              rm;
    std::condition_variable rcv;
    bool                    refresh_now = false;
    std::atomic<bool>       unlocked{false};
    std::atomic<bool>       refresher_running{true};
    std::thread refresher([&]() {
        while (refresher_running.load()) {
            bool want = false;
            {
                std::unique_lock<std::mutex> lk(rm);
                rcv.wait_for(lk, std::chrono::seconds(15),
                             [&] { return refresh_now || !refresher_running.load(); });
                want = refresh_now || unlocked.load();
                refresh_now = false;
            }
            if (!refresher_running.load()) break;
            // Only query while the wallet is unlocked — a locked screen shows
            // nothing, so there is no reason to keep asking the network.
            if (!want || !unlocked.load() || !st.balance) continue;
            const std::string b = st.balance();
            std::lock_guard<std::mutex> lk(g_bal_mu);
            if (b.empty()) {
                g_balance.clear();
                g_balance_note = "(unavailable - no full node reached)";
            } else {
                g_balance      = b;
                g_balance_note.clear();
            }
        }
    });
    auto kick_refresh = [&]() {
        { std::lock_guard<std::mutex> lk(rm); refresh_now = true; }
        rcv.notify_all();
    };

    Screen screen = st.verify_password ? Screen::Locked : Screen::Wallet;
    unlocked.store(screen != Screen::Locked);
    if (unlocked.load()) kick_refresh();

    auto last_input = std::chrono::steady_clock::now();
    auto last_draw  = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();

        if (st.auto_lock_seconds > 0 && screen != Screen::Locked &&
            now - last_input >= std::chrono::seconds(st.auto_lock_seconds)) {
            screen = Screen::Locked;
            unlocked.store(false);
            {
                std::lock_guard<std::mutex> lk(g_bal_mu);
                g_balance.clear();
                g_balance_note.clear();
            }
            g_status = "auto-locked after " +
                       std::to_string(st.auto_lock_seconds) + "s idle";
            g_status_color = CP_DIM;
            last_draw = now - std::chrono::seconds(2);
        }

        if (now - last_draw >= std::chrono::seconds(1)) {
            switch (screen) {
                case Screen::Locked:  draw_locked(st);  break;
                case Screen::Wallet:  draw_wallet(st);  break;
                case Screen::Receive: draw_receive(st); break;
            }
            last_draw = now;
        }

        const int key = getch();
        if (key == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        last_input = now;
        last_draw  = now - std::chrono::seconds(2);   // redraw right after

        if (key == 'q' || key == 'Q' || key == 3) { running.store(false); break; }

        if (screen == Screen::Locked) {
            if (key == 'u' || key == 'U' || key == '\n' || key == '\r' ||
                key == KEY_ENTER) {
                if (authorize(st, "Wallet password")) {
                    screen = Screen::Wallet;
                    unlocked.store(true);
                    g_status.clear();
                    g_status_color = CP_DIM;
                    kick_refresh();
                } else {
                    g_status = "wrong password";
                    g_status_color = CP_WARN;
                }
            }
            continue;
        }

        if (screen == Screen::Receive) {
            if (key == 'b' || key == 'B' || key == 27) screen = Screen::Wallet;
            continue;
        }

        // Screen::Wallet
        if (key == 's' || key == 'S')      { action_send(st); kick_refresh(); }
        else if (key == 'r' || key == 'R') { screen = Screen::Receive; }
        else if (key == 'f' || key == 'F') { kick_refresh(); }
        else if (key == 'l' || key == 'L' || key == 27) {
            screen = Screen::Locked;
            unlocked.store(false);
            {
                std::lock_guard<std::mutex> lk(g_bal_mu);
                g_balance.clear();
                g_balance_note.clear();
            }
            g_status = "locked";
            g_status_color = CP_DIM;
        }
    }

    refresher_running.store(false);
    rcv.notify_all();
    if (refresher.joinable()) refresher.join();

    endwin();
    restore_stdio();
}

} // namespace mc::ui
