// Soulseek — ExternalNetwork implementation (id 'slsk').
//
// ===========================================================================
// LICENSING NOTE FOR THE MAINTAINERS — PLEASE READ
// ===========================================================================
// The Soulseek protocol is undocumented by its authors. Everything in this
// directory (message codes, field order and types, the md5 login hash, the
// zlib-compressed peer payloads, the >2 GiB file-size quirk, the peer-init /
// PierceFireWall handshake and the 'F' file-connection sequence) was derived
// by reading Nicotine+:
//
//     https://github.com/nicotine-plus/nicotine-plus
//     pynicotine/slskmessages.py, pynicotine/slskproto.py
//     SPDX-License-Identifier: GPL-3.0-or-later
//
// No Nicotine+ source was copied verbatim — this is a clean Dart
// reimplementation — but it is unambiguously *derived from* a GPLv3 work in
// the sense that matters legally: the protocol description was taken from it.
// A conservative reading means bopwire, when distributed with this directory
// compiled in, inherits GPL-3.0-or-later obligations.
//
// Options, for the project to decide:
//   1. Accept GPLv3 for the whole app.
//   2. Ship Soulseek support as a separately-distributed optional plugin.
//   3. Re-derive the protocol from a non-GPL description (the Museek /
//      SoulseekQt protocol write-ups) and document that provenance instead.
//   4. Drop Soulseek support.
// Nothing here should ship publicly until that call is made.
// ===========================================================================

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import '../external_network.dart';
import '../network_credentials.dart';
import 'slsk_codec.dart';
import 'slsk_connections.dart';
import 'slsk_download.dart';
import 'slsk_messages.dart';
import 'slsk_share.dart';

const String kSoulseekNetworkId = 'slsk';

/// Default login server. Port 2416 is the documented alternate.
const String kSoulseekServerHost = 'server.slsknet.org';
const List<int> kSoulseekServerPorts = [2242, 2416];

class SoulseekNetwork implements ExternalNetwork {
  SoulseekNetwork({
    String serverHost = kSoulseekServerHost,
    List<int> serverPorts = kSoulseekServerPorts,
    void Function(String)? logger,
    String? usernameOverride,
    String? passwordOverride,
  })  : _serverHost = serverHost,
        _serverPorts = serverPorts,
        _usernameOverride = usernameOverride,
        _passwordOverride = passwordOverride,
        _log = logger ?? _defaultLog;

  static void _defaultLog(String m) {
    assert(() {
      // ignore: avoid_print
      print('[slsk] $m');
      return true;
    }());
  }

  final String _serverHost;
  final List<int> _serverPorts;
  final void Function(String) _log;

  /// Test-only escape hatch: credentials handed straight to the
  /// constructor, bypassing NetworkCredentials. The live integration test
  /// needs this because the real password lives in the wallet vault,
  /// which does not exist under `flutter test`.
  final String? _usernameOverride;
  final String? _passwordOverride;

  // --- ExternalNetwork identity -------------------------------------------

  @override
  String get id => kSoulseekNetworkId;

  @override
  String get displayName => 'Soulseek';

  @override
  List<NetworkCredentialField> get credentialFields => const [
        NetworkCredentialField(
          key: 'username',
          label: 'Username',
          hint: 'Your Soulseek account name',
        ),
        NetworkCredentialField(
          key: 'password',
          label: 'Password',
          secret: true,
        ),
      ];

  @override
  bool get isConfigured =>
      NetworkCredentials.instance.hasRequired(id, credentialFields);

  bool _enabled = false;

  @override
  bool get enabled => _enabled;

  @override
  set enabled(bool v) => _enabled = v;

  NetworkStatus _status = NetworkStatus.disconnected;
  final StreamController<NetworkStatus> _statusCtl =
      StreamController<NetworkStatus>.broadcast();

  @override
  NetworkStatus get status => _status;

  @override
  Stream<NetworkStatus> get statusChanges => _statusCtl.stream;

  /// Human-readable reason for the most recent [NetworkStatus.error].
  String? get lastError => _lastError;
  String? _lastError;

  void _setStatus(NetworkStatus s, {String? error}) {
    _lastError = error;
    if (_status == s) return;
    _status = s;
    if (!_statusCtl.isClosed) _statusCtl.add(s);
  }

  // --- connection state ----------------------------------------------------

  SlskServerConnection? _server;
  SlskPeerListener? _listener;
  StreamSubscription<SlskFrame>? _serverSub;
  StreamSubscription<IncomingPeerHandshake>? _listenSub;
  Future<void>? _connecting;

  String? _username;
  int? _listenPort;

  final Random _rng = Random.secure();
  late int _token = _rng.nextInt(0x3FFFFFFF) + 1;
  int _nextToken() {
    _token = (_token + 1) & 0x7FFFFFFF;
    if (_token == 0) _token = 1;
    return _token;
  }

  final Map<String, SlskPeerConnection> _peers = {};
  static const int _maxPeerConnections = 96;

  /// Indirect connection attempts we started: token -> completer waiting for
  /// the peer to PierceFireWall us back.
  final Map<int, Completer<IncomingPeerHandshake>> _pendingIndirect = {};

  /// GetPeerAddress replies, keyed by username.
  final Map<String, List<Completer<PeerAddress>>> _addressWaiters = {};

  final Map<int, _ActiveSearch> _searches = {};
  final Map<int, Completer<FolderContentsResponse>> _folderWaiters = {};

  /// Downloads waiting for the uploader's TransferRequest, keyed by
  /// `username virtualPath`.
  final Map<String, SlskDownload> _queued = {};

  /// Downloads that have a transfer token, keyed by that token.
  final Map<int, SlskDownload> _active = {};

  /// Uploads of our one shared file (slsk_share.dart), keyed by OUR
  /// transfer token. Created when a peer queues the file, discarded when
  /// the bytes have been pushed (or the offer times out).
  final Map<int, _SlskUpload> _uploads = {};

  Completer<LoginResponse>? _loginWaiter;

  // --- connect / disconnect ------------------------------------------------

  /// Brings the server connection up. Never throws: a failure is reported as
  /// [NetworkStatus.error] with the reason in [lastError], so the settings and
  /// search UI can render it without a try/catch.
  @override
  Future<void> connect() {
    if (_status == NetworkStatus.connected && _server?.isConnected == true) {
      return Future.value();
    }
    return _connecting ??= _connect()
        .catchError((Object e) {
          // _connect already recorded a specific message via _setStatus; this
          // only catches anything unforeseen on the way there.
          if (_status != NetworkStatus.error) {
            _setStatus(NetworkStatus.error, error: '$e');
          }
        })
        .whenComplete(() => _connecting = null);
  }

  /// connect(), then insist it worked. Used by the operations that cannot do
  /// anything useful while disconnected.
  Future<void> _requireConnection() async {
    await connect();
    if (_status != NetworkStatus.connected) {
      throw StateError(_lastError ?? 'Soulseek is not connected');
    }
  }

  Future<void> _connect() async {
    _setStatus(NetworkStatus.connecting);

    String? user = _usernameOverride;
    String? pass = _passwordOverride;
    if (user == null || pass == null) {
      await NetworkCredentials.instance.load(id, credentialFields);
      user ??= NetworkCredentials.instance.get(id, 'username');
      pass ??= NetworkCredentials.instance.get(id, 'password');
    }

    if (user == null || user.isEmpty) {
      _setStatus(NetworkStatus.error,
          error: 'Soulseek username is not set. Add it in settings.');
      throw StateError(_lastError!);
    }
    if (pass == null || pass.isEmpty) {
      // The password lives in the wallet vault, so this is the normal message
      // when the wallet is locked rather than a misconfiguration.
      _setStatus(NetworkStatus.error,
          error: 'Soulseek password unavailable — unlock your wallet '
              '(the password is stored in the encrypted vault).');
      throw StateError(_lastError!);
    }

    Object? lastFailure;
    for (final port in _serverPorts) {
      try {
        await _openServer(user, pass, port);
        return;
      } catch (e) {
        lastFailure = e;
        _log('login via $_serverHost:$port failed: $e');
        await _teardown(keepStatus: true);
      }
    }
    _setStatus(NetworkStatus.error,
        error: 'Could not reach the Soulseek server: $lastFailure');
    throw StateError(_lastError!);
  }

  Future<void> _openServer(String user, String pass, int port) async {
    final server = SlskServerConnection(log: _log);
    await server.connect(_serverHost, port);
    _server = server;
    _username = user;

    _serverSub = server.messages.listen(_onServerMessage);
    unawaited(server.done.then((_) => _onServerClosed()));

    final listener = SlskPeerListener(log: _log);
    _listenPort = await listener.bind();
    _listener = listener;
    _listenSub = listener.incoming.listen(_onIncomingPeer);

    final waiter = _loginWaiter = Completer<LoginResponse>();
    server.send(ServerCode.login, SlskOut.login(user, pass));

    final LoginResponse resp;
    try {
      resp = await waiter.future.timeout(const Duration(seconds: 30));
    } on TimeoutException {
      await _teardown(keepStatus: true);
      throw StateError('Soulseek server did not answer the login.');
    } finally {
      _loginWaiter = null;
    }

    if (!resp.success) {
      final reason = resp.failureReason ?? 'unknown reason';
      await _teardown(keepStatus: true);
      _setStatus(NetworkStatus.error,
          error: 'Soulseek rejected the login: $reason');
      throw StateError(_lastError!);
    }

    // Post-login housekeeping, in the order Nicotine+ sends it.
    server.send(ServerCode.setWaitPort, SlskOut.setWaitPort(_listenPort ?? 0));
    server.send(ServerCode.setStatus, SlskOut.setStatus(2)); // online
    // Announce our one shared folder. Announcing 0/0 made every peer that
    // refuses non-sharers reject our downloads before they started
    // (UploadDenied "Banned"). See slsk_share.dart: we share a single
    // explanatory text file, NOT the user's library.
    unawaited(SlskShare.instance.ensureReady().then((_) {
      server.send(
          ServerCode.sharedFoldersFiles,
          SlskOut.sharedFoldersFiles(
              SlskShare.instance.folderCount, SlskShare.instance.fileCount));
    }).catchError((Object _) {
      // Could not create the share file — fall back to honest zeros
      // rather than claiming a share we cannot serve.
      server.send(
          ServerCode.sharedFoldersFiles, SlskOut.sharedFoldersFiles(0, 0));
    }));
    // We do not participate in the distributed search overlay — we only issue
    // our own searches — so we declare ourselves a childless branch root.
    server.send(ServerCode.haveNoParent, SlskOut.boolMessage(true));
    server.send(ServerCode.branchRoot, SlskOut.stringMessage(user));
    server.send(ServerCode.branchLevel, SlskOut.uint32Message(0));
    server.send(ServerCode.acceptChildren, SlskOut.boolMessage(false));

    _log('logged in as $user (listen port ${_listenPort ?? "none"})');
    _setStatus(NetworkStatus.connected);
  }

  void _onServerClosed() {
    if (_status == NetworkStatus.connected) {
      _setStatus(NetworkStatus.disconnected);
    }
  }

  @override
  Future<void> disconnect() => _teardown();

  Future<void> _teardown({bool keepStatus = false}) async {
    for (final s in _searches.values.toList()) {
      s.close();
    }
    _searches.clear();
    for (final w in _folderWaiters.values) {
      if (!w.isCompleted) {
        w.completeError(StateError('Soulseek disconnected'));
      }
    }
    _folderWaiters.clear();
    for (final c in _pendingIndirect.values) {
      if (!c.isCompleted) {
        c.completeError(StateError('Soulseek disconnected'));
      }
    }
    _pendingIndirect.clear();
    for (final list in _addressWaiters.values) {
      for (final c in list) {
        if (!c.isCompleted) {
          c.completeError(StateError('Soulseek disconnected'));
        }
      }
    }
    _addressWaiters.clear();
    final transfers = <SlskDownload>{..._queued.values, ..._active.values};
    _queued.clear();
    _active.clear();
    await Future.wait(
        [for (final d in transfers) d.shutdown('Soulseek disconnected')]);
    for (final u in _uploads.values) {
      u.expiry?.cancel();
    }
    _uploads.clear();

    for (final p in _peers.values.toList()) {
      await p.close();
    }
    _peers.clear();

    await _serverSub?.cancel();
    _serverSub = null;
    await _listenSub?.cancel();
    _listenSub = null;
    await _listener?.close();
    _listener = null;
    await _server?.close();
    _server = null;
    _listenPort = null;

    if (!keepStatus) _setStatus(NetworkStatus.disconnected);
  }

  /// Awaitable teardown for the app-exit hook: aborts every transfer (so
  /// peers see our slots freed rather than a vanished socket), closes all
  /// peer connections, the listener and the server connection, and only
  /// completes when they are actually released. The network can connect()
  /// again afterwards.
  Future<void> shutdown() => _teardown();

  /// Releases every resource including the status stream. Call on app exit.
  Future<void> dispose() async {
    await _teardown();
    await _statusCtl.close();
  }

  // --- server message dispatch ---------------------------------------------

  void _onServerMessage(SlskFrame frame) {
    try {
      switch (frame.code) {
        case ServerCode.login:
          final w = _loginWaiter;
          if (w != null && !w.isCompleted) {
            w.complete(LoginResponse.parse(frame.payload));
          }

        case ServerCode.getPeerAddress:
          final addr = PeerAddress.parse(frame.payload);
          final waiters = _addressWaiters.remove(addr.username);
          if (waiters != null) {
            for (final c in waiters) {
              if (!c.isCompleted) c.complete(addr);
            }
          }

        case ServerCode.connectToPeer:
          unawaited(
              _onConnectToPeer(ConnectToPeerRequest.parse(frame.payload)));

        case ServerCode.cantConnectToPeer:
          final token = SlskReader(frame.payload).uint32();
          final c = _pendingIndirect.remove(token);
          if (c != null && !c.isCompleted) {
            c.completeError(StateError('peer could not connect back'));
          }

        case ServerCode.relogged:
          _log('logged in from another location — server dropped us');
          unawaited(_teardown(keepStatus: true));
          _setStatus(NetworkStatus.error,
              error: 'Signed in from another location.');

        case ServerCode.serverPing:
          break;

        default:
          // Room lists, recommendations, privileged users, distributed
          // bookkeeping — nothing this client acts on.
          break;
      }
    } catch (e) {
      _log('error handling server message ${frame.code}: $e');
    }
  }

  /// A peer (or a peer we asked for) wants us to dial them.
  Future<void> _onConnectToPeer(ConnectToPeerRequest req) async {
    if (req.connType == ConnType.distributed || req.port <= 0) {
      // We don't join the distributed overlay, and an unroutable address is
      // nothing we can act on.
      _serverSend(ServerCode.cantConnectToPeer,
          SlskOut.cantConnectToPeer(req.token, req.username));
      return;
    }

    try {
      if (req.connType == ConnType.file) {
        final socket = await Socket.connect(req.ipAddress, req.port,
            timeout: const Duration(seconds: 8));
        socket.setOption(SocketOption.tcpNoDelay, true);
        socket.add(framePeerInit(
            PeerInitCode.pierceFireWall, SlskOut.pierceFireWall(req.token)));
        _routeFileSocket(req.username, socket, socket);
        return;
      }

      final conn = await SlskPeerConnection.dialPierce(
        peerUsername: req.username,
        address: req.ipAddress,
        port: req.port,
        token: req.token,
        log: _log,
      );
      _registerPeer(req.username, conn);
    } catch (e) {
      _log('indirect connect to ${req.username} failed: $e');
      _serverSend(ServerCode.cantConnectToPeer,
          SlskOut.cantConnectToPeer(req.token, req.username));
    }
  }

  void _serverSend(int code, Uint8List payload) {
    try {
      _server?.send(code, payload);
    } catch (e) {
      _log('could not send server message $code: $e');
    }
  }

  // --- inbound peer connections --------------------------------------------

  void _onIncomingPeer(IncomingPeerHandshake h) {
    if (h.isPierce) {
      final c = _pendingIndirect.remove(h.token);
      if (c == null || c.isCompleted) {
        _log('unsolicited PierceFireWall token ${h.token}');
        h.socket.destroy();
        return;
      }
      c.complete(h);
      return;
    }

    final user = h.username;
    switch (h.connType) {
      case ConnType.peer:
        if (user == null) {
          h.socket.destroy();
          return;
        }
        _registerPeer(
          user,
          SlskPeerConnection.adopt(user, h.socket, data: h.data, log: _log),
        );
      case ConnType.file:
        _routeFileSocket(user, h.socket, h.data);
      default:
        // 'D' — distributed children. We don't accept any.
        h.socket.destroy();
    }
  }

  void _registerPeer(String username, SlskPeerConnection conn) {
    final old = _peers.remove(username);
    if (old != null && !identical(old, conn)) unawaited(old.close());

    if (_peers.length >= _maxPeerConnections) {
      // A busy search fills this up fast. Evict the oldest connection that is
      // not carrying a transfer — dropping one of those would abort a download.
      final busy = {
        ..._queued.values.map((d) => d.username),
        ..._active.values.map((d) => d.username),
        ..._uploads.values.map((u) => u.username),
      };
      final victim = _peers.keys.cast<String?>().firstWhere(
            (u) => !busy.contains(u),
            orElse: () => null,
          );
      if (victim != null) unawaited(_peers.remove(victim)?.close());
    }
    _peers[username] = conn;

    conn.messages.listen(
      (f) => _onPeerMessage(username, conn, f),
      onError: (Object e, StackTrace _) => _log('peer $username: $e'),
    );
    unawaited(conn.done.then((_) {
      if (identical(_peers[username], conn)) _peers.remove(username);
    }));
  }

  void _onPeerMessage(String username, SlskPeerConnection conn, SlskFrame f) {
    try {
      switch (f.code) {
        case PeerCode.fileSearchResponse:
          final resp = FileSearchResponse.parse(f.payload);
          final search = _searches[resp.token];
          if (search != null) {
            // Only surface peers who can serve RIGHT NOW. A peer with no
            // free upload slot will queue the request; one reporting a
            // non-empty queue alongside a "free" slot is reporting
            // nonsense and is not worth the gamble either. Dropping them
            // here (not dimming in the UI) is deliberate: a row that
            // sits queued when tapped is a row the user should never
            // have seen. The UI layer knows nothing about slot state.
            if (!resp.freeUploadSlots || resp.queueLength > 0) {
              search.dropBusy(resp);
            } else {
              search.add(resp);
            }
          }

        case PeerCode.folderContentsResponse:
          final resp = FolderContentsResponse.parse(f.payload);
          final w = _folderWaiters.remove(resp.token);
          if (w != null && !w.isCompleted) w.complete(resp);

        case PeerCode.transferRequest:
          _onTransferRequest(
              username, conn, TransferRequestMessage.parse(f.payload));

        case PeerCode.transferResponse:
          final r = TransferResponseMessage.parse(f.payload);
          final up = _uploads[r.token];
          if (up != null) {
            // The downloader answered OUR TransferRequest for the shared
            // file. Allowed means: open the 'F' connection and push bytes
            // NOW — accepting and never sending is worse than not sharing.
            if (r.allowed && !up.started) {
              up.started = true;
              unawaited(_pushSharedFile(up));
            } else if (!r.allowed) {
              up.expiry?.cancel();
              _uploads.remove(r.token);
              _log('$username declined our shared-file upload: ${r.reason}');
            }
          } else if (!r.allowed) {
            _log('peer $username refused transfer ${r.token}: ${r.reason}');
          }

        case PeerCode.placeInQueueResponse:
          final r = PlaceInQueueResponse.parse(f.payload);
          final d = _queued[_queueKey(username, r.virtualPath)];
          d?.queuePosition = r.place;
          if (d != null) {
            _log('queue position for ${r.virtualPath} at $username: '
                '${r.place}');
          }

        case PeerCode.uploadDenied:
          final r = UploadDenied.parse(f.payload);
          final key = _queueKey(username, r.virtualPath);
          if (r.reason == TransferRejectReason.queued) {
            _log('$username queued ${r.virtualPath}');
          } else {
            _queued[key]?.fail('$username refused the file: ${r.reason}');
          }

        case PeerCode.uploadFailed:
          final r = UploadFailed.parse(f.payload);
          _queued[_queueKey(username, r.virtualPath)]
              ?.fail('$username reported an upload failure');

        // A peer asking to download from us. Modern clients (Nicotine+,
        // SoulseekQt) request a file with QueueUpload, NOT with a
        // direction-0 TransferRequest — so not handling this message
        // meant every real-world attempt to fetch our shared file went
        // unanswered and we looked like a broken (lying) uploader.
        case PeerCode.queueUpload:
          final path = SlskReader(f.payload).str();
          if (SlskShare.instance.isSharedPath(path)) {
            unawaited(_offerSharedFile(username));
          } else {
            conn.send(
                PeerCode.uploadDenied,
                SlskOut.uploadDenied(
                    path, TransferRejectReason.fileNotShared));
          }

        // A peer browsing our share. Ignoring this was as bad as sharing
        // nothing: a client that asks and gets silence treats the
        // announced counts as a lie and bans us anyway.
        case PeerCode.sharedFileListRequest:
          unawaited(SlskShare.instance.ensureReady().then((_) {
            conn.send(PeerCode.sharedFileListResponse,
                SlskShare.instance.buildSharedFileListResponse());
          }).catchError((Object _) {}));
          break;

        case PeerCode.userInfoRequest:
          // Answering properly needs a share/description; ignoring is safe.
          break;

        default:
          break;
      }
    } catch (e) {
      _log('error handling peer message ${f.code} from $username: $e');
    }
  }

  static String _queueKey(String user, String path) => '$user $path';

  // --- peer connection acquisition -----------------------------------------

  Future<SlskPeerConnection> _peerFor(String username) async {
    final existing = _peers[username];
    if (existing != null && existing.isOpen) return existing;

    final server = _server;
    if (server == null || !server.isConnected) {
      throw StateError('Soulseek: not connected');
    }

    // 1. Ask the server where they are and try a direct dial.
    PeerAddress? addr;
    try {
      addr = await _lookupAddress(username);
    } catch (e) {
      _log('address lookup for $username failed: $e');
    }

    if (addr != null && addr.isRoutable) {
      try {
        final conn = await SlskPeerConnection.dialDirect(
          ourUsername: _username!,
          peerUsername: username,
          address: addr.ipAddress,
          port: addr.port,
          log: _log,
        );
        _registerPeer(username, conn);
        return conn;
      } catch (e) {
        _log('direct dial to $username failed: $e');
      }
    }

    // 2. Fall back to asking the server to have them dial us. This needs our
    //    listen port to be reachable, which behind NAT it usually is not.
    final token = _nextToken();
    final completer = Completer<IncomingPeerHandshake>();
    _pendingIndirect[token] = completer;
    _serverSend(ServerCode.connectToPeer,
        SlskOut.connectToPeer(token, username, ConnType.peer));

    final IncomingPeerHandshake h;
    try {
      h = await completer.future.timeout(const Duration(seconds: 20));
    } on TimeoutException {
      throw StateError('Could not reach Soulseek user $username');
    } finally {
      _pendingIndirect.remove(token);
    }

    final conn =
        SlskPeerConnection.adopt(username, h.socket, data: h.data, log: _log);
    _registerPeer(username, conn);
    return conn;
  }

  Future<PeerAddress> _lookupAddress(String username) {
    final c = Completer<PeerAddress>();
    (_addressWaiters[username] ??= []).add(c);
    _serverSend(ServerCode.getPeerAddress, SlskOut.getPeerAddress(username));
    return c.future.timeout(const Duration(seconds: 10), onTimeout: () {
      final list = _addressWaiters[username];
      list?.remove(c);
      if (list != null && list.isEmpty) _addressWaiters.remove(username);
      throw TimeoutException('no address for $username');
    });
  }

  // --- search --------------------------------------------------------------

  /// How long a search stays open collecting peer answers.
  static const Duration searchWindow = Duration(seconds: 25);

  @override
  Stream<List<ExternalTrack>> search(String query) {
    final cleaned = _cleanQuery(query);
    late final StreamController<List<ExternalTrack>> ctl;
    _ActiveSearch? active;
    Timer? timer;

    Future<void> start() async {
      try {
        await _requireConnection();
      } catch (e) {
        if (!ctl.isClosed) {
          ctl.addError(StateError('Soulseek: ${_lastError ?? e}'));
          await ctl.close();
        }
        return;
      }
      if (cleaned.isEmpty) {
        if (!ctl.isClosed) await ctl.close();
        return;
      }
      final token = _nextToken();
      final search = _ActiveSearch(token, ctl, _toTracks, _log);
      active = search;
      _searches[token] = search;
      _serverSend(ServerCode.fileSearch, SlskOut.fileSearch(token, cleaned));
      timer = Timer(searchWindow, () {
        _searches.remove(token);
        search.close();
      });
    }

    ctl = StreamController<List<ExternalTrack>>(
      onListen: () => unawaited(start()),
      onCancel: () {
        timer?.cancel();
        final a = active;
        if (a != null) {
          _searches.remove(a.token);
          a.close();
        }
      },
    );
    return ctl.stream;
  }

  /// Soulseek treats a bare '-' as an exclusion marker; drop stray ones and
  /// collapse whitespace, as Nicotine+ does.
  static String _cleanQuery(String q) =>
      q.split(RegExp(r'\s+')).where((t) => t.isNotEmpty && t != '-').join(' ');

  List<ExternalTrack> _toTracks(FileSearchResponse resp) {
    final out = <ExternalTrack>[];
    final byFolder = <String, List<SlskFile>>{};

    for (final f in resp.files) {
      out.add(_fileTrack(resp.username, f));
      (byFolder[f.folder] ??= []).add(f);
    }

    // Soulseek shares whole directories, so offer the folder as a bulk grab
    // whenever one peer returned more than one file from it.
    byFolder.forEach((folder, files) {
      if (folder.isEmpty || files.length < 2) return;
      out.add(ExternalTrack(
        networkId: id,
        id: _encodeId(resp.username, folder, isFolder: true),
        title: _lastSegment(folder),
        artist: _artistFromPath(folder),
        album: _lastSegment(folder),
        owner: resp.username,
        remotePath: folder,
        isFolder: true,
        childCount: files.length,
      ));
    });

    return out;
  }

  ExternalTrack _fileTrack(String username, SlskFile f) {
    final folder = f.folder;
    return ExternalTrack(
      networkId: id,
      id: _encodeId(username, f.path, size: f.size),
      title: _stripExtension(f.fileName),
      artist: _artistFromPath(folder),
      album: folder.isEmpty ? null : _lastSegment(folder),
      owner: username,
      remotePath: f.path,
      sizeBytes: f.size,
      bitrate: f.effectiveBitrate,
      durationSeconds: f.durationSeconds,
      extension: f.extension,
    );
  }

  // Soulseek gives us a share path and nothing else, so artist/album are
  // heuristics off the directory names: `.../Artist/Album/01 Track.mp3` or
  // `.../Artist - Album/01 Track.mp3`.
  static String? _artistFromPath(String folder) {
    if (folder.isEmpty) return null;
    final leaf = _lastSegment(folder);
    final dash = leaf.indexOf(' - ');
    if (dash > 0) return leaf.substring(0, dash).trim();
    final parts = folder.split('\\').where((s) => s.isNotEmpty).toList();
    if (parts.length >= 2) return parts[parts.length - 2];
    return null;
  }

  static String _lastSegment(String path) {
    final parts = path.split('\\').where((s) => s.isNotEmpty).toList();
    return parts.isEmpty ? path : parts.last;
  }

  static String _stripExtension(String name) {
    final i = name.lastIndexOf('.');
    return (i <= 0) ? name : name.substring(0, i);
  }

  static String _encodeId(String username, String path,
          {int? size, bool isFolder = false}) =>
      jsonEncode({
        'u': username,
        'p': path,
        if (size != null) 's': size,
        if (isFolder) 'd': 1,
      });

  static _SlskRef _decodeId(String raw) {
    final m = jsonDecode(raw) as Map<String, dynamic>;
    return _SlskRef(
      username: m['u'] as String,
      path: m['p'] as String,
      size: (m['s'] as num?)?.toInt(),
      isFolder: m['d'] == 1,
    );
  }

  // --- folder listing ------------------------------------------------------

  @override
  Future<List<ExternalTrack>> listFolder(ExternalTrack folder) async {
    await _requireConnection();
    final ref = _decodeId(folder.id);
    final conn = await _peerFor(ref.username);

    final token = _nextToken();
    final waiter = Completer<FolderContentsResponse>();
    _folderWaiters[token] = waiter;
    conn.send(PeerCode.folderContentsRequest,
        SlskOut.folderContentsRequest(token, ref.path));

    final FolderContentsResponse resp;
    try {
      resp = await waiter.future.timeout(const Duration(seconds: 30));
    } on TimeoutException {
      _folderWaiters.remove(token);
      throw StateError(
          '${ref.username} did not answer the folder listing in time');
    }

    // The response may carry subfolders too; flatten them, sorted by path.
    final files = <SlskFile>[];
    for (final k in resp.folders.keys.toList()..sort()) {
      files.addAll(resp.folders[k]!);
    }
    files.sort((a, b) => a.path.compareTo(b.path));
    return [for (final f in files) _fileTrack(ref.username, f)];
  }

  // --- download ------------------------------------------------------------

  @override
  Stream<DownloadProgress> download(ExternalTrack track, String destDir) {
    late final StreamController<DownloadProgress> ctl;
    SlskDownload? dl;
    var cancelled = false;

    Future<void> start() async {
      _SlskRef ref;
      try {
        ref = _decodeId(track.id);
      } catch (_) {
        ctl.add(DownloadProgress(
            trackId: track.id,
            receivedBytes: 0,
            error: 'Malformed Soulseek track id'));
        await ctl.close();
        return;
      }

      if (ref.isFolder) {
        ctl.add(DownloadProgress(
            trackId: track.id,
            receivedBytes: 0,
            error: 'Expand the folder first — download() takes single files'));
        await ctl.close();
        return;
      }

      try {
        await _requireConnection();
        if (cancelled) return;
        final conn = await _peerFor(ref.username);
        if (cancelled) return;

        final d = SlskDownload(
          trackId: track.id,
          username: ref.username,
          virtualPath: ref.path,
          expectedSize: ref.size ?? track.sizeBytes,
          destDir: destDir,
          controller: ctl,
          log: _log,
          onFinished: _releaseDownload,
        );
        dl = d;
        _queued[_queueKey(ref.username, ref.path)] = d;

        try {
          conn.send(PeerCode.queueUpload, SlskOut.queueUpload(ref.path));
          // Nudge for a queue position so the log shows progress while
          // waiting.
          conn.send(PeerCode.placeInQueueRequest,
              SlskOut.placeInQueueRequest(ref.path));
        } catch (e) {
          // The peer connection died between _peerFor and the send. fail()
          // (not a bare ctl.add) so the _queued entry is released too.
          d.fail('Could not ask ${ref.username} for the file: $e');
          return;
        }

        ctl.add(DownloadProgress(trackId: track.id, receivedBytes: 0));
      } catch (e) {
        ctl.add(
            DownloadProgress(trackId: track.id, receivedBytes: 0, error: '$e'));
        await ctl.close();
      }
    }

    ctl = StreamController<DownloadProgress>(
      onListen: () => unawaited(start()),
      onCancel: () async {
        // start() checks this after each await, so a subscription that is
        // cancelled while we are still dialling never queues the upload at
        // all — otherwise the entry would sit in _queued forever and the
        // peer would hold a slot for a download nobody wants.
        cancelled = true;
        final d = dl;
        if (d != null) await d.abort();
      },
    );
    return ctl.stream;
  }

  void _releaseDownload(SlskDownload d) {
    _queued.remove(_queueKey(d.username, d.virtualPath));
    final t = d.token;
    if (t != null) _active.remove(t);
  }

  // --- serving our one shared file ----------------------------------------
  //
  // The upload flow, mirroring Nicotine+ (which is also what SoulseekQt
  // speaks in practice):
  //
  //   peer -> us   QueueUpload(path)            (or a legacy direction-0
  //                                              TransferRequest)
  //   us  -> peer  TransferRequest(direction=upload, OUR token, path, size)
  //   peer -> us   TransferResponse(token, allowed)
  //   us  -> peer  'F' connection: unframed uint32 token
  //   peer -> us   unframed uint64 resume offset
  //   us  -> peer  the file bytes from that offset, then close
  //
  // The downloader may also open the 'F' connection itself (directly to
  // our listener, or by asking the server to have us dial them) — both
  // arrive through _routeFileSocket, which recognises the peer as a
  // pending upload target and serves the same sequence on that socket.

  /// Step one: offer the file by sending our own TransferRequest.
  Future<void> _offerSharedFile(String username) async {
    // One offer per peer at a time. These networks serve one transfer per
    // peer, and a duplicate token pair would race for the same 'F' socket.
    if (_uploads.values.any((u) => u.username == username)) return;

    final Uint8List bytes;
    try {
      bytes = await SlskShare.instance.readBytes();
    } catch (e) {
      _log('shared file unavailable, not offering to $username: $e');
      return;
    }

    final token = _nextToken();
    final up = _SlskUpload(username, token, bytes);
    _uploads[token] = up;
    // If the downloader never answers (or answers and never connects),
    // forget the offer so the map cannot grow without bound.
    up.expiry = Timer(const Duration(minutes: 2), () {
      if (identical(_uploads[token], up) && !up.started) {
        _uploads.remove(token);
        _log('shared-file offer to $username expired unanswered');
      }
    });

    try {
      final conn = await _peerFor(username);
      conn.send(
          PeerCode.transferRequest,
          TransferRequestMessage.build(TransferRequestMessage(
            direction: TransferDirection.upload,
            token: token,
            virtualPath: kSlskSharePath,
            fileSize: bytes.length,
          )));
      _log('offered shared file to $username (token $token, '
          '${bytes.length} bytes)');
    } catch (e) {
      up.expiry?.cancel();
      _uploads.remove(token);
      _log('could not offer shared file to $username: $e');
    }
  }

  /// Step two, when the downloader said yes: open the 'F' connection
  /// ourselves (the uploader initiates) and push the bytes.
  Future<void> _pushSharedFile(_SlskUpload up) async {
    Socket? socket;
    Stream<Uint8List>? data;
    try {
      // Direct dial first.
      try {
        final addr = await _lookupAddress(up.username);
        if (addr.isRoutable) {
          final sck = await Socket.connect(addr.ipAddress, addr.port,
              timeout: const Duration(seconds: 8));
          sck.setOption(SocketOption.tcpNoDelay, true);
          sck.add(framePeerInit(PeerInitCode.peerInit,
              SlskOut.peerInit(_username ?? '', ConnType.file)));
          socket = sck;
          data = sck;
        }
      } catch (e) {
        _log('direct F dial to ${up.username} failed: $e');
      }

      // Otherwise ask the server to have them dial us; the pierce arrives
      // in _onIncomingPeer and completes this waiter.
      if (socket == null) {
        final t = _nextToken();
        final completer = Completer<IncomingPeerHandshake>();
        _pendingIndirect[t] = completer;
        _serverSend(ServerCode.connectToPeer,
            SlskOut.connectToPeer(t, up.username, ConnType.file));
        try {
          final h = await completer.future.timeout(const Duration(seconds: 20));
          socket = h.socket;
          data = h.data;
        } finally {
          _pendingIndirect.remove(t);
        }
      }

      await _serveUploadOnSocket(socket, data!, up);
    } catch (e) {
      _log('shared-file upload to ${up.username} failed: $e');
      try {
        socket?.destroy();
      } catch (_) {}
    } finally {
      up.expiry?.cancel();
      _uploads.remove(up.token);
    }
  }

  /// The 'F'-connection sequence itself, whichever side opened the socket:
  /// token out, offset in, bytes out, close.
  Future<void> _serveUploadOnSocket(
      Socket socket, Stream<Uint8List> data, _SlskUpload up) async {
    up.started = true;
    final buf = SlskFrameBuffer();
    final offsetC = Completer<int>();
    final sub = data.listen(
      (chunk) {
        if (offsetC.isCompleted) return;
        buf.add(chunk);
        final raw = buf.takeRaw(8);
        if (raw != null) {
          offsetC
              .complete(ByteData.sublistView(raw).getUint64(0, Endian.little));
        }
      },
      onError: (Object e, StackTrace _) {
        if (!offsetC.isCompleted) offsetC.completeError(e);
      },
      onDone: () {
        if (!offsetC.isCompleted) {
          offsetC.completeError(
              StateError('peer closed before sending a resume offset'));
        }
      },
      cancelOnError: true,
    );

    try {
      socket.add(SlskOut.fileTransferInit(up.token));
      final offset = await offsetC.future.timeout(const Duration(seconds: 30));
      final start = offset < 0
          ? 0
          : (offset > up.bytes.length ? up.bytes.length : offset);
      socket.add(Uint8List.sublistView(up.bytes, start));
      await socket.flush().timeout(const Duration(seconds: 30));
      // Graceful FIN so the downloader sees a clean end-of-file, not a
      // reset that makes it distrust the bytes it already has.
      await socket.close().timeout(const Duration(seconds: 5));
      _log('served shared file to ${up.username} '
          '(${up.bytes.length - start} bytes from offset $start)');
    } finally {
      await sub.cancel();
      socket.destroy();
    }
  }

  /// An 'F' socket whose peer username we know: is it a downloader coming
  /// for OUR file, or an uploader delivering one of OUR downloads?
  void _routeFileSocket(String? username, Socket socket, Stream<Uint8List> data) {
    if (username != null) {
      _SlskUpload? up;
      for (final u in _uploads.values) {
        if (u.username == username && !u.started) {
          up = u;
          break;
        }
      }
      final downloadingFromThem =
          _queued.values.any((d) => d.username == username) ||
              _active.values.any((d) => d.username == username);
      if (up != null && !downloadingFromThem) {
        final chosen = up;
        unawaited(_serveUploadOnSocket(socket, data, chosen)
            .catchError((Object e) {
          _log('shared-file upload to $username failed: $e');
          try {
            socket.destroy();
          } catch (_) {}
        }).whenComplete(() {
          chosen.expiry?.cancel();
          _uploads.remove(chosen.token);
        }));
        return;
      }
    }
    _handleFileSocket(socket, data);
  }

  /// The uploader says it is ready to send us a file.
  void _onTransferRequest(
      String username, SlskPeerConnection conn, TransferRequestMessage req) {
    if (req.direction != TransferDirection.upload) {
      // A peer is pulling a file FROM us with the legacy direction-0
      // request. The only thing we share is the explanatory text file
      // (slsk_share.dart) — honour a request for it and refuse anything
      // else. Announcing a share and then refusing to serve it is worse
      // than sharing nothing: the peer sees a broken uploader and bans
      // us, which is exactly what we're trying to fix.
      //
      // Nicotine+ answers this legacy form with "Queued" and then drives
      // the transfer through its own upload-direction TransferRequest, so
      // that is what we do too — one code path serves both entry points.
      if (SlskShare.instance.isSharedPath(req.virtualPath)) {
        conn.send(
            PeerCode.transferResponse,
            SlskOut.transferResponse(req.token, false,
                reason: TransferRejectReason.queued));
        unawaited(_offerSharedFile(username));
      } else {
        conn.send(
            PeerCode.transferResponse,
            SlskOut.transferResponse(req.token, false,
                reason: TransferRejectReason.fileNotShared));
      }
      return;
    }

    var d = _queued[_queueKey(username, req.virtualPath)];
    if (d == null) {
      // Some clients normalise the path; if this user has exactly one
      // outstanding request, assume it is that one.
      final mine = _queued.values
          .where((x) => x.username == username && x.token == null)
          .toList();
      if (mine.length == 1) d = mine.first;
    }

    if (d == null || d.isFinished) {
      conn.send(
          PeerCode.transferResponse,
          SlskOut.transferResponse(req.token, false,
              reason: TransferRejectReason.cancelled));
      return;
    }

    if (req.fileSize != null && req.fileSize! > 0) {
      d.expectedSize = req.fileSize;
    }
    d.token = req.token;
    _active[req.token] = d;
    conn.send(
        PeerCode.transferResponse, SlskOut.transferResponse(req.token, true));
    _log('accepted transfer ${req.token} for ${req.virtualPath}');
  }

  /// An 'F' connection arrived (or we dialled one).
  void _handleFileSocket(Socket socket, Stream<Uint8List> data) {
    attachFileConnection(
      socket: socket,
      data: data,
      lookup: (token) => _active[token],
      log: _log,
    );
  }
}

/// One pending/active upload of the shared file to one peer.
class _SlskUpload {
  _SlskUpload(this.username, this.token, this.bytes);

  final String username;
  final int token;
  final Uint8List bytes;

  /// True once an 'F' connection is being (or has been) served — the
  /// point after which a second socket for the same peer must not grab
  /// this entry.
  bool started = false;

  /// Forgets an offer nobody ever acted on.
  Timer? expiry;
}

class _SlskRef {
  const _SlskRef({
    required this.username,
    required this.path,
    this.size,
    this.isFolder = false,
  });
  final String username;
  final String path;
  final int? size;
  final bool isFolder;
}

class _ActiveSearch {
  _ActiveSearch(this.token, this._ctl, this._map, this._log);

  final int token;
  final StreamController<List<ExternalTrack>> _ctl;
  final List<ExternalTrack> Function(FileSearchResponse) _map;
  final void Function(String) _log;

  /// Peers whose responses were withheld because they could not serve
  /// immediately (no free slot, or a non-empty queue), and how many
  /// files those responses carried. Logged at close so "0 results" and
  /// "N results, all from busy peers" are distinguishable in the log.
  int busyPeersDropped = 0;
  int busyFilesDropped = 0;
  int shownPeers = 0;

  void add(FileSearchResponse resp) {
    if (_ctl.isClosed) return;
    final tracks = _map(resp);
    if (tracks.isNotEmpty) {
      shownPeers++;
      _ctl.add(tracks);
    }
  }

  void dropBusy(FileSearchResponse resp) {
    busyPeersDropped++;
    busyFilesDropped += resp.files.length;
  }

  void close() {
    if (busyPeersDropped > 0 || shownPeers > 0) {
      _log('search $token: showed $shownPeers peer(s); dropped '
          '$busyPeersDropped busy peer(s) holding $busyFilesDropped '
          'file(s) (no free slot / queued)');
    }
    if (!_ctl.isClosed) _ctl.close();
  }
}
