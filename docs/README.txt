IMAGES FOR THE ARTICLE
======================
The article (../README.md) points at these files. Names and extensions have to
match exactly, since GitHub raw URLs are case-sensitive.

ALREADY HERE
  hero.jpg          Opening shot of the finished badge.
  architecture.png  Signal-flow diagram.
  cad.JPEG          Internal CAD layout.
  panel.jpeg        Panel close-up, near the end.

STILL TO ADD
  demo.gif          The refresh in motion. Sits in the Result section and is the
                    single most convincing thing on the page. Until the file
                    exists the article just skips it.

MAKING demo.gif (iPhone clip -> Windows)
---------------------------------------
What to record: run epaper_cycle and film one full image change, from the old
image through the flicker to the new one settling. A propped phone or tripod
matters more than camera quality, since wobble is obvious in a loop.

A real refresh is 18 to 20 seconds, so a clip lands around 25 seconds. Speeding
it up 4x gives roughly 6 seconds, which loops well.

STEP 1. Get the clip off the phone.
  Plug the iPhone in over USB, unlock it, tap Trust. In File Explorer:
    This PC > Apple iPhone > Internal Storage > DCIM
  Copy the .MOV out.

  Helpful setting on the phone (avoids HEVC surprises):
    Settings > Photos > Transfer to Mac or PC > Automatic
  ffmpeg reads HEVC fine either way, so this is convenience, not a requirement.

STEP 2. Install ffmpeg (PowerShell).
    winget install ffmpeg
  Reopen PowerShell afterward so it is on PATH.

STEP 3. Speed up and convert. cd to the clip's folder, then:

  ffmpeg -i clip.mov -vf "setpts=PTS/4,fps=12,scale=600:-1:flags=lanczos,palettegen" palette.png

  ffmpeg -i clip.mov -i palette.png -lavfi "setpts=PTS/4,fps=12,scale=600:-1:flags=lanczos[v];[v][1:v]paletteuse" demo.gif

  setpts=PTS/4 is the speedup. Use /3 for about 8 s, /5 for about 5 s.
  The two-pass palette is what stops the six-ink colors turning to mud.

  To trim dead air, add  -ss 2 -to 22  right after  -i clip.mov  in BOTH
  commands (starts at 2 s, ends at 22 s).

STEP 4. Check size. Aim under about 5 MB. If larger, use fps=10 or
  scale=480:-1 and rerun.

Then drop demo.gif in this folder. The article picks it up automatically.

Note: the article says the refresh takes 15 to 30 seconds, so a sped-up GIF is
honest as long as that text stays. If you want, the caption can say "4x speed".

An MP4 is smaller and sharper, but GitHub will not autoplay video inline in a
README, which is why this is a GIF.

MORE PHOTOS LATER
-----------------
These would all strengthen the article if you get them. Drop them in and say the
word and I will place them:
  wiring.jpg      the nine wires on the header, pairs with the wiring table
  flipper-ui.jpg  the Flipper screen showing the menu or "Drawing..." status
  print.jpg       the enclosure fresh off the printer or mid-assembly
  macro.jpg       a close-up of the dithered speckle, sells the six-ink trick

TIPS
----
- A few hundred KB per still is plenty. Multi-MB files slow the page down.
- The panel is 400x600 portrait, so portrait framing suits it.
