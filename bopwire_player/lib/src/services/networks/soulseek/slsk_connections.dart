// Socket plumbing: the login-server connection, peer 'P' connections and the
// peer 'F' file connections.
//
// Ported from Nicotine+ (`pynicotine/slskproto.py`), GPL-3.0-or-later — see
// the licensing note in soulseek_network.dart.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'slsk_codec.dart';
import 'slsk_messages.dart';

typedef SlskLog = void Function(String message);

/// The connection to server.slsknet.org. One per SoulseekNetwork.
class SlskServerConnection {
  SlskServerConnection({SlskLog? log}) : _log = log ?? _noop;

  static void _noop(String _) {}
  final SlskLog _log;

  Socket? _socket;
  StreamSubscription<Uint8List>? _sub;
  final SlskFrameBuffer _frames = SlskFrameBuffer();
  final StreamController<SlskFrame> _messages =
      StreamController<SlskFrame>.broadcast();
  final Completer<void> _closed = Completer<void>();

  Stream<SlskFrame> get messages => _messages.stream;
  Future<void> get done => _closed.future;
  bool get isConnected => _socket != null;

  /// The local address the socket bound to. Reported to peers as our own
  /// address when we announce a listen port.
  String? get localAddress => _socket?.address.address;

  Future<void> connect(String host, int port,
      {Duration timeout = const Duration(seconds: 15)}) async {
    final s = await Socket.connect(host, port, timeout: timeout);
    s.setOption(SocketOption.tcpNoDelay, true);
    _socket = s;
    _sub = s.listen(
      _onData,
      onError: (Object e, StackTrace st) {
        _log('server socket error: $e');
        _finish();
      },
      onDone: _finish,
      cancelOnError: true,
    );
  }

  void _onData(Uint8List chunk) {
    _frames.add(chunk);
    List<SlskFrame> frames;
    try {
      frames = _frames.readFrames();
    } on SlskParseException catch (e) {
      _log('server framing error: $e — closing');
      close();
      return;
    }
    for (final f in frames) {
      if (!_messages.isClosed) _messages.add(f);
    }
  }

  void send(int code, Uint8List payload) {
    final s = _socket;
    if (s == null) throw StateError('Soulseek: server connection is down');
    s.add(frameWithUint32Code(code, payload));
  }

  void _finish() {
    _socket = null;
    if (!_closed.isCompleted) _closed.complete();
    if (!_messages.isClosed) _messages.close();
  }

  Future<void> close() async {
    final s = _socket;
    _socket = null;
    await _sub?.cancel();
    _sub = null;
    try {
      await s?.close();
    } catch (_) {}
    s?.destroy();
    _finish();
  }
}

/// A peer connection carrying framed peer messages (connection type 'P').
///
/// Two ways one comes into existence:
///  * we dial the peer and open with PeerInit (direct), or with
///    PierceFireWall when we are answering a ConnectToPeer (indirect);
///  * the peer dials us and opens with PeerInit / PierceFireWall, which
///    [SlskPeerListener] reads before handing the socket over.
class SlskPeerConnection {
  SlskPeerConnection._(this.username, this._socket, {SlskLog? log})
      : _log = log ?? SlskServerConnection._noop;

  final String username;
  final Socket _socket;
  final SlskLog _log;

  final SlskFrameBuffer _frames = SlskFrameBuffer(maxMessageSize: 134217728);
  final StreamController<SlskFrame> _messages =
      StreamController<SlskFrame>.broadcast();
  final Completer<void> _closed = Completer<void>();
  StreamSubscription<Uint8List>? _sub;
  bool _disposed = false;

  Stream<SlskFrame> get messages => _messages.stream;
  Future<void> get done => _closed.future;
  bool get isOpen => !_disposed;

  /// Wraps a socket whose handshake has already been written/read.
  ///
  /// [data] is the stream of post-handshake bytes. A `Socket` is a
  /// single-subscription stream, so whoever read the handshake off an inbound
  /// socket owns its subscription and must hand the remainder over here —
  /// listening to the socket a second time throws.
  static SlskPeerConnection adopt(
    String username,
    Socket socket, {
    required Stream<Uint8List> data,
    SlskLog? log,
  }) {
    final c = SlskPeerConnection._(username, socket, log: log);
    c._start(data);
    return c;
  }

  /// Dials [address]:[port] directly and sends PeerInit.
  static Future<SlskPeerConnection> dialDirect({
    required String ourUsername,
    required String peerUsername,
    required String address,
    required int port,
    Duration timeout = const Duration(seconds: 8),
    SlskLog? log,
  }) async {
    final s = await Socket.connect(address, port, timeout: timeout);
    s.setOption(SocketOption.tcpNoDelay, true);
    s.add(framePeerInit(
        PeerInitCode.peerInit, SlskOut.peerInit(ourUsername, ConnType.peer)));
    return adopt(peerUsername, s, data: s, log: log);
  }

  /// Dials a peer that asked us (via the server) to connect back, sending
  /// PierceFireWall with their token.
  static Future<SlskPeerConnection> dialPierce({
    required String peerUsername,
    required String address,
    required int port,
    required int token,
    Duration timeout = const Duration(seconds: 8),
    SlskLog? log,
  }) async {
    final s = await Socket.connect(address, port, timeout: timeout);
    s.setOption(SocketOption.tcpNoDelay, true);
    s.add(framePeerInit(
        PeerInitCode.pierceFireWall, SlskOut.pierceFireWall(token)));
    return adopt(peerUsername, s, data: s, log: log);
  }

  void _start(Stream<Uint8List> data) {
    _sub = data.listen(
      (chunk) {
        _frames.add(chunk);
        List<SlskFrame> frames;
        try {
          frames = _frames.readFrames();
        } on SlskParseException catch (e) {
          _log('peer $username framing error: $e');
          close();
          return;
        }
        for (final f in frames) {
          if (!_messages.isClosed) _messages.add(f);
        }
      },
      onError: (Object e, StackTrace st) {
        _log('peer $username socket error: $e');
        close();
      },
      onDone: close,
      cancelOnError: true,
    );
  }

  void send(int code, Uint8List payload) {
    if (_disposed) throw StateError('Soulseek: peer connection to $username closed');
    _socket.add(frameWithUint32Code(code, payload));
  }

  Future<void> close() async {
    if (_disposed) return;
    _disposed = true;
    await _sub?.cancel();
    _sub = null;
    try {
      await _socket.close();
    } catch (_) {}
    _socket.destroy();
    if (!_messages.isClosed) await _messages.close();
    if (!_closed.isCompleted) _closed.complete();
  }
}

/// Result of reading the peer-init frame off a freshly accepted socket.
class IncomingPeerHandshake {
  const IncomingPeerHandshake({
    required this.socket,
    required this.isPierce,
    required this.data,
    this.username,
    this.connType,
    this.token,
  });

  final Socket socket;

  /// True for PierceFireWall (peer answering *our* ConnectToPeer), false for
  /// PeerInit (peer dialling us cold).
  final bool isPierce;

  /// Everything on the connection after the handshake frame, including any
  /// bytes that shared its TCP segment. The listener already owns the socket's
  /// only subscription, so this is the sole way to read the rest.
  final Stream<Uint8List> data;

  final String? username;
  final String? connType;
  final int? token;
}

/// Listens for inbound peer connections and reads their handshake frame.
///
/// Announcing a reachable port is what lets peers deliver search results and
/// file transfers without a server round-trip. When the port is unreachable
/// (the common case behind NAT) peers fall back to asking the server to have
/// us connect out, which still works — this listener is best-effort.
class SlskPeerListener {
  SlskPeerListener({SlskLog? log}) : _log = log ?? SlskServerConnection._noop;

  final SlskLog _log;
  ServerSocket? _server;
  StreamSubscription<Socket>? _sub;
  final StreamController<IncomingPeerHandshake> _incoming =
      StreamController<IncomingPeerHandshake>.broadcast();

  Stream<IncomingPeerHandshake> get incoming => _incoming.stream;
  int? get port => _server?.port;

  /// Binds the first port that is free, preferring the Soulseek default 2234.
  /// Returns null when nothing could be bound (we then rely entirely on
  /// indirect connections).
  Future<int?> bind({List<int> preferred = const [2234, 2235, 2236, 0]}) async {
    for (final p in preferred) {
      try {
        final s = await ServerSocket.bind(InternetAddress.anyIPv4, p,
            shared: false);
        _server = s;
        _sub = s.listen(_onSocket, onError: (Object e, StackTrace _) {
          _log('listen socket error: $e');
        });
        return s.port;
      } catch (_) {
        continue;
      }
    }
    _log('could not bind a listen port; indirect connections only');
    return null;
  }

  void _onSocket(Socket socket) {
    socket.setOption(SocketOption.tcpNoDelay, true);
    final buf = SlskFrameBuffer(codeIsByte: true, maxMessageSize: 16384);

    // A Socket can only be listened to once, so this subscription is the
    // connection's only reader for its whole life. Once the handshake frame is
    // read we stop interpreting bytes and republish them on [relay], which the
    // eventual owner (a peer connection or a file transfer) subscribes to.
    // onPause/onResume are forwarded so TCP backpressure still works.
    late StreamSubscription<Uint8List> sub;
    late StreamController<Uint8List> relay;
    var handshakeDone = false;
    var settled = false;

    relay = StreamController<Uint8List>(
      onPause: () => sub.pause(),
      onResume: () => sub.resume(),
      onCancel: () async {
        await sub.cancel();
        socket.destroy();
      },
    );

    final timer = Timer(const Duration(seconds: 15), () {
      if (settled) return;
      settled = true;
      sub.cancel();
      socket.destroy();
      if (!relay.isClosed) relay.close();
    });

    void abandon(String why) {
      _log(why);
      settled = true;
      timer.cancel();
      sub.cancel();
      socket.destroy();
      if (!relay.isClosed) relay.close();
    }

    void onChunk(Uint8List chunk) {
      if (handshakeDone) {
        if (!relay.isClosed) relay.add(chunk);
        return;
      }
      if (settled) return;

      buf.add(chunk);
      SlskFrame? frame;
      try {
        frame = buf.readFrame();
      } on SlskParseException catch (e) {
        abandon('inbound handshake framing error: $e');
        return;
      }
      if (frame == null) return;

      settled = true;
      handshakeDone = true;
      timer.cancel();

      IncomingPeerHandshake? handshake;
      try {
        if (frame.code == PeerInitCode.peerInit) {
          final init = PeerInitRequest.parse(frame.payload);
          handshake = IncomingPeerHandshake(
            socket: socket,
            isPierce: false,
            data: relay.stream,
            username: init.username,
            connType: init.connType,
          );
        } else if (frame.code == PeerInitCode.pierceFireWall) {
          handshake = IncomingPeerHandshake(
            socket: socket,
            isPierce: true,
            data: relay.stream,
            token: SlskReader(frame.payload).uint32(),
          );
        }
      } catch (e) {
        abandon('inbound handshake parse error: $e');
        return;
      }

      if (handshake == null) {
        abandon('unknown peer-init code ${frame.code}');
        return;
      }

      // Anything that shared the handshake's TCP segment is replayed first.
      // The relay buffers it until the new owner subscribes.
      final leftover = buf.drain();
      if (leftover.isNotEmpty) relay.add(leftover);

      _incoming.add(handshake);
    }

    sub = socket.listen(
      onChunk,
      onError: (Object e, StackTrace _) {
        if (!handshakeDone) {
          abandon('inbound socket error before handshake: $e');
          return;
        }
        if (!relay.isClosed) relay.addError(e);
      },
      onDone: () {
        if (!handshakeDone) {
          if (!settled) {
            settled = true;
            timer.cancel();
            socket.destroy();
          }
        }
        if (!relay.isClosed) relay.close();
      },
    );
  }

  Future<void> close() async {
    await _sub?.cancel();
    _sub = null;
    await _server?.close();
    _server = null;
    if (!_incoming.isClosed) await _incoming.close();
  }
}
