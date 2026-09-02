# WineProbe

One-shot capability report for the mod's Windows-only dependencies, used to find out what
does and does not exist under Wine/Proton without needing the game, a GPU, or a Linux
install of your own.

Build with `build.bat` (32-bit, deliberately — see the comment in it), then run it:

- **on Windows**, to get the baseline every other run is compared against;
- **under Wine/Proton**, to see what is missing there.

It never statically imports anything that might be absent, so a missing DLL is a reported
line rather than a failure to launch. Output ends in a PASS/FAIL summary per subsystem.

Findings from the runs so far: `docs/Research/LinuxWineCompatibility.md`.

Hand this to a Linux reporter when a bug report needs their environment characterised.
