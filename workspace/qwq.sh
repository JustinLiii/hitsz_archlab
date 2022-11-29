#! /bin/bash
../pin -t obj-intel64/brchPredict.so -- runspec --size=test --noreportable --nobuild bzip2
mv ./brchPredict.txt ./bzip2/TAGE.txt
../pin -t obj-intel64/brchPredict.so -- runspec --size=test --noreportable --nobuild sjeng
mv ./brchPredict.txt ./sjeng/TAGE.txt
../pin -t obj-intel64/brchPredict.so -- runspec --size=test --noreportable --nobuild wrf
mv ./brchPredict.txt ./wrf/TAGE.txt
../pin -t obj-intel64/brchPredict.so -- runspec --size=test --noreportable --nobuild sphinx3
mv ./brchPredict.txt ./sphinx3/TAGE.txt