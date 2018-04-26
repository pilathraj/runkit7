@echo off

call phpize 2>&1

call configure --help

call configure --disable-all --enable-runkit --enable-runkit-modify --enable-debug  2>&1
    
nmake /nologo 2>&1

exit %errorlevel%