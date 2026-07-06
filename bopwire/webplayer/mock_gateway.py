"""Mock Bopwire gateway: serves a frontend build + fake /api data for UI preview.

Usage: python mock_gateway.py [frontend_dir]
Defaults to the Vite build (webapp/dist); pass a path to serve e.g. the
legacy frontend/ instead.
"""
import hashlib, json, os, random, sys, urllib.parse
from http.server import HTTPServer, SimpleHTTPRequestHandler

_HERE = os.path.dirname(os.path.abspath(__file__))
FRONTEND = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_HERE, "webapp", "dist")

random.seed(7)
GENRES  = ["Indie Rock", "Jazz", "Synthwave", "Hip Hop", "Folk", "Techno", "Lo-fi"]
ARTISTS = ["Neon Harbor", "Moss & Marrow", "DJ Kelvin", "The Paper Suns", "Iva Lune",
           "Quiet Machines", "Red Static", "Marlowe Finch"]
ALBUMS  = ["Night Drives", "Field Notes", "Static Bloom", "Low Tide", "Glasshouse"]
WORDS   = ["Copper", "Midnight", "Satellite", "Harbor", "Echo", "Vellum", "Drift",
           "Summit", "Cinder", "Aurora", "Motorway", "Fern"]

def mk_song(i):
    h = hashlib.sha256(f"song-{i}".encode()).hexdigest()
    artist = ARTISTS[i % len(ARTISTS)]
    return {
        "contentHash": h,
        "title":  f"{random.choice(WORDS)} {random.choice(WORDS)}",
        "artist": artist,
        "album":  ALBUMS[i % len(ALBUMS)],
        "genre":  GENRES[i % len(GENRES)],
        "year":   2015 + (i % 10),
        "trackNumber": (i % 12) + 1,
        "durationMs": 120000 + (i * 7919) % 180000,
        "playCount":  [3, 42, 180, 950, 4200, 12000, 88000][i % 7],
        "swarmSize":  1 + i % 4,
        "available":  i % 6 != 5,   # every 6th song "offline"
    }

SONGS = [mk_song(i) for i in range(56)]

def collection(id_, kind, title, sub, facet, songs):
    return {"id": id_, "kind": kind, "title": title, "subtitle": sub,
            "facet": facet, "songs": songs}

def collections():
    rising = [s for s in SONGS if 0 < s["playCount"] < 10000]
    top    = sorted(SONGS, key=lambda s: -s["playCount"])[:50]
    new    = list(reversed(SONGS))[:50]
    cols = [
        collection("rising:", "rising", "Rising",
                   "Under 10k plays — every listen pays the artist in full", "", rising),
        collection("top:", "top", "Top 50", "The most played songs on the chain", "", top),
        collection("new:", "new", "New Releases", "Fresh on the chain", "", new),
    ]
    for g in GENRES:
        gs = [s for s in SONGS if s["genre"] == g]
        cols.append(collection(f"genre:{g.lower()}", "genre", f"Best of {g}",
                               f"{len(gs)} songs", g.lower(), gs))
    for y in (2024, 2023, 2019):
        ys = [s for s in SONGS if s["year"] == y]
        if ys:
            cols.append(collection(f"year:{y}", "year", f"{y} hits",
                                   f"The best of {y}", str(y), ys))
    return {"epoch": 41, "snapshotHeight": 14760,
            "snapshotBlockHash": "ab" * 32,
            "contentDigest": hashlib.sha256(b"mock").hexdigest(),
            "collections": cols}

def facets():
    art, gen, yrs = {}, {}, {}
    for s in SONGS:
        art[s["artist"]] = art.get(s["artist"], 0) + 1
        gen[s["genre"]]  = gen.get(s["genre"], 0) + 1
        yrs[s["year"]]   = yrs.get(s["year"], 0) + 1
    return {"total": len(SONGS),
            "artists": [{"name": k, "count": v} for k, v in sorted(art.items())],
            "genres":  [{"name": k, "count": v} for k, v in sorted(gen.items())],
            "years":   [{"year": k, "count": v} for k, v in sorted(yrs.items())]}

class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=FRONTEND, **kw)

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/collections":
            return self._json(collections())
        if u.path == "/api/facets":
            return self._json(facets())
        if u.path == "/api/health":
            return self._json({"ok": True, "node": "connected", "uptime_s": 99})
        if u.path == "/api/songs":
            hits = SONGS
            for key in ("artist", "genre", "album"):
                if key in q:
                    hits = [s for s in hits if s[key].lower() == q[key][0].lower()]
            if "q" in q:
                needle = q["q"][0].lower()
                hits = [s for s in hits if needle in s["title"].lower()
                        or needle in s["artist"].lower() or needle in s["album"].lower()
                        or needle in s["genre"].lower()]
            sort = q.get("sort", [""])[0]
            if sort == "plays": hits = sorted(hits, key=lambda s: -s["playCount"])
            if sort == "album": hits = sorted(hits, key=lambda s: (s["album"], s["trackNumber"]))
            off = int(q.get("offset", ["0"])[0]); lim = int(q.get("limit", ["100"])[0])
            if any(k in q for k in ("q","artist","genre","album","offset","limit","sort")):
                return self._json({"total": len(hits), "offset": off, "limit": lim,
                                   "songs": hits[off:off+lim]})
            return self._json(hits)
        if u.path.startswith("/api/stream/"):
            self.send_response(404); self.end_headers(); return
        if u.path.startswith("/api/play/"):
            return self._json({"playId": "mock"})
        return super().do_GET()

    def log_message(self, *a): pass

if __name__ == "__main__":
    print("mock gateway on http://localhost:8091/?gateway=http://localhost:8091")
    HTTPServer(("127.0.0.1", 8091), H).serve_forever()
