#!/bin/bash
# setup_grafana.sh
# Complete setup script for InfluxDB + Grafana dyno telemetry stack
# Run this on the Raspberry Pi

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "Dyno Telemetry Stack Setup"
echo "InfluxDB 2 + Grafana on Raspberry Pi"
echo "=========================================="
echo

# Check Docker
if ! command -v docker &> /dev/null; then
    echo "❌ ERROR: Docker not installed!"
    echo "Install with: curl -fsSL https://get.docker.com | sh"
    echo "Then add user to docker group: sudo usermod -aG docker $USER"
    exit 1
fi

if ! docker compose version &> /dev/null; then
    echo "❌ ERROR: Docker Compose plugin not installed!"
    echo "Install with: sudo apt-get install docker-compose-plugin"
    exit 1
fi

echo "✓ Docker installed"
echo "✓ Docker Compose plugin installed"
echo

# Step 1: Create secrets directory
echo "Step 1: Setting up secrets..."
mkdir -p secrets

# Check if secrets already exist
if [ -f "secrets/influxdb2-admin-username" ] && \
   [ -f "secrets/influxdb2-admin-password" ] && \
   [ -f "secrets/influxdb2-admin-token" ]; then
    echo "✓ Secret files already exist"
    echo "  If you want to recreate them, delete the secrets/ folder first"
else
    echo "Creating secret files..."
    
    # Create username
    echo "admin" > secrets/influxdb2-admin-username
    
    # Generate random password (16 chars)
    tr -dc 'A-Za-z0-9!@#$%^&*' < /dev/urandom | head -c 16 > secrets/influxdb2-admin-password
    
    # Generate random token (44 chars, base64-like)
    tr -dc 'A-Za-z0-9-_' < /dev/urandom | head -c 44 > secrets/influxdb2-admin-token
    
    chmod 600 secrets/influxdb2-admin-*
    
    echo "✓ Created secret files with random credentials"
    echo
    echo "IMPORTANT: Save these credentials!"
    echo "-------------------------------------------"
    echo "Username: $(cat secrets/influxdb2-admin-username)"
    echo "Password: $(cat secrets/influxdb2-admin-password)"
    echo "Token:    $(cat secrets/influxdb2-admin-token)"
    echo "-------------------------------------------"
    echo
    echo "Press Enter to continue..."
    read
fi

# Step 2: Start Docker stack
echo
echo "Step 2: Starting Docker stack..."
echo "This may take a minute on first run (downloading images)..."
echo

docker compose up -d

echo
echo "Waiting for services to be ready..."
sleep 10

# Check if containers are running
if docker compose ps | grep -q "Up"; then
    echo "✓ Docker containers are running"
else
    echo "❌ ERROR: Containers failed to start"
    echo "Check logs with: docker compose logs"
    exit 1
fi

# Step 3: Wait for InfluxDB to be ready
echo
echo "Step 3: Waiting for InfluxDB to initialize..."
echo "This can take 30-60 seconds on first run..."

for i in {1..60}; do
    if curl -s http://localhost:8086/health | grep -q "pass"; then
        echo "✓ InfluxDB is ready!"
        break
    fi
    echo -n "."
    sleep 1
done

if ! curl -s http://localhost:8086/health | grep -q "pass"; then
    echo
    echo "⚠️  WARNING: InfluxDB might not be ready yet"
    echo "Check status with: curl http://localhost:8086/health"
fi

# Step 4: Display access information
echo
echo "=========================================="
echo "Setup Complete!"
echo "=========================================="
echo
echo "Services are running:"
echo "  InfluxDB: http://$(hostname -I | awk '{print $1}'):8086"
echo "  Grafana:  http://$(hostname -I | awk '{print $1}'):3000"
echo
echo "InfluxDB Configuration:"
echo "  Organization: yorkracing"
echo "  Bucket:       telemetry"
echo "  Username:     $(cat secrets/influxdb2-admin-username)"
echo "  Password:     $(cat secrets/influxdb2-admin-password)"
echo "  Token:        $(cat secrets/influxdb2-admin-token)"
echo
echo "Grafana Login:"
echo "  Username: admin"
echo "  Password: ChangeMe_Once"
echo "  (Change this immediately after first login!)"
echo
echo "=========================================="
echo "Next Steps:"
echo "=========================================="
echo
echo "1. Open Grafana in browser:"
echo "   http://$(hostname -I | awk '{print $1}'):3000"
echo
echo "2. Log in with admin/ChangeMe_Once"
echo
echo "3. Add InfluxDB datasource:"
echo "   - Go to Configuration → Data Sources → Add data source"
echo "   - Select InfluxDB"
echo "   - Query Language: Flux"
echo "   - URL: http://influxdb2:8086"
echo "   - Organization: yorkracing"
echo "   - Token: $(cat secrets/influxdb2-admin-token)"
echo "   - Default Bucket: telemetry"
echo "   - Click 'Save & Test'"
echo
echo "4. Start the ingest script:"
echo "   cd ~/FSAE2025/Dyno"
echo "   python3 dyno_ingest_influx.py"
echo
echo "5. Create a dashboard in Grafana to visualize the data!"
echo
echo "=========================================="
echo "Useful Commands:"
echo "=========================================="
echo
echo "View logs:"
echo "  docker compose logs -f"
echo
echo "Stop stack:"
echo "  docker compose down"
echo
echo "Restart stack:"
echo "  docker compose restart"
echo
echo "=========================================="
<parameter name="task_progress">- [x] Verify basic serial communication works
- [x] Optimize Dyno_Final.py for Pi
- [x] Fix baud rate mismatch
- [x] Create Grafana setup script
- [ ] Run setup script on Pi
- [ ] Configure Grafana datasource
- [ ] Test dyno_ingest_influx.py
- [ ] Create basic Grafana dashboard
- [ ] Document complete setup
