#include "node_wallet.h"

#include "../src/crypto/bip39.h"
#include "../src/crypto/keystore.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif
#ifdef MC_TUI_PDCURSES
  #include <curses.h>
#else
  #include <ncurses.h>
#endif

namespace mc {
namespace {

// Last keystore path resolved by load_or_setup_node_identity(), published
// through node_wallet_keystore_path() so the wallet TUI can re-check the
// operator's password against the same file without a second flag.
std::string g_resolved_keystore_path;

// A curses prompt is only possible with a real terminal on stdin. Without
// this check an unattended service (systemd, Windows service) that still has
// tui_mode set would drop into initscr() and either die obscurely or sit
// waiting for a keypress that never comes.
bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

std::string keystore_path(const std::string& data_dir, const std::string& wallet_file) {
    if (!wallet_file.empty()) return wallet_file;
    return data_dir + "/node-wallet.json";
}

std::optional<crypto::KeyPair> kp_from_mnemonic(const std::string& mnemonic) {
    auto kp = crypto::bip39_mnemonic_to_keypair(mnemonic, "");
    if (!kp) return std::nullopt;
    return *kp;
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
    mvprintw(3, 4, "This %s has no wallet yet - set up its identity.", role.c_str());
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
        mvprintw(3, 4, "Write these down - they restore this node's wallet.");
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
    g_resolved_keystore_path = path;
    // Never prompt without a terminal — an unattended start must fail fast
    // with the message below instead of blocking on stdin.
    if (interactive && !stdin_is_tty()) {
        std::cerr << "[wallet] " << role << ": no terminal on stdin - "
                     "resolving the wallet without prompting.\n";
        interactive = false;
    }

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
        if (ks.empty() || !write_secret_file(path, ks)) {
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

// ---- public helpers -------------------------------------------------

const std::string& node_wallet_keystore_path() { return g_resolved_keystore_path; }

bool write_secret_file(const std::string& path, const std::string& contents) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    {
        std::ofstream f(path, std::ios::trunc | std::ios::binary);
        if (!f) return false;
        f << contents;
        if (!f) return false;
    }
    // Tighten to owner-only. Non-fatal on filesystems without POSIX perms
    // (e.g. Windows/FAT) — the write itself already succeeded.
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return true;
}

std::optional<CreatedWallet> write_node_wallet(const std::string& out_path,
                                              const std::string& mnemonic_in,
                                              const std::string& password,
                                              bool               overwrite,
                                              std::string&       err) {
    namespace fs = std::filesystem;
    err.clear();
    if (out_path.empty()) { err = "no keystore path"; return std::nullopt; }

    const std::string perr = crypto::password_policy_error(password);
    if (!perr.empty()) { err = perr; return std::nullopt; }

    std::error_code ec;
    if (fs::exists(out_path, ec) && !overwrite) {
        err = "refusing to overwrite existing wallet at " + out_path +
              " (pass --force to replace it)";
        return std::nullopt;
    }

    std::string mnemonic = mnemonic_in;
    if (mnemonic.empty()) { err = "empty seed phrase"; return std::nullopt; }
    auto kp = kp_from_mnemonic(mnemonic);
    if (!kp) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        err = "not a valid BIP39 seed phrase";
        return std::nullopt;
    }
    const std::string addr = crypto::to_checksum_hex(kp->address);
    const std::string ks   = crypto::keystore_encrypt(mnemonic, password, addr);
    if (ks.empty()) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        err = "keystore encryption failed";
        return std::nullopt;
    }
    if (!write_secret_file(out_path, ks)) {
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        err = "cannot write " + out_path;
        return std::nullopt;
    }
    CreatedWallet out;
    out.mnemonic = mnemonic;
    out.address  = addr;
    out.path     = out_path;
    std::fill(mnemonic.begin(), mnemonic.end(), '\0');
    return out;
}

std::optional<CreatedWallet> create_node_wallet(const std::string& out_path,
                                                const std::string& password,
                                                bool               overwrite,
                                                std::string&       err) {
    std::string mnemonic = crypto::bip39_generate_12();
    if (mnemonic.empty()) { err = "entropy source failed"; return std::nullopt; }
    auto out = write_node_wallet(out_path, mnemonic, password, overwrite, err);
    std::fill(mnemonic.begin(), mnemonic.end(), '\0');
    return out;
}

} // namespace mc
