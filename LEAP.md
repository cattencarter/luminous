# Driving Luminous from a program (LEAP)

Luminous can be driven by a program running beside it on your Mac: one you
write, or one an AI assistant runs for you. It sets the sky and the sun,
applies styles and presets, frames you, changes any setting, and renders
pictures to files. This is done through **LEAP**, the LLSD Event API Plugin
protocol that Linden Lab's viewer and Firestorm carry; Luminous adds a few
APIs of its own to it.

Nothing in Luminous listens for it. A LEAP program is started *by the
viewer*, as its child, and the two talk over the program's own input and
output. Started without one, Luminous runs no program and opens nothing to
the network. There is no assistant built into Luminous.

> **Only run programs you trust.** A program started this way can do
> anything you can do in the viewer: change settings, move your avatar,
> send chat, spend L$.

## Starting Luminous with a program

Quit Luminous, then start it from Terminal with `--leap` and the command
line that runs your program:

```sh
open -n -a Luminous --args --leap "'/path/to/venv/bin/python3 /path/to/myscript.py'"
```

The command line is read as an LLSD string, so it has to be quoted twice:
in single quotes inside the shell's double quotes, as above. A path with a
space or a quote in it will not work; keep the program and its Python in a
plain path. The program runs until the viewer quits; the viewer then closes
its input. Anything the program writes to its standard error goes into the
viewer's log (`~/Library/Application Support/Luminous/logs/Luminous.log`).

`--leap` can be given more than once, to start several programs.

## The protocol

The viewer and the program exchange LLSD messages, each written as its
length in bytes, a colon, and the message:

```
<length>:<message>
```

- The viewer writes to the program's **standard input**, in binary LLSD.
- The program writes to its **standard output**, in **notation** LLSD. Its
  standard output belongs to the protocol: print anything else to standard
  error.
- Every message is a map: `{"pump": <name>, "data": <map>}`. To ask an API
  for something, send to its pump a map with `"op"` naming the operation,
  and its arguments.
- The viewer's **first** message is a handshake. Its `"pump"` is the pump on
  which your program receives replies, and its `data["command"]` the LEAP
  command pump (below).
- To get an answer, add `"reply": <the reply pump>` and a `"reqid"` of your
  own to the request; the answer comes on the reply pump with the same
  `"reqid"`. Requests without a reply are fire-and-forget.

Linden Lab's `llsd` Python package reads and writes LLSD
(`pip install llsd`, in a virtual environment).

### A complete example

```python
#!/usr/bin/env python3
"""Started by Luminous with --leap: reads the handshake, asks for a
setting, sets the picture's shape to a square, and saves a render."""
import itertools
import sys

import llsd  # pip install llsd

stdin, stdout = sys.stdin.buffer, sys.stdout.buffer


def read():
    length = b""
    while (c := stdin.read(1)) != b":":
        if not c:
            raise EOFError  # the viewer quit
        length += c
    remaining, data = int(length), b""
    while remaining:
        chunk = stdin.read(remaining)
        data += chunk
        remaining -= len(chunk)
    return llsd.parse(data)


def send(pump, data):
    body = llsd.format_notation({"pump": pump, "data": data})
    stdout.write(b"%d:" % len(body) + body)
    stdout.flush()


handshake = read()
reply_pump = handshake["pump"]
reqids = itertools.count(1)


def request(pump, data):
    """Send and wait for the answer to this request."""
    reqid = next(reqids)
    send(pump, dict(data, reply=reply_pump, reqid=reqid))
    while True:
        message = read()
        answer = message.get("data")
        if isinstance(answer, dict) and answer.get("reqid") == reqid:
            return answer


shape = request("LLViewerControl", {"op": "get", "group": "Global", "key": "LuminousSnapshotAspect"})
print("shape was", shape.get("value"), file=sys.stderr)

send("LLViewerControl", {"op": "set", "group": "Global", "key": "LuminousSnapshotAspect", "value": 1})
send("UI", {"op": "call", "function": "Luminous.SaveSnapshot",
            "parameter": "/Users/you/Pictures/square.png"})
```

A program that should stay connected keeps reading from standard input;
when the viewer quits, `read()` raises `EOFError`.

### Asking the viewer what it offers

Send `{"op": "getAPIs"}` to the command pump from the handshake, with a
`reply` and `reqid`: the answer lists every API with its description.
`{"op": "getAPI", "api": "<name>"}` describes one API and each of its
operations and their arguments.

## Luminous's own

### Calls (pump `UI`, op `call`)

`{"op": "call", "function": "<name>", "parameter": <value>}` runs one of the
viewer's commands, as its buttons and menus do. Luminous's:

| Function | Parameter | Does |
|---|---|---|
| `Luminous.ApplyStyle` | a style's name, as in the Styles menu | Applies the style: sky, sun, framing and the four tabs' settings. |
| `Luminous.Frame` | `face`, `half`, `low`, `tilted`, `behind`, `sidelow` | Frames your avatar, as Frame me does. |
| `Luminous.ApplyPreset` | a preset's name | Loads a preset saved on the Save / Load tab. |
| `Luminous.Environment` | a sky's name from the Sky menu; `""` the region's | Shows that sky. |
| `Luminous.RegionSky` | -- | The region's own sky again. |
| `Luminous.SaveShot` / `Luminous.GoToShot` | a camera position's name | Saves the camera, or goes back to a saved one. |
| `Luminous.SaveSnapshot` | a file path | Renders with the Render tab's settings and saves to that path, in the chosen format, without showing the picture. |
| `Luminous.SaveHDRSnapshot` | a file path | The same as an OpenEXR file (light, before tonemapping). |
| `Luminous.SaveLargeSnapshot` | a file path | The same as a PNG. |
| `Luminous.PreviewImage` | -- | A quick preview render, shown over the viewer. |
| `Luminous.ResetSettings` | setting names, comma-separated | Puts those settings back to Luminous's defaults. |
| `Luminous.ImportLUT` | a `.cube` file's path | Adds a colour table and selects it. |

A style or a framing asks for the scene to be paused first: set
`LuminousScenePaused` to `true` (below) before applying one.

> **Check a function's name before calling it.** Calling a function the
> viewer does not have stops the viewer (Linden Lab's own behaviour).

### The environment (pump `LuminousEnvironment`)

| Op | Arguments | Does |
|---|---|---|
| `getSun` | `reply` | Answers with the showing sky's sun: `azimuth`, `elevation` in degrees. |
| `setSun` | `azimuth`, `elevation` | Keeps the showing sky as your own environment with its sun there, as Personal Lighting does. |
| `getSky` | `reply` | Answers with the showing sky's settings as `sky` (LLSD). |
| `setSky` | `sky`, optionally `azimuth`, `elevation` | Shows that sky as your own environment. |
| `getWater` | `reply` | Answers with the showing water's settings as `water`. |
| `setWater` | `water`, `reply` | Shows that water; a value out of range changes nothing and answers with `error`. |
| `reset` | -- | The region's or parcel's environment again. |

### Settings (pump `LLViewerControl`)

`{"op": "get", "group": "Global", "key": "<name>"}` answers with the
setting's `value`, `type` and `comment`; `{"op": "set", ..., "value": v}`
sets it; `{"op": "toggle", ...}` flips a true / false one. Every setting
Luminous adds starts with `Luminous`, and its comment says what it does;
**Advanced > Show Debug Settings** lists them all. A few:

| Setting | |
|---|---|
| `LuminousScenePaused` | Pause scene: everything that moves holds still. |
| `LuminousSnapshotAspect`, `LuminousSnapshotPortrait` | The picture's shape: 0 the window's, 1 square, 2 5:4, 3 4:3, 4 3:2, 5 16:9, 6 21:9; standing when Portrait is true. |
| `LuminousSnapshotScale` | 1 the window's size, 2 double. |
| `LuminousSnapshotFormat` | 0 PNG, 1 16-bit TIFF, 2 OpenEXR, 3 JPEG, 4 BMP. |
| `LuminousSnapshotFrames` | Frames averaged in a render. |
| `LuminousPhoto...` | The effects on the sidebar's tabs: each effect's amount, its finer settings, and `...RenderOnly` for whether it shows live or only in renders. |

## A few of Linden Lab's and Firestorm's APIs

These are the viewer's own; `getAPI` describes each fully.

| Pump | Useful ops |
|---|---|
| `LLFloaterReg` | `showInstance` (`name`, optionally `key`), `hideInstance`, `instanceVisible`, `clickButton` (`name`, `button`): open windows and press their buttons. The Luminous sidebar is `luminous`. |
| `LLWindow` | `getPaths` (every interface element's path), `getInfo` (`path`), `mouseDown` / `mouseUp` / `mouseMove` (`path`, or `x`, `y`), `keyDown` / `keyUp`, `pasteText`. |
| `UI` | `call` (above), `getValue` (`path`), `setSelectedByValue` (`path`, `value`). |
| `LLAgent` | `getID`, `setCameraParams` / `removeCameraParams`, `setFollowCamActive`: the camera. |
| `LLViewerWindow` | `saveSnapshot`: Second Life's own snapshot of the window, without Luminous's render. |
| `LLAppViewer` | `requestQuit`: quits as File > Quit does, logging out. |

## Good manners

- **Logging in** uses the account and password saved on the login screen.
  Never put a password in a program.
- **Linden Lab's rules apply** to anything a program does in Second Life,
  among them the rules for automated avatars if it acts while you are away.
- **Be gentle with the grid**: one teleport at a time, waiting for it to
  finish, and not many in a row; do not log in and out repeatedly.
- **Quit, do not kill**, a logged-in viewer (`LLAppViewer` `requestQuit`):
  a killed viewer leaves the avatar standing in the region for a while.

## With an AI assistant

An assistant that can run programs on your Mac -- one with a terminal or
MCP tools, for example -- can use all of the above: give it this page. A
simple way is a small LEAP program that relays commands to the viewer from
a socket only you can open, which the assistant then talks to; the
assistant never needs your password, and the viewer gains nothing that
listens to the network.
