import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:media_kit/media_kit.dart';

import '../models/song.dart';
import '../models/session.dart';
import '../services/audio_stream_proxy.dart';
import '../services/metadata_lookup.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/heartbeat_service.dart';
import 'wallet_provider.dart';

enum PlayerState { idle, loading, playing, paused, stopped }

class PlayerProvider extends ChangeNotifier {
  final NodeClient       _client    = NodeClient();
  late final HeartbeatService _heartbeat;
  final Player           _player    = Player();

  // Per-stream reward lanes (PlayProof v2): seeder + relay peer-ids of the stream
  // serving the current song, captured once streaming resolves and reported to
  // the node at session.complete. Empty for a cached/local play (no peer served).
  String _seederAddr = '';
  String _relayAddr  = '';

  /// The live instance, for the handful of non-widget callers that must
  /// reach the ONE player without a BuildContext — currently the
  /// pre-download cache's preview hook, installed in main.dart. The app
  /// constructs exactly one PlayerProvider (see the provider tree), so
  /// this is a handle to that one, not a second player.
  static PlayerProvider? _current;
  static PlayerProvider get instance {
    final p = _current;
    if (p == null) {
      throw StateError('PlayerProvider has not been constructed yet');
    }
    return p;
  }

  PlayerProvider() {
    _current = this;
    // Share the same NodeClient so heartbeats use the same resolved URL
    _heartbeat = HeartbeatService(_client);
    _player.stream.playing.listen((playing) {
      // Ignore stream events while loading (open() races with play()), and
      // while the player is in a terminal state owned by us (stopped/idle).
      // Otherwise mpv emitting `playing: false` right after stop()/complete()
      // would flip our state back to `paused` and resurrect a session that
      // we just tore down.
      if (state == PlayerState.loading ||
          state == PlayerState.stopped ||
          state == PlayerState.idle) {
        return;
      }
      state = playing ? PlayerState.playing : PlayerState.paused;
      notifyListeners();
    });
    // The player's OWN duration. Chain tracks carry a duration from their
    // metadata, but a foreign-network preview has none — and without this
    // the transport bar had no scale, so the handle drifted to the end and
    // seek() clamped every request to 0.
    _player.stream.duration.listen((d) {
      final ms = d.inMilliseconds;
      if (ms > 0 && ms != playerDurationMs) {
        playerDurationMs = ms;
        notifyListeners();
      }
    });
    _player.stream.position.listen((pos) {
      positionMs = pos.inMilliseconds;
      notifyListeners();
    });
    _player.stream.completed.listen((completed) {
      if (completed) _onComplete();
    });
  }

  Song?         currentSong;
  PlaySession?  currentSession;
  PlayerState   state        = PlayerState.idle;
  int           positionMs   = 0;

  // ---- preview enrichment (web metadata + real cover) -----------------
  // Corrects a foreign-network preview's mangled search-result tags via
  // MetadataLookup (opt-in, debounced, rate-limited) and pulls the real
  // cover from Cover Art Archive as the track buffers. Entirely off the
  // playback path: results arrive via callbacks that are epoch-guarded in
  // PreviewEnricher, so a late reply from a scrubbed-past preview can
  // never land on whatever is playing now.
  final PreviewEnricher _previewEnricher = PreviewEnricher();

  /// Real cover for the CURRENT preview, once the enricher lands one.
  /// Null for chain tracks (those resolve art through AlbumArt/the node)
  /// and for previews with no cover found yet.
  Uint8List? previewArtBytes;

  /// Duration reported by the decoder for whatever is loaded, 0 until it
  /// arrives. Prefer [durationMs], which falls back to chain metadata.
  int           playerDurationMs = 0;

  /// Duration the UI should scale the seek bar to. The decoder's value
  /// wins when it has one: it is authoritative for previews (no chain
  /// metadata) and for files whose tags lie about length.
  int get durationMs =>
      playerDurationMs > 0 ? playerDurationMs : (currentSong?.durationMs ?? 0);

  /// True once we know how long the current item is. The bar should render
  /// indeterminate until then rather than pretending to be seekable.
  bool get hasDuration => durationMs > 0;

  /// 0..1 of the current item that has been downloaded, or null when the
  /// concept doesn't apply (chain streaming, local file). Set by whoever
  /// is feeding a progressive source — see main.dart's preview hook.
  double? bufferedFraction;
  void setBufferedFraction(double? f) {
    final prev = bufferedFraction;
    if (f == prev) return;
    // Only rebuild on a change the eye can see. This is fed per download
    // chunk; notifying on every one rebuilt the whole transport bar
    // hundreds of times a second and made the UI feel frozen.
    if (f != null && prev != null && (f - prev).abs() < 0.005) {
      bufferedFraction = f;   // keep the value current, skip the rebuild
      return;
    }
    bufferedFraction = f;
    notifyListeners();
  }
  String?       errorMessage;

  // Playlist / queue
  List<Song>    playlist     = [];
  int           _playlistIdx = -1;
  String        _playerAddr  = '';

  Future<NodeClient> _getClient() async {
    final pid = await NodeService.getRatsPeerId();
    if (pid.isEmpty) {
      throw Exception('No node discovered yet. Open Settings to refresh.');
    }
    _client.ratsPeerId = pid;
    return _client;
  }

  // ---- Public API ---------------------------------------------------

  Future<void> play(Song song, String playerAddress) async {
    playlist     = [song];
    _playlistIdx = 0;
    _playerAddr  = playerAddress;
    await _playSong(song, playerAddress);
  }

  /// Play a foreign-network preview (Soulseek / napstr) through the SAME
  /// player the library uses, so the transport bar drives it — seek,
  /// pause, stop and the position readout all work.
  ///
  /// This deliberately bypasses [_playSong]: that path opens a chain play
  /// SESSION (startSession + heartbeat + token spend), and a track being
  /// previewed off another network has no chain song to bind to and must
  /// not mint or spend anything. So: no session, no heartbeat, no
  /// contentHash — just audio.
  ///
  /// [url] is normally the pre-download cache's loopback URL, which keeps
  /// serving while the file is still arriving.
  Future<void> playPreview({
    required String url,
    required String title,
    String artist = '',
    String album  = '',
    int durationMs = 0,
  }) async {
    // Close any real session first, or the node keeps an orphaned one
    // open for a song we are no longer playing.
    final priorId = currentSession?.sessionId;
    if (priorId != null) {
      unawaited(_completeSessionSilently(priorId));
      currentSession = null;
    }
    _heartbeat.stop();
    await _player.stop();

    state        = PlayerState.loading;
    errorMessage = null;
    notifyListeners();

    // A synthetic Song purely so the transport bar has something to
    // render. The empty contentHash is the marker that this is not a
    // chain track — see [isPreview].
    final preview = Song(
      contentHash:     '',
      fingerprintHash: '',
      title:           title,
      artist:          artist,
      genre:           '',
      album:           album,
      year:            0,
      trackNumber:     0,
      durationMs:      durationMs,
      playCount:       0,
      swarmSize:       0,
    );

    playlist         = [preview];
    _playlistIdx     = 0;
    currentSong      = preview;
    playerDurationMs = 0;   // repopulated by the duration stream
    positionMs       = 0;
    bufferedFraction = 0;   // progressive source: the bar shows fill

    // A new preview abandons the previous one's lookup NOW — before any
    // await — so a reply already in flight can't land on this track.
    _previewEnricher.cancel();
    previewArtBytes = null;

    try {
      await _player.open(Media(url), play: true);
      state = PlayerState.playing;
    } catch (e) {
      errorMessage = 'Preview failed: $e';
      state = PlayerState.stopped;
      currentSong = null;
    }
    notifyListeners();

    // Kick off the web metadata + cover enrichment (opt-in via Settings,
    // debounced inside the enricher). Fire-and-forget: playback is
    // already running and must never wait on this.
    if (state == PlayerState.playing) {
      unawaited(_previewEnricher.start(
        title: title,
        artist: artist,
        album: album,
        durationMs: durationMs,
        onTags: (out) => _applyPreviewTags(preview, out),
        onArt: (bytes) {
          // Belt and braces on top of the enricher's epoch guard: only a
          // still-current preview may receive the cover.
          if (!isPreview) return;
          previewArtBytes = bytes;
          notifyListeners();
        },
      ));
    }
  }

  /// Swap the synthetic preview Song for one carrying the corrected
  /// title/artist/album. Display-only: contentHash stays '' (still a
  /// preview — see [isPreview]), nothing is written to the library or
  /// the chain.
  void _applyPreviewTags(Song original, MergeOutcome out) {
    final cur = currentSong;
    // Only apply to the exact preview this lookup was started for.
    if (cur == null || !isPreview || !identical(cur, original)) return;
    if (!out.changed) return;
    currentSong = Song(
      contentHash:     '',
      fingerprintHash: '',
      title:           out.title,
      artist:          out.artist,
      genre:           '',
      album:           out.album,
      year:            out.year,
      trackNumber:     out.trackNumber,
      durationMs:      cur.durationMs,
      playCount:       0,
      swarmSize:       0,
    );
    playlist     = [currentSong!];
    _playlistIdx = 0;
    notifyListeners();
  }

  /// True while the transport is driving a foreign-network preview rather
  /// than a chain track. Callers should not offer session-only actions
  /// (rate, tip, add-to-playlist) for these.
  bool get isPreview =>
      currentSong != null && currentSong!.contentHash.isEmpty;

  Future<void> playPlaylist(List<Song> songs, int startIndex, String playerAddress) async {
    playlist     = List.of(songs);
    _playlistIdx = startIndex.clamp(0, songs.length - 1);
    _playerAddr  = playerAddress;
    await _playSong(songs[_playlistIdx], playerAddress);
  }

  Future<void> playNext() async {
    if (playlist.isEmpty) return;
    _playlistIdx = (_playlistIdx + 1) % playlist.length;
    await _playSong(playlist[_playlistIdx], _playerAddr);
  }

  Future<void> playPrev() async {
    if (playlist.isEmpty) return;
    _playlistIdx = (_playlistIdx - 1 + playlist.length) % playlist.length;
    await _playSong(playlist[_playlistIdx], _playerAddr);
  }

  void pause() {
    _player.pause();
    state = PlayerState.paused;
    notifyListeners();
  }

  void resume() {
    _player.play();
    state = PlayerState.playing;
    notifyListeners();
  }

  void stop() {
    final finishedId = currentSession?.sessionId;
    if (finishedId != null) {
      // User-initiated stop counts as a session end too — the home
      // node still checks the 50% rule, so a short stop mid-song
      // won't fraudulently mint anything.
      unawaited(_completeSessionSilently(finishedId));
    }
    _player.stop();
    _heartbeat.stop();
    _previewEnricher.cancel();       // stop() ends any preview lookup too
    previewArtBytes  = null;
    bufferedFraction = null;
    state          = PlayerState.stopped;
    currentSong    = null;
    currentSession = null;
    positionMs     = 0;
    notifyListeners();
  }

  void togglePlayPause() {
    if (state == PlayerState.playing) {
      pause();
    } else if (state == PlayerState.paused) {
      resume();
    }
  }

  void updatePosition(int ms) {
    positionMs = ms;
    notifyListeners();
  }

  /// Seek the underlying media to [ms]. The heartbeat thread picks up
  /// the new position on its next tick (5 s cadence) — the full node
  /// uses that delta vs the wall-clock delta to decide whether to count
  /// the post-seek interval toward the session's effective listen time.
  Future<void> seek(int ms) async {
    // Clamp against the EFFECTIVE duration. Using currentSong.durationMs
    // meant a preview (duration 0) clamped every seek to 0, so the bar
    // appeared frozen while audio kept playing.
    final max = durationMs;
    var clamped = max > 0 ? ms.clamp(0, max) : (ms < 0 ? 0 : ms);

    // Progressive source (Soulseek / napstr preview): those protocols
    // have no resume-from-offset, so only the bytes already on disk can
    // be played. Seeking past the downloaded edge would stall on data
    // that will never arrive at that position, so land on the edge
    // instead. bopwire's own swarm DOES support offset resume and is not
    // a progressive source here, so it keeps full-range seeking.
    final buf = bufferedFraction;
    if (buf != null && max > 0) {
      final edge = (max * buf).floor();
      // A small margin back from the edge: the very last bytes are
      // usually a partial frame the decoder cannot start on.
      final safe = (edge - 750).clamp(0, max);
      if (clamped > safe) clamped = safe;
    }

    await _player.seek(Duration(milliseconds: clamped));

    // Resume if we were playing. After running to the end of the
    // then-available bytes mpv sits at EOF, and a bare seek() leaves it
    // parked there — the bar moves and no audio comes out, which is the
    // "silent from that downloaded piece" case.
    if (state == PlayerState.playing) {
      await _player.play();
    }

    positionMs = clamped;
    notifyListeners();
  }

  Future<void> seekRelative(int deltaMs) =>
      seek(positionMs + deltaMs);

  Future<MintResult?> complete() async {
    if (currentSession == null) return null;
    try {
      final result = await (await _getClient()).completeSession(
          currentSession!.sessionId,
          seederAddress: _seederAddr, miniNodeAddress: _relayAddr);
      _heartbeat.stop();
      currentSession = null;
      state          = PlayerState.idle;
      notifyListeners();
      WalletProvider.refreshNow();
      return result;
    } catch (_) {
      return null;
    }
  }

  /// Fire-and-forget session.complete used by the implicit-finish paths
  /// (track ended naturally, user skipped, user stopped). Swallows errors
  /// so a slow / unreachable full node doesn't block the UI from queueing
  /// the next track. The full node's 50% effective-listen check is what
  /// decides whether this turns into a MintTx, so spamming completes for
  /// short skips can't inflate play counts.
  Future<void> _completeSessionSilently(String sessionId) async {
    try {
      await (await _getClient()).completeSession(sessionId,
          seederAddress: _seederAddr, miniNodeAddress: _relayAddr);
      // Nudge the wallet to re-fetch — completeSession is the moment
      // the chain mints the discoverer + node + artist-escrow credits,
      // so the cached balance the wallet tab shows is stale RIGHT
      // NOW. Static refresh avoids hauling BuildContext through the
      // player stack.
      WalletProvider.refreshNow();
    } catch (_) {/* best effort */}
  }

  // ---- Internal -----------------------------------------------------

  Future<void> _playSong(Song song, String playerAddress) async {
    // Finalize whatever session was active before tearing down the
    // local player. Skip-next / skip-prev would otherwise leave the
    // previous song's session orphaned in the full node's map.
    final priorId = currentSession?.sessionId;
    if (priorId != null) {
      unawaited(_completeSessionSilently(priorId));
      currentSession = null;
    }
    await _player.stop();
    // A chain track replacing a preview abandons the preview's lookup —
    // a late enrichment reply must never restyle the chain track.
    _previewEnricher.cancel();
    previewArtBytes  = null;
    state            = PlayerState.loading;
    errorMessage     = null;
    playerDurationMs = 0;
    bufferedFraction = null;   // chain stream: not a progressive download
    notifyListeners();

    try {
      // Resolve node URL before starting session
      await _getClient();

      // Start play session
      final PlaySession session;
      try {
        session = await _client.startSession(song.contentHash, playerAddress);
      } catch (e) {
        final msg = e.toString();
        errorMessage = msg.contains('402')
            ? 'You need at least 1 token to play this song'
            : 'Failed to start session: $msg';
        state = PlayerState.stopped;
        notifyListeners();
        return;
      }

      currentSong    = song;
      currentSession = session;

      // librats path: rats_send_binary chunks are reassembled in a temp file,
      // then media_kit plays from disk. HTTP fallback returns the streaming
      // URL directly — only useful for nodes that aren't behind NAT.
      final mediaUri = await _client.fetchAudioToCache(song.contentHash);
      // Capture who served THIS song (seeder + relay) now that the stream is
      // resolved, to credit the per-stream reward lanes at session.complete.
      _seederAddr = AudioStreamProxy.instance.currentSeeder;
      _relayAddr  = AudioStreamProxy.instance.currentRelay;
      // `open` defaults to `play: true`, but pairing that with the
      // explicit `play()` below races libmpv on Android: the second play
      // call can land while the first is mid-startup, and mpv ends up
      // paused at offset 0 until the user scrubs the position. Opening
      // with `play: false` and then calling `play()` ourselves removes
      // the ambiguity — there's exactly one start signal.
      await _player.open(Media(mediaUri.toString()), play: false);
      await _player.play();

      // isPlaying gates each tick so heartbeats only fire while the user
      // is actually listening — pause / stop / load freezes the session's
      // position_ms from the server's perspective, which keeps the
      // union-of-timestamp-ranges play check from synthesizing listen
      // credit for a parked song.
      _heartbeat.start(
        session.sessionId,
        () => positionMs,
        isPlaying:      () => state == PlayerState.playing,
        contentHash:    song.contentHash,
        blockHash:      session.blockHash,
        playerAddress:  playerAddress,
        songDurationMs: song.durationMs,
      );

      state = PlayerState.playing;
      notifyListeners();
    } catch (e) {
      errorMessage = e.toString();
      state        = PlayerState.stopped;
      notifyListeners();
    }
  }

  void _onComplete() {
    _heartbeat.stop();
    // Tell the full node the session is finished so it can run the
    // 50% effective-listen check and mint outputs (escrow + node +
    // discoverer credits). Previously _onComplete only stopped the
    // local heartbeat timer — the session sat in the full node's
    // active-sessions map forever and no MintTx ever fired, which is
    // why a finished song never showed up in play_count even after
    // multiple full listens.
    final finishedId = currentSession?.sessionId;
    if (finishedId != null) {
      unawaited(_completeSessionSilently(finishedId));
    }
    currentSession = null;
    // Advance only when there's a real next track. `playlist.length > 1` is
    // the wrong gate: on a 5-track queue it stays true when the last track
    // finishes, so playNext() wraps via `% playlist.length` back to index 0
    // and the player loops the whole album forever instead of stopping.
    if (_playlistIdx < playlist.length - 1) {
      playNext();
    } else {
      state      = PlayerState.stopped;
      positionMs = 0;
      notifyListeners();
    }
  }
}
