# Visual Studio "Play" (ReleaseDeploy / DebugDeploy) setup

Pressing Play on a `*Deploy` config is supposed to do three things:

1. build `bin\Release\dinput8.dll`
2. copy the DLL (+ `BBCFIMUpdater.exe`, `USER_README.txt`) into the BBCF install folder
3. launch BBCF through Steam

Steps 2 and 3 are **not** the debugger's job. They are the `AfterBuild` target in
`BBCF_IM.vcxproj`, and every one of its conditions depends on properties that live in
`deploy/DeploySettings.props`.

## The single most likely cause: `deploy/DeploySettings.props` is missing

`deploy/DeploySettings.props` is **gitignored** (it holds machine-specific paths). The import
in `BBCF_IM.vcxproj` is:

```xml
<Import Project="deploy\DeploySettings.props" Condition="exists('deploy\\DeploySettings.props')" />
```

So if the file is gone, MSBuild does not error. `$(BbcfInstallDir)` becomes empty, every
`Copy` in `AfterBuild` has a false condition, `$(BbcfLaunchAfterDeploy)` is empty, and the
`Exec` that launches the game never runs.

Result: **the build succeeds and absolutely nothing else happens.** No warning, no error, and
`git status` cannot tell you the file vanished, because it's ignored.

Fix: `copy deploy\DeploySettings.props.example deploy\DeploySettings.props` and fill in the
real paths.

## Diagnosis checklist, in order

1. **Does `deploy/DeploySettings.props` exist?** If not, that's it. Stop here.
2. **Did the DLL actually land in the game folder?** Compare timestamps:
   `bin\Release\dinput8.dll` vs `<install>\dinput8.dll`. If the install copy is older than
   the build, the deploy step was skipped — go back to 1.
3. **Set MSBuild verbosity to Detailed** (Tools > Options > Projects and Solutions > Build and
   Run) and search the output for `AfterBuild`. Skipped conditions are printed there with the
   expanded property values, which shows immediately whether `BbcfInstallDir` is empty.
4. **Was a build even run?** `AfterBuild` only fires when the project isn't up to date. An
   up-to-date project => no copy, no launch, Play looks dead. Force it with Rebuild.
5. **Is BBCF still running?** A live game locks `dinput8.dll`; you get `warning MSB3026`,
   ten retries, and the deploy is silently abandoned. Close the game first, or you keep
   testing the previous build.
6. **Is the DLL in the right install folder?** Check `BbcfInstallDir` against the actual Steam
   library — a moved/reinstalled game or a second Steam library silently deploys to nowhere.

## Why the debugger command is a no-op stub

`ReleaseDeploy`'s `LocalDebuggerCommand` is `$(ComSpec)` with `/c exit 0` — deliberately a
command that exits instantly. Two reasons:

- The game must be started **through Steam** (`steam.exe -applaunch 586140`). There is no
  `steam_appid.txt` in the install folder, so launching `BBCF.exe` directly always gives
  "Steam must be running to play this game", even when Steam *is* running.
- If the debugger also launched the game, you'd get two instances — one from `AfterBuild`,
  one from the debugger.

Corollary: if you ever see **"dinput8.dll is not a valid Win32 application"**, the debugger
command is *empty*, not broken. VS falls back to launching the build output (a DLL). This
happens when VS fails to expand a property imported from a props file, which is why the
literal stub lives in the `.vcxproj` and `BBCF_IM.vcxproj.user` (VS reads `.user` last, so it
wins).

`DebugDeploy` uses `$(BbcfDebuggerCommand)` from the props file. Keeping that pointed at the
same `$(ComSpec)` / `/c exit 0` stub makes both Deploy configs behave identically.

## Automated / CLI builds

Always suppress the launch so a build never opens the game behind you:

```bash
MSBuild.exe BBCF_IM.sln /m /p:Configuration=ReleaseDeploy /p:Platform=Win32 \
  /p:PlatformToolset=v143 /p:BbcfLaunchAfterDeploy=false
```

## Unrelated but adjacent

Never build a different branch into the shared `build/` and `bin/` directories — stale objects
from another branch make VS fail in confusing ways. Use a separate `OutDir`/`IntDir` (or a
worktree) instead.
