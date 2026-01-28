#!/bin/bash
# quick_fix.sh - Quick diagnostic and fix for crashed InfluxDB

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "=========================================="
echo "Quick InfluxDB Fix"
echo "=========================================="
echo

echo "Current container status:"
docker compose ps
echo

echo "Checking InfluxDB logs for errors..."
echo "-------------------------------------------"
docker compose logs influxdb2 | tail -50
echo "-------------------------------------------"
echo

read -p "Press Enter to try fixes..."

echo
echo "Applying fixes..."
echo

# Fix 1: Stop everything
echo "1. Stopping containers..."
docker compose down
sleep 2

# Fix 2: Remove volumes (fresh start)
echo "2. Removing old volumes (fresh start)..."
docker volume rm stack_influxdb2-data stack_influxdb2-config 2>/dev/null || true

# Fix 3: Ensure secrets exist and are clean
echo "3. Checking secrets..."
mkdir -p secrets
echo -n "admin" > secrets/influxdb2-admin-username
echo -n "dynopass2024" > secrets/influxdb2-admin-password
echo -n "dynotoken123456789012345678901234567890abc" > secrets/influxdb2-admin-token
chmod 600 secrets/influxdb2-admin-*

# Fix 4: Start fresh
echo "4. Starting containers..."
docker compose up -d

echo
echo "Waiting 30 seconds for startup..."
sleep 30

echo
echo "Final check:"
docker compose ps
echo

if curl -s http://localhost:8086/health | grep -q "pass"; then
    echo "✅ SUCCESS! InfluxDB is now running"
    echo
    echo "Credentials:"
    echo "  Username: admin"
    echo "  Password: dynopass2024"
    echo "  Token: dynotoken123456789012345678901234567890abc"
else
    echo "❌ Still not working. Check logs:"
    docker compose logs influxdb2
fi
