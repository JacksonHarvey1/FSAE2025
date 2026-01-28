#!/bin/bash
# start_ingest.sh - Start the dyno ingest with proper checks

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "=========================================="
echo "Dyno Telemetry Ingest Startup"
echo "=========================================="
echo

# Step 1: Check InfluxDB is accessible
echo "Step 1: Checking InfluxDB..."
if curl -s http://localhost:8086/health | grep -q "pass"; then
    echo "✓ InfluxDB is running and healthy"
else
    echo "❌ InfluxDB is not accessible!"
    echo "   Fix: cd ~/FSAE2025/Dyno/stack && ./fix_port_conflict.sh"
    exit 1
fi
echo

# Step 2: Check token file exists
echo "Step 2: Checking token file..."
TOKEN_FILE="$HOME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"
if [ -f "$TOKEN_FILE" ]; then
    echo "✓ Token file found: $TOKEN_FILE"
    TOKEN=$(cat "$TOKEN_FILE")
    echo "  Token: ${TOKEN:0:10}... ($(echo -n "$TOKEN" | wc -c) chars)"
else
    echo "❌ Token file not found!"
    echo "   Expected: $TOKEN_FILE"
    echo "   Fix: Run the setup script in Dyno/stack/"
    exit 1
fi
echo

# Step 3: Check serial port
echo "Step 3: Checking serial port..."
if ls /dev/serial/by-id/usb-Adafruit_Feather_RP2040* 2>/dev/null | head -1; then
    SERIAL_PORT=$(ls /dev/serial/by-id/usb-Adafruit_Feather_RP2040* 2>/dev/null | head -1)
    echo "✓ Found RP2040: $SERIAL_PORT"
elif [ -e "/dev/ttyACM0" ]; then
    SERIAL_PORT="/dev/ttyACM0"
    echo "✓ Found serial port: $SERIAL_PORT"
else
    echo "⚠️  No RP2040 found, but will try anyway"
    SERIAL_PORT=""
fi
echo

# Step 4: Check Python dependencies
echo "Step 4: Checking Python dependencies..."
if python3 -c "import serial, influxdb_client" 2>/dev/null; then
    echo "✓ Python dependencies installed"
else
    echo "❌ Missing dependencies!"
    echo "   Install with: pip3 install pyserial influxdb-client"
    exit 1
fi
echo

# Step 5: Set environment variables
echo "Step 5: Setting environment variables..."
export INFLUX_URL="http://localhost:8086"
export INFLUX_ORG="yorkracing"
export INFLUX_BUCKET="telemetry"
export INFLUX_TOKEN_FILE="$TOKEN_FILE"
export TELEM_BAUD="921600"
if [ -n "$SERIAL_PORT" ]; then
    export TELEM_PORT="$SERIAL_PORT"
fi

echo "  INFLUX_URL=$INFLUX_URL"
echo "  INFLUX_ORG=$INFLUX_ORG"
echo "  INFLUX_BUCKET=$INFLUX_BUCKET"
echo "  INFLUX_TOKEN_FILE=$INFLUX_TOKEN_FILE"
echo "  TELEM_BAUD=$TELEM_BAUD"
if [ -n "$SERIAL_PORT" ]; then
    echo "  TELEM_PORT=$SERIAL_PORT"
fi
echo

# Step 6: Start ingest
echo "=========================================="
echo "Starting dyno_ingest_influx.py..."
echo "Press Ctrl+C to stop"
echo "=========================================="
echo

python3 dyno_ingest_influx.py
