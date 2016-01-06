# -*- coding: utf-8 -*-
import sys
import subprocess
import os

pvm = "Release\\pvm.exe"
#pvm = "sapi/pvm/pvm"

def usage():
    print "usage:batchtest.py scan|scandir targetfile(or dir)"
    sys.exit(1)

def scanfile(filename):
    print "scanning "+filename
    cmdall = [pvm,'-g','c=d;','-p','a=b;','-k','e=f;','-t','POST','-f',filename]
    retstr = subprocess.check_output(cmdall)
    if 1:#retstr.find('tainted')>0:
        print "!!!Maybe tainted!!!"
        print retstr

def scandir(filename):
    print "scanning dir "+filename
    for lists in os.listdir(filename):
        path = os.path.join(filename, lists) 
        if os.path.isdir(path): 
            scandir(path)
        else:
            scanfile(path)

if __name__ == '__main__':
    if len(sys.argv)!=3:
        usage()
    scanmethod=sys.argv[1]
    if scanmethod=='scandir':
        scandir(sys.argv[2])
    elif scanmethod=='scan':
        scanfile(sys.argv[2])
    else:
        usage()
