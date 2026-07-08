#include "mini_node_tui.h"

#include "../src/crypto/bip39.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/keystore.h"

#ifdef MC_TUI_PDCURSES
  #include <curses.h>
#else
  #include <ncurses.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mc::mini {
namespace {

enum { CP_TITLE = 1, CP_OK, CP_WARN, CP_DIM, CP_HDR };

std::string g_status;          // last action result
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

void action_export(const MiniTuiState& st) {
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

void action_import(const MiniTuiState& st) {
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
    g_status = "import: " + addr.substr(0, 12) + " -> seed. RESTART the mini-node to apply.";
    g_status_color = CP_OK;
}

void draw(const MiniTuiState& st, std::chrono::steady_clock::time_point start) {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(CP_HDR));
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, " Bopwire mini-node ");
    attroff(COLOR_PAIR(CP_HDR));

    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count();
    int r = 2;
    auto line = [&](const char* label, const std::string& val, int cp = CP_DIM) {
        attron(COLOR_PAIR(CP_TITLE));
        mvprintw(r, 2, "%-13s", label);
        attroff(COLOR_PAIR(CP_TITLE));
        attron(COLOR_PAIR(cp));
        mvprintw(r, 17, "%s", val.c_str());
        attroff(COLOR_PAIR(cp));
        r++;
    };
    line("Wallet", st.wallet_address ? st.wallet_address() : "?", CP_OK);
    line("Rats port", std::to_string(st.rats_port));
    line("Peers", std::to_string(st.peer_count ? st.peer_count() : 0));
    line("Routes", std::to_string(st.route_count ? st.route_count() : 0));
    if (st.player_count) line("Players", std::to_string(st.player_count()));
    line("Uptime", std::to_string(up) + "s");
    line("Seed file", st.seed_path);
    r++;
    if (!g_status.empty()) {
        attron(COLOR_PAIR(g_status_color));
        mvprintw(r, 2, "%s", g_status.c_str());
        attroff(COLOR_PAIR(g_status_color));
    }

    attron(COLOR_PAIR(CP_HDR));
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, " X Export   P Import   Q Quit ");
    attroff(COLOR_PAIR(CP_HDR));
    refresh();
}

}  // namespace

void run_mini_tui(const MiniTuiState& st, std::atomic<bool>& running) {
    initscr();
    if (stdscr == nullptr) {
        while (running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    setup_colors();

    const auto start = std::chrono::steady_clock::now();
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) { draw(st, start); last = now; }
        const int key = getch();
        if (key == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) { running.store(false); break; }
        if (key == 'x' || key == 'X') { action_export(st); last = now - std::chrono::seconds(2); }
        else if (key == 'p' || key == 'P') { action_import(st); last = now - std::chrono::seconds(2); }
    }
    endwin();
}

}  // namespace mc::mini
