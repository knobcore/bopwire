// A small multi-relay Nostr client on dart:io WebSocket.
//
// dart:io's WebSocket is deliberate: it ships with the Dart VM and works
// unchanged on Linux, Windows and Android, so napstr adds no new package
// dependency for its transport. (It has no web support, which matches
// bopwire's desktop+Android targets.)
//
// The pool tolerates dead relays: a failed connection is recorded and
// skipped, never fatal, because napstr's default relay set routinely has
// one or two relays returning 503.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'event.dart';

/// Relay defaults taken verbatim from napstr's PROTOCOL.md.
const List<String> kDefaultNapstrRelays = [
  'wss://relay.damus.io',
  'wss://nos.lol',
  'wss://relay.nostr.com',
  'wss://relay.primal.net',
  'wss://relay.snort.social',
  'wss://nostr.mom',
  'wss://relay.nostr.band',
];

/// Parses a user-supplied relay list (comma, whitespace or newline
/// separated), keeping only well-formed ws/wss URLs and de-duplicating.
/// Returns [kDefaultNapstrRelays] when nothing usable is left.
List<String> parseRelayList(String? raw) {
  if (raw == null || raw.trim().isEmpty) return kDefaultNapstrRelays;
  final out = <String>[];
  for (final part in raw.split(RegExp(r'[,\s]+'))) {
    final t = part.trim();
    if (t.isEmpty) continue;
    final uri = Uri.tryParse(t);
    if (uri == null) continue;
    if (uri.scheme != 'ws' && uri.scheme != 'wss') continue;
    if (uri.host.isEmpty) continue;
    final normalised = uri.toString().replaceAll(RegExp(r'/+$'), '');
    if (!out.contains(normalised)) out.add(normalised);
  }
  return out.isEmpty ? kDefaultNapstrRelays : out;
}

class _Relay {
  _Relay(this.url);
  final String url;
  WebSocket? socket;
  String? lastError;
  bool get isOpen => socket?.readyState == WebSocket.open;
}

/// One event as delivered by one relay.
class RelayEvent {
  const RelayEvent(this.relayUrl, this.event);
  final String relayUrl;
  final NostrEvent event;
}

class RelayPool {
  RelayPool(List<String> urls)
      : _relays = [for (final u in urls) _Relay(u)];

  final List<_Relay> _relays;
  int _subCounter = 0;

  List<String> get urls => [for (final r in _relays) r.url];
  int get connectedCount => _relays.where((r) => r.isOpen).length;
  Iterable<String> get errors =>
      _relays.where((r) => r.lastError != null).map((r) => '${r.url}: ${r.lastError}');

  /// Opens every relay, in parallel, ignoring individual failures.
  /// Returns the number that came up.
  Future<int> connect({Duration timeout = const Duration(seconds: 12)}) async {
    await Future.wait([
      for (final r in _relays) _open(r, timeout),
    ]);
    return connectedCount;
  }

  /// Reconnect any relay whose socket has died, and report how many are
  /// live afterwards. Cheap when everything is already up (no I/O), so
  /// callers can front every publish/subscribe with it rather than
  /// discovering a dead pool by timing out 60 seconds later.
  Future<int> ensureLive({
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final dead = _relays.where((r) => !r.isOpen).toList();
    if (dead.isEmpty) return connectedCount;
    await Future.wait([for (final r in dead) _open(r, timeout)]);
    return connectedCount;
  }

  Future<void> _open(_Relay r, Duration timeout) async {
    if (r.isOpen) return;
    try {
      final ws = await WebSocket.connect(r.url).timeout(timeout);
      // Nostr relays drop idle connections after 30-120s. Without a ping
      // the socket goes HALF-OPEN: readyState still reports `open`, so
      // publish() "succeeds" locally while the relay is long gone and no
      // events ever come back. That presents as "request delivered, no
      // seeder answered" — search and preview work because they happen
      // soon after connect, and a download minutes later does not.
      //
      // pingInterval makes Dart send WebSocket pings and tear the socket
      // down when pongs stop, so isOpen becomes truthful and ensureLive()
      // below can reconnect.
      ws.pingInterval = const Duration(seconds: 20);
      ws.done.then((_) {
        if (identical(r.socket, ws)) r.socket = null;
      }, onError: (_) {
        if (identical(r.socket, ws)) r.socket = null;
      });
      r.socket = ws;
      r.lastError = null;
      // A single broadcast listener per socket; subscriptions filter by id.
      _attach(r, ws);
      // Rejoin every subscription that is still live on this fresh socket.
      for (final req in _liveReqs.values) {
        try {
          ws.add(req);
        } catch (_) {}
      }
    } catch (e) {
      r.socket = null;
      r.lastError = e.toString();
    }
  }

  final Map<String, void Function(String relayUrl, List<Object?> msg)> _routes = {};

  /// REQ payloads of subscriptions that are still live, keyed by sub id.
  /// When ensureLive() reopens a relay that died, these are re-sent to
  /// it — otherwise a reopened socket is connected but deaf: the inbox
  /// subscription a download relies on was only ever REQ'd on the relays
  /// that happened to be up when it started.
  final Map<String, String> _liveReqs = {};

  void _attach(_Relay r, WebSocket ws) {
    ws.listen(
      (raw) {
        if (raw is! String) return;
        Object? decoded;
        try {
          decoded = jsonDecode(raw);
        } catch (_) {
          return;
        }
        if (decoded is! List || decoded.isEmpty) return;
        final type = decoded[0];
        if (type is! String) return;
        final subId = decoded.length > 1 && decoded[1] is String
            ? decoded[1] as String
            : null;
        if (subId == null) return;
        _routes[subId]?.call(r.url, decoded);
      },
      onError: (_) {},
      onDone: () {},
      cancelOnError: false,
    );
  }

  Future<void> close() async {
    _routes.clear();
    _liveReqs.clear();
    for (final r in _relays) {
      try {
        await r.socket?.close();
      } catch (_) {}
      r.socket = null;
    }
  }

  /// One-shot query: sends [filters] to every open relay and emits events
  /// until all relays have sent EOSE or [timeout] elapses, whichever first.
  /// Duplicate event ids across relays are suppressed.
  Stream<RelayEvent> query(
    List<Map<String, Object?>> filters, {
    Duration timeout = const Duration(seconds: 8),
    int? maxEvents,
  }) {
    final subId = 'bw${_subCounter++}';
    final controller = StreamController<RelayEvent>();
    final seen = <String>{};
    final open = _relays.where((r) => r.isOpen).toList();
    final pending = <String>{for (final r in open) r.url};
    var emitted = 0;
    Timer? deadline;
    var closed = false;

    Future<void> finish() async {
      if (closed) return;
      closed = true;
      deadline?.cancel();
      _routes.remove(subId);
      _liveReqs.remove(subId);
      for (final r in open) {
        try {
          r.socket?.add(jsonEncode(['CLOSE', subId]));
        } catch (_) {}
      }
      await controller.close();
    }

    _routes[subId] = (relayUrl, msg) {
      if (closed) return;
      final type = msg[0];
      if (type == 'EVENT' && msg.length >= 3) {
        final ev = NostrEvent.tryParse(msg[2]);
        if (ev == null) return;
        if (!seen.add(ev.id)) return;
        controller.add(RelayEvent(relayUrl, ev));
        emitted++;
        if (maxEvents != null && emitted >= maxEvents) finish();
      } else if (type == 'EOSE' || type == 'CLOSED') {
        pending.remove(relayUrl);
        if (pending.isEmpty) finish();
      }
    };

    controller.onCancel = finish;

    if (open.isEmpty) {
      scheduleMicrotask(finish);
      return controller.stream;
    }

    final req = jsonEncode(['REQ', subId, ...filters]);
    _liveReqs[subId] = req;
    for (final r in open) {
      try {
        r.socket!.add(req);
      } catch (_) {
        pending.remove(r.url);
      }
    }
    if (pending.isEmpty) {
      scheduleMicrotask(finish);
    } else {
      deadline = Timer(timeout, finish);
    }
    return controller.stream;
  }

  /// Long-lived subscription. Stays open until the subscription is
  /// cancelled — used to listen for NIP-17 gift wraps addressed to us.
  Stream<RelayEvent> subscribe(List<Map<String, Object?>> filters) {
    final subId = 'bwlive${_subCounter++}';
    final controller = StreamController<RelayEvent>();
    final seen = <String>{};
    var closed = false;

    Future<void> stop() async {
      if (closed) return;
      closed = true;
      _routes.remove(subId);
      _liveReqs.remove(subId);
      for (final r in _relays) {
        if (!r.isOpen) continue;
        try {
          r.socket!.add(jsonEncode(['CLOSE', subId]));
        } catch (_) {}
      }
      await controller.close();
    }

    _routes[subId] = (relayUrl, msg) {
      if (closed) return;
      if (msg[0] == 'EVENT' && msg.length >= 3) {
        final ev = NostrEvent.tryParse(msg[2]);
        if (ev == null || !seen.add(ev.id)) return;
        controller.add(RelayEvent(relayUrl, ev));
      }
    };
    controller.onCancel = stop;

    final req = jsonEncode(['REQ', subId, ...filters]);
    _liveReqs[subId] = req;
    for (final r in _relays) {
      if (!r.isOpen) continue;
      try {
        r.socket!.add(req);
      } catch (_) {}
    }
    return controller.stream;
  }

  /// Publishes [event] to every open relay. Returns the number of relays
  /// that replied with a positive OK before [timeout]; a relay that never
  /// answers is simply not counted.
  Future<int> publish(
    NostrEvent event, {
    Duration timeout = const Duration(seconds: 8),
  }) async {
    final open = _relays.where((r) => r.isOpen).toList();
    if (open.isEmpty) return 0;
    final done = Completer<int>();
    var accepted = 0;
    var answered = 0;
    Timer? deadline;

    void settle() {
      if (done.isCompleted) return;
      deadline?.cancel();
      _routes.remove(event.id);
      done.complete(accepted);
    }

    // Relays key OK responses by event id, so route on that.
    _routes[event.id] = (relayUrl, msg) {
      if (msg[0] != 'OK') return;
      answered++;
      if (msg.length >= 3 && msg[2] == true) accepted++;
      if (answered >= open.length) settle();
    };

    final payload = jsonEncode(['EVENT', event.toJson()]);
    for (final r in open) {
      try {
        r.socket!.add(payload);
      } catch (_) {
        answered++;
      }
    }
    deadline = Timer(timeout, settle);
    return done.future;
  }
}
