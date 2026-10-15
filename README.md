# Luminous

**A Second Life viewer for taking better pictures**, for the Mac, by Needful.

**[Website](https://needfulsl.com/luminous/) · [Download](https://needfulsl.com/luminous/download.php)**

Luminous renders the picture its own way. You set the shot up in the live view,
which stays quick, and Luminous draws the costly effects in full in the
rendered picture:

- **Light**: volumetric light and sun shafts, projector beams in the air, soft
  shadows that harden at contact, contact shadows, GTAO ambient occlusion and
  enhanced reflections.
- **Lens**: depth of field that opens bright points into discs, bloom,
  halation, anamorphic streaks, lens flare, vignette, chromatic aberration and
  lens distortion.
- **Tone and colour**: film curves, highlights and shadows, colour balance,
  colour ranges, black and white through a filter, film grain and colour
  tables of your own.
- **Renders**: many frames averaged as light, sampled through the lens and
  across the sun, at up to twice the window's size, in PNG, JPEG, BMP, 16-bit
  TIFF or OpenEXR, fitted to the memory the Mac has.
- **Styles and presets**: twenty styles that set light, lens and look in one,
  skies made for photographing people, and presets of your own.
- **Sharing**: save a render, open it in Preview, or send it to your inventory,
  your profile feed, Flickr or email.

Everything is set from a sidebar, and each effect can be off, live, or only in
the render.

Help and bug reports: [needfulsl.com/luminous](https://needfulsl.com/luminous/).
In the viewer, Help > Report a Bug sends a report there.

## Requirements

A Mac with Apple silicon (M1 or later), macOS 15 or later, and 16 GB of memory.

## Building

```sh
luminous/scripts/bootstrap-mac.sh
```

It checks the toolchain, fetches the build variables, sets up autobuild in a
local virtualenv and builds the viewer. It reports what is missing rather than
installing anything itself. `--check` inspects and changes nothing;
`--no-build` stops after setup.

The bug report key and the Flickr app key are not in this repository; they are
read at configure time from files beside it. A build without them works, but
Report a Bug says it cannot send and Flickr cannot sign in.

## Source and licence

Luminous is based on the source code of the
[Phoenix Firestorm Viewer](https://www.firestormviewer.org) and Linden Lab's
Second Life viewer, and like them it is released under the **GNU Lesser General
Public License version 2.1**. The copyright notices throughout the source
belong to their authors and are left intact.
[CHANGES-LUMINOUS.md](CHANGES-LUMINOUS.md) states what Luminous changes, as
the LGPL requires, including everything it adds that talks to the network.

Everything in this repository is under the same licence, apart from a few
parts from elsewhere that keep their own. The name Luminous and its icon are
not licensed; see [luminous/NOTICE.md](luminous/NOTICE.md).

Please report any problems with this viewer to
[Needful](https://needfulsl.com/luminous/report.php). Second Life is a trademark
of Linden Research, Inc.
