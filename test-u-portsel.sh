#!/bin/bash

START_PORT=11112
END_PORT=13000

#for portnum in $(seq $START_PORT $END_PORT)
#do
#cat /dev/urandom | tr -dC "[:alnum:]" | head -c 10 | telnet 127.0.0.1 $portnum
##sleep 0.01
#done

for i in $(seq 100)
do
echo "GET cat1.jpg OSP2P" | telnet 127.0.0.1 $START_PORT 1>/dev/null 2>&1 &
sleep 0.0001
done

#sleep 10
