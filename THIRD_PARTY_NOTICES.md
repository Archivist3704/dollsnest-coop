# Third-party notices

The mod's own code is **not open source**: all rights are reserved, with a short list of permissions in the
Licence section of the README. That is separate from the notices below, which apply to the vendored library
regardless.

This repository vendors exactly one third-party library:

| Component | Where | Licence |
|---|---|---|
| MinHook -- API hooking for x64/x86 | `Code/third_party/minhook` | BSD-2-Clause |

MinHook in turn bundles the Hacker Disassembler Engine 32/64 (HDE),
Copyright (c) 2008-2009, Vyacheslav Patkov, under the same permissive terms.

The complete licence text -- MinHook's and HDE's -- is in
[`Code/third_party/minhook/LICENSE.txt`](Code/third_party/minhook/LICENSE.txt).
That file is copied verbatim into every release zip as `THIRD_PARTY_NOTICES.txt`, because
BSD-2-Clause asks for the notice to travel with binary distributions.

Nothing here grants any rights to the game itself: its name, its assets and its code belong to their
owners.
