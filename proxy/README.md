# msw-gcn proxy

Speaks Minecraft 1.8.9 to the MegaSkywars server on one side and **GCLink** to a
GameCube on the other. Mojang auth, AES, zlib, varints and the 1.8 protocol
state machine all live here so that none of them has to live on a 486-era CPU
with 24 MB of RAM. The console sends *intent* and receives *state*.

The wire contract is [`../source/gclink.h`](../source/gclink.h) — that file is
the authority, and `gclink.js` is a transcription of it.

## Running it

```sh
cd proxy
npm install

# one-time: your account, kept out of git
cat > config.local.json <<'EOF'
{ "server": { "host": "your.server", "username": "you@example.com" } }
EOF

node index.js                       # connect and serve one console
node index.js --record game.gcr     # ...and capture the whole GCLink stream
node index.js --replay game.gcr     # replay a capture, no server, no account
node selftest.js                    # everything checkable without either
```

`--replay` is the one to reach for while working on the console: a DOL rebuild
is about ninety seconds, and replaying the same twenty seconds of a real game
into it — with no server, no account and no game in progress — is what makes
that loop bearable. `--loop` and `--speed` are there too.

Anything typed at a running proxy is said by the account, so getting it out of
the hub and onto a map is `/join hontori`. The console can compose the same
commands itself now (D-pad Up opens the palette), so this is a convenience
rather than the only way in. Only active when stdin is a terminal.

`--replay` also decodes what the *console* sends and prints a summary once a
second — the MOVE rate, the last position, the flag bits and the teleport
epoch. That is how the outbound path gets checked without a live server.

Point the console at it by setting `NET_PROXY_IP` in
[`../source/main.c`](../source/main.c); the proxy prints the addresses it is
reachable on at startup.

## What is here

| File | |
|---|---|
| `index.js` | wiring: the `minecraft-protocol` client, the packet routing, the 20 Hz flush, record/replay, reconnect |
| `gclink.js` | the wire contract, framing, the one-console-at-a-time server, capture |
| `world.js` | map identification, the join-time chunk diff, block deltas → `BLOCK_SET` |
| `mworld.js` | the `.mworld` decoder, ported from `tools/compress_worlds.py` |
| `mapdb.js` | which map is the player in, and where is its origin |
| `entities.js` | the entity stream, filtered by type and capped at 128 nearest |
| `chat.js` | 1.8 chat components → 95 printable glyphs and one colour byte |
| `state.js` | health, inventory, held slot, XP, game mode, teleports, outbound chat |
| `netplayer.js` | the console's intent → 1.8 packets: movement selection, the `C0B` edges, dig/place, attack ordering and the `C02` filter |
| `blockmap.js` | 1.8 state ids ↔ engine global ids |
| `stub.js` | a GCLink server that does nothing but the handshake — the known-good other end to bisect against when this one misbehaves |
| `selftest.js` | 668 checks, no server and no console required |

`blockmap.json` and `mapdb.json` are generated — do not edit them:

```sh
python tools/gen_blockmap.py     # after any change to data/blockids.txt
python tools/gen_mapdb.py        # after any change to g_maps[] or the .mworld files
```

`gen_mapdb.py` also asserts what can be checked offline about the scan-origin
assumption the whole delta approach rests on: every scan sits inside the 0..255
build height at its recorded origin, and no two maps overlap. It reports the
margin — currently the closest two origins are 1724 blocks apart and the
furthest any scan reaches from its own origin is 175.

## The link budget

The Broadband Adapter's lwIP has a small receive window and delayed ACKs, which
puts its practical ceiling somewhere around **10–15 KB/s**. Nothing tells you
when you cross it. Node hands a write straight to the kernel whenever the kernel
will take it, so `socket.writableLength` reads zero while the OS send buffer
quietly accumulates *seconds* of backlog — and because it is one TCP stream, that
backlog delays the block diff, the hit feedback and the keepalive equally. The
symptom is the console's RTT readout climbing to 400–800 ms during a fight while
every counter in the proxy looks healthy, because the number on the HUD *is* the
queue depth.

So the queue is measured rather than assumed. `gclink.congested` is true above
`congestedRttMs` (or above `sendHighWater` bytes held by node), and the rule for
what to do about it is one distinction:

- **State** — `ENTITY_MOVE`, `BLOCK_SET`. Every frame is superseded by the next
  one, so these hold off while congested. Skipping costs one stale tick.
- **Events** — adds, removals, equipment, animations, inventory, teleports.
  Nothing later carries them, so these always go. Dropping one desyncs the
  console for good.

That closes the loop: the RTT rises, the state senders back off, the queue
drains, they resume. Two consequences for anything added here — a new frame type
has to be classified into one of those two, and anything sent per-entity
per-tick needs a deadband, because at 20 Hz across 40 entities an 18-byte record
is 14 KB/s on its own.

`link N KB/s rtt N ms (peak N) queued N B ...` is logged every five seconds with
a per-type byte breakdown, busiest first. Five seconds because a fight is two
seconds long and a 30-second average hides it.

## The outbound path

`netplayer.js` is the riskiest file here, and the risk is not that it crashes.
Every item on the 1.8 conformance list, left out, produces either a kick or a
*silent drop* — the console goes on playing, the server ignores it, and nothing
anywhere says so:

| | |
|---|---|
| `C0B` sprint/sneak edges | server-side sprint is driven entirely by these. Sprint without them and you get "moved too quickly" **and** no sprint knockback. |
| the sprint-reset asymmetry | after an attack the server silently clears its own sprint flag; vanilla never re-sends `START_SPRINTING`, so neither does this. |
| `S08` → `C06` | echoes the **server's** coordinates. Until that lands, `hasMoved` stays false and every later packet is discarded. |
| the teleport epoch | `MOVE`s in flight across an `S08` describe a place the player no longer is; they are dropped. |
| packet selection | `C03`/`C04`/`C05`/`C06` by what actually changed, Y as the AABB's `minY`. |
| attack ordering | `C09` → `C0A` → `C02`: the server computes damage from *its* held item. |
| the `C02` filter | attacking an item or an arrow is a kick, not a no-op. |
| `C0F` auto-ack | miss one `S32` and the server ignores every later window click. |
| `C15` / `C17` on join | plugins read `player.getLocale()`; the brand is `Gamecube/1.0`, length-prefixed as vanilla writes it, and is what MegaSkywars announces the join from. |

`selftest.js` asserts every row of that table against fakes, so a regression
shows up in a second rather than as an account that quietly stops being able to
place blocks.
