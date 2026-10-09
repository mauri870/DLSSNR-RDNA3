# Building and running the RDNA3 test kit (instructions for an AI assistant)

You are helping a person test, on their Windows PC with an AMD Radeon RX 7000 series card, whether a
neural-rendering network runs on the AMD Windows driver. The test is a console program, `nr_tester.exe`, inside a
folder called the kit. It writes one text file, `results.txt`. Your job is to build the kit from this branch, run it
unchanged, and report the file back. Do part 1 and part 2 in order.

## Rules

- Do not edit the sources, the shaders or the kit. If a step fails, report the exact error; do not work around it by
  changing code.
- Do not use administrator rights. Nothing here needs them. Do not use `RunAs`, UAC workarounds, or `sudo` on Windows.
  Inside WSL you may use `apt install` for the packages named below and nothing else.
- Do not change drivers or Windows settings. Do not install anything on the Windows side.
- Do not copy, upload or share `nvngx_dlssnr.dll`, `dlssnr.bin` or any other NVIDIA file. They stay on this PC.
- Run the test once. A black screen, flicker, or a graphics driver reset during it is information, not an error to
  repair. Do not retry in a loop.

## Part 1: build the kit (in WSL)

Use the Ubuntu WSL distribution that already has the project's tools. Work in the WSL home directory, not under
`/mnt/c`, so line endings stay LF. Replace `<checkout>` with the existing Windows checkout as seen from WSL (for
example `/mnt/c/Users/<name>/Desktop/RDNA3`); it already has `toolchain/` with glslang 16.5.0 and the Vulkan headers.

```
sudo apt install -y g++ mingw-w64 patch zip python3 python3-numpy python3-pil
cd "<checkout>"
git fetch origin windows-rdna3-probe
git -c core.autocrlf=false worktree add --detach ~/rdna3-probe FETCH_HEAD
ln -s "<checkout>/toolchain" ~/rdna3-probe/toolchain
cd ~/rdna3-probe
bash windows/test/rdna3-probe/build_kit.sh
```

(If `toolchain/` is missing, run `bash fetch_deps.sh` in a normal clone first; it needs glslang and the Vulkan
headers only.) The script takes about a minute. It ends with a line `kit: <folder>`; that folder is the kit, under
`~/rdna3-probe/artifacts/windows/rdna3-probe-kit/`. If the script fails, stop and report the last 30 lines of its output.

Copy the kit to the Windows side, to a folder the person can write to, for example the Desktop:

```
cp -r "<kit folder>" "/mnt/c/Users/<name>/Desktop/"
```

## Part 2: run it (on Windows)

1. NVIDIA's `nvngx_dlssnr.dll` (the 310.8.0.0 build, SHA-256
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`) is needed to make the model. It is only read, never
   run. If the checkout has `build/model/dlssnr.bin`, copy that file to the kit's `dlssnr-amd\dlssnr.bin` and the DLL is
   not needed. Otherwise copy the DLL next to `nr_tester.exe`.
2. Close games and other programs that use the graphics card.
3. In the kit folder, run `.\nr_tester.exe` in a normal (not elevated) Windows terminal. If you only have WSL, run
   `./nr_tester.exe` from the kit folder under `/mnt/c/...`. Leave it alone until it prints
   `The result is in ...\results.txt`.
   - The first run builds shader pipelines and can take several minutes.
   - If both tests fail, it then tries every shader on its own. That can take up to 45 minutes and the window can look
     idle. Wait for it to finish.
4. Read `results.txt` in the kit folder without changing it.

## What to report

Give the person the complete `results.txt` exactly as written (attach the file; do not summarise or trim it). Name it
with the kit version, for example `results-0.0.3-41-gXXXXXXX.txt` (the version is in the kit folder's name). Then say,
in a few lines:

- whether the build and the program finished, and how long each took;
- the lines under `SUMMARY` (picture comparison and times, or the failure);
- if there is a `SHADER PROBE` section, its last lines (`OK n, crashed n`);
- anything unusual on screen (error dialogs, a black screen, a driver reset notice).

## Reading the answer (for your understanding; report it, do not act on it)

- `OK, identical to the Linux driver's picture` in both modes: the network runs correctly on this driver.
- `OK (tiny differences)`: harmless rounding differences.
- `DIFFERENT PICTURE`, `FAILED` or `TIMED OUT`: the log lines say where it stopped.
- `Pipeline probe` with `crashed` above 0: the AMD compiler crashes on those shaders; the table says which.
- `PROBLEM: ...` lines state what is missing (usually the NVIDIA file, or the graphics driver).
