#!/bin/bash
# Klipper Development Container Setup Script
set -e

echo "🚀 Setting up Klipper development environment..."

# Update package lists
echo "📦 Updating package lists..."
sudo apt-get update

# Install system packages required for Klipper development
echo "🔧 Installing system packages..."
PKGLIST="virtualenv python3-dev libffi-dev build-essential"
# kconfig requirements
PKGLIST="${PKGLIST} libncurses-dev"
# hub-ctrl
PKGLIST="${PKGLIST} libusb-dev"
# AVR chip installation and building
PKGLIST="${PKGLIST} avrdude gcc-avr binutils-avr avr-libc"
# ARM chip installation and building
PKGLIST="${PKGLIST} stm32flash dfu-util libnewlib-arm-none-eabi"
PKGLIST="${PKGLIST} gcc-arm-none-eabi binutils-arm-none-eabi libusb-1.0"
# Development tools
PKGLIST="${PKGLIST} git curl wget nano vim tree htop screen tmux"
# Additional useful tools for development
PKGLIST="${PKGLIST} minicom socat can-utils"

sudo apt-get install -y ${PKGLIST}

# Create Python virtual environment for Klipper
echo "🐍 Setting up Python virtual environment..."
PYTHONDIR="${PWD}/klippy-env"

if [ ! -d "${PYTHONDIR}" ]; then
    echo "Creating new virtual environment..."
    python3 -m venv ${PYTHONDIR}
else
    echo "Virtual environment already exists, updating..."
fi

# Install Python dependencies
echo "📚 Installing Python dependencies..."
${PYTHONDIR}/bin/pip install --upgrade pip setuptools wheel
${PYTHONDIR}/bin/pip install -r scripts/klippy-requirements.txt

# Install additional development tools
echo "🛠️ Installing additional Python development tools..."
${PYTHONDIR}/bin/pip install black flake8 pylint mypy

# Set up Git hooks (optional)
echo "🔗 Setting up Git configuration..."
git config --global --add safe.directory /workspaces/klipper

# Create useful aliases
echo "⚡ Setting up helpful aliases..."
cat >> ~/.bashrc << 'EOF'

# Klipper development aliases
alias klippy='~/klippy-env/bin/python'
alias klipper-build='make clean && make menuconfig'
alias klipper-flash='make flash'
alias klipper-test='~/klippy-env/bin/python scripts/test_klippy.py'

# Activate virtual environment automatically
source ~/klippy-env/bin/activate
EOF

cat >> ~/.zshrc << 'EOF'

# Klipper development aliases  
alias klippy='~/klippy-env/bin/python'
alias klipper-build='make clean && make menuconfig'
alias klipper-flash='make flash'
alias klipper-test='~/klippy-env/bin/python scripts/test_klippy.py'

# Activate virtual environment automatically
source ~/klippy-env/bin/activate
EOF

# Add user to dialout group for serial port access
echo "🔌 Adding user to dialout group for serial port access..."
sudo usermod -a -G dialout vscode

# Create sample printer.cfg if it doesn't exist
if [ ! -f "printer.cfg" ]; then
    echo "📄 Creating sample printer.cfg..."
    cat > printer.cfg << 'EOF'
# This is a sample printer configuration file for Klipper development
# Copy and modify this file according to your 3D printer setup

[mcu]
# Replace with your MCU serial port
serial: /dev/ttyUSB0

[printer]
kinematics: cartesian
max_velocity: 300
max_accel: 3000
max_z_velocity: 5
max_z_accel: 100

# Add your printer-specific configuration below
# Refer to https://www.klipper3d.org/Config_Reference.html

EOF
fi

echo "✅ Klipper development environment setup complete!"
echo ""
echo "🎯 Next steps:"
echo "   1. Configure your printer.cfg file"  
echo "   2. Build firmware: make menuconfig && make"
echo "   3. Flash firmware: make flash"
echo "   4. Start Klippy: ~/klippy-env/bin/python klippy/klippy.py printer.cfg"
echo ""
echo "💡 Helpful commands:"
echo "   - klipper-build: Clean build and configure"
echo "   - klipper-test: Run Klippy tests"
echo "   - klippy: Direct access to Python in virtual environment"