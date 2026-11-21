# Dolls Nest Co-op Mod

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

A co-op PvE mod for **Dolls Nest**.
The mod is a native DLL plus a small loader and a list that is preferred when playing over a virtual LAN.

> **Status:** v685, tested on 2 and 3 machines (game 1.3.2). This repository is the source of that build.
>
> **Licence:** all rights reserved -- **commercial use is not permitted**. See [Licence](#license).

**[Download the latest release](../../releases/latest)**

---

## For players

### Install

1. Get `DollsNest-coop-v685.zip` from [Releases](../../releases/latest) and extract it into your Steam library:

   ```
   <Steam library>\steamapps\common\Dolls Nest\
   ```

   The archive already contains `DollsNest\Binaries\Win64\`, so extracting it there puts `version.dll`,
   `dollsnest-coop.dll` and `peers.txt` where the game looks for them.

2. Start the game from Steam as usual.

3. Check that `dsh-proxy-load.txt` appears in the game directory, and that **F7** opens the menu in game.

### Playing together

1. Both players install Radmin VPN (or a similar virtual LAN tool) and join the same network.
2. Put every player's virtual LAN address -- your own included -- into `peers.txt`, one per line.
3. The host, on the map, presses **F7** and hosts.
4. The client presses **F7** and joins.
5. Both players must be on the same map -- different maps cannot connect.

### Limits

- Same map only; changing maps ends the session.
- Supplying at a rest point, or dying, ends the session -- press **F7** and host or join again.
- The client is not told when the host ends the session, and has to leave the menu itself.
- Verified with 2 and 3 players; the 4th has never been tested.
- The host's boss-cleared state is written into the client's save, so that progress stays in step.
- Progress itself does not affect the connection, but prefer letting the player who is behind host.
  If both of you are at the same point it does not matter.
- The game guards story items and progress carefully, but if you are worried about your save, back it
  up before playing.

---

## For developers

### Build

Requirements: Visual Studio 2022/2026 build tools (MSVC 19.5x) and CMake 3.2x+.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build\build.ps1 -BootstrapName dollsnest-coop-v685
```

The output is `out\bin\dollsnest-coop-v685.dll` (plus a `.pdb`). A successful build prints `[build] OK`
and exits 0; a non-zero exit means the script refused to hand over a DLL -- a missing artifact, a name that
does not match what was asked for, or a DLL older than its sources. The `[build]` warning just above says
which.

An offline test suite builds alongside it:

```powershell
out\bin\dsh-selftest.exe
```

It must print `checks: 734 failures: 0`.

### Layout

| Path | What it is |
|---|---|
| `Code/dsh-bootstrap` | how the mod gets into the game: the loader, the command channel, the F7 menu, room discovery |
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

- `Code/third_party/minhook` is vendored (BSD-2-Clause) -- it is the only third-party dependency.
- `Code/third_party/layout` is not shipped: the offsets are compiled in and verified at runtime.
- Two machines must run the same build. The log's first screen prints a `file sha256=`; those have to
  match.

---

## Not affiliated

This is a fan-made mod, not endorsed by or affiliated with the game's developers or publishers.

## License

This repository is **not** open source. All rights are reserved by the author, and the list below is the
whole of what is permitted -- anything not listed is not permitted.

**You may, without asking:**

- read the source and learn from it;
- build it yourself and play with it, friends included;
- open issues and send pull requests;
- show it in videos, streams or articles, with a link back to this repository.

**You may not:**

- **use it commercially in any form** -- selling it, putting it behind a paywall or an ad-gated download,
  bundling it into a paid pack, or shipping it as part of a paid service or paid support;
- re-upload or redistribute it (a mirror, a repack, another mod site) without asking first;
- present it, or a modified copy of it, as your own work.

**Ask first** if you want to redistribute it, bundle it with something else, translate it or port it. For
non-commercial use the answer is usually yes -- I would just like to know where it ends up.

This is a statement of permission, not an open-source licence, and it grants nothing regarding the game
itself: the game, its name and its assets belong to their owners.

`Code/third_party/minhook` is a separate matter -- it is vendored under BSD-2-Clause; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).