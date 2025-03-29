#!/bin/bash
rm kvd-log-*
sudo kill -9 $(ps aux | grep -i "./mtd \-" | cut -d ' ' -f 3)
