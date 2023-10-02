## Background

I have a Yamaha RN-602 streaming amplifier that is connected via optical input to Samsung TV. Using this configuration is not the most convenient as I have to use to remote controls: turn the TV on, turn the amp on, switch amp input; use different remote to control amp volume and finally turn both devices off. This is usually solved by using HDMI input that Yamaha doesn't have.

So, this program makes your raspberry pi a "dummy" audio device seen by the TV. It has been tested with Raspberry PI 4 but theoreticall should work with any other device that supports HDMI-CEC or has a USB/HDMI adapter that supports CEC, i.g. PulseEight.

## Functions

The can run as a service and listens to CEC events. In order to work correctly, you have to switch sound output on your TV to the device running the program. The TV I am using supports parallel sound output to optical always - this has to be enabled for the sound to actually come through. The HDMI connection with TV will only be used for CEC commands (volume control, power on / off).

Please check the CLI arguments for more informaiton.

## Dependencies

- libcurlpp
- [nlohmann json](https://github.com/nlohmann/json)
- boost::program_options
- [libcec](https://github.com/Pulse-Eight/libcec)
