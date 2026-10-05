# Privacy policy

2ksbox runs old operating systems and games on your own computer. It is
open source (the code is at <https://github.com/davidrios/2ksbox>), and
this page says what it does with your data, which is almost nothing.

*This page is the privacy policy for every build of 2ksbox, including
the Microsoft Store package. Last changed 2026-10-05.*

## What 2ksbox collects

Nothing. 2ksbox has no telemetry, no analytics, no crash reporting, no
account and no advertising. It never sends anything about you, your
computer or what you run to us or to anyone else.

## What 2ksbox stores, and where

Everything 2ksbox keeps stays on your computer, in a folder you can
open and delete:

- On Windows, installed from the Microsoft Store: `%USERPROFILE%\2ksbox`.
- On Windows, from the zip: `%APPDATA%\2ksbox\data`.
- On Linux: `~/.local/share/2ksbox`.
- On macOS: `~/Library/Application Support/2ksbox`.

That folder holds your machines (their settings, disk images and
snapshots), your disc shelf (paths to disc images you added), your
shader profiles, downloaded shaders, and two log files, `launcher.log`
and `player.log`. The logs describe what the program did (which machine
started, which devices it set up, errors); they are for troubleshooting,
are never sent anywhere, and you can delete them at any time.

Uninstalling the Microsoft Store package leaves that folder in place,
so your machines survive a reinstall. Delete the folder yourself if you
want everything gone.

## When 2ksbox uses the network

2ksbox connects to the internet in exactly two cases, both of which you
control:

1. **The shader download.** On first run 2ksbox offers to download the
   community's CRT shader collection (the `slang-shaders` repository)
   from GitHub, `codeload.github.com`. This is an ordinary file
   download; it sends nothing but the request for that file. You can
   decline, and 2ksbox works without it. Downloading it means GitHub
   sees the request, under GitHub's own privacy policy.
2. **A machine's network.** A machine (an emulated computer) has
   networking off unless you turn it on in its settings. When it is on,
   the operating system and programs inside that machine can reach the
   internet through your computer's connection, exactly as a real
   computer of that era plugged into your network could, and nothing
   inside the machine is inspected or recorded by 2ksbox.

There is no update check, no licence check and no "phone home".

## Your disk images and discs

2ksbox opens the disk images, disc images and folders you point it at.
It reads and writes them only to run your machines, and never copies or
uploads them.

## The clipboard and a shared folder

A Windows 11 machine can share your clipboard: text you copy on your
computer can be pasted inside the machine, and the other way round. It
is on for a new Windows 11 machine and can be turned off in its
settings. A machine can also be given one folder of yours, which
programs inside it can then read and write. No folder is shared unless
you pick one. Both stay between your computer and the machine; 2ksbox
sends neither anywhere else.

## Children

2ksbox does not collect data from anyone, children included.

## Changes and contact

Changes to this policy are made in the repository, where its history is
public: <https://github.com/davidrios/2ksbox/blob/main/docs/privacy.md>.
Questions go to the repository's issue tracker.
