requires 7-Zip at runtime (7z on PATH, or C:\Program Files\7-Zip\7z.exe)

build w icon (windows):

windres resource.rc resource.o
gcc -g mapXtract.c resource.o -o mapXtract.exe

no icon (x-platform):

gcc -g mapXtract.c -o mapXtract
