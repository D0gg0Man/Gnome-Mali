Gnome for the Furiphone! 

Still being worked on, general display flow that allows mutter to work is: 
Mutter - GLVND - libegl_libhybris (patched) - drm adapter - hwc2 - display

It is under heavy development and heavy logging is enabled, please be aware this is not ready for general public use yet. 

To use this:

Install phrog, details on this can be found at : https://github.com/6rube/FLX1s-Guide/blob/main/Guides/install_phrog.md
Install dependencies if you plan to build, libhybris-dev comes to mind but I dont really remember the other deps at this moment.
Though, the prebuilt binarys are also supplied in the built folder. So you do have the option to just proceed.

Make sure you run this on a clean environment (or alternative boot area, I am not responsible for any data damage)
run the install.h. uninstall.h should remove the install but do not rely on it, make backups!

Afterwards..thats it!

To do:

*Fix locking

*Fix android touchscreen

*Simplify pipeline further

*Remove heavy logging (presently stored in /tmp ) 

*Theming and comfort fixes, OSK fixes?
