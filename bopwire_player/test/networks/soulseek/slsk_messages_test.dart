// Message-level round-trips: every struct we build is parsed back, and the
// byte layout of the ones we only ever *send* is asserted field by field
// against what Nicotine+ writes.

import 'dart:convert';
import 'dart:io' show ZLibCodec;
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';

void main() {
  group('Login (server code 1)', () {
    test('field order is username, password, version, md5 hash, minor', () {
      final payload = SlskOut.login('alice', 'hunter2');
      final r = SlskReader(payload);
      expect(r.str(), 'alice');
      expect(r.str(), 'hunter2');
      expect(r.uint32(), kClientMajorVersion);
      expect(r.str(),
          crypto.md5.convert(utf8.encode('alicehunter2')).toString());
      expect(r.uint32(), kClientMinorVersion);
      expect(r.hasMore, isFalse);
    });

    test('the credential hash is md5(username + password) in lowercase hex',
        () {
      final r = SlskReader(SlskOut.login('u', 'p'));
      r.str();
      r.str();
      r.uint32();
      final hash = r.str();
      // md5("up")
      expect(hash, '46c48bec0d282018b9d167eef7711b2c');
      expect(hash, matches(RegExp(r'^[0-9a-f]{32}$')));
    });

    test('a successful login response parses', () {
      final w = SlskWriter()
        ..boolean(true)
        ..str('Welcome to Soulseek')
        ..raw([10, 1, 168, 192]) // 192.168.1.10, byte-reversed
        ..str('deadbeef')
        ..boolean(true);
      final resp = LoginResponse.parse(w.take());
      expect(resp.success, isTrue);
      expect(resp.greeting, 'Welcome to Soulseek');
      expect(resp.ipAddress, '192.168.1.10');
      expect(resp.isSupporter, isTrue);
    });

    test('a rejected login exposes the reason', () {
      final w = SlskWriter()
        ..boolean(false)
        ..str('INVALIDPASS');
      final resp = LoginResponse.parse(w.take());
      expect(resp.success, isFalse);
      expect(resp.failureReason, 'INVALIDPASS');
    });
  });

  group('server messages we send', () {
    test('SetWaitPort is a bare uint32', () {
      expect(SlskReader(SlskOut.setWaitPort(2234)).uint32(), 2234);
    });

    test('FileSearch is token then query', () {
      final r = SlskReader(SlskOut.fileSearch(99, 'aphex twin'));
      expect(r.uint32(), 99);
      expect(r.str(), 'aphex twin');
    });

    test('ConnectToPeer is token, username, conn type', () {
      final r = SlskReader(SlskOut.connectToPeer(7, 'bob', ConnType.peer));
      expect(r.uint32(), 7);
      expect(r.str(), 'bob');
      expect(r.str(), 'P');
    });

    test('PeerInit is our username, conn type, then a zero token', () {
      final r = SlskReader(SlskOut.peerInit('alice', ConnType.file));
      expect(r.str(), 'alice');
      expect(r.str(), 'F');
      expect(r.uint32(), 0);
      expect(r.hasMore, isFalse);
    });

    test('PierceFireWall is a bare token', () {
      expect(SlskReader(SlskOut.pierceFireWall(4242)).uint32(), 4242);
    });
  });

  group('ConnectToPeer (server code 18) as received', () {
    test('parses username, type, reversed IP, port, token, privileged', () {
      final w = SlskWriter()
        ..str('carol')
        ..str('F')
        ..raw([4, 3, 2, 1]) // 1.2.3.4
        ..uint32(2234)
        ..uint32(555)
        ..boolean(true)
        ..uint32(0) // obfuscation type
        ..uint32(0); // obfuscated port
      final req = ConnectToPeerRequest.parse(w.take());
      expect(req.username, 'carol');
      expect(req.connType, 'F');
      expect(req.ipAddress, '1.2.3.4');
      expect(req.port, 2234);
      expect(req.token, 555);
      expect(req.privileged, isTrue);
    });
  });

  group('GetPeerAddress (server code 3) as received', () {
    test('parses and flags unroutable peers', () {
      final ok = PeerAddress.parse((SlskWriter()
            ..str('dave')
            ..raw([4, 3, 2, 1])
            ..uint32(2234))
          .take());
      expect(ok.ipAddress, '1.2.3.4');
      expect(ok.isRoutable, isTrue);

      final offline = PeerAddress.parse((SlskWriter()
            ..str('dave')
            ..raw([0, 0, 0, 0])
            ..uint32(0))
          .take());
      expect(offline.isRoutable, isFalse);
    });
  });

  group('PeerInit (peer init code 1) as received', () {
    test('parses the dialling peer username and conn type', () {
      final init = PeerInitRequest.parse((SlskWriter()
            ..str('erin')
            ..str('P')
            ..uint32(0))
          .take());
      expect(init.username, 'erin');
      expect(init.connType, 'P');
    });
  });

  group('FileSearchResponse (peer code 9)', () {
    test('round-trips through zlib with lossy attributes', () {
      const original = FileSearchResponse(
        username: 'seeder',
        token: 31337,
        files: [
          SlskFile(
            path: r'@@abc\Music\Boards of Canada\Geogaddi\01 Ready Lets Go.mp3',
            size: 1234567,
            bitrate: 320,
            durationSeconds: 71,
            vbr: 0,
          ),
          SlskFile(
            path: r'@@abc\Music\Boards of Canada\Geogaddi\02 Music Is Math.mp3',
            size: 7654321,
            bitrate: 256,
            durationSeconds: 322,
            vbr: 1,
          ),
        ],
        freeUploadSlots: true,
        uploadSpeed: 512000,
        queueLength: 3,
      );

      final parsed =
          FileSearchResponse.parse(FileSearchResponse.build(original));

      expect(parsed.username, 'seeder');
      expect(parsed.token, 31337);
      expect(parsed.freeUploadSlots, isTrue);
      expect(parsed.uploadSpeed, 512000);
      expect(parsed.queueLength, 3);
      expect(parsed.files, hasLength(2));

      final first = parsed.files.first;
      expect(first.path, original.files.first.path);
      expect(first.size, 1234567);
      expect(first.bitrate, 320);
      expect(first.durationSeconds, 71);
      expect(first.vbr, 0);
      expect(first.fileName, '01 Ready Lets Go.mp3');
      expect(first.folder, r'@@abc\Music\Boards of Canada\Geogaddi');
      expect(first.extension, 'mp3');
      expect(parsed.files[1].vbr, 1);
    });

    test('round-trips lossless attributes (sample rate + bit depth)', () {
      const original = FileSearchResponse(
        username: 'flacfan',
        token: 1,
        files: [
          SlskFile(
            path: r'Share\Album\01.flac',
            size: 30000000,
            durationSeconds: 240,
            sampleRate: 44100,
            bitDepth: 16,
          ),
        ],
      );
      final f = FileSearchResponse.parse(FileSearchResponse.build(original))
          .files
          .single;
      expect(f.sampleRate, 44100);
      expect(f.bitDepth, 16);
      expect(f.durationSeconds, 240);
      expect(f.bitrate, isNull);
      // 44100 * 16 * 2 / 1000
      expect(f.effectiveBitrate, 1411);
      expect(f.extension, 'flac');
    });

    test('forward slashes in paths are normalised to backslashes', () {
      const original = FileSearchResponse(
        username: 'nixuser',
        token: 2,
        files: [SlskFile(path: 'home/music/album/track.ogg', size: 100)],
      );
      final f = FileSearchResponse.parse(FileSearchResponse.build(original))
          .files
          .single;
      expect(f.path, r'home\music\album\track.ogg');
      expect(f.folder, r'home\music\album');
      expect(f.fileName, 'track.ogg');
    });

    test('an empty result set round-trips', () {
      final parsed = FileSearchResponse.parse(FileSearchResponse.build(
          const FileSearchResponse(username: 'x', token: 3, files: [])));
      expect(parsed.files, isEmpty);
      expect(parsed.token, 3);
    });

    test('unknown file attributes are skipped without derailing the parse',
        () {
      // Hand-build one file entry carrying an attribute key we do not model.
      final w = SlskWriter()
        ..str('weird')
        ..uint32(5)
        ..uint32(1) // one file
        ..uint8(1)
        ..str(r'a\b.mp3')
        ..uint64(42)
        ..uint32(0) // obsolete ext
        ..uint32(2) // two attributes
        ..uint32(99) // unknown key
        ..uint32(12345)
        ..uint32(FileAttributeKey.bitrate)
        ..uint32(192)
        ..boolean(false)
        ..uint32(0)
        ..uint32(0)
        ..uint32(0);
      final compressed = Uint8List.fromList(ZLibCodec().encode(w.take()));
      final parsed = FileSearchResponse.parse(compressed);
      expect(parsed.files.single.bitrate, 192);
      expect(parsed.files.single.size, 42);
    });
  });

  group('FolderContentsResponse (peer code 37)', () {
    test('round-trips a folder with its files', () {
      const original = FolderContentsResponse(
        token: 77,
        folder: r'@@abc\Music\Album',
        folders: {
          r'@@abc\Music\Album': [
            SlskFile(path: r'@@abc\Music\Album\01.mp3', size: 10, bitrate: 320),
            SlskFile(path: r'@@abc\Music\Album\02.mp3', size: 20, bitrate: 320),
          ],
        },
      );
      final parsed =
          FolderContentsResponse.parse(FolderContentsResponse.build(original));
      expect(parsed.token, 77);
      expect(parsed.folder, r'@@abc\Music\Album');
      expect(parsed.folders.keys, [r'@@abc\Music\Album']);
      expect(parsed.folders.values.single, hasLength(2));
      expect(parsed.folders.values.single.last.path,
          r'@@abc\Music\Album\02.mp3');
    });

    test('a folder with no files round-trips', () {
      final parsed = FolderContentsResponse.parse(FolderContentsResponse.build(
          const FolderContentsResponse(
              token: 1, folder: r'x\y', folders: {})));
      expect(parsed.folders, isEmpty);
    });
  });

  group('FolderContentsRequest (peer code 36)', () {
    test('is token then folder path', () {
      final r = SlskReader(SlskOut.folderContentsRequest(5, r'a\b'));
      expect(r.uint32(), 5);
      expect(r.str(), r'a\b');
    });
  });

  group('transfers', () {
    test('TransferRequest round-trips in the upload direction with a size',
        () {
      final built = TransferRequestMessage.build(const TransferRequestMessage(
        direction: TransferDirection.upload,
        token: 4242,
        virtualPath: r'share\song.flac',
        fileSize: 44100000,
      ));
      final parsed = TransferRequestMessage.parse(built);
      expect(parsed.direction, TransferDirection.upload);
      expect(parsed.token, 4242);
      expect(parsed.virtualPath, r'share\song.flac');
      expect(parsed.fileSize, 44100000);
    });

    test('TransferRequest in the download direction carries no size', () {
      final built = TransferRequestMessage.build(const TransferRequestMessage(
        direction: TransferDirection.download,
        token: 1,
        virtualPath: 'x',
      ));
      final parsed = TransferRequestMessage.parse(built);
      expect(parsed.direction, TransferDirection.download);
      expect(parsed.fileSize, isNull);
    });

    test('an accepting TransferResponse is token + true', () {
      final bytes = SlskOut.transferResponse(9, true);
      final r = SlskReader(bytes);
      expect(r.uint32(), 9);
      expect(r.boolean(), isTrue);
      expect(r.hasMore, isFalse);

      final parsed = TransferResponseMessage.parse(bytes);
      expect(parsed.allowed, isTrue);
      expect(parsed.token, 9);
    });

    test('a rejecting TransferResponse carries the reason', () {
      final parsed = TransferResponseMessage.parse(SlskOut.transferResponse(
          9, false,
          reason: TransferRejectReason.queued));
      expect(parsed.allowed, isFalse);
      expect(parsed.reason, 'Queued');
    });

    test('an allowing TransferResponse with a size parses the size', () {
      final parsed = TransferResponseMessage.parse(
          SlskOut.transferResponse(3, true, fileSize: 123456789));
      expect(parsed.allowed, isTrue);
      expect(parsed.fileSize, 123456789);
    });

    test('QueueUpload is a bare virtual path', () {
      expect(SlskReader(SlskOut.queueUpload(r'a\b\c.mp3')).str(),
          r'a\b\c.mp3');
    });

    test('PlaceInQueueRequest is a bare virtual path', () {
      expect(SlskReader(SlskOut.placeInQueueRequest('x')).str(), 'x');
    });

    test('PlaceInQueueResponse round-trips', () {
      final parsed = PlaceInQueueResponse.parse(PlaceInQueueResponse.build(
          const PlaceInQueueResponse(r'a\b.mp3', 17)));
      expect(parsed.virtualPath, r'a\b.mp3');
      expect(parsed.place, 17);
    });

    test('UploadDenied exposes path and reason', () {
      final m = UploadDenied.parse((SlskWriter()
            ..str(r'a\b.mp3')
            ..str('Banned'))
          .take());
      expect(m.virtualPath, r'a\b.mp3');
      expect(m.reason, 'Banned');
    });

    test('UploadFailed exposes the path', () {
      expect(UploadFailed.parse((SlskWriter()..str('p')).take()).virtualPath,
          'p');
    });

    test('the F-connection prologue is an unframed token then an offset', () {
      expect(SlskOut.fileTransferInit(0x11223344), hasLength(4));
      expect(SlskReader(SlskOut.fileTransferInit(0x11223344)).uint32(),
          0x11223344);
      expect(SlskOut.fileOffset(0), hasLength(8));
      expect(SlskReader(SlskOut.fileOffset(3000000000)).uint64(),
          3000000000);
    });
  });
}
