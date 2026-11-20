# tuxedo-ppd-adapter

A lightweight C daemon that acts as a drop-in D-Bus bridge between **`power-profiles-daemon` (PPD)** and **TUXEDO** power profiles.

Many Linux desktop environments (GNOME, KDE Plasma) and applications expect `net.hadess.PowerProfiles` / `org.freedesktop.UPower.PowerProfiles` on D-Bus to read or change system power modes. This adapter implements the PPD D-Bus interface via `sd-bus` and maps `power-saver`, `balanced`, and `performance` requests to your configured TUXEDO profiles.
