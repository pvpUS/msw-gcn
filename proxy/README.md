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
the hub and onto a map is `/join hontori`. Without that there is no map, and
without a map the console has nothing to show — composing commands on the pad
is **T23**, two milestones away. Only active when stdin is a terminal.

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
| `blockmap.js` | 1.8 state ids ↔ engine global ids |
| `stub.js` | a GCLink server that does nothing but the handshake — the known-good other end to bisect against when this one misbehaves |
| `selftest.js` | 563 checks, no server and no console required |

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

## Not here yet

Movement, digging, placing and attacking are **T22**, and the plan is explicit
that landing half of that gets the account kicked within seconds with a failure
mode that is very hard to see from the console. `index.js` logs and drops those
messages. Three pieces of the 1.8 conformance table are implemented anyway,
because a proxy that cannot hold a session cannot be tested at all and none of
them is movement: the `C06` reply to `S08` (which echoes the *server's*
coordinates), the teleport epoch, and `C15`/`C17` on join.
