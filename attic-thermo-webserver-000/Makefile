all:
	echo 'You probably want to compile and upload from the Arduino IDE'
	echo 'You can type "make vi" to edit the project files in vim though'

tags: *.ino *.h *.c *.cpp
	ctags *.ino *.h *.c *.cpp

vi:
	vim Makefile ../README.md attic-thermo-webserver-000.ino wifi_config.h ota.cpp ota.h
