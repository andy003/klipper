# Klipper Simulation Guide

## Overview

Klipper provides several simulation capabilities to help with development, testing, and debugging of 3D printer firmware without requiring physical hardware.

## Available Simulation Types

### 1. Host Simulator (src/simulator/)

The host simulator allows running Klipper firmware code on your development machine for testing purposes.

**Configuration:**
- Located in `src/simulator/`
- Uses `MACH_SIMU` configuration option
- Default clock frequency: 20MHz
- Includes GPIO, ADC, SPI, and PWM simulation support

**Files:**
- `main.c` - Main entry point for simulator
- `gpio.c` - GPIO simulation
- `serial.c` - Serial communication simulation  
- `timer.c` - Timer simulation
- `Kconfig` - Configuration options

### 2. AVR SimulAVR Integration (scripts/avrsim.py)

Advanced AVR microcontroller simulation using the SimulAVR tool for accurate hardware emulation.

**Features:**
- Full AVR microcontroller simulation (default: atmega644)
- Serial port emulation via pseudo-TTY
- Real-time pacing support
- VCD trace file generation for signal analysis
- Support for different AVR variants

**Usage:**
```bash
# Compile Klipper for SimulAVR
make menuconfig  # Select AVR atmega644p + SIMULAVR support
make

# Run simulation
PYTHONPATH=/path/to/simulavr/build/pysimulavr/ ./scripts/avrsim.py out/klipper.elf

# With signal tracing
./scripts/avrsim.py out/klipper.elf -t PORTA.PORT,PORTC.PORT -f trace.vcd

# With custom settings
./scripts/avrsim.py out/klipper.elf -m atmega644 -s 16000000 -b 250000 -p /tmp/pseudoserial
```

**Command Options:**
- `-m, --machine`: AVR machine type (default: atmega644)
- `-s, --speed`: Machine speed in Hz (default: 16000000)
- `-r, --rate`: Real-time pacing rate
- `-b, --baud`: Serial baud rate (default: 250000)
- `-t, --trace`: Signals to trace (use `?` for help)
- `-p, --port`: Pseudo-TTY device path (default: /tmp/pseudoserial)
- `-f, --tracefile`: VCD trace filename

### 3. Motion Simulation and Analysis

**Motion Analysis Scripts:**
- `scripts/graph_motion.py` - Motion curve analysis and resonance simulation
- `scripts/graph_shaper.py` - Input shaper vibration response simulation
- `scripts/calibrate_shaper.py` - Input shaper calibration
- `scripts/graph_accelerometer.py` - Accelerometer data analysis

**Key Features:**
- Resonance frequency simulation (default: 60Hz)
- Damping ratio simulation (default: 0.15)
- Step response simulation
- Vibration response modeling

## Setup Instructions

### 1. SimulAVR Setup

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt update
sudo apt install g++ make cmake swig rst2pdf help2man texinfo

# Download and compile SimulAVR
git clone git://git.savannah.nongnu.org/simulavr.git
cd simulavr
make python
make build

# Verify installation
ls ./build/pysimulavr/_pysimulavr.*.so

# Optional: Create system packages
make cfgclean python debian
sudo dpkg -i build/debian/python3-simulavr*.deb
```

### 2. Klipper Simulation Build

```bash
# Configure for simulation
cd /path/to/klipper
make menuconfig
# Select: AVR atmega644p + "Support for simulavr software emulation"
make
```

### 3. Running Simulations

**Basic AVR Simulation:**
```bash
./scripts/avrsim.py out/klipper.elf
```

**With Klippy Integration:**
```bash
# Terminal 1: Start simulator
./scripts/avrsim.py out/klipper.elf

# Terminal 2: Run Klippy with test gcode
~/klippy-env/bin/python ./klippy/klippy.py config/generic-simulavr.cfg -i test.gcode -v
```

**Signal Analysis with GTKWave:**
```bash
# Generate VCD trace
./scripts/avrsim.py out/klipper.elf -t PORTA.PORT,PORTC.PORT

# View with GTKWave
gtkwave avrsim.vcd
```

## Debugging and Testing

### Batch Mode Testing
```bash
# Generate micro-controller commands from gcode
~/klippy-env/bin/python ./klippy/klippy.py ~/printer.cfg -i test.gcode -o test.serial -v -d out/klipper.dict

# Convert to human-readable format
~/klippy-env/bin/python ./klippy/parsedump.py out/klipper.dict test.serial > test.txt
```

### Motion Logging and Analysis
```bash
# Start data logging
~/klipper/scripts/motan/data_logger.py /tmp/klippy_uds mylog

# Generate motion graphs
~/klipper/scripts/motan/motan_graph.py mylog -o mygraph.png
```

## Configuration Files

**SimulAVR Config:** `config/generic-simulavr.cfg`
**Simulator Kconfig:** `src/simulator/Kconfig`

## Use Cases

1. **Firmware Development** - Test code changes without hardware
2. **Motion Planning** - Analyze and optimize motion algorithms
3. **Debugging** - Trace execution and identify issues
4. **Input Shaper Tuning** - Simulate vibration compensation
5. **Protocol Testing** - Verify communication protocols
6. **Education** - Learn firmware internals safely

## Limitations

- Simulator timing may not perfectly match real hardware
- Some hardware-specific features may not be fully emulated
- Performance characteristics differ from actual microcontrollers
- Real-time constraints may not be perfectly replicated

## Additional Tools

- `scripts/logextract.py` - Extract debugging info from logs
- `scripts/graphstats.py` - Generate performance graphs
- `scripts/test_klippy.py` - Automated regression testing
- `scripts/buildcommands.py` - Command generation utilities

This simulation environment provides a powerful platform for developing and testing Klipper firmware safely and efficiently.