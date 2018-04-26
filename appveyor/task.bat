@echo off

call phpize 2>&1

call configure --disable-all --enable-session --enable-debug --enable-cli --enable-cgi --enable-runkit --enable-runkit-spl_object_id --enable-json  --enable-debug-pack 2>&1
    
nmake /nologo 2>&1

exit %errorlevel%