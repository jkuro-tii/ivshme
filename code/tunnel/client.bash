#!/bin/sh

SOCKET=./client.sock
if test -f "$SOCKET"; then
  rm "$SOCKET"
fi

waypipe  -s ./client.sock "$SOCKET"
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
./server client
