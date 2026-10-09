---
title: Keys and the player window
description: Keyboard shortcuts, the mouse, screenshots and closing a machine.
---

Each machine runs in its own player window. On macOS, Ctrl+Alt in the
tables below is ⌥⌘ (Option+Command).

## The machine

| Keys | What |
|---|---|
| Ctrl+Alt+G | Release the mouse. Click in the window to take it again |
| Ctrl+Alt+K | Send shortcuts like the Windows key and Alt+Tab to the machine, or back to your computer |
| Ctrl+Alt+Shift+D | Ctrl+Alt+Del in the machine |
| Ctrl+Alt+Shift+P | Pause and resume |
| Alt+F4, Ctrl+Q (⌘Q) | Close the player. It asks first |

The *Machine* menu also has **Reset** and **Power Button**. The power
button asks Windows to shut down, as on a real PC. It's the clean way to
turn a machine off.

## The view

| Keys | What |
|---|---|
| Ctrl+Alt+Shift+F | Full screen on and off. On macOS, use the green button or *View* > *Enter Full Screen* |
| Ctrl+Alt+0 | Scale: the largest that fits |
| Ctrl+Alt+1 to 4 | Scale: 1x to 4x |
| Ctrl+Alt+Shift+0 | Fit the window to the picture |
| Ctrl+Alt+S | Save a screenshot of the machine's own picture |
| Ctrl+Alt+Shift+S | Save a screenshot of what the window shows, CRT shader included |

Screenshots go to your Pictures folder, in `2ksbox`, numbered
`2ksbox-0001.png`, `2ksbox-0002.png` and so on.

## The mouse

Windows machines start with **Seamless mouse** on. Your pointer is the
machine's pointer, and it moves in and out of the window freely.

Some games want a real PS/2 mouse that the window holds on to. Turn
**Seamless mouse** off on the machine's **Input** page. Then a click
takes the pointer into the window, and Ctrl+Alt+G gives it back. The
title bar reminds you while the window holds the pointer.

## The window title

The title tells you when something is captured:

- *(Ctrl+Alt+G releases the mouse)*: the window holds the pointer.
- *(Ctrl+Alt+K sends shortcuts to the guest)*: the Windows key and
  Alt+Tab go to your computer, not the machine.

## Closing

Closing the player window turns the machine off at once, like pulling
the plug, so the player asks first. Unsaved work in the machine is lost.

Shut the machine down from its own Start menu, or with *Machine* >
**Power Button**, when you can. A Windows 98 machine pulled off this way
runs ScanDisk on its next start.
