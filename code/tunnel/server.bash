#!/bin/sh

SOCKET=./server.sock
if test -f "$SOCKET"; then
  rm "$SOCKET"
fi
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
./server server
sleep 5
waypipe -d -s "$SOCKET" server -- weston-term
