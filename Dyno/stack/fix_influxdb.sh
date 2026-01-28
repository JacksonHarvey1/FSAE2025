#!/bin/bash
# fix_influxdb.sh - Fix common InfluxDB startup issues

set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "=========================================="
echo "InfluxDB Troubleshooting & Fix Script"
echo "=========================================="
echo

# Step 1: Stop everything cleanly
echo "Step 1: Stopping existing containers..."
docker compose down
sleep 2
echo "✓ Containers stopped"
echo

# Step 2: Check and recreate secret files
echo "Step 2: Checking secret files..."
mkdir -p secrets

if [ ! -f "secrets/influxdb2-admin-username" ] || \
   [ ! -f "secrets/influxdb2-admin-password" ] || \
   [ ! -f "secrets/influxdb2-admin-token" ]; then
    echo "Creating new secret files..."
    echo "admin" > secrets/influxdb2-admin-username
    tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 16 > secrets/influxdb2-admin-password
    tr -dc 'A-Za-z0-9-_' < /dev/urandom | head -c 44 > secrets/influxdb2-admin-token
    echo  # Add newline
fi

# Make sure files have no trailing newlines or spaces
sed -i 's/[[:space:]]*$//' secrets/influxdb2-admin-username 2>/dev/null || true
sed -i 's/[[:space:]]*$//' secrets/influxdb2-admin-password 2>/dev/null || true
sed -i 's/[[:space:]]*$//' secrets/influxdb2-admin-token 2>/dev/null || true

chmod 600 secrets/influxdb2-admin-*
echo "✓ Secret files ready"
echo

# Step 3: Verify Docker Compose file
echo "Step 3: Verifying docker-compose configuration..."
if docker compose config > /dev/null 2>&1; then
    echo "✓ Compose file is valid"
else
    echo "❌ ERROR: Compose file has syntax errors"
    docker compose config
    exit 1
fi
echo

# Step 4: Pull latest images
echo "Step 4: Pulling latest Docker images..."
docker compose pull
echo "✓ Images up to date"
echo

# Step 5: Start with verbose logging
echo "Step 5: Starting containers..."
docker compose up -d
echo

# Step 6: Wait and monitor startup
echo "Step 6: Monitoring startup (60 seconds)..."
for i in {1..60}; do
    sleep 1
    
    # Check if containers are running
    if docker compose ps | grep -q "influxdb2.*Up"; then
        echo "✓ InfluxDB container is running"
        
        # Try health check
        if curl -s http://localhost:8086/health 2>/dev/null | grep -q "pass"; then
            echo "✓✓ InfluxDB is healthy!"
            break
        fi
    fi
    
    # Show progress
    if [ $((i % 10)) -eq 0 ]; then
        echo "  ... still waiting ($i/60 seconds)"
    fi
done

echo

# Step 7: Final status check
echo "Step 7: Final Status Check"
echo "-------------------------------------------"
echo

if curl -s http://localhost:8086/health 2>/dev/null | grep -q "pass"; then
    echo "✅ SUCCESS! InfluxDB is running and healthy"
    echo
    echo "Services:"
    echo "  InfluxDB: http://localhost:8086"
    echo "  Grafana:  http://localhost:3000"
    echo
    echo "Credentials:"
    echo "  Username: $(cat secrets/influxdb2-admin-username)"
    echo "  Password: $(cat secrets/influxdb2-admin-password)"
    echo "  Token:    $(cat secrets/influxdb2-admin-token)"
    echo
    echo "Organization: yorkracing"
    echo "Bucket:       telemetry"
    echo
else
    echo "❌ PROBLEM: InfluxDB is not responding"
    echo
    echo "Let's check the logs..."
    echo "-------------------------------------------"
    docker compose logs --tail=30 influxdb2
    echo "-------------------------------------------"
    echo
    echo "Container status:"
    docker compose ps
    echo
    echo "Troubleshooting tips:"
    echo "1. Check logs above for error messages"
    echo "2. Make sure ports 8086 and 3000 are not in use"
    echo "3. Try: docker compose down -v (WARNING: deletes data)"
    echo "4. Then run this script again"
    echo
    exit 1
fi

echo "=========================================="
echo "Setup Complete!"
echo "=========================================="
<parameter name="task_progress">- [x] Verify basic serial communication works
- [x] Optimize Dyno_Final.py for Pi  
- [x] Fix baud rate mismatch
- [x] Create automated Grafana setup script
- [x] Create debug diagnostics script
- [x] Create InfluxDB fix script
- [ ] User runs fix script
- [ ] Diagnose specific error from logs
- [ ] Complete Grafana setup
