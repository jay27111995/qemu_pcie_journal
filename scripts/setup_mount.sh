#!/bin/bash
# Run once to setup auto-mount

mkdir -p /mnt/scripts
echo "scripts /mnt/scripts 9p trans=virtio,nofail 0 0" >> /etc/fstab
mount -a
echo "Done! /mnt/scripts will auto-mount on boot"
