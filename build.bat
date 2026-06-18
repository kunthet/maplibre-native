@echo off
setlocal

REM Adjust this path if you have Community/Enterprise instead of Professional
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo Failed to load Visual Studio environment
    exit /b 1
)

cd /d "%~dp0"

echo Configuring...
cmake --preset windows-opengl
if errorlevel 1 exit /b 1

echo Building...
cmake --build build-windows-opengl --target mbgl-glfw -j 8
if errorlevel 1 exit /b 1

echo Done. Test with:
echo   build-windows-opengl\platform\glfw\mbgl-glfw.exe --style https://raw.githubusercontent.com/maplibre/demotiles/gh-pages/style.json
echo.
echo Tile cache (2nd run is much faster): %%LOCALAPPDATA%%\MapLibre\mbgl-cache.db
