#!/bin/bash
# Installation script for CW Skimmer

set -e

echo "CW Skimmer Installation"
echo "======================="

# Create system user if needed
if ! id "cw-skimmer" &>/dev/null; then
    echo "Creating system user cw-skimmer..."
    sudo useradd --system --home /var/lib/cw-skimmer --shell /usr/sbin/nologin cw-skimmer
fi

# Create installation directory
INSTALL_DIR="/opt/cw-skimmer"
CONFIG_DIR="/etc/cw-skimmer"
LOG_DIR="/var/log/cw-skimmer"

echo "Installing to $INSTALL_DIR..."
sudo mkdir -p $INSTALL_DIR/{bin,lib}
sudo mkdir -p $CONFIG_DIR
sudo mkdir -p $LOG_DIR

# Build and install
echo "Building CW Skimmer..."
make clean
make

# Install binaries
echo "Installing binaries..."
sudo cp bin/cw-skimmer $INSTALL_DIR/bin/
sudo chmod 755 $INSTALL_DIR/bin/cw-skimmer

# Install configuration
echo "Installing configuration..."
sudo cp cw-skimmer.conf $CONFIG_DIR/cw-skimmer.conf
sudo chmod 644 $CONFIG_DIR/cw-skimmer.conf

# Install systemd service
echo "Installing systemd service..."
sudo cp systemd/cw-skimmer.service /etc/systemd/system/
sudo systemctl daemon-reload

# Set permissions
echo "Setting permissions..."
sudo chown -R cw-skimmer:cw-skimmer $LOG_DIR
sudo chown -R cw-skimmer:cw-skimmer $INSTALL_DIR
sudo chown root:root $CONFIG_DIR/cw-skimmer.conf

echo ""
echo "Installation complete!"
echo ""
echo "Configuration: $CONFIG_DIR/cw-skimmer.conf"
echo "Logs: $LOG_DIR/"
echo ""
echo "To enable and start the service:"
echo "  sudo systemctl enable cw-skimmer"
echo "  sudo systemctl start cw-skimmer"
echo ""
echo "To check status:"
echo "  sudo systemctl status cw-skimmer"
echo ""
echo "To view logs:"
echo "  sudo journalctl -u cw-skimmer -f"
