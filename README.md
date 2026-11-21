# Dolls Nest — co-op mod

A peer-to-peer co-op mod for **Dolls Nest**. It lets two (up to four) players see each other's
machines, shoot together, and share one world: the host's world is the world, and the clients' dolls
appear in it.

The mod is a native DLL plus a small loader. It does not modify any game file, does not need an
injector, and does not need administrator rights.

> **Status:** v685, played and verified on two and three machines (game 1.3.2). The source in this
> repository is the source of that build.

---

## For players

### Install

1. Unpack the release zip into your Steam library:

   ```
   <Steam library>\steamapps\common\Dolls Nest\
   ```

   The archive already contains the `DollsNest\Binaries\Win64\` path, so extracting it there puts
   `version.dll` and `dollsnest-coop.dll` where the game looks for them.
2. Start the game from Steam as usual. No injector, no launch options.
3. Check that the game directory now has `dsh-proxy-load.txt` with a recent timestamp, and that
   `dollsnest-coop.log` starts with `build=` and `file sha256=`.

### Play

1. Both players install a virtual LAN tool (Radmin VPN or similar) and join the same network.
2. In the game's `Win64` directory, create `peers.txt` with the other player's virtual-LAN address,
   one per line. Both sides list each other. Without the file the game falls back to LAN broadcast.
3. The host enters the map first, presses **F7**, and picks the first row.
4. The client presses **F7** within a few seconds and picks the second row.
5. Both players must be on the same map. Different maps do not see each other, by design.

The menu follows the game's language (Simplified Chinese, Traditional Chinese, Japanese, English).

### Known limits

- Same map only; changing maps ends the session.
- Resting at a supply point, or dying, ends the session — press F7 again afterwards.
- When the host ends the session, the client is not told; it has to leave the menu itself.
- Verified with 2 and 3 players; the 4th slot has never been tested.
- Use a copy of your save when playing as a client — the game's autosave writes into the save it
  loaded.

---

## For developers

### Build

Requirements: Visual Studio 2022/2026 build tools (MSVC 19.5x) and CMake 3.2x+.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build\build.ps1 -BootstrapName dollsnest-coop-v685
```

The output is `out\bin\dollsnest-coop-v685.dll` (plus a `.pdb`). The script exits with code 1 even on
success; the line to look for is `[build] OK`.

An offline test suite builds alongside it:

```powershell
out\bin\dsh-selftest.exe
```

It must print `checks: 734 failures: 0`.

### Layout

| Path | What it is |
|---|---|
| `Code/dsh-bootstrap` | how the DLL gets into the game: the loader, the command channel, the F7 menu, discovery |
| `Code/dsh-game` | the session: what is sent, what is built, what is mirrored |
| `Code/dsh-reflect` | reading and writing the game's actors (the only place that knows the game's field layout) |
| `Code/dsh-net` | UDP transport, the session handshake, the packet envelope |
| `Code/dsh-sync` | interpolation, the playback clock, the send cadence |
| `Code/dsh-platform` | logging, the hook wrapper, file hashing |
| `Code/dsh-selftest` | the offline suite: the parts that need no game |
| `Code/dsh-proxy` | the `version.dll` shim that makes the game load the mod by itself |
| `tools/build`, `tools/deploy` | build, deploy to a game directory, package a release |

### How it works, in one paragraph

The host samples its own machine's pose and appearance twenty times a second and sends them over a
small UDP protocol; a client assembles that appearance, builds the same machine in its own world, and
keeps it in step. Enemy health, mission state, projectiles and item use are mirrored the same way. The
host's world is authoritative; a client reports what happened on its side and the host decides.

### Notes

- `Code/third_party/minhook` is vendored (BSD-3-Clause) — it is the only third-party dependency.
- `Code/third_party/layout` is not shipped: the offsets are compiled in and verified at runtime.
- Two machines must run the same build. The log's first screen prints a `file sha256=`; those have to
  match.

---

## Not affiliated

This is a fan-made mod, not endorsed by or affiliated with the game's developers or publishers.
