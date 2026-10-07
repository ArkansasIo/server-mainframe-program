# RailControl

**Railway Traffic Control & Interlocking System (RTCIS)**
Railway Traffic Management & Interlocking Simulator

A computer-based interlocking simulator written in C: signal control, point
machines, track circuits, train movement authority, route setting, a signaller
terminal, an automation rule engine and a Win32 GDI dispatch console.

```
Author       : Stephen (@Stephen)
Organisation : ArkansasIo
Repository   : https://github.com/ArkansasIo/server-mainframe-program
License      : MIT
```

---

## Credits

**Stephen** is the author of record for RailControl and is credited in every
department the program is organised into. These departments are the same list
printed by `rcp --title` and `rcp --credits`, so the credits cannot drift away
from the code — change the macro in `include/rc_version.h`, change the credit.

| Department                          | Credited |
| ----------------------------------- | -------- |
| System architecture                 | Stephen  |
| Interlocking and safety logic       | Stephen  |
| Signal control                      | Stephen  |
| Point and switch control            | Stephen  |
| Train control (ATC speed supervision)| Stephen |
| Track circuits and occupancy        | Stephen  |
| Field sensors and SCADA inputs      | Stephen  |
| Automation (CONJOB rule engine)     | Stephen  |
| Scripting (Lua object layer)        | Stephen  |
| Signaller terminal and command language | Stephen |
| Win32 GDI dispatch console          | Stephen  |
| Data and persistence                | Stephen  |
| Accounts, roles and permissions     | Stephen  |
| Build and tooling                   | Stephen  |
| Documentation and safety case       | Stephen  |
| Testing and verification            | Stephen  |
| Release engineering                 | Stephen  |

### Where to change the credits

Everything is driven from one header so the title screen, the About dialogue
and the log header can never disagree:

- `include/rc_version.h` — `RC_AUTHOR_NAME`, `RC_DEPT_*`, `g_credits`
- `src/rc_version.c` — the `g_credits[]` table and the renderers

Run either of these to see the credits rendered:

```powershell
rcp --title       # ASCII title screen with TEAM and DEPARTMENTS
rcp --credits     # About text with the full department list
```

The machine-readable build banner is written as the first line of every run:

```
RailControl 1.0.0 | build dev Oct  6 2026 | author Stephen | ArkansasIo | MIT
```

---

## Safety notice

> **RailControl is a SIMULATOR. It is NOT a certified interlocking and must not
> be connected to real signalling equipment.**

A deployable computer-based interlocking must satisfy **EN 50126** (RAMS),
**EN 50128** (software for railway control and protection systems),
**EN 50129** (safety-related electronic systems for signalling),
**EN 50159** (safety-related communication) and **IEC 61508 SIL 4**, including
fail-safe hardware with 2-out-of-2 or 2-out-of-3 voting, formal verification of
the interlocking truth tables, a certified toolchain and rigorous change
control.

### On the safety assessor role

The credits deliberately leave the **Safety assessor** unassigned. EN 50128
requires the independent safety assessor to be organisationally separate from
the developer. Crediting the author in that role would be a false claim, so it
stays:

```
Safety assessor : UNASSIGNED - required before any real deployment
```

No safety assessment has been carried out. This program claims none of the
properties above.

---

## Building

```powershell
make            # build build/rcp.exe
make test       # build and run the unit tests
make run        # interactive signaller console
make sim        # 60 second headless simulation
make demo       # run the batch example
```

## Running

```powershell
rcp --title                          # title screen and credits
rcp --credits                        # About text
rcp --permissions                    # role / permission matrix
rcp --terminal                       # interactive console
rcp --simulate 60 --scenario peak    # headless run
rcp --command "ROUTE R2" --user admin --password Admin#2024
```

Default accounts are installed for demonstration and **must be changed before
any operational use** (`rcp --permissions` lists them).

## Where RailControl sits in the taxonomy

| Term  | Meaning                                | In this program |
| ----- | -------------------------------------- | --------------- |
| CBI / EI | Computer-Based / Electronic Interlocking | `rcp_interlock.c` — route proof, point locking, aspects |
| CTC   | Centralised Traffic Control            | one control point for the area |
| SCADA | Supervisory Control and Data Acquisition | field inputs, alarms, event log |
| TMS   | Traffic Management System              | CONJOB automation, movement regulation |
| ATC   | Automatic Train Control                | speed supervision in the train controller |
