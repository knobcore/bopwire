// LIVE end-to-end: start the embedded Tor, search napstr for a real
// track, and download it — repeatedly — to find what has to be reset
// between attempts.
//
// Not part of the normal suite's contract: it talks to real relays and
// real seeders over Tor, so it is slow and can fail for reasons that are
// nobody's bug. Run explicitly:
//   flutter test test/networks/napstr_live_download_test.dart
import 'dart:ffi';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/napstr/napstr_network.dart';

const _lib =
    '/home/lain/blockchain/bopwire/build-linux-x64/Release/libbopwire.so';

int _startTor() {
  final lib = DynamicLibrary.open(_lib);
  final start = lib.lookupFunction<Int32 Function(Uint16), int Function(int)>(
      'tor_ffi_start');
  final status =
      lib.lookupFunction<Int32 Function(), int Function()>('tor_ffi_status');
  final port = start(0);
  if (port <= 0) throw StateError('tor_ffi_start failed: $port');
  stdout.writeln('[tor] SOCKS5 on 127.0.0.1:$port, bootstrapping…');
  final deadline = DateTime.now().add(const Duration(seconds: 120));
  while (DateTime.now().isBefore(deadline)) {
    final st = status();
    if (st == 2) {
      stdout.writeln('[tor] READY');
      return port;
    }
    if (st == 3) throw StateError('tor bootstrap failed');
    sleep(const Duration(milliseconds: 500));
  }
  throw StateError('tor did not bootstrap in 120s');
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('napstr: search then download "olympia" repeatedly', () async {
    SharedPreferences.setMockInitialValues({});
    final torPort = _startTor();

    final net = NapstrNetwork();
    // Point it straight at the embedded proxy; no Settings involved.
    SharedPreferences.setMockInitialValues({
      'network.napstr.relays': '',
      'network.napstr.tor_socks': '127.0.0.1:$torPort',
    });

    await net.connect();
    stdout.writeln('[napstr] status=${net.status} tor=${net.torAvailable} '
        'detail=${net.torDetail}');

    // ---- search ----
    final hits = <ExternalTrack>[];
    await for (final batch in net.search('nofx olympia')) {
      hits.addAll(batch);
      if (hits.length >= 5) break;
    }
    stdout.writeln('[napstr] ${hits.length} hits');
    for (final h in hits.take(5)) {
      stdout.writeln('   - ${h.artist} — ${h.title} '
          '(${h.sizeBytes} bytes, id=${h.id.substring(0, 24)}…)');
    }
    expect(hits, isNotEmpty, reason: 'no search results to download');

    final track = hits.first;
    final dir = await Directory.systemTemp.createTemp('napstr-live');

    // ---- CONCURRENT: two requests to the same single seeder, which is
    // what "preview then download" does in the app ----
    stdout.writeln('\n=== concurrent: two simultaneous requests ===');
    var aBytes = 0, bBytes = 0;
    String? aErr, bErr;
    final subA = net.download(track, dir.path).listen((p) {
      if (p.receivedBytes > aBytes) aBytes = p.receivedBytes;
      if (p.error != null) aErr = p.error;
    });
    await Future<void>.delayed(const Duration(seconds: 3));
    final subB = net.download(track, dir.path).listen((p) {
      if (p.receivedBytes > bBytes) bBytes = p.receivedBytes;
      if (p.error != null) bErr = p.error;
    });
    await Future<void>.delayed(const Duration(seconds: 90));
    await subA.cancel();
    await subB.cancel();
    stdout.writeln('=== concurrent result: A bytes=$aBytes err=$aErr');
    stdout.writeln('===                    B bytes=$bBytes err=$bErr');

    // ---- download, three times, watching what changes ----
    for (var attempt = 1; attempt <= 1; attempt++) {
      stdout.writeln('\n=== attempt $attempt: "${track.title}" ===');
      final started = DateTime.now();
      var lastBytes = 0;
      String? error;
      var done = false;

      final sub = net.download(track, dir.path).listen((p) {
        if (p.receivedBytes > lastBytes) lastBytes = p.receivedBytes;
        if (p.error != null) error = p.error;
        if (p.done) done = true;
      });
      // Give each attempt its own window; napstr's own offer wait is 60s.
      await Future<void>.delayed(const Duration(seconds: 75));
      await sub.cancel();

      final secs = DateTime.now().difference(started).inSeconds;
      stdout.writeln('=== attempt $attempt result after ${secs}s: '
          'bytes=$lastBytes done=$done error=$error');
    }
  }, timeout: const Timeout(Duration(minutes: 10)));
}
