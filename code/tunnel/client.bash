#!/bin/sh

SOCKET=./client.sock
if test -f "$SOCKET"; then
  rm "$SOCKET"
fi

echo "Starting waypipe in background"
waypipe  -s "$SOCKET" client &
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
./server
