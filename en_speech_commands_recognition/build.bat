@echo off
cd /d d:\Espressif\esp-skainet\examples\en_speech_commands_recognition
call D:\Espressif\frameworks\esp-idf-v4.4.8\export.bat
idf.py build
echo Build completed with exit code %errorlevel%