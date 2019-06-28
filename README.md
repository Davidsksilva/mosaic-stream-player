# Mosaic Player
A mosaic UDP video stream player using SDL and LibAV

## Getting Started

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.

### Prerequisites

What things you need to install the software and how to install them

- G++ compiler
- SDL2 library
- LibAV (FFMPEG C API)

To install SDL2 in ubuntu based linux distros:
```
sudo apt-get install libsdl2-dev
```

### Configuring

On the current state, the player plays up tp 4 streams. To configure the number of stream edit `config.hpp` file under /source.

### Running

To run the with make, in the directoy file:

```
make
```

And next:

```
./player [streams addresses]
```

An example script with 4 stream inputs is showed in `START.sh`
