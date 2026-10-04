#!/bin/sh
# The Flatpak's `2ksbox`: the launcher on Kirigami in a Plasma session
# when the KDE add-on is installed (com._2ksbox.Launcher.KDE, mounted at
# /app/kde), the GTK one otherwise (user decision, 2026-10-04). Both are
# the same launcher (ADR-023); each finds the app's prefix from its own
# path, so the GTK one is /app/bin/2ksbox-gtk and the KDE one
# /app/kde/2ksbox.
case ":${XDG_CURRENT_DESKTOP:-}:" in
  *:KDE:*)
    if [ -x /app/kde/2ksbox ]; then
      # The add-on has no Plasma platform theme (its manifest says why),
      # so the Breeze style is asked for by name, and KDE's own settings
      # file, kdeglobals (the colour scheme, the icon theme), is read
      # from the session's config directory: the sandbox's is the app's.
      export QT_STYLE_OVERRIDE="${QT_STYLE_OVERRIDE:-Breeze}"
      export XDG_CONFIG_DIRS="$HOME/.config:${XDG_CONFIG_DIRS:-/etc/xdg}"
      export XDG_DATA_DIRS="/app/kde/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
      exec /app/kde/2ksbox "$@"
    fi
    ;;
esac
exec /app/bin/2ksbox-gtk "$@"
