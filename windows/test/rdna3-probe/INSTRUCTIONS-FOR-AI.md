# Running the RDNA3 test kit (instructions for an AI assistant)

You are helping a person run a short test on their Windows PC with an AMD Radeon RX 7000 series card. The kit is a
folder with a console program, `nr_tester.exe`. It checks whether a neural-rendering network runs on the AMD
Windows driver and writes one text file, `results.txt`. Your job is to run it unchanged and report the file back.

## Rules

- Do not edit, rebuild, recompile or "fix" anything in the kit. If something fails, that is the result to report.
- Do not use administrator rights. Nothing here needs them. Do not use `RunAs`, UAC workarounds or WSL for this.
- Do not install anything, change drivers, or change system settings.
- Do not copy, upload or share `nvngx_dlssnr.dll`, `dlssnr.bin` or any other NVIDIA file. Keep them in the kit folder.
- Only run `nr_tester.exe`. It starts `run_frame.exe` and `probe.exe` itself.
- A black screen, flicker, or a graphics driver reset during the test is expected information, not an error to repair.
  Do not retry in a loop: run it once, and report.

## Steps

1. Get the kit zip from the person (for example `DLSSNR-AMD-RDNA3-Windows-test-0.0.3-37-g11b5dc5-diag6.zip`).
   If they gave you a SHA-256, check it. Unzip the whole zip into a new folder they can write to (the Desktop is
   fine). Never run it from inside the zip and never unzip over an older kit folder.
2. The kit needs NVIDIA's `nvngx_dlssnr.dll` (the 310.8.0.0 build, SHA-256
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`), or a zip that contains it. It is only read to make
   test data, never run. Put it in the kit folder, next to `nr_tester.exe`. If an earlier kit folder already has
   `dlssnr-amd\dlssnr.bin`, copy that file into the new folder's `dlssnr-amd\` instead; the model is then not rebuilt.
3. Close games and other programs that use the graphics card.
4. In the kit folder, run `.\nr_tester.exe` in a normal (not elevated) terminal, or double-click `Run test.bat`.
   Leave it alone until it prints `The result is in ...\results.txt`.
   - The first run builds shader pipelines and can take several minutes.
   - If both tests fail, it then tries every shader on its own. That can take up to 45 minutes, and the window can
     look idle. Wait for it to finish.
5. Read `results.txt` in the kit folder without changing it.

## What to report

Give the person the complete `results.txt`, exactly as written (attach the file; do not summarise or trim it).
Then say, in a few lines:

- whether the program finished or you stopped it, and how long it took;
- the lines under `SUMMARY` (picture comparison and times, or the failure);
- if there is a `SHADER PROBE` section, the last lines of it (`OK n, crashed n`);
- anything unusual you saw on screen (error dialogs, a black screen, a driver reset notice).

## Reading the answer (for your own understanding; report it, do not act on it)

- `OK, identical to the Linux driver's picture` in both modes: the network runs correctly on this driver.
- `OK (tiny differences)`: harmless rounding differences.
- `DIFFERENT PICTURE`, `FAILED` or `TIMED OUT`: the log lines say where it stopped.
- `Pipeline probe` with `crashed` above 0: the AMD compiler crashes on those shaders; the table says which.
- `PROBLEM: ...` lines state what is missing (usually the NVIDIA file, or the graphics driver).
