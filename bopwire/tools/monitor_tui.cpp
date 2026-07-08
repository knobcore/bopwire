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

std::string g_status;          // last action result (export/import)
int         g_status_color = CP_DIM;

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
    out = std::move(buf);
    return !out.empty();
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

// ---- Drawing ---------------------------------------------------------

void draw_header(const MonitorState& st, int page) {
    int cols = getmaxx(stdscr);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(0, 0, ' ', cols);
    std::string t = " " + st.title + " ";
    mvprintw(0, 1, "%s", t.c_str());
    const char* tabs = (page == 1) ? "[F1 Status] F2 Logs" : "F1 Status [F2 Logs]";
    int tx = cols - static_cast<int>(std::string(tabs).size()) - 1;
    if (tx > static_cast<int>(t.size()) + 1) mvprintw(0, tx, "%s", tabs);
    attroff(COLOR_PAIR(CP_HDR));
}

void draw_footer(const MonitorState& st) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(rows - 1, 0, ' ', cols);
    std::string f = " F1 Status   F2 Logs   ";
    if (!st.seed_path.empty()) f += "X Export   P Import   ";
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
    if (page == 1) draw_status(st, start);
    else           draw_logs();
    draw_footer(st);
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

    const bool can_wallet = !st.seed_path.empty();
    int page = 1;
    const auto start = std::chrono::steady_clock::now();
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) { draw(st, page, start); last = now; }
        const int key = getch();
        if (key == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) { running.store(false); break; }
        else if (key == KEY_F(1)) { page = 1; last = now - std::chrono::seconds(2); }
        else if (key == KEY_F(2)) { page = 2; last = now - std::chrono::seconds(2); }
        else if (can_wallet && (key == 'x' || key == 'X')) { action_export(st); last = now - std::chrono::seconds(2); }
        else if (can_wallet && (key == 'p' || key == 'P')) { action_import(st); last = now - std::chrono::seconds(2); }
    }
    endwin();
}

}  // namespace mc::ui
