# Tuxedo power profies adapter

A lightweight C daemon that acts as a drop-in D-Bus bridge between **`power-profiles-daemon` (PPD)** and **TUXEDO Control Center (`tccd`)**.

Desktop environments and shells (such as Noctalia, GNOME, and KDE Plasma) use `org.freedesktop.UPower.PowerProfiles` or `net.hadess.PowerProfiles` on the system D-Bus to read and switch between `power-saver`, `balanced`, and `performance`. This adapter implements both D-Bus names and translates profile requests to your configured TUXEDO Control Center profiles.

## What It Does

- **Implements the PPD D-Bus API via `sd-bus`** on both `org.freedesktop.UPower.PowerProfiles` and `net.hadess.PowerProfiles`:
  - `ActiveProfile` (readable and writable by unprivileged desktop users)
  - `Profiles`, `Actions`, `PerformanceDegraded`, `Version`
  - `HoldProfile`, `ReleaseProfile`, `ActiveProfileHolds`, and `ProfileReleased` (with automatic hold cleanup if a client disconnects from D-Bus)
- **Maps PPD profiles to TCC profile IDs** using `/etc/tcc-power-profiles/config.json` and applies them via `com.tuxedocomputers.tccd`'s `SetTempProfileById`.
- **Watches `/etc/tcc-power-profiles` with `inotify`** so edits to `config.json` are validated, reloaded, and re-applied automatically.
- **Syncs external TCC changes back to D-Bus**: polls `tccd` (`GetActiveProfileJSON`) every 1 second and emits `PropertiesChanged` when `tccd` switches to another mapped profile (e.g., on AC/battery changes or via the TCC GUI).

## Dependencies

- **Runtime**: `tuxedo-control-center` (`tccd.service` running), `libsystemd`, `json-c`
- **Build**: `gcc`, `make`, `pkg-config`, `systemd` headers, `json-c` headers

On Arch Linux:

```bash
sudo pacman -S --needed base-devel systemd json-c
```

## Installation

1. Build and install:

   ```bash
   make
   sudo make install
   ```

   This installs:
   - `/usr/bin/tcc-power-profiles`
   - `/etc/tcc-power-profiles/config.json` (default config only if it does not already exist)
   - `/usr/share/dbus-1/system.d/tcc-power-profiles.conf`
   - `/usr/lib/systemd/system/tcc-power-profiles.service`

2. Replace `power-profiles-daemon` (only one daemon can own the PPD D-Bus names):

   - **Option A — Uninstall `power-profiles-daemon`** (recommended if nothing requires it):
     ```bash
     sudo systemctl stop power-profiles-daemon 2>/dev/null || true
     sudo systemctl unmask power-profiles-daemon 2>/dev/null || true
     sudo pacman -R power-profiles-daemon
     ```
     *(Note: uninstalling `power-profiles-daemon` also removes the `powerprofilesctl` CLI tool)*
   - **Option B — Keep `power-profiles-daemon` installed, but masked**:
     ```bash
     sudo systemctl mask --now power-profiles-daemon
     ```

3. Reload D-Bus and enable `tcc-power-profiles`:

   ```bash
   sudo systemctl reload dbus
   sudo systemctl daemon-reload
   sudo systemctl enable --now tcc-power-profiles
   ```

## Configuration

Edit `/etc/tcc-power-profiles/config.json` to map each PPD profile to a valid TCC profile `id`:

```json
{
  "profiles": {
    "power-saver": "__legacy_powersave_extreme__",
    "balanced": "__legacy_cool_and_breezy__",
    "performance": "__legacy_default__"
  }
}
```

To see all available TCC profile IDs on your system:

```bash
busctl call com.tuxedocomputers.tccd /com/tuxedocomputers/tccd com.tuxedocomputers.tccd GetProfilesJSON
```

When `config.json` is saved, `tcc-power-profiles` validates the IDs against `tccd` and re-applies the active profile automatically.

## AC Plug / Unplug Behavior with TUXEDO Control Center

When you plug or unplug your charger, you may notice `tccd` switching away from the profile you picked in PPD back to TCC's own default profile.

### Why this happens

- `tcc-power-profiles` switches profiles using `tccd`'s `SetTempProfileById` method, which only sets a **temporary** profile in `tccd`.
- `tccd`'s internal `StateSwitcherWorker` checks power state every 2 seconds. Whenever the system transitions between AC (`power_ac`) and battery (`power_bat`), `tccd` clears the temporary profile and applies the profile configured in TCC's `stateMap` (`/etc/tcc/settings`).
- TCC does not provide a setting to disable this AC/battery `stateMap` switch.

### How to configure it

1. In **TUXEDO Control Center**, set your AC and battery default profiles to profiles that are mapped in `/etc/tcc-power-profiles/config.json` (for example, `Default` for AC and `Powersave extreme` for battery, rather than an unmapped custom profile like `TUXEDO Defaults`).
2. Because `tccd` does not emit a D-Bus signal when its active profile changes, `tcc-power-profiles` polls `tccd` every 1 second (`GetActiveProfileJSON`). Whenever `tccd` switches to another mapped profile on plug/unplug (or from the TCC GUI), `tcc-power-profiles` detects the change and emits `PropertiesChanged` on D-Bus so Noctalia and other desktop widgets update automatically.

*(To avoid racing with `tccd`'s asynchronous profile switch right after you click a PPD profile, syncing is briefly paused for 2.5 seconds after each local profile change.)*
