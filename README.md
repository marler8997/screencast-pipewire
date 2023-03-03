# Screencast Pipewire example

An example of using the XDP Screencast Portal and then capturing that video with Pipewire.

Use "meson" to build or use the `build` script which has no build tool dependencies (other than bash/coreutils).  It basically does this:

```
cc -o example example.c $(pkg-config --cflags --libs libpipewire-0.3  libportal) -DLIBPORTAL_VERSION=PUT_MINOR_VERSION_HERE
```
