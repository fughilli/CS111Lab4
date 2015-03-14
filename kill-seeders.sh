#!/bin/bash

peer_pids=$(ps aux | grep "./osppeer -s" | grep -v grep | sed "s/^[^0-9]*\([0-9]*\).*/\1/g")

for i in $peer_pids; do kill $i; done

