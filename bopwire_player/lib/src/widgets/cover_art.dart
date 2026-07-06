// Deterministic cover art — no image assets. An integer-only algorithm
// turns 8 seed bytes into a two-hue gradient + geometric motif.
//
// Ported verbatim from coverArt() in the web player
// (bopwire/webplayer/frontend/app.js). Same seed bytes → same hues, same
// motif, same geometry on a 96×96 grid — identical hashes render identical
// art on both clients. KEEP THE TWO IMPLEMENTATIONS IN LOCKSTEP.
//
// Song seed:  first 8 bytes of the content hash.
// Name seed:  FNV-1a-32(name) ++ FNV-1a-32(name + '*'), big-endian bytes.

import 'dart:math' as math;

import 'package:flutter/material.dart';

List<int> seedFromHash(String hex) {
  final b = <int>[];
  for (var i = 0; i < 8; i++) {
    final chunk = (i * 2 + 2 <= hex.length) ? hex.substring(i * 2, i * 2 + 2) : '';
    b.add(int.tryParse(chunk, radix: 16) ?? 0);
  }
  return b;
}

int _fnv32(String s) {
  var h = 0x811c9dc5;
  for (final unit in s.codeUnits) {
    h ^= unit & 0xff;
    h = (h * 0x01000193) & 0xFFFFFFFF;
  }
  return h;
}

List<int> seedFromName(String name) {
  final n = name.trim().toLowerCase().replaceAll(RegExp(r'\s+'), ' ');
  final a = _fnv32(n), b = _fnv32('$n*');
  return [
    (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255,
    (b >> 24) & 255, (b >> 16) & 255, (b >> 8) & 255, b & 255,
  ];
}

class ArtParams {
  final int h1, h2, angle, motif, n, rot;
  final List<int> seed;
  const ArtParams(this.h1, this.h2, this.angle, this.motif, this.n, this.rot,
      this.seed);
}

ArtParams artParams(List<int> seed) {
  final h1    = ((seed[0] << 8) | seed[1]) % 360;
  final h2    = (h1 + 40 + (seed[2] % 200)) % 360;
  final angle = seed[3] % 360;
  final motif = seed[4] % 5;
  final n     = 3 + (seed[5] % 4);
  final rot   = (seed[6] % 4) * 90;
  return ArtParams(h1, h2, angle, motif, n, rot, seed);
}

Color hslColor(int h, int s, int l) =>
    HSLColor.fromAHSL(1, h.toDouble(), s / 100, l / 100).toColor();

/// The two-hue tile gradient for a facet name (genre tiles, playlist cards) —
/// matches tileGradient() in the web player.
LinearGradient tileGradient(List<int> seed) {
  final p = artParams(seed);
  return LinearGradient(
    begin: Alignment.topLeft,
    end: Alignment.bottomRight,
    colors: [hslColor(p.h1, 60, 30), hslColor(p.h2, 70, 42)],
  );
}

class CoverArt extends StatelessWidget {
  const CoverArt({super.key, required this.seed, required this.size});

  final List<int> seed;
  final double size;

  @override
  Widget build(BuildContext context) {
    return RepaintBoundary(
      child: CustomPaint(
        size: Size.square(size),
        painter: _CoverArtPainter(artParams(seed)),
      ),
    );
  }
}

class _CoverArtPainter extends CustomPainter {
  _CoverArtPainter(this.p);
  final ArtParams p;

  int _v(int i) => p.seed[(i + 2) % 8];

  @override
  void paint(Canvas canvas, Size size) {
    final k = size.width / 96; // the algorithm lives on a 96×96 grid
    final rect = Offset.zero & size;

    // background gradient, rotated about the center like the web version
    final bg = Paint()
      ..shader = LinearGradient(
        colors: [hslColor(p.h1, 45, 15), hslColor(p.h2, 60, 28)],
        transform: GradientRotation(p.angle * math.pi / 180),
      ).createShader(rect);
    canvas.drawRect(rect, bg);

    final a1 = hslColor(p.h1, 75, 62).withOpacity(.85);
    final a2 = hslColor(p.h2, 85, 60).withOpacity(.85);
    Color alt(int i) => i % 2 == 0 ? a1 : a2;

    canvas.save();
    canvas.translate(48 * k, 48 * k);
    canvas.rotate(p.rot * math.pi / 180);
    canvas.translate(-48 * k, -48 * k);

    if (p.motif == 0) {
      // concentric rings
      final step = 36 ~/ p.n;
      for (var i = 0; i < p.n; i++) {
        canvas.drawCircle(
          Offset(48 * k, 48 * k),
          (10 + i * step) * k,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 3 * k
            ..color = alt(i),
        );
      }
    } else if (p.motif == 1) {
      // bars
      final w = 64 ~/ p.n;
      for (var i = 0; i < p.n; i++) {
        final h = 18 + (_v(i) % 52);
        canvas.drawRRect(
          RRect.fromRectAndRadius(
            Rect.fromLTWH((16 + i * w) * k, (48 - (h >> 1)) * k,
                (w - 4) * k, h * k),
            Radius.circular(3 * k),
          ),
          Paint()..color = alt(i),
        );
      }
    } else if (p.motif == 2) {
      // nested diamonds — squares rotated 45° about the center
      final step = 26 ~/ p.n;
      canvas.save();
      canvas.translate(48 * k, 48 * k);
      canvas.rotate(math.pi / 4);
      canvas.translate(-48 * k, -48 * k);
      for (var i = 0; i < p.n; i++) {
        final half = 34 - i * step;
        canvas.drawRect(
          Rect.fromLTWH((48 - half) * k, (48 - half) * k,
              half * 2 * k, half * 2 * k),
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 3 * k
            ..color = alt(i),
        );
      }
      canvas.restore();
    } else if (p.motif == 3) {
      // dot grid
      final sp = 64 ~/ (p.n - 1);
      for (var i = 0; i < p.n; i++) {
        for (var j = 0; j < p.n; j++) {
          canvas.drawCircle(
            Offset((16 + i * sp) * k, (16 + j * sp) * k),
            (4 + (_v(i + j) % 3)) * k,
            Paint()..color = alt(i + j),
          );
        }
      }
    } else {
      // stacked triangles
      final step = 24 ~/ p.n;
      for (var i = 0; i < p.n; i++) {
        final s = 38 - i * step;
        final path = Path()
          ..moveTo(48 * k, (48 - s) * k)
          ..lineTo((48 + s) * k, (48 + s) * k)
          ..lineTo((48 - s) * k, (48 + s) * k)
          ..close();
        canvas.drawPath(
          path,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 3 * k
            ..color = alt(i),
        );
      }
    }

    canvas.restore();
  }

  @override
  bool shouldRepaint(_CoverArtPainter old) =>
      old.p.seed.toString() != p.seed.toString();
}
