#!/bin/bash
make clean
#bash ./cleanpvm.sh
make
cd ext/pwatch
bash ./m.sh
#service httpd restart
cd ../..
rm -rf /tmp/pvm*
cp sapi/pvm/pvm /tmp
