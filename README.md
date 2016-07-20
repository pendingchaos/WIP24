# WIP24
A XScreensaver screensaver that displays some shaders from [shadertoy.com](https://shadertoy.com).

# Dependencies
- XScreensaver
- An OpenGL implementation supporting compatibility OpenGL
- OpenGL headers
- libcurl and it's headers
- A C compiler
- A C standard library implementation

Most might be installed with
```shell
dnf install xscreensaver mesa-libGL-devel libcurl-devel gcc
```

# Compilation and Installation
```shell
make
make install
```
It will download XScreensaver 5.35's source code if it has not already. This may print a lot of text to stdout.

Before running, the should be a list of shaders at ~/.wip24/shaders.txt.
```shell
./install_shaders.sh
```
to copy src/shaders.txt to ~/.wip24/shaders.txt (this overwrites any previous list).

# Uninstallation
```shell
make uninstall
``` to uninstall the screensaver.
```shell
make full-uninstall
``` to uninstall the screensaver and delete ~/.wip24/.
