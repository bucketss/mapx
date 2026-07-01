requires libarchive

build w icon (windows):

windres resource.rc resource.o
gcc -g mapXtract.c resource.o -o mapXtract.exe -lzip -larchive

no icon (x-platform):

gcc -g mapXtract.c -o mapXtract -lzip -larchive