@echo off
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

cd /d "%~dp0"
if not exist build mkdir build

set INC=/I third_party\imgui /I third_party\imgui\backends /I third_party\minhook\include
set IMGUI=third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp
set BACKENDS=third_party\imgui\backends\imgui_impl_dx9.cpp third_party\imgui\backends\imgui_impl_win32.cpp
set MINHOOK=third_party\minhook\src\buffer.c third_party\minhook\src\hook.c third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c

cl /nologo /LD /O2 /EHsc /MT /DNDEBUG /Fobuild\ ^
   %INC% ^
   src\dllmain.cpp src\scanner.cpp src\ue3.cpp src\settings.cpp src\items.cpp src\savequeue.cpp ^
   %IMGUI% %BACKENDS% %MINHOOK% ^
   /link /DEF:src\exports.def /OUT:build\dinput8.dll ^
   d3d9.lib user32.lib gdi32.lib dwmapi.lib windowscodecs.lib ole32.lib

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> build\dinput8.dll
endlocal
