// Minimal SOCKS5 client (RFC 1928) used to reach a seeder's v3 onion.
//
// The onion hostname is sent as an ATYP=DOMAINNAME request and is never
// resolved locally — that is what makes it a Tor request rather than a DNS
// leak, and PROTOCOL.md states it as a MUST: "It MUST NOT resolve or
// connect to the peer outside Tor."

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:async/async.dart';

class Socks5Exception implements Exception {
  Socks5Exception(this.message);
  final String message;
  @override
  String toString() => 'SOCKS5: $message';
}

/// A `host:port` SOCKS5 endpoint.
class Socks5Endpoint {
  const Socks5Endpoint(this.host, this.port);
  final String host;
  final int port;

  @override
  String toString() => '$host:$port';

  /// Parses `host:port`, `port` alone, or an empty string (→ null).
  /// Defaults to Tor's standard 9050 when only a host is given.
  static Socks5Endpoint? tryParse(String? raw) {
    if (raw == null) return null;
    final t = raw.trim();
    if (t.isEmpty) return null;
    final idx = t.lastIndexOf(':');
    if (idx < 0) {
      final onlyPort = int.tryParse(t);
      if (onlyPort != null && onlyPort > 0 && onlyPort < 65536) {
        return Socks5Endpoint('127.0.0.1', onlyPort);
      }
      return Socks5Endpoint(t, 9050);
    }
    final host = t.substring(0, idx).trim();
    final port = int.tryParse(t.substring(idx + 1).trim());
    if (host.isEmpty || port == null || port <= 0 || port >= 65536) return null;
    return Socks5Endpoint(host, port);
  }
}

/// True for a syntactically valid v3 onion hostname: 56 characters of
/// lowercase base32 followed by `.onion`.
bool isV3Onion(String host) {
  if (!host.endsWith('.onion')) return false;
  final service = host.substring(0, host.length - '.onion'.length);
  if (service.length != 56) return false;
  for (final c in service.codeUnits) {
    final isLetter = c >= 0x61 && c <= 0x7a; // a-z
    final isDigit = c >= 0x32 && c <= 0x37; // 2-7
    if (!isLetter && !isDigit) return false;
  }
  return true;
}

/// Builds the SOCKS5 CONNECT request for [host]:[port]. Exposed for tests.
Uint8List buildConnectRequest(String host, int port) {
  final name = Uint8List.fromList(host.codeUnits);
  if (name.isEmpty || name.length > 255) {
    throw Socks5Exception('hostname length out of range');
  }
  final out = Uint8List(4 + 1 + name.length + 2);
  out[0] = 0x05; // version
  out[1] = 0x01; // CONNECT
  out[2] = 0x00; // reserved
  out[3] = 0x03; // ATYP = DOMAINNAME (never resolved locally)
  out[4] = name.length;
  out.setAll(5, name);
  out[5 + name.length] = (port >> 8) & 0xff;
  out[6 + name.length] = port & 0xff;
  return out;
}

const Map<int, String> kSocks5ReplyMessages = {
  0x00: 'succeeded',
  0x01: 'general SOCKS server failure',
  0x02: 'connection not allowed by ruleset',
  0x03: 'network unreachable',
  0x04: 'host unreachable (the onion service may be offline)',
  0x05: 'connection refused',
  0x06: 'TTL expired',
  0x07: 'command not supported',
  0x08: 'address type not supported',
};

/// Reads exactly [n] bytes from a queued byte source.
class ByteReader {
  ByteReader(Stream<List<int>> source) : _queue = StreamQueue(source);
  final StreamQueue<List<int>> _queue;
  final BytesBuilder _buffer = BytesBuilder(copy: false);

  Future<Uint8List> readExactly(int n, {Duration? timeout}) async {
    while (_buffer.length < n) {
      Future<bool> has = _queue.hasNext;
      if (timeout != null) has = has.timeout(timeout);
      if (!await has) {
        throw Socks5Exception('connection closed after ${_buffer.length}/$n bytes');
      }
      final chunk = await _queue.next;
      _buffer.add(chunk);
    }
    final all = _buffer.takeBytes();
    _buffer.add(Uint8List.sublistView(all, n));
    return Uint8List.sublistView(all, 0, n);
  }

  Future<void> cancel() async {
    try {
      await _queue.cancel(immediate: true);
    } catch (_) {/* already torn down */}
  }
}

/// A connected SOCKS5 tunnel: the raw socket plus a reader positioned just
/// after the handshake.
class Socks5Connection {
  Socks5Connection(this.socket, this.reader);
  final Socket socket;
  final ByteReader reader;

  Future<void> destroy() async {
    await reader.cancel();
    try {
      socket.destroy();
    } catch (_) {}
  }
}

/// Opens a TCP connection to [host]:[port] through the SOCKS5 proxy at
/// [proxy]. Only the "no authentication" method is offered; Tor's default
/// SocksPort accepts it.
Future<Socks5Connection> socks5Connect(
  Socks5Endpoint proxy,
  String host,
  int port, {
  Duration timeout = const Duration(seconds: 60),
}) async {
  final socket = await Socket.connect(proxy.host, proxy.port, timeout: timeout);
  socket.setOption(SocketOption.tcpNoDelay, true);
  final reader = ByteReader(socket);
  try {
    socket.add(Uint8List.fromList([0x05, 0x01, 0x00]));
    await socket.flush();
    final greeting = await reader.readExactly(2, timeout: timeout);
    if (greeting[0] != 0x05) {
      throw Socks5Exception('proxy is not SOCKS5 (version ${greeting[0]})');
    }
    if (greeting[1] != 0x00) {
      throw Socks5Exception(
          'proxy demands authentication method 0x${greeting[1].toRadixString(16)}');
    }

    socket.add(buildConnectRequest(host, port));
    await socket.flush();
    final head = await reader.readExactly(4, timeout: timeout);
    if (head[0] != 0x05) throw Socks5Exception('bad reply version ${head[0]}');
    if (head[1] != 0x00) {
      throw Socks5Exception(kSocks5ReplyMessages[head[1]] ??
          'reply code 0x${head[1].toRadixString(16)}');
    }
    switch (head[3]) {
      case 0x01:
        await reader.readExactly(4 + 2, timeout: timeout);
        break;
      case 0x03:
        final len = await reader.readExactly(1, timeout: timeout);
        await reader.readExactly(len[0] + 2, timeout: timeout);
        break;
      case 0x04:
        await reader.readExactly(16 + 2, timeout: timeout);
        break;
      default:
        throw Socks5Exception('unknown bound address type ${head[3]}');
    }
    return Socks5Connection(socket, reader);
  } catch (_) {
    await reader.cancel();
    try {
      socket.destroy();
    } catch (_) {}
    rethrow;
  }
}

/// Cheap reachability probe: can we open a SOCKS5 greeting with the proxy?
/// Used so `connect()` can tell the user up front that downloads will not
/// work rather than failing at the first transfer.
Future<bool> socks5ProxyReachable(
  Socks5Endpoint proxy, {
  Duration timeout = const Duration(seconds: 4),
}) async {
  Socket? socket;
  ByteReader? reader;
  try {
    socket = await Socket.connect(proxy.host, proxy.port, timeout: timeout);
    reader = ByteReader(socket);
    socket.add(Uint8List.fromList([0x05, 0x01, 0x00]));
    await socket.flush();
    final greeting = await reader.readExactly(2, timeout: timeout);
    return greeting[0] == 0x05 && greeting[1] == 0x00;
  } catch (_) {
    return false;
  } finally {
    await reader?.cancel();
    try {
      socket?.destroy();
    } catch (_) {}
  }
}
