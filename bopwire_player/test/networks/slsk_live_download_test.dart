// LIVE end-to-end: log into the real Soulseek server, search for NOFX,
// and download real files from real peers.
//
// Not part of the normal suite's contract: it talks to the live network,
// needs a real Soulseek account, and can fail for reasons that are
// nobody's bug (peers go offline, queues are long, some peers ban small
// sharers). Run explicitly:
//
//   SLSK_USER=youruser SLSK_PASS=yourpass \
//     flutter test test/networks/slsk_live_download_test.dart
//
// SLSK_USER falls back to the username stored in the app's settings
// ("knobcore" on the dev machine); SLSK_PASS has no fallback because the
// real password lives in the encrypted wallet vault, which flutter test
// cannot open. Without SLSK_PASS the test skips rather than pretending.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_share.dart';
import 'package:bopwire_player/src/services/networks/soulseek/soulseek_network.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('soulseek: search "nofx" then download real files', () async {
    final user = Platform.environment['SLSK_USER'] ?? 'knobcore';
    final pass = Platform.environment['SLSK_PASS'];
    if (pass == null || pass.isEmpty) {
      markTestSkipped(
          'Set SLSK_USER and SLSK_PASS to run the live Soulseek test. '
          'The stored password is inside the wallet vault and cannot be '
          'read from a test.');
      return;
    }

    SharedPreferences.setMockInitialValues({});
    // flutter test has no path_provider; give the share a real directory
    // or we would announce 1/1 and then have nothing to serve.
    final shareDir = await Directory.systemTemp.createTemp('slsk-live-share');
    SlskShare.instance.baseDirOverride = shareDir;

    final log = <String>[];
    void logAndPrint(String m) {
      log.add(m);
      stdout.writeln('[slsk] $m');
    }

    final net = SoulseekNetwork(
      usernameOverride: user,
      passwordOverride: pass,
      logger: logAndPrint,
    );

    await net.connect();
    stdout.writeln('[test] status=${net.status} err=${net.lastError}');
    expect(net.status, NetworkStatus.connected,
        reason: 'login failed: ${net.lastError}');

    // ---- search ----
    final hits = <ExternalTrack>[];
    await for (final batch in net.search('nofx')) {
      hits.addAll(batch);
      if (hits.length >= 200) break;
    }
    stdout.writeln('[test] ${hits.length} results');

    // Prefer plausible single mp3 tracks, 1-15 MB, from distinct owners.
    final files = hits
        .where((h) =>
            !h.isFolder &&
            (h.extension == 'mp3') &&
            (h.sizeBytes ?? 0) > 1 * 1024 * 1024 &&
            (h.sizeBytes ?? 0) < 15 * 1024 * 1024)
        .toList()
      ..sort((a, b) => (b.bitrate ?? 0).compareTo(a.bitrate ?? 0));
    final byOwner = <String, ExternalTrack>{};
    for (final f in files) {
      byOwner.putIfAbsent(f.owner ?? '?', () => f);
    }
    final candidates = byOwner.values.take(6).toList();
    for (final c in candidates) {
      stdout.writeln('[test] candidate: ${c.owner} | ${c.title} '
          '(${c.sizeBytes} B, ${c.bitrate} kbps) ${c.remotePath}');
    }
    expect(candidates, isNotEmpty, reason: 'no downloadable mp3 results');

    final dir = await Directory.systemTemp.createTemp('slsk-live');
    stdout.writeln('[test] downloading into ${dir.path}');

    // ---- sequential download attempts until two succeed ----
    var successes = 0;
    final failures = <String>[];
    for (final track in candidates) {
      if (successes >= 2) break;
      stdout.writeln('\n=== ${track.owner}: "${track.title}" '
          '(${track.sizeBytes} B) ===');
      var lastBytes = 0;
      String? error;
      String? localPath;
      var done = false;

      final sub = net.download(track, dir.path).listen((p) {
        if (p.receivedBytes > lastBytes) lastBytes = p.receivedBytes;
        if (p.error != null) error = p.error;
        if (p.done) {
          done = true;
          localPath = p.localPath;
        }
      });

      // Window per attempt: queue wait + transfer. Peers with long queues
      // are treated as failures for this smoke test, not waited out.
      final deadline = DateTime.now().add(const Duration(seconds: 90));
      while (!done && error == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(seconds: 1));
      }
      await sub.cancel();

      stdout.writeln('=== result: bytes=$lastBytes done=$done '
          'error=$error path=$localPath');
      if (done && localPath != null) {
        final f = File(localPath!);
        final size = await f.length();
        final head = await f.openRead(0, 3).first;
        stdout.writeln('=== on disk: $size bytes, '
            'first bytes ${head.map((b) => b.toRadixString(16)).join(" ")}');
        expect(size, track.sizeBytes,
            reason: 'size on disk disagrees with the search result');
        successes++;
      } else {
        failures.add('${track.owner}: ${error ?? "timed out at $lastBytes"}');
      }
    }

    stdout.writeln('\n[test] successes=$successes');
    stdout.writeln('[test] failures: ${failures.join(" | ")}');
    final denials = log.where((l) => l.contains('refused')).toList();
    stdout.writeln('[test] refusals seen in log: ${denials.length}');
    for (final d in denials) {
      stdout.writeln('   $d');
    }
    // Peers that browsed / queued our share while we were on:
    final shareActivity = log
        .where((l) =>
            l.contains('offered shared file') ||
            l.contains('served shared file'))
        .toList();
    stdout.writeln('[test] share activity: ${shareActivity.length}');

    expect(successes, greaterThan(0),
        reason: 'no file could be downloaded from any peer: '
            '${failures.join(" | ")}');

    await net.dispose();
    SlskShare.instance.baseDirOverride = null;
  }, timeout: const Timeout(Duration(minutes: 15)));
}
