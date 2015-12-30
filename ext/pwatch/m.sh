#!/bin/bash
make clean
make
cp /root/php-5.3.3/ext/pwatch/modules/pwatch.so /usr/lib/php/modules/ -f
service httpd restart
