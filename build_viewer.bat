@echo off
rem build_viewer.bat - the old name of "build.bat dev", kept so that older instructions and scripts still work: builds
rem RampageRE-dev.exe, the developer build with a console. build.bat alone builds RampageRE.exe, the player build.
call "%~dp0build.bat" dev
