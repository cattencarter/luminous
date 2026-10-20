# Driving Luminous from a program (LEAP)

Luminous can be driven by a program running beside it on your Mac: one you
write, or one an AI assistant runs for you. It sets the sky and the sun,
applies styles and presets, frames you, changes any setting, and renders
pictures to files. This is done through **LEAP**, the LLSD Event API Plugin
protocol that Second Life viewers carry. This page covers the whole viewer
over LEAP -- the camera, windows and settings, the sky, renders, your
avatar, chat, inventory -- with Luminous's own calls where they belong.

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
operations and their arguments. That is always right for the build you
run; the list at the end of this page is Luminous 0.8.4's.

### Three things to know first

- **A UI call of a function the viewer does not have stops the viewer**
  (Linden Lab's own behaviour). Check a name against this page, or the
  viewer's menus and buttons, before calling it.
- **Clicking**: `LLWindow` `mouseDown` / `mouseUp` with only a `path` aim at
  the element's centre and may not press it. Give `x` and `y` as well: the
  centre of `getInfo`'s `rect`, times two on a Retina display (`getInfo`
  answers in interface points, the mouse ops take window pixels).
- **Everything in the world is real.** Chat is heard, teleports and
  movement are seen, and Linden Lab's rules apply to what a program does
  (see *Good manners*).

## Windows and buttons

| Pump | Op | Does |
|---|---|---|
| `LLFloaterReg` | `showInstance` (`name`, optionally `key`), `hideInstance`, `toggleInstance`, `toggleInstanceOrBringToFront` | Opens or closes a window by its name. |
| `LLFloaterReg` | `instanceVisible` (`name`, `reply`) | Whether it is open. |
| `LLFloaterReg` | `clickButton` (`name`, `button`) | Presses a named button in an open window. |
| `LLFloaterReg` | `getBuildMap` (`reply`) | Every window's name. |
| `LLWindow` | `getPaths` (optionally `under`), `getInfo` (`path`) | The interface's elements by path, and one element's class, value, rect and whether it is visible and enabled. |
| `LLWindow` | `mouseDown` / `mouseUp` / `mouseMove` (`x`, `y`, `button`, optionally `path`, `mask`), `mouseScroll` (`clicks`) | The mouse. |
| `LLWindow` | `keyDown` / `keyUp` (`keycode`, `keysym` or `char`, optionally `path`, `mask`), `pasteText` (`text`, optionally `path`) | The keyboard. Escape is keycode 134, Return 129. |
| `UI` | `call` (`function`, `parameter`) | Runs a command as its menu item or button does. |
| `UI` | `getValue` (`path`), `setSelectedByValue` (`path`, `value`) | Reads a control, or chooses in a menu. |

**The Luminous sidebar** is the window `luminous`; its `key` opens a tab:
`light` Scene, `look` Tone, `colour` Colour, `lens` Effects, `snapshot`
Render, `styles` Portraits, `presets` Save / Load, `graphics` Settings.
Paths under it begin
`/main_view/menu_stack/world_panel/Floater View/luminous/luminous_box/luminous_tabs/`.
Two of the viewer's windows Luminous uses: `env_adjust_snapshot` (Personal
Lighting) and `phototools`.

## Settings

Every setting the viewer has can be read and changed (pump
`LLViewerControl`, group `Global` for the viewer's settings, `PerAccount`
for the logged-in account's):

| Op | Arguments | Does |
|---|---|---|
| `get` | `group`, `key`, `reply` | The setting's `value`, `type` and `comment`. |
| `set` | `group`, `key`, `value` | Sets it; without `value`, back to its default. |
| `toggle` | `group`, `key` | Flips a true / false one. |
| `vars` | `group`, `reply` | Every setting in the group with its value and comment. |
| `groups` | `reply` | The groups' names. |

**Advanced > Show Debug Settings** lists them too. Everything Luminous adds
starts with `Luminous`, and its comment says what it does. The sidebar's
effects are `LuminousPhoto...`: each effect's amount or switch, its finer
settings, and `...RenderOnly` for whether it shows live or only in renders.
Among them:

| Setting | |
|---|---|
| `LuminousScenePaused` | Pause scene: everything that moves holds still, the camera stays free. |
| `LuminousPhotoWorn`, `...WornStains`, `...WornPaper`, `...WornDust`, `...WornScratches`, `...WornEdges`, `...WornVariation` | Worn photo: an old, handled print. |
| `LuminousPhotoLightLeak`, `...LightLeakColour`, `...LightLeakPlace`, `...LightLeakSize`, `...LightLeakHardness`, `...LightLeakRainbow`, `...LightLeakVariation` | Light leak: light in from the frame's edge. |
| `LuminousPhotoGlitch`, `...GlitchSplit`, `...GlitchVHS`, `...GlitchWaves`, `...GlitchLines`, `...GlitchLineSize`, `...GlitchVariation` | Glitch: colour split, VHS, waves, scan lines. |

`Luminous.ResetSettings` (pump `UI`, op `call`) puts the settings it is
given, comma-separated, back to their defaults; `Luminous.ResetLook` puts
the tone, the colour grade and the film effects back. `Luminous.ImportLUT`
(a `.cube` file's path) adds a colour table and selects it.

## Sky, sun and water

Luminous's own pump `LuminousEnvironment` reads and sets the personal
environment, as Personal Lighting does; only you see it.

| Op | Arguments | Does |
|---|---|---|
| `getSun` | `reply` | The showing sky's sun: `azimuth` (0 east, 90 south) and `elevation`, in degrees. |
| `setSun` | `azimuth`, `elevation` | Keeps the showing sky as your own with its sun there. |
| `getSky` | `reply` | The showing sky's settings as `sky` (LLSD). |
| `setSky` | `sky`, optionally `azimuth`, `elevation` | Shows that sky as your own. |
| `getWater` | `reply` | The showing water's settings as `water`. |
| `setWater` | `water`, `reply` | Shows that water; a value out of range changes nothing and answers with `error`. |
| `reset` | -- | The region's or parcel's environment again. |

And on pump `UI`, op `call`:

| Function | Parameter | Does |
|---|---|---|
| `Luminous.Environment` | a sky's name from the Scene tab's Sky menu; `""` the region's | Shows that sky. |
| `Luminous.RegionSky` | -- | The region's own sky again. |

`LLPipeline` turns kinds of things off and on in the view
(`toggleRenderTypes` with `types`, e.g. `["water"]`; `hasRenderType`), as
the Advanced menu's Rendering Types do.

## Portraits: styles, framing and presets

A style sets the light, the sky, the sun, the camera's framing, the lens
and the colour grade in one; there are 15, on the Portraits tab. Styles
and framing work only with the scene paused, so set `LuminousScenePaused`
to `true` first. They work best under an open sky.

| Function (pump `UI`, op `call`) | Parameter | Does |
|---|---|---|
| `Luminous.ApplyStyle` | a style's name, as in the Portraits tab's Style menu | Applies it: frames you, shows its sky with the sun (and moon) placed from the camera, and sets the Scene, Tone, Colour and Effects tabs. |
| `Luminous.Frame` | `face`, `half`, `low`, `tilted`, `behind`, `sidelow` | Frames you, as Frame me does; a style's sun turns with the camera. |
| `Luminous.PortraitViewAngle` | a view angle in radians | Changes the lens while keeping you the same size in the frame. |
| `Luminous.PortraitViewAngleDefault` | -- | The Portraits tab's default view angle. |
| `Luminous.ApplyPreset` | a preset's name | Loads a preset saved on the Save / Load tab: the four tabs' settings and the sky. |

## The camera

| | Does |
|---|---|
| `LLAgent` `setCameraParams` (`camera_pos`, `focus_pos` in region metres, `camera_locked`, `focus_locked`, ...), then `setFollowCamActive` (`active` true) | Puts the camera there. |
| `LLAgent` `removeCameraParams`, `setFollowCamActive` (`active` false) | Gives the camera back. |
| `Luminous.SaveShot` / `Luminous.GoToShot` (pump `UI`, op `call`, a name) | Saves the camera with its lens and focus, or goes back to a saved one, as the Save / Load tab's camera positions. |
| `LLViewerControl` `CameraAngle` | The view angle, in radians. |

Esc (`LLWindow` `keyDown` keycode 134) gives a framed camera back.

## Pictures

On pump `UI`, op `call`, Luminous's renders: every effect at full quality,
the frames averaged, in the shape and size the Render tab is set to.

| Function | Parameter | Does |
|---|---|---|
| `Luminous.SaveSnapshot` | a file path | Renders and saves there in the Render tab's format, without showing the picture. Without a path, as the Render button. |
| `Luminous.SaveHDRSnapshot` | a file path | As an OpenEXR file: light, before tonemapping. |
| `Luminous.Save16BitSnapshot` | a file path | As a 16-bit TIFF. |
| `Luminous.SaveLargeSnapshot` | a file path | As a PNG. |
| `Luminous.PreviewImage` | -- | A quick preview render, shown over the viewer. |

| Setting | |
|---|---|
| `LuminousSnapshotAspect`, `LuminousSnapshotPortrait` | The shape: 0 the window's, 1 square, 2 5:4, 3 4:3, 4 3:2, 5 16:9, 6 21:9; standing when Portrait is true. |
| `LuminousSnapshotScale` | 1 the window's size, 2 double. |
| `LuminousSnapshotFormat` | 0 PNG, 1 16-bit TIFF, 2 OpenEXR, 3 JPEG, 4 BMP. |
| `LuminousSnapshotFrames` | Frames averaged in a render. |

`LLViewerWindow` `saveSnapshot` (`filename`, `width`, `height`, `showui`,
`showhud`, `rebuild`, `type` `"COLOR"` or `"DEPTH"`, `reply`) saves Second
Life's own snapshot of the view, without Luminous's render.

## Your avatar

| Pump | Op | Does |
|---|---|---|
| `LLAgent` | `getID`, `getPosition` (`reply`) | Your key; where you are, in region and global metres, and which way you face. |
| `LLAgent` | `requestSit` (`obj_uuid` or `position`), `requestStand` | Sits on a thing, or stands. |
| `LLAgent` | `requestTouch` (`obj_uuid` or `position`, optionally `face`) | Touches a thing, as a click on it does. |
| `LLAgent` | `lookAt` (`type`, `obj_uuid` or `position`), `resetAxes` | Where your avatar looks; facing straight again. |
| `LLAgent` | `playAnimation` (`item_id`, `inworld`), `stopAnimation`, `getAnimationInfo` | Plays an animation from your inventory, for you alone or for everyone. |
| `LLAgent` | `getNearbyAvatarsList`, `getNearbyObjectsList` (`dist`), `getAttachedObjectsList`, `getAgentScreenPos` | Who and what is near, what you wear, where an avatar is on screen. |
| `LLAgent` | `getGroups` (`reply`) | Your groups. |
| `LLAppearance` | `wearOutfit` (`folder_id` or `folder_name`, `append`), `wearItems`, `detachItems` (`items_id`), `getOutfitsList`, `getOutfitItems` | Outfits and what you wear. |
| `LLGesture` | `getActiveGestures`, `startGesture`, `stopGesture`, `isGesturePlaying` (`id`) | Gestures; there once you are logged in. |

## Moving and teleporting

| Pump | Op | Does |
|---|---|---|
| `LLAgent` | `requestTeleport` (`regionname`, `x`, `y`, `z`, optionally `skip_confirmation`) | Teleports, as a map or a location link does. |
| `LLTeleportHandler` | `teleport` (`regionname`, `x`, `y`, `z`; without `regionname`, global coordinates) | The same, lower down. |
| `LLAgent` | `startAutoPilot` (`target_global`, ...), `setAutoPilotTarget`, `stopAutoPilot`, `getAutoPilot` | Walks or flies you to a place. |
| `LLAgent` | `startFollowPilot` (`leader_id` or `avatar_name`) | Follows an avatar. |

> **One teleport at a time.** Wait for one to finish -- or fail -- before
> the next, keep a pause between them, and do not send many in a row: a
> teleport sent while another has not answered can be refused, and the
> grid may disconnect the viewer. Moving your avatar about while you are
> away makes it an automated avatar under Linden Lab's rules.

## Chat

| Pump | Op | Does |
|---|---|---|
| `GroupChat` | `startGroupChat`, `leaveGroupChat`, `sendGroupIM` (`group_id`, `message`) | Group chat. |

Nearby chat has no API of its own in Luminous.

> **Others read it as you.** Do not let a program chat, answer or post on
> its own without your knowing; never spam a channel or a group.

## Inventory

`LLInventory` reads your inventory: `getBasicFolderID` (a folder type's
name, e.g. `"Textures"`), `getDirectDescendants` (`folder_id`),
`collectDescendantsIf` (`folder_id`, and `name`, `desc`, `type` to filter
by), `getItemsInfo` (`item_ids`), `getFolderTypeNames`,
`getAssetTypeNames`. Luminous's environments are in `#Luminous/#Environments`.

## Dialogs and notices

`LLNotifications`: `listChannels`, `listChannelNotifications` (`channel`),
`respond` (`uuid`, `response`) to answer a dialog as its buttons do,
`cancel` (`uuid`), `requestAdd` (`name`, `substitutions`, `payload`) to show
one, `ignore`, and `forward` to have a channel's notices sent to your
program.

## The viewer itself

| Pump | Op | Does |
|---|---|---|
| `LLStats` | `getPerfData` (`reply`) | Frame times and other statistics. |
| `LLFloaterAbout` | `getInfo` (`reply`) | What the About window says: version, graphics, region. |
| `LLStartUp` | `getStateTable`, `postStartupState` | Where the login has got to. |
| `LLToolMgr` | `selectTool` (`tool_name`: Focus, Move, Edit, Create, Land), `openBuildFloater` | The build tools. |
| `LLURLDispatcher` | `dispatch` (`url`, `trusted`) | Opens a Second Life link (a `secondlife:///app/...` address) as a click on it would. |
| `LLCommandDispatcher` | `dispatch` (`cmd`, `params`, `query`), `enumerate` | The viewer's app commands, by name. |
| `LLAppViewer` | `requestQuit` | Quits as Quit does, logging out. |

**Logging in**: the login screen logs in the account it has selected when
Return is pressed in it (`LLWindow` `keyDown` keycode 129). Luminous's `Luminous.SetUpForThisMac` (pump `UI`, op `call`) chooses
settings for the Mac's graphics and display, as at the first start.

## Good manners

- **Logging in** uses the account and password saved on the login screen.
  Never put a password in a program, and check which account and grid the
  login screen has selected before pressing Return.
- **Linden Lab's rules apply** to anything a program does in Second Life,
  among them the rules for automated avatars if it acts while you are away.
- **Be gentle with the grid**: one teleport at a time, as above; do not log
  in and out repeatedly.
- **Quit, do not kill**, a logged-in viewer (`LLAppViewer` `requestQuit`):
  a killed viewer leaves the avatar standing in the region for a while.
  `LLAppViewer` `forceQuit` is that kill.

## With an AI assistant

An assistant that can run programs on your Mac -- one with a terminal or
MCP tools, for example -- can use all of the above: give it this page. A
simple way is a small LEAP program that relays commands to the viewer from
a socket only you can open, which the assistant then talks to; the
assistant never needs your password, and the viewer gains nothing that
listens to the network.

## Every API in Luminous 0.8.4

Ask `getAPIs` and `getAPI` for the build you run; this is 0.8.4's, checked
against its `getAPIs`.

| Pump | Operations |
|---|---|
| (the command pump) | The handshake's `command` pump: `getAPIs`, `getAPI` (`api`), `getFeatures`, `getFeature`, `listen`, `stoplistening`, `newpump`, `ping`. |
| `GroupChat` | Group chat: `startGroupChat`, `leaveGroupChat`, `sendGroupIM`. |
| `LLAgent` | Your avatar and the camera: `getID`, `getPosition`, `getGroups`, `requestTeleport`, `requestSit`, `requestStand`, `requestTouch`, `resetAxes`, `lookAt`, `startAutoPilot`, `getAutoPilot`, `startFollowPilot`, `setAutoPilotTarget`, `stopAutoPilot`, `setCameraParams`, `setFollowCamActive`, `removeCameraParams`, `playAnimation`, `stopAnimation`, `getAnimationInfo`, `getNearbyAvatarsList`, `getNearbyObjectsList`, `getAttachedObjectsList`, `getAgentScreenPos`. |
| `LLAppearance` | Outfits: `wearOutfit`, `wearItems`, `detachItems`, `getOutfitsList`, `getOutfitItems`. |
| `LLAppViewer` | `requestQuit` (logs out), `forceQuit` (does not: avoid). |
| `LLCommandDispatcher` | App commands: `dispatch`, `enumerate`. |
| `LLFloaterAbout` | `getInfo`: what the About window shows. |
| `LLFloaterReg` | Windows: `showInstance`, `hideInstance`, `toggleInstance`, `toggleInstanceOrBringToFront`, `instanceVisible`, `clickButton`, `getBuildMap`. |
| `LLGesture` | Gestures, once logged in: `getActiveGestures`, `isGesturePlaying`, `startGesture`, `stopGesture`. |
| `LLInventory` | Inventory, read only: `getItemsInfo`, `getFolderTypeNames`, `getAssetTypeNames`, `getBasicFolderID`, `getDirectDescendants`, `collectDescendantsIf`. |
| `LLNotifications` | Dialogs and notices: `requestAdd`, `listChannels`, `listChannelNotifications`, `respond`, `cancel`, `ignore`, `forward`. |
| `LLPipeline` | What is drawn: `toggleRenderTypes`, `hasRenderType`, `disableAllRenderTypes`, `enableAllRenderTypes`, `toggleRenderFeatures`, `hasRenderFeature`, `disableAllRenderFeatures`, `enableAllRenderFeatures`, `toggleRenderInfoDisplays`, `hasRenderInfoDisplay`, `disableAllRenderInfoDisplays`, `enableAllRenderInfoDisplays`. |
| `LLStartUp` | The login's progress: `getStateTable`, `postStartupState`. |
| `LLStats` | `getPerfData`: frame times and statistics. |
| `LLTeleportHandler` | `teleport`. |
| `LLToolMgr` | Build tools: `selectTool`, `openBuildFloater`. |
| `LLURLDispatcher` | Second Life links: `dispatch`, `dispatchRightClick`, `dispatchFromTextEditor`. |
| `LLViewerControl` | Settings: `get`, `set`, `set_notation`, `toggle`, `vars`, `groups`. |
| `LLViewerWindow` | `saveSnapshot`, `requestReshape` (`w`, `h`: the window's layout size). |
| `LLWindow` | Interface and input: `getPaths`, `getInfo`, `keyDown`, `keyUp`, `mouseDown`, `mouseUp`, `mouseMove`, `mouseScroll`, `pasteText`. |
| `LuminousEnvironment` | Luminous's: `getSun`, `setSun`, `getSky`, `setSky`, `getWater`, `setWater`, `reset`. |
| `UI` | `call`, `getValue`, `setSelectedByValue`. Luminous's functions for `call`: `Luminous.ApplyStyle`, `Luminous.Frame`, `Luminous.PortraitViewAngle`, `Luminous.PortraitViewAngleDefault`, `Luminous.ApplyPreset`, `Luminous.Environment`, `Luminous.RegionSky`, `Luminous.SaveShot`, `Luminous.GoToShot`, `Luminous.SaveSnapshot`, `Luminous.SaveHDRSnapshot`, `Luminous.Save16BitSnapshot`, `Luminous.SaveLargeSnapshot`, `Luminous.PreviewImage`, `Luminous.ResetSettings`, `Luminous.ResetLook`, `Luminous.ImportLUT`, `Luminous.SetUpForThisMac`. |
