#!/bin/bash

PORT=6666
DEVICE='QV71339P3D'
LOCAL_IP='100.64.0.10'
DEVICE_IP='192.168.10.20'

device_is_ready() {
    adb devices | grep -w "$DEVICE" | grep -w "device" > /dev/null
}

check_enable_tcpip() {
    adb shell "getprop service.adb.tcp.port" | grep -w "$PORT" > /dev/null
    if [ $? -ne 0 ]; then
        echo "TCPIP mode not enabled. Enabling..."
        adb tcpip $PORT
    else
        echo "TCPIP mode already enabled."
    fi
}

check_enable_forward() {
    adb forward --list | grep -w "tcp:$PORT" > /dev/null
    if [ $? -ne 0 ]; then
        echo "Port forwarding not set. Setting up..."
        adb forward tcp:$PORT localabstract:5555
        adb forward tcp:27183 localabstract:27183
        echo "Forwarded local IP $PORT to device:5555"
        echo "Forwarded local IP 27183 to device:27183"
    else
        echo "Port forwarding already set."
    fi
}

check_and_forward_local() {
    for port in 6666 27183; do
        if ! netstat -nat | grep -q "$LOCAL_IP:$port"; then
            socat TCP4-LISTEN:$port,bind=$LOCAL_IP,fork TCP4:127.0.0.1:$port &
            echo "Started socat to forward port $port."
        else
            echo "Port $port is already listening."
        fi
    done
}

check_and_forward_device() {
    for port in 6666 27183; do
        if ! netstat -nat | grep -q "$LOCAL_IP:$port"; then
            socat TCP4-LISTEN:$port,bind=$LOCAL_IP,fork TCP4:$DEVICE_IP:$port &
            echo "Started socat to forward port $port."
        else
            echo "Port $port is already listening."
        fi
    done
}

while true; do
    if ! device_is_ready; then
        echo "Device not ready or not connected. Waiting..."
        sleep 60
        continue
    fi
    
    check_enable_tcpip
    check_and_forward_device
    #check_enable_forward
    #check_and_forward_local

    echo "Waiting 10 minutes before next check..."
    sleep 600
done

