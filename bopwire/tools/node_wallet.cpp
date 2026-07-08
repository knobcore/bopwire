#include "node_wallet.h"

#include "../src/crypto/bip39.h"
#include "../src/crypto/keystore.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
#ifdef MC_TUI_PDCURSES
  #include <curses.h>
#else
  #include <ncurses.h>
#endif

namespace mc {
namespace {

std::string keystore_path(const std::string& data_dir, const std::string& wallet_file) {
    if (!wallet_file.empty()) return wallet_file;
    return data_dir + "/node-wallet.json";
}

std::optional<crypto::KeyPair> kp_from_mnemonic(const std::string& mnemonic) {
    auto kp = crypto::bip39_mnemonic_to_keypair(mnemonic, "");
    if (!kp) return std::nullopt;
    return *kp;
}

bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << contents;
    return true;
}

// ---- minimal curses input (interactive wizard / password prompt) ----

void page(const char* title) {
    erase();
    int cols = getmaxx(stdscr);
    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 2, " %s ", title);
    attroff(A_REVERSE);
}

int getkey() { nodelay(stdscr, FALSE); int c = getch(); nodelay(stdscr, TRUE); return c; }

// Single-line prompt. `secret` masks with '*'. Returns false on empty/ESC.
bool prompt(const char* title, std::string& out, bool secret, int max_len = 256) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    const int y = rows - 4;
    attron(A_REVERSE); mvhline(y, 0, ' ', cols); mvprintw(y, 1, "%s", title); attroff(A_REVERSE);
    mvprintw(y + 1, 1, "> "); move(y + 1, 3);
    curs_set(1); nodelay(stdscr, FALSE);
    std::string buf;
    if (secret) {
        noecho();
        while ((int)buf.size() < max_len) {
            int ch = getch();
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
            if (ch == 27) { buf.clear(); break; }
            if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
                if (!buf.empty()) {
                    buf.pop_back(); int cy, cx; getyx(stdscr, cy, cx);
                    if (cx > 0) { mvaddch(cy, cx - 1, ' '); move(cy, cx - 1); }
                }
                continue;
            }
            if (ch >= 32 && ch < 127) { buf.push_back((char)ch); addch('*'); }
        }
    } else {
        echo();
        std::vector<char> b(max_len + 1, 0);
        getnstr(b.data(), max_len);
        noecho();
        buf.assign(b.data());
        while (!buf.empty() && (buf.back() == '\r' || buf.back() == '\n' || buf.back() == ' '))
            buf.pop_back();
    }
    curs_set(0); nodelay(stdscr, TRUE);
    out = buf;
    return !out.empty();
}

void msg(const char* title, const std::string& text) {
    page(title); mvprintw(3, 4, "%s", text.c_str());
    mvprintw(5, 4, "(press any key)"); refresh(); getkey();
}

// Interactive create/import wizard. On success fills out_mnemonic + out_password.
bool run_wizard(const std::string& role, std::string& out_mnemonic, std::string& out_password) {
    page("node wallet setup");
    mvprintw(3, 4, "This %s has no wallet yet — set up its identity.", role.c_str());
    mvprintw(5, 4, "It is a password-protected, portable 12-word wallet.");
    mvprintw(7, 4, "[C] Create new     [I] Import existing     [Q] Cancel");
    refresh();
    int c = 0;
    while (true) { c = getkey(); if (c=='c'||c=='C'||c=='i'||c=='I'||c=='q'||c=='Q'||c==27) break; }
    if (c=='q'||c=='Q'||c==27) return false;

    std::string mnemonic;
    bool created = false;
    if (c=='c'||c=='C') {
        mnemonic = crypto::bip39_generate_12();
        if (mnemonic.empty()) { msg("error", "entropy source failed"); return false; }
        created = true;
    } else {
        page("import");
        mvprintw(3, 4, "[W] Type the 12 words    [F] Load a keystore file    [Q] cancel");
        refresh();
        int m = 0;
        while (true) { m = getkey(); if (m=='w'||m=='W'||m=='f'||m=='F'||m=='q'||m=='Q'||m==27) break; }
        if (m=='q'||m=='Q'||m==27) return false;
        if (m=='w'||m=='W') {
            std::string words;
            if (!prompt("Enter the 12-word phrase", words, false)) return false;
            mnemonic = words;
        } else {
            std::string path;
            if (!prompt("Keystore file path", path, false)) return false;
            std::ifstream f(path, std::ios::binary);
            if (!f) { msg("import", "cannot read " + path); return false; }
            std::stringstream ss; ss << f.rdbuf();
            std::string pass;
            if (!prompt("Keystore passphrase", pass, true)) return false;
            std::string out;
            bool ok = crypto::keystore_decrypt(ss.str(), pass, out);
            std::fill(pass.begin(), pass.end(), '\0');
            if (!ok) { msg("import", "wrong passphrase or corrupt keystore"); return false; }
            mnemonic = out;
        }
    }
    if (!kp_from_mnemonic(mnemonic)) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        msg("error", "not a valid 12-word wallet");
        return false;
    }
    if (created) {
        page("back up your 12 words");
        mvprintw(3, 4, "Write these down — they restore this node's wallet.");
        attron(A_BOLD); mvprintw(5, 4, "%s", mnemonic.c_str()); attroff(A_BOLD);
        mvprintw(7, 4, "Press any key once you've written them down.");
        refresh(); getkey();
    }
    // Set the wallet password (policy-checked).
    while (true) {
        page("set a wallet password");
        mvprintw(3, 4, "Encrypts the wallet at rest and unlocks it on start.");
        mvprintw(4, 4, "Policy: >= 12 chars, 1 uppercase, 1 special.");
        refresh();
        std::string p1, p2;
        if (!prompt("New password", p1, true)) { std::fill(mnemonic.begin(), mnemonic.end(), '\0'); return false; }
        std::string perr = crypto::password_policy_error(p1);
        if (!perr.empty()) { std::fill(p1.begin(), p1.end(), '\0'); msg("password", perr); continue; }
        if (!prompt("Confirm password", p2, true) || p1 != p2) {
            std::fill(p1.begin(), p1.end(), '\0'); std::fill(p2.begin(), p2.end(), '\0');
            msg("password", "passwords do not match"); continue;
        }
        out_password = p1;
        std::fill(p2.begin(), p2.end(), '\0');
        break;
    }
    out_mnemonic = mnemonic;
    return true;
}

// Wrap a callback in an initscr/endwin curses session. Returns whatever the
// callback returns; false if the terminal can't be opened.
template <typename Fn>
bool curses_session(Fn&& fn) {
    initscr();
    if (stdscr == nullptr) return false;
    raw(); noecho(); keypad(stdscr, TRUE); curs_set(0); nodelay(stdscr, TRUE);
    bool ok = fn();
    endwin();
    return ok;
}

} // namespace

std::optional<crypto::KeyPair> load_or_setup_node_identity(
    const std::string& data_dir, const std::string& wallet_file,
    const std::string& password, bool interactive, const char* role_label) {
    const std::string path = keystore_path(data_dir, wallet_file);
    const std::string role = role_label ? role_label : "node";

    // ---- existing keystore ----
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::stringstream ss; ss << f.rdbuf(); f.close();
            const std::string ks = ss.str();

            // 1) supplied password (headless-friendly).
            if (!password.empty()) {
                std::string mnemonic;
                if (crypto::keystore_decrypt(ks, password, mnemonic)) {
                    auto kp = kp_from_mnemonic(mnemonic);
                    std::fill(mnemonic.begin(), mnemonic.end(), '\0');
                    if (kp) return kp;
                }
            }
            if (!interactive) {
                std::cerr << "[wallet] " << role << ": keystore at " << path
                          << " needs a valid password "
                             "(--wallet-password / config wallet_password)\n";
                return std::nullopt;
            }
            // 2) interactive: prompt for the password (up to 3 tries).
            std::optional<crypto::KeyPair> result;
            curses_session([&]() {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    page("unlock node wallet");
                    mvprintw(3, 4, "Enter the password for %s's wallet:", role.c_str());
                    refresh();
                    std::string pw;
                    if (!prompt("Password", pw, true)) break;
                    std::string mnemonic;
                    bool ok = crypto::keystore_decrypt(ks, pw, mnemonic);
                    std::fill(pw.begin(), pw.end(), '\0');
                    if (ok) {
                        result = kp_from_mnemonic(mnemonic);
                        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
                        if (result) return true;
                    }
                    msg("unlock", "wrong password, try again");
                }
                return false;
            });
            if (!result)
                std::cerr << "[wallet] " << role << ": could not unlock wallet\n";
            return result;
        }
    }

    // ---- no keystore ----
    if (!interactive) {
        std::cerr << "[wallet] " << role << ": no wallet at " << path
                  << ".\n           Run once interactively to create it, or pass "
                     "--wallet-file/--wallet-password.\n";
        return std::nullopt;
    }

    // Interactive first-config: run the create/import wizard.
    std::optional<crypto::KeyPair> result;
    std::string saved_addr;
    curses_session([&]() {
        std::string mnemonic, pw;
        if (!run_wizard(role, mnemonic, pw)) return false;
        auto kp = kp_from_mnemonic(mnemonic);
        if (!kp) { std::fill(mnemonic.begin(), mnemonic.end(), '\0'); return false; }
        const std::string addr = crypto::to_checksum_hex(kp->address);
        std::string ks = crypto::keystore_encrypt(mnemonic, pw, addr);
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        std::fill(pw.begin(), pw.end(), '\0');
        if (ks.empty() || !write_file(path, ks)) {
            msg("error", "could not save wallet keystore to " + path);
            return false;
        }
        result = kp;
        saved_addr = addr;
        msg("done", "wallet saved to " + path + "\n\n           " + role +
                    " identity: " + addr);
        return true;
    });
    if (result)
        std::cout << "[wallet] " << role << " identity created: " << saved_addr
                  << " (keystore " << path << ")\n";
    return result;
}

} // namespace mc
