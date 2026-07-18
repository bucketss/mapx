requires libarchive

build w icon (windows):

windres resource.rc resource.o
gcc -g mapXtract.c resource.o -o mapXtract.exe -larchive

no icon (x-platform):

gcc -g mapXtract.c -o mapXtract -larchive

no libarchive dev package installed? include\ has the libarchive 3.7.4
headers; point -I at it and link the dll directly, e.g.:

gcc -g mapXtract.c resource.o -o mapXtract.exe -Iinclude C:\Strawberry\c\bin\libarchive-13.dll
