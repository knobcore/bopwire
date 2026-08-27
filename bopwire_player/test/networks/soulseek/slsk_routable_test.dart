// A peer that cannot determine its own public address advertises whatever
// its interface holds — typically a 192.168.x.x or 10.x.x.x LAN address.
// The old routability rule was `port > 0 && ip != '0.0.0.0'`, which let
// every one of those through, and a live run then dialled 18 unreachable
// peers and failed 18 times. That looked like a NAT fault but was really
// us dialling addresses that were never reachable from this machine.
//
// The loopback and private cases are the ones with teeth: dialling
// 192.168.1.5 from our LAN can *succeed* against a completely unrelated
// device holding that address, and 127.0.0.1 connects straight back to
// ourselves. Both are worse than a clean failure, which is why these are
// rejected rather than merely deprioritised.

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';

void main() {
  group('isRoutableAddress', () {
    test('accepts ordinary public addresses', () {
      expect(isRoutableAddress('67.161.100.161', 2234), isTrue);
      expect(isRoutableAddress('8.8.8.8', 1), isTrue);
      expect(isRoutableAddress('1.1.1.1', 65535), isTrue);
      // Just outside each private block, so an off-by-one in the range
      // checks shows up here rather than in the field.
      expect(isRoutableAddress('9.255.255.255', 2234), isTrue);
      expect(isRoutableAddress('11.0.0.0', 2234), isTrue);
      expect(isRoutableAddress('172.15.255.255', 2234), isTrue);
      expect(isRoutableAddress('172.32.0.0', 2234), isTrue);
      expect(isRoutableAddress('192.167.255.255', 2234), isTrue);
      expect(isRoutableAddress('192.169.0.0', 2234), isTrue);
      expect(isRoutableAddress('100.63.255.255', 2234), isTrue);
      expect(isRoutableAddress('100.128.0.0', 2234), isTrue);
      expect(isRoutableAddress('223.255.255.255', 2234), isTrue);
    });

    test('rejects RFC1918 private ranges', () {
      expect(isRoutableAddress('10.0.0.1', 2234), isFalse);
      expect(isRoutableAddress('10.255.255.255', 2234), isFalse);
      expect(isRoutableAddress('172.16.0.1', 2234), isFalse);
      expect(isRoutableAddress('172.31.255.255', 2234), isFalse);
      expect(isRoutableAddress('192.168.0.1', 2234), isFalse);
      expect(isRoutableAddress('192.168.1.5', 2234), isFalse);
      // The address this very machine sits on.
      expect(isRoutableAddress('172.20.20.20', 2234), isFalse);
    });

    test('rejects loopback, link-local, CGNAT and this-network', () {
      expect(isRoutableAddress('127.0.0.1', 2234), isFalse);
      expect(isRoutableAddress('127.5.5.5', 2234), isFalse);
      expect(isRoutableAddress('169.254.1.1', 2234), isFalse);
      expect(isRoutableAddress('100.64.0.1', 2234), isFalse);
      expect(isRoutableAddress('100.127.255.255', 2234), isFalse);
      expect(isRoutableAddress('0.0.0.0', 2234), isFalse);
      expect(isRoutableAddress('0.1.2.3', 2234), isFalse);
    });

    test('rejects multicast and reserved space', () {
      expect(isRoutableAddress('224.0.0.1', 2234), isFalse);
      expect(isRoutableAddress('239.1.1.1', 2234), isFalse);
      expect(isRoutableAddress('255.255.255.255', 2234), isFalse);
    });

    test('rejects unusable ports regardless of address', () {
      expect(isRoutableAddress('8.8.8.8', 0), isFalse);
      expect(isRoutableAddress('8.8.8.8', -1), isFalse);
      expect(isRoutableAddress('8.8.8.8', 65536), isFalse);
    });

    test('rejects malformed addresses instead of throwing', () {
      // These arrive off the wire, so a parse failure must degrade to
      // "not routable" rather than take down the message handler.
      expect(isRoutableAddress('', 2234), isFalse);
      expect(isRoutableAddress('8.8.8', 2234), isFalse);
      expect(isRoutableAddress('8.8.8.8.8', 2234), isFalse);
      expect(isRoutableAddress('999.1.1.1', 2234), isFalse);
      expect(isRoutableAddress('a.b.c.d', 2234), isFalse);
      expect(isRoutableAddress('::1', 2234), isFalse);
    });

    test('PeerAddress.isRoutable uses the same rule', () {
      expect(const PeerAddress('u', '8.8.8.8', 2234).isRoutable, isTrue);
      expect(const PeerAddress('u', '192.168.1.9', 2234).isRoutable, isFalse);
      expect(const PeerAddress('u', '8.8.8.8', 0).isRoutable, isFalse);
    });
  });
}
