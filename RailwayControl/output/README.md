# RailControl - binary output directory

This directory holds the **built executables**, ready to copy somewhere and run.
It is the deliverable folder: everything here is a finished binary, nothing here
is an intermediate object file.

The compiler writes its intermediate objects to `../build/`, which is the
scratch directory. This one is the shelf.

## Contents

Run `make dist` (or `build.ps1`) to populate it:

| Binary | Subsystem | Purpose |
| ------ | --------- | ------- |
| `rcp.exe`            | console | scriptable signaller console, batch and headless runs |
| `rcp-gui.exe`        | Windows | Win32 GDI dispatch console with the live track diagram |
| `names_tool.exe`     | console | naming diagnostics: list, check, resolve, validate |
| `auth_console.exe`   | console | register / logon / session console for the account store |

## Running

All four are standalone: they link the C runtime statically, so they do not
need MinGW DLLs on the target machine. They do read the tree around them, so
either run them from `RailwayControl/` or copy the whole directory.

```powershell
.\rcp.exe --version                      # version and copyright
.\rcp.exe --title                        # title screen and credits
.\rcp.exe --permissions                  # role / permission matrix
.\rcp.exe --command "ROUTE R2" --user admin --password Admin#2024

.\rcp-gui.exe                            # opens read-only until you log on
.\rcp-gui.exe --maximized --user admin --password Admin#2024

.\names_tool.exe                         # naming summary + uniqueness check
.\names_tool.exe --resolve-main          # regression-check the command forms

.\auth_console.exe --accounts            # list the installed accounts
.\auth_console.exe --policy              # password policy in force
```

## The demonstration credentials

The accounts below are installed on first run so the program can be
demonstrated. They are **not** secrets and must be changed before the program
is used for anything but training. `auth_console.exe --accounts` lists them.

| Userid | Role | Password |
| ------ | ---- | -------- |
| `admin` | ADMIN | `Admin#2024` |
| `supervisor` | SUPERVISOR | `Super#2024` |
| `signaller1` | SIGNALLER | `Signal#2024` |
| `signaller2` | SIGNALLER | `Signal#2024` |
| `viewer` | VIEWER | `View#2024` |

## Safety notice

> **RailControl is a SIMULATOR. It is NOT a certified interlocking and must not
> be connected to real signalling equipment.**

A deployable computer-based interlocking must satisfy EN 50126, EN 50128,
EN 50129, EN 50159 and IEC 61508 SIL 4. None of that is claimed for these
binaries. See the safety notice in `../README.md`.

## Note on version control

The binaries themselves are gitignored (`*.exe`), so this directory ships with
only this manifest and its `.gitkeep`. That is deliberate: a built artefact is
reproducible from source, and committing a 440 KB executable on every change
would bloat the history without adding anything a rebuild cannot produce.
