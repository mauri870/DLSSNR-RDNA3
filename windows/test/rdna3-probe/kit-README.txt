RDNA3 network test (version @VERSION@)
=================================

Thank you for helping. This checks whether a neural-rendering network runs correctly on your AMD
graphics card under Windows. It takes about 2 to 15 minutes. It installs nothing and changes nothing on your PC.

What you need
  - A Radeon RX 7000 series card (RX 7600, 7700 XT, 7800 XT, 7900 GRE / XT / XTX). Other cards: you can still run it.
  - The current AMD Adrenalin graphics driver.
  - The one extra file you were sent separately (it is called nvngx_dlssnr.dll, or it is a zip file).
    It is only read to build the test data; it is never run.

Steps
  1. Unzip this whole folder somewhere (for example the Desktop). Do not run the test from inside the zip.
  2. Copy the extra file you were sent into the same folder, next to "Run test.bat".
  3. Close games and other programs that use the graphics card.
  4. Double-click "Run test.bat". If Windows says "Windows protected your PC", click "More info", then "Run anyway"
     (the programs are not signed). Some antivirus programs may ask the same; this is expected.
  5. Wait until it says it is done. The first run can take several minutes and the screen may flicker or go
     black for a moment. If nothing seems to happen for more than 30 minutes, close the window.
     If the first two tests fail, the program then tries every shader on its own to find the cause. That can
     take up to 45 minutes and the window may look idle. Please leave it running until it says it is done.
  6. Send back the file results.txt that appears in the same folder. Nothing else is needed.

results.txt holds your Windows version, the name and driver of your graphics card, and the test results.
It contains no personal files.
