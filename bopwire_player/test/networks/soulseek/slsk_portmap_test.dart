// NAT port mapping behaviour (ported from Nicotine+): the client asks the
// router for a UPnP/NAT-PMP mapping of its listen port and announces the
// EXTERNAL port to the server via SetWaitPort — announcing the internal
// port of a NATed host gives peers an unreachable address, which is what
// made NAT-to-NAT transfers impossible.
//
// Driven against a loopback fake of the login server with an injected fake
// mapper, so it runs offline and without native code.
import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/ffi/portmap_bindings.dart';
import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_share.dart';
import 'package:bopwire_player/src/services/networks/soulseek/soulseek_network.dart';

/// Minimal login server: accepts one connection, answers Login with success
/// and records every frame the client sends.
class _FakeSlskServer {
  _FakeSlskServer(this._server);

  final ServerSocket _server;
  final List<SlskFrame> received = [];
  final _frames = SlskFrameBuffer();
  Socket? _client;

  int get port => _server.port;

  static Future<_FakeSlskServer> start() async {
    final s = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final fake = _FakeSlskServer(s);
    s.listen(fake._onClient);
    return fake;
  }

  void _onClient(Socket socket) {
    _client = socket;
    socket.listen((chunk) {
      _frames.add(chunk);
      for (final f in _frames.readFrames()) {
        received.add(f);
        if (f.code == ServerCode.login) _answerLogin(socket);
      }
    }, onError: (Object _) {});
  }

  void _answerLogin(Socket socket) {
    final payload = (SlskWriter()
          ..boolean(true)
          ..str('Welcome to the fake server')
          ..raw(const [1, 2, 3, 4])) // login IP, reversed-octet uint32
        .take();
    socket.add(frameWithUint32Code(ServerCode.login, payload));
  }

  /// All SetWaitPort announcements seen so far, as port numbers.
  List<int> waitPorts() => [
        for (final f in received)
          if (f.code == ServerCode.setWaitPort) SlskReader(f.payload).uint32(),
      ];

  Future<void> close() async {
    _client?.destroy();
    await _server.close();
  }
}

Future<void> _waitFor(bool Function() check,
    {Duration timeout = const Duration(seconds: 5)}) async {
  final deadline = DateTime.now().add(timeout);
  while (!check()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('condition not met within $timeout');
    }
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late _FakeSlskServer fake;
  late Directory shareDir;

  setUp(() async {
    SharedPreferences.setMockInitialValues({});
    shareDir = await Directory.systemTemp.createTemp('slsk-portmap-share');
    SlskShare.instance.baseDirOverride = shareDir;
    fake = await _FakeSlskServer.start();
  });

  tearDown(() async {
    await fake.close();
    if (shareDir.existsSync()) shareDir.deleteSync(recursive: true);
  });

  SoulseekNetwork makeNet({
    required Future<PortMapping?> Function(int) open,
    required Future<void> Function(int) close,
    List<String>? log,
  }) =>
      SoulseekNetwork(
        serverHost: '127.0.0.1',
        serverPorts: [fake.port],
        usernameOverride: 'tester',
        passwordOverride: 'hunter2',
        portMapOpen: open,
        portMapClose: close,
        logger: (m) => log?.add(m),
      );

  test('mapping success: external port is announced via SetWaitPort',
      () async {
    final mapGate = Completer<void>();
    final closed = <int>[];
    int? requestedPort;

    final net = makeNet(
      open: (port) async {
        requestedPort = port;
        await mapGate.future; // resolve strictly after login
        return PortMapping(
            internalPort: port, externalPort: 62233, externalIp: '203.0.113.7');
      },
      close: (port) async => closed.add(port),
    );

    await net.connect();
    expect(net.status, NetworkStatus.connected);
    expect(requestedPort, isNotNull,
        reason: 'binding the listen port must request a mapping');

    // Login-time announcement: the internal port (mapping still pending).
    await _waitFor(() => fake.waitPorts().isNotEmpty);
    expect(fake.waitPorts().single, requestedPort);

    // Mapping lands -> the EXTERNAL port is (re-)announced.
    mapGate.complete();
    await net.debugPortMapAttempt;
    await _waitFor(() => fake.waitPorts().length >= 2);
    expect(fake.waitPorts().last, 62233);

    // Teardown releases the mapping for the port that was requested.
    await net.disconnect();
    expect(closed, [requestedPort]);
    await net.dispose();
  });

  test('mapping resolved before login: SetWaitPort carries the external port',
      () async {
    final net = makeNet(
      open: (port) async => PortMapping(
          internalPort: port, externalPort: 51413, externalIp: '198.51.100.9'),
      close: (_) async {},
    );

    await net.connect();
    await net.debugPortMapAttempt;
    await _waitFor(() => fake.waitPorts().contains(51413));
    // Whatever the race between login and mapping, the LAST announcement
    // the server keeps must be the reachable external port.
    expect(fake.waitPorts().last, 51413);
    await net.dispose();
  });

  test('mapping failure degrades honestly: internal port + a clear log line',
      () async {
    final log = <String>[];
    final closed = <int>[];
    int? requestedPort;
    final net = makeNet(
      open: (port) async {
        requestedPort = port;
        return null;
      },
      close: (port) async => closed.add(port),
      log: log,
    );

    await net.connect();
    expect(net.status, NetworkStatus.connected);
    await net.debugPortMapAttempt;

    // Only the internal port was ever announced.
    await _waitFor(() => fake.waitPorts().isNotEmpty);
    expect(fake.waitPorts(), [requestedPort]);

    expect(
      log.any((m) => m.contains('only directly-reachable peers')),
      isTrue,
      reason: 'the user must be told why transfers may not start: $log',
    );

    // Still released on teardown: the C side may have a half-open session.
    await net.dispose();
    expect(closed, hasLength(1));
  });

  test('mapper that throws is contained and logged', () async {
    final log = <String>[];
    final net = makeNet(
      open: (_) async => throw StateError('no native library'),
      close: (_) async {},
      log: log,
    );

    await net.connect();
    expect(net.status, NetworkStatus.connected);
    await net.debugPortMapAttempt;
    expect(log.any((m) => m.contains('port mapping error')), isTrue);
    expect(log.any((m) => m.contains('only directly-reachable peers')), isTrue);
    await net.dispose();
  });
}
