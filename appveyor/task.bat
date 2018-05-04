@echo off

call phpize 2>&1

call configure --help

call configure --disable-all --enable-debug --enable-runkit-spl_object_id
    
nmake /nologo 2>&1

exit %errorlevel%