@echo off

call phpize 2>&1

call configure --help

call configure --disable-all --disable-zts --enable-runkit=yes
    
nmake /nologo 2>&1

exit %errorlevel%