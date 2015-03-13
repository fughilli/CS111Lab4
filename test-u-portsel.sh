#!/bin/bash

START_PORT=11113
END_PORT=13000

for portnum in $(seq $START_PORT $END_PORT)
do
cat /dev/urandom | tr -dC "[:alnum:]" | head -c 10 | telnet 127.0.0.1 $portnum
#sleep 0.01
done
