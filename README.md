Linux char driver for TI ADC122S101

Must be parallel to the buildroot for beaglebone from git@github.com:watchfrog/Beaglebone-Buildroot.git

To run, insmod adc122s101.ko

Read data from /dev/adc. Data is 16 bit network order records, ch0 alternating with ch1.

i.e. to stream to a udp socket:

cat /dev/adc | nc -u 10.0.1.10 6666

on the other end, run "nc -u -l 6666 > data.bin" or somesuch.

Don Smyth, Jan 3, 2013
