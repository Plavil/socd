# Linux SOCD cleaner
A basic linux SOCD cleaner (last priority ).
Currently this only implements the basic functionality without things such as custom keybinds or 
being able to set the applications in which this works. The only keys supported right now are WASD.

This was built and tested on Arch Linux, so the experience on other distributions may vary, but it should be
relatively easy to make this work.

## Running
This program requires `sudo` to run, or otherwise it won't be able to read key inputs.
First you have to build it with `./release` (if an error with permission denied shows do `chmod +x ./release` and try again) and
then you should be able to run it with `sudo ./socd`. To exit just hit `ctrl + c` in the terminal its run in.


## License
This is licensed under the MIT license.
