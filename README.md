# Windows x64 build environment snapshot for Pico SDK 2.1.0

This is a binary image snapshot of my Windows x64 command-line
firmware build environment for the Raspberry Pi Pico SDK 2.1.0,
which is the latest official release version.  2.1.0 has support
for several newer boards, most notably the RP2350-based Pico 2.

Up through SDK version 1.5.1, Raspberry Pi maintained a one-click
installer for the Windows command-line build environment, at
https://github.com/raspberrypi/pico-setup-windows/.  That has 
since been deprecated, replaced with a VS Code extension, at
https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico.
See also [Getting started with Raspberry Pi Pico](https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf).

I created this snapshot because the VS Code extension still seems a
work-in-progress, to such an extent that I can't even get it to
install on my system.  I tend to prefer a command-line setup anyway,
for more flexibility in selecting editors and other tools, but for now
it's a practical necessity, until the VS Code extension matures to the
point where it's working.

## License

Everything here is licensed under the original terms of the third-party source.
It probably goes without saying that the Gnu toolchain tools are all under GPL.
The Pico SDK library source is released under a 3-clause BSD license.


## How to install

This snapshot is designed to be fully self-contained, following the
pattern of the official SDK 1.5.1 Windows setup.  There's no
installer; it's just a literal copy of my directory structure.

* Copy this entire directory tree to a location on your hard disk, say c:\pico-sdk-2.1.0

* Open `version.ini` from the newly copied files in a text editor, and change PICO_INSTALL_PATH to match the folder where you installed it

* You'll also need to install NMAKE, which is a proprietary Microsoft program that I can't include here.
  I think the easiest way to get it is to install the free Microsoft Visual Studio Community Edition.


### How to start a session

* Open a Windows command prompt window (CMD.EXE)

* Run `c:\pico-sdk-2.1.0\pico-env.cmd` (replace c:\ with the actual install path)

* Also make sure that NMAKE (from the Microsoft Visual Studio program folder) is in your PATH

* Now you can build your Pico projects with CMAKE and NMAKE per the usual command-line build procedures for the Pico SDK


## How I created this snapshot

Just to be clear, **you don't have to repeat these steps** to use the snapshot.
That's the whole point of the snapshot - you just copy it and you're set to go.
I'm documenting the process because it's a certainty that it'll have to be
repeated the next time Raspberry Pi releases a new major Pico SDK update.
I'm hoping that documenting it will save me some time reconstructing what I did.
It also should enable anyone else to make their own updated snapshot, without
having to wait for me to get around to it, in case I don't jump on it the instant
the next SDK comes out.


* Create an empty pico-sdk-2.1.0 folder to serve as the snapshot container

* Create a file called version.ini, containing:
```
[pico-setup-windows]
PICO_SDK_VERSION=2.1.0
PICO_INSTALL_PATH=c:\pico-sdk-2.1.0
PICO_REG_KEY=Software\Raspberry Pi\Pico SDK v2.1.0
```

* Install the Pico SDK 1.5.1
   * https://github.com/raspberrypi/pico-setup-windows/
   * copy the following files and folders into pico-sdk-2.1.0:
       * cmake/
       * git/
       * ninja/
       * openocd/
       * pico-sdk-tools/
       * python/
       * resources/
       * pico-env.cmd
       * ReadMe.txt

* Edit `ReadMe.txt` to add a comment at the top that it's a modified snapshot

* Edit `pico-env.cmd`, find the line `call :AddToPath "%PICO_INSTALL_PATH%\picotool"`, and add `\bin` at the end of the path

* Install the ARM build tools:
   * https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
   * Scroll down to 13.3.Rel1
   * Find the "Windows (mingw-w64-i686) hosted cross toolchains" section
   * Run the .exe installer from that list
   * create folder pico-sdk-2.1.0\gcc-arm-none-eabi
   * copy everything except uninstall.exe from c:\program files (x86)\Arm GNU Toolchain arm-none-eable\13.3 rel1 to pico-sdk-2.1.0\gcc-arm-none-eabi

* Install the 2.1.0 SDK
   * git clone https://github.com/raspberrypi/pico-sdk
   * cd pico-sdk
   * git checkout 2.1.0
   * cd lib\tinyusb
   * git submodule update --init
   * create folder pico-sdk-2.1.0\pico-sdk
   * copy the whole tree, minus the top-level .git/ and .github/ folders, into pico-sdk-2.1.0\pico-sdk

* Edit pico-sdk/src/rp2040/pico_platform/include/pico/platform.h to add missing extern "C" declarations:
```
   // before first function declaration
   #ifdef __cplusplus
   extern "C" {
   #endif

   // before final inclusion-guard #endif at end of file
   #ifdef __cplusplus
   }
   #endif
```

* Build picotool
   * download libUSB from https://libusb.info/, expand somewhere
   * `set LIBUSB_ROOT=<libusb folder you just created>`
   * `git clone https://github.com/raspberrypi/picotool`
   * `git checkout 2.1.0`
   * `mkdir build`
   * `cd build`
   * `cmake -S . -G "Nmake Makefiles" ..`
   * `nmake`
   * create folder `<path-to>\pico-sdk-2.1.0\picotool`
   * `cmake --install . --prefix <path-to>\pico-sdk-2.1.0\picotool`


## Theory of operation

The idea is to set up the 1.5.1 SDK for the Windows tools, then install
the parts that changed in the 2.1.0 SDK:

  - SDK source tree (pico-sdk)
  - Newer ARM Gnu Toolchain (13.3.Rel1)
  - 2.1.0 picotool

The 1.5.1 SDK sets up a bunch of build tools that are needed.  As always
with open-source projects, you need a steaming pile of Linux-world build
tools with specific version numbers, so it's a nightmare to cobble it all
together by hand.  The automated 1.5.1 installer has all of the necessary
tools (at their correct version snapshots) pre-packaged.

The newer SDK needs a newer version of the Gnu toolchain, but that's a
fairly easy one to install manually, since ARM packages a nice Windows
installer for it.

The only other new executable that seems to be required is the latest
picotool, and that's really only needed because the CMake script
checks that it's up to date.  I don't use it for anything myself, but
the CMake script bombs out if it's not present or isn't up-to-date,
so it was easier to just build it than to hack the build script to
bypass it.  Plus some people might use it.

