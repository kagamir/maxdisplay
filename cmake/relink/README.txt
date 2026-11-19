MaxDisplay Windows x64 relinking kit

This directory contains the source of the statically linked libde265 library and
the application objects and other libraries used to link MaxDisplay. The program
uses libde265 under the GNU Lesser General Public License, version 3 or later.
libde265/COPYING contains the GNU LGPL and GPL texts. You may use these objects
to modify the libde265 portion, recombine or relink the application with a modified
version of libde265 for your own use, and reverse engineer it to debug those changes.

On Windows x64, install Visual Studio 2022 with Desktop Development with C++,
a Windows SDK and CMake 3.20 or newer. Extract the kit, make the desired changes
in libde265, and run these commands from its parent directory:

  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release --target maxdisplay --parallel

Run build/bin/maxdisplay.exe in place of the original executable.
