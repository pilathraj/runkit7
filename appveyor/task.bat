@echo off

call phpize 2>&1

call configure --help

call configure --enable-debug --enable-cli --enable-cgi --enable-runkit 2>&1
    
nmake /nologo 2>&1

exit %errorlevel%