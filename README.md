# WIP24
A XScreensaver screensaver that displays shaders from [shadertoy.com](https://shadertoy.com).

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
By default, the shader list is empty. Run
```shell
./install_shaders.sh
```
to install one. The list is stored in ~/.wip24/shaders.txt

# Uninstallation
```shell
make uninstall
```
