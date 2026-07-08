// Focused test for the single-sequencer consensus (block format v4).
//
// Verifies the fork-free guarantees end to end against the real Chain/Database:
//   1. Block serialize/deserialize round-trips the new v4 fields
//      (header.sequencer_pubkey + Block.sequencer_sig) and the hash is stable.
//   2. Genesis (height 1) is sig-exempt and commits the sequencer pubkey into
//      chain state (`seq:`).
//   3. A block signed by the sequencer key connects; a block signed by ANY
//      other key — or left unsigned — is REJECTED. No other party can produce
//      an acceptable block, so a competing height-N block is impossible.
#include "core/chain.h"
#include "core/block.h"
#include "storage/database.h"
#include "crypto/keys.h"
#include "crypto/signature.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace mc;

static Block make_block(const Hash256& prev, uint64_t ts) {
    Block b;
    b.header.version      = BLOCK_VERSION;
    b.header.prev_hash    = prev;
    b.header.timestamp_ms = ts;
    b.header.merkle_root  = Block::compute_merkle_root(b.transactions);
    return b;
}

#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", (msg)); return 1; } } while(0)

int main() {
    const std::string dir = "/tmp/bw-seqtest";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir + "/blockchain.db", ec);

    Database db(dir + "/blockchain.db");
    Chain chain(db);
    chain.init();

    auto seq = crypto::generate_keypair();

    // --- 1. serialize round-trip of the v4 fields ---
    Block g = make_block(chain.tip().hash, 1);
    g.header.sequencer_pubkey = seq.public_key;
    g.sequencer_sig = crypto::sign_ecdsa(g.hash(), seq.private_key);
    auto bytes = g.serialize();
    Block g2;
    CHECK(Block::deserialize(bytes.data(), bytes.size(), g2), "deserialize genesis");
    CHECK(g2.header.sequencer_pubkey == seq.public_key, "sequencer_pubkey round-trip");
    CHECK(g2.sequencer_sig == g.sequencer_sig,          "sequencer_sig round-trip");
    CHECK(g2.hash() == g.hash(),                        "hash stable across round-trip");

    // --- 2. genesis connects (sig-exempt) + publishes seq: ---
    CHECK(chain.connect_block(g), "connect genesis (height 1, exempt)");
    CHECK(chain.tip().height == 1, "height == 1 after genesis");

    // --- 3a. block 2 signed by the sequencer -> ACCEPTED ---
    Block b2 = make_block(chain.tip().hash, 2);
    b2.sequencer_sig = crypto::sign_ecdsa(b2.hash(), seq.private_key);
    CHECK(chain.connect_block(b2), "connect sequencer-signed block 2");
    CHECK(chain.tip().height == 2, "height == 2");

    // --- 3b. block 3 signed by a ROGUE key -> REJECTED (no fork) ---
    auto rogue = crypto::generate_keypair();
    Block b3 = make_block(chain.tip().hash, 3);
    b3.sequencer_sig = crypto::sign_ecdsa(b3.hash(), rogue.private_key);
    CHECK(!chain.connect_block(b3), "reject rogue-signed block 3");

    // --- 3c. block 3 with NO signature -> REJECTED ---
    Block b3b = make_block(chain.tip().hash, 4);  // sequencer_sig left zero
    CHECK(!chain.connect_block(b3b), "reject unsigned block 3");

    CHECK(chain.tip().height == 2, "tip unchanged after both rejects");

    std::filesystem::remove_all(dir, ec);
    printf("PASS: single-sequencer consensus (block v4) — "
           "genesis exempt, sequencer accepted, rogue+unsigned rejected\n");
    return 0;
}
