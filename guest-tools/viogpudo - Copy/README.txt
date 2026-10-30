2ksbox: an EXPERIMENTAL display driver for Windows 11

    Not signed by Microsoft. Not supported. Read all of this first.

What it is

  2ksbox's own build of Red Hat's VirtIO GPU display driver (viogpudo),
  with a vertical blank in step with your computer's screen. Windows'
  desktop composes on that blank, so scrolling, window moves and
  animations are smoother than with Red Hat's driver, which has none.
  Red Hat's driver, installed from this disc, stays the supported one.

What installing it does to this Windows

  - Turns on TEST MODE (bcdedit /set testsigning on). A "Test Mode"
    watermark stays in the corner of the desktop, and Windows then loads
    any test-signed driver, not only this one: this Windows is less
    secure.
  - Makes a code-signing certificate ("2ksbox test signing") on this
    machine, trusts it, and signs the driver with it.
  - Removes Red Hat's display driver and installs this one.

What it needs

  - Secure Boot off. 2ksbox's Windows 11 machines have it off by default.
  - Smart App Control off. If it is on (or evaluating), the script stops
    and says so. Turning it off is permanent: Windows cannot turn it on
    again without being reinstalled.
  - An administrator account.

Installing it

  1. Take a snapshot of the machine in 2ksbox, so you can go back.
  2. Run install.cmd in this folder. It asks for administrator rights,
     then warns you twice; answer y, then type INSTALL.
  3. The first run only turns test mode on. Restart Windows.
  4. Run install.cmd again (two more warnings). It installs the driver.
  5. Restart Windows.

Undoing it

  The easiest way is the snapshot from step 1. Otherwise, in PowerShell as
  administrator, with this disc in drive D: (use its letter):

    Get-WindowsDriver -Online | Where-Object OriginalFileName -like '*\viogpudo.inf' |
      ForEach-Object { pnputil /delete-driver $_.Driver /uninstall /force }
    pnputil /add-driver 'D:\2ksbox\viogpudo\viogpudo.inf' /install
    Remove-ItemProperty HKLM:\SOFTWARE\2ksbox -Name ExperimentalDisplay
    Get-ChildItem Cert:\LocalMachine\My, Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
      Where-Object Subject -eq 'CN=2ksbox test signing' | Remove-Item
    bcdedit /set testsigning off

  then restart Windows.

If the screen stays black

  Go back to the snapshot. Without one, Windows' recovery (hold Shift
  while clicking Restart, or three interrupted boots) offers Safe Mode,
  where the commands above work.

License: BSD-3-Clause, as the virtio-win source it is built from
(virtio-win_license.txt at the disc's root).
