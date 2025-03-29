#!/usr/bin/env bash

echo 4096 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo sysctl -p /etc/sysctl.conf
cat /proc/meminfo | grep -i huge