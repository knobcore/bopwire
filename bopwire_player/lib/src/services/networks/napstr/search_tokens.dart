// Query tokenisation and local match verification, mirroring the reference
// client (src-tauri/src/lib.rs `search_tokens` / `search_matches` and
// network.rs `catalogue_search_tokens`).
//
// This has to match the reference closely for two reasons. Relay `#t`
// filters are OR filters and NIP-50 `search` support varies per relay, so
// PROTOCOL.md requires every returned entry to be re-checked locally; and
// the tag tokens we pick decide which server-side filters we send at all.

/// Words the reference client never emits as a search tag — common English
/// stop words plus the audio format words that would otherwise match
/// everything.
const Set<String> kCatalogueStopWords = {
  'a', 'an', 'and', 'are', 'as', 'at', 'audio', 'be', 'by', 'flac', 'for',
  'from', 'in', 'is', 'it', 'mp3', 'of', 'ogg', 'on', 'opus', 'or', 'the',
  'to', 'wav', 'with',
};

const int kCatalogueSearchTokenLimit = 20;
const int kCatalogueSearchTokenLength = 32;
const int kCatalogueQueryTokenLimit = 4;

final RegExp _nonAlphanumeric = RegExp(r'[^0-9A-Za-zÀ-ɏͰ-￿]+');

/// Splits on non-alphanumeric characters and lowercases. Empty tokens are
/// dropped.
List<String> searchTokens(String value) {
  if (value.isEmpty) return const [];
  final out = <String>[];
  for (final part in value.split(_nonAlphanumeric)) {
    if (part.isEmpty) continue;
    out.add(part.toLowerCase());
  }
  return out;
}

/// The `t` tags a catalogue entry would carry: tokens from each field
/// interleaved round-robin (so no single long field consumes the budget),
/// de-duplicated, stop words and `napstr` removed, capped at 20 tokens of
/// at most 32 characters.
List<String> catalogueSearchTokens(List<String> fields) {
  final perField = <List<String>>[
    for (final f in fields)
      searchTokens(f)
          .where((t) =>
              t != 'napstr' &&
              !kCatalogueStopWords.contains(t) &&
              t.length <= kCatalogueSearchTokenLength)
          .toList(),
  ];
  final seen = <String>{};
  final tokens = <String>[];
  final maxLen =
      perField.fold<int>(0, (m, f) => f.length > m ? f.length : m);
  for (var i = 0; i < maxLen; i++) {
    for (final field in perField) {
      if (i >= field.length) continue;
      if (seen.add(field[i])) {
        tokens.add(field[i]);
        if (tokens.length == kCatalogueSearchTokenLimit) return tokens;
      }
    }
  }
  return tokens;
}

/// Up to four query words to use as indexed `#t` relay filters, longest
/// first — a long word is far more selective than a short one, and the
/// relay `limit` is shared across the whole filter.
List<String> queryTagTokens(String query) {
  final tokens = catalogueSearchTokens([query]);
  tokens.sort((a, b) {
    final byLen = b.length.compareTo(a.length);
    return byLen != 0 ? byLen : a.compareTo(b);
  });
  return tokens.take(kCatalogueQueryTokenLimit).toList();
}

/// True when every token of [query] appears in [fields], either as a
/// substring of some field token or — for query tokens of five characters
/// or more — within edit distance 1 of one. An empty query matches.
bool searchMatches(String query, List<String> fields) {
  final queryTokens = searchTokens(query);
  if (queryTokens.isEmpty) return true;
  final fieldTokens = <String>[];
  for (final f in fields) {
    fieldTokens.addAll(searchTokens(f));
  }
  for (final q in queryTokens) {
    var hit = false;
    for (final f in fieldTokens) {
      if (f.contains(q) ||
          (q.length >= 5 && editDistanceAtMost(q, f, 1))) {
        hit = true;
        break;
      }
    }
    if (!hit) return false;
  }
  return true;
}

/// Bounded Levenshtein: true when the distance between [left] and [right]
/// is at most [limit]. Bails out early, so it is cheap for the common
/// "obviously different" case.
bool editDistanceAtMost(String left, String right, int limit) {
  final a = left.runes.toList();
  final b = right.runes.toList();
  if ((a.length - b.length).abs() > limit) return false;
  var previous = List<int>.generate(b.length + 1, (i) => i);
  for (var i = 0; i < a.length; i++) {
    final current = <int>[i + 1];
    for (var j = 0; j < b.length; j++) {
      final substitution = previous[j] + (a[i] != b[j] ? 1 : 0);
      final deletion = previous[j + 1] + 1;
      final insertion = current[j] + 1;
      var best = substitution;
      if (deletion < best) best = deletion;
      if (insertion < best) best = insertion;
      current.add(best);
    }
    var rowMin = limit + 1;
    for (final v in current) {
      if (v < rowMin) rowMin = v;
    }
    if (rowMin > limit) return false;
    previous = current;
  }
  return previous[b.length] <= limit;
}

/// Control and bidirectional-formatting characters that must never be
/// rendered from relay-supplied text (mirrors the reference client's
/// `is_unsafe_public_chat_character`).
bool isUnsafeTextRune(int rune) {
  if (rune < 0x20 || rune == 0x7f) return true;
  if (rune >= 0x80 && rune <= 0x9f) return true;
  if (rune == 0x061c || rune == 0xfeff) return true;
  if (rune >= 0x200b && rune <= 0x200f) return true;
  if (rune >= 0x202a && rune <= 0x202e) return true;
  if (rune >= 0x2060 && rune <= 0x206f) return true;
  return false;
}

/// napstr caps every displayed metadata field at 256 characters and rejects
/// unsafe runes outright rather than stripping them.
bool isValidCatalogueText(String value) {
  if (value.runes.length > 256) return false;
  for (final r in value.runes) {
    if (isUnsafeTextRune(r)) return false;
  }
  return true;
}
