// Pre-download cache: growing-file HTTP serving, expiry and promotion.
//
// What these tests can and cannot prove
// -------------------------------------
// They prove the half of "streaming" that is ours: that a file being
// written right now is served over the loopback URL from byte 0 to the
// last byte, that reaching the current end of the file is treated as
// "wait", not EOF, that a `.part` -> final rename mid-transfer does not
// truncate the response, and that Range requests (including a suffix
// range aimed past the current end, which is what a tail-indexed
// container asks for) are answered correctly.
//
// They do NOT prove that libmpv is happy with the result — that needs a
// real audio decoder and real network bytes, neither of which exists in
// a unit test. The network side is a fake that writes to a `.part` file
// and renames it at the end, mirroring exactly what slsk_download.dart
// and napstr/transfer.dart do.

import 'dart:async';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/predownload_cache.dart';

// ---------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------

Uint8List _pattern(int n) =>
    Uint8List.fromList(List<int>.generate(n, (i) => (i * 7 + 13) % 251));

Future<Uint8List> _drain(Stream<List<int>> s) async {
  final b = BytesBuilder(copy: false);
  await for (final c in s) {
    b.add(c);
  }
  return b.takeBytes();
}

/// A network that behaves like the real ones: writes to a temporary
/// `.part` file, appends in chunks, then renames into place and reports
/// done with the final path.
class _FakeNetwork implements ExternalNetwork {
  _FakeNetwork(
    this.id, {
    required this.bytes,
    this.chunk = 16 * 1024,
    this.chunkDelay = const Duration(milliseconds: 5),
    this.failAfter,
  });

  @override
  final String id;
  final Uint8List bytes;
  final int chunk;
  final Duration chunkDelay;

  /// When set, the transfer errors out after this many bytes.
  final int? failAfter;

  int downloadCalls = 0;

  @override
  Stream<DownloadProgress> download(ExternalTrack track, String destDir) async* {
    downloadCalls++;
    final sep = Platform.pathSeparator;
    final name = (track.remotePath ?? track.title).split(sep).last;
    final part = File('$destDir$sep$name.part');
    await part.create(recursive: true);

    var sent = 0;
    while (sent < bytes.length) {
      await Future<void>.delayed(chunkDelay);
      final end = min(sent + chunk, bytes.length);
      await part.writeAsBytes(bytes.sublist(sent, end),
          mode: FileMode.append, flush: true);
      sent = end;
      final limit = failAfter;
      if (limit != null && sent >= limit) {
        yield DownloadProgress(
            trackId: track.id,
            receivedBytes: sent,
            totalBytes: bytes.length,
            error: 'peer went away');
        return;
      }
      yield DownloadProgress(
          trackId: track.id, receivedBytes: sent, totalBytes: bytes.length);
    }

    final target = '$destDir$sep$name';
    await part.rename(target);
    yield DownloadProgress(
      trackId: track.id,
      receivedBytes: sent,
      totalBytes: bytes.length,
      localPath: target,
      done: true,
    );
  }

  // -- the rest of the contract, unused here --------------------------
  @override
  String get displayName => id;
  @override
  List<NetworkCredentialField> get credentialFields => const [];
  @override
  bool get isConfigured => true;
  @override
  bool enabled = true;
  @override
  NetworkStatus get status => NetworkStatus.connected;
  @override
  Stream<NetworkStatus> get statusChanges => const Stream.empty();
  @override
  Future<void> connect() async {}
  @override
  Future<void> disconnect() async {}
  @override
  Stream<List<ExternalTrack>> search(String query) => const Stream.empty();
  @override
  Future<List<ExternalTrack>> listFolder(ExternalTrack folder) async => const [];
}

ExternalTrack _track({String id = 't1', String name = 'song.mp3', int? size}) =>
    ExternalTrack(
      networkId: 'fake',
      id: id,
      title: name,
      remotePath: 'shared\\music\\$name',
      sizeBytes: size,
      extension: name.split('.').last,
    );

void main() {
  // ------------------------------------------------------------------
  group('GrowingFileServer', () {
    late GrowingFileServer server;
    late Directory dir;

    setUp(() async {
      server = GrowingFileServer.forTesting();
      dir = await Directory.systemTemp.createTemp('gfs-test-');
    });

    tearDown(() async {
      await server.dispose();
      if (await dir.exists()) await dir.delete(recursive: true);
    });

    test('parseRange handles open, closed and suffix ranges', () {
      expect(GrowingFileServer.parseRange('bytes=10-', 100)?.start, 10);
      expect(GrowingFileServer.parseRange('bytes=10-', 100)?.end, isNull);
      expect(GrowingFileServer.parseRange('bytes=10-19', 100)?.end, 19);
      // Suffix range: the last 32 bytes of a 100-byte file.
      expect(GrowingFileServer.parseRange('bytes=-32', 100)?.start, 68);
      expect(GrowingFileServer.parseRange('bytes=-32', 100)?.end, 99);
      // Suffix range with unknown total cannot be answered.
      expect(GrowingFileServer.parseRange('bytes=-32', null), isNull);
      expect(GrowingFileServer.parseRange(null, 100), isNull);
      expect(GrowingFileServer.parseRange('bytes=-', 100), isNull);
      expect(GrowingFileServer.parseRange('lines=1-2', 100), isNull);
    });

    test('serves a file that is still being written, across a rename',
        () async {
      final data = _pattern(300 * 1024);
      final sep = Platform.pathSeparator;
      final part = File('${dir.path}${sep}a.mp3.part');
      final finalFile = File('${dir.path}${sep}a.mp3');

      // Only a small prefix exists when the client connects. A plain
      // file-backed player would call this the whole track.
      await part.writeAsBytes(data.sublist(0, 8 * 1024), flush: true);

      var complete = false;
      final url = await server.serve(GrowingFileSource(
        resolve: () async {
          if (await finalFile.exists()) return finalFile;
          if (await part.exists()) return part;
          return null;
        },
        totalBytes: () => data.length,
        isComplete: () => complete,
        isAborted: () => false,
      ));

      // Writer keeps appending after the response has started.
      final writer = () async {
        var off = 8 * 1024;
        while (off < data.length) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
          final end = min(off + 24 * 1024, data.length);
          await part.writeAsBytes(data.sublist(off, end),
              mode: FileMode.append, flush: true);
          off = end;
        }
        // Exactly what slsk/napstr do at the end of a transfer.
        await part.rename(finalFile.path);
        complete = true;
      }();

      final client = HttpClient();
      final resp = await (await client.getUrl(Uri.parse(url))).close();
      expect(resp.statusCode, 200);
      expect(resp.headers.contentLength, data.length);
      final got = await _drain(resp);
      client.close();
      await writer;

      expect(got.length, data.length,
          reason: 'the response must not stop at the bytes that happened '
              'to exist when it opened');
      expect(got, equals(data));
    });

    test('answers a byte range out of the completed file', () async {
      final data = _pattern(4096);
      final sep = Platform.pathSeparator;
      final f = File('${dir.path}${sep}b.mp3');
      await f.writeAsBytes(data, flush: true);

      final url = await server.serve(GrowingFileSource(
        resolve: () async => f,
        totalBytes: () => data.length,
        isComplete: () => true,
        isAborted: () => false,
      ));

      final client = HttpClient();
      final req = await client.getUrl(Uri.parse(url));
      req.headers.set(HttpHeaders.rangeHeader, 'bytes=100-199');
      final resp = await req.close();
      expect(resp.statusCode, 206);
      expect(resp.headers.value(HttpHeaders.contentRangeHeader),
          'bytes 100-199/4096');
      final got = await _drain(resp);
      client.close();
      expect(got, equals(data.sublist(100, 200)));
    });

    test('a suffix range past the current end waits for the writer', () async {
      // This is the tail-indexed container case (m4a moov atom): the
      // player asks for the end of the file long before it has arrived.
      final data = _pattern(64 * 1024);
      final sep = Platform.pathSeparator;
      final f = File('${dir.path}${sep}c.m4a');
      await f.writeAsBytes(data.sublist(0, 1024), flush: true);

      var complete = false;
      final url = await server.serve(GrowingFileSource(
        resolve: () async => f,
        totalBytes: () => data.length,
        isComplete: () => complete,
        isAborted: () => false,
      ));

      final writer = () async {
        var off = 1024;
        while (off < data.length) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
          final end = min(off + 8 * 1024, data.length);
          await f.writeAsBytes(data.sublist(off, end),
              mode: FileMode.append, flush: true);
          off = end;
        }
        complete = true;
      }();

      final client = HttpClient();
      final req = await client.getUrl(Uri.parse(url));
      req.headers.set(HttpHeaders.rangeHeader, 'bytes=-512');
      final resp = await req.close();
      expect(resp.statusCode, 206);
      final got = await _drain(resp);
      client.close();
      await writer;
      expect(got, equals(data.sublist(data.length - 512)));
    });

    test('closes early when the transfer is aborted', () async {
      final sep = Platform.pathSeparator;
      final f = File('${dir.path}${sep}d.mp3');
      await f.writeAsBytes(_pattern(512), flush: true);

      var aborted = false;
      final url = await server.serve(GrowingFileSource(
        resolve: () async => f,
        totalBytes: () => null, // unknown size -> chunked
        isComplete: () => false,
        isAborted: () => aborted,
      ));

      final client = HttpClient();
      final resp = await (await client.getUrl(Uri.parse(url))).close();
      Future<void>.delayed(const Duration(milliseconds: 100))
          .then((_) => aborted = true);
      final got = await _drain(resp);
      client.close();
      expect(got.length, 512);
    });

    test('unknown ticket 404s', () async {
      await server.ensureStarted();
      final client = HttpClient();
      final resp = await (await client
              .getUrl(Uri.parse('http://127.0.0.1:${server.port}/nope')))
          .close();
      expect(resp.statusCode, 404);
      await resp.drain<void>();
      client.close();
    });
  });

  // ------------------------------------------------------------------
  group('PredownloadCache', () {
    late Directory root;
    late GrowingFileServer server;
    final caches = <PredownloadCache>[];

    PredownloadCache build({
      required _FakeNetwork net,
      Duration ttl = const Duration(hours: 1),
      void Function(String url)? onOpen,
    }) {
      final c = PredownloadCache.forTesting(
        cacheDir: root,
        lookupNetwork: (id) => id == net.id ? net : null,
        openPlayer: (url) async => onOpen?.call(url),
        stopPlayer: () async {},
        server: server,
        ttl: ttl,
      );
      caches.add(c);
      return c;
    }

    setUp(() async {
      root = await Directory.systemTemp.createTemp('predl-test-');
      server = GrowingFileServer.forTesting();
    });

    tearDown(() async {
      for (final c in caches) {
        c.stopSweeper();
      }
      caches.clear();
      await server.dispose();
      if (await root.exists()) await root.delete(recursive: true);
    });

    test('cache dir is rejected when it overlaps a watched library folder',
        () {
      final sep = Platform.pathSeparator;
      expect(
          PredownloadCache.pathCollides(
              '${sep}home${sep}u${sep}Music${sep}tmp', ['${sep}home${sep}u${sep}Music']),
          isTrue,
          reason: 'cache inside a watched folder would be scanned');
      expect(
          PredownloadCache.pathCollides(
              '${sep}tmp${sep}bopwire', ['${sep}tmp${sep}bopwire${sep}sub']),
          isTrue,
          reason: 'a watched folder inside the cache would be swept');
      expect(
          PredownloadCache.pathCollides('${sep}tmp${sep}bopwire',
              ['${sep}home${sep}u${sep}Music']),
          isFalse);
      // Prefix-but-not-child must not trip it.
      expect(
          PredownloadCache.pathCollides(
              '${sep}tmp${sep}bopwire-x', ['${sep}tmp${sep}bopwire']),
          isFalse);
    });

    test('streaming serves the growing file over the loopback URL',
        () async {
      final data = _pattern(240 * 1024);
      final net = _FakeNetwork('fake', bytes: data);
      String? opened;
      final cache = build(net: net, onOpen: (u) => opened = u);

      final track = _track(size: data.length);
      final entry = await cache.stream(track);

      // The URL is handed over once the playable prefix exists, which is
      // well before the transfer ends.
      final deadline = DateTime.now().add(const Duration(seconds: 10));
      while (opened == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      }
      expect(opened, isNotNull,
          reason: 'playback should start from the partial file');
      expect(entry.state, CacheState.downloading);

      final client = HttpClient();
      final resp = await (await client.getUrl(Uri.parse(opened!))).close();
      final got = await _drain(resp);
      client.close();

      expect(got, equals(data));
      await entry.finished;
      expect(entry.state, CacheState.ready);
      expect(File(entry.path!).existsSync(), isTrue);
      // Nothing landed outside the cache root.
      expect(entry.path!.startsWith(root.path), isTrue);
    });

    test('a second stream of the same track reuses the cached entry',
        () async {
      final data = _pattern(32 * 1024);
      final net = _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
      final cache = build(net: net);
      final track = _track(size: data.length);

      final a = await cache.stream(track);
      await a.finished;
      final b = await cache.stream(track);
      await b.finished;

      expect(identical(a, b), isTrue);
      expect(net.downloadCalls, 1, reason: 'no second transfer');
    });

    test('a failed transfer surfaces the error', () async {
      final data = _pattern(64 * 1024);
      final net = _FakeNetwork('fake',
          bytes: data, chunkDelay: Duration.zero, failAfter: 16 * 1024);
      final cache = build(net: net);

      final entry = await cache.stream(_track(size: data.length));
      await entry.finished;
      expect(entry.state, CacheState.failed);
      expect(entry.error, contains('peer went away'));
    });

    test('an unregistered network fails the entry instead of hanging',
        () async {
      final net = _FakeNetwork('fake', bytes: _pattern(16));
      final cache = build(net: net);
      final entry = await cache.stream(const ExternalTrack(
          networkId: 'nope', id: 'x', title: 'x.mp3', extension: 'mp3'));
      await entry.finished;
      expect(entry.state, CacheState.failed);
      expect(entry.error, contains('not registered'));
    });

    // ----------------------------------------------------------------
    group('promotion', () {
      test('copies cached bytes into the library folder, no re-download',
          () async {
        final data = _pattern(48 * 1024);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache = build(net: net);
        final track = _track(size: data.length);

        final entry = await cache.prefetch(track);
        await entry.finished;
        expect(entry.state, CacheState.ready);

        final dest = await Directory.systemTemp.createTemp('lib-');
        addTearDown(() => dest.delete(recursive: true));

        final promoted = await cache.promoteInto(track, dest.path);
        expect(promoted, isNotNull);
        expect(await File(promoted!).readAsBytes(), equals(data));
        expect(promoted.startsWith(dest.path), isTrue);
        expect(net.downloadCalls, 1, reason: 'promotion is a local copy');
        // The cache copy survives so playback can continue.
        expect(File(entry.path!).existsSync(), isTrue);
      });

      test('a second promotion does not clobber the first file', () async {
        final data = _pattern(4096);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache = build(net: net);
        final track = _track(size: data.length);
        await (await cache.prefetch(track)).finished;

        final dest = await Directory.systemTemp.createTemp('lib-');
        addTearDown(() => dest.delete(recursive: true));

        final a = await cache.promoteInto(track, dest.path);
        final b = await cache.promoteInto(track, dest.path);
        expect(a, isNot(b));
        expect(File(a!).existsSync(), isTrue);
        expect(File(b!).existsSync(), isTrue);
        expect(b, contains('(2)'));
      });

      test('waits for an in-flight cache download rather than racing it',
          () async {
        final data = _pattern(160 * 1024);
        final net = _FakeNetwork('fake',
            bytes: data, chunk: 16 * 1024,
            chunkDelay: const Duration(milliseconds: 5));
        final cache = build(net: net);
        final track = _track(size: data.length);

        final entry = await cache.prefetch(track);
        expect(entry.state, CacheState.downloading);

        final seen = <int>[];
        final dest = await Directory.systemTemp.createTemp('lib-');
        addTearDown(() => dest.delete(recursive: true));

        final promoted = await cache.promoteInto(track, dest.path,
            onProgress: (r, t) => seen.add(r));

        expect(promoted, isNotNull);
        expect(await File(promoted!).readAsBytes(), equals(data));
        expect(net.downloadCalls, 1);
        expect(seen, isNotEmpty, reason: 'progress should be reported');
      });

      test('returns null when the track was never cached', () async {
        final net = _FakeNetwork('fake', bytes: _pattern(16));
        final cache = build(net: net);
        final dest = await Directory.systemTemp.createTemp('lib-');
        addTearDown(() => dest.delete(recursive: true));
        expect(await cache.promoteInto(_track(), dest.path), isNull);
      });

      test('returns null for a failed entry so the caller re-downloads',
          () async {
        final data = _pattern(32 * 1024);
        final net = _FakeNetwork('fake',
            bytes: data, chunkDelay: Duration.zero, failAfter: 1024);
        final cache = build(net: net);
        final track = _track(size: data.length);
        await (await cache.prefetch(track)).finished;

        final dest = await Directory.systemTemp.createTemp('lib-');
        addTearDown(() => dest.delete(recursive: true));
        expect(await cache.promoteInto(track, dest.path), isNull);
      });
    });

    // ----------------------------------------------------------------
    group('expiry', () {
      test('sweeps an entry past its TTL', () async {
        final data = _pattern(4096);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache =
            build(net: net, ttl: const Duration(milliseconds: 40));
        final track = _track(size: data.length);
        final entry = await cache.prefetch(track);
        await entry.finished;
        final dir = entry.dir;
        expect(dir.existsSync(), isTrue);

        await Future<void>.delayed(const Duration(milliseconds: 120));
        await cache.sweep();

        expect(cache.entryFor(track), isNull);
        expect(dir.existsSync(), isFalse);
      });

      test('does not sweep a download in flight', () async {
        final data = _pattern(160 * 1024);
        final net = _FakeNetwork('fake',
            bytes: data, chunk: 8 * 1024,
            chunkDelay: const Duration(milliseconds: 5));
        final cache = build(net: net, ttl: const Duration(milliseconds: 1));
        final track = _track(size: data.length);
        final entry = await cache.prefetch(track);

        await Future<void>.delayed(const Duration(milliseconds: 30));
        await cache.sweep();
        expect(cache.entryFor(track), isNotNull);
        expect(entry.state, CacheState.downloading);

        await entry.finished;
      });

      test('does not sweep the entry that is playing', () async {
        final data = _pattern(4096);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache =
            build(net: net, ttl: const Duration(milliseconds: 40));
        final track = _track(size: data.length);

        final entry = await cache.stream(track);
        await entry.finished;
        // Wait for _playWhenReady to have opened the player.
        final deadline = DateTime.now().add(const Duration(seconds: 5));
        while (!cache.isPreviewing && DateTime.now().isBefore(deadline)) {
          await Future<void>.delayed(const Duration(milliseconds: 5));
        }
        expect(cache.isPreviewing, isTrue);

        await Future<void>.delayed(const Duration(milliseconds: 120));
        await cache.sweep();
        expect(cache.entryFor(track), isNotNull,
            reason: 'the playing file must never be deleted');

        await cache.stopPreview();
        await cache.sweep();
        expect(cache.entryFor(track), isNull);
      });

      test('does not sweep an entry mid-promotion', () async {
        final data = _pattern(4096);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache =
            build(net: net, ttl: const Duration(milliseconds: 20));
        final track = _track(size: data.length);
        final entry = await cache.prefetch(track);
        await entry.finished;

        entry.promoting = true;
        await Future<void>.delayed(const Duration(milliseconds: 60));
        await cache.sweep();
        expect(cache.entryFor(track), isNotNull);
        expect(entry.dir.existsSync(), isTrue);

        entry.promoting = false;
        await cache.sweep();
        expect(cache.entryFor(track), isNull);
      });

      test('init() sweeps leftovers from a previous run', () async {
        // Simulates a crash: directories in the cache root that no live
        // entry claims.
        final sep = Platform.pathSeparator;
        final stale = Directory('${root.path}${sep}deadbeef');
        await stale.create(recursive: true);
        await File('${stale.path}${sep}half.mp3.part')
            .writeAsBytes(_pattern(1024));

        final net = _FakeNetwork('fake', bytes: _pattern(16));
        final cache = build(net: net, ttl: const Duration(milliseconds: 40));

        await Future<void>.delayed(const Duration(milliseconds: 120));
        await cache.init();
        expect(stale.existsSync(), isFalse);
      });

      test('init() keeps leftovers that are still inside the TTL', () async {
        final sep = Platform.pathSeparator;
        final fresh = Directory('${root.path}${sep}freshcafe');
        await fresh.create(recursive: true);

        final net = _FakeNetwork('fake', bytes: _pattern(16));
        final cache = build(net: net, ttl: const Duration(hours: 1));
        await cache.init();
        expect(fresh.existsSync(), isTrue);
      });

      test('clear() drops everything', () async {
        final data = _pattern(4096);
        final net =
            _FakeNetwork('fake', bytes: data, chunkDelay: Duration.zero);
        final cache = build(net: net);
        final t1 = _track(id: 'a', name: 'a.mp3', size: data.length);
        final t2 = _track(id: 'b', name: 'b.mp3', size: data.length);
        await (await cache.prefetch(t1)).finished;
        await (await cache.prefetch(t2)).finished;
        expect(cache.entries.length, 2);

        await cache.clear();
        expect(cache.entries, isEmpty);
        expect(root.listSync(), isEmpty);
      });
    });
  });

  // ------------------------------------------------------------------
  group('format capability reporting', () {
    test('progressive formats are the ones with a leading header', () {
      for (final e in ['mp3', 'MP3', 'flac', 'ogg', 'opus', 'wav']) {
        expect(isProgressiveFriendly(e), isTrue, reason: e);
      }
      // Honest about what cannot stream: the index is at the end.
      for (final e in ['m4a', 'mp4', 'M4A']) {
        expect(isProgressiveFriendly(e), isFalse, reason: e);
        expect(isTailIndexed(e), isTrue, reason: e);
      }
      expect(isProgressiveFriendly(null), isFalse);
      expect(isProgressiveFriendly(''), isFalse);
      // Unknown extensions get the conservative answer.
      expect(isProgressiveFriendly('xyz'), isFalse);
      expect(isTailIndexed('xyz'), isFalse);
    });
  });
}
