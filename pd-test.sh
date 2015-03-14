#!/bin/bash

echo "Start hosting..."

cd hostdir
for i in $(seq 10)
do
	echo "Host #$i"
	./osppeer -s &
done

cd ..

#echo "Download the file"

#./osppeer -p testfile.txt
#mv rc-testfile.txt dl-file.txt
#./clean_dir.sh

#mv dl-file.txt testfile.txt
