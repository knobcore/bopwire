#include "monitor_tui.h"

#include "../src/crypto/bip39.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/keystore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
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

std::string g_status;          // last action result (export/import/wallet)
int         g_status_color = CP_DIM;

// ---- Optional wallet page (F3) state ---------------------------------
// Only reachable when MonitorState::wallet_unlock is set. Kept here rather
// than in the caller so the page behaves identically in every program that
// switches it on.
bool g_wallet_unlocked = false;
bool g_wallet_receive  = false;   // showing the big receive address panel
std::chrono::steady_clock::time_point g_wallet_last_key =
    std::chrono::steady_clock::now();

void setup_colors() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_OK, COLOR_GREEN, -1);
    init_pair(CP_WARN, COLOR_YELLOW, -1);
    init_pair(CP_DIM, COLOR_WHITE, -1);
    init_pair(CP_HDR, COLOR_BLACK, COLOR_CYAN);
}

// ---- Log capture (F2) ------------------------------------------------
// Every std::cout / std::cerr line (librats logger, chain, rats_api, ...)
// is diverted into a bounded ring so the node's chatter shows on F2
// instead of trampling the curses screen.

struct LogRing {
    std::mutex              m;
    std::deque<std::string> lines;
    size_t                  cap = 1000;

    void push(std::string l) {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(std::move(l));
        while (lines.size() > cap) lines.pop_front();
    }
    std::vector<std::string> tail(size_t n) {
        std::lock_guard<std::mutex> g(m);
        std::vector<std::string> out;
        const size_t start = lines.size() > n ? lines.size() - n : 0;
        out.reserve(lines.size() - start);
        for (size_t i = start; i < lines.size(); ++i) out.push_back(lines[i]);
        return out;
    }
};

LogRing g_logs;

class RingStreambuf : public std::streambuf {
public:
    explicit RingStreambuf(LogRing& r) : ring_(r) {}
protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return c;
        std::lock_guard<std::mutex> g(write_m_);
        absorb_(static_cast<char>(c));
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> g(write_m_);
        for (std::streamsize i = 0; i < n; ++i) absorb_(s[i]);
        return n;
    }
private:
    void absorb_(char ch) {           // caller holds write_m_
        if (in_ansi_) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
                in_ansi_ = false;
            return;
        }
        if (ch == '\x1b') { in_ansi_ = true; return; }
        if (ch == '\r')   return;
        if (ch == '\n') { ring_.push(std::move(line_)); line_.clear(); return; }
        line_.push_back(ch);
    }
    LogRing&    ring_;
    std::mutex  write_m_;
    std::string line_;
    bool        in_ansi_ = false;
};

RingStreambuf* g_rb_cout = nullptr;
RingStreambuf* g_rb_cerr = nullptr;
std::streambuf* g_prev_cout = nullptr;
std::streambuf* g_prev_cerr = nullptr;

// ---- Curses input primitives -----------------------------------------

// Visible single-line prompt. Returns false on empty input.
bool prompt_string(const char* title, std::string& out, int max_len = 200) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(COLOR_PAIR(CP_HDR));
    mvhline(y, 0, ' ', cols);
    mvprintw(y, 1, "%s", title);
    attroff(COLOR_PAIR(CP_HDR));
    mvhline(y + 1, 0, ' ', cols);      // wipe whatever the last prompt echoed
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
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    // Force a full repaint next refresh: if the terminal was resized after
    // initscr(), curses' model is smaller than the tty and an erase() alone
    // can leave the typed line on screen.
    clearok(stdscr, TRUE);
    return !out.empty();
}

// Hidden secret prompt (echoes '*'). Returns false on empty / ESC.
bool prompt_secret(const char* title, std::string& out, int max_len = 128) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(COLOR_PAIR(CP_HDR));
    mvhline(y, 0, ' ', cols);
    mvprintw(y, 1, "%s", title);
    attroff(COLOR_PAIR(CP_HDR));
    mvhline(y + 1, 0, ' ', cols);      // wipe whatever the last prompt echoed
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
        if (ch == 27) { buf.clear(); break; }
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
    clearok(stdscr, TRUE);
    out = std::move(buf);
    return !out.empty();
}

// Modal yes/no on the prompt line. True only on an explicit 'y'.
bool prompt_confirm(const std::string& question) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(COLOR_PAIR(CP_WARN));
    mvhline(y, 0, ' ', cols);
    mvprintw(y, 1, "%s  [y/N]", question.c_str());
    attroff(COLOR_PAIR(CP_WARN));
    mvhline(y + 1, 0, ' ', cols);      // wipe the echoed input row
    refresh();
    nodelay(stdscr, FALSE);
    const int c = getch();
    nodelay(stdscr, TRUE);
    clearok(stdscr, TRUE);
    return c == 'y' || c == 'Y';
}

std::string read_seed(const std::string& path) {
    std::ifstream f(path);
    std::string m;
    if (f) std::getline(f, m);
    while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ')) m.pop_back();
    return m;
}

// ---- Export / import of this node's own identity seed -----------------

void action_export(const MonitorState& st) {
    std::string mnemonic = read_seed(st.seed_path);
    if (mnemonic.empty()) {
        g_status = "export: no seed at " + st.seed_path; g_status_color = CP_WARN; return;
    }
    std::string pass;
    if (!prompt_secret("Export passphrase (>=12 chars, 1 upper, 1 special)", pass)) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        g_status = "export: cancelled"; g_status_color = CP_WARN; return;
    }
    const std::string perr = mc::crypto::password_policy_error(pass);
    if (!perr.empty()) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        std::fill(pass.begin(), pass.end(), '\0');
        g_status = "export: " + perr; g_status_color = CP_WARN; return;
    }
    std::string confirm;
    if (!prompt_secret("Confirm passphrase", confirm) || confirm != pass) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        std::fill(pass.begin(), pass.end(), '\0');
        g_status = "export: passphrases do not match"; g_status_color = CP_WARN; return;
    }
    std::string ks = mc::crypto::keystore_encrypt(mnemonic, pass);
    std::fill(mnemonic.begin(), mnemonic.end(), '\0');
    std::fill(pass.begin(), pass.end(), '\0');
    std::fill(confirm.begin(), confirm.end(), '\0');
    if (ks.empty()) { g_status = "export: encryption failed"; g_status_color = CP_WARN; return; }
    std::string out_path;
    if (!prompt_string("Save keystore to path (e.g. /root/bopwire-wallet.json)", out_path)) {
        g_status = "export: cancelled"; g_status_color = CP_WARN; return;
    }
    std::ofstream of(out_path, std::ios::trunc | std::ios::binary);
    if (!of) { g_status = "export: cannot write " + out_path; g_status_color = CP_WARN; return; }
    of << ks;
    of.close();
    g_status = "export: wrote " + out_path; g_status_color = CP_OK;
}

void action_import(const MonitorState& st) {
    std::string in_path;
    if (!prompt_string("Keystore file to import", in_path)) {
        g_status = "import: cancelled"; g_status_color = CP_WARN; return;
    }
    std::ifstream inf(in_path, std::ios::binary);
    if (!inf) { g_status = "import: cannot read " + in_path; g_status_color = CP_WARN; return; }
    std::stringstream ss;
    ss << inf.rdbuf();
    inf.close();
    std::string pass;
    if (!prompt_secret("Import passphrase", pass)) {
        g_status = "import: cancelled"; g_status_color = CP_WARN; return;
    }
    std::string mnemonic;
    const bool ok = mc::crypto::keystore_decrypt(ss.str(), pass, mnemonic);
    std::fill(pass.begin(), pass.end(), '\0');
    if (!ok) { g_status = "import: wrong passphrase or corrupt file"; g_status_color = CP_WARN; return; }
    auto kp = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
    if (!kp) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        g_status = "import: not a valid wallet mnemonic"; g_status_color = CP_WARN; return;
    }
    std::ofstream sf(st.seed_path, std::ios::trunc);
    if (!sf) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        g_status = "import: cannot write " + st.seed_path; g_status_color = CP_WARN; return;
    }
    sf << mnemonic << "\n";
    sf.close();
    const std::string addr = mc::crypto::to_checksum_hex(kp->address);
    std::fill(mnemonic.begin(), mnemonic.end(), '\0');
    g_status = "import: " + addr.substr(0, 12) + " -> seed. RESTART to apply.";
    g_status_color = CP_OK;
}

// ---- Wallet page (F3) actions -----------------------------------------
//
// Strictly the node operator's OWN wallet: unlock, look, send, receive. No
// founder / moderator / bootstrap affordance is offered here or anywhere
// else in this module.

std::string call_or_empty(const std::function<std::string()>& f) {
    return f ? f() : std::string();
}

void wallet_touch() { g_wallet_last_key = std::chrono::steady_clock::now(); }

void action_wallet_unlock(const MonitorState& st) {
    if (!st.wallet_unlock) return;
    std::string pw;
    if (!prompt_secret("Wallet password", pw)) {
        std::fill(pw.begin(), pw.end(), '\0');
        g_status = "unlock: cancelled"; g_status_color = CP_WARN; return;
    }
    const bool ok = st.wallet_unlock(pw);
    std::fill(pw.begin(), pw.end(), '\0');
    g_wallet_unlocked = ok;
    g_wallet_receive  = false;
    wallet_touch();
    g_status = ok ? "wallet unlocked" : "unlock: wrong password";
    g_status_color = ok ? CP_OK : CP_WARN;
}

void action_wallet_send(const MonitorState& st) {
    if (!st.wallet_send) return;
    std::string to;
    if (!prompt_string("Recipient address (0x...)", to, 64)) {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    std::string amount;
    if (!prompt_string("Amount (e.g. 12.5)", amount, 40)) {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    if (!prompt_confirm("Send " + amount + " to " + to + "?")) {
        g_status = "send: cancelled"; g_status_color = CP_WARN; return;
    }
    // The caller owns parsing, the balance check, signing and submission —
    // this module never touches the chain.
    const auto res = st.wallet_send(to, amount);
    g_status = res.second;
    g_status_color = res.first ? CP_OK : CP_WARN;
}

void draw_wallet(const MonitorState& st) {
    int r = 2;
    if (!g_wallet_unlocked) {
        attron(COLOR_PAIR(CP_WARN));
        mvprintw(r, 2, "Wallet locked.");
        attroff(COLOR_PAIR(CP_WARN));
        r += 2;
        mvprintw(r++, 2, "Press U and enter the wallet password to open it.");
        r++;
        attron(A_DIM);
        mvprintw(r++, 2, "(the same password this node was started with)");
        attroff(A_DIM);
    } else if (g_wallet_receive) {
        attron(COLOR_PAIR(CP_TITLE));
        mvprintw(r, 2, "Receive");
        attroff(COLOR_PAIR(CP_TITLE));
        r += 2;
        mvprintw(r++, 2, "Send tokens to this address:");
        r++;
        attron(A_BOLD | COLOR_PAIR(CP_OK));
        mvprintw(r++, 4, "%s", call_or_empty(st.wallet_address).c_str());
        attroff(A_BOLD | COLOR_PAIR(CP_OK));
        r++;
        attron(A_DIM);
        mvprintw(r++, 2, "Press B to go back.");
        attroff(A_DIM);
    } else {
        auto row = [&](const char* label, const std::string& val, int cp) {
            attron(COLOR_PAIR(CP_TITLE));
            mvprintw(r, 2, "%-9s", label);
            attroff(COLOR_PAIR(CP_TITLE));
            attron(COLOR_PAIR(cp));
            mvprintw(r, 13, "%s", val.c_str());
            attroff(COLOR_PAIR(cp));
            r += 2;
        };
        row("Address", call_or_empty(st.wallet_address), CP_OK);
        row("Balance", call_or_empty(st.balance), CP_DIM);
        r++;
        mvprintw(r++, 2, "[S] Send tokens   [R] Receive   [L] Lock");
    }
    if (!g_status.empty()) {
        r++;
        attron(COLOR_PAIR(g_status_color));
        mvprintw(r, 2, "%s", g_status.c_str());
        attroff(COLOR_PAIR(g_status_color));
    }
}

// ---- Drawing ---------------------------------------------------------

void draw_header(const MonitorState& st, int page) {
    int cols = getmaxx(stdscr);
    const bool has_wallet = static_cast<bool>(st.wallet_unlock);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(0, 0, ' ', cols);
    std::string t = " " + st.title + " ";
    mvprintw(0, 1, "%s", t.c_str());
    std::string tabs = (page == 1) ? "[F1 Status] F2 Logs"
                     : (page == 2) ? "F1 Status [F2 Logs]"
                                   : "F1 Status  F2 Logs";
    if (has_wallet) tabs += (page == 3) ? " [F3 Wallet]" : " F3 Wallet";
    int tx = cols - static_cast<int>(tabs.size()) - 1;
    if (tx > static_cast<int>(t.size()) + 1) mvprintw(0, tx, "%s", tabs.c_str());
    attroff(COLOR_PAIR(CP_HDR));
}

void draw_footer(const MonitorState& st, int page) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(rows - 1, 0, ' ', cols);
    std::string f = " F1 Status   F2 Logs   ";
    if (st.wallet_unlock) f += "F3 Wallet   ";
    if (page == 3) {
        if (!g_wallet_unlocked)     f += "U Unlock   ";
        else if (g_wallet_receive)  f += "B Back   ";
        else                        f += "S Send   R Receive   L Lock   ";
    } else if (!st.seed_path.empty()) {
        f += "X Export   P Import   ";
    }
    f += "Q Quit ";
    mvprintw(rows - 1, 1, "%s", f.c_str());
    attroff(COLOR_PAIR(CP_HDR));
}

void draw_status(const MonitorState& st, std::chrono::steady_clock::time_point start) {
    int r = 2;
    auto row = [&](const char* label, const std::string& val, int cp = CP_DIM) {
        if (val.empty()) return;
        attron(COLOR_PAIR(CP_TITLE));
        mvprintw(r, 2, "%-13s", label);
        attroff(COLOR_PAIR(CP_TITLE));
        attron(COLOR_PAIR(cp));
        mvprintw(r, 17, "%s", val.c_str());
        attroff(COLOR_PAIR(cp));
        r++;
    };
    auto call = [](const std::function<std::string()>& f) -> std::string {
        return f ? f() : std::string();
    };

    row("Wallet", call(st.wallet_address), CP_OK);
    row("Balance", call(st.balance));
    row("Escrow", call(st.escrow));
    row("Chain ht", call(st.chain_height));
    row("Songs", call(st.songs));
    row("Peers", call(st.peers));
    row("Routes", call(st.routes));
    row("Players", call(st.players));
    row("Rats port", call(st.rats_port));

    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count();
    row("Uptime", std::to_string(up) + "s");
    if (!st.seed_path.empty()) row("Seed file", st.seed_path);

    if (!g_status.empty()) {
        r++;
        attron(COLOR_PAIR(g_status_color));
        mvprintw(r, 2, "%s", g_status.c_str());
        attroff(COLOR_PAIR(g_status_color));
    }
}

void draw_logs() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int top = 2;
    const int inner_h = rows - 1 - top;
    auto tail = g_logs.tail(inner_h > 0 ? inner_h : 0);
    int r = top;
    int avail = cols - 3;
    for (auto& s : tail) {
        if ((int)s.size() > avail) s.resize(avail);
        mvprintw(r++, 1, "%s", s.c_str());
        if (r >= rows - 1) break;
    }
    if (tail.empty()) {
        attron(A_DIM);
        mvprintw(top, 2, "(no log lines captured yet)");
        attroff(A_DIM);
    }
}

void draw(const MonitorState& st, int page, std::chrono::steady_clock::time_point start) {
    erase();
    draw_header(st, page);
    if      (page == 1) draw_status(st, start);
    else if (page == 3) draw_wallet(st);
    else                draw_logs();
    draw_footer(st, page);
    refresh();
}

}  // namespace

void monitor_start_log_capture() {
    if (g_rb_cout) return;
    g_rb_cout   = new RingStreambuf(g_logs);
    g_rb_cerr   = new RingStreambuf(g_logs);
    g_prev_cout = std::cout.rdbuf(g_rb_cout);
    g_prev_cerr = std::cerr.rdbuf(g_rb_cerr);
}

void monitor_stop_log_capture() {
    if (g_prev_cout) std::cout.rdbuf(g_prev_cout);
    if (g_prev_cerr) std::cerr.rdbuf(g_prev_cerr);
    g_prev_cout = nullptr;
    g_prev_cerr = nullptr;
    delete g_rb_cout; g_rb_cout = nullptr;
    delete g_rb_cerr; g_rb_cerr = nullptr;
}

void run_monitor_tui(const MonitorState& st, std::atomic<bool>& running) {
#ifdef _WIN32
    HANDLE conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE conin  = CreateFileA("CONIN$",  GENERIC_READ | GENERIC_WRITE,
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
        while (running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    setup_colors();

    const bool can_seed_io = !st.seed_path.empty();
    const bool has_wallet   = static_cast<bool>(st.wallet_unlock);
    g_wallet_unlocked = false;      // always start locked
    g_wallet_receive  = false;
    wallet_touch();
    int page = 1;
    const auto start = std::chrono::steady_clock::now();
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        // Idle re-lock: an unattended terminal must not leave the wallet open.
        if (has_wallet && g_wallet_unlocked && st.wallet_idle_lock_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - g_wallet_last_key)
                .count() > st.wallet_idle_lock_seconds) {
            g_wallet_unlocked = false;
            g_wallet_receive  = false;
            g_status = "wallet re-locked (idle)";
            g_status_color = CP_DIM;
            last = now - std::chrono::seconds(2);
        }
        if (now - last >= std::chrono::seconds(1)) { draw(st, page, start); last = now; }
        const int key = getch();
        if (key == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        wallet_touch();
        const auto redraw = [&] { last = now - std::chrono::seconds(2); };
        if (key == KEY_RESIZE) { clearok(stdscr, TRUE); redraw(); continue; }
        // ESC leaves the wallet page rather than killing the node.
        if (key == 27 && page == 3) { page = 1; redraw(); continue; }
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) { running.store(false); break; }
        else if (key == KEY_F(1)) { page = 1; redraw(); }
        else if (key == KEY_F(2)) { page = 2; redraw(); }
        else if (has_wallet && key == KEY_F(3)) { page = 3; g_status.clear(); redraw(); }
        else if (page == 3 && !g_wallet_unlocked && (key == 'u' || key == 'U')) {
            action_wallet_unlock(st); redraw();
        }
        else if (page == 3 && g_wallet_unlocked && g_wallet_receive &&
                 (key == 'b' || key == 'B')) { g_wallet_receive = false; redraw(); }
        else if (page == 3 && g_wallet_unlocked && !g_wallet_receive) {
            if      (key == 's' || key == 'S') { action_wallet_send(st); redraw(); }
            else if (key == 'r' || key == 'R') { g_wallet_receive = true; g_status.clear(); redraw(); }
            else if (key == 'l' || key == 'L') {
                g_wallet_unlocked = false;
                g_status = "wallet locked"; g_status_color = CP_DIM; redraw();
            }
        }
        else if (page != 3 && can_seed_io && (key == 'x' || key == 'X')) { action_export(st); redraw(); }
        else if (page != 3 && can_seed_io && (key == 'p' || key == 'P')) { action_import(st); redraw(); }
    }
    // Never leave a decrypted wallet flagged open for a later run.
    g_wallet_unlocked = false;
    g_wallet_receive  = false;
    endwin();
}

}  // namespace mc::ui
